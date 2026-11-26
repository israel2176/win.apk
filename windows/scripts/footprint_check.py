#!/usr/bin/env python3
"""Win.apk's size budget: fails when the installed runtime or the installer has grown past it.

    footprint_check.py <stage dir> [<installer .exe>]

<stage dir> is what the installer packs (a staged runtime with jre/, as ci.yml's Package job has
it). Prints each part's size beside its budget (and, under GitHub Actions, the same table in the
job's summary) and exits 1 when any part is over, listing the largest files of that part.

The budgets are today's sizes with some headroom, in MB (10^6 bytes), the files' own sizes summed
(what the installer unpacks; NTFS adds a little per file). Raising one is a decision, not a fix:
do it in the change that needs the room, say why in its description, and update the numbers in
windows/README.md, "Footprint". The stage carries AOSP's graphics, resources and views
(build_aosp_gfx.sh, build_aosp_res.sh, build_aosp_views.sh), in CI as in a release: they are the
only ones the framework has.
"""

import os
import sys

# (part, the paths it holds relative to the stage, budget in MB). A path ending in "/" is a folder;
# the last part takes every file the others do not.
BUDGETS = [
    # Before AOSP graphics' part, whose bin/aosp/ would take the table. AOSP's resources and views,
    # the only ones apps run on since November 2026 (Win.apk's own left android-framework.jar, and
    # bin/tools/framework-res.apk, Win.apk's API-36 table, 22.9 MB, went):
    # 1.4 + 6.7 + 13.7 MB, about 19.0 MB under LZMA (the table's three default_wallpaper PNGs are
    # 1x1, build_aosp_res.sh).
    ("AOSP resources and views", ["lib/aosp-resources.jar", "lib/aosp-views.jar", "bin/aosp/framework-res-34.apk"],
     25),
    # AOSP's app layer (ActivityThread, Activity, ContextImpl...: docs/research/aosp-app.md), 1.6 MB with
    # the classes reflection on AOSP's classes names (ReflectClosure.java; 1.3 MB, about 1.25 MB under LZMA,
    # before them); Win.apk's own android.app classes it shadows left the framework jar. 2.1 MB since the
    # platform classes apps extend that Win.apk had none of (aosp_app_take.sh's PLATFORM_TAKE: app search
    # 0.19 MB, Wi-Fi Aware 0.11, RTT 0.05, slices 0.04).
    ("AOSP app layer", ["lib/aosp-app.jar"], 3),
    ("AOSP graphics (what everything draws with)",
     ["bin/aosp/", "bin/robolectric-nativeruntime.dll", "lib/aosp-graphics.jar"], 70),
    ("Java runtime (jre/)", ["jre/"], 140),
    ("ARM translation's compiler (clang/)", ["clang/"], 250),
    # 120 until November 2026; 24 MB less since (the API-36 table and the framework's shadowed classes),
    # and 7.5 MB less again (about 7.4 MB under LZMA) with Win.apk's Java2D graphics gone from
    # android-framework.jar (its classes, and the fonts it bundled: 6.9 MB of the jar).
    ("Win.apk itself (the rest, bin/tools/ included)", None, 92),
]
TOTAL_BUDGET = 574       # the installed folder (572 until AOSP's app layer, +1.3 MB)
INSTALLER_BUDGET = 160   # Win.apk-Setup.exe (158 until AOSP's app layer, +1.25 MB)


def files(stage):
    for root, dirs, names in os.walk(stage):
        dirs[:] = [d for d in dirs if d != ".build"]  # build_aosp_gfx.sh's scratch, never packed
        for n in names:
            path = os.path.join(root, n)
            yield os.path.relpath(path, stage).replace(os.sep, "/"), os.path.getsize(path)


def part_of(rel):
    for i, (_, paths, _) in enumerate(BUDGETS):
        if paths and any(rel.startswith(p) if p.endswith("/") else rel == p for p in paths):
            return i
    return len(BUDGETS) - 1


def mb(n):
    return n / 1e6


def main():
    if len(sys.argv) not in (2, 3) or not os.path.isdir(sys.argv[1]):
        sys.exit(__doc__.split("\n\n")[1])
    stage = sys.argv[1]
    sizes = [0] * len(BUDGETS)
    members = [[] for _ in BUDGETS]
    for rel, size in files(stage):
        i = part_of(rel)
        sizes[i] += size
        members[i].append((size, rel))
    rows = [(name, mb(sizes[i]), budget) for i, (name, _, budget) in enumerate(BUDGETS)]
    rows.append(("Installed, in all", mb(sum(sizes)), TOTAL_BUDGET))
    if len(sys.argv) == 3:
        rows.append(("Installer (" + os.path.basename(sys.argv[2]) + ")", mb(os.path.getsize(sys.argv[2])),
                     INSTALLER_BUDGET))

    over = [r for r in rows if r[1] > r[2]]
    lines = ["| Part | Size | Budget |", "|---|---:|---:|"]
    lines += [f"| {name} | {size:.1f} MB | {budget} MB{' **over**' if size > budget else ''} |"
              for name, size, budget in rows]
    print("\n".join(lines))
    for i, (name, _, budget) in enumerate(BUDGETS):
        if mb(sizes[i]) > budget:
            print(f"\n{name}: its largest files")
            for size, rel in sorted(members[i], reverse=True)[:12]:
                print(f"  {mb(size):8.1f} MB  {rel}")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write("### Footprint (windows/scripts/footprint_check.py)\n\n" + "\n".join(lines) + "\n\n")
    if over:
        print("\n[footprint] OVER BUDGET: " + ", ".join(f"{n} {s:.1f} MB > {b} MB" for n, s, b in over) +
              ". Make it smaller, or raise the budget in windows/scripts/footprint_check.py with the reason"
              " (windows/README.md, \"Footprint\").")
        return 1
    print("\n[footprint] within budget")
    return 0


if __name__ == "__main__":
    sys.exit(main())
