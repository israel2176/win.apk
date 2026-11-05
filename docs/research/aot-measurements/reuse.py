"""How much compiled work survives a growing profile, per way of cutting parts.

usage: python3 -I reuse.py <winapk-aot-x> <lib.so> <work dir>

The profile is modelled as a set of functions that grows: a first session
covers START of the library's instructions, then the profile grows by 10% and
by 25%. Two orders of growth: random (no locality) and clustered (new code
near code already seen, as a feature's functions sit together). For each
snapshot the library is emitted with WINAPK_AOT_ONLY, so each function's C is
exactly what winapk-aot would write (its direct calls depend on what else is
translated). A part's key is the hash of its functions' C; a part whose key
existed before is not compiled again. Prints the share of the new pass's
instructions that must be compiled, per strategy.
"""
import os
import random
import subprocess
import sys

START = 0.30
PART = 20000
JOBS = 15


def emit(aot, lib, funcs, work, tag):
    dump = os.path.join(work, tag + ".dump")
    env = dict(os.environ, WINAPK_AOT_ONLY=",".join(funcs), WINAPK_AOT_EH="1", WINAPK_AOT_DUMP=dump)
    subprocess.run([aot, lib, "/dev/null", "1"], env=env, capture_output=True)
    out = {}
    for line in open(dump):
        a, n, h = line.split()
        out[int(a, 16)] = (int(n), h)
    return out


def balanced(fn):  # today: largest first into JOBS parts, each to the lightest
    order = sorted(fn.items(), key=lambda kv: (-kv[1][0], kv[0]))
    parts = [[] for _ in range(JOBS)]
    load = [0] * JOBS
    for a, (n, h) in order:
        i = load.index(min(load))
        parts[i].append((a, n, h))
        load[i] += n
    return parts


def sequential(fn):  # fixed size, cut in address order of what is translated
    parts, cur, size = [], [], 0
    for a in sorted(fn):
        n, h = fn[a]
        cur.append((a, n, h))
        size += n
        if size >= PART:
            parts.append(cur)
            cur, size = [], 0
    if cur:
        parts.append(cur)
    return parts


def windows(fn, bounds):  # fixed address windows from the whole library
    import bisect
    parts = {}
    for a in sorted(fn):
        n, h = fn[a]
        parts.setdefault(bisect.bisect_right(bounds, a), []).append((a, n, h))
    return list(parts.values())


def keyed(parts):
    return {"|".join(h for _, _, h in p): sum(n for _, n, _ in p) for p in parts}


def main():
    aot, lib, work = sys.argv[1:4]
    os.makedirs(work, exist_ok=True)
    allf = []
    for line in open(lib + ".ehfuncs"):
        a, n, _ = line.split()
        allf.append((int(a, 16), int(n)))
    allf.sort()
    total = sum(n for _, n in allf)
    bounds, acc = [], 0
    for a, n in allf:
        acc += n
        if acc >= PART:
            bounds.append(a + 1)
            acc = 0
    print("library: %d functions, %d instructions" % (len(allf), total))
    for order in ("random", "clustered"):
        rnd = random.Random(7)
        if order == "random":
            seq = allf[:]
            rnd.shuffle(seq)
        else:  # grow around random seeds, in address runs of 1-40 functions
            seq, used = [], set()
            while len(seq) < len(allf):
                i = rnd.randrange(len(allf))
                for j in range(i, min(len(allf), i + rnd.randint(1, 40))):
                    if j not in used:
                        used.add(j)
                        seq.append(allf[j])
        snaps, acc, want = [], 0, [START, START * 1.10, START * 1.10 * 1.25]
        cut = []
        for k, (a, n) in enumerate(seq):
            acc += n
            if want and acc >= want[0] * total:
                cut.append(k + 1)
                want.pop(0)
        ems = [emit(aot, lib, ["%x" % a for a, _ in seq[:c]], work, "%s%d" % (order, i)) for i, c in enumerate(cut)]
        for step, (old, new) in enumerate(((ems[0], ems[1]), (ems[1], ems[2]))):
            grew = "+10%" if step == 0 else "+25%"
            new_insns = sum(n for n, _ in new.values())
            changed_bodies = sum(1 for a in old if a in new and new[a][1] != old[a][1])
            row = []
            for name, cut_fn in (("balanced", balanced), ("sequential", sequential),
                                 ("windows", lambda f: windows(f, bounds))):
                k_old, k_new = keyed(cut_fn(old)), keyed(cut_fn(new))
                todo = sum(n for k, n in k_new.items() if k not in k_old)
                row.append("%s %.0f%%" % (name, 100.0 * todo / new_insns))
            delta = sum(n for a, (n, _) in new.items() if a not in old)
            row.append("delta %.0f%%" % (100.0 * delta / new_insns))
            print("%-9s %s: profile %d insns; old functions whose C changed: %d of %d (%.1f%%); to compile: %s" % (
                order, grew, new_insns, changed_bodies, len(old), 100.0 * changed_bodies / len(old), ", ".join(row)),
                flush=True)


main()
