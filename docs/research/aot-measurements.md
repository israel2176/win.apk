# AOT translation: measurements on four apps, and the settings they point to

Follows [`background-translation.md`](background-translation.md) (issue
israel2176/win.apk#78). That document set a design from other projects and
from one library; this one measures it on four real apps, against the four
goals together:

1. the apps run fast,
2. the PC's memory stays free,
3. little disk is used,
4. no work is done twice.

Everything here was measured on Linux, a 4-vCPU cloud VM with 15 GB of RAM,
using clang 20.1 and `winapk-aot` from `main`. Some tools are patched copies,
and the scripts and patches are in [`aot-measurements/`](aot-measurements/).
Running real apps on Windows (part B, section 8) is still to do.

Contents:

1. [Summary: the settings](#1-summary-the-settings)
2. [The apps](#2-the-apps)
3. [Finding the code: `.eh_frame`](#3-finding-the-code-eh_frame)
4. [Speed of the translated code](#4-speed-of-the-translated-code)
5. [Compile cost: optimization level, part size, optnone](#5-compile-cost-optimization-level-part-size-optnone)
6. [Work done twice: profile growth and deltas](#6-work-done-twice-profile-growth-and-deltas)
7. [Each app in total](#7-each-app-in-total)
8. [What still needs Windows](#8-what-still-needs-windows)

## 1. Summary: the settings

| Setting | Today | Recommended | Goal it serves | Evidence |
|---|---|---|---|---|
| Function discovery | Symbols + BL targets | **Also every function start in `.eh_frame_hdr`** | Speed | Coverage of stripped libraries rises from 1-45% to 93-97% (section 3). A call through a pointer table in a stripped library ran 13× faster |
| Optimization level | `-O1` | **`-O2 -mllvm -fast-isel`** | Disk, CPU | It compiles 8-31% faster than `-O1`, its code is 22-45% smaller, and the code is just as fast (sections 4, 5). Check it with the bundled clang on Windows first (section 8) |
| Part size | `total / jobs` (80k-175k for Waze) | **20,000 instructions, fixed**; 10,000 when free RAM is short | Memory | Throughput is flat from 10k to 80k instructions, but peak memory per clang goes from 270-410 MB at 20k to 750-1250 MB at 80k (section 5) |
| Memory per clang job (governor) | not considered | **512 MB** | Memory | Measured peaks of 245-410 MB at 20k |
| `WINAPK_AOT_OPTNONE_INSNS` | 4000 | **4000** (unchanged) | Speed, disk | 1000 saves 5-18% compile time but adds 5-74% code and leaves more code unoptimized. 16000 costs more time for little gain |
| Whole-library threshold | 400k | **3M instructions** | All four | Organic Maps (2.6M) and Saber (2.5M) are translated whole, once: 34 and 53 CPU-minutes, 94 and 136 MB (section 7) |
| Above the threshold | Retranslate all that ran at +10% | **Deltas of new functions only**, merged into one DLL at idle | No repeated work | Re-cutting parts recompiles 55-100% of each pass. A delta compiles 9-20% (section 6) |
| Calls between DLLs | `aot_rt.branch` → `unordered_map` | **A call-site cache in `CALL_SITE`** and **a direct-mapped table in front of `aot_lookup`** | Speed | Without them, a direct call into another DLL is about 15× slower than inside one. The site cache gives 2.7× and the table 1.9× (section 4) |
| Planning rate | 60k instructions per job-minute | **35k-80k by kind of code**; plan with 45k | Honest estimates | Flutter's engine is the slowest at 36-47k and VLC the fastest at 72-80k (section 5) |

## 2. The apps

Four F-Droid apps, chosen to cover the kinds of native code in issue 78's
table. All four use their arm64-v8a builds.

| App | Version | Kind | arm64 native code (`.text` / 4) | Libraries |
|---|---|---|---|---|
| Organic Maps | 2026.09.29-32 | A large C++ map engine: the closest to Waze | 2.75M | `liborganicmaps.so` |
| VLC | 3.7.2 | Media: ffmpeg, many codecs, hand-written NEON | 8.4M | `libvlc.so` 7.4M, `libmla.so`, `libc++_shared.so` |
| Saber | 1.37.1 | A Flutter app | 4.9M | `libapp.so` 2.4M (Dart AOT), `libflutter.so` 1.6M, `libpdfium.so` 0.9M |
| Fennec | 159.0.0 | A browser (Gecko) | 30M | `libxul.so` 27.3M, and 14 others |

Organic Maps and Fennec also ship x86_64 builds, which Win.apk runs with no
translation at all. The arm64 builds are measured here as stand-ins for apps
that ship only arm64.

## 3. Finding the code: `.eh_frame`

`winapk-aot` finds functions from the symbol tables and then, transitively,
from the targets of BL. Release builds of Android libraries are stripped, so in
practice it finds only what the exported symbols reach:

| Library | Code (instructions) | Found today | Functions today | Functions in `.eh_frame_hdr` | Found with `.eh_frame_hdr` |
|---|---|---|---|---|---|
| liborganicmaps.so | 2.75M | 2.07M (75%) | 11,810 | 26,367 | **2.58M (94%)** |
| libvlc.so | 7.45M | 3.37M (45%) | 30,553 | 57,070 | **7.21M (97%)** |
| libpdfium.so | 0.91M | 0.64M (71%) | 4,600 | 9,111 | **0.87M (96%)** |
| libflutter.so | 1.59M | 0.20M (13%) | 3,631 | 44,954 | **1.48M (93%)** |
| libxul.so | 27.3M | 0.37M (1.3%) | 2,202 | 274,458 | **25.5M (93%)** |
| libapp.so (Dart) | 2.38M | 0 | 0 | none (no `.eh_frame_hdr`) | 0 |

**Why it is there.** `.eh_frame` holds the unwind information that C++
exceptions and Android's crash unwinder need. `.eh_frame_hdr` is its sorted
search table: one entry per function, with its start address. It is in a
loaded segment (`PT_GNU_EH_FRAME`), so it survives stripping.

**Patch and check.**

- The patch (`WINAPK_AOT_EH=1` in `winapk-aot-measure.patch`) adds each entry
  as a function start, about 30 lines of code.
- Checked on the benchmark library, stripped (`llvm-strip --strip-all`):
  - Every result matched native.
  - The benchmark that calls through a pointer table went from 6.42 s
    (its callees on the interpreter) to 0.50 s (translated), against 0.07 s
    native.
- On Windows, the callees would run on the JIT instead (3-6× native rather
  than the interpreter's 80×), so the gain there is smaller but real, and it
  applies to most of the code of every stripped library.

**What remains on the JIT.**

- Flutter's `libapp.so` is the app's Dart code, compiled ahead of time by Dart
  into one blob with no functions in any table. Only the profile can find
  entry points there, which is the next thing to try (section 8).
- Dart's own calling convention is not a problem for `winapk-aot`, which does
  not assume the AAPCS64 (`arm64_aot.h`, "Calls").

## 4. Speed of the translated code

**How it was measured.** `aotbench` runs the project's own benchmarks
(`windows/test/guest/arm64_bench.c`, built for arm64), with a direct-call
benchmark added. It runs them through `winapk-aot`'s translation, compiled
with each setting, and through the interpreter for the code not translated.
Times are ratios to the same C compiled natively; lower is better. Each value
is the median of 5 runs. The VM is shared, so values move by about ±20%; only
large differences count.

| clang setting | compute | float | indirect | memory | calls |
|---|---|---|---|---|---|
| `-O0` | 10.9 | 10.9 | 15.7 | 6.5 | 17.2 |
| `-O1` (today) | 1.25 | 1.19 | 7.10 | 0.63 | 0.77 |
| `-O1 -mllvm -fast-isel` | 0.99 | 1.31 | 7.32 | 0.70 | 0.87 |
| `-O2` | 1.00 | 1.50 | 7.16 | 1.02 | 0.79 |
| `-O2 -mllvm -fast-isel` | 1.05 | 1.13 | 6.76 | n/a | 0.72 |
| `-O3` | 1.01 | 1.14 | 7.36 | 1.19 | 0.58 |

**What the table shows.**

- `-O0` is about 11× slower and is never worth it.
- `-O1`, `-O2`, `-O3` and fast-isel are within the noise of each other.
- Calls through a pointer ("indirect": C++ virtual calls, function tables)
  cost about 7× native in every setting. Every such call goes through
  `aot_rt.branch` and `aot_lookup`.

**Calls between DLLs** (seconds for the same work; native: calls 0.059,
indirect 0.069, compute 0.040). The second DLL holds only the two callees,
as a delta translation would:

| Layout | calls | indirect | compute |
|---|---|---|---|
| One DLL | 0.048 | 0.585 | 0.060 |
| Base + delta (today's code) | **0.813** | 0.525 | 0.043 |
| Base + delta, with the call-site cache | 0.300 | 0.633 | 0.044 |
| One DLL, with the direct-mapped table in `lookup` | 0.050 | **0.306** | 0.048 |
| Base + delta, with the direct-mapped table | 0.427 | 0.323 | 0.043 |

**Reading it.**

- **A direct call that crosses into another DLL costs about 15× one inside a
  DLL.** Inside a DLL, clang calls the callee directly and can inline it.
  Across DLLs, each call is `aot_rt.branch` plus a hash lookup.
- **The call-site cache helps 2.7×.** It is in `winapk-aot-site-cache.patch`:
  each `CALL_SITE` remembers its first translated target, written once and
  published with release/acquire. Calls through it still cannot be inlined,
  so a hot path should still end up in one DLL, through the merge.
- **The direct-mapped table helps 1.9×** on every call through a pointer, for
  every app, delta or not. It is 4096 slots in front of the `unordered_map`,
  filled on hits. In the runtime it belongs in `aot_lookup`
  (`windows/src/arm64/arm64_aot.cpp`).

## 5. Compile cost: optimization level, part size, optnone

**How it was measured.**

- For each of four libraries, a fixed random sample of functions of about
  80,000 instructions, with the library's own mix of sizes.
- Emitted with `WINAPK_AOT_ONLY` and compiled one clang at a time with
  `--target=x86_64-w64-windows-gnu -ffreestanding -march=native
  -fno-strict-aliasing -w`.
- Columns: CPU time (user plus system) per instruction, the peak memory of the
  largest clang, and the size of the code clang produced (`.text`, `.rdata`,
  `.data`, `.xdata`, `.pdata`) per arm64 instruction.

Instructions per CPU-minute (higher is better):

| Setting | organicmaps | vlc | flutter | xul |
|---|---|---|---|---|
| 10k parts, `-O1` | 64k | 72k | 36k | 56k |
| 20k parts, `-O1` (today's flags) | 71k | 73k | 36k | 59k |
| 40k parts, `-O1` | 69k | 72k | 35k | 57k |
| 80k parts, `-O1` | 68k | 68k | 34k | 55k |
| 20k, `-O1 -mllvm -fast-isel` | 83k | 77k | 40k | 67k |
| 20k, `-O0` | 315k | 290k | 203k | 280k |
| 20k, `-O2` | 71k | 73k | 44k | 63k |
| **20k, `-O2 -mllvm -fast-isel`** | **77k** | **80k** | **47k** | **67k** |
| 20k, `-O1`, optnone above 1000 | 75k | 86k | 41k | 69k |
| 20k, `-O1`, optnone above 16000 | 71k | 68k | 34k | 59k |

Peak memory of one clang, MB:

| Part size (`-O1`) | organicmaps | vlc | flutter | xul |
|---|---|---|---|---|
| 10k | 245 | 332 | 370 | 272 |
| 20k | 273 | 389 | 408 | 338 |
| 40k | 438 | 569 | 672 | 526 |
| 80k | 754 | 823 | 1,250 | 945 |

Bytes of x86-64 code and data per arm64 instruction:

| Setting | organicmaps | vlc | flutter | xul |
|---|---|---|---|---|
| `-O0` | 154 | 170 | 245 | 175 |
| `-O1` | 50.5 | 47.0 | 98.6 | 61.8 |
| `-O1 -mllvm -fast-isel` | 51.6 | 49.6 | 100.3 | 63.4 |
| `-O2` | 34.7 | 33.8 | 50.2 | 39.2 |
| **`-O2 -mllvm -fast-isel`** | **36.3** | **36.5** | **54.7** | **41.2** |
| `-O1`, optnone above 1000 | 52.8 | 81.6 | 107.2 | 72.4 |

**Reading it.**

- **Part size changes memory, not speed.** Throughput is within ±5% from 10k
  to 80k, but each clang's peak memory roughly triples. 20k keeps one clang
  under about 410 MB on every library. 10k saves 9-20% more memory for the
  same time, at the cost of twice as many files, so it is the choice when RAM
  is short.
- **`-O2` is not slower to compile than `-O1` with clang 20, and its code is
  28-49% smaller.** `-O2` removes much of the register traffic the emitter
  writes around calls and the interpreter, which `-O1` leaves. On Flutter's
  engine it was even 20% faster to compile.
  - The comment in `runtime_main.cpp` ("-O1, a quarter faster than -O2") was
    measured with an older clang. Repeat this with the bundled clang before
    switching (section 8).
- **fast-isel** (`-mllvm -fast-isel`: LLVM's fast instruction selector, falling
  back to the full one where it cannot) cuts compile time by 5-17% for 2-9%
  more code, with no change in speed. That is worth it.
- **`-O0`** compiles 4-6× faster but makes code 3× larger and 11× slower. It
  is not a useful tier: the JIT is already faster than that.
- **The code mix matters 2×.** Flutter's engine is the slowest to compile
  (more interpreter fallbacks and SIMD), and VLC the fastest. Earlier figures
  with clang 18 on Debian's libx265 were faster still (about 170k), so
  estimates should be made per library from its own sample, or with 45k as a
  conservative default.
- **The optnone threshold stays at 4000.** At 1000 it compiles faster, but
  that is mostly from leaving more code unoptimized: VLC's code grows by 74%,
  and those functions run at `-O0` speed.

## 6. Work done twice: profile growth and deltas

**How it was measured.**

- The profile is modelled as a growing set of `liborganicmaps.so`'s functions
  (27,060 found with `.eh_frame_hdr`). The first pass covers 30% of the
  instructions, then the profile grows by 10% and then by a further 25%.
- Two orders of growth: random, and clustered (new functions in address runs
  of 1-40, as a feature's code sits together).
- Each snapshot is emitted for real, so a function's C is exactly what
  `winapk-aot` writes. That C depends on which callees are translated: a direct
  call or `aot_rt.branch`.
- A part is not compiled again when its content hash already exists (the
  content-addressed parts of the design).

Share of the new pass's instructions that must be compiled:

| Growth | Old functions whose C changed | Balanced parts (today) | Fixed size, address order | Fixed address windows | Delta (new functions only) |
|---|---|---|---|---|---|
| random, +10% | 11.7% | 100% | 100% | 100% | **9%** |
| random, +25% | 29.5% | 100% | 100% | 100% | **20%** |
| clustered, +10% | 1.3% | 100% | 100% | 55% | **9%** |
| clustered, +25% | 25.3% | 100% | 100% | 96% | **20%** |

**Reading it.**

- Content-addressed parts do not save work across profile growth. New
  functions land in most parts, and callers' C changes when a callee becomes
  translated. They still matter for **resuming an interrupted pass**, which
  keeps its plan and so its parts.
- **Only deltas avoid the repeat.** They need the call-site cache and the
  table of section 4. They also need a **merge into one DLL** so that hot
  calls end up inside one DLL again:
  - when the deltas hold 25% or more of the base's instructions, or there are
    4 of them;
  - only at idle, on AC power.

  Each merge is one full pass over the current profile. With merges at every
  25% of growth, the total work is at most 1/(1 - 0.8) = **5× one final
  pass**, against about 11× for today's retranslation at every 10%. In
  practice it is less, because a profile stops growing after a few sessions.
- **For libraries under the whole threshold none of this applies.** They are
  translated whole once, so there is nothing to repeat.

## 7. Each app in total

At the recommended flags (`-O2 -mllvm -fast-isel`, 20k parts), from the rates
and sizes of section 5, for all the code `winapk-aot` finds with `.eh_frame_hdr`:

| App | Instructions to translate | CPU time | Wall time, idle PC (4 jobs) | Wall time, user active (1 job) | Disk (DLLs) | Disk at today's `-O1` | Whole or profile |
|---|---|---|---|---|---|---|---|
| Organic Maps | 2.58M | 34 min | ~9 min | ~34 min | 94 MB | 130 MB | Whole (under 3M) |
| Saber | 2.48M (+2.38M Dart, JIT only) | ~53 min | ~13 min | ~53 min | 136 MB | 245 MB | Whole, library by library |
| VLC | 8.06M | 101 min | ~25 min | ~1.7 h | 294 MB | 379 MB | `libvlc.so` (7.2M) by profile; the rest whole |
| Fennec | 29.8M | 7.4 h | ~1.9 h | ~7.4 h | 1.23 GB | 1.84 GB | `libxul.so` by profile; the rest whole |

**Peak memory.** With the governor of `background-translation.md` this is 4
jobs × ~0.4 GB = 1.6 GB on an idle PC, and 0.4 GB while the user works. Today
it is 7-15 jobs × 0.75-1.7 GB.

**Peak temporary disk** is the C of the parts being compiled (about 100 bytes
per instruction, 2 MB per part) plus the objects of the pass in progress
(about 40 bytes per instruction). The C of a part can be deleted as soon as its
object exists.

**The wall times assume 4 jobs scale.** On this VM, 4 clangs at once each ran
2.5× slower than one alone (the vCPUs share cores). Real 8-core machines scale
better, but that is to be measured (section 8).

## 8. What still needs Windows

Part B of the plan, in the order that decides settings:

1. **The bundled clang.** Repeat section 5's `-O1`, `-O2`, `-O2 -mllvm
   -fast-isel` rows with Win.apk's `clang\bin\clang.exe` on the same samples
   (`matrix.py` runs on Windows with `clang` on the PATH). Switch
   `kAotCompileFlags` only if `-O2 -mllvm -fast-isel` stays at least as fast
   to compile as `-O1`.
2. **Real speed, per app.**
   - Organic Maps (arm64 build), VLC and Saber on Windows. For each, a fixed
     scenario: Organic Maps panning and zooming a downloaded map for 60 s; VLC
     playing a 1080p H.264 file for 60 s; Saber opening and scrolling a 50-page
     PDF.
   - Each scenario three ways: JIT only, today's translation, and the
     recommended one with `.eh_frame` discovery.
   - Measured by the app's CPU time for the scenario and its frame times.
3. **Scaling of jobs** on an 8-core / 16-thread PC: 1, 2, 4, 8 clangs at
   once on 20k parts, with throughput and the system's memory load. This is
   what the governor's `logical / 2` cap rests on.
4. **The responsiveness test** of `background-translation.md`, section 9,
   with the governor in place.
5. **Profile-seeded discovery for Dart.** Add the profile's addresses as
   function starts in `libapp.so`, and check correctness against the JIT with
   the differential test before measuring speed.
