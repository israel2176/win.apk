# Class-data sharing for the app's JVM: what was measured and what ships

Issue: israel2176/win.apk#70.

Until 0.8, every start ran HotSpot with **no class-data sharing (CDS)**:
`jvm_options` applies upstream's `java.base`/`jdk.unsupported` overlays with
`--patch-module`, and HotSpot turns CDS off whenever a module is patched. So
each start parsed and linked the JDK's classes, the framework's
(`android-framework.jar`, `macapk-loader.jar`, bcprov) and the app's again.

What ships now (`windows/host/cds.h`, `windows/scripts/bake_jre.sh`):

- **The baked JRE.** The release's JRE is no longer Adoptium's download but a
  jlink image of Temurin 21's jmods with the overlay classes built into
  `java.base` and `jdk.unsupported`, a module list cut to what runs in the app's
  JVM, `--strip-debug`, and its default CDS archive regenerated over the baked
  classes. The runtime drops `--patch-module` when the JRE's
  `lib/winapk-baked-patches` records the same SHA-256 as `<lib>`'s patch
  folders; otherwise the flag stays and the start runs without CDS, and says so.
- **One framework archive for all apps**, `<data>\cds\framework-<key>.jsa`
  (about 39 MB): the JDK's and the framework's classes, dumped by
  `java -Xshare:dump` in the background after the first app start of each
  Win.apk and JRE version, from a shipped class list (the classes at least three
  of the ten smoke apps load) plus the classes that start loaded. bcprov is
  unsigned at build time, or CDS would skip all of its classes.
- **`-XX:-UsePerfData`**: no `hsperfdata` file per JVM.

## Results: this branch against main (0.8.0)

GitHub `windows-latest` (4 vCPU), headless, the ten smoke apps, run
[42527049721](https://github.com/israel2176/win.apk/actions/runs/42527049721).
*A* is main exactly: main's staged runtime (CI run 42521083975) with the
Adoptium JRE 21.0.13 the release ships. *F* is this branch with its defaults
(the archive made by the app's first start, as on a user's machine). Per app
and configuration: one first start in a fresh data folder (install, DEX
conversion, and for F the recording and the background dump), then three warm
starts, taken in turns A, F, A, F. Time is measured from outside, from creating
the process to its `first frame presented` line; working set and private bytes
(commit) are read at that moment. All 60 warm runs and 20 first starts drew.
That run's JRE had `lib/modules` compressed; the one that ships does not (see
Disk), which the run before found makes no difference to a start (B against Bz,
C against Cz below).

| App (kind) | first start A (s) | first start F (s) | warm A (s) | warm F (s) | warm | WS / private A (MB) | WS / private F (MB) |
|---|---|---|---|---|---|---|---|
| Jerboa (Compose) | 9.10 | 9.30 | 2.138 | 1.724 | -19% | 309 / 326 | 296 / 297 |
| Klooni (libGDX) | 3.99 | 3.30 | 0.923 | 0.689 | -25% | 264 / 314 | 264 / 283 |
| SGT Puzzles (JNI) | 5.33 | 4.73 | 3.204 * | 1.737 | -46% * | 303 / 320 | 297 / 285 |
| Editor (Java) | 4.50 | 3.22 | 1.586 | 1.258 | -21% | 254 / 307 | 255 / 270 |
| Fossify Notes (Kotlin) | 6.84 | 6.30 | 1.983 | 1.656 | -16% | 315 / 315 | 297 / 279 |
| Jellyfin (WebView) | 6.38 | 6.31 | 1.267 | 0.975 | -23% | 292 / 312 | 275 / 280 |
| Jitsi Meet (React Native) | 7.87 | 7.87 | 1.732 | 1.495 | -14% | 357 / 387 | 365 / 379 |
| OpenSudoku (arm64 JIT) | 4.10 | 3.84 | 1.345 | 1.088 | -19% | 287 / 313 | 276 / 274 |
| PPSSPP (NativeActivity) | 3.23 | 2.62 | 0.811 | 0.619 | -24% | 307 / 363 | 311 / 329 |
| Tensy (SDL) | 2.29 | 2.23 | 0.923 | 0.691 | -25% | 266 / 316 | 265 / 284 |

\* SGT Puzzles' main runs were 3.20, 4.12 and 2.07 s (2.1-2.5 s in the other
runs): a noisy minute of the machine; the median over apps is not moved by it.

| | main → this branch |
|---|---|
| Warm start to the first frame | **-22%** median (-14% to -25%), 0.27 s saved (0.19 to 0.41 s; SGT Puzzles' 1.5 s aside) |
| First start (install + conversion included) | -7% median (-29% to +2%) |
| JVM created | 0.12 s → 0.07 s after the process starts |
| Working set at the first frame | -3.5 MB median (-18 to +8) |
| Private bytes (commit) at the first frame | **-33 MB** median (-8 to -39) |
| The archive's dump, once per Win.apk/JRE version | 3 s in the background, at below-normal priority |
| Disk: the JRE | 146 MB (Adoptium) → 122 MB |
| Disk: `%LOCALAPPDATA%\Win.apk\cds` | +39 MB (one archive, all apps) |

Where a warm start's time goes (Fossify Notes): creating the JVM, then
`Boot.run` up to the main loop (the framework, resources, `Application`), then
the app's own Activity until the first frame. CDS shortens the first two; the
third, which is mostly the app's own classes running in C1, it does not touch.
That part is what stage 3 of the issue (caching `Boot.run`'s work) and the
window and ANGLE start would have to address.

The full sweep on this branch (92 apps, the JRE and one shared framework archive,
CI run [42526208806](https://github.com/israel2176/win.apk/actions/runs/42526208806))
matches main: 90 drew, `com.agateau.tinywheels.android` crashed and
`com.artifex.mupdf.viewer.app` stayed blank, as on main.

## How each piece was chosen

Run [42524653832](https://github.com/israel2176/win.apk/actions/runs/42524653832)
tried each piece and the cheap JVM flags on the ten apps, all on one machine per
pair of apps, every configuration's warm starts in turns. Warm first frame, the
median over apps of the ratio to main, and memory against main:

| Configuration | Warm vs main | Working set | Private | Verdict |
|---|---|---|---|---|
| B: baked JRE, its own archive only | -7% | -9 MB | -17 MB | kept (the base) |
| Bz: B with `lib/modules` compressed (zip-6) | -7% | -6 MB | -17 MB | same speed; not kept, see Disk |
| C0: B + framework archive from the start's own classes | -20% | -9 MB | -39 MB | |
| C: B + framework archive from all ten apps' classes (45 MB) | -20% | +2 MB | -28 MB | the shape kept |
| C, archive checked whole (`VerifySharedSpaces`) at every start | -18% (JVM +30 ms) | +2 MB | -28 MB | only on first use |
| C + `-Xms32m` | -18% | +2 MB | -27 MB | no |
| C + `-XX:-UsePerfData` | -20%, JVM 6 ms sooner | +2 MB | -28 MB | **kept** |
| C + G1 instead of Serial | -20% | +8 MB | **+47 MB** | no |
| C + `-XX:ArchiveRelocationMode=0` | -20% | **-10 MB** | -28 MB | no: see below |
| C + C2 (`TieredStopAtLevel=4`) | **+9%** | +54 MB | +26 MB | no: C1 only stays |

The class list. Run [42526208921](https://github.com/israel2176/win.apk/actions/runs/42526208921)
compared, against main: the union of the ten apps' lists (6,425 classes, 45 MB
archive) -19%; each app's own list alone (35 MB) -19%; and an archive made from
one simple app's classes (the editor's) used by all the others -18%. So which
classes the archive holds changes a warm start by 1-2%, but every archived
class costs memory whether an app loads it or not: HotSpot relocates the whole
archive at each start (ASLR), which makes its pages the process's own, about
1 MB of private memory per MB of archive (the union cost about 10 MB more per
running app than an app's own list). The shipped list is therefore the classes
at least three of the ten apps load (5,118, which cover 98.6% of the median
app's), merged on the user's machine with the first app's own; the final
archive is 39 MB.

`ArchiveRelocationMode=0` maps the archive at a fixed address instead and saves
about 11 MB of working set per app at the same speed, but gives up address
randomisation for the JVM's class metadata, which is why the JDK relocates by
default; not taken.

Disk. `--compress=zip-6` takes the JRE from 122 to 73 MB at no measurable cost
to a start (the archives hold what a start loads), but the installer's LZMA
then has little left to compress: the installer came out 146 MB against 0.8.0's
133 MB, while LZMA over the uncompressed JRE gives about 21 MB less than over
the compressed one (xz, measured locally). The JRE ships uncompressed;
`WINAPK_JRE_COMPRESS=zip-6` is there for a build that wants the disk instead.
The JRE also loses the second CDS archive (`classes_nocoops.jsa`, for heaps over
32 GB; the runtime uses 1 GB), the JDK's tools and their modules, JFR, JVMCI,
the jdk.* agents and the incubator modules: 146 → 122 MB.

## Safety

- Nothing here can stop a start. HotSpot validates an archive's header, the
  JRE and the class path jars (size and time) before mapping it, and otherwise
  starts without it (`-Xshare:auto`), with a `[cds]` warning. Measured on Linux
  and Wine: a missing, empty, truncated or random file, a changed jar, and a
  jar list that differs all start normally without the archive.
- A file damaged in its body (bytes flipped mid-file) passes those checks and
  **crashes** the JVM on use. So a new archive is mapped the first time with
  `-XX:+VerifySharedSpaces` (a CRC of the whole file; the damaged file is then
  refused with "Checksum verification failed" and the start goes on), and only
  after it has mapped whole does `framework-<key>.ok` spare later starts the
  check (30 ms). Any crash while the archive is in use deletes `.ok`, so the
  next start checks the file again.
- A start whose archive did not map logs why, deletes it, counts the failure in
  `.fails`, and the next first start makes a new one; after two failures in a
  row for one key it is not made again (`WINAPK_CDS=base` behaviour), and a
  start that maps clears the count.
- The key (FNV-1a) covers the JRE's version, `lib\modules` and `classes.jsa`,
  the three framework jars and the shipped list (path, size, time: any new
  Win.apk changes them), and every JVM option that is not per launch
  (`MACAPK_JVM_OPTS` included). A new key dumps a new archive and deletes the
  old ones once it is in place.
- The dump runs in a process of its own (`winapk-runtime --cds-dump`), one at
  a time per user (a named mutex), into a temporary file renamed into place
  only after `java -Xshare:dump` succeeded, so an app closed early, a second
  app starting, or a kill leaves no half archive.
- The patch digest guards the JRE: a `java_base_patch` rebuilt after the JRE
  was baked no longer matches, the runtime keeps `--patch-module` for it and
  runs that start without CDS (`cds: off: ... not the one baked into the Java
  runtime`), instead of silently using stale classes. A plain JDK
  (`--jdk $JAVA_HOME`) has no record and runs as 0.8 did.
- The app's own classes are never archived. The earlier research (branch
  `claude/issue-70-startup`) found that dumping an archive that holds app
  classes (a per-app dynamic archive, or JDK 25's AOT cache) makes HotSpot
  verify them, and dex-converted code often fails verification: 4 of the 10
  smoke apps threw `VerifyError` and never drew, and the recording start's JVM
  took 5 s longer to create on Windows. The per-app archive also gained nothing
  over the shared one, because HotSpot still reads and CRC-checks such classes
  through the app's class loader.
- `WINAPK_CDS=0` restores the old flags exactly.

Tests: `winapk-cds-test` (the digest against `bake_jre.sh`'s own recipe, the
flags dropped and kept, the key, the class list merge); CI's runtime tests
start Sudoku twice on the staged JRE (the first records and dumps, the second
maps the archive); the smoke test and the sweep run on the staged JRE with one
archive made first (`--cds-prepare`); Wine runs covered the damaged, truncated
and mismatched cases above.

## Measuring again

`windows/test/compat/startbench.py` takes configurations as
`NAME=<jre>[,VAR=value...][,@runtime=<staged runtime>][,@wait=1]` and writes
`report.md`/`report.json`; the one-off workflow that ran it is in the history
of this branch (`.github/workflows/startup-bench.yml`, removed). The smoke
test's `framework-classlist` artifact is the class list to ship again when the
framework or the smoke set changes much (`merge_classlists.py --min-apps 3`).
