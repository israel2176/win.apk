# Background arm64 translation: how others do it, and a design for Win.apk

Issue: israel2176/win.apk#78. Status: research and design; nothing here is
implemented yet. Measurements on four apps
([`aot-measurements.md`](aot-measurements.md)) since then change four
settings here:

- compile with `-O2 -mllvm -fast-isel` instead of `-O1`;
- find functions from `.eh_frame_hdr` too;
- the call-site cache for delta DLLs is required, not optional;
- add a direct-mapped table in front of `aot_lookup`.

They also put the planning rate at 35k-80k instructions a CPU-minute by kind
of code, instead of 60k.

Win.apk translates an app's arm64 libraries ahead of time: `winapk-aot` writes
C, the bundled clang compiles it into `<lib>.so.aot.dll`, in a background
`winapk-runtime --aot-translate` process (`windows/host/runtime_main.cpp`,
"arm64 translation, in the background"). On large apps that worker keeps the
CPU and the memory at 100% at every start and never finishes. This document
looks at how comparable translators and runtimes keep such work out of the
user's way, measures what clang actually costs on Win.apk's output, compares
five approaches with numbers, and recommends one, with parameters, the Windows
APIs to use, an implementation plan by function, and how to check it worked.

Contents:

1. [Summary](#1-summary)
2. [What the worker does today](#2-what-the-worker-does-today)
3. [What clang costs: measurements](#3-what-clang-costs-measurements)
4. [How others do it](#4-how-others-do-it)
5. [Comparison of the projects](#5-comparison-of-the-projects)
6. [Options for Win.apk, with estimates](#6-options-for-winapk-with-estimates)
7. [Recommended design](#7-recommended-design)
8. [Implementation plan](#8-implementation-plan)
9. [Measuring success](#9-measuring-success)
10. [Sources](#10-sources)

## 1. Summary

- **The main cause of 100% memory is the part size, not the number of
  instructions.** Today a library is split into `min(jobs, total / 20000)` C
  parts, so a large library on a machine with many cores gets one huge part per
  core. Measured here, clang `-O1` needs about **300 MB and 7 s** for a part of
  20,000 instructions, and **1.7 GB and 113 s** for a part of 176,000. Per
  instruction, the large part is slower and its memory use grows with it. Waze's
  profiled 1.2M instructions on 16 logical cores become 15 parts of 80,000
  instructions each. Running at once, they need on the order of 10-13 GB, which
  matches what was seen, and paging then explains why the observed rate
  (80,000 instructions a minute on 15 cores) is about a tenth of what one core
  does here on small parts.
- **The second cause is repetition.** Each 10% of profile growth translates
  everything again from scratch, and an interrupted pass loses everything.
  Growing a profile to 1.2M instructions in 10% steps adds up to about 11
  times the work of one translation.
- **What every comparable system does:**
  - It keeps finished work in units smaller than the whole job: ART per
    package, Box64 per file, FEX and Prism per module from a record of what
    ran, ccache per object.
  - It writes atomically: Rosetta's `.aot.in_progress` is renamed to `.aot`
    when done.
  - It does heavy work only when the device is idle, cancels it as soon as the
    user returns, and resumes it later: ART's background dexopt, Windows'
    Automatic Maintenance.
  - It caps threads and CPUs explicitly (`dalvik.vm.background-dex2oat-threads`
    and `-cpu-set`, nice 10, low I/O priority), and lets the compiler keep a
    whole job's worth of memory only when it can afford it.
  - It recompiles from a profile only when the profile has changed by a margin
    with an absolute floor: profman uses 2% and 100 methods.
  - It runs code it cannot see statically on the JIT, which Rosetta and
    Win.apk both already do.
- **Recommended for Win.apk:**
  1. Compile in **fixed parts of about 20,000 instructions**, independent of
     the core count.
  2. **Name each part's object by a hash of its content** so that work survives
     interruption and is never done twice.
  3. **Translate a library whole when it has up to 3M instructions**. That
     covers Waze's `libwaze.so` and every library in issue 78's table except
     the largest few. It is done once and shared between apps.
  4. Translate **only profiled code above 3M**, and later **only the newly
     profiled functions, into additional DLLs**. Never translate everything
     that ever ran again.
  5. **Run the compiler under a governor**: in a job object that kills
     everything when the worker goes, at idle priority with EcoQoS. The number
     of clang processes is set every 2 s from free RAM (about 512 MB per job,
     keeping memory load under 80%), from the other processes' CPU use (total
     under 80%), and from whether the user is active. That means one job while
     the user is active, up to half the logical cores when idle, and none on
     battery saver, in a full-screen app, or in the app's first minute.
- **Expected result for Waze**, about 32 CPU-minutes once, spread so it is
  never felt:
  - It finishes in about 8 minutes if the PC is left alone, or about half an
    hour on one throttled core while the user works.
  - Its peak memory is 0.5-2 GB instead of over 10 GB.
  - Nothing runs after that.
- **For a 44M-instruction browser:**
  - The first profiled pass is about 50 CPU-minutes, and later passes take
    seconds.
  - A whole translation would cost about 12 CPU-hours and roughly 1.9 GB of
    DLL, so it is not done.

## 2. What the worker does today

From `windows/host/runtime_main.cpp` and `windows/tools/arm64_aot_main.cpp` on
`main`:

- **When it starts.** `prepare_arm64` starts the worker at each app start when
  `any_aot_work`. `aot_watch_thread` then flushes the JIT's profile every 2 s
  for the first 30 s and every 30 s after that, and starts the worker again
  whenever a flush grew a profile and some library needs work.
- **When a library needs work** (`aot_needs_work`):
  - There is no DLL yet.
  - The stamp's `kAotVersion` changed.
  - A profile-based translation's profile grew by `max(200 lines, 10%)`.
  - A `wait` stamp (too large with no profile, or failed) is retried on the same
    growth rule, so a failure has no backoff.
- **The unit is the library.** `aot_translate_one`:
  1. Deletes `.aot-tmp`.
  2. Runs `winapk-aot` on the whole library (or on its profiled functions above
     `kAotFullBelow` = 400,000 instructions).
  3. Compiles the parts with `run_parallel` on `hardware_concurrency() - 1`
     jobs.
  4. Links one DLL and moves it in place.

  Nothing in between is kept.
- **How parts are formed.** `winapk-aot` makes `min(parts, (total + 19999) /
  20000)` parts, balanced by instruction count, largest functions first. With
  `parts = jobs`, a part holds `total / jobs` instructions: the fewer the
  cores, or the larger the library, the larger every part.
- **Processes.**
  - The worker sets `BELOW_NORMAL_PRIORITY_CLASS`. Clang inherits it from
    `run_parallel`.
  - `winapk-aot` and the linker run through `run_process` at normal priority.
  - No job object: killing the worker leaves its children running.
- **Profile.**
  - `<lib>.so.aot.profile` is a header line followed by one library vaddr per
    line, for each block the JIT compiled, in the order first seen. Lines are
    only ever appended. There are no counts.
  - `winapk-aot` keeps a function when any profiled address lies in it.
- **Loading.**
  - `aot_load` loads `<lib>.so.aot.dll`. `aot_load_pending` loads one finished
    while the app runs, but only for a library with no DLL yet (`translated`
    is one flag per library).
  - Each load merges the DLL's entries into one copy-on-write
    `unordered_map` (`g_functions`).
- **Calls between translated functions.**
  - A call to a function in the same translation is a direct C call.
  - A call to anything else goes through `aot_rt.branch` → `aot_lookup`, which
    finds a translated function in any loaded DLL. A second DLL for the same
    library therefore needs no change to the emitted code, only to the loader.
- **The shared cache.** Only whole translations are kept in `<data>\aot-cache`,
  keyed by the library's text hash, the tools and the flags, within 2 GB.

From issue 78:

- One profile translation of `libwaze.so` took 473.5 s.
- About 80,000 instructions a minute on all cores but one.
- Waze's main library has about 1.2M instructions in the 26.6k of its 38.3k
  functions that ran. That is about 45 instructions a function, so the whole
  library is about 1.7M instructions of function code, consistent with the
  1.5M given in the source comments.
- Apps range from 1.6M to 44.6M instructions of native code (upper bounds).

## 3. What clang costs: measurements

`winapk-aot` builds and runs on Linux, so its output for real arm64 libraries
can be compiled with the same flags (`-O1 -march=native -fno-strict-aliasing
-w`, clang 18, `--target=x86_64-w64-windows-gnu -ffreestanding`, `-c` only).
The libraries were Debian's arm64 `libsqlite3.so` (178k instructions in 2,864
functions) and `libx265.so` (1.41M instructions in 6,009 functions, close to
Waze's profiled 1.2M). The machine was a 4-vCPU cloud VM with 15 GB of RAM,
and each part was compiled alone except where noted.

| Part | C source | clang time | Peak memory (RSS) | Instructions per core-minute |
|---|---|---|---|---|
| x265, 70 parts of ~20k instructions | 2.2 MB | 6.6-7.6 s | 280-340 MB | ~170k |
| sqlite, 8 parts of ~22k | 2.3 MB | 14-24 s | 260-320 MB | 55-95k |
| x265, 32 parts of ~44k | 4.8 MB | 17.5 s | 546 MB | ~150k |
| x265, 8 parts of ~176k | 18.4 MB | 113 s | 1,722 MB | ~93k |
| x265, 4 parts of ~20k at once on 4 vCPUs | 4 × 2.2 MB | 17.6 s (wall) | 4 × ~300 MB | ~270k in all |

Other figures:

- `winapk-aot` itself takes 0.4 s for sqlite and 3.5 s for x265 (the whole
  library, all parts).
- The C is about 100 bytes and the object about 43 bytes per instruction.
- Run 4 at once, parts took 2.5 times as long as one alone. These vCPUs share
  cores; real hardware should scale better.

What follows from this:

- **Memory per clang job is roughly 250 MB plus 8-9 MB per 1,000
  instructions.** At 20k it is about 0.3 GB, and at 176k about 1.7 GB. Today's
  part size (`total / jobs`) on a 16-thread machine with Waze's 1.2M gives
  80k-instruction parts at about 0.9 GB each, 15 at once: about 13 GB of clang,
  plus the app and its JIT. With 8 threads it is 7 parts of 170k at 1.7 GB:
  about 12 GB. Either way, a 16 GB PC is out of memory, which is what the
  issue reports.
- **Big parts are also slower per instruction** (93k against 150-170k a core-minute).
  Clang's per-function passes are linear, but the module-level work and memory
  traffic are not.
- **Code mix matters about 3×.** sqlite's parts were slower than x265's at
  the same size: fewer, larger functions, more branches. Estimates below use
  **60,000 instructions per job-minute**, below everything measured except the
  worst sqlite part. That leaves room for E-cores and EcoQoS clocks (section
  4.7), which can halve a core's speed.
- **The observed 80k a minute on 15 cores is about 5k per core.** That is
  20-30 times below the measurements here, consistent with heavy paging. Small
  parts, with jobs limited by free RAM, should by themselves make the work
  several times faster *and* lighter.

## 4. How others do it

### 4.1 Android ART: profile-guided `dex2oat` in idle maintenance

ART is the closest analogue: a JIT that records a profile, and a heavy AOT
compiler that later compiles what the profile lists.

- **When it runs.**
  - Android 5-6 compiled everything at install, with "long wait times" and
    more RAM and disk.
  - Android 7 moved to JIT plus profile. Precompilation happens "only when the
    device is idle and charging", and "large apps that required several minutes
    to optimize and install … can now install in just a matter of seconds"
    ([Android 7.0 notes][a7]).
  - At install, AOSP's default filter is `speed-profile`. With no profile (no
    cloud or baseline profile shipped with the APK) that is the same as
    `verify`: no AOT compilation at all ([package manager config][apm];
    [baseline profiles][abp]).
- **Background dexopt** (Android 14+, ART Service, `BackgroundDexoptJob.java`):
  - It runs once a day and requires the device to be idle and charging, with
    the battery not low.
  - It "is cancelled immediately if the device is no longer idle", and is
    retried with a back-off: "30 seconds, exponential, capped at 5 hours".
  - It is also cancelled at `THERMAL_STATUS_MODERATE` ([ART Service
    config][aas]; [source][abdj]).
  - The older post-boot job was dropped because it "sometimes competed for
    resources with the foreground application" ([ART Service config][aas]).
- **Interruption and resume.**
  - Cancelling kills the whole `dex2oat` process group (`SIGKILL`). The current
    package's work is lost.
  - Every package already compiled for its current profile is skipped next
    time (`getDexoptNeeded`, `DEXOPT_SKIPPED`), so in effect it resumes package
    by package ([Dexopter.java][adx]).
  - Each `dex2oat` is limited to 570 s.
- **Throttling.**
  - Background compiles use their own thread count and CPU set
    (`dalvik.vm.background-dex2oat-threads`, `-cpu-set`) and run at
    `ANDROID_PRIORITY_BACKGROUND` (nice 10).
  - They use the task profile `Dex2OatBootComplete`: a `dex2oat` cpu cgroup,
    low I/O priority, and 40 ms timer slack ([configure][acfg];
    [task_profiles.json][atp]).
  - `pm.dexopt.<reason>.concurrency` (default 1) sets how many `dex2oat`
    processes run at once.
  - Memory: `dalvik.vm.dex2oat-Xmx` "should not be reduced". Instead there is
    a swap file for the compiler's own data, and on low-RAM devices
    `--compile-individually`, which "takes longer but reduces the memory
    footprint" ([configure][acfg]).
- **When to recompile.**
  - profman merges the run's profile into the reference profile, and asks for a
    recompile only if the new methods or the new classes exceed
    `max(2% of before, floor)`. The floor is 100 methods or 50 classes
    (`kMinNewMethodsPercentChangeForCompilation = 2` in
    [profile_assistant.cc][apa]).
  - Devices can raise the percentage with `dalvik.vm.bgdexopt.new-methods-percent`;
    the configuration page says 20.
- **Profiles are lossy by design.** ART makes "no attempt to ensure everything
  is recorded (as this can impact runtime performance)" ([JIT][ajit]).
- **Unused apps.** When storage runs low, unused apps are downgraded to `verify`
  ([ART Service config][aas]).

**For Win.apk:**

- Compile when idle and cancel as soon as the user returns, since resuming is
  cheap.
- Use a separate small thread budget for background work, at background CPU
  and I/O priority.
- Recompile only past a margin with an absolute floor.
- Make the unit of work small enough that cancelling loses little. ART's unit
  is a package, and a whole large app is one dex2oat run, which is why it needs
  the 570 s limit.

### 4.2 Apple Rosetta 2: whole-image AOT at install, JIT for the rest

- **When it translates.**
  - "Rosetta sets to work right from the moment your application is being
    installed … Rosetta will start translating all the executable code in your
    application." With no installer, "you may see an extra bounce or two in the
    dock the first time it's launched."
  - Translations are "tied to a single machine, securely stored, and get
    refreshed over OS updates." Code not translated at install "we'll compile
    on the fly" ([WWDC 2020, session 10686][rw]).
- **The unit is the whole text segment of each binary**, translated up front.
  The JIT is "used relatively rarely", for what cannot be found statically
  (computed branches, generated code) ([Dougall Johnson][rdj];
  [Apple][rabout]).
- **Cache and atomicity.**
  - `oahd` keys an AOT file by "the SHA-256 hash from the file path and the file
    contents".
  - `oahd-helper` writes `.aot.in_progress` and renames it to `.aot` when done,
    so no process ever maps a partial file. The result is an ordinary Mach-O
    file mapped as executable.
  - System libraries come precompiled in one shared AOT cache
    ([Project Champollion, part 2][rch2]).
- **Cost.** "in extremes of use OAH can consume a lot of memory and CPU"
  ([Eclectic Light][rel]). No source was found for the priority or QoS class of
  `oahd`.

**For Win.apk:**

- Whole translation once is fine when the binary is of a size a translator can
  finish in minutes. Rosetta's x86→arm translator is a fast, purpose-built
  code generator, not an optimizing C compiler, so its "whole" is far cheaper
  per instruction than Win.apk's clang.
- Key the cache by content.
- Write to a temporary name and rename.
- Leave what cannot be seen statically to the JIT.

### 4.3 Windows on Arm (XTA, Prism): a record of what ran, compiled for next time

- **What Microsoft documents.** The emulator JIT-compiles blocks. "A service
  caches these translated blocks of code to reduce the overhead of instruction
  translation and allow for optimization when the code runs again. The caches
  are produced for each module so that other apps can make use of them on first
  launch" ([How emulation works on Arm][wemu]).
- **What reverse engineering found** (Windows 10 XTA, [Black Hat EU 2020][wbh];
  [XtaTools][wxt]):
  - `xtajit.dll` is the emulator, `XtaCache.exe` is the service, and
    `xtac.exe` is the offline compiler.
  - The app reports its modules to the service. `xtac.exe` "updates an XTA
    cache file during/after emulation to add newly translated blocks of code".
  - The file name is `NAME.<hash of content>.<hash of path>.mp.<N>.jc`. Each
    update copies the previous file into `N+1` and the old one is removed
    later, an append-only, versioned cache.
  - The files are in `%SystemRoot%\XtaCache`.
- **Not documented:** how the cache works under Prism (Windows 11 24H2), the
  service's priority, and the size limit.
- **Arm64EC** is the "ship it precompiled" path. Microsoft recommends
  recompiling "the most CPU-intensive parts" first ([Arm64EC][warm64ec]).

**For Win.apk:**

- This is Win.apk's model already: a per-module cache shared between apps, fed
  by a record of what ran, compiled by a separate process.
- The difference is that XTA only ever *adds* blocks (`mp.N` → `mp.N+1`).
  Win.apk recompiles everything that ever ran.

### 4.4 Box64: DynaCache

- **Since v0.3.8 (October 2025)**, Box64 writes the dynarec's output to disk.
  Since v0.4.4 it is on by default, compressed, "up to 2GB of disk space"
  ([releases][bx]; [USAGE.md][bxu]).
- **The unit is one file per mapped binary and set of options.** Shared
  libraries are reused by other programs. Nothing is written below 30 KB of
  code (`BOX64_DYNACACHE_MIN`). Pruning drops invalid files first, then the
  oldest.
- **Validation.** Three version hashes (the file format, the dynarec sources,
  the arch) must match exactly, plus the CPU extensions and settings. The
  original binary is checked only by path and size ([dynacache.c][bxsrc]).
- **When it writes.** At unmap and at exit, in the process itself. There is no
  background compiler: the cost is the JIT's own, saved for later runs.
- **Measured.** Factorio took 39 s to start without the cache and 29 s with it
  ([box86.org][bxblog]).

**For Win.apk:** a size floor below which caching is not worth it, LRU pruning
within a fixed budget, and keys that include the translator's version. Win.apk's
`aot-cache` does all three already, except the floor.

### 4.5 FEX-Emu: code maps and an offline compiler

- **The old way.** FEX's first persistent cache (AOT IR, `--aotir-generate`)
  was removed in 2024-2025.
- **The new way** ([FEX-2512][fx2512]; `CodeCache.h`,
  `FEXServer/ProcessPipe.cpp` in [FEX][fxgh]):
  - Running processes write **code maps**: `{file, block offset}` for each
    block that ran.
  - The **FEXServer** merges them. For each binary whose code map is newer than
    its cache, it starts `FEXOfflineCompiler generate <codemap>`.
- **A JIT output cache** ([FEX-2609][fx2609]): in September 2026 FEX added a
  DiskCache for whatever its JIT generates, in Fossilize's FOZ database format,
  keyed by a hash of the guest code (plus file ID and build-id). It started with
  no size limit; pruning was added afterwards.
- **No lowering of priority** of the offline compiler was found in the source.
- **The JIT is primary.** The offline compiler builds only blocks a run has
  recorded. "Multiblock" compilation "can cause long JIT compilation times and
  stutter", which is the same trade-off as Win.apk's large parts.

**For Win.apk:**

- One coordinator compiles from a merged record of what ran: the same design as
  Win.apk's worker, with the same open problem of priority.
- Keying by a hash of guest code makes entries independent of layout.

### 4.6 QEMU, HQEMU, Berberis: tiering, and where the slow compiler runs

- **QEMU.** Upstream QEMU has no persistent cache: "it is rare to find
  workloads where code generation is a bottleneck", unless an optimizing tier
  makes translation expensive ([qemu-devel, 2018][qemu]).
  [Persistent code caching for DBT][pcc] (USENIX ATC'16) measured 76% gains
  without helper threads and 9% with them.
- **HQEMU** ([TPDS paper][hq]):
  - Blocks are translated with TCG.
  - Hot traces are found by a counter (threshold 50) and queued in a FIFO to an
    **LLVM optimizer on another thread**. One `-O2` thread served all requests.
  - The optimized trace is patched in asynchronously.
- **Berberis**, Android's riscv64→x86_64 translator ([source][bb]):
  - It has two gears: a lite translator with counters, and a heavy optimizer
    once a region reaches 1000.
  - The heavy pass runs synchronously on the guest thread.
  - It has no persistent cache.

**For Win.apk:**

- Do the expensive optimizing compile off the app's threads, as HQEMU and
  Win.apk both do, and on the hot code first.
- Only HQEMU's "hot" is a count. Win.apk's profile has no counts, but its
  order of lines (first seen) is a usable proxy for "needed early".

### 4.7 Windows: what the platform offers background work

All from Microsoft Learn (links in section 10):

- **Priority and QoS.**
  - `IDLE_PRIORITY_CLASS` runs only when nothing else wants the CPU, and child
    processes inherit it.
  - `PROCESS_MODE_BACKGROUND_BEGIN` (on the current process only) also lowers
    I/O and memory priority. The page warns that "even an idle CPU priority
    process can easily interfere with system responsiveness when it uses the
    disk and memory".
  - `SetProcessInformation(ProcessPowerThrottling,
    PROCESS_POWER_THROTTLING_EXECUTION_SPEED)` makes a process **EcoQoS**:
    lower clocks, efficient cores on hybrid CPUs.
  - `SetProcessInformation(ProcessMemoryPriority, MEMORY_PRIORITY_LOW)` works
    on another process's handle.
- **Job objects.**
  - `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` ends every process in the job when the
    last handle closes, including when the worker dies.
  - `JOB_OBJECT_LIMIT_PROCESS_MEMORY` and `JOB_MEMORY` cap commit, so one
    process gets an allocation failure instead of the machine paging.
  - `JOBOBJECT_CPU_RATE_CONTROL_INFORMATION` with `HARD_CAP` sets CPU cycles in
    1/10,000ths of the machine, so a hard ceiling is possible.
  - `JobObjectBasicAccountingInformation` gives the job's CPU time, so the
    worker can subtract its own use from the system's.
- **Load.**
  - `GetSystemTimes`: system-wide idle, kernel and user times.
  - `GlobalMemoryStatusEx`: `dwMemoryLoad` and `ullAvailPhys`.
  - `CreateMemoryResourceNotification(LowMemoryResourceNotification)`: a
    system event for low physical memory.
- **The user.**
  - `GetLastInputInfo`: last input in this session.
  - `SHQueryUserNotificationState`: `QUNS_BUSY`,
    `QUNS_RUNNING_D3D_FULL_SCREEN`, `QUNS_PRESENTATION_MODE`.
- **Power.**
  - `GetSystemPowerStatus`: `ACLineStatus`, and `SystemStatusFlag` for battery
    saver.
  - `RegisterPowerSettingNotification` with `GUID_ACDC_POWER_SOURCE` and
    `GUID_POWER_SAVING_STATUS`.
  - On 24H2, Energy Saver replaces battery saver and delays idle and
    maintenance tasks.
- **Windows' own definition of idle** (Task Scheduler): the user counts as
  absent after **4 minutes** without input. CPU and disk count as idle below
  **80%**. The check runs every 10 minutes while the user is present and every
  30 s while away. Automatic Maintenance runs "when the machine is idle and on
  AC power", and "suspends any executing maintenance task if the user
  returns".

### 4.8 Analogies: shader pre-caching, ccache

- **Valve's Fossilize** ([repo][fz]):
  - `fossilize-replay` warms driver caches from recorded pipelines.
  - It has `--num-threads` (default: all cores) and a master process that
    spreads index ranges over single-threaded child processes.
  - It restarts past a pipeline that crashed the compiler, which is the same
    "skip the part that fails, keep the rest" Win.apk needs.
  - Steam's "Allow background processing of Vulkan shaders" runs it in the
    background and goes "full-gear" when the game starts. Valve's idle logic is
    not documented ([GamingOnLinux][fzgol]).
- **ccache** ([manual][cc]) names each object by a hash of the preprocessed
  input (or the source and its includes), the compiler's identity, and the
  options. A rerun after an interruption, or with one file changed, compiles
  only what is missing. That is exactly the resume behaviour Win.apk's parts
  need, and their C is already self-contained (no includes), so the key is
  simply the hash of the part's text plus clang's ID and flags.

## 5. Comparison of the projects

| | When | Unit translated and cached | Survives interruption | Throttling | Retranslation as profile grows | Code not found statically |
|---|---|---|---|---|---|---|
| **ART** | Idle + charging, daily; install only with a shipped profile | Package (dex → oat), from profile | Per package; the one in progress is lost | Own thread count + CPU set, nice 10, low I/O cgroup, 1 process, 570 s timeout, cancel on non-idle/thermal | profman: new methods ≥ max(2%, 100) | JIT + interpreter |
| **Rosetta 2** | Install, else first launch | Whole binary | `.aot.in_progress` → `.aot` rename (whole or nothing) | Not documented | None: whole | JIT |
| **Windows XTA / Prism** | During/after runs, by a service | Module; blocks that ran, appended (`.mp.N.jc`) | Versioned files | Not documented | Adds new blocks only | JIT |
| **Box64** | In the process, at unmap/exit | File × settings; JIT output | Whatever was written | n/a (no extra compile) | Adds as JIT grows | It is a JIT |
| **FEX** | Offline compiler started by FEXServer; DiskCache in process | Binary; blocks in merged code maps | Per binary | None found | Code map newer than cache → regenerate | It is a JIT |
| **HQEMU** | While running, helper thread | Hot trace | n/a | Separate thread | Counter threshold 50 | TCG |
| **Fossilize / Steam** | Background, before play | Pipeline | Per pipeline range; skips crashers | `--num-threads`, Steam setting | New pipelines recorded | Compiled at load |
| **ccache** | Build | Object | Per object | n/a | Content hash | n/a |
| **Win.apk today** | Every app start + every profile flush | Library (all parts) | Nothing (`.aot-tmp` wiped) | Below normal, all cores but one, no job object | Everything again at +10% | JIT + interpreter |

## 6. Options for Win.apk, with estimates

**Assumptions.**

- A job compiles **60,000 instructions a minute** with 20,000-instruction parts
  (section 3), and takes **about 0.5 GB** at its peak. That includes headroom;
  0.3 GB was measured.
- Two reference PCs:
  - **A**: 8 cores / 16 threads, 16 GB.
  - **B**: 4 cores / 8 threads, 8 GB.
- **Waze** (`libwaze.so`): whole about 1.7M instructions (plus about 0.2M in its
  other libraries), profiled 1.2M.
- **Browser**: 44M instructions (the issue's upper bound for Brave's
  `libchrome.so`; actual function code is likely around a third). It is
  assumed to settle on a 3M-instruction profile after a few sessions.
- Object and DLL sizes are about 43 bytes per instruction.
- "Idle" means up to 4 jobs on A and 2 on B (half the logical cores, limited by
  memory). "Active" means 1 job.

**Status quo, for reference.**

- With Waze, today's worker splits 1.2M instructions into parts of 80k-170k at
  0.9-1.7 GB each, on all cores but one: **10-13 GB at its peak**. Its
  measured rate is 80k a minute, so a pass takes 8-15 minutes.
- Growing the profile from about 0.1M to 1.2M in 10% steps means about 25
  passes. Each pass translates all that ran, and the total is about 11 passes'
  worth of a full translation: **2-3 hours of all cores** in the first days.
- Every interruption restarts the current pass, so in practice it never
  finishes.

| Option | Waze: CPU time | Waze: wall time (A idle / A active / B active) | Peak memory | Browser: CPU time | Browser: wall | User-visible impact |
|---|---|---|---|---|---|---|
| **(a) Whole, once, at install** | 1.9M → **32 min**, once | 8 min / 32 min / 32-64 min | 4 × 0.5 = **2 GB** (A), 1 GB (B) | 44M → **12 h** | 3 h idle; 12+ h active | Waze: fine (done in the first idle quarter hour, then never again). Browser: hours of background CPU, **1.9 GB** of DLL; not acceptable. |
| **(b) Profile retranslation, fixed** (resumable parts, run when settled, +25%) | First 1.2M → 20 min; then a full pass per +25%: ~3 more in the first week → **~70 min** | 5 / 20 / 20-40 min per pass | 2 GB | Full passes at 1M, 1.25M, 1.56M … 3M as the profile grows: ~11M instructions, **~3 h** in all | Up to 12 min idle per pass | Acceptable, but each growth repeats work already done; the first sessions still change a lot. |
| **(c) Hybrid by threshold** (whole ≤ 3M, profile above) | As (a): **32 min**, once | As (a) | 2 GB | As (b) or (d) | | Waze done for good; the browser stays profile-driven. |
| **(d) Incremental: only new functions, into extra DLLs** | First 20 min; each later delta ~1-3% of the profile = 12k-36k instructions, **< 1 min** | First 5 / 20 min; deltas seconds | 0.5 GB per delta | First **50 min**; deltas < 1 min each | First 12 min idle | Cheapest overall. Costs: more DLLs (merge at idle), calls across DLLs go through `aot_lookup`. |
| **(e) Resource-adaptive scheduling** | Same CPU time as the option it schedules; ~10% overhead from stopping parts | Stretches wall time to fit: 0 jobs while busy, ≤ N/2 idle | Capped by free RAM: ≤ 80% memory load by construction | Same | Same | The PC stays responsive regardless; work moves to idle time. Not an alternative to (a)-(d) but how any of them must run. |

**Reading the table.**

- **Waze-sized libraries** (≤ 3M instructions): the whole translation costs
  only about 40% more than the profiled one, and is then final. It needs no
  profile, is the same for every app with the same library, so it can go in
  the shared cache, and leaves nothing on the JIT. Repeated profile passes, as
  in (b), cost more in total within a week. So whole wins.
- **Browser-sized libraries**: whole is out (12 CPU-hours, 1.9 GB). The first
  profile pass costs about the same as a whole Waze, and after that only deltas
  are cheap. So (d) wins.
- **(e) applies to every option.** Today's failure is as much *how* the work
  runs (huge parts, all cores, no job object) as *what* is translated.
- **The threshold.** The cost of a whole translation scales with instructions:
  3M instructions is about 50 CPU-minutes, or about 12 minutes of an idle PC A.
  On issue 78's table, the threshold places whole translations on everything
  but Brave's `libchrome.so`, CapCut's largest libraries (6.9M) and similar.
  Measure the real function counts (section 9) before fixing the number.

## 7. Recommended design

**(c) + (d) under (e)**, with fixed-size, content-addressed parts.

### 7.1 What is translated, and when

| Library size (function instructions, as `winapk-aot` counts) | What | When |
|---|---|---|
| ≤ **3,000,000** (`kAotWholeBelow`, replaces `kAotFullBelow` = 400,000) | The whole library, once | Right after install (the app may already run on the JIT). Parts compile in profile order when a profile exists, so the profiled code is compiled first |
| > 3,000,000 | Only functions in the profile; then only **newly profiled** functions, as additional DLLs | First pass when the profile has settled (below); a delta when the new functions reach **max(2% of the translated instructions, 5,000 instructions)**: profman's rule, with its floor of 100 methods at ~45 instructions each |
| Not an arm64 ELF file | Nothing (`skip`) | |

**A profile is settled when:**

- The app has exited, or
- No flush has added more than 1% of the profile's lines for **2 minutes**, and
  the app started at least **60 s** ago.

Until then the watch thread only flushes and does not start the worker. This
replaces "start the worker at every growth by a tenth".

**Never more than one full pass of a large library per Win.apk version.** Deltas
are merged into one DLL (see 7.3) only when there are more than **8**, and only
when idle and on AC power.

### 7.2 The unit of work: fixed parts, content-addressed

**Fixed-size parts.**

- `winapk-aot` cuts the functions to translate into parts of about **20,000
  instructions** (at least one function each), *independent of the number of
  jobs*.
- Functions are ordered by first profile line (startup code first), then by
  address.
- Parallelism comes from compiling many parts, never from making fewer, bigger
  ones.

**Content-addressed objects.**

- A part's name is the FNV-1a 64 hash of its C text, clang's `file_id`, the
  compile flags and `kAotVersion`, which is the same recipe as `aot_cache_key`.
- The object is written as `<key>.o.tmp`, then renamed to `<key>.o`. This is
  Rosetta's in-progress rename.
- A part whose `<key>.o` exists is not compiled again.
- An interrupted pass loses at most the parts that were compiling: a few
  seconds of work each.

**A frozen plan.**

- A pass starts by writing `plan`: the list of part keys and the function set
  (the profile snapshot it used).
- A restarted worker finishes that plan instead of starting a new one, unless
  the plan is for an older library or tool version.
- So the profile growing during a pass never invalidates the pass.

**Failures by part.**

- A part that fails to compile, or exceeds the job memory limit, is split in
  two and retried. A single function that still fails is compiled `optnone`;
  if that fails too, it is left out (the JIT runs it).
- The library's translation therefore no longer fails as a whole for one bad
  function.
- Each failure is logged with clang's exit code and, for file errors,
  `GetLastError()`. For example, `cannot write …` becomes `cannot write …
  (error 32, in use by another process)`.

**Backoff for the library as a whole.** This covers `winapk-aot` failing, or
more than a quarter of the parts failing.

- `failures` and `retry-after` in the stamp.
- The next try is after 10 min × 4^(failures−1), capped at one day.
- After **4** failures, no more tries until `kAotVersion` or Win.apk's version
  changes. ART's dexopt retry is "30 seconds, exponential, capped at 5 hours";
  a translation is much more expensive, so the steps here are longer.

### 7.3 Files on disk

```
lib\arm64\<lib>.so
lib\arm64\<lib>.so.aot.profile          (as now)
lib\arm64\<lib>.so.aot.stamp            (key=value lines, below)
lib\arm64\<lib>.so.aot.dll              the whole translation, or the first profile pass
lib\arm64\<lib>.so.aot.1.dll ... .8.dll deltas (functions not in the DLLs before them)
lib\arm64\.aot\<lib>.so\plan            the pass in progress: kind, text hash, part keys, functions
lib\arm64\.aot\<lib>.so\<key>.o         finished parts of the pass in progress
<data>\aot-cache\<key>.aot.dll          whole translations, shared (as now, 2 GB LRU)
```

**The stamp** becomes `key value` lines, which can be extended without breaking
`fscanf`:

```
version 4
kind whole | profile | skip | bad
profile-lines 18234     (profile size the last pass used)
translated-insns 1203448
dlls 3                  (base + deltas)
failures 0
retry-after 0           (Unix seconds)
last-error clang exit 1 in part 3f9a…, 1 function left to the JIT
```

**Clean-up and its rules.**

- `.aot\<lib>.so\` is deleted when its pass is linked and installed.
- A delta DLL is installed under the next free number. A DLL in use is never
  replaced, so `.aot.dll.new` is only needed for the merge, which writes a new
  base and deletes the deltas at the next start (`prepare_arm64`, as today).
- `.tmp` files older than the worker's start are deleted at start. With the job
  object (7.4) no earlier clang can still be writing them.
- **Objects are not kept across passes.** After linking they would only serve a
  re-link, which gains nothing. Disk use stays the DLLs (~43 bytes per
  instruction: ~80 MB for a whole Waze) plus at most one pass's objects.

**Loader.**

- `aot_load` and `aot_load_pending` load every `<lib>.so.aot*.dll` in order.
  `Library::translated` becomes the number of DLLs loaded, so a delta finished
  while the app runs is picked up too.
- Calls between DLLs work without changes to the emitted code: a call to a
  function outside the DLL is already `aot_rt.branch` → `aot_lookup`, which
  sees every loaded DLL (section 2).
- They are slow, though: measured at about 15× a call inside one DLL
  ([`aot-measurements.md`](aot-measurements.md), section 4). So
  `CALL_SITE` also remembers its first translated target (set once per site),
  which is 2.7× faster, and `aot_lookup` gets a direct-mapped table in front of
  its map, which is 1.9× faster for every call through a pointer.
- Deltas are merged into one DLL at idle when they hold 25% of the base's
  instructions or there are 4 of them, so hot calls end up inside one DLL
  again.

### 7.4 Processes and the governor

**The worker** (`--aot-translate`), at start:

- `SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN)` for low
  I/O and memory priority, then `IDLE_PRIORITY_CLASS`.
- `SetProcessInformation(ProcessPowerThrottling, EXECUTION_SPEED)` for EcoQoS.
- It creates a job object and assigns itself to it, with these limits:
  - `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`: killing the worker, or its crash,
    ends every clang, winapk-aot and link.
  - `JOB_OBJECT_LIMIT_PROCESS_MEMORY` = **1.5 GB**: a part that needs more
    fails fast and is split, instead of pushing the machine into paging.
  - `JOB_OBJECT_LIMIT_JOB_MEMORY` = **min(40% of RAM, 6 GB)** as a hard
    backstop.
  - `JOBOBJECT_CPU_RATE_CONTROL_INFORMATION` with `ENABLE | HARD_CAP` at
    **50% of the machine** as a backstop to the governor. It is not available
    under Remote Desktop with DFSS on; the governor still applies there.
- `start_aot_worker` still breaks the worker away from the launcher's job
  (`CREATE_BREAKAWAY_FROM_JOB`) as today. The new job is the worker's own.
- Children are created **suspended**, put in the job, given EcoQoS and
  `MEMORY_PRIORITY_LOW` through `SetProcessInformation` on their handle, then
  resumed. `IDLE_PRIORITY_CLASS` passes to them with `CreateProcess`'s flag.
  `winapk-aot` and the link go through the same path, not `run_process`.

**The governor** is a thread in the worker that runs every **2 s** and sets
`slots`, the number of clang processes that may run. `run_parallel` starts a
part only while `running < slots`:

```
cpu_others = system busy % (GetSystemTimes delta) − own job's share (JobObjectBasicAccountingInformation delta)
mem        = GlobalMemoryStatusEx
idle_for   = now − GetLastInputInfo
reserve    = max(2 GB, 20% of RAM)             // keep memory load ≤ 80%
mem_slots  = floor((ullAvailPhys + own job's working set − reserve) / 512 MB)
cpu_slots  = floor(logical × 0.80 − logical × cpu_others)     // keep total CPU ≤ 80%
cap        = 0  if battery saver / Energy Saver on, or on battery (unless the setting allows),
                or SHQueryUserNotificationState is BUSY / D3D_FULL_SCREEN / PRESENTATION_MODE,
                or the app started < 60 s ago, or the launcher's Pause is on
           = 1  if idle_for < 4 min (the user is active)
           = max(1, logical / 2)  otherwise
slots      = clamp(min(cap, mem_slots, cpu_slots), 0, 16)
```

**Hysteresis.**

- Go up by at most one slot every 10 s. Go down at once.
- If memory load passes **90%**, or the low-memory resource notification is
  signalled, terminate the most recently started clang. Its part goes back to
  the queue and costs seconds at most.
- With `slots` = 0 for **10 minutes**, the worker exits cleanly. Finished
  parts are kept, and the next app start or launcher idle check starts it
  again.

**Why these numbers.**

- **80% CPU and memory** is Windows' own idle threshold (Task Scheduler), and
  what the owner asked for.
- **4 minutes** is Windows' "user absent" interval.
- **Half the logical cores** when idle matches ART's separate, smaller
  background thread budget. It leaves the other half for whatever the user
  comes back to, and on SMT parts the second thread of a core adds little to
  clang.
- **512 MB a job** is the measured 300 MB at 20k-instruction parts, plus
  headroom for larger functions.

**Where it can also run.**

- It is the app that starts the worker. The launcher, which stays resident
  while any window is open, can also start it at its idle check (every 10
  minutes, once the user has been idle 4 minutes, on AC) for an app that still
  has work. Only one worker per app (the existing mutex).
- A Task Scheduler task with `MaintenanceSettings` would also do, but it adds
  an install step and the launcher can already do it.

### 7.5 What the user sees

As the issue's step 7:

- While a worker runs, the launcher's tile and App details page show
  "Optimizing for this PC…" with progress (parts done / parts in the plan,
  from `plan` and the objects present).
- **Pause** sets a flag the governor reads (`cap` = 0).
- **Settings > Apps > "Optimize ARM apps in the background"** (registry
  `AotBackground`, absent = 1) turns it off.
- An app with only arm64 code above `kAotWholeBelow` says so on its details
  page, and points to an x86_64 build where one exists.
- Document all of it in `docs/USER_GUIDE.md`.

## 8. Implementation plan

Each step stands alone and can be measured before the next one.

1. **Job object and process settings** (`aot_translate_main`, `run_parallel`,
   `run_process` for the worker's children):
   - The worker creates the job with the limits in 7.4 and runs every child
     created suspended → assigned → EcoQoS / `MEMORY_PRIORITY_LOW` → resumed.
   - `PROCESS_MODE_BACKGROUND_BEGIN` and `IDLE_PRIORITY_CLASS` for the worker
     itself.
   - Log `GetLastError()` on every failed `CreateProcessW`, `MoveFileExW` and
     `CopyFileW`.
   - Check: kill the worker while it compiles; no clang is left (issue 78,
     "How to check").
2. **Fixed part size** (`arm64_aot_main.cpp`, `main`, the part split):
   - `--part-insns 20000` replaces "parts = jobs". Parts are cut in profile
     order, then address order, instead of balanced largest-first.
   - `aot_translate_one` passes it and lists the parts from `winapk-aot`'s
     output instead of probing `c.N.c`.
   - Check: peak working set of the job (`JOBOBJECT_EXTENDED_LIMIT_INFORMATION.PeakJobMemoryUsed`,
     logged at the end of a pass) stays around jobs × 0.3-0.5 GB.
3. **Content-addressed, resumable parts** (`aot_translate_one`):
   - Stop wiping `.aot-tmp`. Move it to `.aot\<lib>.so\` with the `plan` file.
   - Hash each C part. Skip it if `<key>.o` exists. Compile to `.o.tmp` and
     rename.
   - Split a failing part and retry (7.2).
   - `aot_needs_work` treats an unfinished plan as work.
   - Check: kill the worker in the middle of a pass and start the app again; the
     log shows "N of M parts already compiled".
4. **The governor** (new `aot_governor` thread in `aot_translate_main`;
   `run_parallel` waits on `slots` instead of `jobs`):
   - The policy in 7.4. It logs its decision whenever `slots` changes ("slots
     4 → 1: user active").
   - The fixed `jobs = hardware_concurrency() - 1` goes.
   - Check: section 9, responsiveness.
5. **Settled profiles and backoff** (`aot_watch_thread`, `aot_needs_work`,
   `write_aot_stamp`, `read_aot_stamp`):
   - The stamp's key=value format with `failures` and `retry-after`.
   - The watch thread starts the worker only for a library whose profile is
     settled (7.1), or at the app's exit.
   - Fit for the app's exit: start the worker from the runtime's shutdown path,
     or from the launcher's `run_thread` when the run ends, since it already
     knows.
   - The growth rule becomes `max(2% of translated instructions, 5,000
     instructions)` of new functions. `winapk-aot` reports the count with
     `--dry-run`.
6. **Threshold for whole translation** (`kAotFullBelow` → `kAotWholeBelow` =
   3,000,000; `aot_needs_work`):
   - Whole when below, regardless of the profile.
   - A whole translation with a profile compiles the profiled parts first.
   - Whole results go to the shared cache as today.
7. **Delta DLLs** for libraries above the threshold:
   - `winapk-aot --exclude <list>`: the functions in the DLLs already made,
     read from their `AotModule` tables, or from a `.aot.functions` list
     written beside each DLL.
   - `aot_translate_one` writes `<lib>.so.aot.<n>.dll`.
   - `aot_load` and `aot_load_pending` load every DLL of a library
     (`Library::translated` → a count).
   - The merge after 8 deltas: a full profile pass at idle on AC, written as
     `.aot.dll.new`, with the deltas removed at the next start in
     `prepare_arm64`.
8. **Launcher** (`launcher_main.cpp`):
   - Progress and Pause on the tile and details page. Pause sets a named event
     or registry value the governor reads.
   - The setting.
   - The arm64-only note.
   - The idle-time start (7.4).
   - `tr()` strings with Hebrew, and `docs/USER_GUIDE.md`.
9. **Docs.** `docs/ARCHITECTURE.md` (the arm64 section and "data on disk"),
   `windows/README.md` (any new `WINAPK_AOT_*` variables, e.g.
   `WINAPK_AOT_PART_INSNS`, `WINAPK_AOT_MAX_JOBS`), and the comment block at
   the top of the section in `runtime_main.cpp`.

Steps 1-4 alone fix the "PC unusable" symptom. Steps 5-7 fix "never finishes"
and the repeated work.

## 9. Measuring success

**Instrument first.** The worker's log gains one line per pass and one per
governor change:

```
[win.apk] libwaze.so: pass profile, 61 parts (12 already done), 1.20M instructions
[win.apk] aot: slots 4 -> 1 (user active); others' CPU 23%, memory load 61%
[win.apk] libwaze.so: translated in 412 s wall, 1,186 CPU-s, peak job memory 1.9 GB, 0 parts failed
```

**Tests on a 16 GB, 8-core PC**, Waze and one large app (CapCut or Brave):

| What | How | Target |
|---|---|---|
| No stray compilers | Kill `winapk-runtime --aot-translate` mid-pass (Task Manager) | No `clang.exe` 1 s later |
| Resume | Kill mid-pass, start the app again | Log: parts already done > 0; total CPU of the two runs ≤ 1.1 × one uninterrupted pass |
| Memory | Peak job memory, and system memory load sampled every second during a pass | Job ≤ slots × 0.5 GB; system load ≤ 85% at every sample, ≤ 80% for 95% of samples, caused by the worker |
| CPU while the user is active | Type and browse during a pass; sample `GetSystemTimes` | Worker ≤ 1 logical core; system total ≤ 80% unless other programs take more |
| Responsiveness | A fixed foreground task (e.g. open 20 tabs in a browser, or a timed `7z b -mmt1`) with and without a pass | ≤ 10% slower with the pass running |
| Done for good | Start Waze 5 times after its whole translation | No worker started (stamp `whole`, logged) |
| Deltas stay small | 10 sessions of the large app after its first pass | Each delta < 1 CPU-minute; ≤ 8 DLLs before a merge |
| Throughput | instructions / CPU-s from the pass line | ≥ 1,000 a CPU-second (60k a minute) at 20k parts; compare with today's 80k a minute in all |
| Startup unaffected | Time to first frame (`WINAPK_READY_EVENT`) on cold start, with work pending vs none | No difference (no compile in the first 60 s) |
| Battery | On battery / battery saver | No clang runs |

**Before fixing `kAotWholeBelow`, record** what `winapk-aot` counts as
function instructions for the apps in issue 78's table: the issue's column is
an upper bound, a third of it for Waze. Then set the threshold so that the
whole translations it allows stay under about **one idle hour on PC B** (2 jobs
× 60k × 60 = 7.2M instructions). 3M leaves room for the slower code mixes in
section 3.

## 10. Sources

Android:

- [a7]: Android 7.0 for developers, https://developer.android.com/about/versions/nougat/android-7.0
- [ajit]: Implement ART just-in-time compiler, https://source.android.com/docs/core/runtime/jit-compiler
- [acfg]: Configure ART, https://source.android.com/docs/core/runtime/configure
- [apm]: Package manager configuration, https://source.android.com/docs/core/runtime/configure/package-manager
- [aas]: ART Service configuration, https://source.android.com/docs/core/runtime/configure/art-service
- [abp]: Baseline profiles overview, https://developer.android.com/topic/performance/baselineprofiles/overview
- [abdj]: BackgroundDexoptJob.java, https://android.googlesource.com/platform/art/+/refs/heads/main/libartservice/service/java/com/android/server/art/BackgroundDexoptJob.java
- [adx]: Dexopter.java, https://android.googlesource.com/platform/art/+/refs/heads/main/libartservice/service/java/com/android/server/art/Dexopter.java
- [apa]: profile_assistant.cc, https://android.googlesource.com/platform/art/+/refs/heads/main/profman/profile_assistant.cc
- [atp]: task_profiles.json, https://android.googlesource.com/platform/system/core/+/refs/heads/main/libprocessgroup/profiles/task_profiles.json
- Android 13 BackgroundDexOptService (thermal, battery, storage checks): https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android13-release/services/core/java/com/android/server/pm/BackgroundDexOptService.java

Apple:

- [rw]: WWDC 2020, Explore the new system architecture of Apple silicon Macs, https://developer.apple.com/videos/play/wwdc2020/10686/
- [rabout]: About the Rosetta translation environment, https://developer.apple.com/documentation/apple-silicon/about-the-rosetta-translation-environment
- [rdj]: Dougall Johnson, Why is Rosetta 2 fast?, https://dougallj.wordpress.com/2022/11/09/why-is-rosetta-2-fast/
- [rch2]: Koh M. Nakagawa, Project Champollion part 2, https://ffri.github.io/ProjectChampollion/part2
- [rel]: Eclectic Light, Explainer: Rosetta 2, https://eclecticlight.co/2022/12/10/explainer-rosetta-2/

Windows on Arm:

- [wemu]: How emulation works on Arm, https://learn.microsoft.com/en-us/windows/arm/apps-on-arm-x86-emulation
- [warm64ec]: Arm64EC, https://learn.microsoft.com/en-us/windows/arm/arm64ec
- [wbh]: Koh M. Nakagawa, Jack-in-the-Cache (Black Hat EU 2020), https://i.blackhat.com/eu-20/Wednesday/eu-20-Nakagawa-Jack-In-The-Cache-A-New-Code-Injection-Technique-Through-Modifying-X86-To-Arm-Translation-Cache.pdf
- [wxt]: FFRI XtaTools, https://github.com/FFRI/XtaTools

Box64, FEX, QEMU, Berberis:

- [bx]: Box64 releases, https://github.com/ptitSeb/box64/releases
- [bxu]: Box64 USAGE.md, https://github.com/ptitSeb/box64/blob/main/docs/USAGE.md
- [bxsrc]: Box64 dynacache.c, https://github.com/ptitSeb/box64/blob/main/src/tools/dynacache.c
- [bxblog]: Box64 v0.3.8, https://box86.org/2025/10/new-box64-v0-3-8-released/
- [fxgh]: FEX source, https://github.com/FEX-Emu/FEX
- [fx2512]: FEX-2512, https://fex-emu.com/FEX-2512/
- [fx2609]: FEX-2609, https://github.com/FEX-Emu/FEX/releases/tag/FEX-2609
- [qemu]: qemu-devel, persistent code caching, https://lists.gnu.org/archive/html/qemu-devel/2018-11/msg03306.html
- [pcc]: A General Persistent Code Caching Framework for DBT, USENIX ATC'16, https://usenix.org/conference/atc16/technical-sessions/presentation/wang
- [hq]: HQEMU (TPDS), https://homepage.iis.sinica.edu.tw/papers/dyhong/18243-F.pdf
- [bb]: Berberis, https://android.googlesource.com/platform/frameworks/libs/binary_translation

Shader caches, ccache:

- [fz]: Fossilize, https://github.com/ValveSoftware/Fossilize
- [fzgol]: Steam beta adds Vulkan shader background processing, https://www.gamingonlinux.com/2020/05/steam-beta-adds-vulkan-shader-processing
- DXVK 2.0 (pipeline libraries replace the state cache), https://github.com/doitsujin/dxvk/releases/tag/v2.0
- [cc]: ccache manual, https://ccache.dev/manual/latest.html

Windows APIs (Microsoft Learn):

- SetProcessInformation (EcoQoS, memory priority): https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation
- Quality of service levels: https://learn.microsoft.com/en-us/windows/win32/procthread/quality-of-service
- SetPriorityClass (`IDLE_PRIORITY_CLASS`, `PROCESS_MODE_BACKGROUND_BEGIN`): https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setpriorityclass
- JOBOBJECT_BASIC_LIMIT_INFORMATION: https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_limit_information
- JOBOBJECT_CPU_RATE_CONTROL_INFORMATION: https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_cpu_rate_control_information
- JOBOBJECT_NOTIFICATION_LIMIT_INFORMATION: https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_notification_limit_information
- GetSystemTimes: https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemtimes
- GlobalMemoryStatusEx: https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-globalmemorystatusex
- CreateMemoryResourceNotification: https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-creatememoryresourcenotification
- GetLastInputInfo: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getlastinputinfo
- SHQueryUserNotificationState states: https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ne-shellapi-query_user_notification_state
- SYSTEM_POWER_STATUS: https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-system_power_status
- Power setting GUIDs: https://learn.microsoft.com/en-us/windows/win32/power/power-setting-guids
- Energy Saver: https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/energy-saver
- Task idle conditions: https://learn.microsoft.com/en-us/windows/win32/taskschd/task-idle-conditions
- Automatic Maintenance: https://learn.microsoft.com/en-us/windows/win32/taskschd/task-maintenence

[a7]: https://developer.android.com/about/versions/nougat/android-7.0
[ajit]: https://source.android.com/docs/core/runtime/jit-compiler
[acfg]: https://source.android.com/docs/core/runtime/configure
[apm]: https://source.android.com/docs/core/runtime/configure/package-manager
[aas]: https://source.android.com/docs/core/runtime/configure/art-service
[abp]: https://developer.android.com/topic/performance/baselineprofiles/overview
[abdj]: https://android.googlesource.com/platform/art/+/refs/heads/main/libartservice/service/java/com/android/server/art/BackgroundDexoptJob.java
[adx]: https://android.googlesource.com/platform/art/+/refs/heads/main/libartservice/service/java/com/android/server/art/Dexopter.java
[apa]: https://android.googlesource.com/platform/art/+/refs/heads/main/profman/profile_assistant.cc
[atp]: https://android.googlesource.com/platform/system/core/+/refs/heads/main/libprocessgroup/profiles/task_profiles.json
[rw]: https://developer.apple.com/videos/play/wwdc2020/10686/
[rabout]: https://developer.apple.com/documentation/apple-silicon/about-the-rosetta-translation-environment
[rdj]: https://dougallj.wordpress.com/2022/11/09/why-is-rosetta-2-fast/
[rch2]: https://ffri.github.io/ProjectChampollion/part2
[rel]: https://eclecticlight.co/2022/12/10/explainer-rosetta-2/
[wemu]: https://learn.microsoft.com/en-us/windows/arm/apps-on-arm-x86-emulation
[warm64ec]: https://learn.microsoft.com/en-us/windows/arm/arm64ec
[wbh]: https://i.blackhat.com/eu-20/Wednesday/eu-20-Nakagawa-Jack-In-The-Cache-A-New-Code-Injection-Technique-Through-Modifying-X86-To-Arm-Translation-Cache.pdf
[wxt]: https://github.com/FFRI/XtaTools
[bx]: https://github.com/ptitSeb/box64/releases
[bxu]: https://github.com/ptitSeb/box64/blob/main/docs/USAGE.md
[bxsrc]: https://github.com/ptitSeb/box64/blob/main/src/tools/dynacache.c
[bxblog]: https://box86.org/2025/10/new-box64-v0-3-8-released/
[fxgh]: https://github.com/FEX-Emu/FEX
[fx2512]: https://fex-emu.com/FEX-2512/
[fx2609]: https://github.com/FEX-Emu/FEX/releases/tag/FEX-2609
[qemu]: https://lists.gnu.org/archive/html/qemu-devel/2018-11/msg03306.html
[pcc]: https://usenix.org/conference/atc16/technical-sessions/presentation/wang
[hq]: https://homepage.iis.sinica.edu.tw/papers/dyhong/18243-F.pdf
[bb]: https://android.googlesource.com/platform/frameworks/libs/binary_translation
[fz]: https://github.com/ValveSoftware/Fossilize
[fzgol]: https://www.gamingonlinux.com/2020/05/steam-beta-adds-vulkan-shader-processing
[cc]: https://ccache.dev/manual/latest.html
