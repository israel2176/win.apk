#!/usr/bin/env python3
"""The frame bench's numbers as a table (windows/README.md, "Frame time").

    python windows/test/framebench/report.py <run log>... [--min-frames N] [--budget-ms MS]

Reads the `[win.apk] frame times (<graphics> graphics): ...` line each run's log ends with
(host/frame_times.h) and prints a Markdown table, one row per log: frames, frames a second, and
the median / p90 / p99 / max of layout, draw, reading the pixels, the present and the whole
frame, in ms. With both engines, a line comparing their medians follows.

Exits 1 when a log has no such line (the record is broken, or the app drew nothing), when a run
has fewer than --min-frames frames after its warm-up (the bench did not keep drawing: 300 by
default, 5 s at 60 a second), or when a run's p90 of the whole frame is over --budget-ms (50 by
default, three vsyncs: only a collapse, since CI's machines vary too much for a tight budget, and
AOSP's graphics take 30-40 ms at p90 today under Wine, two copies of the frame's pixels a frame:
docs/research/open-issues.md).
"""

import argparse
import re
import sys

LINE = re.compile(r"frame times \((?P<gfx>\w+) graphics\): (?P<frames>\d+) frames in (?P<span>[\d.]+) s"
                  r"(?:, (?P<rate>[\d.]+)/s)?, (?P<scope>[^;]*); ms median/p90/p99/max: (?P<parts>.*)$")
PART = re.compile(r"(\w+) ([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+)")


def parse(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = LINE.search(raw.rstrip("\r\n"))
            if m:
                parts = {p[0]: [float(x) for x in p[1:]] for p in PART.findall(m.group("parts"))}
                return {"gfx": m.group("gfx"), "frames": int(m.group("frames")), "rate": m.group("rate") or "-",
                        "warmup_only": "warm-up" in m.group("scope"), "parts": parts}
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--min-frames", type=int, default=300)
    ap.add_argument("--budget-ms", type=float, default=50.0)
    a = ap.parse_args()

    failures = []
    runs = []
    for path in a.logs:
        r = parse(path)
        if r is None:
            failures.append(f"{path}: no `frame times` line")
            continue
        runs.append(r)
        if r["warmup_only"] or r["frames"] < a.min_frames:
            failures.append(f"{path}: {r['frames']} frames after the warm-up, fewer than {a.min_frames}")
        if r["parts"].get("total", [0, 0])[1] > a.budget_ms:
            failures.append(f"{path}: whole frame p90 {r['parts']['total'][1]:.2f} ms, over {a.budget_ms} ms")

    print("### Frame time (the frame bench, ms: median / p90 / p99 / max)\n")
    print("| Graphics | Frames | Per second | Layout | Draw | Pixels | Present | Whole frame |")
    print("|---|---:|---:|---|---|---|---|---|")
    for r in runs:
        cells = []
        for name in ("layout", "draw", "pixels", "present", "total"):
            v = r["parts"].get(name)
            cells.append(" / ".join(f"{x:.2f}" for x in v) if v else "-")
        print(f"| {r['gfx']} | {r['frames']} | {r['rate']} | " + " | ".join(cells) + " |")
    by = {r["gfx"]: r for r in runs}
    if "aosp" in by and "java2d" in by:
        a_draw, j_draw = by["aosp"]["parts"]["draw"][0], by["java2d"]["parts"]["draw"][0]
        a_tot, j_tot = by["aosp"]["parts"]["total"][0], by["java2d"]["parts"]["total"][0]
        if a_draw > 0 and a_tot > 0:
            print(f"\nMedian draw: Java2D {j_draw / a_draw:.2f}x AOSP's; median whole frame: "
                  f"Java2D {j_tot / a_tot:.2f}x AOSP's.")
    if failures:
        print("\n**Failed:**\n")
        for f in failures:
            print(f"- {f}")
    print()
    for f in failures:
        print(f"report.py: {f}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
