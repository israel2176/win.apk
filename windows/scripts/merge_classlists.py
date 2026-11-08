#!/usr/bin/env python3
"""Merges HotSpot class lists into windows/java/winapk-framework.classlist.

    merge_classlists.py <out> <list>...

The lists come from app starts with WINAPK_CDS_RECORD_DIR=<dir> (host/cds.h):
one <app>.classlist per app, every class its JVM loaded. The merge is what the
runtime's own merge does (host/cds_util.cpp, merge_classlists): comments and
the per-file " id: <n>" dropped, classes of the app's own loader (lines with
super:/source:) left out, each line once, in the order first seen. The
framework archive is dumped from the result, so a class any of the apps
loaded is in every app's archive.
"""

import sys


def merge(paths):
    seen, out = set(), []
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.rstrip("\r\n")
                if not line or line.startswith("#"):
                    continue
                if not line.startswith("@"):
                    if " super: " in line or " source: " in line:
                        continue
                    line = line.split(" id: ", 1)[0]
                if line not in seen:
                    seen.add(line)
                    out.append(line)
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    lines = merge(sys.argv[2:])
    with open(sys.argv[1], "w", encoding="utf-8", newline="\n") as f:
        f.write("# Win.apk's framework class list (host/cds.h): the classes the smoke apps load,\n")
        f.write("# merged by windows/scripts/merge_classlists.py. HotSpot's class list format.\n")
        f.write("\n".join(lines) + "\n")
    print(f"[merge_classlists] {len(lines)} lines from {len(sys.argv) - 2} lists -> {sys.argv[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
