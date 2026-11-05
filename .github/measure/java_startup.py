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
    for raw in proc.stdout:
        lines.append((time.time() - start, raw.decode("utf-8", "replace").rstrip()))
    proc.wait(timeout=300)
    with open(os.path.join(args.out, name + ".log"), "w", encoding="utf-8") as f:
        for t, l in lines:
            f.write(f"{t:8.3f} {l}\n")
    r = {"first_frame": None, "lines": len(lines), "exit": proc.returncode}
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
    ap.add_argument("--reps", type=int, default=3)
    args = ap.parse_args()
    args.out = os.path.abspath(args.out)
    os.makedirs(args.out, exist_ok=True)
    apk = fetch(args.pkg, args.vc, os.path.abspath("apk-cache"))
    data = os.path.join(args.out, "data")
    run(args, apk, data, {}, "install", install_only=True)
    run(args, apk, data, {}, "warmup")

    # Diagnostics, once: is the JDK's class archive (CDS) in use at all, and where classes come from.
    # -Xlog splits on ':', so the file is named relative to the working directory, then moved.
    jsa_log = os.path.join(args.out, "classload.txt")
    run(args, apk, data, {"MACAPK_JVM_OPTS": "-Xlog:cds=info,class+load=info:file=classload.txt"}, "diag-cds")
    if os.path.exists("classload.txt"):
        os.replace("classload.txt", jsa_log)
    shared = other = 0
    cds_lines = []
    if os.path.exists(jsa_log):
        for l in open(jsa_log, encoding="utf-8", errors="replace"):
            if "source: shared objects file" in l:
                shared += 1
            elif "class,load" in l:
                other += 1
            elif "cds" in l and len(cds_lines) < 15:
                cds_lines.append(l.strip()[:200])
    diag = {"classes_from_cds": shared, "classes_loaded_otherwise": other, "cds_log": cds_lines}

    jsa = os.path.join(args.out, "app.jsa")
    configs = [
        ("base", {}),
        ("quiet", {"MACAPK_QUIET_CLASSLOAD": "1"}),
        ("quiet+c1", {"MACAPK_QUIET_CLASSLOAD": "1", "MACAPK_JVM_OPTS": "-XX:TieredStopAtLevel=1"}),
        ("quiet+xshareoff", {"MACAPK_QUIET_CLASSLOAD": "1", "MACAPK_JVM_OPTS": "-Xshare:off"}),
        ("quiet+appcds", {"MACAPK_QUIET_CLASSLOAD": "1",
                          "MACAPK_JVM_OPTS": f"-XX:+AutoCreateSharedArchive -XX:SharedArchiveFile={jsa}"}),
    ]
    results = {}
    for name, env in configs:
        if name == "quiet+appcds":  # one run to write the archive
            r, lines = run(args, apk, data, env, "appcds-dump")
            diag["appcds_archive_written"] = os.path.exists(jsa)
            diag["appcds_dump_msgs"] = [l[:200] for _, l in lines if "shar" in l.lower() or "cds" in l.lower()][:10]
        runs = [run(args, apk, data, env, f"{name}-{i}")[0] for i in range(args.reps)]
        results[name] = runs

    def med(runs, key):
        v = [r[key] for r in runs if r.get(key) is not None]
        return round(statistics.median(v), 2) if v else None

    out = [f"## {args.pkg}", "", "| Config | First frame | Boot.run | manifest | resources | App.onCreate done | Log lines |",
           "|---|---|---|---|---|---|---|"]
    for name, runs in results.items():
        out.append(f"| {name} | {med(runs, 'first_frame')} | " + " | ".join(str(med(runs, m)) for m in MARKS)
                   + f" | {med(runs, 'lines')} |")
    out += ["", "```", json.dumps(diag, indent=1), "```", ""]
    md = "\n".join(out)
    print(md)
    open(os.path.join(args.out, "summary.md"), "w").write(md)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        open(os.environ["GITHUB_STEP_SUMMARY"], "a").write(md)


if __name__ == "__main__":
    sys.exit(main())
