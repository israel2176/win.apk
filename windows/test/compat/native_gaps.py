#!/usr/bin/env python3
"""native_gaps.py -- what the native code of a set of apps imports that the runtime does not have.

    native_gaps.py <repository> <dir of .apk/.xapk/.apkm/.apks> <out dir>

For each app (its x86_64 libraries when it has them, else arm64-v8a, through the splits of a
bundle) the undefined dynamic symbols and DT_NEEDED libraries of every .so are listed
(llvm-readelf), and those no other library of the app defines are looked up in what the runtime
registers: every name a shim or host table carries is a C string in winapk-runtime.exe
(windows/build/host, built first), however the table's macro spells it, plus the string literals
of the sources. What is left would go to the loader's missing-function stub (returns 0), to
zeroed memory (a variable), or fail to load (a library). <out dir>/native_gaps.md ranks them by
the number of apps; native_gaps.json has them per app. C++ names are left out where the app
links libc++_shared, and a library an app names but the download lacks (another split) shows as
missing with its symbols: read the list with that in mind. The Java side's counterpart is
windows/test/apicheck/ApiCheck.java; popular.txt and fetch_apkpure.sh fetch a corpus.
"""
import collections, io, json, os, re, subprocess, sys, tempfile, zipfile
repo, apks, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)
SYSLIBS = set()
for m in re.finditer(r'"(lib[\w+.-]+\.so)"', open(f"{repo}/windows/src/linker.cpp").read()): SYSLIBS.add(m.group(1))
provided = set()
roots = [f"{repo}/windows/src", f"{repo}/windows/host", f"{repo}/windows/include", f"{repo}/windows/build/host/gl",
         f"{repo}/runtime/src"]
for r in roots:
    for d, _, fs in os.walk(r):
        for f in fs:
            if f.endswith((".cpp", ".h", ".inc", ".c")):
                provided.update(re.findall(r'"([A-Za-z_][\w$]*)"', open(os.path.join(d, f), errors="replace").read()))
# Ground truth: every name a table registers, however its macro spells it, is a C string in the binary.
for exe in ("winapk-runtime.exe",):
    provided.update(subprocess.run(["strings", "-n", "2", f"{repo}/windows/build/host/{exe}"], capture_output=True, text=True).stdout.split("\n"))
def elf_syms(path):
    r = subprocess.run(["llvm-readelf", "--dyn-syms", "-d", "-W", path], capture_output=True, text=True).stdout
    und, weak, defs, needed = {}, set(), set(), []
    for line in r.splitlines():
        m = re.search(r"\(NEEDED\)\s+Shared library: \[(.*?)\]", line)
        if m: needed.append(m.group(1)); continue
        p = line.split()
        if len(p) >= 8 and p[0].rstrip(":").isdigit():
            name = p[7].split("@")[0]
            if p[6] == "UND":
                und[name] = p[3]
                if p[4] == "WEAK": weak.add(name)
            else: defs.add(name)
    return und, weak, defs, needed
def abis_of(archive):
    found = set()
    def walk(z):
        for n in z.namelist():
            if n.startswith("lib/") and n.endswith(".so"): found.add(n.split("/")[1])
            elif n.endswith(".apk"):
                try: walk(zipfile.ZipFile(io.BytesIO(z.read(n))))
                except Exception: pass
    walk(zipfile.ZipFile(archive)); return found
def libs_of(archive, abi):
    """(name, bytes) of every .so for the abi, through the splits of an xapk."""
    want = "lib/" + abi + "/"
    def walk(z):
        for n in z.namelist():
            if n.startswith(want) and n.endswith(".so"): yield os.path.basename(n), z.read(n)
            elif n.endswith(".apk"):
                try: yield from walk(zipfile.ZipFile(io.BytesIO(z.read(n))))
                except Exception: pass
    yield from walk(zipfile.ZipFile(archive))
miss_fn, miss_var, miss_lib = (collections.defaultdict(set) for _ in range(3))
apps = {}
for f in sorted(os.listdir(apks)):
    m = re.match(r"(.+)@(\w+)\.(x?apk)$", f)
    if not m: continue
    pkg = m.group(1)
    try: have = abis_of(os.path.join(apks, f))
    except Exception as e: apps[pkg] = {"abi": "broken: %s" % e, "libs": []}; continue
    abi = "x86_64" if "x86_64" in have else "arm64-v8a" if "arm64-v8a" in have else ""
    info = apps[pkg] = {"abi": abi or ("none" if not have else "unsupported: " + ",".join(sorted(have))), "libs": [], "missing_functions": [], "missing_variables": [], "missing_libraries": []}
    und_all, weak_all, defs_all, needed_all = {}, set(), set(), set()
    with tempfile.TemporaryDirectory() as t:
        try:
            for name, data in (libs_of(os.path.join(apks, f), abi) if abi else ()):
                p = os.path.join(t, name); open(p, "wb").write(data)
                und, weak, defs, needed = elf_syms(p)
                info["libs"].append(name); und_all.update(und); weak_all |= weak; defs_all |= defs; needed_all.update(needed)
        except Exception as e:
            info["error"] = str(e)
    own = set(info["libs"])
    for lib in sorted(needed_all - own - SYSLIBS): info["missing_libraries"].append(lib); miss_lib[lib].add(pkg)
    for s, kind in sorted(und_all.items()):
        if not s or s in defs_all or s in provided: continue
        if s.startswith("_Z") and s not in provided and ("libc++_shared.so" in needed_all - own): continue
        tag = s + (" (weak)" if s in weak_all else "")
        if kind == "OBJECT" or kind == "TLS": info["missing_variables"].append(tag); miss_var[tag].add(pkg)
        else: info["missing_functions"].append(tag); miss_fn[tag].add(pkg)
json.dump(apps, open(f"{out}/native_gaps.json", "w"), indent=1)
with open(f"{out}/native_gaps.md", "w") as o:
    o.write(f"{len(apps)} apps, {sum(1 for a in apps.values() if a['libs'])} with native code\n")
    for title, d in (("libraries", miss_lib), ("variables", miss_var), ("functions", miss_fn)):
        o.write(f"\n## missing {title}: {len(d)}\n")
        for k, v in sorted(d.items(), key=lambda kv: (-len(kv[1]), kv[0])):
            o.write(f"{len(v):3d}  {k}  [{', '.join(sorted(v)[:4])}{'…' if len(v) > 4 else ''}]\n")
