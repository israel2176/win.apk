# Win.apk: building, running and testing

This is the developer guide for the Windows port: how to build it, run an app
from a build, run the tests, and keep the fork in step with upstream.
[`../docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md) explains how the pieces
fit; [`PORTING.md`](PORTING.md) holds the design decisions, the technical
findings, the known gaps and the milestone plan; the
[user guide](../docs/USER_GUIDE.md) describes what a user sees.

Win.apk is an unofficial port of [Mac.apk](https://github.com/kksimp/Mac.apk)
to Windows on Intel/AMD (x64). It is not affiliated with or endorsed by the
Mac.apk project or Voltare. Win.apk's own code lives in `windows/`;
`runtime/` (the Android framework in Java and the shared native sources)
comes from upstream and is kept close to it so that upstream fixes can still
be merged. Upstream's macOS-only parts (the Swift launcher, the Mac build
scripts, the prebuilt Mac libraries and the Mac documentation) are not in
this repository.

- [What is here](#what-is-here)
- [Build on Windows](#build-on-windows)
- [Build on Linux and run under Wine](#build-on-linux-and-run-under-wine)
- [Run an app from a build](#run-an-app-from-a-build)
- [Environment variables for development](#environment-variables-for-development)
- [Tests](#tests)
  - [The UI gate: only closer to Android](#the-ui-gate-only-closer-to-android)
- [The compatibility sweep](#the-compatibility-sweep)
  - [Every screen: the screens crawl](#every-screen-the-screens-crawl)
- [CI and releases](#ci-and-releases)
- [Keeping up with upstream](#keeping-up-with-upstream)
  - [Changes to the upstream tree](#changes-to-the-upstream-tree)
- [License](#license)

## What is here

| Path | What it is |
|---|---|
| `include/winapk/`, `src/` | The ELF loader (`elf_loader.cpp`), the guest linker (`linker.cpp`), dynamic ELF TLS (`elf_tls.cpp`), the `%fs` stack-guard patch (`fs_tls_patch.cpp`), the ABI thunks (`abi_thunk.cpp`), the guest `JNIEnv` (`guest_jni.cpp`, `jni_slots.inc`), the variadic JNI bridge (`jni_variadic.cpp`), native-method binding (`jni_bind.cpp`), libjnigraphics (`jnigraphics.cpp`) |
| `src/mini_bionic.cpp`, `src/bionic/` | The Bionic shim: Android's libc, libdl, liblog, libm, libz and the NDK's `ALooper` and `ATrace` on the UCRT and Win32, about 900 System V functions |
| `src/arm64/` | The arm64 engines: interpreter (`arm64_cpu.cpp`, `arm64_simd.cpp`), JIT (`arm64_jit.cpp`, dynarmic), ahead-of-time translator (`arm64_aot.cpp`) and the AAPCS64 bridge (`arm64_guest.cpp`, `arm64_abi.cpp`) |
| `host/` | `winapk-runtime.exe` (`runtime_main.cpp`), the app window (`win32_window.cpp`), the host natives (`native_*.cpp`, one file per Java class; `host_natives.cpp` binds them), the GL layer (`gl/`), the app-library loading path (`guest_libs.cpp`, `guest_stub.cpp`, `translator_main.cpp`), shared storage (`user_folders.cpp`), java.io's file opens with `FILE_SHARE_DELETE` (`java_io_open.cpp`), class-data sharing (`cds.cpp`, `cds_util.cpp`), `WINAPK_AOSP_GFX` (`aosp_gfx.cpp`, `aosp_gfx_redirect.cpp`) |
| `launcher/` | `Win.apk.exe`, the app library, the install helper and the permission dialogs (`launcher_main.cpp`, `launcher.rc`, the icon), and its state machines, each a header the unit test drives (`app_runs.h`: an app's run; `installs.h`: an install; `updates.h`: the update check and download) |
| `java/` | Windows-only Java: `com.winapk.tools.AppInstall` and `AppMeta` (`winapk-tools.jar`), `java_base_patch/` (an overlay for `java.base`), `aosp_gfx/` (the stand-ins in `aosp-graphics.jar`: ART's, Robolectric's, and an `android.icu.text.Bidi` on `java.text.Bidi`), and `winapk-framework.classlist` (the classes of the framework archive, `host/cds.h`) |
| `tools/` | `winapk-elfrun` (the loader test runner), `winapk-jvm-run` (the real-JVM test), `winapk-aot` (`arm64_aot_main.cpp`), and the generators `gen_jni_slots.py`, `gen_gl_stubs.py`, `gen_icon.py` |
| `scripts/` | `build_java.sh`, `build_framework_res.sh`, `build_aosp_gfx.sh` (`WINAPK_AOSP_GFX`'s files), `fetch_angle.sh`, `fetch_webview2.sh`, `stage_clang.sh`, `stage_runtime.sh`, `bake_jre.sh` (the JRE Win.apk ships), `merge_classlists.py` |
| `test/` | The unit tests (`*_test.cpp`), the guest test libraries (`guest/`, built with plain clang + lld, no NDK), the Java tests (`java/`), the Wine runners (`run_*_wine.sh`), the compatibility sweep and the screens crawl (`compat/`) |
| `package/` | The installer (`winapk.iss`, Inno Setup), `README.txt` and `RELEASE_NOTES.md` shipped with a release |
| `cmake/`, `compat/`, `third_party/` | The Linux cross toolchain file, the CMake steps that cut the GL and Bitmap bridges out of upstream's `native_methods.cpp` and `bionic_shim.cpp`, a CoreFoundation shim for `native_sqlite.cpp`, the Khronos headers |
| `CMakeLists.txt` | Every Windows target |

`PORTING.md`, section 4, describes each component and test in more detail.

## Build on Windows

Toolchain: clang only. MSVC cannot emit System V calls, and MinGW GCC 13
emits broken SEH unwind data for `sysv_abi` functions. CI builds on
`windows-latest` with MSYS2 CLANG64 and Temurin 21.

In an [MSYS2](https://www.msys2.org/) CLANG64 shell, with a JDK 21 installed
and `JAVA_HOME` pointing at it:

```sh
pacman -S mingw-w64-clang-x86_64-{clang,cmake,ninja,zlib,boost}
cmake -S windows -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DWINAPK_JNI_INCLUDE="$JAVA_HOME/include" -DWINAPK_ARM64_JIT=ON
cmake --build build
windows/scripts/stage_runtime.sh build runtime
windows/scripts/bake_jre.sh "$JAVA_HOME" runtime/lib runtime/jre
```

- `-DWINAPK_JNI_INCLUDE=<jdk>/include` builds the JNI library, its tests,
  `winapk-jvm-run` and the runtime; without it only the loader, the shim and
  their tests build.
- `-DWINAPK_ARM64_JIT=ON` fetches dynarmic (needs the Boost headers) and
  builds the arm64 JIT. CI and releases use it; without it arm64 code runs on
  the interpreter until its translation is ready. One change is made to it
  (`cmake/dynarmic_patch.cmake`): FRECPS and FRSQRTS keep their special
  case for 0 times infinity under the NaN optimization Win.apk uses.
- `-DWINAPK_VERSION=v0.2.0-preview` (or `0.2.0`) is the version stamped into
  `Win.apk.exe` (`launcher/launcher.rc`, through a generated
  `winapk_version.h`), which Settings shows and the update check compares with
  the newest release. The release workflow passes its tag; `ci.yml` passes the
  newest release tag + `-dev.<run>` (the update check reads it as that release,
  so a CI build is not offered the release it came after); the default is 0.1.0.
- `stage_runtime.sh <build dir> [out]` assembles a runnable folder (default
  `windows/build/runtime`): `Win.apk.exe`, `bin/` (the runtime, the
  translator and stub DLL, `winapk-aot`, ANGLE from `fetch_angle.sh`,
  `WebView2Loader.dll` from `fetch_webview2.sh`, `tools/framework-res.apk`),
  `lib/` (the jars from `build_java.sh`) and `clang/` (from
  `stage_clang.sh`, the compiler the arm64 translation uses). The JDK is not
  copied: the runtime uses `<out>/jre` if present, else `WINAPK_JAVA_HOME` or
  `JAVA_HOME`. A plain JDK works, but without class-data sharing (the run log
  says `cds: off`): the patch classes need `--patch-module` there.
- `bake_jre.sh <jdk> <lib dir> <out>` makes the JRE releases ship (`<out>/jre`):
  a jlink image of the JDK's jmods (needs a full JDK 21) with the module list
  the framework needs, `--strip-debug`, and `<lib>`'s `java_base_patch` and
  `jdk_unsupported_patch` built into `java.base` and `jdk.unsupported`, then
  its default CDS archive (`bin/server/classes.jsa`). It records the patches'
  digests in `lib/winapk-baked-patches`; a patch rebuilt later no longer
  matches and the runtime applies it with `--patch-module` again (no CDS for
  that run), so re-run the bake after `build_java.sh`. `WINAPK_JRE_COMPRESS`,
  `WINAPK_JRE_MODULES`, and `WINAPK_JMOD`/`WINAPK_JLINK` (a Windows JDK's tools
  under Wine, to bake on Linux) change it.
- `build_java.sh [out]` builds upstream's Java side with a stock JDK 21:
  `android-framework.jar`, `macapk-loader.jar`, `winapk-tools.jar`, the
  BouncyCastle provider (unsigned: class-data sharing skips a signed jar's
  classes), the `java.base` and `jdk.unsupported` patches, and the framework
  archive's class list.
  Android's own `org.apache.http.legacy` (Apache-2.0, converted to JVM classes
  in `thirdparty/apache-http-legacy/`) is unpacked into
  `android-framework.jar`, as upstream's build does.
- `build_framework_res.sh [out.apk]` builds the platform resource APK from
  AOSP sources with Google's standalone `aapt2` (9.x or later), through
  upstream's `scripts/build_framework_res.sh`. The result is cached under
  `thirdparty/framework-res`.

## Build on Linux and run under Wine

```sh
sudo apt install clang lld cmake ninja-build mingw-w64 libz-mingw-w64-dev wine64
```

You also need a Windows JDK (for example the Temurin 21 zip) for `jvm.dll`,
`jni.h` and `jvmti.h`, and any JDK 21 or later on the host for `javac`.
Configure with `-DCMAKE_TOOLCHAIN_FILE=windows/cmake/clang-mingw-x86_64.cmake`
and `-DWINAPK_JNI_INCLUDE=<windows-jdk>/include`. The scripts under
`windows/test/` do the whole thing:

| Script | What it does |
|---|---|
| `windows/test/run_poc_wine.sh` | Builds the loader and the guest libraries and runs the loader, linker, arm64 and crash-report tests under Wine. The arm64 test libraries are translated first, as CI's step does, which takes a clang 19 or later (`clang-19`, `-20`, `-21`, or `WINAPK_AOT_CLANG`). The toolchain file adds `-femulated-tls` and winpthreads, which GCC's mingw-w64 libstdc++ needs |
| `WINAPK_WIN_JDK=<windows-jdk> windows/test/run_e2e_wine.sh` | Builds and runs the real-JVM end-to-end test, with the x86_64 and the arm64 guest library (`WINAPK_HOST_JAVAC` picks the javac) |
| `windows/test/aot_worker/run_aot_worker_wine.sh` | The background translation worker (`host/runtime_main.cpp`) alone, with a stand-in for clang, on generated arm64 libraries: parts, the stamp and the shared cache; a worker killed half way (no compiler outlives it, the next run goes on); failures and their waits; a library translated from its profile, at most once an hour; the Settings switch off; four apps' workers on one library and one cache at once; a worker killed at each step of a write (`WINAPK_AOT_FAULT`); a DLL or a cache entry cut short; a library dated in the future; stale temporary files. Needs wine64, mingw-w64 g++, and clang, lld and llvm-strip for the guest libraries |
| `WINAPK_WIN_JDK=<windows-jdk> windows/test/run_app_wine.sh app.apk [frames]` | Stages the runtime and boots an APK headless (needs `curl` and `unzip` for the first resource build); leaves `windows/build/frame.bmp` and `boot.log` |
| `windows/test/build_guest.sh [out]`, `build_e2e.sh <jni include> <javac> <out>` | Build the guest test libraries alone |

Under Wine the paths, user-folders, translation-cache and GL tests fail where Wine differs from
Windows (names with `|` or `:`, junctions, hard links between racing processes, no D3D11 for
ANGLE, so Flutter apps and GL games end at their first surface), and an app that asks
`ConnectivityManager` for the network ends in the JDK's
`NetworkInterface` unless `windows/test/wine_iphlpapi.sh` has given the prefix the function Wine 9
lacks (`run_app_wine.sh` runs it; before a sweep run it once); the tests that link libstdc++ dynamically need
`WINEPATH=/usr/lib/gcc/x86_64-w64-mingw32/<version>-posix`. `stage_runtime.sh` on Linux stages
no `clang/` (there is no Windows clang to copy), so arm64 libraries run on the JIT there.

To see a window under Wine, start `Xvfb :99` and run `winapk-runtime.exe`
with `DISPLAY=:99` and without `WINAPK_HEADLESS`; `xdotool` and ImageMagick's
`import` can drive and capture it. Wine is a convenience: the CI job on a
real Windows runner is what counts. Wine 9 lacks a function the JDK's
`NetworkInterface` code delay-loads, which `run_app_wine.sh` works around by
putting the SunMSCAPI provider first.

## Run an app from a build

```sh
runtime/bin/winapk-runtime.exe --apk app.apk [--jdk "$JAVA_HOME"] [--data <dir>] [--install-only]
runtime/bin/winapk-runtime.exe [--jdk "$JAVA_HOME"] [--data <dir>] --gms <command> [args]
runtime/bin/winapk-runtime.exe [--jdk "$JAVA_HOME"] [--data <dir>] --verify-apk <file>
```

`--apk` takes a plain APK or a split bundle (`.apkm`, `.xapk`, `.apks`).
`--data` defaults to `%LOCALAPPDATA%\Win.apk`; a separate folder keeps a
development run away from the user's library. `--install-only` installs,
converts and writes the icon and name, then exits (3 if the app is running).
`--gms` runs upstream's `com.macapk.gms.GmsCli` (`status`, `selftest`,
`accounts`, `signin-window`, `signout`, `checkin`, `license`, `gcmtoken`...)
with no app; exit 0 done, 1 error, 2 cancelled or bad arguments.
`--gms signin-master` adds a Google account from its e-mail and master token,
read as two lines from standard input, after Google accepts the token; with
Google services on, the broker puts it in the accounts table as the device's
Google account (for tests with a test account, never on a command line).
`--verify-apk` runs `com.winapk.tools.ApkCheck` (`windows/java`): the file's
package, whether its signature verifies (`ApkVerification`, the lineage
included) and whether Google signed it, as `KEY<tab>value` lines; exit 0
Google-signed, 4 another signer, 3 does not verify, 1 not an app. The
launcher's Google services setup installs a file only on 0.

`Win.apk.exe` has the user-facing command line (`<file>`, `--run`) and the
helper verbs `install`, `uninstall`, `runtime-permission`, `special-access`
and `app-details`, documented at the top of `launcher/launcher_main.cpp` and
in the [user guide](../docs/USER_GUIDE.md#command-line).

The runtime and the app data may live in folders with names in any language:
the runtime's manifest makes its ANSI code page UTF-8 (Windows 10 1903 and
later), which is how the JVM gets such paths whole. Older Windows ignores the
manifest; there the runtime passes the JVM 8.3 short names, which covers the
app data, but the runtime's own folder needs plain letters.

## Environment variables for development

The user-facing ones (`WINAPK_PHONE_SIZE`, `WINAPK_RESIZABLE`,
`WINAPK_AOT_CLANG`, `WINAPK_AOT`, `WINAPK_ABIS`, `WINAPK_WEBVIEW2`, `WINAPK_PROXY`,
`WINAPK_ALLOW_SECOND_INSTANCE`, the headless ones) are in the
[user guide](../docs/USER_GUIDE.md#command-line). For development and tests:

| Variable | Effect |
|---|---|
| `WINAPK_HEADLESS=1` | No window; `WINAPK_HEADLESS_FRAME=<file.bmp>` saves the last frame and `WINAPK_HEADLESS_EXIT_AFTER_FRAMES=<n>` exits after that many presented frames (an idle app counts too) |
| `WINAPK_TEST_WORKAREA=<w>x<h>[+<x>+<y>]` | Stands in for the monitor's work area, in pixels; `@file` reads the value from a file at every fit |
| `MACAPK_BROKER=1` or `0` | Overrides the "Use Google services" setting for this run (the system broker and the real Google apps); `MACAPK_BROKER_IDLE_GRACE_S`, `MACAPK_BROKER_IDLE_EXIT=0` tune the broker's idle exit |
| `WINAPK_TEST_ANDROID_ROOT=<dir>` | `winapk-binder-test` only: the android root its JVM gets (the broker checks' scratch data folder) |
| `WINAPK_TEST_JAVA_BASE_PATCH=<dir>` | `winapk-binder-test` only: the java.base overlay its JVM runs with (`stage\lib\java_base_patch`) |
| `WINAPK_JAVA_IO_SHARE_DELETE=0` | java.io opens files as the JDK does, without `FILE_SHARE_DELETE` (`host/java_io_open.cpp`), for comparing |
| `WINAPK_DUMP_VIEWS=1` | Logs the top window's views (`[viewdump]` lines: class, id name, text, content description, clickable, enabled, bounds in the frame's pixels) each time its content changes and then holds still for half a second, with a Compose view's nodes from its accessibility provider, as uiautomator's dump does; a test finds and taps a button from them (`runtime/java/com/macapk/loader/ViewDump.java`) |
| `WINAPK_TEST_INSTALLER=<package>` | Debugging only: `getInstallerPackageName` and `getInstallSourceInfo` answer that installer (e.g. `com.android.vending`, Google Play) for every app, as if a store installed it; nothing in Win.apk sets it (`runtime/java/com/macapk/loader/InstallerOfRecord.java`) |
| `WINAPK_TEST_RESIZE=<w>x<h>` | A second after the first frame, resizes the window to a client area of that many pixels (CI's "Boot an APK in a window") |
| `MACAPK_APK_PATH` | Set by the runtime, not by hand: the installed APK, which `PresentedSdk` reads the app's targetSdk and minSdk from to choose the API level the app is presented (its targetSdk within 29..36, never below its minSdk); `MACAPK_SDK_INT=<n>` forces a level, `MACAPK_SDK_FROM_TARGET=0` presents 29 (still never below the minSdk) |
| `MACAPK_BROKER_TRIGGER_CHECKIN=0` | The broker does not nudge Play services to check in at boot (it nudges at most three times, and not at all once Play services has broadcast `CHECKIN_COMPLETE`) |
| `WINAPK_READY_EVENT=<handle>` | Set by the launcher, not by hand: an inherited event handle, as a decimal number, that the runtime sets once its window has painted the app's first frame (the launcher's starting window waits for it) |
| `WINAPK_START_WINDOW=<hwnd>` | Set by the launcher, not by hand: its starting window for the app, as a decimal number. The app's window opens on that window's monitor, at its top and centred on it (the same rectangle, since both are sized by `host/window_fit.h`), and so takes its place. Ignored when it is no window, hidden or minimized |
| `WINAPK_EMBED_PARENT=<hwnd>` | Set by the launcher, not by hand: a window handle, as a decimal number, that the app's window is created as a borderless child of, filling it (Google Play inside the library window); not a window, or the child cannot be made: a window of its own |
| `WINAPK_DENSITY=<dpi>` | Overrides the density the app sees |
| `WINAPK_WINDOW_RTL=1` or `0` | Overrides the locale's reading layout for the app window's frame |
| `WINAPK_ANGLE_BACKEND=d3d11` or `swiftshader` | Forces a graphics backend (the default is a GPU, then WARP, then SwiftShader) |
| `WINAPK_MAXIMIZE_TURNS=0` | A maximized window gives the app a display of the window's pixels at the normal scale (a tablet-sized one) instead of the phone turned: its short side kept in dp, the frames resampled to the window, and the next start planned at the density that draws it pixel for pixel (`win32_window.cpp`, "a maximized window is the phone turned") |
| `WINAPK_GL_ASYNC_READBACK=1` | A presenting swap (`eglSwapBuffers` on a window surface) reads the frame back asynchronously through a double pixel pack buffer (`host/gl/gl_surface.cpp`) instead of the default synchronous `glReadPixels`: the GL thread no longer stalls on the GPU->CPU copy, at a cost of one frame of latency. Off by default because an on-demand renderer (a Flutter UI) drops its final frame on this path; even when on, it engages only after a sustained run of continuous swaps (`guest_gl.cpp`). `winapk-runtime` logs `window surface frames N: readback X ms present Y ms (async\|sync)` every 120 frames so the two are comparable |
| `MACAPK_LETTERBOX=0` | With `WINAPK_RESIZABLE=0`, stretches the phone over the whole window instead of letterboxing |
| `MACAPK_SCROLL_SCALE=<n>` | Scales the drag a wheel step becomes in an app whose views take no scroll event |
| `MACAPK_WHEEL_SCROLL_EVENT=0` | Every wheel step is the finger drag; no `ACTION_SCROLL` is sent first |
| `MACAPK_HOVER_EVENTS=0` | The pointer over the app sends no hover events |
| `MACAPK_MOUSE_POINTER=0` | The right and the middle mouse button and the hovering pointer are not reported to the app |
| `WINAPK_ARM64_ENGINE=interp` | Forces the arm64 interpreter instead of the JIT |
| `WINAPK_ARM64_STATS=1` | Prints at exit where the guest's time went (host functions versus arm64 code, transitions, interpreter fallbacks), and the same for the eight busiest threads still running |
| `WINAPK_AOT_EH_FRAME=0` | `winapk-aot` leaves out the functions `.eh_frame_hdr` lists (for comparing) |
| `WINAPK_ARM64_FP=exact` | Keeps the arm64 JIT's floating point bit-exact (it uses the host's FMA otherwise) |
| `WINAPK_AOT_MAX_INSNS=<n>` | The function size (arm64 instructions, default 4000) above which a function is left to the JIT instead of translated |
| `WINAPK_AOT_FAULT=<point>[,<point>...]` | Tests only (`include/winapk/aot_files.h`): `nolink` makes the translation's files copies instead of hard links; `kill:<step>` ends the process at once at that step of a write: `seal` (half a DLL's seal), `cache-put` (a cache entry under its temporary name), `install` / `installed` (before and after a DLL is renamed into place), `write` (half a stamp) |
| `WINAPK_AOT_FULL_BELOW=<n>` | The library size (arm64 instructions, default 1,000,000) up to which the background translation translates a library whole; a larger one is translated from its profile (the worker's test sets it to make a small library count as large) |
| `HKCU\Software\Win.apk\AotBackground=0` | No background translation: no worker starts, and a running one stops before its next compile (the launcher's Settings > Apps switch). Translations already made still load; `WINAPK_AOT=0` turns those off |
| `WINAPK_AOT_JOBS=<n>` | The background translation runs up to n compilers at once even while the user is active (otherwise one then, and half the logical processors while the user is away); free memory, battery saver and full-screen programs still count |
| `WINAPK_SYSTEM_CACERTS=0` | Does not build Android's CA store from the Windows roots |
| `WINAPK_USER_FOLDER_TARGETS` | Names the Windows folders the shared-storage junctions point at (tests) |
| `MACAPK_USER_FOLDERS=0`, `HKCU\Software\Win.apk\UserFolders=0` | Turn the shared-storage mapping off (the setting is what the launcher writes) |
| `MACAPK_JVM_OPTS` | Extra JVM options, passed through after Win.apk's own (`-XX:TieredStopAtLevel=4` brings back the C2 compiler, which the runtime leaves out for a faster start) |
| `WINAPK_SHORT_DATA_DIR=0` | The app's data directory by its long name, not the 8.3 one (`-Dwinapk.data.dir`). For Wine, which makes short names up and does not find a new file by one: without it every save through a temporary file fails there. `run_app_wine.sh` and the sweep under `--launcher wine` set it |
| `WINAPK_DEBUG=1` or `0` | Debug mode for this run, over the launcher's Settings > Troubleshooting > Debug mode (`HKCU\Software\Win.apk\DebugMode`): the runtime starts its thread agent (`host/debug_agent.cpp`), which answers the launcher's Threads window on `\\.\pipe\winapk-debug-<pid>` (`include/winapk/debug_protocol.h`): `threads` lists every Windows thread of the process with its kind (java, guest, jvm, runtime, system), Java name and state and where it started; `stack <tid> <jid>` gives one thread's Java frames, contended monitor and native stack. Only for a run of an app and the system broker, not an install |
| `WINAPK_AOSP_GFX=1` | An app's run on AOSP's own graphics, milestone 2's switch (`host/aosp_gfx.h`; off by default, and then nothing changes; on, an app draws through AOSP's `Canvas`, with the gaps [Milestone 2](#milestone-2-the-runtime-on-aosp-graphics) lists): `lib\aosp-graphics.jar` goes before `android-framework.jar` on the class path, `-Dicu.data.path`, `-Dicu.locale.default`, `-Dwinapk.aosp.gfx=1`, `-Dwinapk.aosp.fonts` and `-Dwinapk.aosp.hyphen` are set, class-data sharing is off (the old `--patch-module` flags), and once the host's natives are bound `bin\robolectric-nativeruntime.dll` loads through `host/aosp_gfx_redirect.cpp`: its natives go onto the jar's classes, only `android.graphics.**`, `android.text.**`, `android.util.PathParser` and two helpers (an allowlist); those for `android.database(.sqlite)`, `android.view.Surface`, `android.media.ImageReader` and `PropertyValuesHolder` are refused, Win.apk's stay. One log line: `AOSP graphics: robolectric-nativeruntime.dll loaded in N ms: natives bound N (5 through API 34 adapters), skipped N, refused N ...`. A file missing, or the DLL not loading, and the run goes on with the switch off (`AOSP graphics: off, ...`). Only an app's run, not an install, `--gms`, `--system` or `--verify-apk`. `WINAPK_AOSP_GFX_VERBOSE=1` lists each skipped native and each class routed to the stand-in |
| `WINAPK_CDS=0` or `base` | Class-data sharing (`host/cds.h`): `0` back to the old flags (`--patch-module`, no archive), `base` the JRE's archive without the framework archive. `WINAPK_CDS_DIR=<dir>` puts the framework archive elsewhere (tests share one), `WINAPK_CDS_CLASSLIST=<file>` dumps from another class list (`none`: only the start's own), `WINAPK_CDS_RECORD_DIR=<dir>` writes every app start's loaded classes to `<dir>\<app>.classlist` (for `scripts/merge_classlists.py`), `WINAPK_CDS_VERIFY=1` checks the archive whole at every start, `WINAPK_CDS_DUMP_DELAY_MS` (default 10000) is the wait after the first frame before the background dump |
| `MACAPK_FRAMESPEED_DAMAGE=1` | Partial redraw (off by default): a frame that only invalidations asked for draws the view tree clipped to the area they damaged and hands the window only that rectangle, instead of redrawing and copying the whole window (`docs/ARCHITECTURE.md`, "The view tree's frames"). Unset or anything else, every frame is whole. `MACAPK_DAMAGE_VALIDATE=1` with it draws every frame whole but compares what changed with the area claimed: `[damage-validate] under-damage` names a frame that changed outside it, and the `M6:` line counts `validate: claimed= clean= under=`. `MACAPK_TRACE_DAMAGE=1` logs every 5 s what the frames' damage was (`[damage]`), `MACAPK_FRAME_PHASE_TRACE=1` a frame's time by phase (`[frame-phase]`) |
| `MACAPK_QUIET_CLASSLOAD=0` | Logs a line for every class the app's class loader loads (the runtime sets it to 1, as upstream's launcher does: thousands of lines cost up to 0.4 s of a start) |
| `MACAPK_ALL_PERMS_ASK=0` | Returns the permission groups beyond storage and location to the install-time rule (declared means held), the Mac's behaviour; `MACAPK_MEDIA_PERMS_ASK=0` and `MACAPK_ALLFILES_HONEST=0` keep their upstream meaning |
| `MACAPK_GMS_PRESENT=0`, `MACAPK_GMS_FCM=0`, `MACAPK_GMS_SIGNIN=0`, `MACAPK_GMS_LOCATION=0`, `MACAPK_GMS_DIR` | Upstream's Play services switches and the state folder |
| `MACAPK_GMS_GOOGLE_CERT=0` | Reports the platform certificate for `com.google.android.gms` and `com.android.vending` instead of Google's (apps then see Play services as invalid) |
| `MACAPK_LAYOUT_SWEEP=1` | The screens sweep (`runtime/java/com/macapk/loader/LayoutSweep.java`, [Every screen against Android](#every-screen-against-android)): once `Application.onCreate` has returned, every layout resource of the app is inflated, measured, laid out and drawn at the screen's size, left to right and right to left, instead of the launcher activity starting; each view's place is printed as `PROBE <line>` on stdout, then `PROBE done`, and the process ends. `MACAPK_LAYOUT_SWEEP_THEME=<0x7f... or style/Name>` the theme (default the launcher activity's, else the application's), `MACAPK_LAYOUT_SWEEP_FILTER=<regex>` only the layouts whose name matches, `MACAPK_LAYOUT_SWEEP_PICTURES=<dir>` each case as a PNG |
| `MACAPK_VIRTUAL_CAMERA=0`, `MACAPK_SV_OVERSIZE_HOLDER=0`, `MACAPK_MANIFEST_FEATURE_SPLITS=0`, `MACAPK_LIFECYCLE_*=0`, `MACAPK_CONTEXT_MCLASSLOADER=0` | Switch off individual framework fixes listed under [Changes to the upstream tree](#changes-to-the-upstream-tree) |

The Java framework has many more `MACAPK_*` switches of upstream's; grep
`runtime/java` for the one you need.

## Tests

Every test is a program built by CMake; `ci.yml` runs each of them on
`windows-latest`, and `test/run_*_wine.sh` run the main ones under Wine.

| Program | What it checks |
|---|---|
| `winapk-elfrun <poc.so> <poc_packed.so>` | Two builds of the Android test library (plain RELA, and APS2 + RELR) load and run: argument passing across the ABI boundary, RELATIVE and `R_X86_64_64` relocations, constructors, shim calls, stack-protected code on new threads, a stack-guard load in code the guest writes at run time (patched when it faults), a guest comparator in host `qsort`, a host callback from guest code |
| `winapk-aot-guard-test <scratch dir>` | The translation's crash guard (`host/aot_guard.h`) through the real crash path, each step a child process with `winapk-aot-guard-dll.dll` loaded as its translation: a fault with translated code loaded is counted, a clean run sets the count back, a crash with nothing translated loaded is not counted; after a fault and an `abort()` in a row the next start has translation off and logs the `off for this app` line naming the state file; two crashes on the JIT log `crashes on the JIT too: not the translation` and the next start turns it on for good; another build of the app, or deleting the file, starts over |
| `winapk-cds-test <scratch dir>` | Class-data sharing's pure parts (`host/cds_util.h`): the patch digest equals `bake_jre.sh`'s recipe on the same files, which `--patch-module` flags a JRE's record lets go (a rebuilt patch and an unbaked module keep theirs), the archive's key and file stamps, the per-launch options, the class list merge |
| `winapk-crash-test` | The crash path of `host/crash_report.cpp`: a child with a guest SIGSEGV handler that declines three times, then the report line with the faulting module and offset, a minidump (`MDMP`) in the logs folder, pruning to the last three |
| `winapk-bluetooth-test` | The Bluetooth host's pure parts without a radio: SHA-1 against the FIPS vectors, the parameterized WinRT interface ids derived from type signatures against five published ones, UUID and address conversions, advertisement bytes |
| `ScanRecordTest` (`test/java/`) | `ScanRecord`'s advertisement parser and `ScanFilter` against the built jars |
| `Camera2StillTest` (`test/java/`) | A camera2 still on the virtual camera against the built jars (run by hand, no natives): JPEG sizes in the stream map, a `TEMPLATE_STILL_CAPTURE` into an `ImageReader(JPEG)` giving a one-plane Image that `javax.imageio` decodes at the reader's size, EXIF orientation and thumbnail, one timestamp across Image and result, callback order, `maxImages`, a still beside a repeating YUV reader, a recorder-only session failing to configure |
| `winapk-video-test <test/video dir>` | The Media Foundation video backend without a JVM: the YUV converter, a decoder MFT, the source reader and the player on `test/video/color.mp4` (1 s of H.264 + AAC); decoder checks are skipped where no H.264 MFT exists. Then what recording and `MediaExtractor` stand on: the demuxer on that file (tracks, csd, samples into the decoders), the H.264 and AAC encoders (codec config first, key frames, times; decoded back), and the MP4 writer (its file read back by the demuxer and the source reader). It prints the encoder MFTs the machine has and skips what needs a missing one |
| `winapk-thunk-test` | Native-method thunks with mixed, stack-overflowing and narrow arguments; the variadic entry stub's `va_list`; the tagged zero stub |
| `winapk-jni-test`, `winapk-jni-variadic-test` | The guest `JNIEnv` against a mock JVM; the variadic bridge for all four call kinds and every argument type, through `...` and `va_list` |
| `winapk-linker-test <dir>` | A four-library tree with a `DT_NEEDED` cycle and a runtime plugin: constructor order, symbol resolution and its order (libc at its place in `DT_NEEDED`, the global group before it), `dlsym` scopes, the guest's `dlopen`, `dlsym`, `dlerror` and `dladdr`, a library opened by its Android path |
| `winapk-bionic-test <bionic_guest.so> <scratch dir>` | `test/guest/bionic_guest.c` exercises the shim from guest code: strings, printf and scanf through the System V `va_list`, threads and keys, files and directories, ELF TLS, system properties, locales, futex, an 8 GiB `PROT_NONE` reservation, pipes, eventfd, epoll, sockets, `ALooper`, clocks, `mmap`; the network (loopback only): a UDP datagram, socket options that are mapped, accepted as tuning or refused with `ENOPROTOOPT`, `SO_LINGER`, a multicast group joined with `IP_ADD_MEMBERSHIP` and with `MCAST_JOIN_GROUP` and a datagram received through each (skipped where the host has no multicast route), `if_nametoindex`/`if_indextoname`/`if_nameindex` against `getifaddrs`, edge-triggered epoll on a socket (`EPOLLOUT` once, then a 300 ms wait that takes 300 ms and no processor time; `EPOLLIN` once per arrival and read) and on an eventfd (every write), `EPOLLONESHOT`, `epoll_wait` and `poll` woken by another thread's send; `random(3)` against glibc's sequences (the same generator), `fts` walks, `link`, `popen`/`system` without a shell, `fscanf`, `strptime`, cleanup handlers run by `pthread_exit`, `sched_getaffinity`, FORTIFY's `FD_*`, the binary128 `lroundl` family; the calls the loader's missing-function stub used to answer with 0 (`test_missing_forms`): FORTIFY's `__pwrite_chk`, `__poll_chk`, `__getcwd_chk` and the string forms, `readv`/`writev`, `sendfile`, `truncate`, `renameat2`, `readdir_r`, `rewinddir`, `scandir`, `mkstemps`, `getc_unlocked`, `epoll_pwait2`, `sigwait`/`sigtimedwait`, `pthread_spin_lock`, `pthread_mutex_clocklock`, `sem_clockwait`, `ASharedMemory_create`, `sysinfo`, and that `posix_spawn`, `execv`, `wait4`, `openpty`, `inotify_init1`, `iconv_open`, `regcomp`, `mremap`, and `symlink` fail as Linux reports it, `gethostbyname`, `gethostbyname2` and `gethostbyname_r` on a numeric address, `localhost`, `::1`, a buffer too small (`ERANGE`) and a family with no hosts; and the ones the most installed apps import (`test_popular_imports`): `div`/`ldiv`/`lldiv`, `ilogbf`, `strcasestr`, `asctime_r`/`ctime`, `getrusage`, `times`, `mallinfo`, `__sched_cpucount`, `sched_setaffinity`/`sched_getcpu`, `__progname`/`getprogname`, `sys_signame`/`strsignal`, `__system_property_foreach`, `utimensat`/`futimens`/`utimes` (with `UTIME_OMIT` and a null `times`), `mkdtemp`, `ptrace`, `fchmodat`, `syslog`, `__android_log_buf_write`/`_print` |
| `winapk-arm64-test <dir>` | The arm64 engines: `test/guest/arm64_isa.c` built for the host and for arm64 at three optimization levels must agree on every function, on the interpreter, the JIT and the AOT translation; then `arm64_bridge_guest.c` (calls in both directions, variadics, threads, atexit, `fts` filling arm64's `struct stat`, cleanup handlers); `arm64_flow_guest.c` (returns into an ancestor frame with RET and BR, coroutines, RETAA, deep recursion on a 1 MB thread, `pthread_getattr_np`'s bounds against the guest stack the caller runs on (and an 8 MB thread's), saturating conversions, FRECPS/FRSQRTS, an exclusive-monitor counter shared by translated code and the JIT, the STLR/LDAR litmus test) against values worked out by hand; `arm64_patch_guest.c` (a library patching its own code, and code patched by one thread while another runs it), `arm64_eh_guest.cpp` (C++ exceptions through the NDK's libc++ and unwinder, built when `ANDROID_NDK_HOME` names an NDK), a library unloaded with `dlclose`, one linked at another address, a translation cut short under a stamp that says it is sealed (refused) and the same one whole (loaded); 400 functions of random instructions from `gen_arm64_fuzz.py`, whose results on the JIT and the translation must match the interpreter's (`WINAPK_FUZZ_STATE=<i>` starts every function from function i's states, to bisect a difference); and `arm64_bench.c` (the speed ratios) |
| `winapk-aot-stamp-test` | A library's translation stamp (`include/winapk/aot_stamp.h`), with no file behind it: each kind written and read back, an old stamp read as a short one, a newer build's word; what the loader refuses and when it asks for a sealed DLL; for each kind whether the worker has a step to take now (a changed library or translator, another CPU, the back-off after failures and its limit, a pass to finish, a profile that grew, the interval between two translations from a profile); which stamps the launcher takes for "translated by its profile" |
| `winapk-applink-test` | The App Link sign-in window's decision (`host/applink_match.h`): the navigation that returns to the app is the `redirect_uri` without its query, exactly (scheme and host without case, a default port as none); another path, host, port, scheme, a subdomain, userinfo or the address inside a query is not |
| `winapk-aot-cache-test <scratch dir>` | The arm64 translation's files (`include/winapk/aot_files.h`) in real processes, with `winapk-aot-test-dll.dll` as the translation: a seal against DLLs cut short, emptied, zeroed or changed; eight processes taking, making, putting and loading one cache entry at once (hard links and copies); writers killed at random moments and at each step of a write; broken cache entries deleted and replaced; eviction and replacement while another process has the DLL loaded; the clock (60 days, dates in the future, the size cap); stamps rewritten by four processes while another reads them |
| `winapk-audio-test <test/audio>` | The WASAPI mixer and the decoders |
| `winapk-user-folders-test <dir>` | The shared-storage junction rules |
| `winapk-window-test` | The window geometry decisions (the letterbox, how a frame is shown) without a window, and the size a window opens at (`host/window_fit.h`: the monitor's scale, made to fit the work area at a whole density), which the launcher's starting window takes too. And the window's input decisions (`host/pointer_input.h`): a client position in the app's box, mirrored or not; the Android pointer stream a touchscreen's contacts become (ids, indices, one MOVE per frame, CANCEL); the pinch a Ctrl+wheel becomes; which characters go out with a key (dead keys, AltGr, surrogate pairs) and what a release sends. `windows/test/java/com/macapk/loader/HostTouchTest.java` reads the resulting multi-pointer `MotionEvent`s back on a JVM (by hand: `javac`/`java` with `android-framework.jar` and `macapk-loader.jar`; no CI step runs it yet) |
| `winapk-launcher-state-test` | The launcher's state machines, which hold no lock and make no Windows call (it also builds with the host's compiler; CI's launcher check runs it that way). An app's run (`launcher/app_runs.h`): every path from start to end says the right thing (nothing, the splash goes, "didn't start", "stopped with an error"), Stop at each point of a run, steps out of order change nothing. An install (`launcher/installs.h`): done, failed, left alone because the app runs; Cancel while it waits, while the file is copied, while the runtime converts it, once it is being kept (refused) and after its end (refused, and nothing is left for the next install of that name); the queue's order. The update check and download (`launcher/updates.h`): one check at a time, what each result leaves known and what the InfoBar says of it (a release once), a failed check keeps a newer version found before, the download's progress. The Threads window (`launcher/debug_threads.h`, `include/winapk/debug_protocol.h`): an agent's answer reads back and survives a cut-off reply, every Windows thread is a row with or without an agent, the agent's extra threads are rows too, CPU shares, the search, a sort that holds still |
| `winapk-gl-test` | ANGLE makes a context, clears a pbuffer and the read-back frame has the right colors (WARP or SwiftShader on CI) |
| `winapk-jvm-run <jni_guest.so> <classes>` | A real HotSpot JVM loads a guest library through the linker: its `JNI_OnLoad` registers a native through the guest `RegisterNatives`, another native is bound by name through the `NativeMethodBind` hook, both are called from Java and call back through the variadic bridge. Run with the x86_64 and the arm64 build of the library, and again on the staged `android-framework.jar` for libjnigraphics |
| `test/java/android/net/HostNetworkTest.java` | Run by hand against the built jars (`MACAPK_LOG_LEVEL=7`, no natives): what `ConnectivityManager` makes of the host's default network (`MacApkHostNetwork.fromHost`: the transport for each adapter type, addresses with prefix lengths, DNS servers, no default route, junk in the answer), and that `NsdManager`'s three failures are `FAILURE_INTERNAL_ERROR`, delivered after the call from its own thread |
| `test/java/FrameworkStubsTest.java` | Framework methods that were stubs (`test/apicheck/Completeness.java` lists them) against what Android's do: `AnimatorSet`'s interpolator, `View`'s clip bounds and the drawing cut to them. One check per stub given a body |
| `test/java/PermissionModelTest.java` | `SpecialAccessStore.GroupModel` against PermissionController's rules (grant, deny, deny twice, auto grant, background permissions, legacy targets, App info rows) |
| `test/java/FileSeparatorRewriteTest.java` | Guest code reads Android's `/` from `File.separator` while the JDK's own classes keep the backslash |
| `winapk-binder-test <classpath> com.macapk.loader.BinderTransportTest` | `host/binder_win.cpp` between real JVMs in three processes (`test/java/com/macapk/loader/BinderTransportTest.java`): transact, the OS calling pid and the receiver-derived uid, oneway order, a nested callback, binder identity both ways, a Messenger and its replyTo with the sender's uid in `Message.sendingUid` and a PendingIntent's creator uid, fds both ways, death notification; then the system broker's checks: a process that is no installed app, or that claims another package, is refused, android:exported, the package uid; then the account service's Google accounts (`accounts` child, its own GmsStore folder): an account signed in or out by another process after the service started is mirrored into or removed from the accounts table, asked or not, with the master token as password and `GoogleUserId`, and a row it did not mirror stays |
| `winapk-binder-test <classpath> FileShareDeleteTest` | java.io's opens as `host/java_io_open.cpp` binds them (`test/java/FileShareDeleteTest.java`, with the java.base overlay from `WINAPK_TEST_JAVA_BASE_PATCH`): a file renamed or deleted while a stream has it open, and renamed over a file a stream has open, as on Android; append, truncate, the `FileNotFoundException` text and the rest as the JDK has them |
| `test/aospgfx/run.sh <deps> <out>` (CI job "AOSP graphics") | AOSP 14's own `android.graphics` / `android.text` (android-all, API 34) drawing through Robolectric's native runtime (Skia raster, minikin, HarfBuzz, FreeType, ICU, libpng: `robolectric-nativeruntime.dll`) inside `winapk-runtime.exe --java-main` on the staged JRE, bound by `test/aospgfx/redirect_agent.c`: more than 600 natives bound, the four API 34 signature adapters; rects, a linear gradient, a stroked cubic, `"Hello Skia"` + Hebrew (a font for Hebrew in the fallback chain, both runs inked), a PNG decoded by libpng and drawn scaled, a `BitmapShader` circle, `saveLayerAlpha`, pixels checked; `StaticLayout` lays the mixed line out with the Hebrew run right to left; the picture written as PNG and read back pixel for pixel; then a 1080x2340 list frame timed (a `RESULT` line). Artifact `aosp-graphics`: `aospgfx.png`, `aospgfx-bench.png`, the logs. See [AOSP graphics](#aosp-graphics-milestone-1) |
| Headless boot, class-data sharing, window boot, service host, GMS CLI (CI steps) | The staged runtime boots the F-Droid Sudoku APK headless (30 frames); on the staged JRE, a first start records its classes and starts the framework archive's dump, and the next start maps it (`class-data sharing ON`); and in a window with a resize (`WINAPK_TEST_RESIZE=400x600`); started as a service host (`MACAPK_SERVICE_HOST=1`) and then opened, the open replaces the headless process and draws; `--gms selftest` and `--gms status` pass; `--verify-apk` answers 4 for a test-signed APK, 3 for an unsigned one and 1 for a file that is no app |

The guest libraries are built with plain clang + lld (`test/build_guest.sh`,
`test/build_e2e.sh`), so no NDK is needed; CI builds them on Ubuntu and
hands them to the Windows job.

### The UI gate: only closer to Android

A pull request that touches the framework (`runtime/java`, `java/`) or the
host and loader under it (`host/`, `src/`, `include/`), or the gate itself, is
held by `ci.yml` to "only closer to Android, never further": the UI probe
(`test/uiprobe`) and the screens sweep ([Every screen against
Android](#every-screen-against-android)) of the apps in `test/screens/gate.txt`
(Fossify Gallery and Open Sudoku, each pinned by F-Droid version and SHA-256)
run in Win.apk at the pull request's head and at its base, and
`test/uiprobe/regress.py` compares the two against Android's lines. Any
`REGRESSED`, `FARTHER` or `MISSING` line fails the job; its summary has the
counts and the first 40 of those lines, its artifact (`ui-gate-<target>`) both
sides' `ours.txt` and logs and regress.py's whole output.

- **Android's side** (job "UI gate, Android's side and the base", Ubuntu):
  `test/uiprobe/gate_oracle.sh` builds the probe APK and its `oracle.txt`
  (`build.sh`) and each app's `oracle.txt` and `manifest.env` (`screens.sh
  --skip-ours`, with the head's `build_java.sh` jars for the DEX converter).
  Android does not change with a pull request, so the result is cached by what
  it is made from (the probe's sources, `test/screens/`, `LayoutSweep.java`,
  the DEX converter, the pinned APKs) and built again only when one of those
  changes; Gradle's downloads and Robolectric's android-all are cached too, and
  Maven Central is reached through Google's mirror first
  (`.github/ci/maven-mirror.gradle`; `-Drobolectric.dependency.repo.url` for
  Robolectric's own fetch), since Central rate-limits CI runners. A push to
  `main` runs this job only to fill those caches for the pull requests after it.
- **The base** is the commit the pull request merges into (the merge commit's
  first parent; by hand, the merge base with `main`). When `main`'s own CI run
  of exactly that commit still has its `runtime-stage` (kept 3 days), that is
  the base: its natives and its jars. Otherwise the base is the head's build
  with the base's `android-framework.jar` and `macapk-loader.jar`, built by the
  base's own `build_java.sh` on Ubuntu (cached by the base's source trees).
  That fallback always exists and needs no Windows build of the base, but it
  compares only the Java side: a change to the host alone then compares equal,
  and the job summary says which base it used. Waiting for or rebuilding a
  Windows build of the base instead would double the gate's time, and the
  newest `main` build of another commit would charge `main`'s own later changes
  to the pull request.
- **Win.apk's side** (job "UI gate, `<target>`", `windows-latest`, one job per
  target, from `targets.json`): `test/uiprobe/gate_side.sh` runs the target on
  the base and on the head side by side (`run_ours.sh`, or `screens.sh
  --skip-oracle`, which reads `manifest.env` and needs no Android SDK). Both
  scripts end Win.apk once it has printed `PROBE done` (`run_until.py`: some
  apps' processes hang at exit after it), when no new `PROBE` line has come for
  `GATE_STALL` seconds (default 240: `RUN_UNTIL_STALL`) and at `GATE_TIMEOUT`
  (default 480); a run that stops short (no `done`, or under 90% of Android's
  cases: `gate.py complete`) is run once more on a fresh data folder, as a
  first run sometimes needs. Then `gate.py report` runs regress.py and writes
  the summary. A base whose `macapk-loader.jar` has no `LayoutSweep` (one from
  before the sweep existed) ignores `MACAPK_LAYOUT_SWEEP` and runs the app
  itself, which never ends: `gate_side.sh` sees that, runs nothing and exits 3,
  and the job then checks only that the head's sweep finishes (`gate.py report
  --no-base`, said in the summary). What it costs, from the first run (run
  44569377329): the downloads about 25 s; each app's run about 30 s (Gallery
  31 s, of it 11 s converting its DEX and 12 s to boot; Sudoku 23 s), base and
  head at once on the one runner without slowing each other; the probe about
  as long. So a job takes 1 to 2 minutes after the build; the limits make the
  worst case (both tries of a side running to `GATE_TIMEOUT`) 16 minutes plus
  that, under the job's 25. The DEX conversion runs on each side, as each
  side's own converter would; at 10 s it is not worth sharing. Android's side,
  when cached, takes about a minute and runs beside the Windows build; built
  afresh, about 15.

A pull request that changes what the probe or `LayoutSweep` prints changes the
lines on one side only and fails the gate with `changed` and `MISSING` lines:
read the artifact's `regress.txt` and say so in the pull request. A new app for
the gate is a line in `gate.txt` (`.github/probe/fdroid_fetch.py <package>
<versionCode> <out.apk>` and `sha256sum` give its fields).

To check one app the same way without a machine of one's own, run
`.github/workflows/ui-sweep.yml` by hand (Actions > UI sweep > Run workflow)
with an F-Droid package (and a version) or an APK's URL (and its SHA-256), and
a branch: the oracle on Ubuntu and Win.apk on Windows (the branch's newest CI
build, else `main`'s, `.github/probe/stage.sh`) in parallel, with pictures;
the `ui-sweep` artifact holds `oracle.txt`, `ours.txt`, `diff.txt`,
`summary.txt` and both sides' pictures, and the run's summary shows
`summary.txt`.

## AOSP graphics (milestone 1)

Win.apk draws with its own `android.graphics` on Java2D and lays text out
with AWT. The plan is to draw with Android's own: AOSP's `android.graphics` /
`android.text` classes over native Skia, minikin and HarfBuzz, as
Robolectric's native runtime ships them (`org.robolectric:nativeruntime-dist-compat`:
one library per OS, `robolectric-nativeruntime.dll` on Windows, built with
clang, importing only KERNEL32, ADVAPI32 and the UCRT; software Skia, no GPU
back end). Milestone 1 is `test/aospgfx/`: that library loads and draws inside
`winapk-runtime.exe`'s JVM on Windows, checked by CI's "AOSP graphics" job.

How it binds. The library's `JNI_OnLoad` registers its natives on
Robolectric's `org/robolectric/nativeruntime/<X>Natives` classes. The
redirect agent (`redirect_agent.c`, a C file built with the same clang)
swaps two entries of the JNI function table over JVMTI while the library
loads: `FindClass` maps those names back to AOSP's classes (loaded, not
initialized), and `RegisterNatives` registers one method at a time, skipping
a variant of another API level instead of failing the class and adapting the
four signatures API 34 changed (`FontFamily.Builder.nBuild`,
`BitmapShader.nativeCreate`, `MeasuredText.Builder.nAddStyleRun` and
`nBuildMeasuredText`: each drops an argument new in 34). Loaded with
`System.load` its `JNI_OnLoad` takes a JVMTI environment in the live phase,
which is what the runtime itself would do; `-agentpath` works too. Then the
original table goes back. The parts of ART the classes call
(`VMRuntime`, `NativeAllocationRegistry` on `java.lang.ref.Cleaner`,
`SystemProperties`, `Log`, `Trace`) and `RuntimeEnvironment.getApiLevel()`
are small stand-ins (`test/aospgfx/src/`; `NativeAllocationRegistry` and
`RuntimeEnvironment`, which ship, in `windows/java/aosp_gfx/`); the system
fonts come from the runtime's own `fonts.xml` through
`SystemFonts.getSystemFontConfigInternal`.

Run it. `fetch.sh <deps> [linux|windows]` downloads android-all
`14-robolectric-10818077` and nativeruntime-dist-compat 1.0.18 from Maven
Central's Google mirror (SHA-256 checked, ~330 MB unpacked; CI caches the
folder) and keeps only the platform's library, ICU's data and the fonts;
`run.sh <deps> <out> [bench W H warmup frames]` builds the agent and the
classes and runs the test, with `java` or, with `AOSPGFX_RUNTIME` set, in
`winapk-runtime.exe --java-main` (`AOSPGFX_JRE` the JVM). On Linux it runs the
`.so` through the same agent code. When it fails it runs again with every
skipped native listed (`AGENT_VERBOSE=1`, `registered.txt`) and, in
winapk-runtime, once more with plain `java.exe` of the same JRE, to tell the
library's fault from the process's. The same downloads now ship, for
milestone 2's switch (below); `THIRD_PARTY_NOTICES.txt` has their entries,
written by hand (the jar and the library carry no notice file), and the
licence texts are in `third_party/aosp_gfx_licenses/`.

What it measured (Linux, the 4-vCPU Xeon 2.1 GHz VM of the Skia benchmark,
JDK 21, three runs; the frame is that benchmark's: header gradient, 40 list
rows with a round avatar through a `BitmapShader`, an English and a
Hebrew/mixed line, a cubic path, a sheet in a 75% layer):

| | AOSP Canvas (this) | Win.apk's Canvas today | AWT Graphics2D | Skija, cached shaping |
|---|---|---|---|---|
| Frame 1080x2340, median (p90) | 3.4-4.0 ms (3.8-4.3) | 12.7-13.0 ms (15-18) | 10.9-11.2 ms | 3.7-3.9 ms |
| Frame 1920x1080, median | 3.2-3.6 ms | 11.0 ms | 9.3 ms | 3.4 ms |
| Allocated per frame | 3.8 KB | 2960 KB | 505 KB | 45 KB |

Loading the library takes 54-119 ms (RSS +12 MB, most of it ICU), the system
fonts (143 families, mapped) 128-291 ms (RSS +26-31 MB), the first
`StaticLayout` 15-25 ms, the first frame 10-18 ms; the JVM's RSS goes from
51 MB before the libraries to 104-133 MB after the benchmark (the 10 MB frame
included). minikin caches shaped words, so its text is the "cached" kind.
Under Wine, inside the staged `winapk-runtime.exe` with the Windows DLL, every
check passes too (the agent built with MinGW there).

### Milestone 2: the runtime on AOSP graphics

Behind a switch (`WINAPK_AOSP_GFX=1`, default off), falling back to today's
Java2D classes, which stay as they are:

Done so far: the plumbing around the switch, the seam (step 1) and a first
screen (step 2, below): with the switch on an app reaches its frames drawn by
AOSP's `Canvas` (Skia, minikin, HarfBuzz in the DLL) and presented as today's
are. Under Wine the uiprobe app runs to its end: all 60 cases, 1656 of the
2104 lines Android's (the Java2D path: 1895; table below). With the switch off
nothing changes: the uiprobe app's `ours.txt` is the same byte for byte as
before these changes.

- Build: `scripts/build_aosp_gfx.sh <stage> [deps]`, run by
  `stage_runtime.sh` (`WINAPK_STAGE_AOSP_GFX=0` leaves it out, as
  `windows-release.yml` does until the switch draws apps as well as the Java2D
  path), fetches
  android-all and the native runtime through `test/aospgfx/fetch.sh` (pinned,
  SHA-256 checked; into `thirdparty/aosp-gfx/`, which CI caches) and stages
  `lib/aosp-graphics.jar` (AOSP's `android.graphics.**` without
  `SurfaceTexture`, the `android.text` classes named below and `FontConfig`,
  `android.util.PathParser`, the few classes of android-all those name that
  the framework jar lacks, found with `jdeps` against the stage's own
  `android-framework.jar`, never `com.android.internal.R`, `android.app` or
  `android.animation`; and `windows/java/aosp_gfx`'s stand-ins, about 500
  classes, 1 MB), `bin/robolectric-nativeruntime.dll` (12 MB: its COFF
  symbol table, 3.3 MB that nothing reads, stripped with LLVM's strip),
  `bin/aosp/icu/icudt68l.dat` (16.4 MB: ICU's data without the trees the
  DLL cannot open, rewritten by `scripts/TrimIcuData.java`),
  `bin/aosp/fonts/` with `fonts.xml` and only the fonts it names (47 MB, the
  Noto CJK Regular 19 MB of it), `bin/aosp/hyphen-data/` (2 MB) and the
  licences (`bin/licenses/aosp-graphics/`). 79 MB on disk, down from 210 MB
  as the dist has them: what was left out is what the native runtime cannot
  read, and the test's picture is the same byte for byte with and without it
  ([Footprint](#footprint)). The installer packs the stage whole (`[Files]`
  takes `bin\*`).
- Host: `host/aosp_gfx.cpp` decides at an app's start (the files there, the
  DLL loads), puts the jar first on the class path and the ICU and font
  properties in, and after the host's natives and SQLite's are bound loads
  the DLL with `Runtime.load0` for a class of the jar (its `JNI_OnLoad` finds
  classes through that class's loader) inside `host/aosp_gfx_redirect.cpp`'s
  redirect: the test's agent made an allowlist. `FindClass` maps the
  library's `<X>Natives` names to AOSP's classes only in the allowed
  packages, and the real names of what Win.apk keeps (`android.database`,
  `android.media.ImageReader`, `android.view.Surface`, `PropertyValuesHolder`,
  `OperationCanceledException`) to a stand-in (`java.lang.Void`) whose
  registrations are dropped and whose field and method lookups answer an ID
  nothing uses: the library looks those classes' members up as it registers
  and aborts the process when one is missing, and a method lookup would run
  Win.apk's static initializer before `Boot`. A lookup on any other class of
  Win.apk's that finds nothing is answered the same way and logged. On
  Win.apk's framework (Linux, the `.so`, the same code): 773 natives bound
  (4 through the adapters), 63 skipped, 95 refused, and a `Bitmap` drawn
  through `Canvas` reads back its pixel. Under Wine, in an app's run: 774
  bound, 5 through adapters (the fifth, `Paint.nSetElegantTextHeight`, goes
  the other way: the library has only API 35's int form, and TextView sets it
  from every text appearance), 62 skipped.
- The first screen (step 2). Why it stopped before: HWUI's
  `Typeface::resolveDefault` aborts when a paint with no typeface measures
  text and no default was ever set (`LOG_ALWAYS_FATAL_IF(src == nullptr &&
  gDefaultTypeface == nullptr)`: never set, not set twice). AOSP 14's
  `Typeface` static initializer only preloads font files; the default comes
  from `setSystemFontMap`, which the zygote (or Robolectric's
  `loadPreinstalledSystemFontMap`) calls, and nothing did. The `[Fatal Error]
  :1:1: Invalid byte 1 of 1-byte UTF-8 sequence` lines were nine-patch PNGs fed
  to a text XML parser: `Resources` builds a `NinePatchDrawable` and calls
  `macapkSetOpticalInsets` / `macapkSetDensities` on it, which AOSP's class has
  not; the `NoSuchMethodError` was swallowed and the raster branch fell through
  to the XML inflater with the PNG's bytes. What is in now:
  - `GfxBackend.get()` is `AospGfx` when `-Dwinapk.aosp.gfx=1`
    (`GfxBackend.isAosp()`), `Java2DGfx` otherwise, chosen once. `AospGfx`
    names no `macapk*` member and no `MacApk*` graphics class; what only
    AOSP's classes have (`Typeface.setSystemFontMap`, `SystemFonts`, a
    drawable's state fields) it reaches by reflection, since it compiles
    against Win.apk's `android.graphics`. Bidi, replacement spans and
    `SurfaceTexture` (Win.apk's, kept out of the jar) go to `Java2DGfx`, whose
    classes for them link on either side.
  - Fonts: `Boot`'s `materializeSystemFonts` builds the families of
    `bin\aosp\fonts\fonts.xml` (`SystemFonts.getSystemFontConfigInternal`,
    `buildSystemFallback`, `buildSystemTypefaces`) and hands them to
    `Typeface.setSystemFontMap`, which sets the default; that folder is then
    `/system/fonts` for native code too.
  - The frame: `ViewCanvasDriver` draws the view tree in software, as
    `View.draw(Canvas)` does into a bitmap on a device (Win.apk's `View` has no
    `RenderNode` display lists; HWUI's `RenderNode`/`RecordingCanvas` and its
    `ThreadedRenderer` are not used), into `GfxBackend.createFrameBitmap`: an
    AOSP `Bitmap` (Skia raster, ARGB 8888) with a `Canvas` over it. It is
    presented as today: `argbBuffer` copies the pixels out with
    `Bitmap.getPixels` (unpremultiplied ARGB, the Java2D bitmap's layout;
    `liveArgbOrNull` is null, so the copy is the path) and the host's
    `presentBitmap` takes them. One copy per present; `copyPixelsToBuffer`
    into a direct buffer and a premultiplied present would save it.
    `Surface.lockCanvas`, `SurfaceView` and `TextureView` canvases, `Boot`'s
    and `GlideRescue`'s bitmaps take the same `createFrameBitmap`.
  - Members Win.apk's framework called that AOSP's classes have not, found by
    a linkage check of the two jars in class-path order (member by member,
    access included), now go through the seam: `Bitmap.createBitmapWithCanvasBacking`,
    `GradientDrawable.MACAPK_SHAPE_RING`/`_TINT` (`readsShapeRing`/`readsShapeTint`),
    `Drawable.getTintList` (`tintList`), the `VectorDrawable(Resources, int,
    Theme)` and `(Element, Theme, Resources)` constructors (`vectorFromResource`,
    `vectorFromElement`), `RotateDrawable(Drawable)`, `LayerDrawable()` and
    `RippleDrawable()` (package-private in AOSP: `IllegalAccessError`), and
    `AnimatedVectorDrawable.macapkSetInner` (`animatedVector`).
    Left: `Canvas.sGlobal*` (a Compose trace under
    `MACAPK_TRACE_COMPOSE_FALLBACK`), `RippleDrawable.getColor` (`View`
    catches it), `android.view.RenderNode` and `Layout.Builder` (no caller on
    a first screen).
  - The other way, hidden members AOSP's classes call that the framework
    lacked, added as AOSP has them: `TextUtils` (`couldAffectRtl`,
    `doesNotNeedBidi`, `obtain`, `recycle`, `getEllipsisString`,
    `packRangeInLong` and its two unpackers, `removeEmptySpans`,
    `nullIfEmpty`), `TextPaint.setUnderlineText(int, float)`,
    `SpannableStringBuilder.getSpans(..., boolean)`,
    `ColorStateList.hasFocusStateSpecified`/`getColors`,
    `StateSet.containsAttribute`, `TypedArray.extractThemeAttrs` (null: this
    TypedArray resolves theme attributes when obtained),
    `Choreographer.getFrameIntervalNanos`, `Parcel.read/writeString8`, and
    `PropertyValuesHolder.PropertyValues` with `getPropertyValues` and
    `getValueType` (AOSP's `AnimatedVectorDrawable` constructor makes its
    render-thread animator, which names them). `build_aosp_gfx.sh` takes the
    classes of the taken packages that jdeps left out (it filters a class's
    own package by default): `AutoGrowArray`, `SpanSet`, `AndroidBidi`,
    `PackedIntVector`, `GradientColor`'s factory... (`View$AttachInfo`, a
    nested class of Win.apk's own, never). `AndroidBidi` calls
    `android.icu.text.Bidi`, which the framework has not: a stand-in on
    `java.text.Bidi` (`windows/java/aosp_gfx/android/icu/text/Bidi.java`).
  - Vectors: `AospVector` builds AOSP's own `VectorDrawable` from the element
    (the platform's compiled files through `Resources.macapkDrawableXml`):
    `VGroup`, `VFullPath`, `VClipPath`, `PathParser.PathData` and the native
    tree, the members AOSP's inflate and animators use. A look-alike would not
    do: AppCompat refuses to start a widget (`checkVectorDrawableSetup`:
    "This app has been built with an incorrect configuration") unless its
    test vector's class is exactly `android.graphics.drawable.VectorDrawable`.
    Not yet: gradient fills, theme re-application, the animators.
  - `Gravity.apply` places by AOSP's rules (FILL stretches, AXIS_CLIP clips)
    under the switch only: AOSP's `BitmapDrawable` takes its destination and
    optical insets from it; the Java2D side keeps its answer.
  - `Resources` logs, under the switch only, a drawable that did not inflate
    on AOSP's classes, where it used to stand an empty vector in silently.

  The probe, by case (lines that match Android's within compare.py's 1 dp;
  Java2D = the switch off):

  | Cases | Lines | Java2D | AOSP |
  |---|---|---|---|
  | theme, text, styles, linear_weights, frame_gravity, relative, edit_compound, custom_params, icons, lib_constraint, lib_images, clip_bounds, text_baselines | 468 | 422 | 422 |
  | drawables | 356 | 325 | 322 |
  | text_gravity, lib_text_lines | 60 | 52 | 22 |
  | `*_material` (4 themes) | 520 | 516 | 376 |
  | `*_appbar` (3) | 84 | 68 | 38 |
  | `*_list` (3) | 432 | 360 | 348 |
  | `*_dialog` (4) | 184 | 152 | 128 |
  | total | 2104 | 1895 | 1656 |

  Text widths now match Android's where Java2D's were half a dp off
  (`lastw=30.1`, Android's, against 30.5), but a `TextView` with more than one
  line measures 2 to 2.5 dp a line short (`size 344x85` for Android's 95,
  while its `Layout`'s height, AOSP's `StaticLayout`, is 95): `TextView`'s own
  line arithmetic over `Paint.getFontMetricsInt` against AOSP's metrics, and
  its fallback line spacing is the AWT half (`resolvedFont` is null here).
  Everything placed below such a view moves with it, which is most of the
  text and material rows.
- Still to do (step 2's rest, then 3): `TextView`'s line heights from AOSP's
  per-line extents (`Paint.getFontMetricsInt(text, ...)`, or `TextView` on
  `StaticLayout`); `Resources` handing AOSP's drawables a parser so they
  inflate themselves (that needs `com.android.internal.R.styleable` with
  framework-res's ids, and `TypedArray.getComplexColor`, so `ColorStateList`
  a `ComplexColor`); an app's font families (`res/font`: one file is chosen,
  the family's other faces not built); `<animated-image>`; the present
  without the copy. `dalvik.system.VMRuntime.addressOf` is the framework's
  (0): in the jar only `PathIterator` (API 34's `Path.getPathIterator`) uses
  it, so the text classes do not need the test's direct-buffer kind. HWUI's
  surface natives read `android.view.Surface.mNativeObject`, which Win.apk's
  `Surface` has not (bound against the stand-in, and not called: frames are
  software). Compose (`drawRenderNode` on a software canvas throws in AOSP)
  and `SurfaceTexture`'s producers on native pixels are step 3.

- Where the classes come from. One `android.graphics.Canvas` per JVM: the
  app links against the name, so it is one implementation or the other for
  the whole run, chosen at JVM start. With the switch on the runtime puts a
  jar of AOSP's classes before `android-framework.jar` on the class path
  (first wins): `android.graphics.**` (drawables, fonts, text and animation
  included), the parts of `android.text` that measure and lay out (`Layout`,
  `StaticLayout`, `BoringLayout`, `DynamicLayout`, `MeasuredParagraph`,
  `PrecomputedText`, `TextLine`, `Hyphenator`) and `android.util.PathParser`,
  taken from android-all 14 at build time, plus Win.apk's few patches.
  Measuring and drawing text move together, or line breaks and glyphs would
  disagree. `host/` loads the redirect (as `JNI_OnLoad` does here, on the
  runtime's own JavaVM), then the DLL, from `bin/` beside ICU's data and the
  fonts. Its class list must become an allowlist: the DLL also registers
  natives for `android.database` and `android.database.sqlite` (46, which
  Win.apk has from `native_sqlite.cpp`), `android.view.Surface` (21),
  `android.media.ImageReader` (9) and `android.animation.PropertyValuesHolder`
  (12), which must stay Win.apk's; those it should leave unregistered.
- What blocked a straight swap: Win.apk's `android.graphics` / `android.text`
  declare 306 `macapk*` methods (156 private) and 10 `MacApk*` classes; the
  rest of the framework called 46 of those methods at 89 places, read or set
  3 of their public fields (`Canvas.macapkDrawOps`,
  `Outline.macapkPathShadowRadius`, `VectorDrawable.macapkUnresolvedStub`) at
  14, and named 5 of the classes 18 times in code, in 21 files. Since step (1)
  every one of them goes through `GfxBackend` (below). By the class they
  belong to:
  - `SurfaceTexture` (16 methods, 29 calls: camera, video, `TextureView`, the GL window;
    `macapkAttachCameraSource`, `macapkQueueFrame`, `macapkLatchBitmap`...).
    AOSP's are BufferQueue natives the DLL does not have: keep Win.apk's
    `SurfaceTexture` (and `Surface`) over the AOSP `Bitmap`.
  - `Bitmap` (3 methods, 13 calls: `macapkArgbBuffer`, `macapkLiveArgbOrNull`, `macapkAwtImage`
    in `ViewCanvasDriver`, `Surface`, `SurfaceView`, `WindowOverlayStack`,
    `GLUtils`), and `Canvas.macapkDrawOps` (4, whether a frame drew ink).
    AOSP's pixels are native: the frame goes to the window through
    `copyPixelsToBuffer` into a direct buffer (one copy, as
    `GetIntArrayElements` is today), and libjnigraphics (`src/jnigraphics.cpp`,
    `AndroidBitmap_*` on the `int[]`) needs the same, since the DLL exports
    no `AndroidBitmap_*`.
  - `Canvas` / `Outline` (6 methods, 14 calls: `macapkClipChildRect`,
    `macapkQuickRejectChild`, `macapkGetCurrentTx/Ty`, `macapkDeviceOrigin`,
    `macapkClipPath`, in `View`, `ViewGroup`, `ImageView`, `ViewCanvasDriver`;
    and `Outline.macapkPathShadowRadius`, 4, in `View` and
    `WindowOverlayStack`): Win.apk's view drawing over its own display lists
    (`MacApkDisplayList`; `View.macapkApplyTransform` is `View`'s own).
    AOSP's `View` draws through `RenderNode`/`RecordingCanvas` natives the DLL
    has (HWUI's, software pipeline), so these become the AOSP calls they
    stand for.
  - Text (5 methods, 10 calls, and the 18 mentions of 5 classes): `TextView`
    names `MacApkFont`, `MacApkBidi`, `MacApkReplacement` and calls `Layout.macapkMeasureSpanned`,
    `Paint.macapkResolvedFont`; `Resources` builds fonts with `MacApkFont`,
    `MacApkFontFamilies`, `Typeface.macapkFromStream` / `macapkFromAppFamily`;
    `Boot` sets the system fonts up with `MacApkSystemFonts` (which uses
    `MacApkHostFonts`); `TextUtils.macapkIsLocaleRtl` (4, in `View` and
    `Configuration`).
  - Resources' drawable inflation (8 methods, 14 calls): `Color.macapkParseResourceColor` (4),
    `NinePatchDrawable.macapkSetDensities` / `macapkSetOpticalInsets`,
    `GradientDrawable.macapkSet/GetUseLevelForShape`,
    `AnimatedVectorDrawable.macapkSetInner`, `AnimatedImageDrawable.macapkSetBitmap`,
    `VectorDrawable.macapkPaintsNothing` (4) and `macapkUnresolvedStub` (6):
    AOSP inflates its drawables itself from the XML, so most of these go away
    with `Resources`' shortcut.
  - Debug dumps in `ViewCanvasDriver` (`macapkDump*`, `macapkStatsDump`: 8 methods, 9 calls):
    drop under the switch.
- The seam (step 1, done). `runtime/java/com/macapk/loader/gfx/GfxBackend.java`
  is the one interface those calls go through, `GfxBackend.get()` the backend
  of the JVM, grouped as above: present a frame (a `Bitmap`'s pixels, the
  draw-op count), fonts and `Typeface` creation (the system fonts, a `Typeface`
  from a stream or an app's family, a font file's face as a neutral
  `GfxBackend.FontFace`), the view's clip and transform, text (locale
  direction, spanned measuring, the AWT font helpers `TextView`'s fallback
  line spacing uses, bidi, replacement spans), `SurfaceTexture`, the drawable
  setters, the debug dumps. `Java2DGfx` beside it is today's implementation:
  each method calls the `macapk*` method or `MacApk*` class it stands for and
  nothing else, so behaviour is unchanged (the UI probe, the screens sweep on
  Fossify Gallery and Open Sudoku, `FrameworkStubsTest` and `DamageRectTest`
  give identical output before and after). Inside `android.graphics` /
  `android.text` the classes still call each other directly. A grep for
  `macapk*` members of those two packages, or `MacApk*` classes, outside them
  and `com/macapk/loader/gfx/` finds only comments and other classes'
  members of the same name (`View.macapkApplyTransform`, `ImageReader`'s).
- The order. The swap is all or nothing for the package (one `Canvas` per
  JVM), so the steps are about what has to be ready before the switch can
  draw a screen, and what after. (1) A seam, under Java2D, with no change in
  behaviour: every outside call above goes through one interface (present a
  frame, set the system fonts up, a `Typeface` from a stream or an app's
  family, the view's clip and transform, the drawable setters), implemented
  today by the Java2D classes, so the AOSP side needs adapters and no patched
  copies of AOSP classes. Done: `GfxBackend`, above. (2) Under the switch, a first screen: the frame
  presented from an AOSP `Bitmap`, the fonts from `fonts.xml`, `TextView` and
  `Layout` on AOSP's text (with `Paint`: they move together), `Resources`'
  colours and drawables (AOSP inflates its own from the XML), the view's
  clip and transform through AOSP's `RenderNode` and `RecordingCanvas`.
  (3) What is not on a first screen: `SurfaceTexture` (kept Win.apk's, left
  out of the AOSP jar, over the AOSP `Bitmap`: camera, video, `TextureView`),
  libjnigraphics and `GLUtils` on native pixels, `PictureRecorder`/PDF. The
  debug dumps are dropped under the switch. Each step is checked by the
  smoke set run both ways, and by this test's pixels.

After graphics: AOSP's resources and views. The plan is in
`docs/research/aosp-views.md` (`PORTING.md` M11). It covers what those layers
need underneath, the seams for Win.apk's own additions, and the steps behind
`WINAPK_AOSP_RES` and `WINAPK_AOSP_VIEWS`. Its spike is
`test/aospviews/` (`fetch.sh`, `run.sh`; Linux, plain JDK 21, not in CI yet).
It runs AOSP 14's `Resources` / `TypedArray` / `Theme` over Robolectric's Java
port of libandroidfw, which the native runtime does not carry. The natives are
rewritten at build time into Java calls, with no instrumentation at run time.
AOSP's `LayoutInflater` and widgets then run on this test's graphics. Against
the uiprobe oracle:

- theme lines: 29 of 29 equal (Win.apk's framework: 19);
- 8 layout cases: 44 of 44 lines equal and 8 of 8 pictures pixel-identical.

Two rules for every AOSP layer, this one and those after it (resources,
views...), so that moving onto AOSP's code does not make Win.apk heavy:

- One shared copy of every AOSP file per install, in the program folder
  (`bin\aosp\`, `bin\`, `lib\`), never one per app or per data folder; what
  is made from them at run time (the class-data sharing archive, font caches)
  is made once per data folder, like `cds\` and `cache\` today. Only what the
  code can read is staged (the fonts `fonts.xml` names, the ICU trees the DLL
  can open): see [Footprint](#footprint).
- When an AOSP layer becomes the default, the Win.apk implementation it
  replaces is deleted in that change (or the one right after), not kept
  beside it behind a switch: two implementations are twice the disk, both
  on the class path, and twice the tests. The switch lives only while the
  AOSP side cannot yet draw what the old one does.

## Footprint

Win.apk has to stay light: a download and an install folder of a few hundred
MB, and an app in a few hundred MB of RAM, on a PC with 4 GB. This is what it
takes today, how it was measured, and the budget CI holds it to.

On disk (November 2026, the files' sizes summed, MB = 10^6 bytes; a CI
stage as the Package job packs it):

| | Without the AOSP files (a release) | With them, as the dist ships them | With them, trimmed (now) |
|---|---:|---:|---:|
| Installed folder | 465 MB | 676 MB | 545 MB |
| of it the AOSP graphics files | | 210 MB | 79 MB |
| Installer (`Win.apk-Setup.exe`) | 125.5 MB | 233.1 MB | about 163 MB |

The installer sizes are CI's `winapk-setup` artifacts (run 44545762483,
before the AOSP files were staged, and 44557131107, after); the trimmed one
is an estimate until CI packs it: `xz -9` makes 104.7 MB of the dist's files
(the installer grew by 107.6 MB) and 36.6 MB of the trimmed ones, so about
70 MB less. Release v0.8.0: installer 132.8 MB, zip 184.6 MB. Where the
installed 545 MB go: `clang\` 233 MB (the arm64 translation's compiler:
`libLLVM` 156 MB, `libclang-cpp` 63 MB), `jre\` 127 MB, the AOSP files 79 MB
(fonts 47, ICU 16.4, the DLL 12, hyphenation 2, the jar 1), `bin\tools\`
35 MB (`framework-res.apk` 22, `apkeep` 12), the rest of Win.apk about 70 MB.

What a user's data folder adds: once, for all apps, `cds\` (the framework
archive, 40 MB) and `cache\` (16 MB: `android-system-fonts\` 12,
`fonts\` 4, `fwview\` 1); per app, its `apps\<name>\classes.jar` (the
probe app 17 MB, Open Sudoku 5 MB), the installed copy under
`android\data\app\` (the APK again, 6 and 19 MB, beside the library's own
copy in `library\`) and, for an arm64-only app, its extracted libraries and
their translations (Open Sudoku: its three libraries 38 MB, two
translations 25 MB, hard links of the `aot-cache\` entries, which apps with
the same library share; its 33 MB OpenCV library waits for a profile; 114 MB
in `lib\arm64\` at the peak of a translation, its temporary files). The arm64
translation's compiler runs as one or two `clang.exe` at about 400 MB each,
in the background, after the first frame. The AOSP files add nothing to a data
folder: with the switch on class-data sharing is off, so no second archive.

Memory (under Wine 9, so relative numbers: Windows' own come from the smoke
test now, below). The runtime's process, peak resident set polled every
0.2 s (`VmHWM`), and the working set the runtime prints at the first frame:

| Run | Peak | At the first frame |
|---|---:|---:|
| UI probe, switch off, archive made (a second start) | 377 MB | 377 MB |
| UI probe, switch off, `WINAPK_CDS=0` | 370 MB | 368 MB |
| UI probe, `WINAPK_AOSP_GFX=1`, up to its abort before the first frame (trimmed files: 322 MB) | 324 MB | (none) |
| Open Sudoku (smoke set, arm64), first start | 413 MB | 408 MB |

A first start also runs helpers as processes of their own, one after the
other: the install helper (about 90 MB), the dex conversion (`--java-main`,
0.4 to 1.4 GB at its peak in these runs: its heap is the JVM's default, a
quarter of the RAM, so on this 16 GB machine it collects late; a limit of its
own would hold it down on any PC), and after the first frame the archive
dump (a JVM of about 0.4 GB, once per data folder) and the arm64 translation
(26 MB, and its compilers, above). In AOSP's own test (`test/aospgfx`, on
Linux) the native runtime adds 12 MB resident when it loads and setting the
system fonts up from `fonts.xml` 31 MB (the fonts are mapped), the same with
the trimmed files as without.

What was cut, without changing what an app sees (`build_aosp_gfx.sh`):

- Fonts, 160 to 47 MB: only those `fonts.xml` names. The dist also carries
  fonts for Robolectric's older SDKs: NotoSansCJK's six other weights
  (107 MB; a device has the Regular alone and minikin makes the bold),
  `DroidSansFallback`, `NanumGothic`, Lato, Rubik, Arvo. `SystemFonts` opens
  no file `fonts.xml` does not name.
- ICU's data, 25.8 to 16.4 MB (`scripts/TrimIcuData.java`, what
  `icupkg -r` does): ICU is linked into the DLL, so what it can open is
  fixed. Gone are the trees it has no name for (`icudt68l-zone`, `-unit`,
  `-rbnf`, `-translit` are not in it: time zone names, units, spelled-out
  numbers, transliterators), the charset converters (`ucnv_open` and
  `cnvalias` are not linked) and StringPrep and spoof data (`usprep`,
  `uspoof` not linked). Kept: the break iterators, collation (SQLite's, should
  that layer move), locale, language, region and currency names, the root
  bundles (time zone rules among them) and the Unicode properties. The
  script refuses a rule that drops nothing, so ICU data of another version
  gets looked at again rather than trimmed blind.
- The DLL's COFF symbol table, 3.3 MB, stripped with LLVM's strip (the
  sections stay as lld wrote them; the exports the JVM binds by stay).

Checked: `test/aospgfx/run.sh` (on Linux, the `.so` with the same data) draws
the same picture byte for byte and prints the same lines with the dist's
files and with the trimmed ones; the UI probe's `ours.txt` with the switch
off is the same on a stage with either; with the switch on, a run gets as
far with either (the DLL loads, 773 natives bound, the same abort).

Not cut, and why:

- Noto CJK Regular (19 MB) and Noto Color Emoji (9 MB). Windows 10 and 11
  have CJK fonts of their own (Microsoft YaHei, Yu Gothic, Malgun Gothic) and
  Segoe UI Emoji, but with other metrics: an app's Chinese, Japanese or
  Korean text would wrap and measure differently from a phone. Latin,
  Hebrew and Arabic keep Android's Roboto and Noto in any case. The next
  step, if the 19 MB matter, is fetching the CJK font on first need into the
  data folder (one copy for all apps, as above), which needs the framework's font
  setup to wait for it; not done.
- Windows' own ICU (`icu.dll`, Windows 10 1903+) cannot stand in: the DLL
  links ICU 68 statically (`_68` symbols) and reads only its own `.dat`;
  using the system's would mean rebuilding the native runtime.
- The jar (1 MB) is already only what is referenced (`jdeps`).
- The installer is LZMA2, solid, at `max` (`package/winapk.iss`); fonts and
  ICU's data are mostly incompressible, so a bigger dictionary would gain
  little, and the budget below would show what any change does.

### The budget

`scripts/footprint_check.py <stage> [installer]` sums the stage's files by
part and fails when one is over its budget, listing its largest files; CI's
Package job runs it on the stage and `Win.apk-Setup.exe` it just built
(the run's summary shows the table):

| Part | Today | Budget |
|---|---:|---:|
| AOSP graphics files (`bin\aosp\`, the DLL, the jar) | 79.2 MB | 90 MB |
| Java runtime (`jre\`) | 127.3 MB | 140 MB |
| The arm64 translation's compiler (`clang\`) | 232.9 MB | 250 MB |
| Win.apk itself (the rest, `bin\tools\` included) | 105.1 MB | 120 MB |
| Installed, in all | 544.5 MB | 600 MB |
| Installer | about 163 MB | 175 MB |

The CI stage has the AOSP files (`WINAPK_STAGE_AOSP_GFX` defaults to 1);
a release leaves them out until the switch draws an app, so it is below
these. To change a budget, edit `BUDGETS`, `TOTAL_BUDGET` or
`INSTALLER_BUDGET` at the top of the script in the change that needs the
room, say why in its description, and update this table. Raising it is a
decision about the product, not a way to make CI green.

Memory is recorded, not budgeted yet: the smoke test (`test/compat/sweep.py`)
puts each app's peak working set and peak private bytes (the runtime's
process over the whole run, `GetProcessMemoryInfo` every 0.5 s) and its
working set at the first frame into `report.md` and the run's summary. Once
a few runs show how much the numbers move between runners, a limit can go
beside the others.

## The compatibility sweep

`test/compat/sweep.py` boots every app of `test/compat/apps.txt` (open-source
apps from F-Droid, pinned to one version each; about 90 apps covering Java
and Kotlin apps, games, Compose, Flutter, React Native, WebView shells,
native-heavy apps and arm64-only builds) headless, with a fresh data
directory, and reports what blocks them ranked by the number of apps each
problem affects:

```sh
python windows/test/compat/sweep.py --runtime <staged runtime> --out <report dir> \
    [--jdk <dir>] [--apps apps.txt] [--apks <cache dir>] [--frames 60] [--timeout 180] \
    [--launcher wine] [--only pkg,...] [--shard I/N] [--only-from smoke.txt] [--require-drew]
python windows/test/compat/sweep.py --merge <report dir>... --out <report dir>
```

Each app gets one status: `drew` (ran its frames and the last frame has real
content), `blank` (the last frame is one or two colors), `no-frame` (no frame
before the timeout) or `crashed`. The report (`report.md`, `report.json`)
collects the host natives called while still stubbed, the exceptions Java
logged, HotSpot crash files, and for native code the system functions the shim
lacks and the libraries that could not load, and each app's memory: the peak
working set and peak private bytes of the runtime's process (on Windows) and
the working set at the first frame (the runtime's own line, Wine too). Each
line of an app's log (`logs/<package>.log`) starts with the seconds since its start, so an app that
misses the frame budget shows where its time went. `--shard I/N` runs every Nth
app from the Ith, and `--merge` joins the shards' report directories into one.
`--only-from` runs only the packages a file lists, and `--require-drew` makes
the sweep a test: an app that does not draw gets one more try, then the exit
code is 1.

In CI, every pull request that touches the runtime boots the smoke set,
`test/compat/smoke.txt`: ten apps, one of each kind (Java, Kotlin, Compose,
JNI, libGDX, SDL, NativeActivity, React Native, a WebView shell, an
arm64-only build), all of which draw today, with `--require-drew`. Keep only
apps that reliably draw in it. The whole corpus runs only when asked for:
Actions > CI > Run workflow on the branch, with "Full compatibility sweep"
ticked (the default). It gives each app 300 frames (5 s) after its first,
where the smoke set has 150: a slow first start (LocalSend, a Flutter app
whose first frame comes about 2.5 s after the window's on a runner) drew or
not by a few hundred ms at 150, on main as on branches. It runs as four shards on four machines, then joins them:
the ranked report is the run's summary, and the report, the logs and the last
frames are the `compat-sweep` artifact. It fails nothing.

### Every screen: the screens crawl

The sweep sees an app's first screen only; what lies past a login, a
first-run wizard or a menu is never reached. `test/compat/screens_crawl.py`
starts every activity the APK's manifest declares, exported or not, each as
the root of its own headless run, as `am start -n <package>/<class>` would:
the launcher activity first (that run installs the app), then each of the
others through `MACAPK_LAUNCH_ACTIVITY` (`Boot.java`; also
`MACAPK_LAUNCH_ACTION`, `_DATA` and `_EXTRAS`). An app's runs share one data
directory, so it is installed once.

```sh
python windows/test/compat/screens_crawl.py --runtime <staged runtime> --out <report dir> \
    (--apk <file>... | --only pkg,... [--apps apps.txt] [--apks <cache dir>]) \
    [--launcher wine] [--jdk <dir>] [--frames 30] [--timeout 120] [--first-timeout 400] \
    [--max N] [--activities substr,...] [--app-only] [--intents intents.json] [--scale 4] [--keep-data]
python windows/test/compat/screens_crawl.py --reanalyze --out <report dir>
```

Each screen gets `drew`, `blank`, `finished` (it finished and nothing took
its place; one with an intent filter is then started once more with the
filter's first action, as many check the action they came with), `stalled`
(a first frame, then not its frames), `no-frame` or `crashed`, and the report
keeps which activities really started and which drew the last frame (a screen
may send the run to a login), the frames presented, the last frame as a PNG
(`shots/`, a quarter size by default), every Java stack trace in the log, the
"not implemented on Windows yet" stubs reached (with the counts of the
summary at exit when the runtime prints it) and the errors the app's native
code logged. Stack traces are ranked by where they start, the first frame of
the innermost cause past the Java library: in `android.*`, `com.android.*`
or `com.macapk.*` it is Win.apk's to fix, first in the report (with an
app `NullPointerException` on a null a framework method returned, other than
an Intent or Bundle getter, and a `NumberFormatException` on a resource
name, an AttributeSet value a phone spells `@<id>`); in the app's code it is
mostly a screen opened without the extras or state its caller passes, listed
apart as expected.
A framework exception that reports the caller's own input (an SQL error in
the app's statement, an activity no app has, a null the app passed as an
argument) counts as the app's. `--intents` gives a screen its
start, `{"<class>": {"action": ..., "data": ..., "extras": "k=v;k2=v2"}}`.
Per app it writes `<out>/<package>/report.md`, `report.json`, `logs/` and
`shots/`, and `<out>/report.md` and `report.json` across the apps.
`--reanalyze` rebuilds the reports from the kept logs. A screen that is a
fragment of a single-activity app (Navigation) is not an activity and is not
reached this way. Under Wine a stopped run ends with `wineserver -k`, which
ends every Wine process of the prefix: give a crawl a `WINEPREFIX` of its own
when something else runs in it.

`test/compat/startbench.py` measures starts instead: per app and per
configuration (a JRE, environment variables, another staged runtime such as
`main`'s), one first start and then warm starts taken in turns, timed from
outside to the `first frame presented` line, with the working set and private
bytes read then. `docs/research/startup-cds.md` has its numbers.

### What popular apps name that the runtime lacks

The sweep runs open-source apps. For the apps people actually install, two static
checks need no window and no Windows, only the repository built on Linux:

```sh
windows/test/compat/fetch_apkpure.sh                 # popular.txt, with apkeep, into windows/build/popular-apks
python3 windows/test/compat/native_gaps.py . windows/build/popular-apks <report dir>
java -cp windows/build/runtime/lib/macapk-loader.jar com.macapk.dex.Main \
    --classpath windows/build/runtime/lib/android-framework.jar <app.xapk> <dir>/<app>/classes.jar
java windows/test/apicheck/ApiCheck.java windows/build/runtime/lib <out.tsv> <dir>/*/classes.jar   # JDK 24+
```

`native_gaps.py` lists the system functions, variables and libraries the apps'
native code imports that no table of the built `winapk-runtime.exe` names (each
would go to the missing-function stub), ranked by the number of apps. The
converter's own output says which classes it could not translate and which
oversize methods it had to replace with a stub (`STUBBED oversize method`),
and `ApiCheck` lists the framework classes, methods and fields the converted
apps name that `android-framework.jar` does not have. `popular.txt` is the
apps most installed in Israel and worldwide; APKPure's API serves about two
thirds of them (few of the local Israeli apps), and a 32-bit split for some
whatever is asked for. The files are for analysis only and are not committed.

What the first pass (89 apps, 2026-11) left open: the NDK's `AMediaCodec`,
`AMediaFormat` and `AMediaExtractor` (10 apps), `ASensorManager` (10) and
`AHardwareBuffer` (mostly weak references); wide-character stdio (`swprintf`,
`getwc`, in every statically linked libc++ but rarely called) and `<fenv.h>`;
12 oversize methods in 8 apps the splitter cannot cut (a `synchronized` block from end to
end: WhatsApp's `DeepLinkActivity.onCreate`; X's `Item$$serializer.deserialize`; two oversize
`<clinit>`s, one of boxed `Long`s in Instagram, that `ClinitPrecompute` does not model); and, on the
Java side, `android.app.appsearch`, `android.icu`'s date-interval and list
formatters, `ActivityGroup` / `TabActivity`, and a real PDF engine behind
`PdfRenderer`.

### What the framework has but does not do

`ApiCheck` finds what is absent. A method that exists but answers a constant,
or does nothing where Android does something, fails later and quietly: a view
drawn whole that should be cut, an icon missing, a state lost. `Completeness`
compares every method of `android-framework.jar` with Android's own, from
Robolectric's `android-all` (the plain one, not `-instrumented`), and weights
each by how often the libraries apps are built from call it:

```sh
curl -fLO https://repo1.maven.org/maven2/org/robolectric/android-all/14-robolectric-10818077/android-all-14-robolectric-10818077.jar
java windows/test/apicheck/Completeness.java windows/build/runtime/lib/android-framework.jar \
    android-all-14-robolectric-10818077.jar <out.tsv> <AndroidX / Material classes.jar>...   # JDK 24+
```

The libraries' `classes.jar` are in Gradle's cache after the UI probe's build
(`~/.gradle/caches/*/transforms/*/transformed/<library>/jars/classes.jar`);
converted apps' `classes.jar` work too. Each row is `missing`, `constant` (returns
without reading state or calling anything), `noop` (a void method that does
nothing) or `shallow` (under a tenth of Android's bytecode), with how many
references the given jars make to it. The body is measured alone: a method that
hands its work to a helper can look shallow. The first run (2026-11, AndroidX
and Material 1.12) found 164 `constant`, 126 `noop`, 8 `shallow` and 5 `missing`
methods those libraries call; `View.setClipBounds` was one. Check a fix
against Android with the UI probe (`windows/test/uiprobe`).

### Every screen against Android

`windows/test/uiprobe` compares a fixed probe APK. `windows/test/screens` does
it for any app's own screens, including the ones reached only after a login,
without logging in: their layouts are all in the APK. Every layout resource is
inflated in the app's theme (through AppCompat's or Material's view inflater
when the app has one, as an `AppCompatActivity` does), measured, laid out and
drawn at 360x640 dp, left to right and right to left, once on Android's own
framework (Robolectric, over the APK's resources and its classes converted with
`com.macapk.dex.Main`) and once in Win.apk (`MACAPK_LAYOUT_SWEEP=1`), by the
same code (`runtime/java/com/macapk/loader/LayoutSweep.java`, which prints the
UI probe's lines):

```sh
WINE=wine64 windows/test/screens/screens.sh <app.apk> <out dir> <stage>/bin/winapk-runtime.exe \
    [--theme 0x7f.. | style/Name] [--filter <regex>] [--pictures]
```

It writes `oracle.txt`, `ours.txt`, the UI probe's `compare.py` diff
(`diff.txt`) and `summary.txt` (`summarize.py`): first the layouts that inflate
on Android and throw in Win.apk, with the exception, its root cause and where
it was thrown, then those that throw on one side or both, then the layouts with
the most differing lines. Without `WINE` it runs the exe directly (Windows,
MSYS2). The theme is the one most of the app's activities wear
(`manifest_info.py`: a launcher activity is often a splash screen). It needs an
Android SDK (aapt2, platform 34), a JDK 17 or later, Gradle 8.7 or later and
the staged runtime's `lib/` (or `SCREENS_LIB`); Robolectric fetches its Android
into `~/.m2` (`SCREENS_M2`). `--skip-ours` with `SCREENS_LIB` and `-` for the
exe runs Android's side alone, on any system; `--skip-oracle` then reads the
`manifest.env` it left and needs no SDK. Win.apk's run is ended once it prints
`PROBE done` (`test/uiprobe/run_until.py`; some apps' processes hang at exit),
after `SCREENS_TIMEOUT` seconds, or when `RUN_UNTIL_STALL` seconds pass with no
new `PROBE` line (off unless set; the UI gate sets it). What it cannot see: what code does after inflating (a list's
rows, texts from data, views shown or hidden), a `<fragment>` (there is no
activity), and a custom view whose constructor wants state the app sets up at
start; those throw alike on both sides, and only a difference counts.

## CI and releases

| Workflow | Trigger | What it does |
|---|---|---|
| `.github/workflows/ci.yml` | Pull requests touching `windows/` or `runtime/`; by hand; a push to `main` (build only, and the UI gate's caches) | The pipeline below |
| `.github/workflows/ui-sweep.yml` | By hand only: an F-Droid package or an APK URL, and a branch | The screens sweep of one app, Android's side on Ubuntu and Win.apk's on Windows, with pictures ([The UI gate](#the-ui-gate-only-closer-to-android)) |
| `.github/workflows/windows-release.yml` | By hand only, with a tag such as `v0.2.0-preview` | Builds with `-DWINAPK_VERSION=<tag>`, stages, packages `Win.apk-windows-x64.zip` with the JRE from `bake_jre.sh` and `package/README.txt`, smoke-tests the package (boots an APK headless), builds `Win.apk-Setup.exe` with Inno Setup, and publishes both as a GitHub pre-release with `package/RELEASE_NOTES.md` as the notes |

`ci.yml`'s jobs, in the order they can start:

| Job | Runs on | Needs | What |
|---|---|---|---|
| What changed | Ubuntu | | Reads the pull request's files: docs alone run nothing; `launcher/` or `package/` alone build and package but run no app; anything else runs everything, and the UI gate when the framework, the host, the loader or the gate changed. By hand, everything; a push to `main`, the build alone (and the UI gate's caches) |
| Guest libraries | Ubuntu | What changed | The x86_64 and arm64 test libraries and the end-to-end inputs (`test/build_guest.sh`, `test/build_e2e.sh`) |
| Launcher check | Ubuntu | What changed | The launcher compiled with MinGW-w64, warnings as errors: an answer in about a minute |
| Build | `windows-latest` | What changed | Every CMake target with MSYS2 CLANG64 through ccache (the cache is restored from the newest run, `main`'s for a pull request's first one), then `stage_runtime.sh build stage` and `bake_jre.sh` into `stage/jre`. Uploads `winapk-binaries` (every exe) and `runtime-stage` |
| Unit tests | `windows-latest` | Guest libraries, Build | Every test program above, the arm64 AOT translation included |
| Runtime tests | `windows-latest` | Guest libraries, Build | The tests that need the staged jars or ANGLE, the APK booted headless and in a window (`headless-boot` artifact), the GMS CLI |
| App smoke test | `windows-latest` | Build | The ten apps of `test/compat/smoke.txt` (`--require-drew`) on the staged JRE with one framework archive made first (`--cds-prepare`): fails when one no longer draws (`smoke` artifact). Each app's peak working set and private bytes go in the report and the run's summary ([Footprint](#footprint)). The apps' classes, merged, are the `framework-classlist` artifact: copy it to `java/winapk-framework.classlist` when the framework or the smoke set has changed much |
| AOSP graphics | `windows-latest` | Build | `test/aospgfx/`: AOSP's Canvas on Robolectric's native runtime in `winapk-runtime.exe` on the staged JRE (the agent built with MSYS2 CLANG64 clang; android-all and the DLL fetched from Maven Central and cached). Fails when a check fails; the `aosp-graphics` artifact has the picture, the benchmark frame and the logs. [AOSP graphics](#aosp-graphics-milestone-1) |
| UI gate, Android's side and the base | Ubuntu | What changed | Android's lines for the UI probe and `test/screens/gate.txt`'s apps (`gate_oracle.sh`, cached by what they are made from), the base's commit, and the base's framework jars when `main` has no CI build of it left (`ui-oracle`, `ui-base-jars`). [The UI gate](#the-ui-gate-only-closer-to-android) |
| UI gate, `<target>` | `windows-latest` × 3 | Build, UI gate's Android side | One per target: Win.apk at the base and at the head (`gate_side.sh`), then `regress.py`; fails on a `REGRESSED`, `FARTHER` or `MISSING` line, with the first of them in the summary (`ui-gate-<target>` artifact) |
| Sweep 1/4 to 4/4 | `windows-latest` × 4 | Build | Only by hand, with "Full compatibility sweep": a quarter of the corpus each (`--shard`), each with its own APK cache, on the staged JRE with one framework archive (`--cds-prepare`) |
| Sweep report | Ubuntu | Sweep | `sweep.py --merge`: the run's summary and the `compat-sweep` artifact |
| Package | `windows-latest` | Build | The staged folder with its JRE (`winapk-preview`) and the installer (`winapk-setup`); beside the tests, not after them. Then `scripts/footprint_check.py` fails the job when the installed folder or the installer is over its budget ([Footprint](#footprint)) |

A new push to a pull request cancels its run in progress. Nothing is released
automatically. The platform resource APK, ANGLE, the compile cache, the
sweep's APKs and the UI gate's oracle are cached between runs. The runtime is staged into `stage/`,
never `runtime/`, which is upstream's source tree.

Pull requests: one topic each, CI green on Windows before merging, merged
with rebase. [`../CONTRIBUTING.md`](../CONTRIBUTING.md) has the rest.

## Keeping up with upstream

`runtime/` was copied from Mac.apk, not merged, so the two histories share no
commit and `git merge upstream/main` cannot find a base. Sync file by file
instead, with a three-way merge whose base is the upstream commit last synced:

```sh
git remote add upstream https://github.com/kksimp/Mac.apk.git   # once
git fetch upstream
# for each file under runtime/java, runtime/java_base_patch and the shared
# runtime/src files below that changed between BASE and upstream/main:
git merge-file <file> <(git show BASE:<file>) <(git show upstream/main:<file>)
```

The last sync is upstream **1500431** (Mac.apk v1.0.2379, 2026-11); the first
copy was 66cb689 (v1.0.2238). A file only upstream changed is taken as it is,
one only Win.apk changed is kept, and a file both changed is resolved by hand,
keeping every change listed below. Synced: `runtime/java/**`,
`runtime/java_base_patch/**`, `thirdparty/apache-http-legacy/`, and the
upstream C++ Windows compiles: `runtime/src/native_methods.cpp` (its GL
stretch), `egl14_bridges.inc`, `native_sqlite.cpp`. Not synced: upstream's
Mac-only native code (`runtime/src/*.mm`, the other `runtime/src/*.cpp`,
`runtime/translator/`), the Swift launcher, `launcher/SystemBrokerMain.c`, the
Skia dylib, entitlements and plists; Windows has its own counterparts in
`windows/`. Every native a synced class gains needs a Windows answer in
`windows/host/` (for the classes `host_natives.cpp` binds, an unimplemented
one returns 0 / null) or a guard in the Java that catches its absence.

### Changes to the upstream tree

Files in `runtime/` are changed only where a Windows hook is unavoidable, and
every such change is listed here:

- `runtime/java/android/os/MacApkBinderTransport.java`: tries `nativeInit()`
  before looking for upstream's `libmacapk-binder.dylib`, since the Windows
  host binds the transport's natives itself (`windows/host/binder_win.cpp`);
  `callingUidFor` / `setUidResolver`: on Windows the calling uid is derived
  from the OS-reported pid by the receiver, never sent by the caller.
- `runtime/java/com/macapk/loader/WinProcesses.java` (new),
  `WindowsBroker.java` (new), `SystemServer.java`, `SystemClient.java`,
  `AppUids.java`: the system broker on Windows. `winapk-runtime --system`
  started by the first app that needs it; apps started as
  `winapk-runtime --apk`; a process is a package by the broker's record (the
  process it started, or the `--apk` on its command line), and a caller
  argument or registration that names another package is refused and logged;
  `TX_UID_OF_PID` (new) answers an app's question about its caller's uid; the
  registration reply carries the broker's pid, and an app's application thread
  takes calls from that pid only; state under `%LOCALAPPDATA%\Win.apk`
  (`HostAppSupport`). On the Mac (no `WinProcesses`) nothing changes.
- `runtime/java/android/os/Messenger.java`, `android/app/PendingIntent.java`,
  `com/macapk/loader/AppUids.java`, `Manifest.java`: what Google Play
  services' GCM registrar checks before it hands out an FCM token. A Message
  sent to a Messenger carries the sender's uid in `sendingUid` (AOSP's
  `Handler.MessengerImpl`); a PendingIntent carries its creator's uid across
  processes (`getCreatorUid`); a package's uid is fixed once assigned, and the
  Google packages that share `com.google.uid.shared` get it even from a caller
  that has no manifest at hand.
- `runtime/java/com/macapk/loader/AppUids.java`,
  `android/app/AppOpsManager.java`, `android/os/MacApkBinderTransport.java`:
  the uid table (`app_uids.properties`) is read under its file lock too, and
  written only when a package gets a uid. Every `getApplicationInfo` in every
  process used to rewrite it under the lock while `getPackagesForUid` and
  `AppOpsManager.checkPackage` read it without one; a Windows file lock is
  mandatory, so such a read failed, the answer was "no package has this uid",
  and Play services refused its caller ("Unknown calling package name"). A
  refused `checkPackage` and a calling pid with no uid on record are each
  logged once (`[appops]`, `[binder]`).
- `runtime/java/android/content/Context.java`, `ContextWrapper.java`:
  `createContextAsUser` (the one user's own Context; Play services calls it),
  and a wrapper's `getSystemService` that comes back to itself for the same
  name on one thread is answered from the innermost base context, as a
  ContextImpl would (Play services' `GmsApplication` cycled through a wrapper
  around itself into a StackOverflowError).
- `runtime/java/android/app/ServiceRegistry.java`: a service is "being
  created" from its attachBaseContext on, so a start of it from there is
  queued as on a device (Play services' chimera proxies recursed).
  `MacApkPackageManager.java`: with Google services on and the real Play
  services installed, the in-process GMS stand-ins stay out also before the
  process attaches to the broker.
- `runtime/java/android/view/LayoutInflater.java`: an inflated view is
  attached with `addView(view, params)`, as AOSP's inflate and rInflate do
  (an `<include>`'s root with `addView(view)`, as `parseInclude` does), so a
  ViewGroup overriding one overload sees the one a device calls (the Play
  Store's install sheet).
- `runtime/java/android/content/ContentResolver.java`, `SystemClient.java`,
  `SystemServer.java`: with Google services on, `notifyChange` reaches the
  `ContentObserver`s of other processes through the broker (AOSP's
  `ContentService`, by authority), so the Play Store sees the Services
  Framework's settings change when a check-in finishes after it started.
- `runtime/java/com/macapk/gms/GoogleAuthClient.java`, `AccountsService.java`:
  a sign-in from a master token fetches the account's gaia id when Google's
  answer leaves it out (a userinfo token), and the account row gets it as
  `GoogleUserId`; without it Play services' `getToken` refused every app
  ("missing gaiaId").
- `runtime/java/com/macapk/loader/SystemClient.java`: a call from the broker
  that arrives before the process has read the broker's pid from its
  registration waits for it, instead of being refused as a stranger's.
- `runtime/java/android/net/LocalServerSocket.java`, `LocalSocket.java`,
  `LocalSocketAddress.java`: the ABSTRACT namespace (`new LocalServerSocket(name)`,
  `connect(new LocalSocketAddress(name))`), which upstream refuses. Windows has
  AF_UNIX but no abstract names, so a name is the socket file
  `android\dev\socket\a-<hash of the name>`, shared by the apps of the install;
  the `.lock` file beside it, locked while the server socket is open, says
  whether a file left by a killed process may be replaced.
- `runtime/java/android/net/Uri.java`: `Uri.fromFile` of a drive path gives
  the path `/C:/Users/...` (a leading `/`, `/` separators), so the URI's
  string parses back to the same path; `file://C:%5C...` parsed back with the
  path taken for the authority and an empty path (the Play Store's
  downloads wrote to `""`).
- `runtime/java/com/macapk/loader/PackageInstallerBackend.java`: a session's
  dex metadata (`.dm`) and signature files (`.fsv_sig`, `.digests`,
  `.signature`, `.idsig`) are not made splits of the installed bundle, as
  AOSP's session leaves them out of its APKs (the Play Store writes a `.dm`
  with every app).
- `runtime/java/com/macapk/loader/ViewDump.java` (new),
  `ViewCanvasDriver.java`: `WINAPK_DUMP_VIEWS=1` logs the top window's views
  with their bounds after each frame that changed them (a Compose view's
  semantics through its `AccessibilityNodeProvider`), for tests that drive a
  real app's window. `AccessibilityNodeProvider.HOST_VIEW_ID` is AOSP's.
- `runtime/java/com/macapk/loader/AccountsService.java`,
  `ApkVerification.java`: a package signed with the authenticator's key
  manages its account type too (AOSP's `isAccountManagedByCaller` through
  `hasSignatureCapability(..., AUTH)` over the verified signatures and
  lineage), so the Play Store reads Google accounts' user data as on a device.
- `runtime/java/android/content/pm/dex/ArtManager.java` (new),
  `PackageManager.java`: `getArtManager()`, with runtime profiling off (no
  ART); the Play Store's profile upload job failed to start without it and
  its job scheduler retried it without pause.
- `runtime/java/com/macapk/gms/GmsCli.java`, `GoogleAuthClient.java`: the
  `signin-master` verb (an account from a master token, on stdin); a refusal
  from Google's `/auth` is logged with its reason line, not only its status.
- `runtime/java/com/macapk/loader/Manifest.java`, `MacApkPackageManager.java`,
  `android/content/pm/PackageManager.java`: `<permission-tree>`, and
  `addPermission` / `removePermission` inside a tree of the package's own
  (Google Services Framework adds its GOOGLE_AUTH.<service> permissions so).
- `runtime/java/com/macapk/loader/ApkVerification.java` (new),
  `MacApkPackageManager.java`, `android/content/pm/PackageManager.java`,
  `Signature.java`, `com/macapk/gms/GoogleGmsCertificate.java`: APK
  signatures are verified cryptographically with AOSP's apksig
  (`thirdparty/apksig/`, built into `macapk-loader.jar`) before any
  certificate is reported or trusted; upstream read the certificate out of the
  signing block without checking a signature. An installed app's base.apk and
  splits, or a bundle's APKs, must all verify with one signer; an APK that
  does not verify reports no certificate. A v3 key rotation is reported as on
  a device: `GET_SIGNATURES` the oldest certificate of the lineage,
  `SigningInfo` the lineage with its capability flags, `checkSignatures` and
  `hasSigningCertificate` through it. On Windows the system-app treatment of
  `com.google.android.gms`, `.gsf` and `com.android.vending` needs a verified
  APK that Google's certificate signed, directly or as an ancestor in the
  lineage (Play services 26.x is signed by a newer Google key), not just the
  package name; the in-process stand-ins (no APK) are unchanged. Verdicts are
  cached in `%LOCALAPPDATA%\Win.apk\cache\apk-verify\`;
  `windows/java/com/winapk/tools/AppMeta.java` verifies at install.
- `runtime/java/com/macapk/loader/Manifest.java`: a manifest's `@string/` and
  `@xml/` references in a split bundle (`.apks`, `.apkm`, `.xapk`, which the
  broker parses as the library holds them) resolve in the bundle's base APK
  (`resourceApk`); they were looked up at the bundle's top, which has no
  `resources.arsc`, so Play services' `android:process="@string/..."` stayed
  `@0x7f...`.
- `runtime/java/com/macapk/loader/PresentedSdk.java`: never presents a level
  below the app's minSdkVersion (Play services 26.x needs 31); the runtime
  exports `MACAPK_APK_PATH` as upstream's launcher does, so the per-app level
  works at all (without it every app was presented 29).
- `runtime/java/android/os/BaseBundle.java`: copying a bundle that is still
  parcelled copies its bytes instead of unparcelling it, as AOSP's
  `copyInternal` does, so the broker can pass on an intent whose extras hold
  an app's own Parcelables (Play services' `FileApkService` start failed with
  `BadParcelableException` in the broker).
- `runtime/java/com/macapk/loader/SystemServer.java`: the boot check-in nudge
  stops once Play services has broadcast `CHECKIN_COMPLETE`.
- `runtime/java/android/accounts/AccountManager.java`,
  `com/macapk/gms/NativeSignIn.java`: with Google services on and the real
  Play services installed, `addAccount("com.google")` (the Play Store's
  "Sign in") opens Win.apk's own sign-in window in the caller's process and
  answers with the account it added; it went to Play services'
  authenticator, whose answer is an Intent for Play services' own sign-in
  activity that no app on Windows can start in another app's process with
  the authenticator's response binder, so the Play Store waited forever.
  With an Activity (the user pressed a button) the window opens every time.
- `runtime/java/com/macapk/gms/GmsStore.java`: a cached copy of
  `gms_state.properties` is used only while the file is unchanged (another
  process, the launcher's Settings or an app, signs in or out), and the file
  is written beside itself and moved over, so a reader never sees half of
  it; a process kept its first read for good and wrote it back over the
  others' changes.
- `runtime/java/com/macapk/loader/AccountsService.java`: the broker follows
  GmsStore's file (a `WatchService`) as well as each `getAccounts`, mirrors
  a new account at once and broadcasts `LOGIN_ACCOUNTS_CHANGED`, and removes
  a row it mirrored (marked `winapk.mirrored`) once the account is signed out
  of Win.apk; it asks Google for a missing gaia id once per account and run.

- `runtime/java/com/macapk/loader/HostAppSupport.java` (new) and
  `InstallerOfRecord.java`: the small files every app process shares (the
  installer of record, and with Google services on the per-package uids and
  the broker's state) live in the data folder, `%LOCALAPPDATA%\Win.apk`,
  beside `installed_apps.json`, not in a `Library\Application Support` folder
  under the user profile. On the Mac nothing changes. `InstallerOfRecord.get`
  also answers `WINAPK_TEST_INSTALLER`, a debugging override (see the
  environment table).
- `HostAppSupport` users that upstream spells with Mac paths: the caches
  (`MacApkSystemFonts`: the `/system/fonts` Flutter's Skia reads,
  `MacApkFontStore`, `ResourceIndex`'s framework view, `monet/Monet`) go to
  `%LOCALAPPDATA%\Win.apk\cache\`, and the run lock (`RunLock`,
  `CrossAppDoorbell`), `SettingsStore`'s boot count and the per-app input
  settings (`ArrowPad`, `SpaceTap`, `TiltControl`, `KeyBindProfiles`) to
  `%LOCALAPPDATA%\Win.apk`, never `Library\Caches` or
  `Library\Application Support` under the user profile.
- `runtime/java/android/app/NativeActivity.java` and `Activity.java`
  (`macapkInstalledContentView`): the full-window SurfaceView that stands in
  for the window surface is installed when the root is still
  `NativeContentView`, looked at through the FrameLayout root upstream's
  `ActivityWindow.macapkFrameRootFor` (#29) puts above a non-frame content
  view. Comparing the wrapper itself skipped it, so a NativeActivity (SDL)
  never got its window and stayed blank. An upstream bug too.
- `runtime/java/android/os/Build.java`, `com/macapk/gms/CheckinClient.java`:
  on Windows the device is Win.apk: `BRAND`, `MANUFACTURER` and `MODEL`
  "Win.apk", `DEVICE`, `PRODUCT`, `BOARD` and `USER` "winapk", `HARDWARE`
  "x86_64", `DISPLAY` "Win.apk-1", `HOST` "winapk"; `FINGERPRINT`, the `ro.*`
  properties and the GMS clients' user agents follow from them, and the
  check-in's own User-Agent says Win.apk. Upstream reports Apple and the Mac's
  hardware model, which a PC is not; no real phone or Google device is named.
- `runtime/java/com/macapk/gms/McsClient.java`: `WINAPK_PROXY` (a SOCKS proxy
  for the push connection) wins over upstream's per-app `HostProxy` policy.
- `runtime/java/com/macapk/loader/Manifest.java` and `MacApkPackageManager.java`:
  an intent filter's `<data>` ports, paths and ssps are matched as AOSP's
  `IntentFilter` does (beside upstream's `autoVerify`).
- `runtime/java/com/macapk/loader/ClassFileRewriter.java`, `AppClassLoader.java`,
  `NativeLoadBridge.java` (new): in guest classes, `File.separator(Char)` reads
  Android's `/` (Pattern S; `MACAPK_ANDROID_FILE_SEPARATOR=0` turns it off) and
  `System.mapLibraryName` / `load` go through the on-demand native loader
  (Pattern N); beside upstream's Patterns L and J.
- `runtime/java/com/macapk/loader/AppClassLoader.java`: `findLibrary` asked by
  app code (not by `System.loadLibrary` or the framework) answers as Android
  does, `/data/app/.../lib/<isa>/lib<name>.so`, where upstream answers the
  translated library; `loadNativeOnDemand` loads that name through the same
  stub. Only where Pattern N is on.
- `runtime/java/dalvik/system/NativeLibraryPath.java` (new), `DexPathList.java`,
  `BaseDexClassLoader.java`, `DexClassLoader.java`, `InMemoryDexClassLoader.java`
  and `AppClassLoader.libraryOnSearchPath`: a dex loader's `librarySearchPath`
  is kept and `findLibrary` searches it before the app's lib dir, as AOSP's
  `DexPathList` does; a `<zip>!/<dir>` entry's library is copied out to
  `<data>/code_cache/macapk-native-libs/` with the DT_NEEDED it has in that zip
  directory, and loads through an on-demand stub bound to that loader (Play
  services' `CronetDynamite.apk` in a client app). Upstream drops the path.
  `windows/test/java/LibrarySearchPathTest.java` runs on any JVM.
- `runtime/java/android/app/Activity.java`: `Window.setContentView(View)` puts
  the view into the decor's content frame and keeps the decor as the root, as
  AOSP's `PhoneWindow` does (a view added to the decor floats over the content),
  and `findViewById(android.R.id.content)` finds the sub-decor's frame in that
  tree before upstream's frame-root unwrap.
- `runtime/java/android/bluetooth/` (`BluetoothAdapter`, `BluetoothDevice`,
  `BluetoothGatt`, `BluetoothManager`, `le/BluetoothLeScanner`,
  `le/ScanRecord`): Win.apk's `HostBluetooth` versions (below); the pre-API-21
  `startLeScan` / `stopLeScan` scan through the LE scanner, as in AOSP. Upstream later
  wrote a CoreBluetooth backend (`BluetoothBridge`, `BluetoothPeripheralBridge`)
  into the same classes; those files are synced but unused here.
- `runtime/java/android/content/res/Resources.java`: `getIntArray` resolves the
  items of a typed `<array>` (for example a list of color references) instead
  of returning an empty array. Not Windows-specific; worth offering upstream.
- `runtime/java/android/view/ViewPropertyAnimator.java`: starts by itself on
  the next frame when the app never calls `start()`, as on Android. Not
  Windows-specific; worth offering upstream.
- `runtime/java/android/widget/RatingBar.java` and `ProgressBar.java`: a rating
  bar reads `numStars`, `rating`, `stepSize` and `isIndicator` from XML and
  measures one row of stars, as AOSP does. Not Windows-specific.
- `runtime/java/android/widget/ProgressBar.java` (the indeterminate tick): it
  invalidates only while the bar is shown (`isShown()`), as AOSP animates
  only a visible bar. A hidden spinner kept the whole window redrawing 20
  times a second (Google Maps idle: a third of all frames). Not
  Windows-specific.
- `runtime/java/android/view/View.java`, `ViewGroup.java`, `ViewRootImpl.java`
  and `widget/ProgressBar.java` (views that are not on screen):
  `ViewGroup.removeDetachedView` detaches the child from the window and
  reports it removed, as AOSP's does (a RecyclerView row that a layout did not
  reuse stayed "attached" for good and never got `onDetachedFromWindow`); an
  invalidation asks for a frame only when the view can be drawn, that is no
  view from it up to its window's root is hidden without a View animation and
  its tree is in a window (AOSP's `skipInvalidate` and `mAttachInfo` tests;
  `MACAPK_INVALIDATE_SKIP_UNDRAWN=0` turns it off, and
  `MACAPK_TRACE_INVALIDATE=1` lists such invalidations as `skipped`); and
  `onVisibilityAggregated`, plus `onWindowVisibilityChanged` and
  `onVisibilityChanged` at attach and detach, are called as AOSP calls them,
  with `ProgressBar` stopping an animated indeterminate drawable while it is
  not shown (`MACAPK_VISIBILITY_AGGREGATED=0`). Google Maps' idle map redrew
  the whole window 57 times a second for a progress bar and Compose content
  of a row that had left its list. Not Windows-specific.
- `runtime/java/android/view/View.java`, `ViewGroup.java`,
  `ViewRootImpl.java` and `com/macapk/loader/ViewCanvasDriver.java` (partial
  redraw, behind `MACAPK_FRAMESPEED_DAMAGE=1`): the view half ("S1") of
  upstream's damage-clipped frames, which the driver ("S2") had waited for.
  `ViewGroup.drawChild` records the clip each child is drawn under on the
  frame; an invalidation damages that rectangle (its parent's when the view
  has moved, was not drawn or casts a shadow; everything when no rectangle
  can be vouched for) and `ViewRootImpl.macapkConsumeDamage` hands the frame's
  union to the driver. In the driver the flag is read through `ViewRootImpl`
  (on only at `1`), the draw is bracketed by `macapkBeginDraw` /
  `macapkEndDraw`, damage raised during a frame is kept for the next one
  instead of dropped, and `MACAPK_DAMAGE_VALIDATE=1` compares frames in Java
  (the Windows host has no diffing present). Checked by
  `windows/test/java/DamageRectTest.java`. Not Windows-specific.
- `runtime/java/android/database/sqlite/SQLiteDatabase.java`: `BEGIN`,
  `COMMIT` and `ROLLBACK` given to `execSQL` nest in the transaction stack, as
  AOSP's `SQLiteSession.executeSpecial` does. Not Windows-specific.
- `runtime/java/android/view/LayoutInflater.java`: a `<merge>` layout inflated
  without a parent merges into a `FrameLayout` instead of being dropped, so
  `setContentView` of a `<merge>` layout works. Not Windows-specific.
- `runtime/java/android/app/Activity.java`, `android/view/LayoutInflater.java`
  and `android/widget/TableLayout.java`: while `setContentView(int)` inflates
  a `<merge>` layout, `Activity.findViewById` finds the top-level views
  already inflated, as in the content frame PhoneWindow inflates into (a view
  that looks up a sibling in its constructor got null); and a `TableLayout`
  reads `stretchColumns`, `shrinkColumns` and `collapseColumns` from XML.
  Vanilla Music's permission banner lay under its bottom bar with its button
  off the screen. Not Windows-specific.
- `runtime/java/com/macapk/loader/ActivityHost.java`: an activity that calls
  `finish()` in its own `onCreate` is torn down after `onCreate` returns, as on
  AOSP, not in the middle of it; one that calls it in `onResume` after
  `onPostResume` (AOSP destroys it in a later transaction; tearing it down
  inside `onResume` nulled the FragmentManager's host under `onPostResume`,
  "No activity" in sgtpuzzles). Not Windows-specific.
- `runtime/java/com/macapk/loader/TouchDispatcher.java`: the entries the
  Windows window calls for what a Mac reports differently or not at all
  (`windows/host/win32_window.h`, `WindowInput`): `dispatchHostTouch` (a
  touchscreen's contacts and a pen as a multi-pointer stream; one pointer goes
  down the host mouse's path, and `touchEvent` then carries its tool, pressure
  and stylus button), `dispatchHostWheel` (`dispatchWheel`'s `ACTION_SCROLL`,
  and the old finger drag when no view handled it; `deliverWheelToWindows` now
  says whether one did), `dispatchHostPinch` (`dispatchPinch`, not begun under a
  floating window or a drag), `dispatchHostText`, `dispatchHostComposingText`,
  `hostImeInline` and `hostImeAnchor` (composed text and an input method's
  composition through the focused field's `InputConnection`). Upstream's
  callers and its single-pointer path are unchanged.
- `runtime/java/com/macapk/loader/ActivityHost.java`: `macapkHostMinimized`,
  which the window calls when the user minimizes it: the resumed activity is
  paused and stopped, as a task moved to the back (the transitions of
  `macapkMoveTaskToBack`, without hiding the window), and `macapkTaskToFront`
  brings it back on restore. Closing the window needs no change there: the
  host now calls upstream's `macapkTerminate`.
- `runtime/java/com/macapk/loader/ActivityHost.java` and
  `android/app/Activity.java`: an activity that finishes before its first
  `onResume` is driven down only from where it is, as AOSP's
  `performDestroyActivity` does: `onDestroy` alone after `onCreate`, `onStop`
  and `onDestroy` after `onStart` (it got `onPause` and `onStop` too, which
  made androidx start the fragments of an activity on its way out), with the
  androidx lifecycle taken to CREATED first; and `isDestroyed()` turns true
  right before `onDestroy`, not at `finish()`. Not Windows-specific.
- `runtime/java/android/provider/SettingsStore.java`: `Secure.DEFAULT_INPUT_METHOD`
  is seeded with the host keyboard's component, as a device always has a
  current IME (Key Mapper read it without a null check). Not Windows-specific.
- `runtime/java/com/macapk/loader/FragmentInflaterFactory.java`: a fragment in
  a layout is hosted by the activity the inflater belongs to, which also works
  during the first activity's `onCreate`. Not Windows-specific.
- `runtime/java/com/macapk/loader/DomXmlResourceParser.java` and
  `BinaryXmlParser.java`: the resource parser's `next()` skips whitespace-only
  text and comments, as the device's parser over aapt-compiled XML never sees
  them (a `while (next() != END_TAG) nextText()` loop over a text-form resource
  threw "nextText not on START_TAG"), and the compiled-XML reader keeps text
  chunks, so an element's content (`<key>` in a media-browser allow list) is no
  longer empty. Not Windows-specific; worth offering upstream.
- `runtime/java/com/macapk/loader/DomXmlResourceParser.java`: an XML file with
  a `<!DOCTYPE ...>` parses, as on a device, where the pull parser reads past
  the declaration; the DTD and external entities are still never loaded. This
  is also `Xml.newPullParser()`, and it refused such a file outright, so
  AndroidSVG could not read an SVG that starts with the usual W3C DOCTYPE
  (DroidFish drew its board without the pieces, issue #118). Not
  Windows-specific; worth offering upstream.
- `runtime/java/com/macapk/dex/InitRepointPlan.java`: the DEX converter also
  synthesises the constructor R8 deleted when the call site still names the
  allocated class (`new-instance T; invoke-direct T.<init>(D)` with no
  `T.<init>(D)` left, often no class data at all), which ART resolves up the
  superclass chain and HotSpot refuses with NoSuchMethodError. NetGuard's
  main activity and androidx.startup failed on it (170 such sites in one
  DEX). Not Windows-specific; worth offering upstream.
- `runtime/java/android/system/Os.java`: `chmod` and `fchmod` (Key Mapper
  died at start on the missing method). Not Windows-specific.
- `runtime/java/com/macapk/loader/Boot.java` (`writeJavaProcMirror`,
  `writeProcCmdline`): the `/proc` mirror also writes
  `<android root>/proc/<pid>/cmdline`, the process name and a NUL, which
  `/proc/self/cmdline` and `/proc/<pid>/cmdline` name (`paths.cpp`;
  `java.io.FileReader`'s `/proc` hook, `Cocoa_remapProcPath`), and drops the
  directories of pids that no longer run (Breezy Weather reads the file in
  `Application.onCreate`, issue #123). With Android paths
  (`-Dwinapk.android.root`) the `/proc/meminfo` alias `/tmp/mcmeminf` is a
  copy, not a symbolic link, which a standard Windows account may not create.
- `runtime/java/android/preference/Preference.java`: an app subclass that
  overrides `onGetDefaultValue(TypedArray, int)` gets it called, with a
  TypedArray holding `android:defaultValue`, and one that overrides neither
  form gets no default at all, as AOSP's base `onGetDefaultValue` returns null
  (the raw String the base twin returned broke DroidFish's
  ColorPickerPreference, which casts the default to Integer, with a
  ClassCastException). Not Windows-specific.
- `runtime/java/com/macapk/loader/FragmentInflaterFactory.java`: a
  `FragmentContainerView[android:name]` whose app FragmentManager cannot be
  found by name (R8 renamed it) goes through the activity's own
  `onCreateView` override, AOSP's route, instead of the view's 2-arg
  constructor, which throws (Auxio, Tremotesf). Not Windows-specific.
- `runtime/java/android/provider/MediaStoreProvider.java`: a projection may
  hold an aggregate over a known column (`COUNT(is_music)`, `COUNT(*)`,
  `MAX(date_added)`) or an alias (`_data AS path`), as MediaProvider's
  projection greylist allows; before, the aggregate was answered as a NULL
  column and a COUNT over an empty table had no row. Not Windows-specific.
- `runtime/java/android/security/keystore/AndroidKeyStoreKeyFactorySpi.java`:
  delegates to the other providers only, instead of recursing into itself
  until the stack overflows. Not Windows-specific.
- `runtime/java/android/os/Build.java`, `dalvik/system/VMRuntime.java`,
  `com/macapk/loader/Boot.java` (`os.arch`), `InstalledSplits.java` and
  `MacApkPackageManager.java`: the device ABI is the host's, so x86_64 on
  Windows x64, and the native library directory includes `lib/x86_64`. On the
  Mac nothing changes. `Boot.java` also looks for `bin/macapk-translator.exe`.
  On an x86_64 host `Build.SUPPORTED_ABIS` also lists arm64-v8a, which is
  translated to x86-64 (ahead of time, with a JIT fallback), as an x86_64
  Android device with an ARM translator reports it.
- `runtime/java/android/content/Context.java`: `getDatabasePath` also takes a
  drive-letter path as absolute, so a database named by its full path is not
  nested inside `databases\`.
- `runtime/java/com/macapk/loader/CameraBridge.java`: when the capture natives
  are unbound or report no device (no webcam, or camera access is off in
  Windows privacy settings; the webcams themselves come from
  `windows/host/native_camera.cpp`), one virtual back camera delivers
  dark-grey frames, so screens built around a preview open and their other
  controls work. `MACAPK_VIRTUAL_CAMERA=0` turns it off. A real camera always
  wins.
- `runtime/java/android/hardware/camera2/` (`JpegStill.java` new,
  `CameraCaptureSessionImpl.java`, `CameraBackend.java`,
  `params/StreamConfigurationMap.java`, `CaptureRequest.java`) and
  `android/media/ImageReader.java`: camera2 takes stills. The stream map lists
  JPEG and PRIVATE beside YUV_420_888 (same sizes); a request that targets an
  `ImageReader(JPEG)` gets its frame encoded with `Bitmap.compress`, as
  `Camera.takePicture` does, at the reader's size, with `JPEG_QUALITY`,
  `JPEG_ORIENTATION` as the EXIF Orientation tag (pixels as captured) and the
  `JPEG_THUMBNAIL_SIZE` thumbnail; the Image carries the result's
  `SENSOR_TIMESTAMP` and `maxImages` is kept. Before, `getOutputSizes(JPEG)`
  was null, so camera2 apps and CameraX's ImageCapture had no still output.
  The characteristics gained the keys they read (3A mode lists, zoom 1.0,
  pixel array, thumbnail sizes, output counts). Not Windows-specific.
- `runtime/java/android/view/SurfaceView.java`: a SurfaceView laid out larger
  than its window (a center-cropped camera preview) whose holder has no GL
  consumer yet (no renderer, not a `GLSurfaceView`, not the native-window
  owner) reports the view's size and gets `surfaceChanged` for it, as AOSP
  always does. Apps that start their camera preview only after that
  `surfaceChanged` waited forever. `MACAPK_SV_OVERSIZE_HOLDER=0` turns it off.
  Not Windows-specific.
- `runtime/java/android/webkit/MacApkWebViewBridge.java` and `WebView.java`:
  `macapkWantsIntercept` / `macapkInterceptRequest`, through which an engine
  that can hold a request open (`windows/host/native_webview.cpp`, WebView2's
  deferral) offers it to `WebViewClient.shouldInterceptRequest` and serves the
  app's response. Cordova and Capacitor apps (`WebViewAssetLoader`,
  `https://localhost/`) failed with `net::ERR_FAILED` without it. The Mac's
  engine does not call them.
- `runtime/java/com/macapk/loader/LocationBridge.java`, `SpecialAccessHandler.java`,
  `ActivityHost.java` and `MacApkPackageManager.java`: the host may name its
  location settings page (`nativeHostSettingsUrl`; Windows:
  `ms-settings:privacy-location`, from `windows/host/native_location.cpp`).
  `Settings.ACTION_LOCATION_SOURCE_SETTINGS` opens it, and so does a location
  request the host itself refuses (location off for apps in Windows), once per
  run. `android.hardware.location` and `.network` are declared when the
  location is bridged. On the Mac, which binds no such native, nothing changes
  but the two features.
- `runtime/java/android/net/MacApkHostNetwork.java`, `ConnectivityManager.java`
  and `com/macapk/loader/HostRadios.java` (`defaultNetwork`): on Windows the
  network `ConnectivityManager`, `NetworkCapabilities` and `LinkProperties`
  describe is the adapter that carries the default route, asked of
  `windows/host/native_radios.cpp` (`GetBestInterfaceEx`,
  `GetAdaptersAddresses`): its transport (Ethernet, Wi-Fi, mobile broadband as
  `TRANSPORT_CELLULAR` / `TYPE_MOBILE`, a tunnel as VPN), addresses, DNS
  servers and MTU. Before, the Mac's tools (`route`, `networksetup`, `scutil`)
  failed here, so the JDK's first interface with a routable address was taken
  (a Hyper-V or VPN adapter as readily), every network was Wi-Fi and no DNS
  server was listed. A host without the native (the Mac) answers "not known"
  and asks its tools as before.
- `runtime/java/android/net/nsd/NsdManager.java`: the failure every request
  ends in (`FAILURE_INTERNAL_ERROR`; there is no mDNS behind it) reaches the
  listener from an "NsdManager" handler thread after the call returned, as
  AOSP's does, not from inside the call; a null listener or a protocol other
  than `PROTOCOL_DNS_SD` is an `IllegalArgumentException`. Not
  Windows-specific.
- `runtime/java/android/speech/tts/TextToSpeech.java` and
  `android/media/midi/MidiManager.java`: a request that was accepted and
  never answered now ends in the failure AOSP documents, after the call has
  returned and with one log line. There is no speech engine, so an utterance
  with an id reaches `UtteranceProgressListener.onError(id, ERROR_SYNTHESIS)`
  (and the deprecated `OnUtteranceCompletedListener`) from a "TextToSpeech"
  handler thread, where it was queued with `SUCCESS` and never heard of again;
  `playSilentUtterance` needs no engine and reports `onStart`, then `onDone`
  when its time is up. `MidiManager.openDevice` and `openBluetoothDevice`
  report `onDeviceOpened(null)` on the caller's handler. Not Windows-specific.
- `runtime/java/com/macapk/loader/HostRadios.java` (new), with
  `android/net/wifi/WifiManager.java`, `WifiInfo.java`,
  `android/bluetooth/BluetoothAdapter.java`, `BluetoothDevice.java` and
  `MacApkPackageManager.java`: the host's Wi-Fi (state, connection, scan
  results, `SCAN_RESULTS_AVAILABLE_ACTION`) and Bluetooth radio (on/off, name,
  paired devices; no connections) reach the app, from
  `windows/host/native_radios.cpp`; `android.hardware.bluetooth` is declared
  when there is a radio. A host without these natives (the Mac) keeps the old
  answers. Also the interface's DHCP configuration (`getDhcpInfo`:
  gateway, netmask, DNS, server, lease) and the host's Wi-Fi notifications
  (`HostRadios.wifiEvent`), which one thread turns into
  `WIFI_STATE_CHANGED_ACTION`, `NETWORK_STATE_CHANGED_ACTION` (with
  `EXTRA_NETWORK_INFO`: `android/net/NetworkInfo.java` is now Parcelable,
  as on AOSP, and its detailed state follows its state),
  `SUPPLICANT_CONNECTION_CHANGE_ACTION`, `RSSI_CHANGED_ACTION` and
  `SCAN_RESULTS_AVAILABLE_ACTION` when a scan finishes (the fixed timer
  stays for a host without events).
- `runtime/java/com/macapk/loader/HostBluetooth.java` (new), with
  `android/bluetooth/BluetoothSocket.java`, `BluetoothServerSocket.java`,
  `BluetoothDevice.java`, `BluetoothAdapter.java`, `BluetoothGatt.java`,
  `BluetoothGattService.java`, `BluetoothGattCharacteristic.java`,
  `BluetoothGattDescriptor.java` (the hidden instance-id constructors),
  `android/bluetooth/le/BluetoothLeScanner.java`, `ScanRecord.java` (AOSP's
  parser) and `MacApkPackageManager.java`: talking to Bluetooth devices
  through the host, from `windows/host/native_bluetooth.cpp`: RFCOMM sockets
  both ways (Winsock AF_BTH, the SDP query and record), LE scans (the
  advertisement watcher) and the GATT client (services, reads, writes,
  notifications, MTU). The permission checks are AOSP's: `BLUETOOTH_CONNECT`
  / `BLUETOOTH_SCAN` (implied by `BLUETOOTH` / `BLUETOOTH_ADMIN` below API
  31) throw `SecurityException` when missing, scan results need the location
  permission unless the scan is `neverForLocation` (`PackageInfo` now carries
  that flag). `android.hardware.bluetooth_le` is declared when the host
  scans. A host without these natives (the Mac) keeps the old answers: the
  sockets' "no Bluetooth radio", a GATT client that reports the failure, a
  scan that fails.
- `runtime/java/com/macapk/loader/ActivityHost.java` and
  `MacApkPackageManager.java`: a Custom Tab launch opens in
  `HostBrowserTab` (new, with `HostCustomTabsService`) when the host has a
  browser tab of its own, and the host browser row then answers the Custom Tabs
  service; its PackageInfo carries `signingInfo`. On the Mac, which has no such
  tab, nothing changes but the `signingInfo`.
- `runtime/java/android/webkit/WebView.java`: with a modal window above a
  WebView, the occluder rects sent to the engine add that window's bounds
  after the whole-view rect, so an engine that shows the page over the canvas
  (Windows) can keep the dialog visible. The union, which the Mac's hit test
  uses, is unchanged.
- `runtime/java/android/widget/TextView.java`: the protected `onTextChanged`
  hook is called on every text change (constructor, `setText`, typing,
  `append`, in-place edits), with or without `TextWatcher`s, as AOSP does. It
  was declared but never called, so custom fields that keep their state there
  (an SMS-code `PinView`) drew nothing and ignored typing. Also
  `setText(text, BufferType.SPANNABLE)` stores a `Spannable`, and the type
  sticks for later `setText` calls, so `(Spannable) getText()` works. Not
  Windows-specific; worth offering upstream.
- `runtime/java/android/view/Window.java`: the eight activity-transition
  getters and setters keep their values and default to the theme's
  transitions (`windowActivityTransitions`), as AOSP `PhoneWindow` does,
  instead of always returning null; apps that add a listener to
  `getSharedElementEnterTransition()` no longer crash in `onCreate`. Not
  Windows-specific.
- `runtime/java/android/content/res/TypedArray.java` and `Resources.java`:
  `getDimensionPixelSize` (and a drawable's `<stroke android:width>` /
  `<size>`) round and keep a non-zero size at least 1 px, as AOSP does, so
  0.5dp hairlines and borders are drawn; `getDimensionPixelOffset` still
  truncates. Not Windows-specific.
- `runtime/java/com/macapk/loader/SharedStorage.java`,
  `android/provider/MediaStoreProvider.java`,
  `android/app/usage/StorageStatsManager.java` and
  `com/macapk/loader/HostDocumentsProvider.java`: the volume's Download,
  Pictures, Documents, Music and Movies are junctions to the user's Windows
  folders, made by the host (`windows/host/user_folders.cpp`, named in
  `WINAPK_USER_FOLDER_TARGETS`). Java sees a junction as a plain directory, so
  these recognise it (`isJunction`, `isHostFolderMapping`, `isLink`): MediaStore
  keeps upstream's privacy filter and walk cache for them and indexes at most
  10,000 files per folder, breadth first (`walkHostFolder`); no walk, size or
  delete follows a junction inside a folder; and `mapUserFolders` stays off when
  the host maps, since it would merge a junction into itself. On the Mac nothing
  changes.
- `runtime/java/com/macapk/loader/Manifest.java`: the components of installed
  feature splits (`isFeatureSplit="true"`) are merged into the package, as
  AOSP `parseSplitApplication` does. Chrome keeps its launcher alias and
  `ChromeTabbedActivity` in `split_chrome.apk`, and was rejected with "no
  launchable activity". `MACAPK_MANIFEST_FEATURE_SPLITS=0` reads the base alone.
  Not Windows-specific.
- `runtime/java/android/app/HandoffActivityData.java`,
  `HandoffActivityDataRequestInfo.java` (new, API 37 type-only placeholders)
  and `com/macapk/loader/ActivityHost.java`: an activity overriding a newer-API
  callback no longer fails every lifecycle call with `NoClassDefFoundError`;
  a class whose methods cannot be resolved is skipped in favour of the
  superclass declaration, which still dispatches to the override
  (`MACAPK_LIFECYCLE_SKIP_UNLINKABLE=0` turns that off). Not Windows-specific.
- `runtime/java/android/app/Application.java` (`AppBaseContext`): has the
  `mClassLoader` field of AOSP `ContextImpl`, and `getClassLoader` returns it
  when set; Chromium sets it reflectively for isolated splits
  (`MACAPK_CONTEXT_MCLASSLOADER=0` ignores it). Not Windows-specific.
- `runtime/java/android/app/Application.java`, `Activity.java` and
  `com/macapk/loader/ActivityHost.java`: `onActivityPreCreated` goes out before
  `onCreate`, `onActivityCreated` from `Activity.onCreate` (or after it when
  the override skips super), and `onActivityPostCreated` before a `finish()` in
  `onCreate` tears the activity down, as AOSP does. Chromium's
  `ApplicationStatus` depends on it. `MACAPK_LIFECYCLE_CREATED_ORDER=0` restores
  the old order. Not Windows-specific. Upstream later split the three phases
  the same way (for the Play Store); its `dispatchActivityPreCreated` /
  `dispatchActivityPostCreated` are used, the gate and the `finish()` handling
  stay Win.apk's.
- `runtime/java/com/macapk/loader/SpecialAccessStore.java`: on Windows the
  app-ops store (`appops\app-ops.json`, `signing-key.pub`) and the registry
  (`installed_apps.json`) are read from the data folder, the parent of the
  android root, and an app's identity is the registry's one row for the
  running package (every app shares the android root here). The launcher is
  the helper that writes the store (`Win.apk.exe runtime-permission`,
  `special-access`, `app-details`, in `launcher/launcher_main.cpp`), with
  upstream's payload and tokens. On the Mac nothing changes.
- Every dangerous permission group, gated on `SpecialAccessStore.allPermsAsk()`
  (Windows, unless `MACAPK_ALL_PERMS_ASK=0`; the Mac's helper knows only
  storage and location, so there nothing changes): the group table
  `com/macapk/loader/RuntimePermissionGroups.java` (new) and
  `SpecialAccessStore.GroupModel` (granted, rationale, the dialogs to show,
  App info's rows, the background permissions); `MacApkPackageManager.
  checkPermission` and `GET_PERMISSIONS` flags answer from it, and
  `canRequestPackageInstalls` from the "Install unknown apps" op;
  `SpecialAccessHandler` runs one `runtime-permission --prompt <group>` per
  group, resolves `MANAGE_OVERLAY_PERMISSION`, `MANAGE_UNKNOWN_APP_SOURCES`,
  the notification / accessibility / usage / DND / battery / alarm / write-
  settings screens and `APP_NOTIFICATION_SETTINGS`, and lists location and
  the groups in App info; `android/app/AppOpsManager.java` keeps each
  runtime-permission op in step with its permission (MODE_IGNORED when
  denied) and answers op 24 / op 66 from the store;
  `android/provider/Settings.java` `canDrawOverlays`, `android/app/
  NotificationManager.java` `areNotificationsEnabled`, `MacApkWindowManager`
  (a system-alert window type without the grant throws BadTokenException),
  `android/hardware/Camera.java`, `camera2/CameraManager.java` and
  `AudioInputBridge` honour the Android permission before the Windows
  privacy switch.
- `runtime/java/com/macapk/loader/HostNotifications.java` (new) and
  `android/app/NotificationManager.java`: `notify`, `cancel` and `cancelAll`
  reach the host (Windows: `windows/host/native_notifications.cpp`, toasts in
  the Action Center with the app's name and icon; a click brings the window
  forward and sends the `contentIntent`), once `areNotificationsEnabled`
  (POST_NOTIFICATIONS, pre-granted below T) and the channel's importance let
  the notification through; LOW and MIN do not pop up. The channel list is
  one per process, as the service's is. A host without the natives (the Mac)
  shows nothing, as before. `android/app/Notification.java`: `Style.addExtras`
  as AOSP has it, so a built notification's extras carry `BigTextStyle`'s
  text, `InboxStyle`'s lines and `MessagingStyle`'s messages (as lines) under
  the `EXTRA_*` keys; the styles kept nothing before. Not Windows-specific.
- `runtime/java/com/macapk/loader/ActivityHost.java`: without the AOSP window
  policy (Windows), an activity that owns the window box asks the host for its
  manifest's `screenOrientation` at launch (`NativeCocoa.applyWindowOrientation`,
  the same native the policy's branch uses), so a portrait-only app opens
  portrait although the library opens apps landscape. A host without the
  native keeps the old behaviour. When that activity finishes, the one it
  reveals turns the phone back (`windowOrientationOnRevealed`): its own fixed
  orientation (the manifest's, or its `setRequestedOrientation`), or, for a
  free one, the user rotation when the finished activity had fixed a side.
  Before, a landscape-only activity (Mercantile's cheque scanner) left its
  portrait app landscape.

- `runtime/java/com/macapk/loader/HostVideo.java` (new): MediaPlayer's video
  player on the host (`windows/host/native_media_video.cpp`): a thread pulls
  each picture when the host says it is due and shows it where the app's
  Surface points (a SurfaceView's own layer through
  `SurfaceView.macapkPresentVideoFrame`, new, public; a SurfaceTexture's
  producer queue; else the window), with `setVideoScalingMode`'s cropping.
  Also `presentDecodedFrame`, which the host's `NativeMedia.nativeVidRender`
  calls back to find MediaCodec's registered SurfaceView sink or
  SurfaceTexture target (the Mac's video plane does the same lookups in
  native code), and `vpxBuiltIn`: whether libvpx is built in (the Mac) or
  VP8/VP9 are the host's, listed by `nativeVideoSupportedMimes` when present.
  `MediaCodec.isVideoMimeSupported` and `MediaCodecList`'s libvpx provider
  ask it instead of assuming libvpx. On the Mac every native is unbound and
  nothing changes.
- `runtime/java/com/macapk/loader/HostMedia.java` (new): the host's demuxer,
  H.264 and AAC encoders and MP4 writer (`windows/host/native_media_codec.cpp`
  over `video_decode.h`'s `Demuxer`, `media_encode.h`, `mp4_writer.h`).
  `android/media/MediaExtractor.java` reads every audio and video track
  through it (upstream's `NativeMedia.nativeDemux*` is an audio-only demuxer
  Windows does not have) and `MediaMetadataRetriever.java` answers from that
  extractor and decodes frames for a path or URL; `MediaMuxer.java` writes MP4
  and 3GPP (H.264 + AAC) and refuses the rest; `MediaCodec.java` gains its
  encoder mode (`CONFIGURE_FLAG_ENCODE`, `createEncoderByType`,
  `createInputSurface`, `signalEndOfInputStream`, `setParameters`), riding the
  decoder's buffer queues, and `MediaCodecList.java` / `MediaCodecInfo.java` /
  `HostCodecCapability.java` list the two encoders when the host has them;
  `MediaRecorder.java` records the microphone and `VideoSource.SURFACE` on
  those, and fails `prepare()` for what the host cannot record.
- `runtime/java/android/view/Surface.java`: `macapkSetFrameSink` (new, public):
  what a Canvas posts on the Surface goes to the sink, at the sink's size,
  instead of the window. An encoder's input Surface sets it.
- `runtime/java/android/media/MediaPlayer.java`: `setDataSource(path)` and a
  `file:`/`http(s):` Uri keep the path (read at `prepare`, so a video is not
  slurped into memory and a URL is streamed); `prepare` tries
  `HostVideo.Player` first and, for a source with a video track, every
  transport call, `getVideoWidth/Height`, `setDisplay`/`setSurface`,
  `setVideoScalingMode` and `onVideoSizeChanged` go to it; an audio-only
  source, or a host without the player, keeps the audio path as it was.
- `runtime/java/android/widget/VideoView.java`: binds the player to its
  holder (`setDisplay`), measures to the video's aspect ratio
  (`onMeasure`), and keeps a `start()`/`seekTo()` made before the player is
  prepared for `onPrepared`, as AOSP does. Not Windows-specific.
- `runtime/java/com/macapk/gms/GoogleSignInActivity.java`: with no Google
  account on the PC at all (and a request that does not filter by authorized
  accounts), the app's "Sign in with Google" opens Win.apk's sign-in window
  at once; the account added is Win.apk's, shared by every app, and signs in
  to the app, and a cancel cancels. The screen's texts name Win.apk and
  Settings > Google account instead of Mac.apk > Accounts.
- `runtime/java/com/macapk/loader/HostTrust.java` (new) and
  `runtime/java/com/macapk/gms/GmsCli.java`: the JVM's default trust store is
  the JDK's CAs plus the roots Windows trusts (a filtering proxy's among
  them), set before any TLS. `Boot.run` did it for an app's
  process only; `GmsCli.run` (`--gms`, Settings > Google account) now does
  too, or the sign-in's token exchange failed with "PKIX path building
  failed". The set is kept as one JKS file, `%TEMP%\winapk-trust.jks`, that a
  start uses at once and, when it is more than a day old, rebuilds in the
  background 20 s later; only the first start ever builds it before the app (building it cost 0.6-1.3 s of
  every start). Not on other systems.
- `runtime/java/com/macapk/gms/AccountPhoto.java` (new),
  `NativeSignIn.java` and `GmsStore.java`: after a sign-in, the picture the
  sign-in window found (`signInPhotoUrl`, a native only Windows has; on the
  Mac its absence means no picture) is saved as `gms\photo-<account>.img`
  for the launcher's account item, and removed with the account.
- `runtime/java/com/macapk/gms/GoogleGmsCertificate.java` (new),
  `MacApkPackageManager.java` and `PlatformSignature.java`:
  `com.google.android.gms` and `com.android.vending` report Google's public
  Play services certificate instead of the platform one. Every app's own
  play-services-basement compares the two with the copy it embeds before it
  uses Play services at all; with ours it answered `SERVICE_INVALID`, so
  "Sign in with Google" failed at once (BuyMe, Waze) and the location and
  sign-in providers were never asked. `MACAPK_GMS_GOOGLE_CERT=0` goes back.
- `runtime/java/com/macapk/loader/PlayServices.java` and
  `MacApkPackageManager.java`: the Play services version is 26.44
  (`264400000`); app clients of 2026 want more than 24.44 for some APIs
  (`SERVICE_VERSION_UPDATE_REQUIRED`).
- `runtime/java/com/macapk/gms/LegacySignInService.java` (new) and
  `MacApkPackageManager.java`: the legacy Google Sign-In service
  (`com.google.android.gms.auth.api.signin.service.START`: silentSignIn,
  signOut, revokeAccess). Flutter's google_sign_in calls signOut before every
  sign-in; with the bind unanswered it threw and City Car never asked for an
  account.
- `runtime/java/com/macapk/loader/ResourceIndex.java`: an enum or flag value
  given by name (apktool-decoded XML carries `evenOdd`, not 1) resolves
  against the platform's own `<attr>` definitions in `framework-res.apk`
  when the app does not declare the attr, instead of TypedArray's short
  hand table. `android:fillType` read as nonZero, so Compose and AndroidX
  vector icons cut out of a filled shape drew as solid shapes.
- `runtime/java/com/macapk/gms/GoogleSignInActivity.java`,
  `GoogleSignInBroker.java` and `AccountPhoto.java`: the account chooser is
  in Hebrew when Win.apk is (`winapk.ui.lang`, which `runtime_main.cpp` sets
  from the launcher's Language setting, else Windows'), and right to left.
  The signed-in account an app gets carries the picture
  (`GoogleSignInAccount.photoUrl`, from the ID token), which is also saved for
  the launcher's account item when the sign-in window found none. A copy of
  an app re-signed by someone else, which Google refuses
  (`UNREGISTERED_ON_API_CONSOLE`), and every other failed sign-in show
  "Google sign-in failed (error: <code>, status <n>)" and stay open instead
  of closing on the app's generic error; leaving then hands the app that
  failure.
- `runtime/java/android/graphics/drawable/VectorDrawable.java`: a path's
  `android:fillType="evenOdd"` is honoured; icons cut out of a filled shape
  filled solid.
- `runtime/java/android/content/pm/PackageManager.java`:
  `getApplicationIcon(String)` throws `NameNotFoundException` for a package
  that is not installed, as in AOSP, instead of returning a transparent
  stand-in that apps then tinted into a solid shape.
- `runtime/java/com/macapk/loader/Boot.java`: the app's data directory is the
  8.3 short form `runtime_main.cpp` passes in `winapk.data.dir`, when the
  volume has short names. Native code sized for `/data/user/0/<package>` cut
  the ~150-character full form (Waze: a 128-byte buffer, so no map).
- `runtime/java/com/macapk/loader/Boot.java`, `NativeCocoa.java`
  (`setAppData`), `android/os/Environment.java`,
  `android/content/StorageContext.java`, `android/app/Application.java`: the app
  sees its files by Android's names, not host paths, as on a device. The data
  directory is `/data/user/0/<package>` (`getDataDir`, `getFilesDir`,
  `getCacheDir`, `getDatabasePath`, `java.io.tmpdir`, `ApplicationInfo.dataDir`,
  all derived from it), its device-protected sibling `/data/user_de/0/<package>`,
  and primary shared storage `/storage/emulated/0`
  (`getExternalStorageDirectory`, and `getExternalFilesDir`/`getObbDir`/… built
  on it). `ApplicationInfo.sourceDir`/`publicSourceDir`/`splitSourceDirs`/
  `nativeLibraryDir` stay host paths on purpose: the ELF loader mmaps them and
  HotSpot's `System.load` opens them, neither through the path mapper (see
  `Application.macapkSetDataDir`). The host maps an Android path to the real
  folder: `windows/src/bionic/paths.cpp` for native code (native code walks a
  path up to `/` and joins relative paths onto its working directory, so with
  `C:\...` it never reached `/` — Google Maps' recursive mkdir overflowed its
  stack — and took the folder for a relative path), and the java.base overlay's
  `jdk.internal.util.WinApkPaths` for java.io and java.nio. Translated arm64
  guest code goes through the same Bionic shim, so it needs no separate remap.
  File names with characters Windows forbids (`< > : " | ? *`, control
  characters, a final dot or space; YouTube's `<account>||.entitystore`
  database) are stored with WSL's and Cygwin's mapping, each such character as
  U+F000 + the character, in the Android part of a mapped path only
  (`host_names` in `paths.cpp`); a listing under an Android path turns the
  names back (the shim's `readdir`/`scandir`, `WinNTFileSystem.list`,
  `WindowsPath.createFromNormalizedPath` for a `DirectoryStream`'s entries).
  `java.nio`'s own parser still refuses such a name in `Paths.get`.
- `windows/java/java_base_patch/jdk/internal/util/WinApkPaths.java` (new),
  `java/io/WinNTFileSystem.java`, `java/io/File.java`,
  `sun/nio/fs/WindowsPath.java`: the java.base overlay keeps a `/`-rooted Android
  path in that form (`/` separators, no drive, absolute, `.`/`..` folded by text)
  through `java.io.File` and `java.nio.file.Path`, and maps it to the host file
  it names for every call that reaches Win32 (`host0`, bound in
  `windows/host/java_io_open.cpp`). `file.separator` stays `\`.
- `windows/host/java_io_open.cpp`, `sqlite_paths.cpp` (new),
  `native_media_video.cpp`: `FileInputStream`/`FileOutputStream`/
  `RandomAccessFile` opens, the replacing rename, the framework's default SQLite
  VFS (`SQLiteDatabase` hands it Android database paths) and `MediaPlayer`'s file
  source (`HostVideo`) all map an Android path to the host file first.
- `runtime/java/javax/microedition/khronos/egl/MacApkEGL10.java`: an app's
  own GL thread with a real context (not a GLSurfaceView fork) that calls
  `eglCreateWindowSurface` on a SurfaceView or its holder gets a window
  surface on the SurfaceView: `nativeCreateSurfaceViewWindow` makes a pbuffer
  over the native window, and `nativeSwapSurfaceViewWindow` presents it as
  the guest layer (`windows/host/gl/egl10_window.cpp`, `guest_gl.cpp`). It
  was refused and the thread parked (Google Maps' map stayed white).
  `WINAPK_GL_VIEW_WINDOW=0` restores the refusal.
- `runtime/java/javax/microedition/khronos/egl/MacApkEGL10.java`:
  `eglGetCurrentContext` / `eglGetCurrentDisplay` / `eglGetCurrentSurface`
  answer `EGL_NO_CONTEXT` / `EGL_NO_DISPLAY` / `EGL_NO_SURFACE` on a thread
  with no context current at all (`nothingCurrent`), as on Android, not the
  sentinel objects: an app that reads its GL limits "in the current context,
  else in one of my own" read them with none (Google Maps' vertex attribute
  table had `GL_MAX_VERTEX_ATTRIBS` = 0 entries, and its GL thread died).
- `runtime/java/android/view/LayoutInflater.java`: `resolveString` answers
  null for `@null` and "" for `@empty`, as `TypedArray.getText` does, not the
  reference itself (a Chip's text read "@null").
- `runtime/java/com/google/android/gms/common/security/ProviderInstallerImpl.java`
  and `com/google/android/gms/org/conscrypt/OpenSSLProvider.java` (new): Play
  services' side of `ProviderInstaller.installIfNeeded`, which an app loads
  from Play services' code once its own check finds Play services installed:
  `GmsCore_OpenSSL` over the JDK's TLS goes first in the provider list and
  becomes the default SSLContext. The class was missing, every install
  failed, and Google Maps' own `Ssl_Guard` provider (no TLS until that install
  succeeds) stayed first: gRPC got a null SSLEngine and no map tile loaded.
  `android/security/MacApkConscryptProvider.java` gains
  `copyPlatformServices`, which copies only the platform's providers.
- `runtime/java/com/macapk/loader/Boot.java`: `CODE_DIR_OURS` names the files
  Win.apk keeps in an app's code folder (`label.txt`, `package.txt`,
  `icon.png.previous`, `classes.jar.tmp`, `AndroidManifest.txt`,
  `aot-guard.txt`, `notifications`). An app started while it was being
  updated took them for its own data, moved them to its data folder and left
  links: the Play Store updating itself lost its `label.txt` that way, the
  update counted as failed and the Play Store left the library.
- `runtime/java/android/widget/TextView.java`, `android/text/MacApkBidi.java`:
  a paragraph with no strong character (an empty field, a number) takes the
  view's layout direction, and a hint on show is measured as the hint, as
  AOSP's `getTextDirectionHeuristic` has it: in a right-to-left app a hint or
  a number sat at the left end. The constructor reads gravity, textAlignment,
  ellipsize, maxLines and lines from the widget's default style
  (`defStyleAttr` / `defStyleRes`), which reached only the text appearance:
  a design system's `gravity="center_vertical"` or `textAlignment="viewStart"`
  in its base style was lost (Moovit's search hint at the top of its box).
- `runtime/java/android/view/ViewGroup.java`: `LayoutParams(Context,
  AttributeSet)` and `MarginLayoutParams(Context, AttributeSet)` read the
  layout size and margins for a LayoutParams class the app defines, as AOSP
  does. They read nothing (the inflater writes them after construction), so
  a subclass that looks at them in its own constructor saw zeros: the
  ConstraintLayout 1.0 old apps bundle took every child for "match
  constraints" (YouTube's toolbar and bottom bar were empty).
- `runtime/java/com/macapk/loader/PackageInstallerBackend.java`,
  `android/content/pm/PackageInstaller.java`: a `MODE_INHERIT_EXISTING`
  session keeps the installed package's APKs it does not replace, as AOSP
  does. The Play Store adds a language to itself with a session of one
  config split; installed as the whole app, the split replaced the Play
  Store, which was left with no code.
- `runtime/java/android/content/Context.java`: `startActivity(Intent,
  Bundle)` launches by itself. It called `startActivity(Intent)` on the same
  object, and Chrome's context wrapper overrides the two the other way round:
  a `StackOverflowError` at its start. `android/view/Display.java`:
  `getDeviceProductInfo()` (null), which Chrome's first activity needs to
  attach. `com/google/android/gms/common/GooglePlayServicesUtilLight.java`:
  `isGooglePlayServicesAvailable(int, Context)`, the form R8 leaves when it
  reorders the parameters across an app's copy of play-services-basement
  (eBay).
- `runtime/java/com/macapk/dex/MethodSplitter.java` (`cloneableHandlers`),
  `Translator.java` (`emitHandlerClones`, `labelFor`): a handler copied into
  each part may hold a short forward branch to an instruction of its own
  block, and the copy gets a label (and a frame) there. That is the handler
  R8 leaves for `catch (Throwable t) { Throwable c = t.getCause(); if (c !=
  null) throw c; throw t; }`, which a Dagger component's constructor shares
  between hundreds of two-instruction try ranges: each range then spans to the
  method's end, no cut is valid, and the constructor became the stub that
  throws. Netflix's singleton component (341 ranges; `Application.onCreate`
  threw), 21 of PayPal's 25 stubbed methods and Booking's one now split; the
  25 PayPal classes pass `-Xverify:all`.
- `runtime/java/android/widget/TextView.java`: under a movement method the
  text is held as a `Spannable`, as AOSP's `setText` does (`type == SPANNABLE
  || mMovement != null`), and `setMovementMethod` converts what is there.
  Snapchat's sign-in link casts `getText()` in its constructor.
  `android/icu/util/TimeZone.java`: `getIanaID(String)` (API 34), missing on
  TripAdvisor's main thread at start.
- `runtime/java/android/companion/virtual/VirtualDeviceManager.java`,
  `VirtualDevice.java` (new), `Context.java`: the API 34 service exists and
  has no virtual devices. Google Translate reaches it on its main thread at
  start, behind `SDK_INT >= 34`, and ended on `NoClassDefFoundError`.
- `runtime/java/android/graphics/Path.java` (`addRoundRect` with eight
  radii): the radii are scaled to fit their sides by one factor, as Skia's
  `SkRRect::setRectRadii` does, and each corner is a quarter ellipse (one
  cubic; it was a quadratic). A corner radius far past the shape's size is
  how a drawable asks for a pill (`android:radius="1000dp"`), and Compose
  hands every rounded shape over this way: unscaled, each corner's curve ran
  past the opposite one, so the outline of Any.do's sign-in buttons was two
  crossing arcs between two straight ends.
- `runtime/java/com/macapk/loader/MacApkNativeLoad.java`,
  `AppClassLoader.java`: while a class of another class loader loads a
  native library, the app's loader also finds that loader's classes, as
  `FindClass` in `JNI_OnLoad` uses the caller's loader on Android. AWS WAF's
  SDK defines a class in a loader of its own (from a DEX it unpacks) whose
  `<clinit>` loads the library that registers its natives by name; `FindClass`
  answered null, and `RegisterNatives(null)` took the JVM down at Booking's
  start (`windows/src/jni_bind.cpp` now throws `NoClassDefFoundError` for a
  null class instead of passing it on).
- Framework members the most installed apps name and the tree did not have
  (`windows/test/apicheck/ApiCheck.java` over 86 apps from APKPure), each a
  `NoSuchMethodError`, `NoSuchFieldError` or `NoClassDefFoundError` where it
  was reached: `PackageManager.getActivityIcon(Intent)` and
  `getPackageInfo(VersionedPackage, ...)`; `NetworkInfo.State.SUSPENDED`
  (and the enum in AOSP's order); `WifiManager`'s `is...Supported()` family,
  `WifiInfo`'s Passpoint and link-ceiling getters, `ScanResult`'s
  `venueName`, `operatorFriendlyName`, `centerFreq0/1` and
  `getInformationElements()`; `LinkProperties.getDhcpServerAddress()`;
  `BluetoothDevice.getAddressType()`; `android.system.Os.rename`, `remove`,
  `unlink`, `mkdir`, `fsync`, `fdatasync`, `pread`, `pwrite`, `getppid`,
  `geteuid`, `getegid`; every Linux errno in `OsConstants` (it had nine) and
  the `RT_SCOPE_` values; `TelephonyManager.isMultiSimSupported()` returns
  AOSP's `int` (it was declared `boolean`, which no caller links against);
  `android.graphics.pdf.PdfRenderer` exists and refuses a document with an
  `IOException` (there is no PDF engine yet), where 27 of the apps met a
  missing class.
- `runtime/java/com/macapk/dex/Translator.java` (`F_NEW_INSTANCE`): the label
  of an eagerly emitted `new` is recorded as owned by the writer it is marked
  in, as `labelFor`'s labels are. Without that `ownerOf()` answered the main
  writer for it, so in a method `MethodSplitter` had cut into parts, a frame
  of a later part that names the `new` (an uninitialized reference kept in a
  register across a branch target) was taken for one that "crossed a part
  boundary", after the split had been committed, and the whole class was
  dropped. Telegram 12.10's `ChatMessageCell` (it draws every message;
  `setMessageContent` is split in three) was lost that way: the app's first
  activity then failed with `NoClassDefFoundError`.
- `runtime/java/com/macapk/dex/Translator.java`, `SuperInterfacePlan.java`: a
  non-virtual call (invoke-direct, invoke-super) to a final method of a
  superclass on a receiver that is not the current class is an
  invokevirtual. It was an invokespecial with the receiver cast to the
  current class, which threw ClassCastException when the receiver was
  another subclass: Compose's node chain in the Play Store, on every wheel
  and hover event.
- `runtime/java/android/graphics/drawable/RippleDrawable.java`: the pressed
  fill fades in over 225 ms and out over 150 ms (AOSP's ripple durations)
  instead of appearing and vanishing whole, a hard box under every tap.
- `runtime/java_base_patch/java/lang/Thread.java`: the current thread's
  `getStackTrace()` starts with `dalvik.system.VMStack.getThreadStackTrace`,
  as on Android, so code that reads its caller at a fixed index finds it (a
  bank's security library read index 3 of a three-frame trace, its native
  side went on with a null object and the process crashed).
- `runtime/java/android/graphics/Bitmap.java`: the host images are
  `transient`, so reflection over a model that holds a Bitmap (Gson) does not
  walk into `java.awt.image`.
- `runtime/java/android/security/keystore/AndroidKeyStoreKeyPairGeneratorSpi.java`:
  accepts the pre-API-23 `KeyPairGeneratorSpec`, converted as AOSP does.
- `runtime/java/android/content/pm/PackageInstaller.java`:
  `SessionParams.setAllocateAggressive`, which the Play Store calls on every
  install.
- `runtime/java/com/google/android/gms/maps/internal/CreatorImpl.java` (new):
  Play services' side of the Maps SDK (the `ICreator` binder), which a client
  library renamed by R8 loads from Play services' code. It was missing, so
  `MapsInitializer.initialize` threw in `Application.onCreate` and the app's
  window stayed empty (Moovit). It answers as a device whose map never
  finishes loading: initialization succeeds, a map view or fragment gets a
  plain view, `getMapAsync` does not call back. There is still no map.
- `runtime/java/android/view/View.java`, `ViewGroup.java`: a view asked for
  its layout direction before the top of its tree was measured kept that
  top's unresolved left-to-right for good, and everything added below
  inherited it: apps in Hebrew or Arabic were laid out left to right (the Play
  Store, YouTube). Such a tree now answers the Configuration's direction and
  keeps nothing; a view given a parent resolves again, its inheriting
  children with it (AOSP `addViewInner`, `resetResolvedLayoutDirection`).
- `runtime/java/com/macapk/loader/ResourceIndex.java`: `-ldrtl` and `-ldltr`
  resources match the configuration's layout direction; `-ldrtl` was never
  selected.
- `windows/java/java_base_patch/sun/reflect/annotation/AnnotationParser.java`
  (new, the JDK's with one change): an enum-valued annotation member is the
  enum's static field of the recorded name, as ART reads it, before the
  constant of that `name()`. R8 renames the field and keeps the name string,
  so the JDK's lookup failed, the annotation was an `AnnotationFormatError`,
  and Litho's layout threads died on `isAnnotationPresent` (YouTube: whole
  screens empty).
- `runtime/java/android/security/MacApkConscryptTls.java` (new): the SSLContexts
  of the `Conscrypt` and `GmsCore_OpenSSL` providers hand out the JDK's
  sockets wrapped to also answer Conscrypt's methods (`setAlpnProtocols`,
  `getAlpnSelectedProtocol`, `setHostname`, `setUseSessionTickets`), mapped to
  the JDK's application protocols and server names. gRPC's okhttp Platform
  sets ALPN that way on a Conscrypt provider's sockets; on the JDK's it found
  no such method, skipped ALPN, and Google Maps' gRPC calls (h2 only) failed.
- `runtime/java/com/macapk/loader/CriticalNatives.java` (new): which guest
  natives ART calls as `@CriticalNative` (no JNIEnv*, no jclass), read from the
  converted class file; `windows/src/jni_bind.cpp` gives those a thunk that
  drops the two. HotSpot passed them, so the guest read its JNIEnv* as its
  first argument (Google Maps' Elements and map renderer: a corrupted heap).
- `runtime/java/com/macapk/loader/ViewCanvasDriver.java` and
  `ActivityHost.java`: an activity stopped under another does not present.
  Each driver presents the whole window, so a covered one that kept drawing (a
  SurfaceView's overlay driver, ticked by the app's own render loop)
  alternated with the top: Waze flickered between the map and its sign-in
  screens. A translucent or floating top (no opaque window background) draws
  the last frame of the activity below before its own, as Android composites
  the two windows; it cleared over it, so Waze's translucent sign-in steps
  showed an empty window.
- `runtime/java/com/macapk/gms/AuthorizationService.java` (new),
  `GoogleSignInBroker.java`, `IdentitySignInService.java` and
  `MacApkPackageManager.java`: the Identity Authorization API
  (`com.google.android.gms.auth.api.identity.service.authorization.START`,
  `AuthorizationClient.authorize`): a server auth code, an access token or an
  ID token for the account the app signed in with on this device. Waze asks
  for a server auth code right after the account is picked; the bind was
  unanswered and the sign-in ended in an error.
- `runtime/java/com/macapk/dex/NestIndex.java` and `NestPlan.java`: a class
  whose superclass is in the same package and has only private constructors
  is put in that superclass's nest when R8 dropped the enclosing-class
  annotations. Enum constants with bodies, renamed by R8, call the enum's
  private constructor (legal on ART); HotSpot threw `IllegalAccessError`
  (Truecaller crashed in the background).
- `runtime/java/com/macapk/loader/SharedPrefsImpl.java`: characters Windows
  does not allow in a file name are written as `%XX` in a preferences file's
  name. Firebase Remote Config's `frc_1:<project>:android:...` file failed
  every write.
- `runtime/java/android/content/Context.java`, `com/macapk/loader/SharedPrefsImpl.java`:
  `Context.getSharedPreferences(File, int)`, which Android removed from the SDK
  but kept in `Context`. Aurora Store reads its blacklist through it by
  reflection; without the method the blacklist always read as empty.
- `runtime/java/com/macapk/loader/BinaryXmlParser.java`,
  `DomXmlResourceParser.java`, `android/util/AttributeSet.java`,
  `android/view/LayoutInflater.java` and the framework readers of a view's
  attributes (`TypedArray`, `Resources.StyleMergedAttributeSet`, `TextView`,
  `ViewStub`, `GestureOverlayView`, `KeyboardView`, `AnimationUtils`,
  `Transition`, `PreferenceAttrs`, `FragmentInflaterFactory`): a view's
  `AttributeSet.getAttributeValue` answers a compiled enum or flag as a device
  does (`TypedValue.coerceToString`: `"0x2"` for `inputType="number"`, or the
  raw string when the file kept one), and `getAttributeIntValue` its number.
  The parser records that value beside the symbol it spells the layout with
  (`MACAPK_VALUE_NS`); the framework reads the symbol through
  `AttributeSet.macapkAttributeText`. The bank app com.ideomobile.mercantile
  ran `Integer.decode` on `getAttributeValue(android, "inputType")` and
  crashed on `"number"`.
- `runtime/net_spi/com/macapk/net/AndroidCharsetAliasProvider.java` (new,
  registered in `runtime/net_spi/META-INF/services`): a charset name with `_`
  for `-` (`"UTF_8"`, `"US_ASCII"`, `"UTF_16LE"`) is the charset, as on
  Android, whose ICU lookup ignores `-` and `_`. `URLEncoder.encode(s, "UTF_8")`
  threw `UnsupportedEncodingException`. A `CharsetProvider` on the class path
  rather than a copy of `java.nio.charset.Charset` in `java_base_patch`: the
  JDK asks providers only after its own charsets miss.
- `runtime/java/com/android/internal/graphics/drawable/AnimationScaleListDrawable.java`
  (new) and `android/content/res/Resources.java`: the platform's progress-bar
  styles root their indeterminate drawable in this @hide class; it now inflates
  (the animated child, or the static one when the animator duration scale is 0)
  instead of a transparent drawable, so a progress bar that draws the style's
  drawable itself (Google Play) shows its spinner. Not Windows-specific.
- `runtime/java/com/macapk/loader/MacApkPackageManager.java`, `SystemClient.java`,
  `SystemServer.java` (`PROVIDER_PACKAGE`): with the broker on,
  `resolveContentProvider` also finds an authority another installed package
  declares and answers from that package's manifest, as PMS does. Compose's
  downloadable fonts (`com.google.android.gms.fonts`) failed with "The
  requested provider was not found on this device." even with Google Play
  services installed. Not Windows-specific.
- `runtime/java/android/content/res/TypedArray.java`: `getString` of an app
  attribute set in the XML to a compiled enum or flag answers the device's
  spelling (`"0x2"`), which the parser already records, not the symbol.
  Discount's and Mercantile's `DiscountTextInputLayout` run `Integer.decode`
  on it; every input field became a placeholder. Not Windows-specific.
- `runtime/java/android/provider/Settings.java`: `Settings.System` reads a
  setting that moved to Secure or Global (`android_id`, `adb_enabled`...)
  from there, and `Settings.Secure` one that moved to Global, as AOSP's
  `MOVED_TO_SECURE` / `MOVED_TO_GLOBAL` do. Discount and Mercantile read
  `Settings.System` `android_id` and threw on null in `Application.onCreate`.
  Not Windows-specific.
- `runtime/java/com/macapk/loader/Manifest.java`, `MacApkPackageManager.java`:
  `ServiceInfo.permission` is the service's `android:permission` (else the
  application's). IBM Trusteer's TAZ checks a job service for
  `BIND_JOB_SERVICE` and stopped its start without it. Not Windows-specific.
- `runtime/java/com/macapk/loader/Boot.java`, `HttpAgent.java`: `java.vm.version`
  is ART's `2.1.0`, as on every Android since 5. androidx MultiDex 2.x reads it
  to decide whether the VM loads secondary dex files itself; HotSpot's
  `21.0.x+y-LTS` did not match its pattern, so it reflected on
  `DexPathList.makeDexElements`, which our loader does not have, and logged
  "MultiDex installation failed" (KeePassDX, Tripeaks). Not Windows-specific.
- `runtime/java/android/view/WindowManagerGlobal.java`, with
  `com/macapk/loader/WindowOverlayStack.java` and `ActivityHost.java`: AOSP's
  `mViews` and `mParams` lists exist and follow the windows we hold (each
  activity's window from its first resume to its destroy, every
  `WindowManager.addView` window), changed with `add` and `remove(int)` through
  the field, so a list an app swaps in (LeakCanary's Curtains) sees each
  change. AnkiDroid's LeakCanary failed with `NoSuchFieldException: mViews`.
  `mRoots` is not provided (one `ViewRootImpl` for the process). Not
  Windows-specific.
- `runtime/java/android/graphics/FontFamily.java` (new), `Typeface.java`
  (`createFromFamiliesWithDefault`) and `android/content/res/AssetManager.java`
  (`openNonAsset`): the hidden family builder androidx.core's
  `TypefaceCompatApi21/24/26/28Impl` reflect on, backed by our font loader; a
  family becomes an app font family, so its bold face is used for bold.
  DroidFish logged `ClassNotFoundException: android.graphics.FontFamily`.
  Not Windows-specific.
- `runtime/java/android/content/res/AssetManager.java`: `open`, `openFd` and
  `openNonAssetFd` throw `FileNotFoundException` for a name the APK does not
  have, as AOSP does; they threw a plain `IOException`, which a
  `catch (FileNotFoundException)` fallback misses. Not Windows-specific.
- `runtime/java/android/app/Activity.java` and `MacApkToolbarActionBar.java`
  (new): a window whose theme asks for the platform action bar
  (`windowActionBar`, as Holo, Material and DeviceDefault do; not AppCompat)
  gets AOSP's `screen_action_bar` decor: a real `android.widget.Toolbar`
  with the id `@android:id/action_bar` in `action_bar_container` above the
  content, styled from `?android:actionBarStyle`. `getActionBar()` drives it
  (title, subtitle, display options, Up, show and hide, custom view), and the
  options menu fills its menu, with item clicks and Up (`android.R.id.home`)
  going to `onMenuItemSelected`. Before, there was no bar, and apps that look
  the Toolbar up by id got null (Currency's `setNavigationIcon` NPE).
  `MACAPK_PLATFORM_ACTION_BAR=0` restores the old decor. Not
  Windows-specific.
- `runtime/java/android/content/res/Resources.java` (`openRawResourceFd`)
  and `ContentResolver.java` (a comment): the raw resource's file as the
  apk's own descriptor at the entry's offset, through `openNonAssetFd`, as
  AOSP's `ResourcesImpl.openRawResourceFd` does; an id with no file or a
  compressed entry throws `NotFoundException`. It returned null, and
  ConnectBot's terminal service died with a `NullPointerException` on the
  descriptor. Not Windows-specific.
- `runtime/java/com/android/internal/policy/DecorView.java` (new), used by
  `android/app/Activity.java` and `Dialog.java` for the window's root view,
  with the field `mWindow`: square's curtains (in AnkiDroid, LeakCanary) finds
  a window's root by that class name and reads its window from that field.
  AnkiDroid logged `ClassNotFoundException: com.android.internal.policy.DecorView`.
  Not Windows-specific.
- `runtime/java/com/macapk/loader/BinaryXmlParser.java`: a compiled XML
  element keeps its own namespace (`ResXMLTree_attrExt.ns`), and a default
  namespace declaration (`xmlns="..."`) is kept, so `getNamespace()` answers
  what Android's parser does. OpenTasks reads its tab list
  (`<tabconfig xmlns="http://schema.dmfs.org/tasks">`) by qualified name and
  found nothing (`Broken fragile delegate`, an NPE in `TabConfig.load`). Not
  Windows-specific.
- `runtime/java/android/app/Activity.java`: the window's generateLayout
  reads the framework's window attributes (`android:windowNoTitle`,
  `android:windowActionBar`, ...), as `R.styleable.Window` does. Read by bare
  name, AppCompat's own `windowNoTitle` (false) and `windowActionBar` (true)
  won, so every DarkActionBar AppCompat activity got the platform action bar
  and its options menu as a platform `Menu` (NetGuard's SearchView and
  SMSSecure's NPEs in `onCreateOptionsMenu`/`onPrepareOptionsMenu`). Not
  Windows-specific.
- `runtime/java/android/widget/ActionMenuView.java`: the Toolbar menu's
  `addSubMenu` is MenuBuilder's, a submenu tied to its item, so `findItem`
  reaches items nested in a submenu (Editor's NPE on its Typeface items).
  Opening a submenu from the bar is not drawn yet. Not Windows-specific.
- `runtime/java/android/widget/RelativeLayout.java` (`solveBoundsForMeasure`,
  `onLayout`): a rule waits for its anchor's edges on its own axis only, as
  in AOSP, which positions every child horizontally and then vertically. A
  child aligned vertically to a sibling that sits horizontally beside it
  (arrows with `layout_alignTop`/`alignBottom` on a title that is
  `layout_toEndOf`/`toStartOf` them) was a cycle to the solver, and every
  child of the layout became an equal horizontal slice of it (Fossify
  Calendar's month header). Not Windows-specific.
- `runtime/java/android/media/RingtoneManager.java` (`getDefaultUri`,
  `getDefaultType`, `isDefault`) and `android/preference/RingtonePreference.java`
  (comments): the default sound of a type is AOSP's `Settings.System` URI
  (`content://settings/system/alarm_alert`...), not null. Fossify Clock
  builds its alarm table from it and died creating its database. Not
  Windows-specific.
- `runtime/java/android/content/res/Resources.java`: `getString(int)` is
  `getText(int)`'s string and `getText` looks the resource up itself, as in
  AOSP. `getText` called `getString`, so a `Resources` subclass whose
  `getString` calls its own `getText` and falls back to `super.getText`
  recursed for ever (Bit's Lokalise `LokaliseResources`: a
  `StackOverflowError` on the main thread at its first string). Not
  Windows-specific.
- `runtime/java/com/macapk/loader/AppLinks.java`, `Boot.java`,
  `android/content/pm/verify/domain/DomainVerificationManager.java`: App Link
  verification no longer fetches at start. Upstream fetched
  `/.well-known/assetlinks.json` for every `autoVerify` host on every run
  (Google Maps declares hundreds of Google domains: ~356 HTTPS requests per
  start). Now a domain is fetched only when a sign-in's `redirect_uri` is on
  it, one domain at a time (same rules: HTTPS, no redirects, the package and a
  real signing certificate, a wildcard against its parent), and the verdict is
  kept in `%LOCALAPPDATA%\Win.apk\cache\applinks\<package>.properties`:
  7 days, an unreachable site 1 hour, the whole file dropped when the APK's
  versionCode, update time or certificate changes. `DomainVerificationManager`
  answers from it without waiting (a domain not checked yet is
  `DOMAIN_STATE_NONE`). `MACAPK_APP_LINKS=0` still turns it all off. Test:
  `windows/test/java/com/macapk/loader/AppLinksTest.java`.
  `NativeCocoa.openAppLinkWindow`, the sign-in window AppLinks asks for (a
  stub answering false before), is now Windows' own
  (`windows/host/native_browser_tab.cpp`, no `runtime/` change): the
  browser-tab window of a Custom Tab in the app's `browser` WebView2 profile,
  whose one check is the return prefix (`host/applink_match.h`, exact
  `redirect_uri` without its query); that navigation, a server redirect or a
  pop-up included, is cancelled, queued for `HostUrlIntake` like a tab's
  hand-off, and the window closes. Closed by the user, or after 10 minutes,
  the app gets nothing; without a window the URL goes to the system browser as
  before. Log lines: `applink-signin <n>: ...`, queries redacted.
- `runtime/java/com/macapk/gms/NativeSignIn.java` and `GmsCli.java`: a
  sign-in window that ends without a token says why, as a code
  (`signInWindowError`, a native only Windows has: the WebView2 HRESULT, the
  page's `COREWEBVIEW2_WEB_ERROR_STATUS` and HTTP status, `TIMEOUT (600 s)`,
  `WEBVIEW2_RUNTIME_MISSING (0x...)`); `--gms signin-window` prints it as its
  `ERROR` line (exit 1) instead of exiting 2 as a cancel, and the launcher
  shows it. The window no longer has a note line above the page.
- Framework fidelity found by `windows/test/uiprobe` (the same probe run on
  Android's own framework) and `windows/test/apicheck`; none is
  Windows-specific:
  - `android/content/res/TypedArray.java`: `getDrawable` of a colour written
    out or reached through `?attr` is a `ColorDrawable` (a style's
    `background="?attr/colorPrimary"`); `getFloat` follows a theme attribute
    or a compiled reference (an alpha kept as a float dimen);
    `getColorStateList` is null for an attribute no tier sets.
  - `android/content/res/Resources.java`, `com/macapk/loader/ResourceIndex.java`,
    `android/graphics/BitmapFactory.java` and
    `android/graphics/drawable/NinePatchDrawable.java`: a bitmap or a
    nine-patch is as dense as the folder it came from
    (`ResourceIndex.densityFor`, `TypedValue.density`), so a picture shipped
    in one bucket only has its size in dp; a `<ripple>` is a `RippleDrawable`;
    a shape's `<padding>` and the Material button background's are read; a
    nine-patch's layout bounds (`npLb`) are its optical insets, which
    `InsetDrawable` adds its insets to.
  - `android/graphics/drawable/VectorDrawable.java`: `android:tint` may be a
    colour state list, followed through `isStateful` and `onStateChange` (the
    platform's check box and radio button).
  - `android/text/Layout.java`: `getPrimaryHorizontal`,
    `getOffsetForHorizontal` and `isRtlCharAt` follow the line's bidi runs;
    `getLineRight` is the visible extent.
  - `android/widget/TextView.java`: the baseline, `getLayout()`'s alignment
    and direction, `getGravity()` (always names a side), a password field and
    a phone number left to right, `setTextAppearance` with `textAllCaps` and
    colour state lists, a compound drawable takes the view's state when set.
  - `android/widget/ProgressBar.java`: `android:mirrorForRtl`.
  - `android/view/View.java`, `ViewGroup.java`: a group's children that
    inherit their direction resolve it with the group; margin and padding
    start/end ids.
  - `android/view/LayoutInflater.java`: an element's colours resolve against
    the view's own theme, and a widget's own `app:tint` is left alone.
  - `com/macapk/loader/AppClassLoader.java`: an app that carries Firebase
    gets no stand-in for a Firebase class it lacks (old `FirebaseApp` probes
    its optional modules by name).
  - Theme threading: a colour file whose default row is a theme attribute
    (`android:textColorPrimary` of Material and of the platform) and a layout
    size written as a theme attribute (`?attr/actionBarSize`) are resolved
    against the theme of the context asking, not the application's, and the
    hand-built platform button and edit-text backgrounds take that theme's
    colours. A dark activity in a light app had black text, a pale button and
    a black underline; a Material 3 toolbar was 8dp short.
  - `android/widget/TextView.java`: `maxWidth`, `maxHeight`, `width`,
    `height`, `ems`, `maxEms`, `minEms`, line spacing, `minLines` and
    `letterSpacing` are read over all the tiers; each paragraph takes its own
    direction for its lines' side; a line-limited text's baseline and top
    follow its gravity; a measured view with no text has its line.
  - `android/widget/ImageView.java`: `getImageMatrix` is the configured
    matrix; a newly set stateful drawable takes the view's state.
  - Platform members two or more of 50 apps name and the runtime lacked
    (`AccessibilityEvent.getTextChangeTypes`, `Network.CREATOR`,
    `OsConstants.PROT_READ`, `Context.bindService(.., BindServiceFlags)`,
    `android.icu.text.IDNA`, `sun.misc.Cleaner` and about fifty more).
- `runtime/java/com/macapk/loader/Boot.java`: a launch through
  `MACAPK_LAUNCH_ACTIVITY` carries no LAUNCHER category, as `am start -n`
  sends a component alone. The library starts an app with no launcher activity
  that way, on the activity the user chose of those the app lets others start
  (`windows/java/.../AppMeta.java` lists them in `library\<name>.package`).
- `runtime/java/android/security/MacApkTrustProvider.java`: a server chain the
  default trust manager refuses is logged once per host and reason (tag
  `MacApkTrust`): the chain as sent, the anchors the manager holds, the SNI
  name and whose TLS stack asked. Play services' check-in failing with "PKIX
  path building failed" behind it all (android-id 0, the Play Store's server
  errors) could not be told apart from that message alone.
- `runtime/java/android/accounts/AccountManager.java`: a Google account added
  through Win.apk's sign-in window (the Play Store's Sign in) is announced to
  the caller's own listeners and receivers before the add's result is posted,
  as AOSP sends LOGIN_ACCOUNTS_CHANGED first. The broker's broadcast came after
  it, and the Play Store took the add for a cancel until it was restarted.
- `runtime/java/com/macapk/loader/InstalledSplits.java`, `AppClassLoader.java`:
  `fallbackLibDirs` is the install dir's `lib/arm64/` when it is not the
  native library directory: where `AppInstall` puts the arm64 libraries an
  x86_64 build lacks, with what they need. `locateApkSo` looks there for a
  library the native library directory lacks, and `stage6SourceStillPresent`
  no longer takes a library of the x86_64 set for an arm64 one whose source
  is gone (which would have translated it again at every start).
- `runtime/java/com/macapk/loader/CameraCaptureHandler.java` (new),
  `ActivityHost.java`, `MacApkPackageManager.java`: `MediaStore.ACTION_IMAGE_CAPTURE`
  (and `_SECURE`) resolve to the camera app's capture activity
  (`com.android.camera2`, listed by `queryIntentActivities`, startable by that
  component too) and open the host's camera screen (`native_camera.cpp`,
  `nativeCapture`): the JPEG goes to `EXTRA_OUTPUT` (or the intent's ClipData)
  through the app's own `ContentResolver`, or a thumbnail comes back in the
  `data` extra; RESULT_CANCELED when the user cancels. Nothing resolved them
  before, so "take a photo" buttons were dead. `MACAPK_IMAGE_CAPTURE=0` leaves
  them unresolved.
- `runtime/java/android/view/LayoutInflater.java`: a bare `<ImageView>`,
  `<TextView>`, `<Button>`... becomes AppCompat's subclass only where Android's
  AppCompat factory would make it: when the inflater has a factory, or inside an
  `AppCompatActivity`. An Application's context, a plain `Activity` and the
  wrappers around them get the framework's own classes, and `app:tint` /
  `app:srcCompat` (AppCompat's attributes) no longer apply to the framework's
  `ImageView` / `ImageButton` there. Before, any app that bundled AppCompat got
  AppCompat's classes, tints and default styles on every inflater (found by
  the UI probe). `MACAPK_APPCOMPAT_REWRITE_ANY=1` restores that.
- `runtime/java/android/view/View.java`, `ViewGroup.java`: `setClipBounds` keeps
  the rect and the parent cuts the child's drawing to it where it clips the
  child to its bounds (all three child-draw paths), whatever the parent's
  `clipChildren`; `getClipBounds` answers null (`false`) when none was set, as
  AOSP does. The setter did nothing and the getters answered the whole view,
  so whatever Material, MotionLayout or a transition cut away was drawn in full
  (found by `Completeness`; the UI probe's `clip_bounds` case now matches
  Android's).
- `runtime/java/android/widget/TextView.java`, `android/view/LayoutInflater.java`:
  `setFirstBaselineToTopHeight` / `setLastBaselineToBottomHeight` move the top
  (bottom) padding as AOSP's do, and the inflater applies
  `firstBaselineToTopHeight` / `lastBaselineToBottomHeight` after the padding
  and the text size, where AOSP's TextView constructor does (the `app:` ones
  only where AppCompat's views are, as the tint above). Both setters did
  nothing, so Material's type styles and AppCompat's attributes left text where
  the font put it (UI probe `text_baselines`).
- `runtime/java/android/animation/AnimatorSet.java`: `setInterpolator` keeps the
  curve and `start` gives it to every child, as AOSP's `initAnimation` does;
  Animator's no-op was inherited, so a set's curve was dropped
  (`FrameworkStubsTest`).
- `runtime/java/android/widget/RelativeLayout.java`: a `MATCH_PARENT` child is laid
  out at its measured size, as AOSP's `positionChildHorizontal` /
  `positionChildVertical` place it (`MATCH_PARENT` only makes its measure spec
  the parent's room); it was stretched to the parent's far edge whatever it
  measured, so a square view (Fossify's `MySquareImageView`, which passes its
  width spec as its height) filled the whole cell. The parent's edge is still
  used when the child measured 0. A child with no rule on an axis sits at the
  start edge (the right one under RTL) at its measured size, 0 included, as
  AOSP's `positionAtEdge` puts it: an empty `wrap_content` TextView was
  stretched across the parent and kept on the left under RTL. Screens sweep:
  Fossify Gallery's grid items and empty labels now match Android's (363
  differing lines to 335, none new); UI probe 227 to 219.
  `MACAPK_RL_MATCH_FILLS=1` restores the old placement.
- `runtime/java/android/content/res/Resources.java` (`Theme.resolveDefStyleName`),
  `android/view/LayoutInflater.java` (the default-style tier): a framework
  widget's default-style attribute (`com.android.internal.R.attr.checkboxStyle`
  and the like, every framework constructor passes one) is asked of the theme
  as `android:X`, and so is a `?android:attr/X` hop; AOSP matches theme items by
  attribute id, so the app's own bare `X` (Material 3's `checkboxStyle`, meant
  for `MaterialCheckBox`, with a 48 dp touch target) never answers it. Asked
  bare, it did: a plain `CheckBox` in a Material theme was 48x48 where Android
  has 32x32. AppCompat's and Material's widgets still ask their library's
  attribute. Screens sweep: Fossify Gallery 335 to 333, Open Sudoku 316 to 309
  lines, none new. `MACAPK_DEFSTYLE_BARE=1` asks the bare name again.
- `runtime/java/android/widget/ImageButton.java`: the constructors chain to
  `android.R.attr.imageButtonStyle` as AOSP's do (`ImageButton(Context)` through
  `this(context, null)`), so a plain `ImageButton` takes
  `Widget.Material.ImageButton`'s background and padding. It took no default
  style, which went unseen while every bare `<ImageButton>` became
  `AppCompatImageButton` (UI probe `lib_images`: 48 dp and unpadded where
  Android has 72 dp and 12,10,12,10; found by `test/uiprobe/regress.py`).
  `MACAPK_IMAGEBUTTON_NO_DEFSTYLE=1` restores the old chain.
- `runtime/java/com/macapk/loader/AppCaller.java` (new), `BinaryXmlParser.java`,
  `DomXmlResourceParser.java`, `android/view/LayoutInflater.java`: an app that
  reads an attribute's text (`AttributeSet.getAttributeValue`) gets Android's
  spelling, `@2131886250` for a reference and `?2130968577` for a theme
  attribute, where it got the framework's `@string/name`. The binary XML
  decoder keeps the raw form beside each reference attribute, in its own
  namespace (`http://macapk/axml-ref`, hidden from every attribute listing),
  and the parser hands it out when `AppCaller` finds the caller's class came
  from the app's class loader; Win.apk's own framework keeps the names it
  resolves by. Markor's settings (`GsFontPreferenceCompat`) parsed the text as
  a number and threw `NumberFormatException` (found by
  `test/compat/screens_crawl.py`). `MACAPK_ATTR_TEXT_FOR_APPS=1` gives apps the
  names again.
- Found by the Israeli-app screens checks (Maccabi, Cal), each read against AOSP:
  - `android/view/LayoutInflater.java`: `android:weightSum` is read (it was set
    only by `setWeightSum`, so a search field took half its row); a widget's
    default style no longer installs an attribute the element sets itself, so an
    element's own `android:background` leaves no padding of the default one behind
    (12,10,12,10 on a button Android pads 0). `MACAPK_DEFSTYLE_UNDER_ELEMENT=1`
    restores the old order.
  - `com/macapk/loader/BinaryXmlParser.java`: a newline, return or tab in an
    attribute is written as a character reference, so a two-line `android:text`
    stays two lines (attribute-value normalization made it one).
  - `android/widget/TextView.java`: `android:includeFontPadding` is read in the
    constructor, from every tier (it was never applied from XML;
    `MACAPK_IGNORE_INCLUDE_FONT_PADDING=1` skips it), and `getBaseline` takes the
    first line's ascent as `onDraw` places it, widened for a fallback font, so a
    Hebrew line without font padding is not 2 dp high.
  - `android/content/res/Resources.java`: a `<selector>` of colours under
    `res/drawable` is a ColorStateList, as AOSP takes any file-backed value by
    its root tag (a textColor of `@drawable/x` came out black: Cal's white
    buttons were black on black). `MACAPK_NO_DRAWABLE_CSL=1` turns it off.
  - `android/security/MacApkTrustProvider.java`: `KeyManagerFactory` "PKIX",
    alias "X509", as Conscrypt names it, over the JDK's NewSunX509; an app that
    loads its client certificate with `getInstance("X509")` got
    `NoSuchAlgorithmException` and went without it (Cal).
  - From Moovit: `com/google/android/gms/common/GooglePlayServicesUtilLight.java`'s
    `getRemoteContext`/`getRemoteResource` answer Play services' package context,
    as Play services does, where they answered null while availability said
    SUCCESS (a Maps client threw in `Application.onCreate`: a blank window);
    `LayoutInflater` applies `android:enabled="false"` and
    `android:measureAllChildren`; `ViewAnimator` measures all its children by
    default and `Space` starts INVISIBLE, as AOSP's constructors do;
    `ScrollView`/`HorizontalScrollView` give their child the viewport's size with
    UNSPECIFIED (AOSP's `makeSafeMeasureSpec`), not 0; and
    `MacApkConscryptProvider` hands a service whose class java.base cannot reach
    (`jdk.crypto.ec`'s) to its own provider, so "EC" by name under "Conscrypt" or
    "GmsCore_OpenSSL" no longer throws IllegalAccessException.
  - From bit: `RelativeLayout` keeps a wrap_content child that measured 0 at 0
    (AOSP's `mBottom = mTop + getMeasuredHeight()`; it took the parent's whole
    height and pushed the PIN screen off the window; `MACAPK_RL_ZERO_FILLS=1`
    restores that), and `android.os.Build` and `CheckinClient` tell Windows by
    the path separator, since Boot sets `os.name` to "Linux" first (a Windows run
    reported BRAND "Apple", MODEL "Mac").
  - From Bank Hapoalim and Waze: `LayoutInflater.inflate(res, root, true)` returns
    `root`, as AOSP's does (it returned the child: a custom view that inflates into
    itself and casts the result threw, and became an empty FrameLayout;
    `MACAPK_INFLATE_RETURNS_CHILD=1` restores that); `android:orientation` given as
    a number or an `@integer` reference is read (it was horizontal unless the
    literal "vertical").
  - From Mercantile: `TextView`'s constructor already reads textStyle,
    fontFamily, typeface and textFontWeight from every tier, as AOSP's does, so
    the inflater no longer applies them again after construction, which replaced
    a typeface a subclass set in its constructor with the default family
    (`MACAPK_INFLATE_TYPEFACE_AGAIN=1` restores that); `LayoutInflater` applies
    `android:contentDescription` and `android:importantForAccessibility` (a
    view that read its description in its constructor threw and became an empty
    FrameLayout); and `MacApkJobScheduler` calls the service's `onBind` once,
    as JobServiceContext's bind does, before looking for a JobServiceEngine the
    service makes there (IBM Trusteer's job was dropped; `MACAPK_JOB_NO_ONBIND=1`
    skips it).
- `runtime/java/com/macapk/loader/LayoutSweep.java` (new), `Boot.java`: the
  screens sweep's Win.apk side, a debug mode off unless `MACAPK_LAYOUT_SWEEP=1`
  (see [Every screen against Android](#every-screen-against-android)). Boot
  runs it on the main thread once `Application.onCreate` has returned, instead
  of starting the launcher activity, and it ends the process. It names only
  `android.*`, so `windows/test/screens` compiles the same file into its
  Robolectric oracle.
- `runtime/java/com/macapk/loader/gfx/GfxBackend.java` (new), `Java2DGfx.java`
  (new): step (1) of [milestone 2](#milestone-2-the-runtime-on-aosp-graphics),
  the one seam between the framework and Win.apk's own `android.graphics` /
  `android.text` plumbing. Every call from outside those packages to a
  `macapk*` method or field of theirs, or to a `MacApk*` class, now goes
  through it, with no change in behaviour: `android/content/res/Resources.java`,
  `Configuration.java`, `TypedArray.java`, `ColorStateList.java`,
  `android/view/View.java`, `ViewGroup.java`, `TextureView.java`,
  `Surface.java`, `SurfaceView.java`, `LayoutInflater.java`,
  `android/widget/TextView.java`, `ImageView.java`, `Switch.java`,
  `android/hardware/Camera.java`, `camera2/CameraCaptureSessionImpl.java`,
  `android/opengl/GLUtils.java`, `javax/microedition/khronos/egl/MacApkGlWindow.java`,
  `com/macapk/loader/Boot.java`, `HostVideo.java`, `ViewCanvasDriver.java`,
  `WindowOverlayStack.java`.
- `runtime/java/com/macapk/loader/gfx/AospGfx.java`, `AospVector.java` (new),
  `GfxBackend.java`, `Java2DGfx.java`: step (2) of
  [milestone 2](#milestone-2-the-runtime-on-aosp-graphics), the backend on
  AOSP's classes under `WINAPK_AOSP_GFX=1`, and the seam's new members
  (`createFrameBitmap`, `readsShapeRing`/`readsShapeTint`, `tintList`,
  `vectorFromResource`/`vectorFromElement`, `rotateDrawable`, `layerDrawable`,
  `rippleDrawable`, `animatedVector`), which the Java2D side answers with the
  calls they replace: `android/content/res/Resources.java`,
  `android/view/Surface.java`, `SurfaceView.java`, `TextureView.java`,
  `android/widget/Switch.java`, `com/macapk/loader/Boot.java`,
  `GlideRescue.java`, `ViewCanvasDriver.java`. AOSP's hidden members its
  graphics and text classes call, added: `android/text/TextUtils.java`,
  `TextPaint.java`, `SpannableStringBuilder.java`,
  `android/content/res/ColorStateList.java`, `TypedArray.java`,
  `android/util/StateSet.java`, `android/view/Choreographer.java`,
  `android/os/Parcel.java`, `android/animation/PropertyValuesHolder.java`.
  Under the switch only: `android/view/Gravity.java` places by AOSP's rules,
  and `Resources` logs a drawable that did not inflate.
- `runtime/java/com/macapk/loader/view/ViewHost.java` (new), `WinApkViewHost.java`
  (new): step 0 of `docs/research/aosp-views.md`, views half, the one seam
  between the rest of the runtime and Win.apk's own `android.view` /
  `android.widget` / `com.android.internal.policy` members that AOSP's classes
  do not have (the `macapk*` methods and fields, `MacApkDragSession`,
  `ViewRootImpl.getInstance`, Win.apk's constructors of `Display`,
  `InputDevice`, `DecorView` and the in-process managers...). Every call from
  outside those packages now goes through it, with no change in behaviour:
  `WinApkViewHost` calls the member each method stands for on the same receiver
  with the same arguments (a hook a subclass overrides is still called
  virtually). Grouped as a window's root (attach, detach, remove, the window
  records), the frame (traversals, damage, the tree observer, insets), input
  (key fields, touch mode, hover, listeners, input devices), drag, the IME's
  target, the display and the system bars, the view tree's owners, the
  in-process services, dialog chrome, and `Surface` / `SurfaceView`'s GL, video
  and encoder plumbing (`ViewHost.FrameSink` stands for `Surface.MacapkFrameSink`).
  199 sites in 29 files: `android/app/Activity.java`, `AlertDialog.java`,
  `Application.java`, `LocaleManager.java`, `NativeActivity.java`,
  `android/content/Context.java`, `android/hardware/SensorManager.java`,
  `camera2/CameraCaptureSessionImpl.java`, `camera2/params/StreamConfigurationMap.java`,
  `display/DisplayManager.java`, `input/InputManager.java`,
  `android/media/MediaCodec.java`, `android/opengl/GLSurfaceView.java`,
  `android/webkit/WebView.java`, `javax/microedition/khronos/egl/MacApkEGL10.java`,
  `MacApkGlWindow.java`, and in `com/macapk/loader/` `ActivityHost.java`,
  `ComposeViewFallback.java`, `DisplayConfig.java`, `GamepadBridge.java`,
  `HostVideo.java`, `InputRecorder.java`, `KeyLogPolicy.java`,
  `MacApkWindowManager.java`, `StatusBarDriver.java`, `TouchDispatcher.java`,
  `ViewCanvasDriver.java` (its reflective lookup of
  `ViewRootImpl.macapkConsumeDamage` became the seam's call), `ViewTextDump.java`,
  `WindowOverlayStack.java`. `android/view/SurfaceView.java`: the ten
  `protected` GL hooks `GLSurfaceView` calls are `public` (still `final`), since
  the seam calls them from another package. What stays direct: constructors
  that are `super()` calls (`Activity`'s and `Dialog`'s `Window` subclasses,
  `Dialog`'s anonymous `DecorView`), `GLSurfaceView`'s overrides of
  `macapkDrawFrame` / `macapkSurfaceChanged` / `macapkRendersOnOsMain` and their
  `super.` calls, `Dialog`'s override of `macapkOwnInflater`, and
  `Gravity.parse` in `android.content.res` (the resources seam's). The UI probe,
  the screens sweep on Fossify Gallery and Open Sudoku, `FrameworkStubsTest` and
  `DamageRectTest` give identical output before and after.

## License

AGPL-3.0, like Mac.apk (see `LICENSE`). The Mac.apk name and logo belong to the
upstream project (see `TRADEMARKS.md`).
