#!/usr/bin/env python3
"""Runs the UI probe or the screens sweep in Win.apk and ends it once it is done.

    run_until.py <log> <timeout seconds> -- <command> [args...]

The command's stdout and stderr go to <log>. Some apps' runtime does not exit
after the sweep has printed its last line ("PROBE done"): the process is left
waiting on a thread of the app. So once that line is in the log, the command
gets a few seconds (RUN_UNTIL_GRACE, default 10) to exit by itself and is then
ended, with every process it started. At the timeout it is ended too.

RUN_UNTIL_STALL (seconds, default 0: off) also ends it when that long passes
with no new "PROBE " line (counted from the start for the first one): a runtime
that never starts the sweep (one older than LayoutSweep runs the app itself,
which never ends) or a sweep stuck on one case is ended then, not at the
timeout.

Exit status: the command's own, 0 when it was ended after "PROBE done", 124 at
the timeout (as timeout(1)), 125 when it stalled.
"""
import os
import re
import signal
import subprocess
import sys
import time

DONE = re.compile(rb'^PROBE done\r?$', re.M)
LINE = re.compile(rb'^PROBE ', re.M)


def kill_tree(p):
    if os.name == 'nt':
        subprocess.run(['taskkill', '/F', '/T', '/PID', str(p.pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            p.kill()
    try:
        p.wait(timeout=30)
    except subprocess.TimeoutExpired:
        pass


def done_in(path, offset):
    """(done seen, a new PROBE line seen, new offset): reads only the whole lines the
    log gained since the last look; offset is always just after a line's end."""
    try:
        with open(path, 'rb') as f:
            f.seek(offset)
            data = f.read()
    except OSError:
        return False, False, offset
    whole = data.rfind(b'\n') + 1  # a line half written now is read whole at the next look
    data = data[:whole]
    return bool(DONE.search(data)), bool(LINE.search(data)), offset + whole


def main(argv):
    if len(argv) < 4 or argv[2] != '--':
        print(__doc__, file=sys.stderr)
        return 2
    log, timeout, cmd = argv[0], float(argv[1]), argv[3:]
    grace = float(os.environ.get('RUN_UNTIL_GRACE', '10'))
    stall = float(os.environ.get('RUN_UNTIL_STALL', '0'))
    start = time.monotonic()
    progress = start
    with open(log, 'wb') as out:
        kw = {} if os.name == 'nt' else {'start_new_session': True}
        p = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **kw)
        offset, done_at = 0, None
        while True:
            try:
                rc = p.wait(timeout=2)
                return rc if done_at is None else 0
            except subprocess.TimeoutExpired:
                pass
            now = time.monotonic()
            if done_at is None:
                seen, line, offset = done_in(log, offset)
                if line:
                    progress = now
                if seen:
                    done_at = now
            if done_at is not None and now - done_at >= grace:
                print(f'[run_until] done, but still running after {grace:.0f} s: ended', file=sys.stderr)
                kill_tree(p)
                return 0
            if done_at is None and stall > 0 and now - progress >= stall:
                print(f'[run_until] no new PROBE line for {stall:.0f} s: ended', file=sys.stderr)
                kill_tree(p)
                return 125
            if now - start >= timeout:
                print(f'[run_until] timed out after {timeout:.0f} s: ended', file=sys.stderr)
                kill_tree(p)
                return 124


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
