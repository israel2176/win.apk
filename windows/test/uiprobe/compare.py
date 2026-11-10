#!/usr/bin/env python3
"""Lines up what Android did (oracle.txt) with what Win.apk did (ours.txt).

    compare.py <oracle.txt> <ours.txt> [--tolerance DP]

Both files are Probes.runAll's lines. A view's numbers may differ by the
tolerance (1 dp by default: fonts are hinted a little differently, and the
two sides round a pixel their own way); anything further apart is printed,
Android's line first. Exit status 1 when something differs.
"""
import re
import sys

NUM = re.compile(r'-?\d+(?:\.\d+)?')


def cases(path):
    out, name = {}, None
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('case '):
                name = line[5:]
                out[name] = []
            elif name is not None and line.startswith('  '):
                out[name].append(line)
    return out


# Win.apk's inflater hands out AppCompat's and Material's widgets for the plain
# tags wherever the app carries those libraries (as AppCompatActivity's own
# inflater does); the oracle inflates through a bare ContextThemeWrapper. The
# widget is the same one either way, so the names are compared without the prefix.
CLASS = re.compile(r'\b(?:AppCompat|Material)(?=[A-Z][a-z])')


def same(a, b, tolerance):
    """Equal apart from numbers, and each pair of numbers within the tolerance."""
    a, b = CLASS.sub('', a), CLASS.sub('', b)
    if NUM.sub('#', a) != NUM.sub('#', b):
        return False
    return all(abs(float(x) - float(y)) <= tolerance for x, y in zip(NUM.findall(a), NUM.findall(b)))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    tolerance = 1.0
    if '--tolerance' in sys.argv:
        tolerance = float(sys.argv[sys.argv.index('--tolerance') + 1])
        args.remove(sys.argv[sys.argv.index('--tolerance') + 1])
    oracle, ours = cases(args[0]), cases(args[1])
    differing = total = 0
    for name, want in oracle.items():
        got = ours.get(name)
        if got is None:
            print(f'== {name}: missing in Win.apk')
            differing += len(want)
            total += len(want)
            continue
        shown = False
        for i in range(max(len(want), len(got))):
            w = want[i] if i < len(want) else '  (nothing)'
            g = got[i] if i < len(got) else '  (nothing)'
            total += 1
            if same(w, g, tolerance):
                continue
            if not shown:
                print(f'== {name}')
                shown = True
            differing += 1
            print(f'  android:{w}')
            print(f'  win.apk:{g}')
    print(f'{differing} of {total} lines differ')
    return 1 if differing else 0


if __name__ == '__main__':
    sys.exit(main())
