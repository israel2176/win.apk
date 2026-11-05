#!/usr/bin/env python3
"""One-off (to be deleted): what class data sharing would give an app's start.

Modes, each --reps warm starts (median seconds to the first frame):
  today          as shipped (java.base patched, so no CDS at all)
  jdk-cds        java.base not patched: the JDK's own archive of its classes
  app-cds        and an archive of this app's classes too, dumped with jcmd
                 from a running start (-XX:+RecordDynamicDumpInfo)
"""
import argparse, os, statistics, subprocess, sys, threading, time, urllib.request


def fetch(pkg, vc, cache):
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, f"{pkg}_{vc}.apk")
    if not os.path.exists(path):
        urllib.request.urlretrieve(f"https://f-droid.org/repo/{pkg}_{vc}.apk", path)
    return path


def run(args, apk, data, extra, name, install_only=False, ticks=30, on_frame=None):
    cmd = [os.path.join(args.runtime, "bin", "winapk-runtime.exe"), "--apk", apk, "--data", data, "--jdk", args.jdk]
    if install_only:
        cmd.append("--install-only")
    env = dict(os.environ, WINAPK_HEADLESS="1", WINAPK_HEADLESS_EXIT_AFTER_FRAMES=str(ticks))
    env.update(extra)
    start = time.time()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    first = None
    with open(os.path.join(args.out, name + ".log"), "w", encoding="utf-8") as log:
        for raw in proc.stdout:
            t = time.time() - start
            line = raw.decode("utf-8", "replace").rstrip()
            log.write(f"{t:8.3f} {line}\n")
            if first is None and "first frame presented" in line:
                first = round(t, 3)
                if on_frame:
                    threading.Thread(target=on_frame, args=(proc.pid,), daemon=True).start()
    try:
        proc.wait(timeout=300)
    except subprocess.TimeoutExpired:
        proc.kill()
    return first


def main():
    ap = argparse.ArgumentParser()
    for a in ("--runtime", "--jdk", "--pkg", "--vc", "--out"):
        ap.add_argument(a, required=True)
    ap.add_argument("--abis", default="")
    ap.add_argument("--reps", type=int, default=5)
    args = ap.parse_args()
    args.out = os.path.abspath(args.out)
    args.runtime = os.path.abspath(args.runtime)
    os.makedirs(args.out, exist_ok=True)
    apk = fetch(args.pkg, args.vc, os.path.abspath("apk-cache"))
    base = {"WINAPK_ABIS": args.abis} if args.abis else {}
    nopatch = dict(base, WINAPK_EXPERIMENT_NO_BASE_PATCH="1")
    rows, notes = [], []
    jsa = os.path.join(args.out, "app.jsa")

    def mode(label, env, data):
        firsts = [run(args, apk, data, env, f"{label}-{i}") for i in range(args.reps)]
        ok = [f for f in firsts if f is not None]
        rows.append((label, round(statistics.median(ok), 2) if ok else None, len(firsts) - len(ok)))

    for label, env in (("diag-nobase", dict(nopatch, MACAPK_JVM_OPTS="-Xlog:cds=info")),
                       ("diag-nopatch", dict(base, WINAPK_EXPERIMENT_NO_BASE_PATCH="2", MACAPK_JVM_OPTS="-Xlog:cds=info"))):
        d = os.path.join(args.out, "d-" + label)
        run(args, apk, d, env, label + "-install", install_only=True)
        run(args, apk, d, env, label)
        lines = [l for l in open(os.path.join(args.out, label + ".log"), encoding="utf-8") if "[cds" in l][:12]
        notes.append(label + ": " + " || ".join(l.strip()[9:170] for l in lines))
    nopatch = dict(base, WINAPK_EXPERIMENT_NO_BASE_PATCH="2")
    for label, env in (("today", base), ("jdk-cds", nopatch)):
        data = os.path.join(args.out, "d-" + label)
        run(args, apk, data, env, label + "-install", install_only=True)
        run(args, apk, data, env, label + "-warmup")
        mode(label, env, data)

    # app-cds: one start with RecordDynamicDumpInfo; 3 s after its first frame, jcmd dumps the archive.
    data = os.path.join(args.out, "d-app")
    run(args, apk, data, nopatch, "app-install", install_only=True)
    run(args, apk, data, nopatch, "app-warmup")
    jcmd = os.path.join(args.jdk, "bin", "jcmd.exe")

    def dump(pid):
        time.sleep(3)
        r = subprocess.run([jcmd, str(pid), "VM.cds", "dynamic_dump", jsa], capture_output=True, text=True)
        notes.append("jcmd: " + (r.stdout + r.stderr).strip().replace("\n", " | ")[:400])

    run(args, apk, data, dict(nopatch, MACAPK_JVM_OPTS="-XX:+RecordDynamicDumpInfo"), "app-dump", ticks=600,
        on_frame=dump)
    notes.append(f"archive: {os.path.getsize(jsa) // 1024} KB" if os.path.exists(jsa) else "archive: not written")
    if os.path.exists(jsa):
        mode("app-cds", dict(nopatch, MACAPK_JVM_OPTS=f"-XX:SharedArchiveFile={jsa}"), data)

    md = [f"## {args.pkg} {args.abis or 'x86_64'}", "", "| Mode | First frame, median (s) | Runs with no frame |",
          "|---|---|---|"] + [f"| {r[0]} | {r[1]} | {r[2]} |" for r in rows] + [""] + ["- " + n for n in notes] + [""]
    text = "\n".join(md)
    print(text)
    open(os.path.join(args.out, "summary.md"), "w").write(text)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        open(os.environ["GITHUB_STEP_SUMMARY"], "a").write(text)


if __name__ == "__main__":
    sys.exit(main())
