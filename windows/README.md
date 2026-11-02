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
| `host/` | `winapk-runtime.exe` (`runtime_main.cpp`), the app window (`win32_window.cpp`), the host natives (`native_*.cpp`, one file per Java class; `host_natives.cpp` binds them), the GL layer (`gl/`), the app-library loading path (`guest_libs.cpp`, `guest_stub.cpp`, `translator_main.cpp`), shared storage (`user_folders.cpp`) |
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
  the interpreter until its translation is ready.
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
```

`--apk` takes a plain APK or a split bundle (`.apkm`, `.xapk`, `.apks`).
`--data` defaults to `%LOCALAPPDATA%\Win.apk`; a separate folder keeps a
development run away from the user's library. `--install-only` installs,
converts and writes the icon and name, then exits (3 if the app is running).
`--gms` runs upstream's `com.macapk.gms.GmsCli` (`status`, `selftest`,
`accounts`, `signin-window`, `signout`, `checkin`, `license`, `gcmtoken`...)
with no app; exit 0 done, 1 error, 2 cancelled or bad arguments.

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
`WINAPK_AOT_CLANG`, `WINAPK_ABIS`, `WINAPK_WEBVIEW2`, `WINAPK_PROXY`,
`WINAPK_ALLOW_SECOND_INSTANCE`, the headless ones) are in the
[user guide](../docs/USER_GUIDE.md#command-line). For development and tests:

| Variable | Effect |
|---|---|
| `WINAPK_HEADLESS=1` | No window; `WINAPK_HEADLESS_FRAME=<file.bmp>` saves the last frame and `WINAPK_HEADLESS_EXIT_AFTER_FRAMES=<n>` exits after that many presented frames (an idle app counts too) |
| `WINAPK_TEST_WORKAREA=<w>x<h>[+<x>+<y>]` | Stands in for the monitor's work area, in pixels; `@file` reads the value from a file at every fit |
| `WINAPK_TEST_RESIZE=<w>x<h>` | A second after the first frame, resizes the window to a client area of that many pixels (CI's "Boot an APK in a window") |
| `WINAPK_DENSITY=<dpi>` | Overrides the density the app sees |
| `WINAPK_WINDOW_RTL=1` or `0` | Overrides the locale's reading layout for the app window's frame |
| `WINAPK_ANGLE_BACKEND=d3d11` or `swiftshader` | Forces a graphics backend (the default is a GPU, then WARP, then SwiftShader) |
| `MACAPK_LETTERBOX=0` | With `WINAPK_RESIZABLE=0`, stretches the phone over the whole window instead of letterboxing |
| `MACAPK_SCROLL_SCALE=<n>` | Scales the drag the mouse wheel makes |
| `WINAPK_ARM64_ENGINE=interp` | Forces the arm64 interpreter instead of the JIT |
| `WINAPK_ARM64_STATS=1` | Prints at exit where the guest's time went (host functions versus arm64 code, transitions, interpreter fallbacks) |
| `WINAPK_ARM64_FP=exact` | Keeps the arm64 JIT's floating point bit-exact (it uses the host's FMA otherwise) |
| `WINAPK_AOT_OPTNONE_INSNS=<n>` | The function size above which the translation is compiled unoptimized |
| `WINAPK_SYSTEM_CACERTS=0` | Does not build Android's CA store from the Windows roots |
| `WINAPK_USER_FOLDER_TARGETS` | Names the Windows folders the shared-storage junctions point at (tests) |
| `MACAPK_USER_FOLDERS=0`, `HKCU\Software\Win.apk\UserFolders=0` | Turn the shared-storage mapping off (the setting is what the launcher writes) |
| `MACAPK_JVM_OPTS` | Extra JVM options, passed through |
| `MACAPK_ALL_PERMS_ASK=0` | Returns the permission groups beyond storage and location to the install-time rule (declared means held), the Mac's behaviour; `MACAPK_MEDIA_PERMS_ASK=0` and `MACAPK_ALLFILES_HONEST=0` keep their upstream meaning |
| `MACAPK_GMS_PRESENT=0`, `MACAPK_GMS_FCM=0`, `MACAPK_GMS_SIGNIN=0`, `MACAPK_GMS_LOCATION=0`, `MACAPK_GMS_DIR` | Upstream's Play services switches and the state folder |
| `MACAPK_VIRTUAL_CAMERA=0`, `MACAPK_SV_OVERSIZE_HOLDER=0`, `MACAPK_MANIFEST_FEATURE_SPLITS=0`, `MACAPK_LIFECYCLE_*=0`, `MACAPK_CONTEXT_MCLASSLOADER=0` | Switch off individual framework fixes listed under [Changes to the upstream tree](#changes-to-the-upstream-tree) |

The Java framework has many more `MACAPK_*` switches of upstream's; grep
`runtime/java` for the one you need.

## Tests

Every test is a program built by CMake; `windows-poc.yml` runs each of them on
`windows-latest`, and `test/run_*_wine.sh` run the main ones under Wine.

| Program | What it checks |
|---|---|
| `winapk-elfrun <poc.so> <poc_packed.so>` | Two builds of the Android test library (plain RELA, and APS2 + RELR) load and run: argument passing across the ABI boundary, RELATIVE and `R_X86_64_64` relocations, constructors, shim calls, stack-protected code on new threads, a guest comparator in host `qsort`, a host callback from guest code |
| `winapk-crash-test` | The crash path of `host/crash_report.cpp`: a child with a guest SIGSEGV handler that declines three times, then the report line with the faulting module and offset, a minidump (`MDMP`) in the logs folder, pruning to the last three |
| `winapk-thunk-test` | Native-method thunks with mixed, stack-overflowing and narrow arguments; the variadic entry stub's `va_list`; the tagged zero stub |
| `winapk-jni-test`, `winapk-jni-variadic-test` | The guest `JNIEnv` against a mock JVM; the variadic bridge for all four call kinds and every argument type, through `...` and `va_list` |
| `winapk-linker-test <dir>` | A four-library tree with a `DT_NEEDED` cycle and a runtime plugin: constructor order, symbol resolution, `dlsym` scopes, the guest's `dlopen`, `dlsym`, `dlerror` and `dladdr` |
| `winapk-bionic-test <bionic_guest.so> <scratch dir>` | `test/guest/bionic_guest.c` exercises the shim from guest code: strings, printf and scanf through the System V `va_list`, threads and keys, files and directories, ELF TLS, system properties, locales, futex, an 8 GiB `PROT_NONE` reservation, pipes, eventfd, epoll, sockets, `ALooper`, clocks, `mmap` |
| `winapk-arm64-test <dir>` | The arm64 engines: `test/guest/arm64_isa.c` built for the host and for arm64 at three optimization levels must agree on every function, on the interpreter, the JIT and the AOT translation; then `arm64_bridge_guest.c` (calls in both directions, variadics, threads, atexit) and `arm64_bench.c` (the speed ratios) |
| `winapk-audio-test <test/audio>` | The WASAPI mixer and the decoders |
| `winapk-user-folders-test <dir>` | The shared-storage junction rules |
| `winapk-window-test` | The window geometry decisions (the letterbox, how a frame is shown) without a window |
| `winapk-gl-test` | ANGLE makes a context, clears a pbuffer and the read-back frame has the right colors (WARP or SwiftShader on CI) |
| `winapk-jvm-run <jni_guest.so> <classes>` | A real HotSpot JVM loads a guest library through the linker: its `JNI_OnLoad` registers a native through the guest `RegisterNatives`, another native is bound by name through the `NativeMethodBind` hook, both are called from Java and call back through the variadic bridge. Run with the x86_64 and the arm64 build of the library, and again on the staged `android-framework.jar` for libjnigraphics |
| `test/java/PermissionModelTest.java` | `SpecialAccessStore.GroupModel` against PermissionController's rules (grant, deny, deny twice, auto grant, background permissions, legacy targets, App info rows) |
| `test/java/FileSeparatorRewriteTest.java` | Guest code reads Android's `/` from `File.separator` while the JDK's own classes keep the backslash |
| Headless boot, window boot, GMS CLI (CI steps) | The staged runtime boots the F-Droid Sudoku APK headless (30 frames) and in a window with a resize (`WINAPK_TEST_RESIZE=400x600`), and `--gms selftest` and `--gms status` pass |

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
    [--launcher wine] [--only pkg,...]
```

Each app gets one status: `drew` (ran its frames and the last frame has real
content), `blank` (the last frame is one or two colors), `no-frame` (no frame
before the timeout) or `crashed`. The report (`report.md`, `report.json`)
collects the host natives called while still stubbed, the exceptions Java
logged, HotSpot crash files, and for native code the system functions the shim
lacks and the libraries that could not load. `compat-sweep.yml` runs it on
pull requests and attaches the report, the logs and the last frames.

## CI and releases

| Workflow | Trigger | What it does |
|---|---|---|
| `.github/workflows/windows-poc.yml` | Pull requests touching `windows/`; by hand | Builds the guest libraries on Ubuntu; on `windows-latest` builds everything with MSYS2 CLANG64 and Temurin 21, runs every test above, stages the runtime, boots an APK headless and in a window, runs the GMS CLI, then packages the staged folder with a Temurin JRE (`winapk-preview` artifact) and builds the installer (`winapk-setup`) |
| `.github/workflows/compat-sweep.yml` | Pull requests touching `windows/` or `runtime/java/`; by hand | The compatibility sweep, with the ranked report as the run's summary |
| `.github/workflows/windows-release.yml` | By hand only, with a tag such as `v0.2.0-preview` | Builds, stages, packages `Win.apk-windows-x64.zip` with a JRE and `package/README.txt`, smoke-tests the package (boots an APK headless), builds `Win.apk-Setup.exe` with Inno Setup, and publishes both as a GitHub pre-release with `package/RELEASE_NOTES.md` as the notes |

Nothing runs on a push to `main`, and nothing is released automatically. The
platform resource APK and the ANGLE download are cached between runs.

Pull requests: one topic each, CI green on Windows before merging, merged
with rebase. [`../CONTRIBUTING.md`](../CONTRIBUTING.md) has the rest.

## Keeping up with upstream

```sh
git remote add upstream https://github.com/kksimp/Mac.apk.git   # once
git fetch upstream
git merge upstream/main
```

Upstream's macOS-only files were removed here, so a merge that touches them
stops with a modify/delete conflict: keep them deleted (`git rm` the file).
### Changes to the upstream tree

Files in `runtime/` are changed only where a Windows hook is unavoidable, and
every such change is listed here:

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
  AOSP, not in the middle of it. Not Windows-specific.
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
  TypedArray holding `android:defaultValue`; the String the AttributeSet twin
  reads broke DroidFish's ColorPickerPreference with a ClassCastException. Not
  Windows-specific.
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
  answers.
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
  the old order. Not Windows-specific.
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
- `runtime/java/com/macapk/loader/ActivityHost.java`: without the AOSP window
  policy (Windows), an activity that owns the window box asks the host for its
  manifest's `screenOrientation` at launch (`NativeCocoa.applyWindowOrientation`,
  the same native the policy's branch uses), so a portrait-only app opens
  portrait although the library opens apps landscape. A host without the
  native keeps the old behaviour.

## License

AGPL-3.0, like Mac.apk (see `LICENSE`). The Mac.apk name and logo belong to the
upstream project (see `TRADEMARKS.md`).
