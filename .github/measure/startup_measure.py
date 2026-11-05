#!/usr/bin/env python3
"""One-off measurement (to be deleted): where does an arm64 app's start time go?

For one app, with a fresh data directory per mode:

  x86     the app's x86_64 build: no translation at all (the reference)
  jit     the arm64 build with winapk-aot hidden: the JIT runs everything
  user    the arm64 build as a user gets it today: the background translation
          starts at install and runs while the app starts; then we wait for it
          to finish (timed), and run again on the translation

Each run is headless, until --frames frames or --timeout. Every output line is
timestamped from the process start, so the logs show where the time goes.

    startup_measure.py --runtime stage --jdk <dir> --pkg P --x86 VC --arm VC --out DIR
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.request

import psutil


def fetch(pkg, vc, cache):
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, f"{pkg}_{vc}.apk")
    if not os.path.exists(path):
        for base in ("repo", "archive"):
            try:
                urllib.request.urlretrieve(f"https://f-droid.org/{base}/{pkg}_{vc}.apk", path + ".part")
                os.replace(path + ".part", path)
                break
            except Exception as e:
                print(f"fetch {base}: {e}")
    return path


def run(args, apk, data, env_extra, name, install_only=False):
    exe = os.path.join(args.runtime, "bin", "winapk-runtime.exe")
    cmd = [exe, "--apk", apk, "--data", data, "--jdk", args.jdk] + (["--install-only"] if install_only else [])
    env = dict(os.environ)
    env.update({"WINAPK_HEADLESS": "1", "WINAPK_HEADLESS_EXIT_AFTER_FRAMES": str(args.frames),
                "WINAPK_HEADLESS_FRAME": os.path.join(args.out, f"{name}.bmp")})
    env.update(env_extra)
    log_path = os.path.join(args.out, f"{name}.log")
    marks = {}
    start = time.time()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    ps = psutil.Process(proc.pid)
    cpu = [0.0]

    def sample():
        while proc.poll() is None:
            try:
                t = ps.cpu_times()
                cpu[0] = t.user + t.system
            except Exception:
                pass
            time.sleep(0.5)

    threading.Thread(target=sample, daemon=True).start()

    def reader():
        with open(log_path, "w", encoding="utf-8", errors="replace") as log:
            for raw in proc.stdout:
                t = time.time() - start
                line = raw.decode("utf-8", "replace").rstrip()
                log.write(f"{t:8.2f} {line}\n")
                if "first frame presented" in line and "first_frame" not in marks:
                    marks["first_frame"] = round(t, 2)
                if "cannot load" in line or "AOT" in line or ".aot.dll" in line:
                    marks.setdefault("aot_lines", []).append(f"{t:.1f} {line[:200]}")

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    try:
        code = proc.wait(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        for c in ps.children(recursive=True):
            if "--aot-translate" not in " ".join(c.cmdline() if c.is_running() else []):
                c.kill()
        proc.kill()
        proc.wait()
        code = "timeout"
    th.join(5)
    total = round(time.time() - start, 2)
    frames = 0
    for line in open(log_path, encoding="utf-8", errors="replace"):
        m = re.search(r"\] (\d+) frames presented", line)
        if m:
            frames = max(frames, int(m.group(1)))
    r = {"name": name, "exit": code, "seconds": total, "first_frame": marks.get("first_frame"),
         "frames": frames, "cpu_seconds": round(cpu[0], 1)}
    print(json.dumps(r), flush=True)
    return r


def aot_workers(data):
    found = []
    for p in psutil.process_iter(["name", "cmdline"]):
        try:
            cl = " ".join(p.info["cmdline"] or [])
        except Exception:
            continue
        if "--aot-translate" in cl and os.path.basename(data) in cl:
            found.append(p)
    return found


def wait_aot(data, cap, label):
    start = time.time()
    peak_clang = 0
    while time.time() - start < cap:
        w = aot_workers(data)
        if not w:
            break
        n = sum(1 for p in psutil.process_iter(["name"]) if (p.info["name"] or "").lower().startswith("clang"))
        peak_clang = max(peak_clang, n)
        time.sleep(5)
    done = not aot_workers(data)
    secs = round(time.time() - start, 1)
    print(f"[{label}] translation {'finished' if done else 'NOT finished (cap)'} after {secs}s more, "
          f"peak clang processes {peak_clang}", flush=True)
    return {"label": label, "seconds": secs, "finished": done, "peak_clang": peak_clang}


def aot_state(data):
    out = []
    for so in glob.glob(os.path.join(data, "android", "data", "app", "*", "*", "lib", "arm64", "*.so")):
        st = open(so + ".aot.stamp", encoding="utf-8", errors="replace").read().strip() \
            if os.path.exists(so + ".aot.stamp") else "-"
        dll = so + ".aot.dll"
        out.append({"lib": os.path.basename(so), "so_mb": round(os.path.getsize(so) / 1048576, 1),
                    "stamp": st.replace("\n", " ")[:60],
                    "dll_mb": round(os.path.getsize(dll) / 1048576, 1) if os.path.exists(dll) else None})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runtime", required=True)
    ap.add_argument("--jdk", required=True)
    ap.add_argument("--pkg", required=True)
    ap.add_argument("--x86", required=True)
    ap.add_argument("--arm", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--frames", type=int, default=150)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--aot-cap", type=int, default=55 * 60)
    args = ap.parse_args()
    args.runtime, args.out = os.path.abspath(args.runtime), os.path.abspath(args.out)
    os.makedirs(args.out, exist_ok=True)
    cache = os.path.abspath("apk-cache")
    x86_apk, arm_apk = fetch(args.pkg, args.x86, cache), fetch(args.pkg, args.arm, cache)
    res = {"pkg": args.pkg, "cpus": os.cpu_count(), "ram_gb": round(psutil.virtual_memory().total / 2**30, 1),
           "runs": [], "aot": [], "libs": []}
    arm = {"WINAPK_ABIS": "arm64-v8a"}

    # x86_64: the reference.
    d = os.path.join(args.out, "d-x86")
    res["runs"].append(run(args, x86_apk, d, {}, "x86-install", install_only=True))
    res["runs"].append(run(args, x86_apk, d, {}, "x86-run1"))
    res["runs"].append(run(args, x86_apk, d, {}, "x86-run2"))

    # arm64, JIT only: hide winapk-aot so no translation starts.
    aot_exe = os.path.join(args.runtime, "bin", "winapk-aot.exe")
    shutil.move(aot_exe, aot_exe + ".off")
    try:
        d = os.path.join(args.out, "d-jit")
        res["runs"].append(run(args, arm_apk, d, arm, "jit-install", install_only=True))
        res["runs"].append(run(args, arm_apk, d, arm, "jit-run1"))
        res["runs"].append(run(args, arm_apk, d, arm, "jit-run2"))
    finally:
        shutil.move(aot_exe + ".off", aot_exe)

    # arm64 as a user gets it: the translation starts at install, in the background.
    d = os.path.join(args.out, "d-user")
    t0 = time.time()
    res["runs"].append(run(args, arm_apk, d, arm, "user-install", install_only=True))
    res["runs"].append(run(args, arm_apk, d, arm, "user-run1-while-translating"))
    res["aot"].append(wait_aot(d, args.aot_cap, "after run1"))
    res["libs"].append({"after": "first wait", "libs": aot_state(d)})
    res["runs"].append(run(args, arm_apk, d, arm, "user-run2-translated"))
    res["aot"].append(wait_aot(d, max(60, args.aot_cap - (time.time() - t0)), "after run2"))
    res["runs"].append(run(args, arm_apk, d, arm, "user-run3"))
    res["libs"].append({"after": "end", "libs": aot_state(d)})
    res["aot_total_seconds_from_install"] = round(time.time() - t0, 1)

    with open(os.path.join(args.out, "result.json"), "w") as f:
        json.dump(res, f, indent=2)
    lines = [f"## {args.pkg} ({res['cpus']} CPUs, {res['ram_gb']} GB)", "",
             "| Run | First frame (s) | Total (s) | CPU (s) | Frames | Exit |", "|---|---|---|---|---|---|"]
    for r in res["runs"]:
        lines.append(f"| {r['name']} | {r['first_frame']} | {r['seconds']} | {r['cpu_seconds']} | {r['frames']} | {r['exit']} |")
    lines += ["", "Translation waits: " + "; ".join(f"{a['label']}: {a['seconds']}s, "
                                                    f"{'done' if a['finished'] else 'not done'}, "
                                                    f"peak clang {a['peak_clang']}" for a in res["aot"]), ""]
    lines += ["| Library | .so MB | Stamp | DLL MB |", "|---|---|---|---|"]
    for l in res["libs"][-1]["libs"]:
        lines.append(f"| {l['lib']} | {l['so_mb']} | {l['stamp']} | {l['dll_mb']} |")
    md = "\n".join(lines) + "\n"
    print(md)
    with open(os.path.join(args.out, "summary.md"), "w") as f:
        f.write(md)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write(md)


if __name__ == "__main__":
    sys.exit(main())
