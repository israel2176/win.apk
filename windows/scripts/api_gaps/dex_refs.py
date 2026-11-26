#!/usr/bin/env python3
"""dex_refs.py -- what an app's code asks of the framework (docs/research/api-gaps.md).

Reads each APK's classes*.dex (no Android tools: the dex format is parsed here) and writes every
framework class, method and field its code uses, where it uses it: only references from an
instruction count (an invoke, a field read or write, new-instance, const-class, check-cast,
instance-of), not entries of the constant pool nothing executes. A reference is "framework" when the
app does not define its class. Reflection is caught by a heuristic: a method that calls
Class.forName / getMethod / getDeclaredMethod / getField / getDeclaredField and loads a string that
names a framework class ("android.app.ActivityThread") and then an identifier ("currentActivityThread")
is taken to reach that member. The manifest's minSdkVersion and targetSdkVersion are read too, since
Win.apk presents max(clamp(targetSdk, 29, 36), minSdk) as Build.VERSION.SDK_INT (PresentedSdk.java):
an app targeting 35 takes AndroidX's `SDK_INT >= 35` branches.

    python3 -I dex_refs.py --out refs.tsv <app.apk>...

refs.tsv, tab-separated:
    APP   <package>  <minSdk>  <targetSdk>
    C     <class>                    <package>  <referencing packages, comma-separated>
    M     <class>  <name>  <desc>    <package>  <referencing packages>
    F     <class>  <name>  <desc>    <package>  <referencing packages>
    R     <class>  <member or ->     <package>  <referencing packages>      (reflection, heuristic)
Class names are internal (android/app/Activity). Referencing packages are the first three segments of
the calling class's package (androidx.core.view), so a gap can be told apart as the app's own call or
a library's.
"""
import argparse
import re
import struct
import sys
import zipfile

FRAMEWORK = re.compile(r"^(android|com/android|dalvik|java|javax|org/apache/http|org/json|org/w3c|org/xml|"
                       r"libcore|sun|org/xmlpull)/")
CLASS_STRING = re.compile(r"^(android|com\.android|dalvik|libcore)(\.[a-z0-9_]+)*\.[A-Z][A-Za-z0-9_$]*[a-z][A-Za-z0-9_$]*$")
IDENT = re.compile(r"^[A-Za-z_$][A-Za-z0-9_$]*$")
REFLECT = {("java/lang/Class", n) for n in ("forName", "getMethod", "getDeclaredMethod", "getField",
                                             "getDeclaredField", "getConstructor", "getDeclaredConstructor")}

# Code units of each opcode's instruction (the dex format's instruction formats).
WIDTH = [1] * 256
for op, w in [(0x02, 2), (0x03, 3), (0x05, 2), (0x06, 3), (0x08, 2), (0x09, 3), (0x13, 2), (0x14, 3), (0x15, 2),
              (0x16, 2), (0x17, 3), (0x18, 5), (0x19, 2), (0x1a, 2), (0x1b, 3), (0x1c, 2), (0x1f, 2), (0x20, 2),
              (0x22, 2), (0x23, 2), (0x24, 3), (0x25, 3), (0x26, 3), (0x29, 2), (0x2a, 3), (0x2b, 3), (0x2c, 3),
              (0xfa, 4), (0xfb, 4), (0xfc, 3), (0xfd, 3), (0xfe, 2), (0xff, 2)]:
    WIDTH[op] = w
for lo, hi, w in [(0x2d, 0x31, 2), (0x32, 0x3d, 2), (0x44, 0x6d, 2), (0x6e, 0x72, 3), (0x74, 0x78, 3),
                  (0x90, 0xaf, 2), (0xd0, 0xe2, 2)]:
    for op in range(lo, hi + 1):
        WIDTH[op] = w
INVOKE = set(range(0x6e, 0x73)) | set(range(0x74, 0x79))
FIELD = set(range(0x52, 0x6e))
TYPE = {0x1c, 0x1f, 0x20, 0x22}


def uleb(b, i):
    v = s = 0
    while True:
        x = b[i]
        i += 1
        v |= (x & 0x7f) << s
        s += 7
        if x < 0x80:
            return v, i


class Dex:
    def __init__(self, b):
        self.b = b
        u = lambda o: struct.unpack_from("<I", b, o)[0]
        (self.n_str, self.o_str, self.n_type, self.o_type, self.n_proto, self.o_proto, self.n_field, self.o_field,
         self.n_meth, self.o_meth, self.n_cls, self.o_cls) = [u(0x38 + 4 * k) for k in range(12)]
        self._s = {}

    def string(self, i):
        if i not in self._s:
            off = struct.unpack_from("<I", self.b, self.o_str + 4 * i)[0]
            _, p = uleb(self.b, off)
            e = self.b.index(b"\0", p)
            self._s[i] = self.b[p:e].decode("utf-8", "replace")  # MUTF-8: close enough for names
        return self._s[i]

    def type(self, i):
        return self.string(struct.unpack_from("<I", self.b, self.o_type + 4 * i)[0])

    def proto(self, i):
        _, ret, params = struct.unpack_from("<III", self.b, self.o_proto + 12 * i)
        args = ""
        if params:
            n = struct.unpack_from("<I", self.b, params)[0]
            args = "".join(self.type(struct.unpack_from("<H", self.b, params + 4 + 2 * k)[0]) for k in range(n))
        return "(" + args + ")" + self.type(ret)

    def method(self, i):
        c, p, n = struct.unpack_from("<HHI", self.b, self.o_meth + 8 * i)
        return self.type(c), self.string(n), self.proto(p)

    def field(self, i):
        c, t, n = struct.unpack_from("<HHI", self.b, self.o_field + 8 * i)
        return self.type(c), self.string(n), self.type(t)

    def classes(self):
        """(class descriptor, [code_item offsets]) for each class defined here."""
        for k in range(self.n_cls):
            ci, _, _, _, _, _, data, _ = struct.unpack_from("<8I", self.b, self.o_cls + 32 * k)
            codes = []
            if data:
                p = data
                sf, p = uleb(self.b, p)
                inf, p = uleb(self.b, p)
                dm, p = uleb(self.b, p)
                vm, p = uleb(self.b, p)
                for _ in range(sf + inf):
                    _, p = uleb(self.b, p)
                    _, p = uleb(self.b, p)
                for _ in range(dm + vm):
                    _, p = uleb(self.b, p)
                    _, p = uleb(self.b, p)
                    code, p = uleb(self.b, p)
                    if code:
                        codes.append(code)
            yield self.type(ci), codes

    def insns(self, code):
        size = struct.unpack_from("<I", self.b, code + 12)[0]
        base = code + 16
        i = 0
        while i < size:
            unit = struct.unpack_from("<H", self.b, base + 2 * i)[0]
            op = unit & 0xff
            if op == 0 and unit != 0:  # a switch or array payload
                ident = unit
                if ident == 0x0100:
                    n = struct.unpack_from("<H", self.b, base + 2 * i + 2)[0]
                    i += 4 + 2 * n
                elif ident == 0x0200:
                    n = struct.unpack_from("<H", self.b, base + 2 * i + 2)[0]
                    i += 2 + 4 * n
                elif ident == 0x0300:
                    w = struct.unpack_from("<H", self.b, base + 2 * i + 2)[0]
                    n = struct.unpack_from("<I", self.b, base + 2 * i + 4)[0]
                    i += 4 + (n * w + 1) // 2
                else:
                    i += 1
                continue
            if op == 0x1b:
                yield op, struct.unpack_from("<I", self.b, base + 2 * i + 2)[0]
            elif WIDTH[op] >= 2 and (op in INVOKE or op in FIELD or op in TYPE or op == 0x1a):
                yield op, struct.unpack_from("<H", self.b, base + 2 * i + 2)[0]
            i += WIDTH[op]


def internal(desc):
    return desc[1:-1] if desc.startswith("L") and desc.endswith(";") else None


def pkg3(cls):
    return ".".join(cls[1:-1].split("/")[:-1][:3])


def manifest_sdk(apk):
    """minSdkVersion and targetSdkVersion from the binary manifest's uses-sdk (resource ids 0x0101020c,
    0x01010270), without a parser for the whole format: the attributes' resource ids and typed values."""
    b = apk.read("AndroidManifest.xml")
    ids = []
    p = 8
    while p < len(b):
        t, hs, size = struct.unpack_from("<HHI", b, p)
        if t == 0x0180:  # resource map
            ids = list(struct.unpack_from("<%dI" % ((size - hs) // 4), b, p + hs))
        elif t == 0x0102:  # start element
            n = struct.unpack_from("<H", b, p + 28)[0]
            a = p + 36
            got = {}
            for k in range(n):
                ns, name, raw, vs, vz, vt, data = struct.unpack_from("<IIIHBBI", b, a + 20 * k)
                if name < len(ids) and ids[name] in (0x0101020c, 0x01010270) and vt in (0x10, 0x11):
                    got[ids[name]] = data
            if got:
                return got.get(0x0101020c, 1), got.get(0x01010270, got.get(0x0101020c, 1))
        p += size
    return 1, 1


def scan(path, out):
    z = zipfile.ZipFile(path)
    names = sorted(n for n in z.namelist() if re.fullmatch(r"classes\d*\.dex", n))
    dexes = [Dex(z.read(n)) for n in names]
    pkg = path.rsplit("/", 1)[-1].rsplit("_", 1)[0]
    mn, tg = manifest_sdk(z)
    out.write(f"APP\t{pkg}\t{mn}\t{tg}\n")
    defined = set()
    for d in dexes:
        for c, _ in d.classes():
            defined.add(c)
    refs = {}

    def add(key, caller):
        refs.setdefault(key, set()).add(pkg3(caller))

    for d in dexes:
        for cls, codes in d.classes():
            for code in codes:
                strings = []
                reflective = False
                for op, idx in d.insns(code):
                    if op in (0x1a, 0x1b):
                        strings.append(d.string(idx))
                    elif op in INVOKE:
                        owner, name, desc = d.method(idx)
                        if owner not in defined and internal(owner) and FRAMEWORK.match(internal(owner)):
                            add(("M", internal(owner), name, desc), cls)
                        if (internal(owner), name) in REFLECT:
                            reflective = True
                    elif op in FIELD:
                        owner, name, t = d.field(idx)
                        if owner not in defined and internal(owner) and FRAMEWORK.match(internal(owner)):
                            add(("F", internal(owner), name, t), cls)
                    elif op in TYPE:
                        t = d.type(idx).lstrip("[")
                        if t not in defined and internal(t) and FRAMEWORK.match(internal(t)):
                            add(("C", internal(t)), cls)
                if reflective:
                    for k, s in enumerate(strings):
                        if CLASS_STRING.match(s) and ("L" + s.replace(".", "/") + ";") not in defined:
                            member = strings[k + 1] if k + 1 < len(strings) and IDENT.match(strings[k + 1]) else "-"
                            add(("R", s.replace(".", "/"), member), cls)
    for key in sorted(refs):
        callers = ",".join(sorted(refs[key])[:6])
        out.write("\t".join(key) + f"\t{pkg}\t{callers}\n")
    return len(refs)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("apks", nargs="+")
    a = ap.parse_args()
    with open(a.out, "w") as out:
        for apk in a.apks:
            n = scan(apk, out)
            print(f"{apk}: {n} framework references", file=sys.stderr)


if __name__ == "__main__":
    main()
