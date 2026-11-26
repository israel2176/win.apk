#!/usr/bin/env python3
"""ndk_gaps.py -- which public NDK functions Win.apk lacks or only stubs, and which apps import them.

A survey tool, not a test: it reads AOSP's NDK version scripts, Win.apk's own symbol tables and the
dynamic imports of a set of apps' .so files, and prints a Markdown report.

Usage:
  python3 -I ndk_gaps.py --so-root DIR [--cache DIR] [--branch android16-release] [--repo ROOT] [--offline]

  --so-root  a directory holding one sub-directory per app (named after its package), each with that
             app's .so files (x86_64, or arm64-v8a when the app has no x86_64 build).
  --cache    where the fetched *.map.txt files are kept (default: $NDK_GAPS_CACHE, else the system temp
             directory). Keep it out of the repository.
  --offline  use only the cached maps.

Python 3 standard library only; `readelf` (binutils or llvm-readelf) is run for the ELF reading.

WHERE THE LISTS COME FROM AND THE HEURISTICS (each one says why):

1. The public NDK. Each system library's version script (<lib>.map.txt) is fetched from
   android.googlesource.com (?format=TEXT, base64) at --branch, falling back to android15-release and
   android14-release when a file is missing there. A symbol is public NDK unless its line or its block
   carries `llndk`, `apex`, `systemapi` or `platform-only`, or its block is a *_PRIVATE / *_PLATFORM one
   (those are what the NDK's stub generator leaves out). Lines inside `extern "C++"` are C++ patterns,
   not names, and are skipped. Arch tags (arm, arm64, x86, x86_64, riscv64, lp32, lp64) keep a symbol
   only if it exists on x86_64 or arm64, the two ABIs Win.apk runs. The level is the symbol's
   introduced-x86_64= / introduced-arm64= (whichever is lower), else its introduced=, else its block's,
   else 21 (the first 64-bit level, which is where every untagged symbol starts on these ABIs).
   Codenames (Q, Tiramisu, UpsideDownCake, VanillaIceCream, Baklava) are turned into numbers.

2. What Win.apk implements. A guest import is resolved by NAME ONLY, through one global chain
   (windows/host/guest_libs.cpp, resolve_system): the Bionic shim's merged table (mini_bionic.cpp's
   kSymbols plus every src/bionic/*.cpp ShimTable), the GL/EGL table (host/gl/guest_gl.cpp, symbols(),
   with the generated guest_gl.inc), native_window, native_asset, native_activity, native_aaudio,
   native_media, native_sensor, native_opensles, jnigraphics; the linker answers libdl's names itself
   (src/linker.cpp kLibdl). So the implemented set is every name in those tables, found textually:
     * WINAPK_SHIM("name", fn) and {"name", <non-string>} entries (the second excludes string-to-string
       tables such as the environment's defaults);
     * NAME(ident) for the per-file table macros (WINAPK_NA, WINAPK_ASSET, WINAPK_NW, WINAPK_MEDIA,
       WINAPK_SENSOR, AAUDIO_FN, WINAPK_GUEST_GL, WINAPK_LIBDL), and WINAPK_NA_CONF(X) as
       AConfiguration_getX / _setX;
     * strcmp(name, "X") == 0 in the resolvers that compare names directly (jnigraphics, OpenSL ES);
     * arm64 only: guest_intrinsic's and abi_override's names (src/arm64/), which also bind.
   Only names that are public NDK symbols matter, so a stray string that matches nothing is harmless.

3. Which implemented entries are stubs. For each table entry the function it binds is looked up (its
   definition in windows/src or windows/host) and its body read. A body that is empty, or is a single
   `return <constant>;` (a literal, nullptr, true/false, a k-constant, {}), or only sets errno and
   returns a constant, is a STUB: it answers without doing anything. Its constant is reported. This is
   a heuristic: a constant is sometimes exactly what a phone answers (getuid's fixed uid, a sensor list
   that is empty), so the report says what the stub returns and the reader judges. GL entries
   (guest_gl.inc) forward to ANGLE and are counted as implemented; a function ANGLE lacks returns {}
   there, which this script does not see.

4. A miss. A name none of those answers is bound by the ELF loader (src/elf_loader.cpp, bind) to:
   0 for a WEAK undefined symbol (as Android's linker does for a weak miss), a shared zeroed 4 KiB
   block for an OBJECT (guest_libs.cpp, missing_object), and for anything else a per-name stub that
   logs its first call and returns 0 (guest_libs.cpp, missing_stub; counted by host/stub_hits.h).
   dlsym() of a missing name returns NULL with an error (linker.cpp, lookup), as Android does.

5. App imports. `readelf -W --dyn-syms -d` on each .so: undefined, non-local dynamic symbols and the
   DT_NEEDED list. A symbol some .so of the same app defines (libc++_shared.so, an app's own helper
   library) is the app's, not the system's, and is dropped. The rest is attributed to an NDK library:
   by its symbol version (LIBC* is libc, libm or libdl, chosen by which map lists the name; LIBANDROID,
   LIBLOG, LIBEGL... name the library), else by the system DT_NEEDED libraries whose map lists it, else
   by any NDK map that lists it. Names no map lists are reported apart (non-NDK imports).
"""

import argparse
import base64
import collections
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

# ---- 1. the NDK's version scripts -------------------------------------------------------------------

# library -> (AOSP project, path in it). android16-release is asked first.
MAPS = {
    "libc": ("bionic", "libc/libc.map.txt"),
    "libm": ("bionic", "libm/libm.map.txt"),
    "libdl": ("bionic", "libdl/libdl.map.txt"),
    "libstdc++": ("bionic", "libc/libstdc++.map.txt"),
    "liblog": ("system/logging", "liblog/liblog.map.txt"),
    "libz": ("external/zlib", "libz.map.txt"),
    "libandroid": ("frameworks/base", "native/android/libandroid.map.txt"),
    "libjnigraphics": ("frameworks/base", "native/graphics/jni/libjnigraphics.map.txt"),
    "libamidi": ("frameworks/base", "media/native/midi/libamidi.map.txt"),
    "libnativewindow": ("frameworks/native", "libs/nativewindow/libnativewindow.map.txt"),
    "libEGL": ("frameworks/native", "opengl/libs/libEGL.map.txt"),
    "libGLESv1_CM": ("frameworks/native", "opengl/libs/libGLESv1_CM.map.txt"),
    "libGLESv2": ("frameworks/native", "opengl/libs/libGLESv2.map.txt"),
    "libGLESv3": ("frameworks/native", "opengl/libs/libGLESv3.map.txt"),
    "libvulkan": ("frameworks/native", "vulkan/libvulkan/libvulkan.map.txt"),
    "libbinder_ndk": ("frameworks/native", "libs/binder/ndk/libbinder_ndk.map.txt"),
    "libsync": ("system/core", "libsync/libsync.map.txt"),
    "libmediandk": ("frameworks/av", "media/ndk/libmediandk.map.txt"),
    "libcamera2ndk": ("frameworks/av", "camera/ndk/libcamera2ndk.map.txt"),
    "libaaudio": ("frameworks/av", "media/libaaudio/src/libaaudio.map.txt"),
    "libOpenSLES": ("frameworks/wilhelm", "src/libOpenSLES.map.txt"),
    "libOpenMAXAL": ("frameworks/wilhelm", "src/libOpenMAXAL.map.txt"),
    "libneuralnetworks": ("packages/modules/NeuralNetworks", "runtime/libneuralnetworks.map.txt"),
    "libicu": ("external/icu", "libicu/libicu.map.txt"),
}
BRANCHES_FALLBACK = ["android15-release", "android14-release"]

CODENAMES = {"Q": 29, "R": 30, "S": 31, "Sv2": 32, "Tiramisu": 33, "UpsideDownCake": 34,
             "VanillaIceCream": 35, "Baklava": 36}
EXCLUDE_TAGS = {"llndk", "apex", "systemapi", "platform-only"}
ARCH_TAGS = {"arm", "arm64", "x86", "x86_64", "riscv64", "lp32", "lp64", "mips", "mips64"}
OUR_ARCHES = {"x86_64", "arm64", "lp64"}
FIRST_64BIT_LEVEL = 21


def fetch_map(lib, cache, branch, offline):
    """The map's text, from the cache or googlesource (base64 with ?format=TEXT)."""
    project, path = MAPS[lib]
    cached = os.path.join(cache, lib + ".map.txt")
    if os.path.exists(cached):
        with open(cached, encoding="utf-8") as f:
            return f.read(), open(cached + ".source", encoding="utf-8").read().strip() if os.path.exists(
                cached + ".source") else "cache"
    if offline:
        return None, "not cached"
    for b in [branch] + [x for x in BRANCHES_FALLBACK if x != branch]:
        url = "https://android.googlesource.com/platform/%s/+/refs/heads/%s/%s?format=TEXT" % (project, b, path)
        text = None
        for attempt in range(4):
            # A plain client name: googlesource answers Python's default agent with 503 at times.
            req = urllib.request.Request(url, headers={"User-Agent": "ndk-gaps/1.0"})
            try:
                with urllib.request.urlopen(req, timeout=60) as r:
                    text = base64.b64decode(r.read()).decode("utf-8")
                break
            except urllib.error.HTTPError as e:
                if e.code == 404:
                    break
                if e.code in (429, 500, 502, 503) and attempt < 3:
                    time.sleep(2 * (attempt + 1))
                    continue
                raise
        if text is None:
            continue
        os.makedirs(cache, exist_ok=True)
        with open(cached, "w", encoding="utf-8") as f:
            f.write(text)
        with open(cached + ".source", "w", encoding="utf-8") as f:
            f.write(url)
        return text, url
    return None, "not found on any branch"


def level_of(value):
    if value.isdigit():
        return int(value)
    return CODENAMES.get(value)


def parse_tags(comment):
    """'# introduced=29 var' -> set of plain tags and a dict of key=value tags."""
    plain, kv = set(), {}
    for t in comment.split():
        if "=" in t:
            k, v = t.split("=", 1)
            kv[k] = v
        else:
            plain.add(t)
    return plain, kv


def symbol_level(kv, block_level):
    arch = [level_of(kv[k]) for k in ("introduced-x86_64", "introduced-arm64") if k in kv]
    arch = [a for a in arch if a]
    if arch:
        return max(min(arch), FIRST_64BIT_LEVEL)
    if "introduced" in kv and level_of(kv["introduced"]):
        return max(level_of(kv["introduced"]), FIRST_64BIT_LEVEL)
    return max(block_level or FIRST_64BIT_LEVEL, FIRST_64BIT_LEVEL)


def parse_map(text):
    """{symbol: {"level": n, "var": bool}} of the public NDK symbols on x86_64/arm64."""
    out = {}
    block_ok, block_level, in_cpp = True, None, False
    for raw in text.splitlines():
        line, _, comment = raw.partition("#")
        line = line.strip()
        plain, kv = parse_tags(comment)
        m = re.match(r"^([A-Za-z0-9_]+)\s*\{$", line)
        if m:
            name = m.group(1)
            block_ok = not (plain & EXCLUDE_TAGS) and not name.endswith(("_PRIVATE", "_PLATFORM"))
            # A block tagged with arches it alone has (e.g. "arm platform-only") is not ours.
            if plain & ARCH_TAGS and not plain & OUR_ARCHES:
                block_ok = False
            block_level = None
            for k in ("introduced-x86_64", "introduced-arm64", "introduced"):
                if k in kv and level_of(kv[k]):
                    block_level = level_of(kv[k])
                    break
            continue
        if line.startswith('extern "C++"'):
            in_cpp = True
            continue
        if line.startswith("}"):
            if in_cpp:
                in_cpp = False
            continue
        if in_cpp or not block_ok or line in ("global:", "local:") or not line.endswith(";"):
            continue
        sym = line[:-1].strip()
        if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", sym):
            continue
        if plain & EXCLUDE_TAGS:
            continue
        if plain & ARCH_TAGS and not plain & OUR_ARCHES:
            continue
        out[sym] = {"level": symbol_level(kv, block_level), "var": "var" in plain}
    return out


# ---- 2 and 3. Win.apk's tables ---------------------------------------------------------------------

# The files whose tables resolve_system (host/guest_libs.cpp) and the linker consult.
TABLE_FILES = [
    "src/mini_bionic.cpp", "src/linker.cpp", "src/jnigraphics.cpp",
    "host/native_window.cpp", "host/native_asset.cpp", "host/native_activity.cpp",
    "host/native_aaudio.cpp", "host/native_media.cpp", "host/native_sensor.cpp",
    "host/native_opensles.cpp", "host/gl/guest_gl.cpp", "host/gl/guest_gl.inc",
]
ARM64_FILES = ["src/arm64/arm64_abi.cpp", "src/arm64/arm64_guest.cpp"]
NAME_MACROS = ["WINAPK_NA", "WINAPK_ASSET", "WINAPK_NW", "WINAPK_MEDIA", "WINAPK_SENSOR", "AAUDIO_FN",
               "WINAPK_GUEST_GL", "WINAPK_LIBDL"]


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    return re.sub(r"//[^\n]*", " ", s)


def table_entries(root):
    """[(name, file, line, fn-or-None)] for every name Win.apk's resolvers answer."""
    files = TABLE_FILES + sorted("src/bionic/" + f for f in os.listdir(os.path.join(root, "src/bionic"))
                                 if f.endswith(".cpp"))
    entries = []
    for rel in files:
        path = os.path.join(root, rel)
        text = open(path, encoding="utf-8", errors="replace").read()
        code = text  # line numbers must match the file, so comments are not stripped here
        # WINAPK_SHIM, WINAPK_LIBDL, WINAPK_MEDIA_AS, WINAPK_SENSOR_AS...: ("name", fn).
        for m in re.finditer(r'\bWINAPK_[A-Z0-9_]+\(\s*"([^"]+)"\s*,\s*([A-Za-z_][A-Za-z0-9_:]*)', code):
            entries.append((m.group(1), rel, line_of(text, m.start()), m.group(2)))
        # misc.cpp's libm table: M(sin) is sin and sinf, bound to m_sin and m_sinf.
        for m in re.finditer(r"(?<![A-Za-z0-9_])M\(\s*([a-z][a-z0-9_]*)\s*\)", code):
            for n in (m.group(1), m.group(1) + "f"):
                entries.append((n, rel, line_of(text, m.start()), "m_" + n))
        for m in re.finditer(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*(?!")([^}\n]*)', code):
            rest = m.group(2)
            fn = re.search(r"&\s*([A-Za-z_][A-Za-z0-9_:]*)", rest)
            entries.append((m.group(1), rel, line_of(text, m.start()), fn.group(1) if fn else None))
        for mac in NAME_MACROS:
            for m in re.finditer(r"\b%s\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)" % mac, code):
                fn = ("guest_" + m.group(1)) if mac == "WINAPK_GUEST_GL" else m.group(1)
                entries.append((m.group(1), rel, line_of(text, m.start()), fn))
        for m in re.finditer(r"\bWINAPK_NA_CONF\(\s*([A-Za-z]+)\s*\)", code):
            for verb in ("get", "set"):
                n = "AConfiguration_%s%s" % (verb, m.group(1))
                entries.append((n, rel, line_of(text, m.start()), n))
        for m in re.finditer(r'strcmp\(\s*name\s*,\s*"([^"]+)"\s*\)\s*==\s*0\s*\)\s*(?:\{\s*)?return\s+'
                             r'(?:reinterpret_cast<void\*>\()?&?([A-Za-z_][A-Za-z0-9_]*)', code):
            entries.append((m.group(1), rel, line_of(text, m.start()), m.group(2)))
    return entries


def arm64_entries(root):
    out = []
    for rel in ARM64_FILES:
        text = open(os.path.join(root, rel), encoding="utf-8", errors="replace").read()
        for m in re.finditer(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*(?:reinterpret_cast<void\*>\(&|k)', text):
            out.append((m.group(1), rel, line_of(text, m.start()), None))
    return out


class Sources:
    """Every .cpp/.h/.inc under windows/src and windows/host, for finding a function's definition."""

    def __init__(self, root):
        self.files = []
        for top in ("src", "host"):
            for d, _, names in os.walk(os.path.join(root, top)):
                for n in names:
                    if n.endswith((".cpp", ".h", ".inc")):
                        p = os.path.join(d, n)
                        self.files.append((os.path.relpath(p, root), open(p, encoding="utf-8",
                                                                            errors="replace").read()))
        self.cache = {}

    def definition(self, fn, prefer):
        """(file, line, body) of fn's definition, the table's own file first; None if not found."""
        key = (fn, prefer)
        if key in self.cache:
            return self.cache[key]
        short = fn.split("::")[-1]
        pat = re.compile(r"(?<![A-Za-z0-9_.>])%s\s*\(([^;{}]*)\)\s*(?:const\s*)?(?:noexcept\s*)?\{" % re.escape(short))
        ordered = sorted(self.files, key=lambda f: f[0] != prefer)
        found = None
        for rel, text in ordered:
            for m in pat.finditer(text):
                # Not a call inside an expression: the text before the name on its line is a declaration's.
                start_line = text.rfind("\n", 0, m.start()) + 1
                before = text[start_line:m.start()]
                if re.search(r"(return|=|\(|,)\s*$", before):
                    continue
                depth, i = 0, m.end() - 1
                while i < len(text):
                    if text[i] == "{":
                        depth += 1
                    elif text[i] == "}":
                        depth -= 1
                        if depth == 0:
                            break
                    i += 1
                found = (rel, line_of(text, m.start()), text[m.end():i], m.group(1))
                break
            if found:
                break
        self.cache[key] = found
        return found


# A constant a stub may return: a number, nullptr, a bool, {}, a kName or E/EGL_/GL_/SL_RESULT_ constant.
CONST_RE = (r"(?:-?\s*\(?\s*(?:0x[0-9a-fA-F]+|[0-9]+(?:\.[0-9]*)?f?)\s*\)?|nullptr|NULL|true|false|\{\s*\}"
            r"|-?\s*k[A-Z][A-Za-z0-9_]*|-?E[A-Z]+|EGL_[A-Z_]+|GL_[A-Z_]+|SL_RESULT_[A-Z_]+"
            r"|static_cast<[^>]+>\(\s*-?[0-9]+\s*\))")


def classify(body):
    """None for a real body; else a short description of what the stub does."""
    b = strip_comments(body)
    b = re.sub(r"\(void\)\s*[A-Za-z_][A-Za-z0-9_]*\s*;", " ", b)  # (void)unused;
    b = re.sub(r"\s+", " ", b).strip()
    if b == "":
        return "does nothing"
    m = re.fullmatch(r"return (%s) ?;" % CONST_RE, b)
    if m:
        return "returns %s" % re.sub(r"\s+", "", m.group(1))
    m = re.fullmatch(r"(?:[A-Za-z_:]*errno[A-Za-z_]*\s*(?:=\s*[A-Za-z0-9_:]+|\(\s*[A-Za-z0-9_:]+\s*\))\s*;)"
                     r" ?return (%s) ?;" % CONST_RE, b)
    if m:
        err = re.search(r"\b(k?E[A-Z]+)\b", b)
        return "sets errno%s, returns %s" % ((" " + err.group(1)) if err else "", re.sub(r"\s+", "", m.group(1)))
    return None


def win_apk_symbols(root, ndk):
    """{name: {"where": "file:line", "stub": None|text, "def": "file:line"|None}} limited to NDK names."""
    src = Sources(root)
    out = {}
    for name, rel, ln, fn in table_entries(root) + arm64_entries(root):
        if name not in ndk or name in out:
            continue
        info = {"where": "windows/%s:%d" % (rel, ln), "stub": None, "def": None}
        # GL/EGL: guest_gl.inc's wrappers forward to ANGLE; the hand-written EGL ones are read.
        if fn and not rel.endswith(".inc"):
            d = src.definition(fn, rel)
            if d:
                info["def"] = "windows/%s:%d" % (d[0], d[1])
                info["stub"] = classify(d[2])
        out[name] = info
    return out


# ---- 5. the apps' imports --------------------------------------------------------------------------

VERSION_LIBS = [("LIBC", ["libc", "libm", "libdl"]), ("LIBANDROID", ["libandroid"]), ("LIBLOG", ["liblog"]),
                ("LIBEGL", ["libEGL"]), ("LIBGLESV1_CM", ["libGLESv1_CM"]), ("LIBGLESV2", ["libGLESv2"]),
                ("LIBGLESV3", ["libGLESv3"]), ("LIBJNIGRAPHICS", ["libjnigraphics"]),
                ("LIBMEDIANDK", ["libmediandk"]), ("LIBCAMERA2NDK", ["libcamera2ndk"]),
                ("LIBAAUDIO", ["libaaudio"]), ("LIBOPENSLES", ["libOpenSLES"]), ("LIBOPENMAXAL", ["libOpenMAXAL"]),
                ("LIBNATIVEWINDOW", ["libnativewindow"]), ("LIBVULKAN", ["libvulkan"]), ("LIBSYNC", ["libsync"]),
                ("LIBBINDER_NDK", ["libbinder_ndk"]), ("LIBAMIDI", ["libamidi"]), ("ZLIB", ["libz"]),
                ("LIBNEURALNETWORKS", ["libneuralnetworks"]), ("LIBICU", ["libicu"])]


def readelf(path):
    """(needed, defined names, [(name, version, bind, type)] of undefined imports)."""
    exe = "readelf"
    for cand in ("readelf", "llvm-readelf"):
        if subprocess.run(["which", cand], capture_output=True).returncode == 0:
            exe = cand
            break
    dyn = subprocess.run([exe, "-W", "-d", path], capture_output=True, text=True).stdout
    needed = re.findall(r"\(NEEDED\).*\[([^\]]+)\]", dyn)
    syms = subprocess.run([exe, "-W", "--dyn-syms", path], capture_output=True, text=True).stdout
    defined, undefined = set(), []
    for line in syms.splitlines():
        f = line.split()
        if len(f) < 8 or not f[0].endswith(":"):
            continue
        typ, bind, ndx, name = f[3], f[4], f[6], f[7]
        ver = ""
        if "@" in name:
            name, _, ver = name.partition("@")
            ver = ver.lstrip("@")
        if bind == "LOCAL" or not name:
            continue
        if ndx == "UND":
            undefined.append((name, ver, bind, typ))
        else:
            defined.add(name)
    return needed, defined, undefined


def app_imports(so_root, ndk_by_lib):
    """{app: {"libs": [...], "imports": {name: {"lib", "weak", "type", "from": set(sofiles)}}, "other": {...}}}"""
    sym_libs = collections.defaultdict(list)
    for lib, syms in ndk_by_lib.items():
        for s in syms:
            sym_libs[s].append(lib)
    apps = {}
    for app in sorted(os.listdir(so_root)):
        d = os.path.join(so_root, app)
        if not os.path.isdir(d):
            continue
        sos = sorted(f for f in os.listdir(d) if f.endswith(".so"))
        own, per = set(), []
        for so in sos:
            needed, defined, undefined = readelf(os.path.join(d, so))
            own |= defined
            per.append((so, needed, undefined))
        imports, other = {}, {}
        shipped = set(sos)
        unshipped = sorted({n for _, needed, _ in per for n in needed if n not in shipped and n[:-3] not in MAPS})
        for so, needed, undefined in per:
            sys_needed = [n[:-3] for n in needed if n[:-3] in MAPS]
            for name, ver, bind, typ in undefined:
                if name in own:
                    continue
                cands = sym_libs.get(name, [])
                lib = None
                for prefix, libs in VERSION_LIBS:
                    if ver.startswith(prefix):
                        lib = next((l for l in libs if l in cands), None)
                        break
                if not lib:
                    lib = next((l for l in sys_needed if l in cands), None)
                if not lib and cands:
                    lib = cands[0]
                if not lib:
                    o = other.setdefault(name, {"weak": True, "from": set()})
                    o["weak"] = o["weak"] and bind == "WEAK"
                    o["from"].add(so)
                    continue
                e = imports.setdefault(name, {"lib": lib, "weak": True, "type": typ, "from": set()})
                e["weak"] = e["weak"] and bind == "WEAK"
                e["from"].add(so)
        apps[app] = {"sos": sos, "imports": imports, "other": other, "unshipped": unshipped}
    return apps


# ---- report ------------------------------------------------------------------------------------------

# What a phone does and how much the gap matters, for the symbols the surveyed apps import. Judged by
# hand from the import site (which .so, which library family) and Win.apk's code; keyed by name, or by a
# prefix ending in "*". A symbol an app imports that has no entry here is printed with "?" so that a new
# app's gaps are not silently given a verdict.
#   severity: "certain crash" (the first call dereferences what the stub returns, or the caller aborts),
#             "wrong answer" (runs on with a value a phone would not give), "unused" (bound but not
#             reached in these apps, or harmless as it stands).
ANNOTATIONS = {
    "__libc_init": ("Bionic's start-up for an executable: _start (crtbegin) calls it, it runs the constructors "
                    "and main.", "unused",
                    "Imported only by libpuzzlesgen.so, a PIE executable (INTERP /system/bin/linker64) that SGT "
                    "Puzzles runs as a child process; Win.apk never runs an ELF executable (no runner in "
                    "windows/; the guest exec family fails with ENOSYS), so the real gap is running it, not "
                    "this symbol."),
    "fputwc": ("Writes a wide character to a FILE.", "unused",
               "libc++'s wide streams (std::wcout/wcerr) and WebRTC's copy of them; no wide console I/O in these "
               "apps. If reached: returns 0 (a phone returns the character or WEOF), nothing written."),
    "getwc": ("Reads a wide character from a FILE.", "unused",
              "libc++'s std::wcin buffer only. If reached: 0 is L'\\0', not WEOF, so a read loop would spin on "
              "NULs instead of ending."),
    "ungetwc": ("Pushes a wide character back on a FILE.", "unused", "libc++'s std::wcin buffer only."),
    "swprintf": ("Formats into a wchar_t buffer.", "wrong answer",
                 "std::to_wstring (libc++) and PPSSPP/libjxl format with it; the stub returns 0 and writes "
                 "nothing, so the wstring holds whatever was in the buffer. Reached only by wide-string code."),
    "tmpnam": ("Returns a unique temporary path name (Bionic: under /data/local/tmp/).", "certain crash",
               "If PPSSPP reaches it, the stub's NULL is used as a path (strcpy/fopen of NULL). Rare: not on "
               "PPSSPP's normal paths."),
    "feclearexcept": ("Clears the FP exception flags (MXCSR on x86_64).", "wrong answer",
                      "Called from a stripped internal function of libreactnative.so that clears and then tests "
                      "the flags; the stub clears nothing, and fetestexcept's 0 then reports no exception."),
    "fetestexcept": ("Reports the raised FP exception flags.", "wrong answer",
                     "Always 0 in libreactnative.so: an FE_INVALID/FE_OVERFLOW a phone would report (an "
                     "out-of-range double conversion) is never seen. Only matters for such values."),
    "fegetenv": ("Saves the FP environment into *envp.", "unused",
                 "OpenCV's bundled TBB captures the caller's FP settings for its workers "
                 "(task_group_context_impl::capture_fp_settings, task_arena_impl::execute); the stub writes "
                 "nothing and the matching fesetenv does nothing, so workers keep the default environment, "
                 "which is what the app thread has anyway."),
    "fesetenv": ("Restores the FP environment from *envp.", "unused",
                 "TBB's restore of what fegetenv (a no-op here) saved: a no-op pair."),
    "fegetround": ("Returns the current rounding mode.", "unused",
                   "OpenCV's dnn module saves the mode around a rounding step; 0 is FE_TONEAREST on arm64 and "
                   "x86_64, which is the mode actually current."),
    "fesetround": ("Sets the rounding mode; 0 on success.", "wrong answer",
                   "OpenCV's dnn module (code beside ShrinkLayer::create) sets a rounding mode and is told 0 "
                   "while the mode stays round-to-nearest: a .5 case rounds differently. dnn is not on "
                   "OpenSudoku's path unless it runs a network."),
    "__ctype_get_mb_cur_max": ("MB_CUR_MAX: 4 in Bionic's default C.UTF-8 locale (1 only after "
                               "setlocale/uselocale to plain \"C\").", "unused",
                               "Returns 4, which is what a phone answers unless the app switched to \"C\"."),
    "__register_atfork": ("Registers fork handlers (pthread_atfork).", "unused",
                          "fork fails on Win.apk, so no handler would ever run; 0 is the right answer here."),
    "chmod": ("Changes a file's mode; ENOENT for a missing path.", "wrong answer",
              "Always 0, also for a path that does not exist; PPSSPP only sets modes on files it just made."),
    "fchmod": ("Changes an open file's mode.", "unused",
               "libc++ std::filesystem::permissions; Windows has no POSIX modes, 0 matches what callers expect."),
    "fchmodat": ("Changes a file's mode relative to a directory fd.", "unused",
                 "libc++ std::filesystem::permissions; as fchmod."),
    "closelog": ("Closes the syslog connection (Bionic: nothing to close).", "unused", "No-op on a phone too."),
    "openlog": ("Sets syslog's tag and options.", "unused", "Bionic keeps only the tag; nothing observable."),
    "syslog": ("Writes a line to logcat at the given priority.", "wrong answer",
               "The line is dropped instead of reaching the run log; libc++abi reports an uncaught exception "
               "through it before abort(), so that reason is lost from the user's log. Diagnostics only."),
    "freelocale": ("Frees a locale_t from newlocale.", "unused",
                   "newlocale/duplocale return the one shared locale object (locale.cpp), so there is nothing to "
                   "free."),
    "getuid": ("The app's uid, 10000 + app id.", "unused", "A fixed 10100: plausible and stable."),
    "geteuid": ("As getuid.", "unused", "Fixed 10100."),
    "getgid": ("The app's gid (same as its uid for an app).", "unused", "Fixed 10100."),
    "getegid": ("As getgid.", "unused", "Fixed 10100."),
    "getpagesize": ("The page size: 4096 on x86_64; 4096 or 16384 on arm64 devices.", "unused",
                    "4096 is the host's real page size and what mmap uses here."),
    "pthread_attr_destroy": ("Frees nothing on Bionic either (attributes are plain data).", "unused",
                             "Correct as it stands."),
    "pthread_cond_destroy": ("Bionic: marks the condition destroyed; nothing to free.", "unused",
                             "Win.apk's condition is a word in the guest's own struct; nothing to free."),
    "pthread_condattr_destroy": ("Plain data; nothing to free.", "unused", "Correct as it stands."),
    "pthread_mutex_destroy": ("Bionic: EBUSY if locked, else marks it destroyed.", "unused",
                              "Win.apk's mutex is a word in the guest's struct; only the EBUSY check is missing."),
    "pthread_mutexattr_destroy": ("Plain data; nothing to free.", "unused", "Correct as it stands."),
    "pthread_rwlock_destroy": ("Bionic: EBUSY if held, else nothing to free.", "unused",
                               "As pthread_mutex_destroy."),
    "sem_destroy": ("Nothing to free for an unnamed semaphore.", "unused", "Correct as it stands."),
    "pthread_setschedparam": ("Sets a thread's policy/priority; an app asking for SCHED_FIFO/RR gets EPERM.",
                              "wrong answer",
                              "Returns 0 and changes nothing: WebRTC's and tensy's audio/real-time threads "
                              "believe they got the priority (on a phone they would get EPERM and fall back)."),
    "sched_setaffinity": ("Pins a thread to CPUs.", "unused",
                          "Tuning only (libjxl's thread pool); 0 with no pinning is safe."),
    "ANativeWindow_release": ("Drops a reference; the window is freed at zero.", "unused",
                              "Win.apk has one ANativeWindow for every Surface (native_window.cpp, g_window and "
                              "from_surface), so there is no count to drop. The real gap is fromSurface itself: "
                              "android-gif-drawable's GifTextureView and tensy's surface all get the same window."),
    "AMediaCodec_*": ("A real hardware/software codec.", "unused",
                      "OpenCV's videoio MediaNDK backend (VideoCapture/VideoWriter of a file); OpenSudoku "
                      "uses OpenCV on camera frames, not video files. If reached: create* returns NULL (\"no "
                      "codec\", native_media.cpp), which OpenCV reports as a failed open."),
    "AMediaExtractor_*": ("A real demuxer (MP4, WebM, ...).", "unused",
                          "OpenCV's videoio MediaNDK backend; every source is refused (AMEDIA_ERROR_UNSUPPORTED), "
                          "so VideoCapture.open fails cleanly if ever called."),
    "AMediaMuxer_*": ("Writes MP4/WebM/3GP files from encoded samples.", "unused",
                      "OpenCV's MediaNDK VideoWriter only. If reached: AMediaMuxer_new's 0 is a NULL muxer that "
                      "OpenCV checks, then later calls (addTrack/start) get 0 = AMEDIA_OK on NULL: no crash, "
                      "no file."),
}


def annotation(name):
    if name in ANNOTATIONS:
        return ANNOTATIONS[name]
    for k, v in ANNOTATIONS.items():
        if k.endswith("*") and name.startswith(k[:-1]):
            return v
    return ("?", "?", "not judged yet")


def cite(windows, rel, pattern):
    """windows/<rel>:<line> of the first line matching pattern, so the explanation's references follow the code."""
    with open(os.path.join(windows, rel), encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            if re.search(pattern, line):
                return "windows/%s:%d" % (rel, n)
    return "windows/%s" % rel


def explanation(windows):
    c = lambda rel, pat: cite(windows, rel, pat)  # noqa: E731
    return [
        "## 1. How Win.apk resolves a guest library's imports",
        "",
        "- **Loading.** A guest `.so` is mapped by Win.apk's own ELF loader and linked by `GuestLinker` (%s). A "
        "DT_NEEDED name on the list of NDK/public system libraries (%s: libc, libm, libdl, liblog, libz, "
        "libandroid, libEGL, libGLESv1_CM/v2/v3, libOpenSLES, libOpenMAXAL, libmediandk, libjnigraphics, "
        "libvulkan, libaaudio, libamidi, libnativewindow, libstdc++, libcamera2ndk, libneuralnetworks, libsync, "
        "libbinder_ndk, libicu) is not a file: it becomes one shared \"system\" node (%s)."
        % (c("src/linker.cpp", r"^GuestLibrary\* GuestLinker::open"), c("src/linker.cpp", r"bool is_system_library"),
           c("src/linker.cpp", r"if \(is_system_library\(file_name_of\(name\)\)\) return system_")),
        "- **Lookup order** is Bionic's (%s): the linker's own names (libdl's `dlopen`/`dlsym`/... and "
        "`__tls_get_addr`), then RTLD_GLOBAL libraries, then the root's tree breadth-first in DT_NEEDED order; "
        "when the walk meets the system node it asks the host, and the host is asked once more at the end. "
        "So an app's own `libc++_shared.so` or allocator wins over the shim where it comes first, as on a phone."
        % c("src/linker.cpp", r"SymbolResolver scope = "),
        "- **The host side is one flat, global name table, not per library** (%s, `resolve_system`): the Bionic "
        "shim (`mini_bionic_resolve`, %s, which merges mini_bionic.cpp's table with every `src/bionic/*.cpp` "
        "ShimTable: libc, libm, libz, liblog, ALooper, ATrace, ASharedMemory...), then GL/EGL (%s), "
        "`native_window`, `native_asset`, `native_activity` (input queue, AConfiguration), `native_aaudio`, "
        "`native_media` (libmediandk), `native_sensor`, `native_opensles`, and libjnigraphics' three "
        "AndroidBitmap functions (%s). The symbol's version (`@LIBC`, `@LIBANDROID`) and which library it was "
        "asked of are ignored: a name means the same function whatever library declared it."
        % (c("host/guest_libs.cpp", r"^void\* resolve_system"),
           c("src/mini_bionic.cpp", r"^const std::unordered_map<std::string, void\*>& all_symbols"),
           c("host/gl/guest_gl.cpp", r"^void\* guest_gl_resolve"),
           c("src/jnigraphics.cpp", r"^void\* jnigraphics_resolve")),
        "- **On a miss** (%s): a WEAK undefined symbol gets 0, as Bionic's linker gives it. Otherwise "
        "`config.load.unresolved` binds it (%s): a FUNC gets a per-name stub (`missing_stub`, %s) that logs "
        "`guest called a system function not implemented on Windows yet, returning 0: <name>` on its first call "
        "(`report_missing`, %s) and returns 0 in rax and xmm0 (%s); out-parameters, a struct returned through "
        "a hidden pointer and errno are left as they were. Every call is counted and listed at exit "
        "(`stub_hits.h`, %s). An OBJECT gets a shared zeroed 4 KiB block "
        "(`missing_object`, %s). The library therefore always loads; the failure, if any, comes at the first "
        "call, often far from it. `dlsym` of a missing name returns NULL with an error (%s), as on Android, so "
        "code that probes with dlsym sees the truth while code that links directly sees a 0-returning function."
        % (c("src/elf_loader.cpp", r"if \(st_bind\(s.st_info\) == kStbWeak\)"),
           c("host/guest_libs.cpp", r"config.load.unresolved = "),
           c("host/guest_libs.cpp", r"^void\* missing_stub"), c("host/guest_libs.cpp", r"^void report_missing"),
           c("src/abi_thunk.cpp", r"^void\* make_tagged_zero_stub"),
           c("host/stub_hits.h", r"inline void print_summary"),
           c("host/guest_libs.cpp", r"^void\* missing_object"),
           c("src/linker.cpp", r"if \(!found\) \*error")),
        "- **arm64 libraries** (OpenSudoku here) resolve through the same chain, after the interpreter's own "
        "intrinsics (setjmp/longjmp/strtold) and the arm64 ABI overrides (stat, open, mmap, epoll...) "
        "(%s, %s)." % (c("src/arm64/arm64_guest.cpp", r"^void\* guest_intrinsic"),
                       c("src/arm64/arm64_abi.cpp", r"^void\* abi_override")),
        "- **\"Stub\" below** means a table entry whose function body is empty or a single `return <constant>;` "
        "(the script's heuristic, `windows/scripts/api_gaps/ndk_gaps.py` header). Some are deliberate and right "
        "for a PC (an empty sensor list, `getpagesize` 4096, `pthread_*_destroy` with nothing to free); some "
        "are not (`regexec` always REG_NOMATCH, `AAudioStream_read` always 0 frames, every `AMediaCodec_*` "
        "\"no codec\"). GL entries forward to ANGLE and count as implemented.",
        "",
    ]


def group(names):
    """Names of one library and level, shortened: a family of 3+ names sharing `Prefix_` becomes `Prefix_*` (n)."""
    fam = collections.defaultdict(list)
    for n in names:
        m = re.match(r"^([A-Za-z][A-Za-z0-9]*_)", n)
        fam[m.group(1) if m and not n.startswith("_") else n].append(n)
    parts = []
    for k in sorted(fam):
        v = fam[k]
        family = len(v) >= 3 and k.endswith("_")
        parts.append("`%s*` (%d)" % (k, len(v)) if family else ", ".join("`%s`" % x for x in v))
    return ", ".join(parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--so-root", required=True)
    ap.add_argument("--cache", default=os.environ.get("NDK_GAPS_CACHE",
                                                      os.path.join(tempfile.gettempdir(), "ndk_gaps_cache")))
    ap.add_argument("--branch", default="android16-release")
    ap.add_argument("--repo", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
    ap.add_argument("--offline", action="store_true")
    ap.add_argument("--no-native", action="append", default=[], metavar="PACKAGE",
                    help="an app of the survey that ships no .so (named in the report, nothing to read)")
    ap.add_argument("--section", choices=["all", "counts", "imported", "unimported", "stubs"], default="all")
    args = ap.parse_args()
    windows = os.path.join(os.path.abspath(args.repo), "windows")

    ndk_by_lib, sources = {}, {}
    for lib in MAPS:
        text, src = fetch_map(lib, args.cache, args.branch, args.offline)
        if text is None:
            print("<!-- %s: %s -->" % (lib, src))
            continue
        ndk_by_lib[lib] = parse_map(text)
        sources[lib] = src
    ndk = {}
    for lib, syms in ndk_by_lib.items():
        for s, info in syms.items():
            ndk.setdefault(s, dict(info, libs=[]))["libs"].append(lib)
    ours = win_apk_symbols(windows, ndk)
    apps = app_imports(args.so_root, ndk_by_lib)

    def state(sym):
        o = ours.get(sym)
        if not o:
            return "missing"
        return "stub" if o["stub"] else "implemented"

    out = []
    if args.section == "stubs":
        # Every stub found, for checking the heuristic by eye.
        for name in sorted(ours):
            if ours[name]["stub"]:
                out.append("%s\t%s\t%s\t%s" % (name, "/".join(ndk[name]["libs"]), ours[name]["stub"],
                                                ours[name]["def"]))
    if args.section == "all":
        out.append("# Native code of apps: public NDK functions Win.apk lacks or stubs")
        out.append("")
        out.append("Generated by `windows/scripts/api_gaps/ndk_gaps.py` from AOSP's NDK version scripts (%s), "
                   "Win.apk's sources and the dynamic imports of %d apps' native libraries (%s)%s."
                   % (args.branch, len(apps), ", ".join(sorted(apps)),
                      ("; %s ship%s no native code" % (", ".join(args.no_native),
                                                       "s" if len(args.no_native) == 1 else ""))
                      if args.no_native else ""))
        out.append("")
        out.extend(explanation(windows))
    if args.section in ("all", "counts"):
        if args.section == "all":
            out.append("## 2. Per library")
            out.append("")
            out.append("Public = in the library's `*.map.txt`, not `llndk`/`apex`/`systemapi`/`platform-only`, "
                       "present on x86_64 or arm64. A symbol two maps list (libGLESv2 and v3 share the ES 2.0 "
                       "functions; ANativeWindow_* is in libandroid and libnativewindow) counts in each.")
            out.append("")
        out.append("| Library | Public symbols | Implemented | Stubbed | Missing | Map (android.googlesource.com) |")
        out.append("|---|---:|---:|---:|---:|---|")
        for lib, syms in ndk_by_lib.items():
            c = collections.Counter(state(s) for s in syms)
            out.append("| %s | %d | %d | %d | %d | %s |" % (lib, len(syms), c["implemented"], c["stub"], c["missing"],
                                                           sources[lib].split("?")[0].replace(
                                                               "https://android.googlesource.com/platform/", "")))
        out.append("")
    if args.section in ("all", "imported"):
        if args.section == "all":
            out.append("## 3. Missing or stubbed symbols the apps import")
            out.append("")
        out.append("| Symbol | Library | Level | Win.apk today | What Android does | Apps importing it (.so) | "
                   "Severity: reasoning |")
        out.append("|---|---|---:|---|---|---|---|")
        rows = collections.defaultdict(list)
        for app, a in apps.items():
            for name, e in a["imports"].items():
                if state(name) != "implemented":
                    rows[(name, e["lib"])].append((app, e))
        sev_order = {"certain crash": 0, "wrong answer": 1, "unused": 2, "?": 3}

        def today_of(name, users):
            o = ours.get(name)
            if o:
                t = "stub: %s (%s)" % (o["stub"], o["def"] or o["where"])
            elif ndk[name]["var"]:
                t = "missing -> zeroed 4 KiB block"
            else:
                t = "missing -> 0-returning stub"
            return t + ("; weak import -> 0" if all(e["weak"] for _, e in users) else "")

        # One row per symbol; the members of a family judged together (an ANNOTATIONS key ending in "*") that
        # the same .so files import share one row, so the table stays readable.
        merged = collections.OrderedDict()
        for (name, lib), users in sorted(rows.items(), key=lambda r: r[0][0]):
            fam = next((k for k in ANNOTATIONS if k.endswith("*") and name.startswith(k[:-1])), None)
            who = "; ".join("%s (%s)" % (app, ", ".join(sorted(e["from"]))) for app, e in users)
            key = (fam or name, lib, who)
            m = merged.setdefault(key, {"names": [], "today": [], "levels": set(), "who": who, "lib": lib})
            m["names"].append(name)
            m["levels"].add(ndk[name]["level"])
            t = today_of(name, users)
            if fam and t.startswith("stub: "):
                t = "%s: %s" % (name[len(fam) - 1:], t)
            m["today"].append(t)
        for key, m in sorted(merged.items(), key=lambda kv: (sev_order[annotation(kv[1]["names"][0])[1]], kv[1]["lib"],
                                                             kv[1]["names"][0])):
            android, sev, why = annotation(m["names"][0])
            if len(m["names"]) == 1:
                sym, today = "`%s`" % m["names"][0], m["today"][0]
            else:
                prefix = key[0][:-1]
                sym = "`%s{%s}` (%d)" % (prefix, ", ".join(n[len(prefix):] for n in m["names"]), len(m["names"]))
                distinct = list(dict.fromkeys(m["today"]))
                today = "<br>".join(distinct) if not all(t.startswith("missing") for t in distinct) else distinct[0]
            levels = "/".join(str(l) for l in sorted(m["levels"]))
            out.append("| %s | %s | %s | %s | %s | %s | **%s**: %s |"
                       % (sym, m["lib"], levels, today, android, m["who"], sev, why))
        out.append("")
        other = {}
        for app, a in apps.items():
            for name, o in a["other"].items():
                if name.startswith("_Z"):
                    continue
                e = other.setdefault(name, {"weak": True, "apps": set()})
                e["weak"] = e["weak"] and o["weak"]
                e["apps"].add(app)
        if other:
            weak = sorted(n for n, e in other.items() if e["weak"])
            strong = sorted(n for n, e in other.items() if not e["weak"])
            out.append("Imports no NDK map lists (so neither \"missing\" nor \"stubbed\" above):")
            out.append("")
            if weak:
                out.append("- weak, 0 on a phone as here (optional hooks): " + ", ".join(
                    "`%s` (%s)" % (n, ", ".join(sorted(other[n]["apps"]))) for n in weak))
            if strong:
                for app, a in sorted(apps.items()):
                    mine = sorted(n for n in strong if app in other[n]["apps"])
                    if mine:
                        out.append("- %s: %s%s" % (app, group(mine), (" -- from %s, which the app does not ship "
                                   "(the importing library cannot load on a phone either)" % ", ".join(a["unshipped"]))
                                   if a["unshipped"] else ""))
            out.append("")
    if args.section in ("all", "unimported"):
        if args.section == "all":
            out.append("## 4. Missing or stubbed public symbols no surveyed app imports")
            out.append("")
            out.append("Grouped by library and API level; `Prefix_*` (n) stands for n names of one family. "
                       "**Bold** levels are 34 (Android 14), 35 (15), 36 (16) and 37 (in android16-release's maps "
                       "already), whose symbols are listed in full. Whole libraries that are absent (libvulkan, "
                       "libcamera2ndk, libbinder_ndk, libamidi, libsync, libOpenMAXAL, libneuralnetworks, libicu) "
                       "still \"load\": the name is a system library, so DT_NEEDED and dlopen succeed and every "
                       "function is a 0-returning stub (dlsym: NULL).")
            out.append("")
        imported = {n for a in apps.values() for n in a["imports"]}
        for lib, syms in ndk_by_lib.items():
            miss = sorted((s, i["level"]) for s, i in syms.items() if state(s) == "missing" and s not in imported)
            stub = sorted((s, i["level"]) for s, i in syms.items() if state(s) == "stub" and s not in imported)
            if not miss and not stub:
                continue
            whole = len(miss) == len(syms)
            out.append("### %s%s" % (lib, " (nothing implemented: all %d missing)" % len(syms) if whole else ""))
            by_level = collections.defaultdict(list)
            for s, l in miss:
                by_level[l].append(s)
            for l in sorted(by_level):
                names = by_level[l]
                if l >= 34:
                    text = ", ".join("`%s`" % s for s in names)
                    out.append("- **level %d** (%d): %s" % (l, len(names), text))
                elif whole and lib in ("libicu", "libvulkan"):
                    # Vulkan's levels are its core versions: 21 = 1.0 (and WSI), 28 = 1.1, 31 = 1.2, 33 = 1.3.
                    core = {21: "1.0 + Android WSI", 28: "1.1", 31: "1.2", 33: "1.3"}.get(l, "")
                    text = group(names) if lib != "libvulkan" else "the Vulkan %s core entry points" % core
                    out.append("- level %d (%d): %s" % (l, len(names), text))
                elif lib.startswith("libGLES") and len(names) > 20:
                    # GL extension entry points: by vendor suffix (gl*OES), the family a GL programmer looks for.
                    sfx = r"(OES|IMG|EXT|KHR|NV|QCOM|AMD|ARB|ANGLE|Bounds)$"
                    by_ext = collections.Counter(re.search(sfx, n).group(1) if re.search(sfx, n) else "core"
                                                 for n in names)
                    out.append("- level %d (%d): %s" % (l, len(names), ", ".join(
                        "`gl*%s` (%d)" % (k, v) for k, v in sorted(by_ext.items()))))
                else:
                    out.append("- level %d (%d): %s" % (l, len(names), group(names)))
            if stub:
                by_what = collections.defaultdict(list)
                for s, l in stub:
                    by_what[ours[s]["stub"]].append("`%s`%s" % (s, " (**%d**)" % l if l >= 34 else ""))
                out.append("- stubbed: " + "; ".join("%s: %s" % (w, ", ".join(v)) for w, v in sorted(by_what.items())))
            out.append("")
    print("\n".join(out))


if __name__ == "__main__":
    sys.exit(main())
