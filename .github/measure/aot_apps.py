#!/usr/bin/env python3
"""One-off (to be deleted): arm64 apps on the JIT alone, then translated.

For one app (its arm64-v8a libraries, WINAPK_ABIS=arm64-v8a): two headless
runs on the JIT (WINAPK_ARM64_ENGINE=jit), the background translation run
by hand to the end, two runs with the translation. Each run: exit code,
time to the first frame, frames presented in 1,200 display frames after it,
CPU seconds, peak working set, a crash line if any, the arm64 stats line;
the translation: wall and CPU time, the size of the libraries, of their
DLLs and of the shared cache. Writes a markdown table to the job summary.
"""
import argparse, json, os, re, subprocess, sys, threading, time, urllib.request, zipfile
import psutil

CRASH = re.compile(r"arm64 guest stopped|arm64 AOT: (access violation|illegal instruction)|crash dump|"
                   r"Unhandled|EXCEPTION_|fatal error|guest memory fault")


def kill_workers():
    for p in psutil.process_iter(["cmdline"]):
        try:
            if "--aot-translate" in " ".join(p.info["cmdline"] or []):
                p.kill()
        except psutil.Error:
            pass


def run(args, apk, data, name, engine, frames=1200, limit=420):
    cmd = [os.path.join(args.runtime, "bin", "winapk-runtime.exe"), "--apk", apk, "--data", data, "--jdk", args.jdk]
    env = dict(os.environ, WINAPK_HEADLESS="1", WINAPK_HEADLESS_EXIT_AFTER_FRAMES=str(frames),
               WINAPK_ABIS="arm64-v8a", WINAPK_ARM64_STATS="1")
    if engine:
        env["WINAPK_ARM64_ENGINE"] = engine
    t0 = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    lines, first = [], [None]

    def read():
        for raw in p.stdout:
            line = raw.decode("utf-8", "replace").rstrip()
            lines.append(line)
            if first[0] is None and "first frame" in line:
                first[0] = time.time() - t0

    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    ps = psutil.Process(p.pid)
    cpu = peak = 0.0
    code = None
    while code is None:
        try:
            ct = ps.cpu_times()
            cpu = ct.user + ct.system
            peak = max(peak, ps.memory_info().peak_wset / 2**20)
        except psutil.Error:
            pass
        code = p.poll()
        if code is None and time.time() - t0 > limit:
            p.kill()
            code = "timeout"
        time.sleep(0.25)
    reader.join(10)
    wall = time.time() - t0
    kill_workers()
    text = "\n".join(lines)
    open(os.path.join(args.out, f"{name}.log"), "w", encoding="utf-8").write(text)
    fr = re.search(r"\] (\d+) frames presented in (\d+) display frames", text)
    crash = next((l for l in lines if CRASH.search(l)), "")
    stats = next((l for l in lines if "arm64 stats (" in l), "")
    st = {}
    m = re.search(r"([\d.]+) ms in guest calls, of which ([\d.]+) ms in host", stats)
    if m:
        st["guest_ms"], st["host_ms"] = float(m.group(1)), float(m.group(2))
    m = re.search(r"(\d+) guest entries \((\d+) into AOT code, (\d+) of them went on in the JIT\), (\d+) host calls, "
                  r"(\d+) JIT exits, (\d+) interpreted", stats)
    if m:
        st.update(entries=int(m.group(1)), aot=int(m.group(2)), bails=int(m.group(3)), host_calls=int(m.group(4)),
                  exits=int(m.group(5)), interp=int(m.group(6)))
    dlls = sum(1 for l in lines if "arm64 AOT:" in l and "translated functions" in l)
    return dict(name=name, code=code, wall=round(wall, 1), first=round(first[0], 1) if first[0] else None,
                frames=f"{fr.group(1)}/{fr.group(2)}" if fr else "", cpu=round(cpu, 1), peak_mb=round(peak),
                crash=crash[:160], dlls=dlls, **st)


def libs_dir(data):
    for root, dirs, _ in os.walk(data):
        if root.endswith(os.path.join("lib", "arm64")):
            return root
    return None


def size(path, suffix):
    return sum(os.path.getsize(os.path.join(path, f)) for f in os.listdir(path) if f.endswith(suffix))


def translate(args, data, cache):
    libs = libs_dir(data)
    if not libs:
        return {"error": "no lib/arm64"}
    kill_workers()
    cmd = [os.path.join(args.runtime, "bin", "winapk-runtime.exe"), "--aot-translate", libs, "--cache", cache]
    env = dict(os.environ, WINAPK_AOT_JOBS=str(args.jobs))
    c0 = psutil.cpu_times()
    t0 = time.time()
    log = open(os.path.join(args.out, "translate.log"), "ab")
    p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
    try:
        p.wait(timeout=args.translate_limit)
        done = True
    except subprocess.TimeoutExpired:
        kill_workers()
        p.kill()
        done = False
    c1 = psutil.cpu_times()
    stamps = {f[:-10]: open(os.path.join(libs, f)).read().split()[:3] for f in os.listdir(libs)
              if f.endswith(".aot.stamp")}
    per_lib = {f[:-3]: [round(os.path.getsize(os.path.join(libs, f)) / 2**20, 2),
                        round(os.path.getsize(os.path.join(libs, f[:-3] + ".so.aot.dll")) / 2**20, 2)
                        if os.path.exists(os.path.join(libs, f[:-3] + ".so.aot.dll")) else None]
               for f in os.listdir(libs) if f.endswith(".so")}
    return dict(done=done, wall=round(time.time() - t0), cpu=round((c1.user + c1.system) - (c0.user + c0.system)),
                so_mb=round(size(libs, ".so") / 2**20, 1), dll_mb=round(size(libs, ".aot.dll") / 2**20, 1),
                cache_mb=round(size(cache, ".dll") / 2**20, 1) if os.path.isdir(cache) else 0,
                stamps=stamps, per_lib=per_lib)


def main():
    ap = argparse.ArgumentParser()
    for a in ("--runtime", "--jdk", "--out", "--pkg", "--vc"):
        ap.add_argument(a, required=True)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--translate-limit", type=int, default=2700)
    args = ap.parse_args()
    args.out, args.runtime = os.path.abspath(args.out), os.path.abspath(args.runtime)
    os.makedirs(args.out, exist_ok=True)
    apk = os.path.abspath(f"{args.pkg}_{args.vc}.apk")
    for base in ("https://f-droid.org/repo/", "https://f-droid.org/archive/"):
        if os.path.exists(apk):
            break
        try:
            urllib.request.urlretrieve(f"{base}{args.pkg}_{args.vc}.apk", apk)
        except Exception as e:
            print("download:", base, e)
    with zipfile.ZipFile(apk) as z:
        names = z.namelist()
    abis = sorted({n.split("/")[1] for n in names if n.startswith("lib/") and n.count("/") >= 2})
    arm64 = [n for n in names if n.startswith("lib/arm64-v8a/") and n.endswith(".so")]
    summary = {"pkg": args.pkg, "vc": args.vc, "abis": abis, "arm64_libs": len(arm64)}
    if not arm64:
        summary["skipped"] = "no lib/arm64-v8a"
    else:
        data = os.path.join(args.out, "data")
        cache = os.path.join(data, "aot-cache")
        runs = [run(args, apk, data, "jit-1", "jit"), run(args, apk, data, "jit-2", "jit")]
        summary["translate"] = translate(args, data, cache)
        runs += [run(args, apk, data, "aot-1", None), run(args, apk, data, "aot-2", None)]
        summary["runs"] = runs
    json.dump(summary, open(os.path.join(args.out, "summary.json"), "w"), indent=1)
    md = [f"## {args.pkg} {args.vc} (ABIs {', '.join(abis) or 'none'}; {len(arm64)} arm64 libraries)", ""]
    if "runs" in summary:
        md += ["| Run | Exit | First frame (s) | Frames | Wall (s) | CPU (s) | Peak WS (MB) | AOT DLLs | Guest ms "
               "| AOT entries / bails | Interp. insns | Crash |", "|" + "---|" * 12]
        for r in summary["runs"]:
            md.append(f"| {r['name']} | {r['code']} | {r['first']} | {r['frames']} | {r['wall']} | {r['cpu']} | "
                      f"{r['peak_mb']} | {r['dlls']} | {r.get('guest_ms', '')} | {r.get('aot', '')} / "
                      f"{r.get('bails', '')} | {r.get('interp', '')} | {r['crash']} |")
        t = summary["translate"]
        md += ["", f"Translation: done {t.get('done')}, {t.get('wall')} s wall, {t.get('cpu')} CPU-s; .so "
               f"{t.get('so_mb')} MB, DLLs {t.get('dll_mb')} MB, cache {t.get('cache_mb')} MB",
               "", "Per library (.so MB, DLL MB): " + json.dumps(t.get("per_lib")), "",
               "Stamps: " + json.dumps(t.get("stamps")), ""]
    else:
        md += [summary.get("skipped", ""), ""]
    text = "\n".join(md)
    print(text)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8").write(text + "\n")


if __name__ == "__main__":
    sys.exit(main())
