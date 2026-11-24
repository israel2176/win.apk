"""The comparison matrix: matrix.py <apps.txt> "<selection>" prints {"include": [...]}.

The selection is empty (every app of apps.txt) or packages separated by spaces or commas; a
package apps.txt lacks is given as <package>:<versionCode|latest>, unpinned. Which runtime
goes first alternates from app to app, so neither always gets the colder disk cache."""
import json
import re
import sys


def pinned(path):
    apps = {}
    for line in open(path, encoding="utf-8"):
        body, _, kind = line.partition("#")
        fields = body.split()
        if not fields:
            continue
        if len(fields) != 3 or not re.fullmatch(r"[0-9a-f]{64}", fields[2]):
            sys.exit(f"apps.txt: bad line: {line.strip()}")
        apps[fields[0]] = {"pkg": fields[0], "version": fields[1], "sha256": fields[2], "kind": kind.strip()}
    return apps


def main():
    apps = pinned(sys.argv[1])
    wanted = [w for w in re.split(r"[\s,]+", sys.argv[2] if len(sys.argv) > 2 else "") if w]
    rows = []
    for w in wanted or list(apps):
        pkg, _, version = w.partition(":")
        if not re.fullmatch(r"[A-Za-z0-9_]+(\.[A-Za-z0-9_]+)+", pkg) or not re.fullmatch(r"(\d+|latest)?", version):
            sys.exit(f"not a package[:versionCode]: {w}")
        if version and (pkg not in apps or apps[pkg]["version"] != version):
            rows.append({"pkg": pkg, "version": version, "sha256": "-", "kind": "not pinned"})
        elif pkg in apps:
            rows.append(dict(apps[pkg]))
        else:
            sys.exit(f"{pkg} is not in apps.txt: give it as {pkg}:<versionCode|latest>")
    for i, row in enumerate(rows):
        row["first"] = "winapk" if i % 2 == 0 else "other"
    print(json.dumps({"include": rows}))


main()
