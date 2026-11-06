#!/usr/bin/env python3
"""One-off (to be deleted): do arm64 apps run translated with .eh_frame_hdr's
starts (WINAPK_AOT_EH_FRAME=1) once a call that returns elsewhere hands over to
the JIT? Install, two runs with the translation finished after each, then two
30 s runs; frames, exit, the crash line if any, AOT DLLs loaded.
"""
import argparse, os, subprocess, sys, time, urllib.request
import psutil


def run(args, apk, data, name, install_only=False, ticks=1800):
    cmd = [os.path.join(args.runtime, "bin", "winapk-runtime.exe"), "--apk", apk, "--data", data, "--jdk", args.jdk]
    if install_only:
        cmd.append("--install-only")
    e = dict(os.environ, WINAPK_HEADLESS="1", WINAPK_HEADLESS_EXIT_AFTER_FRAMES=str(ticks), WINAPK_ABIS="arm64-v8a",
             WINAPK_AOT_JOBS="4")
    if args.eh:
        e["WINAPK_AOT_EH_FRAME"] = "1"
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=e, timeout=400)
        out, code = p.stdout, p.returncode
    except subprocess.TimeoutExpired as t:
        out, code = t.stdout or b"", "timeout"
    text = out.decode("utf-8", "replace")
    open(os.path.join(args.out, name + ".log"), "w", encoding="utf-8").write(text)
    frames = 0
    for l in text.splitlines():
        if "frames presented in" in l:
            frames = int(l.split("] ")[1].split()[0])
    crash = next((l for l in text.splitlines() if "arm64 guest stopped" in l or "crash dump" in l), "")
    dlls = sum(1 for l in text.splitlines() if "arm64 AOT:" in l and "translated functions" in l)
    return frames, code, crash, dlls


def wait_aot():
    t0 = time.time()
    while time.time() - t0 < 1200:
        if not any("--aot-translate" in " ".join(p.info["cmdline"] or []) for p in psutil.process_iter(["cmdline"])):
            return round(time.time() - t0)
        time.sleep(5)
    return None


def main():
    ap = argparse.ArgumentParser()
    for a in ("--runtime", "--jdk", "--out", "--pkg", "--vc"):
        ap.add_argument(a, required=True)
    ap.add_argument("--eh", action="store_true")
    args = ap.parse_args()
    args.out, args.runtime = os.path.abspath(args.out), os.path.abspath(args.runtime)
    os.makedirs(args.out, exist_ok=True)
    apk = os.path.abspath(f"{args.pkg}_{args.vc}.apk")
    if not os.path.exists(apk):
        urllib.request.urlretrieve(f"https://f-droid.org/repo/{args.pkg}_{args.vc}.apk", apk)
    d = os.path.join(args.out, "data")
    rows = []
    run(args, apk, d, "install", install_only=True)
    for i in range(2):
        rows.append((f"warmup {i}",) + run(args, apk, d, f"warmup{i}", ticks=600) + (wait_aot(),))
    for i in range(2):
        rows.append((f"run {i}",) + run(args, apk, d, f"run{i}") + (None,))
    stamps = []
    for root, _, files in os.walk(os.path.join(d, "android")):
        for f in files:
            if f.endswith(".aot.stamp"):
                stamps.append(f"{f[:-10]}: {open(os.path.join(root, f)).read().strip()[:40]}")
    md = [f"## {args.pkg} arm64, eh_frame {'on' if args.eh else 'off'}", "",
          "| Run | Frames | Exit | Crash | AOT DLLs | Translation wait (s) |", "|---|---|---|---|---|---|"]
    md += [f"| {r[0]} | {r[1]} | {r[2]} | {r[3][:110]} | {r[4]} | {r[5]} |" for r in rows]
    md += ["", "Stamps: " + "; ".join(sorted(stamps)), ""]
    text = "\n".join(md)
    print(text)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        open(os.environ["GITHUB_STEP_SUMMARY"], "a").write(text)


if __name__ == "__main__":
    sys.exit(main())
