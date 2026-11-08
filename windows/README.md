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
- [The compatibility sweep](#the-compatibility-sweep)
- [CI and releases](#ci-and-releases)
- [Keeping up with upstream](#keeping-up-with-upstream)
  - [Changes to the upstream tree](#changes-to-the-upstream-tree)
- [License](#license)

## What is here

| Path | What it is |
|---|---|
| `include/winapk/`, `src/` | The ELF loader (`elf_loader.cpp`), the guest linker (`linker.cpp`), dynamic ELF TLS (`elf_tls.cpp`), the `%fs` stack-guard patch (`fs_tls_patch.cpp`), the ABI thunks (`abi_thunk.cpp`), the guest `JNIEnv` (`guest_jni.cpp`, `jni_slots.inc`), the variadic JNI bridge (`jni_variadic.cpp`), native-method binding (`jni_bind.cpp`), libjnigraphics (`jnigraphics.cpp`) |
| `src/mini_bionic.cpp`, `src/bionic/` | The Bionic shim: Android's libc, libdl, liblog, libm, libz and the NDK's `ALooper` on the UCRT and Win32, about 520 System V functions |
| `src/arm64/` | The arm64 engines: interpreter (`arm64_cpu.cpp`, `arm64_simd.cpp`), JIT (`arm64_jit.cpp`, dynarmic), ahead-of-time translator (`arm64_aot.cpp`) and the AAPCS64 bridge (`arm64_guest.cpp`, `arm64_abi.cpp`) |
| `host/` | `winapk-runtime.exe` (`runtime_main.cpp`), the app window (`win32_window.cpp`), the host natives (`native_*.cpp`, one file per Java class; `host_natives.cpp` binds them), the GL layer (`gl/`), the app-library loading path (`guest_libs.cpp`, `guest_stub.cpp`, `translator_main.cpp`), shared storage (`user_folders.cpp`), java.io's file opens with `FILE_SHARE_DELETE` (`java_io_open.cpp`) |
| `launcher/` | `Win.apk.exe`, the app library, the install helper and the permission dialogs (`launcher_main.cpp`, `launcher.rc`, the icon) |
| `java/` | Windows-only Java: `com.winapk.tools.AppInstall` and `AppMeta` (`winapk-tools.jar`), and `java_base_patch/` (an overlay for `java.base`) |
| `tools/` | `winapk-elfrun` (the loader test runner), `winapk-jvm-run` (the real-JVM test), `winapk-aot` (`arm64_aot_main.cpp`), and the generators `gen_jni_slots.py`, `gen_gl_stubs.py`, `gen_icon.py` |
| `scripts/` | `build_java.sh`, `build_framework_res.sh`, `fetch_angle.sh`, `fetch_webview2.sh`, `stage_clang.sh`, `stage_runtime.sh` |
| `test/` | The unit tests (`*_test.cpp`), the guest test libraries (`guest/`, built with plain clang + lld, no NDK), the Java tests (`java/`), the Wine runners (`run_*_wine.sh`), the compatibility sweep (`compat/`) |
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
  the newest release. The release workflow passes its tag; the default is 0.1.0.
- `stage_runtime.sh <build dir> [out]` assembles a runnable folder (default
  `windows/build/runtime`): `Win.apk.exe`, `bin/` (the runtime, the
  translator and stub DLL, `winapk-aot`, ANGLE from `fetch_angle.sh`,
  `WebView2Loader.dll` from `fetch_webview2.sh`, `tools/framework-res.apk`),
  `lib/` (the jars from `build_java.sh`) and `clang/` (from
  `stage_clang.sh`, the compiler the arm64 translation uses). The JDK is not
  copied: the runtime uses `<out>/jre` if present, else `WINAPK_JAVA_HOME` or
  `JAVA_HOME`.
- `build_java.sh [out]` builds upstream's Java side with a stock JDK 21:
  `android-framework.jar`, `macapk-loader.jar`, `winapk-tools.jar`, the
  BouncyCastle provider, and the `java.base` and `jdk.unsupported` patches.
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
| `windows/test/run_poc_wine.sh` | Builds the loader and the guest libraries and runs the loader, linker, arm64 and crash-report tests under Wine. The toolchain file adds `-femulated-tls` and winpthreads, which GCC's mingw-w64 libstdc++ needs |
| `WINAPK_WIN_JDK=<windows-jdk> windows/test/run_e2e_wine.sh` | Builds and runs the real-JVM end-to-end test, with the x86_64 and the arm64 guest library (`WINAPK_HOST_JAVAC` picks the javac) |
| `windows/test/aot_worker/run_aot_worker_wine.sh` | The background translation worker (`host/runtime_main.cpp`) alone, with a stand-in for clang, on generated arm64 libraries: parts, the stamp and the shared cache; a worker killed half way (no compiler outlives it, the next run goes on); failures and their waits; a library translated from its profile, at most once an hour; the Settings switch off; four apps' workers on one library and one cache at once; a worker killed at each step of a write (`WINAPK_AOT_FAULT`); a DLL or a cache entry cut short; a library dated in the future; stale temporary files. Needs wine64, mingw-w64 g++, and clang, lld and llvm-strip for the guest libraries |
| `WINAPK_WIN_JDK=<windows-jdk> windows/test/run_app_wine.sh app.apk [frames]` | Stages the runtime and boots an APK headless (needs `curl` and `unzip` for the first resource build); leaves `windows/build/frame.bmp` and `boot.log` |
| `windows/test/build_guest.sh [out]`, `build_e2e.sh <jni include> <javac> <out>` | Build the guest test libraries alone |

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
| `WINAPK_TEST_RESIZE=<w>x<h>` | A second after the first frame, resizes the window to a client area of that many pixels (CI's "Boot an APK in a window") |
| `MACAPK_APK_PATH` | Set by the runtime, not by hand: the installed APK, which `PresentedSdk` reads the app's targetSdk and minSdk from to choose the API level the app is presented (its targetSdk within 29..36, never below its minSdk); `MACAPK_SDK_INT=<n>` forces a level, `MACAPK_SDK_FROM_TARGET=0` presents 29 (still never below the minSdk) |
| `MACAPK_BROKER_TRIGGER_CHECKIN=0` | The broker does not nudge Play services to check in at boot (it nudges at most three times, and not at all once Play services has broadcast `CHECKIN_COMPLETE`) |
| `WINAPK_READY_EVENT=<handle>` | Set by the launcher, not by hand: an inherited event handle, as a decimal number, that the runtime sets once its window has painted the app's first frame (the launch splash waits for it) |
| `WINAPK_EMBED_PARENT=<hwnd>` | Set by the launcher, not by hand: a window handle, as a decimal number, that the app's window is created as a borderless child of, filling it (Google Play inside the library window); not a window, or the child cannot be made: a window of its own |
| `WINAPK_DENSITY=<dpi>` | Overrides the density the app sees |
| `WINAPK_WINDOW_RTL=1` or `0` | Overrides the locale's reading layout for the app window's frame |
| `WINAPK_ANGLE_BACKEND=d3d11` or `swiftshader` | Forces a graphics backend (the default is a GPU, then WARP, then SwiftShader) |
| `MACAPK_LETTERBOX=0` | With `WINAPK_RESIZABLE=0`, stretches the phone over the whole window instead of letterboxing |
| `MACAPK_SCROLL_SCALE=<n>` | Scales the drag the mouse wheel makes |
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
| `MACAPK_QUIET_CLASSLOAD=0` | Logs a line for every class the app's class loader loads (the runtime sets it to 1, as upstream's launcher does: thousands of lines cost up to 0.4 s of a start) |
| `MACAPK_ALL_PERMS_ASK=0` | Returns the permission groups beyond storage and location to the install-time rule (declared means held), the Mac's behaviour; `MACAPK_MEDIA_PERMS_ASK=0` and `MACAPK_ALLFILES_HONEST=0` keep their upstream meaning |
| `MACAPK_GMS_PRESENT=0`, `MACAPK_GMS_FCM=0`, `MACAPK_GMS_SIGNIN=0`, `MACAPK_GMS_LOCATION=0`, `MACAPK_GMS_DIR` | Upstream's Play services switches and the state folder |
| `MACAPK_GMS_GOOGLE_CERT=0` | Reports the platform certificate for `com.google.android.gms` and `com.android.vending` instead of Google's (apps then see Play services as invalid) |
| `MACAPK_VIRTUAL_CAMERA=0`, `MACAPK_SV_OVERSIZE_HOLDER=0`, `MACAPK_MANIFEST_FEATURE_SPLITS=0`, `MACAPK_LIFECYCLE_*=0`, `MACAPK_CONTEXT_MCLASSLOADER=0` | Switch off individual framework fixes listed under [Changes to the upstream tree](#changes-to-the-upstream-tree) |

The Java framework has many more `MACAPK_*` switches of upstream's; grep
`runtime/java` for the one you need.

## Tests

Every test is a program built by CMake; `ci.yml` runs each of them on
`windows-latest`, and `test/run_*_wine.sh` run the main ones under Wine.

| Program | What it checks |
|---|---|
| `winapk-elfrun <poc.so> <poc_packed.so>` | Two builds of the Android test library (plain RELA, and APS2 + RELR) load and run: argument passing across the ABI boundary, RELATIVE and `R_X86_64_64` relocations, constructors, shim calls, stack-protected code on new threads, a guest comparator in host `qsort`, a host callback from guest code |
| `winapk-aot-guard-test <scratch dir>` | The translation's crash guard (`host/aot_guard.h`) through the real crash path, each step a child process with `winapk-aot-guard-dll.dll` loaded as its translation: a fault with translated code loaded is counted, a clean run sets the count back, a crash with nothing translated loaded is not counted; after a fault and an `abort()` in a row the next start has translation off and logs the `off for this app` line naming the state file; two crashes on the JIT log `crashes on the JIT too: not the translation` and the next start turns it on for good; another build of the app, or deleting the file, starts over |
| `winapk-crash-test` | The crash path of `host/crash_report.cpp`: a child with a guest SIGSEGV handler that declines three times, then the report line with the faulting module and offset, a minidump (`MDMP`) in the logs folder, pruning to the last three |
| `winapk-bluetooth-test` | The Bluetooth host's pure parts without a radio: SHA-1 against the FIPS vectors, the parameterized WinRT interface ids derived from type signatures against five published ones, UUID and address conversions, advertisement bytes |
| `ScanRecordTest` (`test/java/`) | `ScanRecord`'s advertisement parser and `ScanFilter` against the built jars |
| `winapk-video-test <test/video dir>` | The Media Foundation video backend without a JVM: the YUV converter, a decoder MFT, the source reader and the player on `test/video/color.mp4` (1 s of H.264 + AAC); decoder checks are skipped where no H.264 MFT exists |
| `winapk-thunk-test` | Native-method thunks with mixed, stack-overflowing and narrow arguments; the variadic entry stub's `va_list`; the tagged zero stub |
| `winapk-jni-test`, `winapk-jni-variadic-test` | The guest `JNIEnv` against a mock JVM; the variadic bridge for all four call kinds and every argument type, through `...` and `va_list` |
| `winapk-linker-test <dir>` | A four-library tree with a `DT_NEEDED` cycle and a runtime plugin: constructor order, symbol resolution, `dlsym` scopes, the guest's `dlopen`, `dlsym`, `dlerror` and `dladdr` |
| `winapk-bionic-test <bionic_guest.so> <scratch dir>` | `test/guest/bionic_guest.c` exercises the shim from guest code: strings, printf and scanf through the System V `va_list`, threads and keys, files and directories, ELF TLS, system properties, locales, futex, an 8 GiB `PROT_NONE` reservation, pipes, eventfd, epoll, sockets, `ALooper`, clocks, `mmap` |
| `winapk-arm64-test <dir>` | The arm64 engines: `test/guest/arm64_isa.c` built for the host and for arm64 at three optimization levels must agree on every function, on the interpreter, the JIT and the AOT translation; then `arm64_bridge_guest.c` (calls in both directions, variadics, threads, atexit); `arm64_flow_guest.c` (returns into an ancestor frame with RET and BR, coroutines, RETAA, deep recursion on a 1 MB thread, saturating conversions, FRECPS/FRSQRTS, an exclusive-monitor counter shared by translated code and the JIT, the STLR/LDAR litmus test) against values worked out by hand; `arm64_patch_guest.c` (a library patching its own code, and code patched by one thread while another runs it), `arm64_eh_guest.cpp` (C++ exceptions through the NDK's libc++ and unwinder, built when `ANDROID_NDK_HOME` names an NDK), a library unloaded with `dlclose`, one linked at another address, a translation cut short under a stamp that says it is sealed (refused) and the same one whole (loaded); 400 functions of random instructions from `gen_arm64_fuzz.py`, whose results on the JIT and the translation must match the interpreter's (`WINAPK_FUZZ_STATE=<i>` starts every function from function i's states, to bisect a difference); and `arm64_bench.c` (the speed ratios) |
| `winapk-aot-cache-test <scratch dir>` | The arm64 translation's files (`include/winapk/aot_files.h`) in real processes, with `winapk-aot-test-dll.dll` as the translation: a seal against DLLs cut short, emptied, zeroed or changed; eight processes taking, making, putting and loading one cache entry at once (hard links and copies); writers killed at random moments and at each step of a write; broken cache entries deleted and replaced; eviction and replacement while another process has the DLL loaded; the clock (60 days, dates in the future, the size cap); stamps rewritten by four processes while another reads them |
| `winapk-audio-test <test/audio>` | The WASAPI mixer and the decoders |
| `winapk-user-folders-test <dir>` | The shared-storage junction rules |
| `winapk-window-test` | The window geometry decisions (the letterbox, how a frame is shown) without a window |
| `winapk-gl-test` | ANGLE makes a context, clears a pbuffer and the read-back frame has the right colors (WARP or SwiftShader on CI) |
| `winapk-jvm-run <jni_guest.so> <classes>` | A real HotSpot JVM loads a guest library through the linker: its `JNI_OnLoad` registers a native through the guest `RegisterNatives`, another native is bound by name through the `NativeMethodBind` hook, both are called from Java and call back through the variadic bridge. Run with the x86_64 and the arm64 build of the library, and again on the staged `android-framework.jar` for libjnigraphics |
| `test/java/PermissionModelTest.java` | `SpecialAccessStore.GroupModel` against PermissionController's rules (grant, deny, deny twice, auto grant, background permissions, legacy targets, App info rows) |
| `test/java/FileSeparatorRewriteTest.java` | Guest code reads Android's `/` from `File.separator` while the JDK's own classes keep the backslash |
| `winapk-binder-test <classpath> com.macapk.loader.BinderTransportTest` | `host/binder_win.cpp` between real JVMs in three processes (`test/java/com/macapk/loader/BinderTransportTest.java`): transact, the OS calling pid and the receiver-derived uid, oneway order, a nested callback, binder identity both ways, a Messenger and its replyTo with the sender's uid in `Message.sendingUid` and a PendingIntent's creator uid, fds both ways, death notification; then the system broker's checks: a process that is no installed app, or that claims another package, is refused, android:exported, the package uid |
| `winapk-binder-test <classpath> FileShareDeleteTest` | java.io's opens as `host/java_io_open.cpp` binds them (`test/java/FileShareDeleteTest.java`, with the java.base overlay from `WINAPK_TEST_JAVA_BASE_PATCH`): a file renamed or deleted while a stream has it open, and renamed over a file a stream has open, as on Android; append, truncate, the `FileNotFoundException` text and the rest as the JDK has them |
| Headless boot, window boot, service host, GMS CLI (CI steps) | The staged runtime boots the F-Droid Sudoku APK headless (30 frames) and in a window with a resize (`WINAPK_TEST_RESIZE=400x600`); started as a service host (`MACAPK_SERVICE_HOST=1`) and then opened, the open replaces the headless process and draws; `--gms selftest` and `--gms status` pass; `--verify-apk` answers 4 for a test-signed APK, 3 for an unsigned one and 1 for a file that is no app |

The guest libraries are built with plain clang + lld (`test/build_guest.sh`,
`test/build_e2e.sh`), so no NDK is needed; CI builds them on Ubuntu and
hands them to the Windows job.

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
lacks and the libraries that could not load. Each line of an app's log
(`logs/<package>.log`) starts with the seconds since its start, so an app that
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

## CI and releases

| Workflow | Trigger | What it does |
|---|---|---|
| `.github/workflows/ci.yml` | Pull requests touching `windows/` or `runtime/`; by hand; a push to `main` (build only) | The pipeline below |
| `.github/workflows/windows-release.yml` | By hand only, with a tag such as `v0.2.0-preview` | Builds with `-DWINAPK_VERSION=<tag>`, stages, packages `Win.apk-windows-x64.zip` with a JRE and `package/README.txt`, smoke-tests the package (boots an APK headless), builds `Win.apk-Setup.exe` with Inno Setup, and publishes both as a GitHub pre-release with `package/RELEASE_NOTES.md` as the notes |

`ci.yml`'s jobs, in the order they can start:

| Job | Runs on | Needs | What |
|---|---|---|---|
| What changed | Ubuntu | | Reads the pull request's files: docs alone run nothing; `launcher/` or `package/` alone build and package but run no app; anything else runs everything. By hand, everything; a push to `main`, the build alone |
| Guest libraries | Ubuntu | What changed | The x86_64 and arm64 test libraries and the end-to-end inputs (`test/build_guest.sh`, `test/build_e2e.sh`) |
| Launcher check | Ubuntu | What changed | The launcher compiled with MinGW-w64, warnings as errors: an answer in about a minute |
| Build | `windows-latest` | What changed | Every CMake target with MSYS2 CLANG64 through ccache (the cache is restored from the newest run, `main`'s for a pull request's first one), then `stage_runtime.sh build stage`. Uploads `winapk-binaries` (every exe) and `runtime-stage` |
| Unit tests | `windows-latest` | Guest libraries, Build | Every test program above, the arm64 AOT translation included |
| Runtime tests | `windows-latest` | Guest libraries, Build | The tests that need the staged jars or ANGLE, the APK booted headless and in a window (`headless-boot` artifact), the GMS CLI |
| App smoke test | `windows-latest` | Build | The ten apps of `test/compat/smoke.txt` (`--require-drew`): fails when one no longer draws (`smoke` artifact) |
| Sweep 1/4 to 4/4 | `windows-latest` × 4 | Build | Only by hand, with "Full compatibility sweep": a quarter of the corpus each (`--shard`), each with its own APK cache |
| Sweep report | Ubuntu | Sweep | `sweep.py --merge`: the run's summary and the `compat-sweep` artifact |
| Package | `windows-latest` | Build | The staged folder with a Temurin JRE (`winapk-preview`) and the installer (`winapk-setup`); beside the tests, not after them |

A new push to a pull request cancels its run in progress. Nothing is released
automatically. The platform resource APK, ANGLE, the compile cache and the
sweep's APKs are cached between runs. The runtime is staged into `stage/`,
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

- `runtime/java/com/macapk/loader/HostAppSupport.java` (new) and
  `InstallerOfRecord.java`: the small files every app process shares (the
  installer of record, and with Google services on the per-package uids and
  the broker's state) live in the data folder, `%LOCALAPPDATA%\Win.apk`,
  beside `installed_apps.json`, not in a `Library\Application Support` folder
  under the user profile. On the Mac nothing changes.
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
- `runtime/java/android/app/Activity.java`: `Window.setContentView(View)` puts
  the view into the decor's content frame and keeps the decor as the root, as
  AOSP's `PhoneWindow` does (a view added to the decor floats over the content),
  and `findViewById(android.R.id.content)` finds the sub-decor's frame in that
  tree before upstream's frame-root unwrap.
- `runtime/java/android/bluetooth/` (`BluetoothAdapter`, `BluetoothDevice`,
  `BluetoothGatt`, `BluetoothManager`, `le/BluetoothLeScanner`,
  `le/ScanRecord`): Win.apk's `HostBluetooth` versions (below). Upstream later
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
- `runtime/java/android/database/sqlite/SQLiteDatabase.java`: `BEGIN`,
  `COMMIT` and `ROLLBACK` given to `execSQL` nest in the transaction stack, as
  AOSP's `SQLiteSession.executeSpecial` does. Not Windows-specific.
- `runtime/java/android/view/LayoutInflater.java`: a `<merge>` layout inflated
  without a parent merges into a `FrameLayout` instead of being dropped, so
  `setContentView` of a `<merge>` layout works. Not Windows-specific.
- `runtime/java/com/macapk/loader/ActivityHost.java`: an activity that calls
  `finish()` in its own `onCreate` is torn down after `onCreate` returns, as on
  AOSP, not in the middle of it; one that calls it in `onResume` after
  `onPostResume` (AOSP destroys it in a later transaction; tearing it down
  inside `onResume` nulled the FragmentManager's host under `onPostResume`,
  "No activity" in sgtpuzzles). Not Windows-specific.
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
- `runtime/java/com/macapk/dex/InitRepointPlan.java`: the DEX converter also
  synthesises the constructor R8 deleted when the call site still names the
  allocated class (`new-instance T; invoke-direct T.<init>(D)` with no
  `T.<init>(D)` left, often no class data at all), which ART resolves up the
  superclass chain and HotSpot refuses with NoSuchMethodError. NetGuard's
  main activity and androidx.startup failed on it (170 such sites in one
  DEX). Not Windows-specific; worth offering upstream.
- `runtime/java/android/system/Os.java`: `chmod` and `fchmod` (Key Mapper
  died at start on the missing method). Not Windows-specific.
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
  native keeps the old behaviour.

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
  the JDK's CAs plus the roots Windows trusts (a filtering proxy's such as
  NetFree's among them), set before any TLS. `Boot.run` did it for an app's
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
  an app re-signed by someone else (NetFree's patcher) that Google refuses
  (`UNREGISTERED_ON_API_CONSOLE`) gets an explanation instead of a closed window.
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

## License

AGPL-3.0, like Mac.apk (see `LICENSE`). The Mac.apk name and logo belong to the
upstream project (see `TRADEMARKS.md`).
