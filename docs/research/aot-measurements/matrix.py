"""Compile-cost matrix for winapk-aot output.

usage: python3 -I matrix.py <winapk-aot-x> <work dir> <lib.so> [<lib.so> ...]

For each library: a fixed random sample of functions (about SAMPLE instructions,
the library's own size mix), emitted with WINAPK_AOT_ONLY, then compiled part by
part, one clang at a time, under each configuration. Prints one CSV row per
(library, configuration): CPU seconds, instructions per CPU-minute, the largest
peak RSS of one clang, and the object code size per instruction.
"""
import os
import random
import subprocess
import sys
import time

SAMPLE = int(os.environ.get("MATRIX_SAMPLE", "80000"))
BASE = ["--target=x86_64-w64-windows-gnu", "-ffreestanding", "-march=native", "-fno-strict-aliasing", "-w"]

# (name, parts for the sample, extra clang flags, extra environment)
CONFIGS = [
    ("part10k-O1", 8, ["-O1"], {}),
    ("part20k-O1", 4, ["-O1"], {}),
    ("part40k-O1", 2, ["-O1"], {}),
    ("part80k-O1", 1, ["-O1"], {}),
    ("part20k-O1-fastisel", 4, ["-O1", "-mllvm", "-fast-isel"], {}),
    ("part20k-O0", 4, ["-O0"], {}),
    ("part20k-O2", 4, ["-O2"], {}),
    ("part20k-O2-fastisel", 4, ["-O2", "-mllvm", "-fast-isel"], {}),
    ("part20k-O1-optnone1000", 4, ["-O1"], {"WINAPK_AOT_OPTNONE_INSNS": "1000"}),
    ("part20k-O1-optnone16000", 4, ["-O1"], {"WINAPK_AOT_OPTNONE_INSNS": "16000"}),
]


def text_size(obj):
    out = subprocess.run(["llvm-size", "-A", obj], capture_output=True, text=True).stdout
    total = 0
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 2 and p[0].startswith((".text", ".rdata", ".data", ".xdata", ".pdata")):
            total += int(p[1])
    return total


def compile_one(src, obj, flags):
    t = time.time()
    p = subprocess.Popen(["clang"] + BASE + flags + ["-c", "-o", obj, src])
    _, status, ru = os.wait4(p.pid, 0)
    return ru.ru_utime + ru.ru_stime, ru.ru_maxrss / 1024, time.time() - t, status


def main():
    aot, work, libs = sys.argv[1], sys.argv[2], sys.argv[3:]
    only = os.environ.get("MATRIX_ONLY", "")
    print("lib,config,insns,parts,cpu_s,insns_per_cpu_min,max_part_rss_mb,code_bytes_per_insn,failed", flush=True)
    for lib in libs:
        funcs = []
        for line in open(lib + os.environ.get("MATRIX_FUNCS", ".funcs")):
            a, n, _ = line.split()
            funcs.append((a, int(n)))
        rnd = random.Random(1)
        rnd.shuffle(funcs)
        pick, total = [], 0
        for a, n in funcs:
            if total >= SAMPLE:
                break
            pick.append(a)
            total += n
        name = os.path.basename(lib)
        for cname, parts, flags, env in CONFIGS:
            if only and cname not in only.split(","):
                continue
            d = os.path.join(work, name, cname)
            os.makedirs(d, exist_ok=True)
            c = os.path.join(d, "x.c")
            e = dict(os.environ, WINAPK_AOT_ONLY=",".join(pick), WINAPK_AOT_EH="1", **env)
            subprocess.run([aot, lib, c, str(parts)], env=e, capture_output=True)
            srcs = [c] + [c + ".%d.c" % i for i in range(1, parts + 1) if os.path.exists(c + ".%d.c" % i)]
            cpu = rss = 0.0
            size = failed = 0
            for s in srcs:
                ucpu, urss, _, status = compile_one(s, s + ".o", flags)
                cpu += ucpu
                rss = max(rss, urss)
                if status != 0:
                    failed += 1
                else:
                    size += text_size(s + ".o")
                    os.remove(s + ".o")
            for s in srcs:
                os.remove(s)
            print("%s,%s,%d,%d,%.1f,%.0f,%.0f,%.1f,%d" % (name, cname, total, len(srcs) - 1, cpu,
                                                          total / (cpu / 60), rss, size / total, failed), flush=True)


main()
