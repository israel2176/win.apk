#!/usr/bin/env python3
"""One-off (to be deleted): where an app's start goes on the Java side, and what
JVM settings change it. x86_64 builds only (no translation involved); every
configuration runs --reps times on a warm install, and the median is reported.

    java_startup.py --runtime stage --jdk <dir> --pkg P --vc VC --out DIR
"""
import argparse, os, re, statistics, subprocess, sys, time, urllib.request, json

MARKS = ["Boot.run entered", "manifest parsed", "resource index joined", "Application init returned"]


def fetch(pkg, vc, cache):
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, f"{pkg}_{vc}.apk")
    if not os.path.exists(path):
        urllib.request.urlretrieve(f"https://f-droid.org/repo/{pkg}_{vc}.apk", path)
    return path


def run(args, apk, data, extra, name, install_only=False, frames=30):
    exe = os.path.join(args.runtime, "bin", "winapk-runtime.exe")
    cmd = [exe, "--apk", apk, "--data", data, "--jdk", args.jdk] + (["--install-only"] if install_only else [])
    env = dict(os.environ)
    env.update({"WINAPK_HEADLESS": "1", "WINAPK_HEADLESS_EXIT_AFTER_FRAMES": str(frames)})
    env.update(extra)
    start = time.time()
    lines = []
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    import psutil, threading
    cpu = [0.0]
    def sample():
        ps = psutil.Process(proc.pid)
        while proc.poll() is None:
            try:
                t = ps.cpu_times(); cpu[0] = t.user + t.system
            except Exception:
                pass
            time.sleep(0.5)
    threading.Thread(target=sample, daemon=True).start()
    for raw in proc.stdout:
        lines.append((time.time() - start, raw.decode("utf-8", "replace").rstrip()))
    proc.wait(timeout=600)
    with open(os.path.join(args.out, name + ".log"), "w", encoding="utf-8") as f:
        for t, l in lines:
            f.write(f"{t:8.3f} {l}\n")
    r = {"first_frame": None, "lines": len(lines), "exit": proc.returncode, "cpu": round(cpu[0], 1)}
    for t, l in lines:
        if "first frame presented" in l and r["first_frame"] is None:
            r["first_frame"] = round(t, 3)
        for m in MARKS:
            if f"ms {m}" in l and m not in r:
                r[m] = round(t, 3)
    return r, lines


def main():
    ap = argparse.ArgumentParser()
    for a in ("--runtime", "--jdk", "--pkg", "--vc", "--out"):
        ap.add_argument(a, required=True)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--mode", default="start")
    ap.add_argument("--steady-ticks", type=int, default=3600)
    args = ap.parse_args()
    args.out = os.path.abspath(args.out)
    os.makedirs(args.out, exist_ok=True)
    apk = fetch(args.pkg, args.vc, os.path.abspath("apk-cache"))
    data = os.path.join(args.out, "data")
    run(args, apk, data, {}, "install", install_only=True)
    run(args, apk, data, {}, "warmup")

    diag = {}
    c1 = {"MACAPK_JVM_OPTS": "-XX:TieredStopAtLevel=1"}
    results = {}
    if args.mode == "start":
        configs = [("logging on (as before)", {"MACAPK_QUIET_CLASSLOAD": "0"}), ("new defaults", {}),
                   ("new defaults + C1 only", c1)]
        for name, env in configs:
            results[name] = [run(args, apk, data, env, f"{name[:12]}-{i}".replace(" ", "_"))[0]
                             for i in range(args.reps)]
    else:  # steady: frames and CPU over --steady-ticks display ticks, tiered vs C1
        import psutil
        for name, env in [("tiered (today)", {}), ("C1 only", c1)]:
            runs = []
            for i in range(2):
                t0 = time.time()
                r, lines = run(args, apk, data, env, f"steady-{name[:6]}-{i}".replace(" ", "_"),
                               frames=args.steady_ticks)
                frames = max([int(m.group(1)) for _, l in lines
                              for m in [re.search(r"\] (\d+) frames presented", l)] if m] or [0])
                r["fps"] = round(frames / (args.steady_ticks / 60), 1)
                runs.append(r)
            results[name] = runs
    def med(runs, key):
        v = [r[key] for r in runs if r.get(key) is not None]
        return round(statistics.median(v), 2) if v else None

    out = [f"## {args.pkg} ({args.mode})", "",
           "| Config | First frame | Boot.run | manifest | resources | App.onCreate done | Log lines | FPS | CPU s |",
           "|---|---|---|---|---|---|---|---|---|"]
    for name, runs in results.items():
        out.append(f"| {name} | {med(runs, 'first_frame')} | " + " | ".join(str(med(runs, m)) for m in MARKS)
                   + f" | {med(runs, 'lines')} | {med(runs, 'fps')} | {med(runs, 'cpu')} |")
    trust = [l for _, l in run(args, apk, data, {}, "trust-check")[1] if "TLS trust" in l]
    diag["trust_lines"] = trust[:3]
    out += ["", "```", json.dumps(diag, indent=1), "```", ""]
    md = "\n".join(out)
    print(md)
    open(os.path.join(args.out, "summary.md"), "w").write(md)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        open(os.environ["GITHUB_STEP_SUMMARY"], "a").write(md)


if __name__ == "__main__":
    sys.exit(main())
