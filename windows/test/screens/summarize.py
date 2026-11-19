#!/usr/bin/env python3
"""The screens sweep's findings, one line per layout, most important first.

    summarize.py <oracle.txt> <ours.txt> [--tolerance DP] [--top N]

1. Layouts that inflate on Android and throw in Win.apk, with Win.apk's
   exception, its root cause and where it was thrown.
2. Layouts that throw on Android only, and layouts that throw on both sides
   (with a different exception, then the same).
3. Layouts drawn on one side only (NOT DRAWN).
4. The layouts with the most differing lines (compare.py's rule), with the
   first difference.
Then the framework classes the Win.apk failures were thrown in, by count.
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'uiprobe'))
from compare import same  # noqa: E402


def read(path):
    head, cases, name = [], collections.OrderedDict(), None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('case '):
                name = line[5:]
                cases[name] = []
            elif name is not None and line.startswith('  '):
                cases[name].append(line)
            elif line != 'done':
                head.append(line)
    return head, cases


def failure(lines):
    """(kind, exception, cause, where) of a case that failed, or None."""
    for i, line in enumerate(lines):
        s = line.strip()
        for kind in ('FAILED ', 'NOT DRAWN '):
            if s.startswith(kind):
                exc = s[len(kind):]
                cause = where = ''
                for more in lines[i + 1:i + 5]:
                    m = more.strip()
                    if m.startswith('cause ') and not cause:
                        cause = m[6:]
                    elif m.startswith('at ') and not where:
                        where = m[3:]
                return kind.strip(), exc, cause, where
    return None


def exc_class(text):
    m = re.match(r'([\w.$]+)', text or '')
    return m.group(1) if m else ''


def layout_of(case):
    return case.rsplit('.', 1)[0]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    tolerance, top = 1.0, 40
    if '--tolerance' in sys.argv:
        tolerance = float(sys.argv[sys.argv.index('--tolerance') + 1])
        args.remove(sys.argv[sys.argv.index('--tolerance') + 1])
    if '--top' in sys.argv:
        top = int(sys.argv[sys.argv.index('--top') + 1])
        args.remove(sys.argv[sys.argv.index('--top') + 1])
    ohead, oracle = read(args[0])
    whead, ours = read(args[1])

    print('== Header (Android | Win.apk)')
    for i in range(max(len(ohead), len(whead))):
        a = ohead[i] if i < len(ohead) else ''
        b = whead[i] if i < len(whead) else ''
        print(f'  {a}' if a == b else f'  {a}  |  {b}   <-- differs')

    layouts = list(dict.fromkeys(layout_of(c) for c in list(oracle) + list(ours)))
    only_android = [l for l in layouts if l + '.ltr' in oracle and l + '.ltr' not in ours]
    only_ours = [l for l in layouts if l + '.ltr' in ours and l + '.ltr' not in oracle]
    print(f'\n{len(layouts)} layouts; on Android only: {len(only_android)}; in Win.apk only: {len(only_ours)}')
    last = list(ours)[-1] if ours else None
    if ours and 'done' not in open(args[1], encoding='utf-8', errors='replace').read().split('\n'):
        print(f'  Win.apk did not finish: the last case it began is {last} (a hang or a crash there)')
    for l in only_android[:20]:
        print(f'  Android only: {l}')
    for l in only_ours[:20]:
        print(f'  Win.apk only: {l}')

    groups = collections.defaultdict(list)  # bucket -> [(layout, detail)]
    thrown_in = collections.Counter()
    differing = []
    for l in layouts:
        fo = [failure(oracle.get(l + d, [])) for d in ('.ltr', '.rtl') if l + d in oracle]
        fw = [failure(ours.get(l + d, [])) for d in ('.ltr', '.rtl') if l + d in ours]
        if not fo or not fw:
            continue
        o, w = fo[0], fw[0]
        ofail = o is not None and o[0] == 'FAILED'
        wfail = w is not None and w[0] == 'FAILED'
        if wfail and not ofail:
            groups['1'].append((l, w))
            where = w[3]
            m = re.match(r'([\w.$]+)\.[\w$<>]+\(', where)
            if m:
                thrown_in[m.group(1)] += 1
        elif ofail and not wfail:
            groups['2a'].append((l, o))
        elif ofail and wfail:
            key = '2b' if exc_class(o[2] or o[1]) != exc_class(w[2] or w[1]) else '2c'
            groups[key].append((l, (o, w)))
        if not ofail and not wfail:
            odraw = o is not None and o[0] == 'NOT DRAWN'
            wdraw = w is not None and w[0] == 'NOT DRAWN'
            if odraw != wdraw:
                groups['3'].append((l, w if wdraw else o, 'Win.apk' if wdraw else 'Android'))
        # Differing lines over both directions.
        n, total, first = 0, 0, None
        for d in ('.ltr', '.rtl'):
            want, got = oracle.get(l + d, []), ours.get(l + d, [])
            for i in range(max(len(want), len(got))):
                a = want[i] if i < len(want) else '  (nothing)'
                b = got[i] if i < len(got) else '  (nothing)'
                total += 1
                if not same(a, b, tolerance):
                    n += 1
                    if first is None:
                        first = (d[1:], a.strip(), b.strip())
        if n:
            differing.append((n, total, l, first))

    def show(text):
        return text if len(text) <= 300 else text[:297] + '...'

    print(f'\n== 1. Inflate on Android, throw in Win.apk: {len(groups["1"])}')
    for l, (kind, exc, cause, where) in groups['1']:
        print(f'  {l}')
        print(f'      {show(exc)}')
        if cause:
            print(f'      cause {show(cause)}')
        if where:
            print(f'      at {where}')
    print(f'\n== 2a. Throw on Android only: {len(groups["2a"])}')
    for l, (kind, exc, cause, where) in groups['2a']:
        print(f'  {l}: {show(cause or exc)}')
    print(f'\n== 2b. Throw on both, differently: {len(groups["2b"])}')
    for l, (o, w) in groups['2b']:
        print(f'  {l}')
        print(f'      android: {show(o[2] or o[1])}')
        print(f'      win.apk: {show(w[2] or w[1])}')
    print(f'\n== 2c. Throw on both, alike: {len(groups["2c"])}')
    for l, (o, w) in groups['2c']:
        print(f'  {l}: {show(o[2] or o[1])}')
    print(f'\n== 3. Drawn on one side only: {len(groups["3"])}')
    for l, (kind, exc, cause, where), side in groups['3']:
        print(f'  {l}: not drawn on {side}: {show(cause or exc)} at {where}')

    differing.sort(key=lambda x: (-x[0], x[2]))
    print(f'\n== 4. Most differing lines ({len(differing)} of {len(layouts)} layouts differ; top {top})')
    for n, total, l, first in differing[:top]:
        print(f'  {n:4d}/{total:<4d} {l}')
        if first:
            print(f'      {first[0]} android:{show(first[1])}')
            print(f'      {first[0]} win.apk:{show(first[2])}')

    if thrown_in:
        print('\n== Where the Win.apk-only failures were thrown')
        for cls, n in thrown_in.most_common():
            print(f'  {n:4d} {cls}')
    # Exceptions grouped, so one framework gap shows once.
    kinds = collections.Counter(exc_class(w[2] or w[1]) + ': ' + re.sub(r'\d+', '#', (w[2] or w[1]))[:160]
                                for _, w in groups['1'])
    if kinds:
        print('\n== Win.apk-only failures by root cause')
        for k, n in kinds.most_common():
            print(f'  {n:4d} {k}')


if __name__ == '__main__':
    main()
