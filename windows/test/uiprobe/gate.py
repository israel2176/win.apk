#!/usr/bin/env python3
"""The CI's UI gate (ci.yml, "UI gate"): the probe's or the screens sweep's
lines of a pull request against its base, never further from Android.

    gate.py complete <oracle.txt> <ours.txt> [--min FRACTION]
        exit 0 when Win.apk's run finished ("done") and printed at least
        FRACTION (default 0.9) of the cases Android printed; 1 otherwise. A
        first run sometimes stops short; gate_side.sh runs it once more then.

    gate.py report <name> <oracle.txt> <base/ours.txt> <head/ours.txt> [--max N] [--note TEXT] [--out FILE]
        regress.py on the three, as Markdown for the job summary: the counts,
        then the first N (default 40) REGRESSED / FARTHER / MISSING entries.
        regress.py's whole output goes to FILE. Exit status regress.py's: 1 on
        any of those.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from compare import cases  # noqa: E402

BAD = ('REGRESSED', 'FARTHER', 'MISSING')


def opt(args, name, default):
    if name in args:
        i = args.index(name)
        value = args[i + 1]
        del args[i:i + 2]
        return value
    return default


def finished(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            return any(line.rstrip('\r\n') == 'done' for line in f)
    except OSError:
        return False


def complete(args):
    need = float(opt(args, '--min', '0.9'))
    oracle, ours = args
    want = len(cases(oracle))
    got = len(cases(ours)) if os.path.exists(ours) else 0
    done = finished(ours)
    ok = done and got >= need * want
    print(f'{got} of {want} cases, {"finished" if done else "did not finish"}: {"complete" if ok else "SHORT"}')
    return 0 if ok else 1


def entries(text):
    """regress.py's report as entries: a line at the margin and the indented lines under it."""
    out = []
    for line in text.splitlines():
        if line.startswith('== '):
            continue
        if line.startswith(' ') and out:
            out[-1] += '\n' + line
        elif line:
            out.append(line)
    return out


def report(args):
    limit = int(opt(args, '--max', '40'))
    note = opt(args, '--note', '')
    full = opt(args, '--out', '')
    name, oracle, base, head = args
    md = [f'### {name}', '']
    if note:
        md += [note, '']
    missing = [p for p in (base, head) if not os.path.exists(p)]
    if missing:
        md.append('No lines to compare: ' + ', '.join(f'`{p}`' for p in missing) + ' was not written.')
        print('\n'.join(md))
        return 1
    for label, path in (('base', base), ('head', head)):
        if not finished(path):
            md.append(f'**The {label} run did not finish**: its missing cases count as MISSING (head) or '
                      'as printed now, not before (base).')
    p = subprocess.run([sys.executable, os.path.join(HERE, 'regress.py'), oracle, base, head],
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    if full:
        with open(full, 'w', encoding='utf-8') as f:
            f.write(p.stdout)
    total = next((line for line in reversed(p.stdout.splitlines()) if line.startswith('== ')), '== (no total)')
    md += ([] if md[-1] == '' else ['']) + ['`' + total[3:] + '`', '']
    bad = [e for e in entries(p.stdout) if e.split(' ', 1)[0] in BAD]
    if bad:
        md += [f'{len(bad)} went further from Android (REGRESSED, FARTHER, MISSING)'
               + (f'; the first {limit}:' if len(bad) > limit else ':'), '', '```']
        md += bad[:limit]
        md += ['```', '']
    if p.returncode not in (0, 1):
        md += ['regress.py failed:', '```', p.stderr[-3000:], '```']
    print('\n'.join(md))
    return p.returncode


def main(argv):
    if not argv or argv[0] not in ('complete', 'report'):
        print(__doc__, file=sys.stderr)
        return 2
    return complete(argv[1:]) if argv[0] == 'complete' else report(argv[1:])


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
