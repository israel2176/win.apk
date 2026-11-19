#!/usr/bin/env python3
"""Did a change only make Win.apk more like Android? Before and after, line by line.

    regress.py <oracle.txt> <before/ours.txt> <after/ours.txt> [--tolerance DP] [--quiet]

All three are the UI probe's (or the screens sweep's) lines: Android's, Win.apk's
before a change, Win.apk's after it. Each line of each case is classed:

  fixed      differed from Android before, matches it now
  closer     differs before and after, but now nearer to Android's numbers
  REGRESSED  matched Android before, differs now
  FARTHER    differs before and after, and now further from Android's numbers
  MISSING    a case Win.apk printed before and not now (a throw, a hang, a crash)
  changed    differs before and after, and the shape changed so that no distance
             can be measured (a field appeared or went): read it
  (a line that only moved within the tolerance, or did not move, is not shown)

Exit status 1 when anything is REGRESSED, FARTHER or MISSING: a change that is
meant to be an improvement only must leave none. compare.py's notion of "the
same" (the tolerance, colours compared as written) decides what matches.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare import CLASS, NUM, cases, same  # noqa: E402


FIELD = re.compile(r' (\w+)=(\S+)')


def comparable(line, android):
    """`line` without what cannot be weighed against Android's: the AppCompat /
    Material class prefix (compare.py ignores it too) and any key=value field
    Android's line does not have (a field one side prints and the other not)."""
    keys = {k for k, _ in FIELD.findall(android)}
    line = CLASS.sub('', line)
    return FIELD.sub(lambda m: m.group(0) if m.group(1) in keys else '', line)


def distance(android, line):
    """Sum of the numbers' differences from Android's line, or None when the
    shapes still differ once comparable() has evened them out."""
    a, b = CLASS.sub('', android), comparable(line, android)
    if NUM.sub('#', a) != NUM.sub('#', b):
        return None
    return sum(abs(float(x) - float(y)) for x, y in zip(NUM.findall(a), NUM.findall(b)))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    tolerance = 1.0
    if '--tolerance' in sys.argv:
        tolerance = float(sys.argv[sys.argv.index('--tolerance') + 1])
        args.remove(sys.argv[sys.argv.index('--tolerance') + 1])
    quiet = '--quiet' in sys.argv
    oracle, before, after = cases(args[0]), cases(args[1]), cases(args[2])

    counts = {k: 0 for k in ('fixed', 'closer', 'REGRESSED', 'FARTHER', 'MISSING', 'changed')}
    report = []
    for name, want in oracle.items():
        b_lines, a_lines = before.get(name), after.get(name)
        if b_lines is None and a_lines is None:
            continue
        if a_lines is None:
            counts['MISSING'] += 1
            report.append(f'MISSING   {name}: printed before, not now')
            continue
        if b_lines is None:
            counts['fixed'] += 1
            report.append(f'fixed     {name}: printed now, not before')
            continue
        for i in range(max(len(want), len(b_lines), len(a_lines))):
            w = want[i] if i < len(want) else '  (nothing)'
            b = b_lines[i] if i < len(b_lines) else '  (nothing)'
            a = a_lines[i] if i < len(a_lines) else '  (nothing)'
            if a == b:
                continue
            ok_b, ok_a = same(w, b, tolerance), same(w, a, tolerance)
            if ok_b and ok_a:
                continue
            # Only a field Android does not print, or the class prefix, changed.
            if not ok_b and not ok_a and comparable(b, w) == comparable(a, w):
                continue
            if ok_b:
                kind = 'REGRESSED'
            elif ok_a:
                kind = 'fixed'
            else:
                db, da = distance(w, b), distance(w, a)
                if db is None or da is None:
                    kind = 'changed'
                elif da < db:
                    kind = 'closer'
                elif da > db:
                    kind = 'FARTHER'
                else:
                    kind = 'changed'
            counts[kind] += 1
            report.append(f'{kind:<9} {name}\n    android:{w}\n    before: {b}\n    after:  {a}')
    order = ('REGRESSED', 'FARTHER', 'MISSING', 'changed', 'closer', 'fixed')
    if not quiet:
        for kind in order:
            for r in report:
                if r.startswith(kind):
                    print(r)
    total = ', '.join(f'{counts[k]} {k}' for k in order)
    bad = counts['REGRESSED'] + counts['FARTHER'] + counts['MISSING']
    print(f'== {total}' + ('' if bad else '  (no line went further from Android)'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
