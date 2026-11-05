#!/usr/bin/env python3
"""One-off (to be deleted): an arm64 app's start, main against this branch.

    arm_start.py --old <main's staged runtime> --new <this branch's> --jdk <dir> --pkg P --vc VC --out DIR

Modes, each with a fresh data folder, an install, one warm-up start, then --reps
measured starts (median): old JIT only, new JIT only (winapk-aot hidden in
both), and new translated (the warm-up's profile translated before measuring).
"""
import argparse, os, shutil, statistics, subprocess, sys, threading, time, urllib.request
import psutil


def fetch(pkg, vc, cache):
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, f"{pkg}_{vc}.apk")
    if not os.path.exists(path):
        urllib.request.urlretrieve(f"https://f-droid.org/repo/{pkg}_{vc}.apk", path)
    return path


def run(rt, args, apk, data, name, install_only=False):
    cmd = [os.path.join(rt, "bin", "winapk-runtime.exe"), "--apk", apk, "--data", data, "--jdk", args.jdk]
    if install_only:
        cmd.append("--install-only")
    env = dict(os.environ, WINAPK_HEADLESS="1", WINAPK_HEADLESS_EXIT_AFTER_FRAMES="30", WINAPK_ABIS="arm64-v8a")
    start = time.time()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    cpu = [0.0]

    def sample():
        try:
            ps = psutil.Process(proc.pid)
            while proc.poll() is None:
                t = ps.cpu_times()
                cpu[0] = t.user + t.system
                time.sleep(0.25)
        except Exception:
            pass

    threading.Thread(target=sample, daemon=True).start()
    first = None
    with open(os.path.join(args.out, name + ".log"), "w", encoding="utf-8") as log:
        for raw in proc.stdout:
            t = time.time() - start
            line = raw.decode("utf-8", "replace").rstrip()
            log.write(f"{t:8.3f} {line}\n")
            if first is None and "first frame presented" in line:
                first = round(t, 3)
    try:
        proc.wait(timeout=300)
    except subprocess.TimeoutExpired:
        proc.kill()
    return {"first": first, "cpu": round(cpu[0], 1), "exit": proc.returncode}


def wait_aot(data, cap):
    t0 = time.time()
    while time.time() - t0 < cap:
        busy = False
        for p in psutil.process_iter(["cmdline"]):
            try:
                if "--aot-translate" in " ".join(p.info["cmdline"] or []) and os.path.basename(data) in " ".join(p.info["cmdline"]):
                    busy = True
            except Exception:
                pass
        if not busy:
            return round(time.time() - t0)
        time.sleep(5)
    return None


def main():
    ap = argparse.ArgumentParser()
    for a in ("--old", "--new", "--jdk", "--pkg", "--vc", "--out"):
        ap.add_argument(a, required=True)
    ap.add_argument("--reps", type=int, default=5)
    args = ap.parse_args()
    args.out = os.path.abspath(args.out)
    os.makedirs(args.out, exist_ok=True)
    apk = fetch(args.pkg, args.vc, os.path.abspath("apk-cache"))
    rows = []

    def mode(label, rt, translate):
        rt = os.path.abspath(rt)
        aot = os.path.join(rt, "bin", "winapk-aot.exe")
        if not translate:
            shutil.move(aot, aot + ".off")
        try:
            data = os.path.join(args.out, "d-" + label.split()[0] + ("-aot" if translate else ""))
            run(rt, args, apk, data, label + "-install", install_only=True)
            cold = run(rt, args, apk, data, label + "-first")
            waited = None
            if translate:
                waited = wait_aot(data, 900)
                run(rt, args, apk, data, label + "-second")
                waited2 = wait_aot(data, 300)
                waited = (waited or 0) + (waited2 or 0)
            runs = [run(rt, args, apk, data, f"{label}-{i}") for i in range(args.reps)]
            firsts = [r["first"] for r in runs if r["first"] is not None]
            cpus = [r["cpu"] for r in runs]
            rows.append((label, cold["first"], round(statistics.median(firsts), 2) if firsts else None,
                         round(statistics.median(cpus), 1), waited, sum(1 for r in runs if r["first"] is None)))
        finally:
            if not translate:
                shutil.move(aot + ".off", aot)

    mode("before JIT", args.old, False)
    mode("after JIT", args.new, False)
    mode("after translated", args.new, True)
    md = [f"## {args.pkg} (arm64-v8a)", "",
          "| Mode | First start (s) | Later starts, median (s) | CPU per start (s) | Translation took (s) | Runs with no frame |",
          "|---|---|---|---|---|---|"]
    md += [f"| {r[0]} | {r[1]} | {r[2]} | {r[3]} | {r[4] if r[4] is not None else ''} | {r[5]} |" for r in rows]
    text = "\n".join(md) + "\n"
    print(text)
    open(os.path.join(args.out, "summary.md"), "w").write(text)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        open(os.environ["GITHUB_STEP_SUMMARY"], "a").write(text)


if __name__ == "__main__":
    sys.exit(main())
