"""comparison's table: summary.py <dir of compare-<package> folders> <out dir>.

Each folder holds app.json (the app and how the job ran) and measure.ps1's
<runtime>-<install|cold|warm>.json. Writes compare.json (everything) and compare.md (the table,
which the workflow also puts in its summary). Reads only JSON it parses itself (python -I)."""
import json
import pathlib
import statistics
import sys

RUNTIMES = (("winapk", "Win.apk"), ("other", "the other runtime"))


def load(path):
    try:
        with open(path, encoding="utf-8-sig") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def num(v, unit=""):
    return "–" if v is None else f"{v:g}{unit}"


def cell_install(r):
    if not r:
        return "not run"
    return f"{'yes' if r.get('install_ok') else 'NO'} {num(r.get('install_s'), ' s')}"


def cell_drawn(r):
    if not r:
        return "not run"
    mark = "yes" if r.get("drawn") else "NO"
    if r.get("exited_by_itself_s") is not None:
        mark += f", exited at {num(r['exited_by_itself_s'], ' s')}"
    return f"{mark} ({num(r.get('nonblank_pct'), '%')})"


def pair(a, b, key, unit=""):
    va = a.get(key) if a else None
    vb = b.get(key) if b else None
    return f"{num(va, unit)} / {num(vb, unit)}"


def main():
    src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    apps = []
    for d in sorted(p for p in src.iterdir() if p.is_dir()) if src.is_dir() else []:  # none when every job was cancelled
        meta = load(d / "app.json") or {"pkg": d.name.removeprefix("compare-")}
        runs = {f"{rt}-{ph}": load(d / f"{rt}-{ph}.json") for rt, _ in RUNTIMES for ph in ("install", "cold", "warm")}
        apps.append({"app": meta, "runs": runs})
    (out / "compare.json").write_text(json.dumps(apps, indent=1), encoding="utf-8")

    sources = sorted({a["app"].get(k, "") for a in apps for k in ("winapk_source", "other_source")} - {""})
    md = ["## Win.apk and the other runtime, the same apps on the same runner", ""]
    md += [f"- {s}" for s in sources]
    if apps:
        m = apps[0]["app"]
        md.append(f"- {m.get('seconds', '?')} s a run, display {m.get('display', '?')} dp at 160 dpi, {m.get('runner', '')}")
    md += ["- cold: nothing of the runtime running; warm: started again right after the cold run closed",
           "- cells with a slash are cold / warm; times from the launch command; memory = peak summed working "
           "set of the runtime's processes (the other runtime's waiting launcher apart); idle CPU in % of one core, "
           "last 30 s of the run", ""]
    md.append("| App | Kind | Runtime | Installs | Draws (cold) | First frame, s | First non-blank, s | "
              "Peak MB | Idle CPU % | Closes on WM_CLOSE |")
    md.append("|---|---|---|---|---|---|---|---|---|---|")
    totals = {rt: {"drawn": 0, "installed": 0, "frames": []} for rt, _ in RUNTIMES}
    for a in apps:
        meta, runs = a["app"], a["runs"]
        for i, (rt, label) in enumerate(RUNTIMES):
            ins, cold, warm = runs[f"{rt}-install"], runs[f"{rt}-cold"], runs[f"{rt}-warm"]
            closes = "/".join("–" if not r else ("yes" if r.get("closed_cleanly") else "no") for r in (cold, warm))
            first = "" if meta.get("first") != rt else " (ran first)"
            md.append("| " + " | ".join([
                f"{meta.get('pkg')} {meta.get('version', '')}" if i == 0 else "",
                meta.get("kind", "") if i == 0 else "",
                label + first, cell_install(ins), cell_drawn(cold),
                pair(cold, warm, "first_frame_s"), pair(cold, warm, "first_nonblank_s"),
                pair(cold, warm, "peak_mb"), pair(cold, warm, "idle_cpu_pct"), closes]) + " |")
            t = totals[rt]
            t["installed"] += bool(ins and ins.get("install_ok"))
            t["drawn"] += bool(cold and cold.get("drawn"))
    # First frames compared only where both drew: a median over different apps says nothing.
    both = [a for a in apps if all(a["runs"][f"{rt}-cold"] and a["runs"][f"{rt}-cold"].get("drawn")
                                   and a["runs"][f"{rt}-cold"].get("first_frame_s") is not None for rt, _ in RUNTIMES)]
    md += ["", f"Of {len(apps)} apps:", ""]
    for rt, label in RUNTIMES:
        t = totals[rt]
        line = f"- {label}: installed {t['installed']}, drew its first screen {t['drawn']}"
        for ph in ("cold", "warm"):
            frames = [a["runs"][f"{rt}-{ph}"].get("first_frame_s") for a in both if a["runs"][f"{rt}-{ph}"]]
            frames = [f for f in frames if f is not None]
            mems = [a["runs"][f"{rt}-{ph}"].get("peak_mb") for a in both if a["runs"][f"{rt}-{ph}"]]
            mems = [m for m in mems if m is not None]
            if frames:
                line += f"; {ph} median first frame {statistics.median(frames):g} s"
            if mems:
                line += f", median peak {statistics.median(mems):g} MB"
        md.append(line + f" (medians over the {len(both)} apps both drew)")
    (out / "compare.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    print("\n".join(md))


main()
