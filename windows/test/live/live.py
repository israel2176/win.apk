"""A live Win.apk test session on one Windows runner (test only; nothing here
ships). Run by .github/workflows/apps-live-test.yml.

    live.py --runtime <stage> --apks <dir> --data <dir> --out <dir> --branch live/<run id> [--hours 5.5]

Checks every file under --apks with winapk-runtime --verify-apk, names it by
the package it holds and installs one file per package (a package of Google's
only when Google signed it), then polls the branch every ~20 s for cmd.txt.
When its content changes, each line runs in order and the results are pushed
back to the same branch under results/<seq>-<command>/ (logs, view dumps,
screenshots, thread dumps; never an APK, never a secret), with status.txt and
done.txt (the hash of the cmd.txt that was run). The branch is public: text
is copied with every line that may carry account data dropped (scrub()), and
the data folder's gms folder is never copied.
Google services are on (GoogleServices=1) with Google's three apps installed
first, when they verify, and every run has
WINAPK_TEST_INSTALLER=com.android.vending (Google Play as installer). After
each run the system broker's logs (logs/system.log, logs/services/) are copied
beside the run's. Commands:

    run <pkg> [seconds] [dump=S,S..] [pan=S] [resize=S]
                               open the app in a window, follow it (harness.drive);
                               dump=: a Java thread dump (jcmd Thread.print) at those seconds;
                               pan=: at S, the CPU it uses idle for 10 s, then while dragged for 10 s;
                               resize=: at S, the window made 30% wider and 20% lower, a picture 5 s later
    env NAME=VALUE             set (or with an empty VALUE, drop) an env var for later runs
    install <pkg>              install the app again (its data is kept)
    fetch <url> [name.apk]     download a file onto the runner (curl; never uploaded), check it with
                               --verify-apk (a Google package only when Google signed it), install it;
                               the link is not repeated in results, status or commit messages
    clear <pkg>                remove the app's data (android/data/data/<pkg>, apps/<pkg> caches)
    update-java <sha|branch>   build israel2176/win.apk's Java at that commit and put the jars in the stage
    update-exe <ci run id>     replace the stage with that Win.apk CI run's runtime-stage artifact
    logs                       Win.apk's logs folder (crash dumps excepted)
    ps <powershell>            one PowerShell line (diagnostics)
    stop                       end the session
"""

import argparse
import gzip
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402

REPO_WINAPK = "https://github.com/israel2176/win.apk.git"
# Google's Services Framework, Play services and Play Store: installed first, in this order.
GOOGLE = ("com.google.android.gsf", "com.google.android.gms", "com.android.vending")


def verify_apk(args, path):
    """winapk-runtime --verify-apk's KEY/value lines and exit code (0: Google signed it)."""
    code, out = sh([os.path.join(args.runtime, "bin", "winapk-runtime.exe"), "--verify-apk", path, "--data", args.data],
                   timeout=600, env=harness.clean_env())
    return code, out


def sh(cmd, cwd=None, timeout=1800, env=None):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout, env=env,
                       encoding="utf-8", errors="replace")
    return r.returncode, (r.stdout or "") + (r.stderr or "")


class Branch:
    def __init__(self, branch, workdir):
        self.branch = branch
        self.dir = workdir
        shutil.rmtree(self.dir, ignore_errors=True)
        sh(["git", "worktree", "add", "--detach", self.dir])
        self.git("checkout", "--orphan", "live")
        self.git("rm", "-rf", "--cached", ".")
        for n in os.listdir(self.dir):
            if n != ".git":
                p = os.path.join(self.dir, n)
                shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else os.remove(p)
        self.git("config", "user.name", "github-actions[bot]")
        self.git("config", "user.email", "41898282+github-actions[bot]@users.noreply.github.com")
        self.write("README.md", __doc__)
        self.push("live session started")

    def git(self, *a):
        return sh(["git", *a], cwd=self.dir, timeout=300)

    def write(self, name, text):
        p = os.path.join(self.dir, name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(text)

    def pull(self):
        if self.git("fetch", "-q", "origin", self.branch)[0] == 0:
            self.git("reset", "-q", "--hard", "FETCH_HEAD")

    def push(self, msg):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", msg)
        for _ in range(3):
            if self.git("push", "-q", "origin", f"HEAD:refs/heads/{self.branch}")[0] == 0:
                return
            # Someone pushed a new cmd.txt meanwhile: keep theirs and ours.
            self.git("pull", "-q", "--rebase", "-X", "ours", "origin", self.branch)


# A line that may carry an account, a token or a device identifier: never pushed
# (the branch is public). Signed-out runs have few; a false hit costs one line.
SECRET_LINE = re.compile(r"(token|oauth|bearer|authori[sz]ation|cookie|passw|secret|credential|aas_et|ya29\.|ghs_|"
                         r"gh[pousr]_[A-Za-z0-9]{20}|@gmail|@googlemail|accountname|account=|authAccount|"
                         r"android_?id|gsf_?id|device_?id|\bserial(no)?\b|imei|droidguard|\blsid\b|\bsid=|auth=|"
                         r"refresh_|access_token|id_token|x-goog|apk_urls|https?://\S*\?\S*(key|sig|token)=)", re.I)
TEXT_EXTS = (".log", ".txt", ".md", ".json", ".properties", ".xml", ".csv", ".tsv", "")


def scrub(text):
    out, dropped = [], 0
    for line in text.split("\n"):
        if SECRET_LINE.search(line):
            dropped += 1
            out.append("[line dropped: it may carry account data]")
        else:
            out.append(line)
    return "\n".join(out), dropped


def copy_file(s, t):
    """One file into the branch: text scrubbed (gzip'd past 1 MB), PNGs as they are, anything else left out."""
    n = os.path.basename(s).lower()
    if n.endswith(".png"):
        os.makedirs(os.path.dirname(t), exist_ok=True)
        shutil.copy2(s, t)
        return
    if not n.endswith(TEXT_EXTS) or n.endswith((".apk", ".apks", ".xapk", ".apkm", ".dex", ".so", ".jar", ".dmp")):
        return
    try:
        raw = open(s, "rb").read()
    except OSError:
        return
    if b"\0" in raw[:4096]:
        return
    text, _ = scrub(raw.decode("utf-8", "replace"))
    data = text.encode("utf-8")
    os.makedirs(os.path.dirname(t), exist_ok=True)
    if len(data) > (1 << 20):
        with gzip.open(t + ".gz", "wb") as fh:
            fh.write(data)
    else:
        with open(t, "wb") as fh:
            fh.write(data)


def copy_results(src_dir, dst_dir):
    """A folder into the branch (copy_file each), never a gms folder (the Google account's state)."""
    for root, dirs, files in os.walk(src_dir):
        dirs[:] = [x for x in dirs if x.lower() != "gms"]
        for n in files:
            s = os.path.join(root, n)
            copy_file(s, os.path.join(dst_dir, os.path.relpath(s, src_dir)))


def copy_broker_logs(args, res):
    """The system broker's logs (logs/system.log, logs/services/) and the Google setup's, beside a run's."""
    logs = os.path.join(args.data, "logs")
    for n in ("system.log", "services", "gms.log", "google-setup.log"):
        s = os.path.join(logs, n)
        if os.path.isdir(s):
            copy_results(s, os.path.join(res, "broker", n))
        elif os.path.exists(s):
            copy_file(s, os.path.join(res, "broker", n))


def check(args, f, root):
    """(package, usable, a line saying why) for one downloaded file: --verify-apk must read a package
    and a signature that verifies, and a package of Google's must be signed by Google."""
    code, out = verify_apk(args, f)
    kv = dict(l.split("\t", 1) for l in out.splitlines() if "\t" in l)
    pkg = kv.get("PACKAGE", "")
    google_pkg = pkg in GOOGLE or pkg.startswith("com.google.")
    ok = bool(re.fullmatch(r"[A-Za-z0-9_.]+", pkg)) and (code == 0 if google_pkg else code in (0, 4))
    line = (f"{os.path.relpath(f, root)}: exit {code}, package {pkg or '?'}, google={kv.get('GOOGLE')}, "
            f"abis={kv.get('ABIS')}, minsdk={kv.get('MINSDK')}"
            + (f", reason={kv.get('REASON')}" if kv.get("REASON") else "") + ("" if ok else " -> NOT USED"))
    return pkg, ok, line


def take(f, pkg, root):
    """The file as <root>/<package><ext>: the library name the commands use."""
    dst = os.path.join(root, pkg + os.path.splitext(f)[1].lower())
    if os.path.abspath(dst) != os.path.abspath(f):
        if os.path.exists(dst):
            os.remove(dst)
        shutil.move(f, dst)
    return dst


def fetch(args, url, name, seq, root):
    """fetch <url> [name]: the file onto the runner (never uploaded), checked; (path or None, package, log).
    Nothing here repeats the link: curl's own messages are left out."""
    stem = name or url.split("?", 1)[0].split("#", 1)[0].rstrip("/").rsplit("/", 1)[-1]
    ext = os.path.splitext(stem)[1].lower()
    ext = ext if ext in harness.EXTS else ".apk"
    d = os.path.join(root, "fetched")
    os.makedirs(d, exist_ok=True)
    f = os.path.join(d, f"fetch-{seq}{ext}")
    code, _ = sh(["curl", "-fsSL", "--retry", "2", "-o", f, url], timeout=3600)
    if code or not os.path.exists(f):
        return None, "", [f"download FAILED (curl exit {code})"]
    pkg, ok, line = check(args, f, root)
    log = [f"downloaded {os.path.getsize(f) >> 20} MB ({ext})", "verify " + line]
    if not ok:
        os.remove(f)
        return None, pkg, log
    return take(f, pkg, root), pkg, log


def update_java(args, ref, log):
    src = os.path.join(os.environ.get("RUNNER_TEMP", "."), "winapk-src")
    if not os.path.isdir(os.path.join(src, ".git")):
        log.append(sh(["git", "clone", "-q", "--filter=blob:none", "--no-checkout", REPO_WINAPK, src])[1])
    code, out = sh(["git", "fetch", "-q", "origin", ref], cwd=src)
    log.append(out)
    code, out = sh(["git", "checkout", "-q", "-f", "FETCH_HEAD"], cwd=src)
    log.append(out)
    if code:
        return False
    log.append("win.apk at " + sh(["git", "log", "--oneline", "-1"], cwd=src)[1])
    built = os.path.join(os.environ.get("RUNNER_TEMP", "."), "java-out")
    shutil.rmtree(built, ignore_errors=True)
    bash = r"C:\Program Files\Git\bin\bash.exe"
    code, out = sh([bash, "windows/scripts/build_java.sh", built.replace("\\", "/")], cwd=src, timeout=3600)
    log.append(out[-20000:])
    if code:
        return False
    lib = os.path.join(built, "lib")
    for n in os.listdir(lib):
        if n.endswith((".jar", ".classlist")):
            shutil.copy2(os.path.join(lib, n), os.path.join(args.runtime, "lib", n))
            log.append(f"stage/lib/{n} replaced")
    # The shared framework archive is for the old jars; the next start makes a new one.
    shutil.rmtree(os.path.join(args.data, "cds"), ignore_errors=True)
    return True


def update_exe(args, run_id, log):
    tmp = os.path.join(os.environ.get("RUNNER_TEMP", "."), f"stage-{run_id}")
    shutil.rmtree(tmp, ignore_errors=True)
    code, out = sh(["gh", "run", "download", run_id, "-R", "israel2176/win.apk", "-n", "runtime-stage", "-D", tmp])
    log.append(out)
    if code or not os.path.exists(os.path.join(tmp, "bin", "winapk-runtime.exe")):
        return False
    harness.kill_all()
    # Copied over the stage in place (a folder an exited process held may not
    # delete at once), then checked: a stage without its jars runs nothing.
    shutil.copytree(tmp, args.runtime, dirs_exist_ok=True)
    shutil.rmtree(tmp, ignore_errors=True)
    shutil.rmtree(os.path.join(args.data, "cds"), ignore_errors=True)
    missing = [p for p in ("bin/winapk-runtime.exe", "lib/android-framework.jar", "lib/macapk-loader.jar")
               if not os.path.exists(os.path.join(args.runtime, p))]
    if missing:
        log.append(f"FAILED: the stage lacks {missing} after the copy")
        return False
    log.append(f"stage updated from run {run_id}'s runtime-stage")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runtime", required=True)
    ap.add_argument("--apks", required=True)
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--branch", required=True)
    ap.add_argument("--hours", type=float, default=5.5)
    a = ap.parse_args()
    args = argparse.Namespace(runtime=os.path.abspath(a.runtime), data=a.data, out=os.path.abspath(a.out),
                              jdk="", minutes=3, install_timeout=1800, dump_at=[], actions=[],
                              extra_env={"WINAPK_TEST_INSTALLER": "com.android.vending"})
    os.makedirs(args.out, exist_ok=True)
    shutil.rmtree(args.data, ignore_errors=True)
    os.makedirs(args.data, exist_ok=True)
    subprocess.run(["reg", "add", r"HKCU\Software\Win.apk", "/v", "Landscape", "/t", "REG_DWORD", "/d", "0", "/f"],
                   capture_output=True)
    # Settings > Google account > Use Google services: the runtime reads it and sets MACAPK_BROKER=1.
    subprocess.run(["reg", "add", r"HKCU\Software\Win.apk", "/v", "GoogleServices", "/t", "REG_DWORD", "/d", "1",
                    "/f"], capture_output=True)
    br = Branch(a.branch, os.path.join(os.environ.get("RUNNER_TEMP", "."), "live-wt"))
    status = []

    def note(msg):
        line = f"[{harness.now()}] {msg}"
        # python -I ignores PYTHONIOENCODING: the console is cp1252, and app texts are Hebrew.
        print(line.encode("ascii", "backslashreplace").decode("ascii"), flush=True)
        status.append(line)
        br.write("status.txt", "\n".join(status[-400:]) + "\n")

    if os.path.exists(os.path.join(a.apks, "sources.txt")):
        copy_file(os.path.join(a.apks, "sources.txt"), os.path.join(br.dir, "sources.txt"))
    # Every downloaded file, checked; the first that passes names its package's source: the user's
    # links (urls/) before APKPure's. Google's packages only when Google signed them.
    srcs, checks = {}, []
    files = []
    for sub in ("urls", "apkpure"):
        d = os.path.join(a.apks, sub)
        if os.path.isdir(d):
            files += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith(harness.EXTS)]
    for f in files:
        pkg, ok, line = check(args, f, a.apks)
        line += "" if not ok or pkg not in srcs else " (another copy used)"
        checks.append(line)
        note("verify " + line)
        if ok and pkg not in srcs:
            srcs[pkg] = take(f, pkg, a.apks)
    br.write("verify.txt", "\n".join(checks) + "\n")
    installed = {}
    results = {}
    order = [p for p in GOOGLE if p in srcs] + [p for p in srcs if p not in GOOGLE]
    for pkg in order:
        os.makedirs(os.path.join(args.out, pkg), exist_ok=True)
        r, apk = harness.install(args, srcs[pkg], results)
        installed[pkg] = apk
        note(f"installed {pkg}: {r}")
        copy_results(os.path.join(args.out, pkg), os.path.join(br.dir, "install", pkg))
    harness.write_registry(args)
    note("ready: push cmd.txt (one command per line, see README.md)")
    br.push("installed")

    deadline = time.time() + a.hours * 3600
    done, seq = None, 0
    while time.time() < deadline:
        time.sleep(20)
        br.pull()
        p = os.path.join(br.dir, "cmd.txt")
        if not os.path.exists(p):
            continue
        body = open(p, "rb").read()
        h = hashlib.sha1(body).hexdigest()
        if h == done:
            continue
        done = h
        stop = False
        for line in body.decode("utf-8", "replace").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            seq += 1
            words = line.split()
            # A fetch's link stays in cmd.txt only: not in the folder name, the status or the commits.
            shown = f"fetch {words[2] if len(words) > 2 else '(link)'}" if words[0] == "fetch" else line
            slug = re.sub(r"[^A-Za-z0-9._-]+", "_", shown)[:60]
            res = os.path.join(br.dir, "results", f"{seq:03d}-{slug}")
            os.makedirs(res, exist_ok=True)
            note(f"#{seq} {shown}")
            br.push(f"running #{seq} {shown}")
            log = []
            try:
                cmd = words[0]
                if cmd == "run" and len(words) >= 2 and installed.get(words[1]):
                    pkg = words[1]
                    opts = [w for w in words[2:] if "=" in w]
                    secs = [w for w in words[2:] if "=" not in w]
                    args.minutes = (float(secs[0]) if secs else 180) / 60
                    args.dump_at = sorted(float(x) for w in opts if w.startswith("dump=") for x in w[5:].split(",") if x)
                    args.actions = sorted((float(w.split("=", 1)[1]), w.split("=", 1)[0]) for w in opts
                                          if w.split("=", 1)[0] in ("pan", "resize"))
                    run_dir = os.path.join(args.out, pkg)
                    shutil.rmtree(run_dir, ignore_errors=True)
                    os.makedirs(run_dir, exist_ok=True)
                    r = {}
                    harness.drive(args, pkg, installed[pkg], "", r)
                    st = r.get("run", {})
                    log.append(f"exit={st.get('exit')} window={st.get('window')} drew={st.get('drew')} "
                               f"first_draw={st.get('first_draw_s')} login={st.get('login')!r} "
                               f"login_offer={st.get('login_offer')!r} error_dialog={st.get('error_dialog')!r} "
                               f"abi={st.get('abi')} peak_ws_mb={st.get('peak_ws_mb')}")
                    log.append(f"MEASURE first_frame={st.get('first_frame')} at_60s={st.get('mem_60')} "
                               f"peak_ws_process_mb={st.get('peak_ws_process_mb')} cpu={st.get('cpu')}")
                    log += harness.gl_lines(os.path.join(run_dir, f"run-{pkg}.log"))
                    log += st.get("events", [])
                    log.append("first error lines:")
                    log += harness.first_errors(os.path.join(run_dir, f"run-{pkg}.log"), 80)
                    copy_results(run_dir, res)
                    copy_broker_logs(args, res)
                elif cmd == "env" and len(words) == 2 and "=" in words[1]:
                    k, v = words[1].split("=", 1)
                    if v:
                        args.extra_env[k] = v
                    else:
                        args.extra_env.pop(k, None)
                    log.append(f"run env now: {sorted(args.extra_env.items())}")
                elif cmd == "install" and len(words) >= 2 and words[1] in srcs:
                    r, apk = harness.install(args, srcs[words[1]], {})
                    installed[words[1]] = apk
                    harness.write_registry(args)
                    log.append(str(r))
                    copy_file(os.path.join(args.out, words[1], "install.log"), os.path.join(res, "install.log"))
                elif cmd == "fetch" and len(words) >= 2:
                    path, pkg, flog = fetch(args, words[1], words[2] if len(words) > 2 else "", seq, a.apks)
                    log += flog
                    if path:
                        srcs[pkg] = path
                        os.makedirs(os.path.join(args.out, pkg), exist_ok=True)
                        r, apk = harness.install(args, path, {})
                        installed[pkg] = apk
                        harness.write_registry(args)
                        log.append(f"installed {pkg}: {r}")
                        copy_file(os.path.join(args.out, pkg, "install.log"), os.path.join(res, "install.log"))
                elif cmd == "clear" and len(words) >= 2:
                    harness.kill_all()
                    for d in (os.path.join(args.data, "android", "data", "data", words[1]),):
                        shutil.rmtree(d, ignore_errors=True)
                        log.append(f"removed {d}")
                elif cmd == "update-java" and len(words) >= 2:
                    harness.kill_all()
                    log.append("ok" if update_java(args, words[1], log) else "FAILED")
                elif cmd == "update-exe" and len(words) >= 2 and words[1].isdigit():
                    log.append("ok" if update_exe(args, words[1], log) else "FAILED")
                elif cmd == "logs":
                    copy_results(os.path.join(args.data, "logs"), res)
                elif cmd == "ps":
                    # Without the job's tokens: what it prints is pushed to a public branch.
                    code, out = sh(["pwsh", "-NoProfile", "-Command", line[3:]], timeout=600, env=harness.clean_env())
                    log.append(f"exit {code}\n{scrub(out[-60000:])[0]}")
                elif cmd == "stop":
                    stop = True
                else:
                    log.append("unknown or incomplete command (see README.md)")
            except Exception as e:
                import traceback
                log.append("harness error: " + traceback.format_exc())
            br.write(os.path.join(os.path.relpath(res, br.dir), "result.txt"), scrub("\n".join(log))[0] + "\n")
            note(f"#{seq} done: {log[0][:200] if log else ''}")
            if stop:
                break
        br.write("done.txt", h + "\n")
        br.push(f"done {h[:8]}")
        if stop:
            break
    note("session over")
    br.push("session over")


if __name__ == "__main__":
    main()
