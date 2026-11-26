#!/usr/bin/env python3
"""report.py -- the generated tables of docs/research/api-gaps.md, from ApiGapScan.java's and
UnansweredScan.java's output (run_scan.sh runs all three).

    python3 -I report.py --scan <scan dir> [--min-level 34]

Prints Markdown: counts per level, missing classes and members of --min-level and later grouped by
package and class, stubs of those levels, every app reference that fails or reaches a stub, and the
in-process services' unanswered methods. The document's own tables (severity, what Android does) are
written by hand from these; this keeps the lists themselves reproducible.
"""
import argparse
import collections
import os


def rows(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [l.rstrip("\n").split("\t") for l in f if l.strip() and not l.startswith("#")]


def dotted(c):
    return c.replace("/", ".")


def simple(c):
    return c.rsplit("/", 1)[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scan", required=True)
    ap.add_argument("--min-level", type=int, default=34)
    a = ap.parse_args()
    lo = a.min_level
    classes = rows(os.path.join(a.scan, "classes.tsv"))
    members = rows(os.path.join(a.scan, "members.tsv"))
    stubs = rows(os.path.join(a.scan, "stubs.tsv"))
    out = []
    p = out.append

    # Counts per level.
    per = collections.defaultdict(lambda: [0, 0, 0, 0])
    for r in classes:
        per[int(r[0])][0] += 1
    for r in members:
        per[int(r[0])][1 if r[5] != "missing-constant" else 2] += 1
    for r in stubs:
        per[int(r[0])][3] += 1
    p("| Level | Missing classes | Missing methods and fields | Missing constants (inlined) | Stubs |")
    p("|---:|---:|---:|---:|---:|")
    for lvl in sorted(per):
        c = per[lvl]
        p(f"| {lvl} | {c[0]} | {c[1]} | {c[2]} | {c[3]} |")
    tot = [sum(per[l][k] for l in per) for k in range(4)]
    p(f"| all | {tot[0]} | {tot[1]} | {tot[2]} | {tot[3]} |")
    p("")

    # Missing classes from lo on, by package.
    p(f"### Missing classes, level {lo} and later")
    p("")
    p("| Package | Level: classes |")
    p("|---|---|")
    bypkg = collections.defaultdict(lambda: collections.defaultdict(list))
    for r in classes:
        if int(r[0]) >= lo:
            pkg, _, name = r[1].rpartition("/")
            bypkg[dotted(pkg)][int(r[0])].append(name)
    for pkg in sorted(bypkg):
        cells = "; ".join(f"**{l}**: " + ", ".join(sorted(bypkg[pkg][l])) for l in sorted(bypkg[pkg]))
        p(f"| `{pkg}` | {cells} |")
    p("")

    # Missing members from lo on, by class (constants left out: javac copies them into the app).
    p(f"### Missing methods and fields, level {lo} and later (classes that exist)")
    p("")
    p("| Class | Level: members |")
    p("|---|---|")
    missing_cls = {r[1] for r in classes}
    bycls = collections.defaultdict(lambda: collections.defaultdict(list))
    for r in members:
        lvl, kind, cls, name, desc, prob = int(r[0]), r[1], r[2], r[3], r[4], r[5]
        if lvl < lo or prob == "missing-constant" or cls in missing_cls:
            continue
        label = (name + "()" if kind == "M" else name) + ("" if prob == "missing" else f" ({prob})")
        if name == "<init>":
            label = "constructor " + desc.split(")")[0] + ")"
        bycls[cls][lvl].append(label)
    for cls in sorted(bycls):
        cells = "; ".join(f"**{l}**: " + ", ".join(sorted(set(bycls[cls][l]))) for l in sorted(bycls[cls]))
        p(f"| `{dotted(cls)}` | {cells} |")
    p("")

    # Other link problems at any level.
    odd = [r for r in members if r[5] in ("static-mismatch", "not-public", "abstract")]
    if odd:
        p("### Members that exist but would not link or would not run as on Android (any level)")
        p("")
        p("| Level | Member | Problem |")
        p("|---:|---|---|")
        for r in sorted(odd, key=lambda r: (r[2], r[3])):
            p(f"| {r[0]} | `{dotted(r[2])}.{r[3]}{r[4] if r[1] == 'M' else ''}` | {r[5]} |")
        p("")

    # Stubs from lo on.
    p(f"### Stubs, level {lo} and later")
    p("")
    p("| Level | Method | Body | Jar |")
    p("|---:|---|---|---|")
    for r in sorted((r for r in stubs if int(r[0]) >= lo), key=lambda r: (int(r[0]), r[2], r[3])):
        p(f"| {r[0]} | `{dotted(r[2])}.{r[3]}` | {r[5]} | {r[6]} |")
    p("")

    # App references.
    app = rows(os.path.join(a.scan, "apprefs.tsv"))
    if app:
        p("### What the apps' code uses that fails or reaches a stub")
        p("")
        p("| Problem | Member | Level | Apps | Called from |")
        p("|---|---|---|---|---|")
        order = {"no-class": 0, "no-method": 1, "no-field": 2, "no-member": 3}
        for r in sorted(app, key=lambda r: (order.get(r[0], 9), -len(r[6].split(",")), r[2], r[3])):
            member = dotted(r[2]) + ("." + r[3] if r[3] else "")
            p(f"| {r[0]} | `{member}` | {r[5]} | {r[6].replace(',', ', ')} | {r[7][:80].replace(',', ', ')} |")
        p("")

    # Unanswered in-process service methods.
    un = rows(os.path.join(a.scan, "unanswered.tsv"))
    if un:
        p("### In-process services: methods with no answer")
        p("")
        p("| Interface | Answered | Unanswered methods |")
        p("|---|---:|---|")
        by = collections.defaultdict(lambda: [0, []])
        for r in un:
            by[r[0]][0] += r[3] == "answered"
            if r[3] == "unanswered":
                by[r[0]][1].append(r[1])
        for i in sorted(by):
            total = by[i][0] + len(by[i][1])
            p(f"| `{i}` | {by[i][0]}/{total} | {', '.join(sorted(set(by[i][1])))} |")
        p("")
    print("\n".join(out))


if __name__ == "__main__":
    main()
