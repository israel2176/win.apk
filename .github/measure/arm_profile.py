#!/usr/bin/env python3
"""One-off (to be deleted): where an arm64 app's time goes while it runs.

For one app, 30 s headless runs: x86_64 build, arm64 on the JIT only, arm64
translated. Each: frames, process CPU, the busiest threads' CPU, and
WINAPK_ARM64_STATS=1's report (guest time, host-function time, calls, exits).
"""
import argparse, os, shutil, subprocess, sys, threading, time, urllib.request
import psutil


def fetch(pkg, vc, cache):
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, f"{pkg}_{vc}.apk")
    if not os.path.exists(path):
        urllib.request.urlretrieve(f"https://f-droid.org/repo/{pkg}_{vc}.apk", path)
    return path


def run(args, apk, data, extra, name, install_only=False, ticks=1800):
    cmd = [os.path.join(args.runtime, "bin", "winapk-runtime.exe"), "--apk", apk, "--data", data, "--jdk", args.jdk]
    if install_only:
        cmd.append("--install-only")
    env = dict(os.environ, WINAPK_HEADLESS="1", WINAPK_HEADLESS_EXIT_AFTER_FRAMES=str(ticks), WINAPK_ARM64_STATS="1")
    env.update(extra)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    ps = psutil.Process(proc.pid)
    snap = {}

    def sample():
        while proc.poll() is None:
            try:
                snap["threads"] = {t.id: t.user_time + t.system_time for t in ps.threads()}
                c = ps.cpu_times()
                snap["cpu"] = c.user + c.system
            except Exception:
                pass
            time.sleep(0.5)

    threading.Thread(target=sample, daemon=True).start()
    lines = []
    with open(os.path.join(args.out, name + ".log"), "w", encoding="utf-8") as log:
        for raw in proc.stdout:
            line = raw.decode("utf-8", "replace").rstrip()
            lines.append(line)
            log.write(line + "\n")
    proc.wait(timeout=600)
    frames = 0
    for l in lines:
        if "frames presented in" in l:
            frames = int(l.split("] ")[1].split()[0])
    stats = [l for l in lines if "arm64 stats" in l]
    top = sorted(snap.get("threads", {}).items(), key=lambda kv: -kv[1])[:6]
    return {"frames": frames, "cpu": round(snap.get("cpu", 0), 1), "top": [(tid, round(t, 1)) for tid, t in top],
            "stats": stats}


def main():
    ap = argparse.ArgumentParser()
    for a in ("--runtime", "--jdk", "--pkg", "--x86", "--arm", "--out"):
        ap.add_argument(a, required=True)
    args = ap.parse_args()
    args.out, args.runtime = os.path.abspath(args.out), os.path.abspath(args.runtime)
    os.makedirs(args.out, exist_ok=True)
    x86, arm = fetch(args.pkg, args.x86, "apk-cache"), fetch(args.pkg, args.arm, "apk-cache")
    a = {"WINAPK_ABIS": "arm64-v8a"}
    out = []

    d = os.path.join(args.out, "d-x86")
    run(args, x86, d, {}, "x86-install", install_only=True)
    run(args, x86, d, {}, "x86-warmup", ticks=300)
    out.append(("x86", run(args, x86, d, {}, "x86")))

    aot = os.path.join(args.runtime, "bin", "winapk-aot.exe")
    shutil.move(aot, aot + ".off")
    d = os.path.join(args.out, "d-jit")
    run(args, arm, d, a, "jit-install", install_only=True)
    run(args, arm, d, a, "jit-warmup", ticks=300)
    out.append(("arm64 JIT", run(args, arm, d, a, "jit")))
    shutil.move(aot + ".off", aot)

    d = os.path.join(args.out, "d-aot")
    run(args, arm, d, a, "aot-install", install_only=True)
    for i in range(2):
        run(args, arm, d, a, f"aot-warmup{i}", ticks=600)
        t0 = time.time()
        while time.time() - t0 < 900 and any("--aot-translate" in " ".join(p.info["cmdline"] or [])
                                             for p in psutil.process_iter(["cmdline"])):
            time.sleep(5)
    out.append(("arm64 translated", run(args, arm, d, a, "aot")))

    md = [f"## {args.pkg}", "", "| Mode | FPS (30 s) | CPU s | Busiest threads (CPU s) |", "|---|---|---|---|"]
    for name, r in out:
        md.append(f"| {name} | {round(r['frames'] / 30, 1)} | {r['cpu']} | {', '.join(str(t) for _, t in r['top'])} |")
    md.append("")
    for name, r in out:
        md.append(f"### {name}")
        md += ["    " + s for s in r["stats"]] or ["    (no arm64 stats)"]
        md.append("")
    text = "\n".join(md)
    print(text)
    open(os.path.join(args.out, "summary.md"), "w").write(text)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        open(os.environ["GITHUB_STEP_SUMMARY"], "a").write(text)


if __name__ == "__main__":
    sys.exit(main())
