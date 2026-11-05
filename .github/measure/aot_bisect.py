#!/usr/bin/env python3
"""One-off (to be deleted): which of PR 79's changes makes translated Jitsi crash.

One variant per job (an environment switch); the arm64 build is installed,
run twice with the translation finishing after each, then run 30 s twice.
"""
import argparse, os, subprocess, sys, time, urllib.request
import psutil


def run(args, apk, data, env, name, install_only=False, ticks=1800):
    cmd = [os.path.join(args.runtime, "bin", "winapk-runtime.exe"), "--apk", apk, "--data", data, "--jdk", args.jdk]
    if install_only:
        cmd.append("--install-only")
    e = dict(os.environ, WINAPK_HEADLESS="1", WINAPK_HEADLESS_EXIT_AFTER_FRAMES=str(ticks), WINAPK_ABIS="arm64-v8a",
             WINAPK_AOT_JOBS="4")
    e.update(env)
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=e, timeout=600)
    text = p.stdout.decode("utf-8", "replace")
    open(os.path.join(args.out, name + ".log"), "w", encoding="utf-8").write(text)
    frames = 0
    for l in text.splitlines():
        if "frames presented in" in l:
            frames = int(l.split("] ")[1].split()[0])
    crash = next((l for l in text.splitlines() if "arm64 guest stopped" in l or "crash dump" in l), "")
    aot = [l for l in text.splitlines() if "arm64 AOT:" in l]
    return frames, p.returncode, crash, len(aot)


def wait_aot():
    t0 = time.time()
    while time.time() - t0 < 900:
        if not any("--aot-translate" in " ".join(p.info["cmdline"] or []) for p in psutil.process_iter(["cmdline"])):
            return round(time.time() - t0)
        time.sleep(5)
    return None


def main():
    ap = argparse.ArgumentParser()
    for a in ("--runtime", "--jdk", "--out", "--variant"):
        ap.add_argument(a, required=True)
    args = ap.parse_args()
    args.out, args.runtime = os.path.abspath(args.out), os.path.abspath(args.runtime)
    os.makedirs(args.out, exist_ok=True)
    apk = os.path.abspath("org.jitsi.meet_26000002.apk")
    if not os.path.exists(apk):
        urllib.request.urlretrieve("https://f-droid.org/repo/org.jitsi.meet_26000002.apk", apk)
    env = {"base": {}, "no-eh": {"WINAPK_EXPERIMENT_NO_EH": "1"}, "o1": {"WINAPK_EXPERIMENT_O1": "1"},
           "no-lookup-cache": {"WINAPK_EXPERIMENT_NO_LOOKUP_CACHE": "1"}}[args.variant]
    d = os.path.join(args.out, "data")
    rows = []
    run(args, apk, d, env, "install", install_only=True)
    for i in range(2):
        rows.append((f"warmup {i}",) + run(args, apk, d, env, f"warmup{i}", ticks=600) + (wait_aot(),))
    for i in range(2):
        rows.append((f"run {i}",) + run(args, apk, d, env, f"run{i}") + (None,))
    md = [f"## Jitsi arm64, variant {args.variant}", "", "| Run | Frames | Exit | Crash | AOT DLLs loaded | Translation wait (s) |",
          "|---|---|---|---|---|---|"]
    md += [f"| {r[0]} | {r[1]} | {r[2]} | {r[3][:120]} | {r[4]} | {r[5]} |" for r in rows]
    text = "\n".join(md) + "\n"
    print(text)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        open(os.environ["GITHUB_STEP_SUMMARY"], "a").write(text)


if __name__ == "__main__":
    sys.exit(main())
