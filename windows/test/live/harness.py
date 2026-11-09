"""Win.apk app test on a Windows runner: installs each app of --apks the way
the launcher does, opens it in a window, follows its first screens and writes
what happened.

    harness.py --runtime <stage> --apks <dir> --data <dir> --out <dir> [--minutes N] [--only pkg,pkg]

Per app: install log, run log (Win.apk's output with seconds since start), the
window every ~20 s and at each new screen (PNG), Win.apk's view dumps
(WINAPK_DUMP_VIEWS), the dialogs Win.apk itself asked (permissions), and a
summary.md with the first errors of each log. Onboarding buttons (Skip, Next,
Continue, Accept...) are pressed; nothing is typed, and a screen asking for a
login stops the app's run: that is as far as the test goes. Nothing secret is
used here.
"""

import argparse
import datetime
import json
import os
import re
import shutil
import struct
import subprocess
import threading
import time
import zlib

EXTS = (".apk", ".apks", ".xapk", ".apkm")


def now():
    return datetime.datetime.now().strftime("%H:%M:%S")


def say(msg):
    print(f"[{now()}] {msg}".encode("ascii", "backslashreplace").decode("ascii"), flush=True)


if os.name == "nt":
    import ctypes
    from ctypes import wintypes
    U32 = ctypes.WinDLL("user32", use_last_error=True)
    G32 = ctypes.WinDLL("gdi32")
    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    H = wintypes.HANDLE
    U32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    U32.GetDC.argtypes = [wintypes.HWND]
    U32.GetDC.restype = H
    U32.ReleaseDC.argtypes = [wintypes.HWND, H]
    U32.PrintWindow.argtypes = [wintypes.HWND, H, wintypes.UINT]
    U32.IsWindowVisible.argtypes = [wintypes.HWND]
    G32.CreateCompatibleDC.argtypes = [H]
    G32.CreateCompatibleDC.restype = H
    G32.CreateCompatibleBitmap.argtypes = [H, ctypes.c_int, ctypes.c_int]
    G32.CreateCompatibleBitmap.restype = H
    G32.SelectObject.argtypes = [H, H]
    G32.SelectObject.restype = H
    G32.DeleteObject.argtypes = [H]
    G32.DeleteDC.argtypes = [H]
    G32.GetDIBits.argtypes = [H, H, wintypes.UINT, wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT]


def top_windows():
    """(hwnd, pid, class, title) of every visible top-level window."""
    out = []

    def cb(h, _):
        if not U32.IsWindowVisible(h):
            return True
        pid = wintypes.DWORD()
        U32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        cls = ctypes.create_unicode_buffer(256)
        U32.GetClassNameW(h, cls, 256)
        title = ctypes.create_unicode_buffer(256)
        U32.GetWindowTextW(h, title, 256)
        out.append((h, pid.value, cls.value, title.value))
        return True
    U32.EnumWindows(WNDENUMPROC(cb), 0)
    return out


def click(hwnd, x, y):
    lp = (int(y) << 16) | (int(x) & 0xFFFF)
    U32.PostMessageW(hwnd, 0x0201, 1, lp)   # WM_LBUTTONDOWN
    time.sleep(0.08)
    U32.PostMessageW(hwnd, 0x0202, 0, lp)   # WM_LBUTTONUP


def capture_window(hwnd):
    """(w, h, BGRA rows top-down) of the window's client area, or None."""
    # PrintWindow waits for the window's thread: a hung app would hang the test.
    res = ctypes.c_size_t()
    if not U32.SendMessageTimeoutW(hwnd, 0, 0, 0, 0x0002, 3000, ctypes.byref(res)):  # WM_NULL, SMTO_ABORTIFHUNG
        return None
    rc = wintypes.RECT()
    U32.GetClientRect(hwnd, ctypes.byref(rc))
    w, h = rc.right, rc.bottom
    if w <= 0 or h <= 0:
        return None
    hdc = U32.GetDC(hwnd)
    mem = G32.CreateCompatibleDC(hdc)
    bmp = G32.CreateCompatibleBitmap(hdc, w, h)
    old = G32.SelectObject(mem, bmp)
    U32.PrintWindow(hwnd, mem, 3)   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    hdr = struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    info = ctypes.create_string_buffer(hdr + b"\0" * 16)
    G32.GetDIBits(mem, bmp, 0, h, buf, info, 0)
    G32.SelectObject(mem, old)
    G32.DeleteObject(bmp)
    G32.DeleteDC(mem)
    U32.ReleaseDC(hwnd, hdc)
    data = buf.raw
    return w, h, [data[r * w * 4:(r + 1) * w * 4] for r in range(h)]


def write_png(bmp, path):
    w, h, rows = bmp
    raw = bytearray()
    for r in rows:
        raw.append(0)
        px = bytearray(w * 3)
        px[0::3] = r[2::4][:w]
        px[1::3] = r[1::4][:w]
        px[2::3] = r[0::4][:w]
        raw += px

    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                 + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def blank(bmp):
    """True when the picture is one colour (nothing drawn yet)."""
    w, h, rows = bmp
    seen = set()
    for y in range(0, h, max(1, h // 40)):
        r = rows[y]
        for x in range(0, w, max(1, w // 40)):
            seen.add(bytes(r[x * 4:x * 4 + 3]))
            if len(seen) > 3:
                return False
    return True


VD_HEAD = re.compile(r"\[viewdump\] begin seq=(\d+) activity=(\S+) window=(\S+) frame=(\d+)x(\d+)")
VD_VIEW = re.compile(r'\[viewdump\] (\S+) id=(\S+) text="(.*?)" desc="(.*?)" click=(\d) enabled=(\d) '
                     r'bounds=(-?\d+),(-?\d+),(-?\d+),(-?\d+)\s*$')


def latest_viewdump(log_path):
    try:
        with open(log_path, "rb") as fh:
            fh.seek(0, 2)
            n = fh.tell()
            fh.seek(max(0, n - (4 << 20)))
            text = fh.read().decode("utf-8", "replace")
    except OSError:
        return None
    lines = text.split("\n")
    end = None
    for i in range(len(lines) - 1, -1, -1):
        if "[viewdump] end seq=" in lines[i]:
            end = i
            break
    if end is None:
        return None
    for i in range(end, -1, -1):
        m = VD_HEAD.search(lines[i])
        if m:
            views = []
            for l in lines[i + 1:end]:
                v = VD_VIEW.search(l)
                if v:
                    views.append((v.group(1), v.group(2), v.group(3), v.group(4), v.group(5) == "1",
                                  v.group(6) == "1", tuple(int(v.group(k)) for k in range(7, 11))))
            raw = [l[l.index("[viewdump]"):] for l in lines[i:end + 1] if "[viewdump]" in l]
            return {"seq": int(m.group(1)), "activity": m.group(2), "window": m.group(3),
                    "frame": (int(m.group(4)), int(m.group(5))), "views": views, "raw": raw}
    return None


def norm(s):
    return re.sub(r"\s+", " ", s or "").strip().lower()


def find_view(dump, labels, done):
    """The first enabled view whose text or description is one of labels, not
    tapped twice already."""
    for want in labels:
        for v in dump["views"]:
            key = (dump["activity"], v[1], v[2], v[3])
            if v[5] and want in (norm(v[2]), norm(v[3])) and done.get(key, 0) < 2:
                return v, key
    return None, None


def tap_view(hwnd, dump, v):
    rc = wintypes.RECT()
    U32.GetClientRect(hwnd, ctypes.byref(rc))
    fw, fh = dump["frame"]
    l, t, r, b = v[6]
    x = (l + r) / 2 * (rc.right / fw if fw else 1)
    y = (t + b) / 2 * (rc.bottom / fh if fh else 1)
    click(hwnd, x, y)
    return f"tapped {v[0].rsplit('.', 1)[-1]} text={v[2]!r} desc={v[3]!r} at {x:.0f},{y:.0f}"


# Buttons of first-run screens, pressed in this order of preference. Nothing
# that signs in, registers or pays.
ONBOARD = [norm(x) for x in [
    "Skip", "דלג", "דילוג", "Not now", "לא עכשיו", "No thanks", "לא תודה", "Later", "אחר כך", "מאוחר יותר",
    "Got it", "הבנתי", "Close", "סגור", "Next", "הבא", "Continue", "המשך", "Accept", "I agree", "Agree",
    "מסכים", "אני מסכים", "מאשר", "אני מאשר", "Accept all", "OK", "אישור", "אוקיי", "Allow", "Start", "התחל",
    "Get started", "בואו נתחיל", "Let's start", "יאללה", "Use without an account", "Stay signed out",
    "Use Google Maps without an account", "Continue without account", "Agree and continue",
    "Accept & continue", "Accept and continue",
]]
# An app's error or refusal dialog: a window that draws is not a working app.
ERROR_DIALOG = re.compile(r"(התקנה מחדש|להתקין את האפליקציה מחדש|שגיאה|תקלה|אירעה בעיה|נסה שוב מאוחר|עדכן את האפליקציה|"
                          r"גרסה חדשה|מכשיר פרוץ|root|rooted|emulator|אמולטור|error|went wrong|try again later|"
                          r"update required|please update|reinstall|not supported|unsupported device|"
                          r"requires the google app|download or enable)", re.I)
# A screen that asks for a login: as far as a bank app goes in this test.
LOGIN = re.compile(r"(סיסמ|password|קוד משתמש|שם משתמש|user ?name|user ?code|תעודת זהות|תז\b|ת\.ז|כניסה לחשבון|"
                   r"התחבר|log ?in|sign ?in|הזדה|מספר טלפון|phone number|קוד אימות|verification code)", re.I)


def processes():
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='winapk-runtime.exe' or Name='java.exe'\" | "
          "Select-Object ProcessId,ParentProcessId,WorkingSetSize,CommandLine | ConvertTo-Json -Compress")
    r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True)
    try:
        data = json.loads(r.stdout or "[]")
    except ValueError:
        return []
    if isinstance(data, dict):
        data = [data]
    return [(d["ProcessId"], d["ParentProcessId"], d.get("WorkingSetSize") or 0, d.get("CommandLine") or "")
            for d in data]


def thread_dump(pid, path):
    """Every Java thread's stack of the app's process (its JVM runs in winapk-runtime.exe), through the
    runner's JDK's jcmd (JAVA_HOME, set by setup-java)."""
    jcmd = os.path.join(os.environ.get("JAVA_HOME", ""), "bin", "jcmd.exe")
    if not os.path.exists(jcmd):
        return f"no jcmd at {jcmd}"
    try:
        r = subprocess.run([jcmd, str(pid), "Thread.print", "-l"], capture_output=True, text=True, timeout=90,
                           encoding="utf-8", errors="replace")
        out = (r.stdout or "") + (r.stderr or "")
    except subprocess.TimeoutExpired:
        out, r = "jcmd timed out", None
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(out)
    return f"thread dump -> {os.path.basename(path)} ({len(out)} chars, exit {r.returncode if r else '-'})"


def memory_of(pid):
    """(working set MB, peak working set MB, private bytes MB) of one process, or None."""
    class Counters(ctypes.Structure):  # PROCESS_MEMORY_COUNTERS_EX
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
            (n, ctypes.c_size_t) for n in ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                                           "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
                                           "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage",
                                           "PrivateUsage")]
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = wintypes.HANDLE
    h = k32.OpenProcess(0x1000 | 0x0010, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ
    if not h:
        return None
    c = Counters()
    c.cb = ctypes.sizeof(c)
    ok = k32.K32GetProcessMemoryInfo(wintypes.HANDLE(h), ctypes.byref(c), c.cb)
    k32.CloseHandle(wintypes.HANDLE(h))
    if not ok:
        return None
    return c.WorkingSetSize >> 20, c.PeakWorkingSetSize >> 20, c.PrivateUsage >> 20


def cpu_seconds(pid):
    """The process's user + kernel CPU time in seconds, or None."""
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = wintypes.HANDLE
    h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None
    t = [wintypes.FILETIME() for _ in range(4)]
    ok = k32.GetProcessTimes(wintypes.HANDLE(h), *[ctypes.byref(x) for x in t])
    k32.CloseHandle(wintypes.HANDLE(h))
    if not ok:
        return None
    return sum(((x.dwHighDateTime << 32) | x.dwLowDateTime) for x in t[2:]) / 1e7


def drag(hwnd, x0, y0, dx, dy, steps=20, step_s=0.02):
    """A finger drag as the window's mouse messages: down, moves with the button held, up."""
    def lp(x, y):
        return (int(y) << 16) | (int(x) & 0xFFFF)
    U32.PostMessageW(hwnd, 0x0201, 1, lp(x0, y0))   # WM_LBUTTONDOWN
    for i in range(1, steps + 1):
        time.sleep(step_s)
        U32.PostMessageW(hwnd, 0x0200, 1, lp(x0 + dx * i / steps, y0 + dy * i / steps))  # WM_MOUSEMOVE, MK_LBUTTON
    time.sleep(step_s)
    U32.PostMessageW(hwnd, 0x0202, 0, lp(x0 + dx, y0 + dy))   # WM_LBUTTONUP


def pan_cpu(pid, hwnd, seconds=10):
    """CPU of the app's process idle for `seconds`, then while its window is dragged around for as
    long: percent of one core each."""
    c0, t0 = cpu_seconds(pid), time.time()
    time.sleep(seconds)
    c1, t1 = cpu_seconds(pid), time.time()
    rc = wintypes.RECT()
    U32.GetClientRect(hwnd, ctypes.byref(rc))
    cx, cy, d = rc.right / 2, rc.bottom / 2, min(rc.right, rc.bottom) / 4
    moves = [(d, 0), (-d, 0), (0, d), (0, -d), (d, d), (-d, -d)]
    i = 0
    while time.time() - t1 < seconds:
        mx, my = moves[i % len(moves)]
        drag(hwnd, cx - mx / 2, cy - my / 2, mx, my)
        time.sleep(0.15)
        i += 1
    c2, t2 = cpu_seconds(pid), time.time()
    if None in (c0, c1, c2):
        return "cpu: unreadable"
    return (f"cpu idle {100 * (c1 - c0) / (t1 - t0):.0f}% of a core, while dragged ({i} drags) "
            f"{100 * (c2 - c1) / (t2 - t1):.0f}%")


def resize(hwnd):
    """The window 30% wider and 20% lower, as a user's drag of its corner would end."""
    rc = wintypes.RECT()
    U32.GetWindowRect(hwnd, ctypes.byref(rc))
    w, h = rc.right - rc.left, rc.bottom - rc.top
    U32.SetWindowPos(hwnd, 0, rc.left, rc.top, int(w * 1.3), int(h * 0.8), 0x0014)  # SWP_NOZORDER|SWP_NOACTIVATE
    U32.GetWindowRect(hwnd, ctypes.byref(rc))
    return f"window {w}x{h} -> {rc.right - rc.left}x{rc.bottom - rc.top}"


GL_LINE = re.compile(r"(window surface|readback|EGL|eglCreate|SurfaceView|GLSurfaceView|\[gl\]|ANGLE|\[provider|"
                     r"SSLContext|createSSLEngine|Conscrypt)", re.I)


def gl_lines(path, limit=60):
    """The run log's GL and TLS provider lines (the window surface's frame cost among them)."""
    out = []
    try:
        for line in open(path, encoding="utf-8", errors="replace"):
            if GL_LINE.search(line) and "[viewdump]" not in line:
                out.append(line.rstrip()[:300])
    except OSError:
        pass
    return (["gl/tls lines:"] + out[:limit // 2] + (["..."] if len(out) > limit else []) + out[-(limit // 2):]
            if len(out) > limit else (["gl/tls lines:"] + out if out else []))


FIRST_FRAME = re.compile(r"startup: first frame at ([0-9.]+) s; working set (\d+) MB \(peak (\d+)\), private (\d+) MB")


def kill_all():
    subprocess.run(["taskkill", "/F", "/T", "/IM", "winapk-runtime.exe"], capture_output=True)


def runtime_cmd(args, apk, install_only=False):
    cmd = [os.path.join(args.runtime, "bin", "winapk-runtime.exe"), "--apk", apk, "--data", args.data]
    if args.jdk:
        cmd += ["--jdk", args.jdk]
    return cmd + (["--install-only"] if install_only else [])


def logged_popen(cmd, log_path, env):
    start = time.time()
    log = open(log_path, "w", encoding="utf-8", errors="replace")
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)

    def copy():
        try:
            for raw in p.stdout:
                log.write(f"{time.time() - start:8.3f} " + raw.decode("utf-8", "replace"))
                log.flush()
        except (OSError, ValueError):
            pass
        log.close()
    t = threading.Thread(target=copy, daemon=True)
    t.start()
    return p, t


def clean_env():
    env = dict(os.environ)
    for k in list(env):
        if k.startswith(("APKEEP", "APK_URLS", "WINAPK_", "MACAPK_")) or "TOKEN" in k.upper() or "SECRET" in k.upper():
            env.pop(k)
    return env


def install(args, src, results):
    name = os.path.basename(src)
    stem, ext = os.path.splitext(name)
    lib = os.path.join(args.data, "library")
    os.makedirs(lib, exist_ok=True)
    target = os.path.join(lib, name)
    shutil.copy(src, target)
    say(f"install {stem} ({os.path.getsize(src) >> 20} MB)")
    start = time.time()
    p, t = logged_popen(runtime_cmd(args, target, True), os.path.join(args.out, stem, "install.log"), clean_env())
    try:
        code = p.wait(timeout=args.install_timeout)
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], capture_output=True)
        code = "timeout"
    t.join(timeout=10)
    secs = round(time.time() - start, 1)
    app = os.path.join(args.data, "apps", stem)
    ok = code == 0 and os.path.exists(os.path.join(app, "label.txt"))
    if ok:
        shutil.move(os.path.join(app, "label.txt"), os.path.join(lib, stem + ".label"))
        if os.path.exists(os.path.join(app, "package.txt")):
            shutil.move(os.path.join(app, "package.txt"), os.path.join(lib, stem + ".package"))
    pkgfile = os.path.join(lib, stem + ".package")
    pk = open(pkgfile, encoding="utf-8").read().split("\n") if os.path.exists(pkgfile) else []
    size = 0
    for root, _, files in os.walk(app):
        for f in files:
            try:
                size += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    r = {"install_exit": code, "install_seconds": secs, "installed": ok, "package_txt": pk[:4],
         "app_dir_mb": size >> 20}
    say(f"  -> {r}")
    return r, target if ok else None


def write_registry(args):
    """installed_apps.json as the launcher writes it."""
    lib = os.path.join(args.data, "library")
    rows = []
    for f in sorted(os.listdir(lib)):
        stem, ext = os.path.splitext(f)
        if ext.lower() not in EXTS or not os.path.exists(os.path.join(lib, stem + ".label")):
            continue
        pkgf = os.path.join(lib, stem + ".package")
        p = open(pkgf, encoding="utf-8").read().replace("\r", "").split("\n") if os.path.exists(pkgf) else []
        if not p or not p[0]:
            continue
        when = datetime.datetime.utcfromtimestamp(os.path.getmtime(pkgf)).strftime("%Y-%m-%dT%H:%M:%SZ")
        apk = os.path.join(lib, f)
        rows.append({"package": p[0], "label": open(os.path.join(lib, stem + ".label"), encoding="utf-8").read().strip(),
                     "versionCode": int(p[1]) if len(p) > 1 and p[1].strip().isdigit() else 0,
                     "versionName": p[2] if len(p) > 2 else "", "apkPath": apk, "bundlePath": apk,
                     "dirName": stem, "state": "installed", "installedAt": when, "updatedAt": when})
    with open(os.path.join(args.data, "installed_apps.json"), "w", encoding="utf-8") as fh:
        json.dump({"schema": 2, "apps": rows}, fh, indent=1)


def drive(args, pkg, apk, tag, r):
    """Opens the app in a window and follows it for args.minutes."""
    d = os.path.join(args.out, pkg)
    shots = os.path.join(d, "shots" + tag)
    os.makedirs(shots, exist_ok=True)
    log_path = os.path.join(d, f"run-{pkg}{tag}.log")
    env = clean_env()
    env["WINAPK_DUMP_VIEWS"] = "1"
    env.update(args.extra_env)
    p, reader = logged_popen(runtime_cmd(args, apk), log_path, env)
    start = time.time()
    events, done = [], {}
    st = {"exit": None, "window": False, "drew": False, "login": None, "taps": 0, "screens": [],
          "prompts": [], "peak_ws_mb": 0, "first_draw_s": None}
    last_seq, last_shot, nshot = -1, 0.0, 0
    login_at = None
    dump_at = sorted(getattr(args, "dump_at", []) or [])
    actions = list(getattr(args, "actions", []) or [])

    def ev(msg):
        e = f"{round(time.time() - start)}s: {msg}"
        events.append(e)
        say(f"{pkg}{tag}: {e}")

    while time.time() - start < args.minutes * 60:
        time.sleep(2)
        t = time.time() - start
        if p.poll() is not None:
            st["exit"] = p.returncode
            ev(f"the app exited with {p.returncode} (0x{p.returncode & 0xFFFFFFFF:08X})")
            break
        if t >= 60 and "mem_60" not in st:
            m = memory_of(p.pid)
            if m:
                st["mem_60"] = {"ws_mb": m[0], "peak_ws_mb": m[1], "private_mb": m[2]}
        if dump_at and t >= dump_at[0]:
            dump_at.pop(0)
            ev(thread_dump(p.pid, os.path.join(d, f"threads-{int(t):04d}s.txt")))
        if int(t) % 10 < 2:
            ws = sum(x[2] for x in processes() if f"{pkg}" in x[3]) >> 20
            st["peak_ws_mb"] = max(st["peak_ws_mb"], ws)
        wins = top_windows()
        for h, pid, cls, title in wins:
            if cls == "WinApkPrompt":
                bmp = capture_window(h)
                if bmp:
                    write_png(bmp, os.path.join(shots, f"{int(t):04d}s-prompt.png"))
                st["prompts"].append(title)
                ev(f"Win.apk asked: {title!r}; Enter (its default answer)")
                U32.PostMessageW(h, 0x0100, 0x0D, 0)   # WM_KEYDOWN VK_RETURN
                U32.PostMessageW(h, 0x0101, 0x0D, 0)
        app = [h for h, pid, cls, _ in wins if cls == "WinApkAppWindow" and pid == p.pid]
        hwnd = app[0] if app else None
        if hwnd and not st["window"]:
            st["window"] = True
            ev("window open")
        if hwnd and actions and t >= actions[0][0]:
            kind = actions.pop(0)[1]
            if kind == "pan":
                b = capture_window(hwnd)
                if b:
                    write_png(b, os.path.join(shots, f"{int(t):04d}s-before-pan.png"))
                st["cpu"] = pan_cpu(p.pid, hwnd)
                ev(st["cpu"])
                time.sleep(1)
                b = capture_window(hwnd)
                if b:
                    write_png(b, os.path.join(shots, f"{int(time.time() - start):04d}s-after-pan.png"))
            elif kind == "resize":
                ev(resize(hwnd))
                time.sleep(5)
                b = capture_window(hwnd)
                if b:
                    write_png(b, os.path.join(shots, f"{int(time.time() - start):04d}s-resized.png"))
                    ev(f"after the resize: client {b[0]}x{b[1]}, blank={blank(b)}")
            continue
        bmp = capture_window(hwnd) if hwnd else None
        if bmp and not st["drew"] and not blank(bmp):
            st["drew"] = True
            st["first_draw_s"] = round(t)
            ev("the window shows something")
        dump = latest_viewdump(log_path)
        new = dump is not None and dump["seq"] != last_seq
        if bmp and (new or t - last_shot > 20) and nshot < 60:
            nshot += 1
            last_shot = t
            write_png(bmp, os.path.join(shots, f"{int(t):04d}s.png"))
        if not dump or not hwnd:
            continue
        if new:
            last_seq = dump["seq"]
            with open(os.path.join(d, f"viewdumps{tag}.txt"), "a", encoding="utf-8") as fh:
                fh.write(f"--- {int(t)}s\n" + "\n".join(dump["raw"]) + "\n")
            texts = [(v[2] or v[3]).strip() for v in dump["views"] if (v[2] or v[3]).strip()]
            act = dump["activity"].rsplit(".", 1)[-1]
            st["screens"].append(f"{act}: {texts[:12]}")
            ev(f"screen {dump['seq']}: {dump['activity']} ({dump['window']}), {len(dump['views'])} views: {texts[:30]}")
            fields = [v for v in dump["views"] if "EditText" in v[0] or "TextField" in v[0]]
            bad = [x for x in texts if ERROR_DIALOG.search(x)]
            if bad and not st.get("error_dialog"):
                st["error_dialog"] = f"{act} ({dump['window']}): {bad[:4]}"
                ev(f"an error dialog: {bad[:4]}")
            asks = any(LOGIN.search(x) for x in texts + [v[1] for v in fields])
            if st["login"] is None and fields and asks:
                st["login"] = f"{act}: {texts[:12]}"
                login_at = t
                ev("a login screen (input fields): the test stops here for this app")
            elif asks and not st.get("login_offer"):
                st["login_offer"] = f"{act}: {texts[:12]}"
                ev("a screen that offers a login")
        if login_at is not None:
            if t - login_at > 15:
                break
            continue
        if st["taps"] < 15:
            v, key = find_view(dump, ONBOARD, done)
            if v:
                time.sleep(1)
                ev(tap_view(hwnd, dump, v))
                done[key] = done.get(key, 0) + 1
                st["taps"] += 1
    if hwnd := next((h for h, pid, cls, _ in top_windows() if cls == "WinApkAppWindow" and pid == p.pid), None):
        bmp = capture_window(hwnd)
        if bmp:
            write_png(bmp, os.path.join(shots, f"{int(time.time() - start):04d}s-last.png"))
    if p.poll() is None:
        m = memory_of(p.pid)  # the process's own peak, over the whole run
        if m:
            st["peak_ws_process_mb"] = m[1]
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], capture_output=True)
        p.wait()
    reader.join(timeout=10)
    kill_all()
    st["events"] = events
    try:
        text = open(log_path, encoding="utf-8", errors="replace").read()
        m = FIRST_FRAME.search(text)
        if m:
            st["first_frame"] = {"s": float(m.group(1)), "ws_mb": int(m.group(2)), "peak_ws_mb": int(m.group(3)),
                                 "private_mb": int(m.group(4))}
        abis = sorted(set(re.findall(r"\\lib\\(x86_64|arm64|arm64-v8a|x86)\\", text)))
        st["abi"] = ",".join(abis) or "no native code loaded"
    except OSError:
        st["abi"] = "?"
    r["run" + tag] = st


ERR = re.compile(r"(FATAL|AndroidRuntime|Exception|Error\b|not implemented|cannot load|UnsatisfiedLink|"
                 r"unresolved|SIGSEGV|crash|abort|emulator|root(ed)?\b|Integrity|unsupported|missing)", re.I)


def first_errors(path, limit=40):
    out = []
    try:
        for line in open(path, encoding="utf-8", errors="replace"):
            if ERR.search(line) and "[viewdump]" not in line:
                out.append(line.rstrip()[:300])
                if len(out) >= limit:
                    break
    except OSError:
        pass
    return out


def collect(args):
    """Win.apk's own logs folder (crash dumps excepted)."""
    src = os.path.join(args.data, "logs")
    dst = os.path.join(args.out, "winapk-logs")
    if os.path.isdir(src):
        for root, _, files in os.walk(src):
            for n in files:
                if n.endswith(".dmp"):
                    continue
                s = os.path.join(root, n)
                t = os.path.join(dst, os.path.relpath(s, src))
                os.makedirs(os.path.dirname(t), exist_ok=True)
                try:
                    shutil.copy2(s, t)
                except OSError:
                    pass
    dumps = []
    for root, _, files in os.walk(src):
        dumps += [n for n in files if n.endswith(".dmp")]
    return dumps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runtime", required=True)
    ap.add_argument("--jdk", default="")
    ap.add_argument("--apks", required=True)
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--minutes", type=float, default=4)
    ap.add_argument("--runs", type=int, default=2, help="opens per app (the second after the first's caches)")
    ap.add_argument("--only", default="")
    ap.add_argument("--install-timeout", type=int, default=1800)
    ap.add_argument("--env", action="append", default=[], help="NAME=VALUE for the app runs")
    args = ap.parse_args()
    args.runtime = os.path.abspath(args.runtime)
    args.out = os.path.abspath(args.out)
    args.extra_env = dict(e.split("=", 1) for e in args.env)
    os.makedirs(args.out, exist_ok=True)
    shutil.rmtree(args.data, ignore_errors=True)
    os.makedirs(args.data, exist_ok=True)
    # Portrait, as a phone: the bank apps are portrait anyway.
    subprocess.run(["reg", "add", r"HKCU\Software\Win.apk", "/v", "Landscape", "/t", "REG_DWORD", "/d", "0", "/f"],
                   capture_output=True)
    srcs = {os.path.splitext(f)[0]: os.path.join(args.apks, f) for f in sorted(os.listdir(args.apks))
            if f.endswith(EXTS)}
    only = [x for x in args.only.split(",") if x]
    say(f"APK sets: {sorted(srcs)}")
    results = {}
    # In --only's order: an app another needs (the Google app for Gemini) goes first.
    for pkg in (only or list(srcs)):
        src = srcs.get(pkg)
        if not src:
            results[pkg] = {"install_exit": "no download", "install_seconds": 0, "installed": False,
                            "package_txt": [], "app_dir_mb": 0}
            continue
        os.makedirs(os.path.join(args.out, pkg), exist_ok=True)
        try:
            r, apk = install(args, src, results)
            results[pkg] = r
            write_registry(args)
            if apk:
                for n in range(args.runs):
                    drive(args, pkg, apk, "" if n == 0 else f"-{n + 1}", r)
        except Exception as e:
            import traceback
            traceback.print_exc()
            results.setdefault(pkg, {"install_exit": "harness error", "install_seconds": 0, "installed": False,
                                     "package_txt": [], "app_dir_mb": 0})["harness_error"] = repr(e)
            kill_all()
        with open(os.path.join(args.out, "results.json"), "w", encoding="utf-8") as fh:
            json.dump(results, fh, indent=1, ensure_ascii=False)
    dumps = collect(args)
    lines = [f"# Win.apk app test, {datetime.datetime.now():%Y-%m-%d %H:%M}", ""]
    if dumps:
        lines += [f"Crash dumps: {dumps}", ""]
    for pkg, r in results.items():
        lines.append(f"## {pkg}")
        if "harness_error" in r:
            lines.append(f"- harness error: {r['harness_error']}")
        lines.append(f"- install: exit {r['install_exit']} in {r['install_seconds']} s, installed={r['installed']}, "
                     f"package.txt={r['package_txt']}, app folder {r['app_dir_mb']} MB")
        for k in sorted(x for x in r if x.startswith("run")):
            st = r[k]
            verdict = ("ERROR DIALOG: " + st["error_dialog"]) if st.get("error_dialog") else \
                ("login screen" if st.get("login") or st.get("login_offer") else
                 ("drew" if st["drew"] else "nothing drawn"))
            lines.append(f"- {k}: **{verdict}** (native code: {st.get('abi')})")
            lines.append(f"- {k}: exit={st['exit']} window={st['window']} drew={st['drew']} (at {st['first_draw_s']} s) "
                         f"login={st['login']!r} taps={st['taps']} peak WS {st['peak_ws_mb']} MB prompts={st['prompts']}")
            for s in st["screens"][:15]:
                lines.append(f"    - {s}")
        lines.append("- first error lines (install):")
        lines += ["    " + l for l in first_errors(os.path.join(args.out, pkg, "install.log"), 15)]
        lines.append("- first error lines (run):")
        lines += ["    " + l for l in first_errors(os.path.join(args.out, pkg, f"run-{pkg}.log"))]
        lines.append("")
    with open(os.path.join(args.out, "summary.md"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    say("summary.md written")


if __name__ == "__main__":
    main()
