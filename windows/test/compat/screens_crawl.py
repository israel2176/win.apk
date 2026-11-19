#!/usr/bin/env python3
"""Screens crawl: start every activity an APK declares, one run each, and report
what breaks on which screen.

    screens_crawl.py --runtime <staged runtime dir> --out <report dir>
                     (--apk <file>... | --only pkg,... [--apps apps.txt] [--apks <cache dir>])
                     [--launcher wine] [--jdk <dir>] [--frames 30] [--timeout 120]
                     [--first-timeout 400] [--max N] [--activities substr,...]
                     [--app-only] [--intents intents.json] [--scale 4]
    screens_crawl.py --reanalyze --out <report dir>

The sweep (sweep.py) boots an app on its launcher activity, so everything past
a login, a first-run wizard or a menu is never reached. Here each activity of
the manifest -- exported or not, the way `am start -n <pkg>/<class>` would
start it -- is the root of its own headless run: the launcher activity first,
which also installs the app, then every other one through
MACAPK_LAUNCH_ACTIVITY (Boot.java; the activity does not have to be exported,
as for a debuggable build's `am start`). One data directory per app is shared
by its runs, so the app is installed once and the state one screen leaves
(preferences, a database) is there for the next.

The activities come from the APK's own binary AndroidManifest.xml (parsed
here, so no SDK is needed). Activity aliases are left out: their target is
listed itself.

Each run records whether a first frame was drawn, the frames presented, which
activities really started (an activity may finish and send the run elsewhere,
to a login screen say), the activity that drew the last frame, every Java
stack trace in the log (FATAL EXCEPTION, a lifecycle callback that threw, or
one the app logged), the stubs reached ("not implemented on Windows yet":
host natives and the system functions an app's native code calls, with the
counts of the summary at exit when the runtime prints one) and the last frame
as a PNG, downscaled by --scale.

Stack traces are sorted by where they start: the first frame of the innermost
cause that is not the Java library (java.*, kotlin.*, ...). In android.*,
com.android.*, com.macapk.* or com.winapk.* it is Win.apk's (the framework
is ours); in the app's code it is the app's, and those are mostly a screen
opened without the extras or the state it expects -- expected, listed apart.
An app NullPointerException whose message says a framework method returned
null (not an Intent or Bundle getter) is ranked with Win.apk's: on a phone
that method would have answered.

--intents names a JSON file of per-activity starts, `{"<class>": {"action":
..., "data": ..., "extras": "k=v;k2=v2"}}`, for screens that need them
(MACAPK_LAUNCH_ACTION, _DATA, _EXTRAS).

Writes <out>/<package>/report.json, report.md, logs/<n>-<class>.log and
shots/<n>-<class>.png, and <out>/report.md + report.json across all apps.
"""

import argparse
import collections
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sweep  # noqa: E402  (fetch, win_path, read_apps, the log patterns)

# ---- The binary manifest ----------------------------------------------------

RES_NAME, RES_ENABLED, RES_EXPORTED, RES_TARGET = 0x01010003, 0x0101000E, 0x01010010, 0x01010202


def _pool(data, off):
    """The strings of a ResStringPool chunk at off."""
    header_size, = struct.unpack_from("<H", data, off + 2)
    count, _styles, flags, strings_start = struct.unpack_from("<IIII", data, off + 8)
    utf8 = flags & 0x100
    base = off + strings_start
    out = []
    for i in range(count):
        p = base + struct.unpack_from("<I", data, off + header_size + 4 * i)[0]
        if utf8:
            n = data[p]; p += 2 if n & 0x80 else 1           # length in characters
            n = data[p]
            if n & 0x80:
                n = ((n & 0x7F) << 8) | data[p + 1]; p += 2
            else:
                p += 1
            out.append(data[p:p + n].decode("utf-8", "replace"))
        else:
            n, = struct.unpack_from("<H", data, p); p += 2
            if n & 0x8000:
                n = ((n & 0x7FFF) << 16) | struct.unpack_from("<H", data, p)[0]; p += 2
            out.append(data[p:p + 2 * n].decode("utf-16-le", "replace"))
    return out


def parse_manifest(data):
    """(package, [activity dicts]) from a binary AndroidManifest.xml."""
    strings, resmap = [], []
    pkg = None
    stack = []  # element names
    activities, aliases = [], []
    cur = None
    off = 8
    while off + 8 <= len(data):
        ctype, chead, csize = struct.unpack_from("<HHI", data, off)
        if csize < 8:
            break
        if ctype == 0x0001:
            strings = _pool(data, off)
        elif ctype == 0x0180:
            resmap = list(struct.unpack_from(f"<{(csize - chead) // 4}I", data, off + chead))
        elif ctype == 0x0102:
            name_i, = struct.unpack_from("<I", data, off + 20)
            astart, asize, acount = struct.unpack_from("<HHH", data, off + 24)
            tag = strings[name_i] if name_i < len(strings) else ""
            attrs = {}
            for k in range(acount):
                a = off + 16 + astart + k * asize
                _ns, an, raw, _sz, _r, dtype, val = struct.unpack_from("<IIIHBBI", data, a)
                key = resmap[an] if an < len(resmap) and resmap[an] else (strings[an] if an < len(strings) else an)
                if raw != 0xFFFFFFFF and raw < len(strings):
                    attrs[key] = strings[raw]
                elif dtype == 0x12:
                    attrs[key] = val != 0
                elif dtype == 0x03 and val < len(strings):
                    attrs[key] = strings[val]
                else:
                    attrs[key] = None  # a resource reference: not known without the table
            # aapt writes the manifest's own attributes by name, the framework's by id.
            if tag == "manifest":
                pkg = attrs.get("package")
            elif tag == "activity" and stack and stack[-1] == "application":
                cur = {"class": attrs.get(RES_NAME) or attrs.get("name") or "",
                       "exported": attrs.get(RES_EXPORTED, attrs.get("exported")),
                       "enabled": attrs.get(RES_ENABLED, attrs.get("enabled")),
                       "actions": [], "categories": []}
                activities.append(cur)
            elif tag == "activity-alias" and stack and stack[-1] == "application":
                # Not run itself, but a launcher alias makes its target the launcher activity.
                cur = {"class": attrs.get(RES_TARGET) or attrs.get("targetActivity") or "",
                       "alias_enabled": attrs.get(RES_ENABLED, attrs.get("enabled")),
                       "actions": [], "categories": []}
                aliases.append(cur)
            elif tag in ("action", "category") and cur is not None and stack[-1] == "intent-filter":
                v = attrs.get(RES_NAME) or attrs.get("name")
                if v:
                    cur["actions" if tag == "action" else "categories"].append(v)
            stack.append(tag)
        elif ctype == 0x0103:
            if stack:
                if stack.pop() in ("activity", "activity-alias"):
                    cur = None
        off += csize
    for a in activities + aliases:
        c = a["class"]
        if c.startswith("."):
            a["class"] = pkg + c
        elif "." not in c:
            a["class"] = pkg + "." + c
    for a in activities:
        # An activity with an intent filter is exported unless it says otherwise (before API 31).
        if a["exported"] is None:
            a["exported"] = bool(a["actions"])
        a["launcher"] = ("android.intent.action.MAIN" in a["actions"]
                         and "android.intent.category.LAUNCHER" in a["categories"])
    for al in aliases:
        if (al["alias_enabled"] is not False and "android.intent.action.MAIN" in al["actions"]
                and "android.intent.category.LAUNCHER" in al["categories"]):
            for a in activities:
                if a["class"] == al["class"]:
                    a["launcher"] = True
    return pkg, activities


def apk_activities(apk):
    with zipfile.ZipFile(apk) as z:
        return parse_manifest(z.read("AndroidManifest.xml"))


# ---- Screenshots ------------------------------------------------------------

def bmp_to_png(bmp, png, scale):
    """A 32-bit BMP (the runtime's headless frame) as a PNG, every scale-th pixel."""
    try:
        with open(bmp, "rb") as f:
            data = f.read()
        offset, = struct.unpack_from("<I", data, 10)
        width, height = struct.unpack_from("<ii", data, 18)
        top_down = height < 0
        height = abs(height)
        ow, oh = max(1, width // scale), max(1, height // scale)
        rows = []
        for oy in range(oh):
            y = oy * scale if top_down else height - 1 - oy * scale
            row = bytearray(b"\0")
            base = offset + y * width * 4
            for ox in range(ow):
                i = base + ox * scale * 4
                row += bytes((data[i + 2], data[i + 1], data[i]))
            rows.append(bytes(row))

        def chunk(kind, body):
            return (struct.pack(">I", len(body)) + kind + body
                    + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))
        with open(png, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", ow, oh, 8, 2, 0, 0, 0))
                    + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))
        return True
    except Exception:
        return False


# ---- The log ----------------------------------------------------------------

PREFIX_RE = re.compile(r"^\[[VDIWEF]\] [^:]{1,80}: ")
HEAD_RE = re.compile(r"^(?:Caused by: )?([a-zA-Z_][\w$]*(?:\.[\w$]+)+)(?::\s*(.*))?$")
AT_RE = re.compile(r"^\s+at ([\w$.<>/-]+)\.([\w$<>-]+)\((.*)\)")
SUMMARY_RE = re.compile(r"^\[win\.apk\]\s+(\d+)x (native method|system function) (\S+)")
STARTED_RE = re.compile(r"ActivityHost: starting activity (\S+)")
OWNER_RE = re.compile(r"macapkDrawFrame fire #\d+ .*owner=(\S+)")
# An app's native code logging an error (android/log.h): a file or a system
# interface it expected and did not find often shows here first.
NATIVE_ERR_RE = re.compile(r"^E/([^:]{1,60}): (.{1,200})")
TASK_EMPTY = "task is empty (last activity finished)"

# Frames that are only the Java library: the first frame past them is where the
# trouble starts.
NEUTRAL = ("java.", "javax.", "jdk.", "sun.", "kotlin.", "kotlinx.coroutines.", "com.sun.")
FRAMEWORK = ("android.", "com.android.", "com.macapk.", "com.winapk.", "dalvik.", "libcore.",
             "org.apache.harmony.", "com.google.android.collect.")
# Getters whose null is an activity opened without what its caller passes.
STATE_GETTERS = re.compile(r"android\.content\.Intent\.get|android\.os\.(Bundle|BaseBundle)\.get|"
                           r"getArguments|getIntent|getExtras|getParcelable|getSerializable")
HELPFUL_NULL_RE = re.compile(r'because the return value of "([\w$.]+)\(')
STATE_EXC = ("NullPointerException", "IllegalArgumentException", "IllegalStateException",
             "ClassCastException", "UninitializedPropertyAccessException", "NoSuchElementException",
             "IndexOutOfBoundsException", "NumberFormatException")
STATE_WORDS = re.compile(r"intent|extra|argument|bundle|must be (provided|set|passed)|missing|"
                         r"required|not (initiali[sz]ed|set)|lateinit|null", re.I)


# Framework exceptions that report the caller's own bad input, the same on a
# phone: the trouble starts at the first frame past the framework.
CALLER_INPUT = [
    (re.compile(r"android\.database\.sqlite\.SQLite\w*Exception"),
     re.compile(r"prepare failed|syntax error|duplicate column|no such (table|column)|already exists|"
                r"constraint failed|UNIQUE|NOT NULL", re.I)),
    (re.compile(r"android\.content\.ActivityNotFoundException"), re.compile(r"")),
]


def is_exception_name(name):
    last = name.rsplit(".", 1)[-1]
    return last.endswith(("Exception", "Error", "Throwable")) or "Exception$" in last or "Error$" in last


def parse_traces(lines):
    """Every Java stack trace in the log: [{head, message, blocks, fatal, thread, lifecycle}]."""
    traces = []
    i, n = 0, len(lines)
    while i < n:
        text = PREFIX_RE.sub("", lines[i].rstrip("\n"))
        m = HEAD_RE.match(text.strip())
        if m and is_exception_name(m.group(1)) and i + 1 < n and AT_RE.match(lines[i + 1]):
            ctx = " ".join(lines[max(0, i - 3):i])
            fatal = "FATAL EXCEPTION" in ctx
            thread = re.search(r"FATAL EXCEPTION: (.*?)(?:\n|Process:|$)", ctx)
            blocks = [{"exception": m.group(1), "message": (m.group(2) or "").strip()[:300], "frames": []}]
            j = i + 1
            suppressed = False  # a Suppressed: block's frames are not the trace's
            while j < n:
                t = lines[j].rstrip("\n")
                fm = AT_RE.match(t)
                if fm:
                    if not suppressed:
                        blocks[-1]["frames"].append(f"{fm.group(1)}.{fm.group(2)}")
                elif t.strip().startswith("Suppressed: "):
                    suppressed = True
                elif t.strip().startswith("Caused by: ") and not suppressed:
                    cm = HEAD_RE.match(t.strip())
                    blocks.append({"exception": cm.group(1) if cm else t.strip()[11:],
                                   "message": ((cm.group(2) if cm else "") or "").strip()[:300], "frames": []})
                elif t.strip().startswith(("... ", "Caused by: ")):
                    pass
                else:
                    break
                j += 1
            traces.append({"head": blocks[0]["exception"], "message": blocks[0]["message"], "blocks": blocks,
                           "fatal": fatal, "thread": thread.group(1).strip() if thread else None,
                           "lifecycle": " threw " in lines[i - 1] if i else False})
            i = j
            continue
        i += 1
    return traces


def classify(trace, pkg):
    """('framework' | 'framework-null' | 'app-state' | 'app', origin frame, root block)."""
    root = trace["blocks"][-1]
    frames = root["frames"] or trace["blocks"][0]["frames"]
    origin = next((f for f in frames if not f.startswith(NEUTRAL)), frames[0] if frames else "?")
    if origin.startswith(FRAMEWORK):
        caller = next((f for f in frames if not f.startswith(NEUTRAL + FRAMEWORK)), None)
        if caller and any(e.fullmatch(root["exception"]) and m.search(root["message"]) for e, m in CALLER_INPUT):
            return "app", caller, root
        return "framework", origin, root
    exc = root["exception"].rsplit(".", 1)[-1]
    msg = root["message"]
    nulled = HELPFUL_NULL_RE.search(msg)
    if nulled and nulled.group(1).startswith(FRAMEWORK) and not STATE_GETTERS.search(nulled.group(1)):
        return "framework-null", origin, root
    near_state = any(STATE_GETTERS.search(f) for f in frames[:8])
    if exc in STATE_EXC or near_state or STATE_WORDS.search(msg):
        return "app-state", origin, root
    return "app", origin, root


def analyze(text, code, timed_out, task_empty, seconds, pkg, activity):
    lines = text.splitlines()
    frames = max([int(x) for x in sweep.FRAMES_RE.findall(text)] or [0])
    first_frame = "first frame presented" in text
    started = STARTED_RE.findall(text)
    owners = OWNER_RE.findall(text)
    stubs = collections.Counter()
    for name in sweep.STUB_RE.findall(text):
        stubs[name] += 0
    for name in sweep.GUEST_MISSING_RE.findall(text):
        stubs[name] += 0
    for line in lines:
        m = SUMMARY_RE.match(line.strip())
        if m:
            stubs[m.group(3)] = max(stubs[m.group(3)], int(m.group(1)))
    traces = parse_traces(lines)
    for t in traces:
        t["class"], t["origin"], _ = classify(t, pkg)
    if code == 0 and frames > 0:
        status = "drew"
    elif task_empty:
        status = "finished"  # the activity finished, and nothing took its place
    elif timed_out:
        status = "no-frame" if not first_frame else "stalled"
    else:
        status = "crashed"
    return {
        "activity": activity,
        "status": status,
        "exit_code": code,
        "seconds": seconds,
        "first_frame": first_frame,
        "frames": frames,
        "started": started,
        "last_frame_owner": owners[-1] if owners else None,
        "jvm_crash": "A fatal error has been detected by the Java Runtime Environment" in text,
        "stubs": dict(stubs),
        "native_errors": sorted(set(f"{m.group(1)}: {m.group(2)}" for m in map(NATIVE_ERR_RE.match, lines) if m)),
        "traces": traces,
    }


# ---- One run ----------------------------------------------------------------

def run_activity(apk, pkg, activity, intent, args, app_out, index, timeout):
    short = activity.rsplit(".", 1)[-1] if activity else "launcher"
    stem = f"{index:02d}-{short}"
    log_path = os.path.join(app_out, "logs", stem + ".log")
    bmp = os.path.join(app_out, "shots", stem + ".bmp")
    png = os.path.join(app_out, "shots", stem + ".png")
    for p in (bmp, png):
        if os.path.exists(p):
            os.remove(p)
    exe = os.path.join(args.runtime, "bin", "winapk-runtime.exe")
    cmd = ([args.launcher] if args.launcher else []) + [
        exe, "--apk", sweep.win_path(apk, args.launcher),
        "--data", sweep.win_path(os.path.join(app_out, "data"), args.launcher)]
    if args.jdk:
        cmd += ["--jdk", sweep.win_path(args.jdk, args.launcher)]
    env = dict(os.environ)
    env["WINAPK_HEADLESS"] = "1"
    env["WINAPK_HEADLESS_EXIT_AFTER_FRAMES"] = str(args.frames)
    env["WINAPK_HEADLESS_FRAME"] = sweep.win_path(bmp, args.launcher)
    if args.launcher:
        env["WINAPK_SHORT_DATA_DIR"] = "0"
        env.setdefault("WINEDLLOVERRIDES", "iphlpapi=n,b")
    env.setdefault("WINEDEBUG", "-all")
    for k in ("MACAPK_LAUNCH_ACTIVITY", "MACAPK_LAUNCH_ACTION", "MACAPK_LAUNCH_DATA", "MACAPK_LAUNCH_EXTRAS"):
        env.pop(k, None)
    if activity:
        env["MACAPK_LAUNCH_ACTIVITY"] = activity
        for key, var in (("action", "MACAPK_LAUNCH_ACTION"), ("data", "MACAPK_LAUNCH_DATA"),
                         ("extras", "MACAPK_LAUNCH_EXTRAS")):
            if intent.get(key):
                env[var] = intent[key]
    start = time.time()
    # Set by the reader: the moment the run has nothing more to show (the task
    # emptied, or the main thread died), so it need not wait out the timeout.
    done_at = [None]
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        flags = {"task_empty": False}

        def copy():
            pending_fatal = False
            try:
                for raw in proc.stdout:
                    line = raw.decode("utf-8", "replace")
                    log.write(line)
                    log.flush()
                    if TASK_EMPTY in line:
                        flags["task_empty"] = True
                        done_at[0] = time.time() + 3
                    if "FATAL EXCEPTION: main" in line:
                        pending_fatal = True
                    if pending_fatal and done_at[0] is None:
                        done_at[0] = time.time() + 8  # long enough for the trace and a last frame
            except (OSError, ValueError):
                pass

        reader = threading.Thread(target=copy, daemon=True)
        reader.start()
        code, timed_out = None, False
        while True:
            try:
                code = proc.wait(timeout=0.5)
                break
            except subprocess.TimeoutExpired:
                now = time.time()
                if now - start > timeout or (done_at[0] is not None and now > done_at[0]):
                    timed_out = done_at[0] is None
                    proc.kill()
                    proc.wait()
                    break
        reader.join(timeout=10)
    if args.launcher and os.path.basename(args.launcher).startswith("wine"):
        subprocess.run(["wineserver", "-k"], capture_output=True)
    seconds = round(time.time() - start, 1)
    text = open(log_path, encoding="utf-8", errors="replace").read()
    r = analyze(text, code, timed_out, flags["task_empty"], seconds, pkg, activity)
    if os.path.exists(bmp):
        if bmp_to_png(bmp, png, args.scale):
            r["shot"] = os.path.relpath(png, app_out)
        if r["status"] == "drew" and sweep.bmp_colors(bmp) <= 2:
            r["status"] = "blank"
        os.remove(bmp)
    r["log"] = os.path.relpath(log_path, app_out)
    return r


# ---- Reports ----------------------------------------------------------------

def group_traces(runs):
    """{class: {key: {...}}}: one entry per (exception, origin frame), with the screens it showed on."""
    groups = collections.defaultdict(dict)
    for r in runs:
        target = r["activity"] or r.get("launcher_class") or "launcher"
        for t in r["traces"]:
            root = t["blocks"][-1]
            key = f"{root['exception']} @ {t['origin']}"
            g = groups[t["class"]].setdefault(key, {
                "exception": root["exception"], "origin": t["origin"], "message": root["message"],
                "head": t["head"], "frames": (root["frames"] or t["blocks"][0]["frames"])[:12],
                "fatal": False, "lifecycle": False, "activities": []})
            g["fatal"] |= t["fatal"]
            g["lifecycle"] |= t["lifecycle"]
            if target not in g["activities"]:
                g["activities"].append(target)
    return groups


def short(cls):
    return cls.rsplit(".", 1)[-1] if cls else "launcher"


def app_report(pkg, runs, app_out):
    groups = group_traces(runs)
    stubs = collections.defaultdict(lambda: {"calls": 0, "activities": []})
    for r in runs:
        for name, calls in r["stubs"].items():
            s = stubs[name]
            s["calls"] += calls
            s["activities"].append(r["activity"] or r.get("launcher_class") or "launcher")
    report = {"package": pkg, "runs": runs,
              "exceptions": {k: sorted(v.values(), key=lambda g: (-len(g["activities"]), not g["fatal"]))
                             for k, v in groups.items()},
              "stubs": dict(sorted(stubs.items(), key=lambda kv: (-len(kv[1]["activities"]), -kv[1]["calls"])))}
    with open(os.path.join(app_out, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    lines = [f"# Screens crawl: {pkg}", ""]
    by = collections.Counter(r["status"] for r in runs)
    lines.append(f"{len(runs)} activities: " + ", ".join(f"{n} {s}" for s, n in by.most_common()))
    lines += ["", "| # | Activity | Exported | Status | Frames | Drew last | Started | Traces | Shot |",
              "|---|---|---|---|---|---|---|---|---|"]
    for i, r in enumerate(runs):
        started = " > ".join(short(s) for s in r["started"]) or "-"
        if r.get("retried_with"):
            started += f" (finished on the bare start; run again with {r['retried_with']})"
        shot = f"[png]({r['shot']})" if r.get("shot") else "-"
        lines.append(f"| {i} | `{short(r['activity'] or r.get('launcher_class'))}` | "
                     f"{'yes' if r.get('exported') else 'no'} | {r['status']}{' (JVM crash)' if r['jvm_crash'] else ''} | "
                     f"{r['frames']} | {r['last_frame_owner'] or '-'} | {started} | {len(r['traces'])} | {shot} |")
    lines += exception_sections(report["exceptions"])
    lines += ["", "## Not implemented on Windows yet, by screens that reached it", "",
              "| Screens | Calls | Stub |", "|---|---|---|"]
    for name, s in report["stubs"].items():
        lines.append(f"| {len(s['activities'])} | {s['calls'] or '?'} | `{name}` |")
    errs = collections.defaultdict(list)
    for r in runs:
        for e in r.get("native_errors", []):
            errs[e].append(short(r["activity"] or r.get("launcher_class")))
    if errs:
        lines += ["", "## Errors the app's native code logged", ""]
        lines += [f"- `{e}` ({', '.join(a)})" for e, a in errs.items()]
    with open(os.path.join(app_out, "report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return report


TITLES = [
    ("framework", "Framework exceptions (Win.apk bugs: they start in android.* / com.macapk.*)"),
    ("framework-null", "App exceptions on a null a framework method returned (likely Win.apk)"),
    ("app-state", "App exceptions on missing intent extras or state (expected for a screen opened alone)"),
    ("app", "Other app exceptions"),
]


def exception_sections(exceptions, with_pkg=False):
    lines = []
    for cls, title in TITLES:
        items = exceptions.get(cls) or []
        lines += ["", f"## {title}", ""]
        if not items:
            lines.append("None.")
            continue
        for g in items:
            where = ", ".join(f"`{short(a)}`" for a in g["activities"])
            tags = [t for t, on in (("FATAL", g["fatal"]), ("lifecycle callback", g["lifecycle"])) if on]
            pkg = f"{g['package']}: " if with_pkg else ""
            lines.append(f"- {pkg}**`{g['exception']}`** at `{g['origin']}`"
                         f"{' (' + ', '.join(tags) + ')' if tags else ''} on {len(g['activities'])} screen(s): {where}")
            if g["message"]:
                lines.append(f"  - {g['message'][:240]}")
            if cls.startswith("framework"):
                lines.append("    ```")
                lines += [f"    at {f}" for f in g["frames"][:8]]
                lines.append("    ```")
    return lines


def overall_report(reports, out):
    merged = collections.defaultdict(list)
    for rep in reports:
        for cls, items in rep["exceptions"].items():
            for g in items:
                merged[cls].append(dict(g, package=rep["package"]))
    stubs = collections.defaultdict(lambda: {"apps": [], "screens": 0, "calls": 0})
    for rep in reports:
        for name, s in rep["stubs"].items():
            stubs[name]["apps"].append(rep["package"])
            stubs[name]["screens"] += len(s["activities"])
            stubs[name]["calls"] += s["calls"]
    with open(os.path.join(out, "report.json"), "w", encoding="utf-8") as f:
        json.dump({"apps": [{"package": r["package"],
                             "statuses": dict(collections.Counter(x["status"] for x in r["runs"]))}
                            for r in reports],
                   "exceptions": merged, "stubs": stubs}, f, indent=2)
    lines = ["# Screens crawl", "", "| App | Activities | Statuses | Framework exceptions | Report |",
             "|---|---|---|---|---|"]
    for r in reports:
        st = collections.Counter(x["status"] for x in r["runs"])
        fw = len(r["exceptions"].get("framework", [])) + len(r["exceptions"].get("framework-null", []))
        lines.append(f"| {r['package']} | {len(r['runs'])} | "
                     + ", ".join(f"{n} {s}" for s, n in st.most_common())
                     + f" | {fw} | [report]({r['package']}/report.md) |")
    for cls in merged:
        merged[cls].sort(key=lambda g: (-len(g["activities"]), not g["fatal"]))
    lines += exception_sections(merged, with_pkg=True)
    lines += ["", "## Not implemented on Windows yet", "", "| Apps | Screens | Calls | Stub |", "|---|---|---|---|"]
    for name, s in sorted(stubs.items(), key=lambda kv: (-len(kv[1]["apps"]), -kv[1]["screens"])):
        lines.append(f"| {len(s['apps'])} | {s['screens']} | {s['calls'] or '?'} | `{name}` |")
    text = "\n".join(lines) + "\n"
    with open(os.path.join(out, "report.md"), "w", encoding="utf-8") as f:
        f.write(text)
    return text


# ---- Main -------------------------------------------------------------------

def crawl(apk, args, out, intents):
    pkg, acts = apk_activities(apk)
    app_out = os.path.join(out, pkg)
    for sub in ("logs", "shots", "data"):
        os.makedirs(os.path.join(app_out, sub), exist_ok=True)
    launcher = next((a for a in acts if a["launcher"]), None)
    rest = [a for a in acts if a is not launcher]
    if args.app_only:
        # Library screens (androidx, Google's, a crash reporter's) are not the app's.
        prefix = pkg.rsplit(".", 1)[0] + "."
        rest = [a for a in rest if a["class"].startswith(prefix)]
    if args.activities:
        wanted = [w for w in args.activities.split(",") if w]
        rest = [a for a in rest if any(w in a["class"] for w in wanted)]
    rest = rest[:args.max] if args.max else rest
    print(f"[crawl] {pkg}: {len(acts)} activities in the manifest, {1 + len(rest)} runs", flush=True)
    runs = []
    # The first run installs the app: through its launcher activity as the user
    # would, or, with none, through the first activity it declares.
    plan = [(None if launcher else (rest[0]["class"] if rest else None), launcher or (rest.pop(0) if rest else None))]
    plan += [(a["class"], a) for a in rest]
    for i, (cls, act) in enumerate(plan):
        if act is None:
            break
        r = run_activity(apk, pkg, cls, intents.get(act["class"], {}), args, app_out, i,
                         args.first_timeout if i == 0 else args.timeout)
        r["launcher_class"] = act["class"]
        r["exported"] = bool(act["exported"])
        r["actions"] = act["actions"]
        if cls is None:
            r["activity"] = None
        if r["first_frame"] is False and i == 0 and r["status"] == "no-frame":
            print(f"[crawl] {pkg}: the first run drew nothing (still installing?), trying it once more", flush=True)
            r2 = run_activity(apk, pkg, cls, intents.get(act["class"], {}), args, app_out, i, args.first_timeout)
            r2.update(launcher_class=act["class"], exported=bool(act["exported"]), actions=act["actions"])
            r = r2
        actions = [x for x in act["actions"] if x != "android.intent.action.MAIN"]
        if r["status"] == "finished" and cls and actions and act["class"] not in intents:
            # It finished at once on the bare start (many check the action they
            # came with): once more with the first action its intent filter takes.
            r2 = run_activity(apk, pkg, cls, {"action": actions[0]}, args, app_out, i, args.timeout)
            r2.update(launcher_class=act["class"], exported=bool(act["exported"]), actions=act["actions"],
                      retried_with=actions[0], bare_status=r["status"])
            r = r2
        fw = sum(1 for t in r["traces"] if t["class"].startswith("framework"))
        print(f"[crawl] {pkg} [{i}/{len(plan) - 1}] {short(act['class'])}: {r['status']} in {r['seconds']} s, "
              f"{r['frames']} frames, {len(r['traces'])} traces ({fw} framework), {len(r['stubs'])} stubs, "
              f"last frame {r['last_frame_owner']}", flush=True)
        runs.append(r)
    return app_report(pkg, runs, app_out)


def reanalyze(out):
    """The reports again from the logs a crawl kept (after a change to the parsing here)."""
    reports = []
    for pkg in sorted(os.listdir(out)):
        path = os.path.join(out, pkg, "report.json")
        if not os.path.isfile(path):
            continue
        old = json.load(open(path, encoding="utf-8"))
        for r in old["runs"]:
            lines = open(os.path.join(out, pkg, r["log"]), encoding="utf-8", errors="replace").read().splitlines()
            r["traces"] = parse_traces(lines)
            r["native_errors"] = sorted(set(f"{m.group(1)}: {m.group(2)}"
                                            for m in map(NATIVE_ERR_RE.match, lines) if m))
            for t in r["traces"]:
                t["class"], t["origin"], _ = classify(t, pkg)
        reports.append(app_report(pkg, old["runs"], os.path.join(out, pkg)))
    print(overall_report(reports, out))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--runtime")
    ap.add_argument("--reanalyze", action="store_true", help="rebuild the reports in --out from its logs")
    ap.add_argument("--out", required=True)
    ap.add_argument("--apk", nargs="*", default=[], help="APK files to crawl")
    ap.add_argument("--only", default="", help="packages of --apps to fetch and crawl")
    ap.add_argument("--apps", default=os.path.join(HERE, "apps.txt"))
    ap.add_argument("--apks", default=None, help="APK cache for --only (default: <out>/apks)")
    ap.add_argument("--jdk")
    ap.add_argument("--launcher", default=None, help="command prefix, e.g. wine")
    ap.add_argument("--frames", type=int, default=30, help="WINAPK_HEADLESS_EXIT_AFTER_FRAMES per screen")
    ap.add_argument("--timeout", type=int, default=120, help="seconds per screen")
    ap.add_argument("--first-timeout", type=int, default=400, help="seconds for the first run, which installs")
    ap.add_argument("--max", type=int, default=0, help="at most this many activities past the first")
    ap.add_argument("--activities", default="", help="only activities whose class contains one of these")
    ap.add_argument("--app-only", action="store_true", help="skip activities outside the app's package")
    ap.add_argument("--intents", help="JSON: {class: {action, data, extras}} for screens that need them")
    ap.add_argument("--scale", type=int, default=4, help="screenshot downscale factor")
    ap.add_argument("--keep-data", action="store_true", help="keep each app's data directory afterwards")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    if args.reanalyze:
        return reanalyze(out)
    if not args.runtime:
        ap.error("--runtime is required unless --reanalyze is given")
    os.makedirs(out, exist_ok=True)
    intents = json.load(open(args.intents, encoding="utf-8")) if args.intents else {}
    apks = [os.path.abspath(a) for a in args.apk]
    only = set(filter(None, args.only.split(",")))
    if only:
        cache = args.apks or os.path.join(out, "apks")
        for pkg, version, name in sweep.read_apps(args.apps, only):
            apk = sweep.fetch(pkg, version, name, cache)
            if apk:
                apks.append(apk)
            else:
                print(f"[crawl] {pkg}: download failed", flush=True)
    if not apks:
        ap.error("no APK: give --apk or --only")
    reports = []
    for apk in apks:
        rep = crawl(apk, args, out, intents)
        reports.append(rep)
        if not args.keep_data:
            shutil.rmtree(os.path.join(out, rep["package"], "data"), ignore_errors=True)
        overall_report(reports, out)  # after each app, so a long crawl has a report as it goes
    print(overall_report(reports, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
