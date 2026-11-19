#!/usr/bin/env python3
"""Runs the UI probe or the screens sweep in Win.apk and ends it once it is done.

    run_until.py <log> <timeout seconds> -- <command> [args...]

The command's stdout and stderr go to <log>. Some apps' runtime does not exit
after the sweep has printed its last line ("PROBE done"): the process is left
waiting on a thread of the app. So once that line is in the log, the command
gets a few seconds (RUN_UNTIL_GRACE, default 10) to exit by itself and is then
ended, with every process it started. At the timeout it is ended too.

Exit status: the command's own, 0 when it was ended after "PROBE done", 124 at
the timeout (as timeout(1)).
"""
import os
import re
import signal
import subprocess
import sys
import time

DONE = re.compile(rb'^PROBE done\r?$', re.M)


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
    """(seen, new offset): reads only what the log gained since the last look."""
    start = max(0, offset - 16)  # the line may have been half written at the last look
    try:
        with open(path, 'rb') as f:
            f.seek(start)
            data = f.read()
    except OSError:
        return False, offset
    end = start + len(data)
    if start > 0:
        data = data[data.find(b'\n') + 1:]  # from a line's start: '^' must not match mid-line
    return bool(DONE.search(data)), end


def main(argv):
    if len(argv) < 4 or argv[2] != '--':
        print(__doc__, file=sys.stderr)
        return 2
    log, timeout, cmd = argv[0], float(argv[1]), argv[3:]
    grace = float(os.environ.get('RUN_UNTIL_GRACE', '10'))
    start = time.monotonic()
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
                seen, offset = done_in(log, offset)
                if seen:
                    done_at = now
            if done_at is not None and now - done_at >= grace:
                print(f'[run_until] done, but still running after {grace:.0f} s: ended', file=sys.stderr)
                kill_tree(p)
                return 0
            if now - start >= timeout:
                print(f'[run_until] timed out after {timeout:.0f} s: ended', file=sys.stderr)
                kill_tree(p)
                return 124


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
