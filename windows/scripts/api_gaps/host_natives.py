#!/usr/bin/env python3
"""host_natives.py -- which Java native methods of the runtime are bound to a zero stub
(docs/research/api-gaps.md, "Native methods of the framework").

windows/host/host_natives.cpp binds the natives of every class in kHostClasses: a method some provider
table implements ({"name", "signature", fn} under the class's name) gets that function, any other a stub
that returns zero (null, 0, false) and is counted by stub_hits.h ("host natives: N classes, M implemented,
K stubbed" in every run's log). This lists the K: the native methods those classes declare in the shipped
jars that no provider table names. The provider tables are found by their text: a quoted name followed by the
string literals of a JNI signature, in windows/host (WINAPK_HOST_NATIVE) and runtime/src (upstream's GL
and EGL tables, which gl/gl_bridges.cpp registers).

    python3 -I host_natives.py <repo> <stage lib dir>

Needs javap (a JDK) on the PATH. Prints: class  method  signature.
"""
import os
import re
import subprocess
import sys


def main():
    repo, lib = sys.argv[1], sys.argv[2]
    host = os.path.join(repo, "windows", "host")
    src = open(os.path.join(host, "host_natives.cpp"), encoding="utf-8").read()
    block = src[src.index("kHostClasses[] = {"):]
    block = block[:block.index("};")]
    classes = re.findall(r'"([\w.$]+)"', block)
    implemented = set()
    # WINAPK_HOST_NATIVE(kClass, "name", "sig" "sig continued", fn) in windows/host, and upstream's GL and
    # EGL tables ({"name", "sig", (void*) fn}) in runtime/src that gl_bridges.cpp hands over: a name, then
    # one or more adjacent string literals that make a JNI signature.
    pair = re.compile(r'"(\w+)"\s*,\s*((?:\(\s*(?:const\s+)?char\s*\*\s*\)\s*)?(?:"[^"]*"\s*)+)')
    for root in (host, os.path.join(repo, "runtime", "src")):
        for d, _, files in os.walk(root):
            for f in files:
                if not f.endswith((".cpp", ".h", ".inc")):
                    continue
                text = open(os.path.join(d, f), encoding="utf-8", errors="replace").read()
                for name, lits in pair.findall(text):
                    sig = "".join(re.findall(r'"([^"]*)"', lits))
                    if sig.startswith("("):
                        implemented.add((name, sig))
    cp = os.pathsep.join(os.path.join(lib, j) for j in
                         ("aosp-app.jar", "aosp-views.jar", "aosp-resources.jar", "aosp-graphics.jar",
                          "android-framework.jar", "macapk-loader.jar"))
    out = subprocess.run(["javap", "-p", "-s", "-cp", cp] + classes, capture_output=True, text=True).stdout
    cls = None
    pending = None
    stubbed = 0
    for line in out.splitlines():
        m = re.match(r"^(?:public |final |abstract |class |interface )*(?:class|interface) ([\w.$]+)", line.strip())
        if line and not line.startswith(" ") and m:
            cls = m.group(1)
            continue
        s = line.strip()
        if " native " in f" {s} " and "(" in s:
            pending = re.search(r"(\w+)\(", s).group(1)
        elif s.startswith("descriptor:") and pending:
            sig = s.split(":", 1)[1].strip()
            if (pending, sig) not in implemented:
                print(f"{cls}\t{pending}\t{sig}")
                stubbed += 1
            pending = None
        else:
            pending = pending if s.startswith("descriptor") else None
    print(f"# {len(classes)} classes, {stubbed} natives bound to a zero stub", file=sys.stderr)


if __name__ == "__main__":
    main()
