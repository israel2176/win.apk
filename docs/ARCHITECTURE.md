# How Win.apk works

This document explains Win.apk's design: the problem an x64 PC poses for
Android apps, the three classes of apps, each layer of the runtime, what
happens during an install and a run, the data on disk, and how the project is
built and tested. It is written for someone who wants to understand or change
the code; [`windows/README.md`](../windows/README.md) has the build steps and
[`windows/PORTING.md`](../windows/PORTING.md) the design decisions, findings
and plan in detail.

- [The problem](#the-problem)
- [Three classes of apps](#three-classes-of-apps)
- [The layers](#the-layers)
  - [The Android framework in Java](#the-android-framework-in-java)
  - [The host](#the-host)
  - [The ELF loader and linker](#the-elf-loader-and-linker)
  - [Thread-local storage and the %fs patch](#thread-local-storage-and-the-fs-patch)
  - [The Bionic shim](#the-bionic-shim)
  - [ABI thunks and the guest JNI](#abi-thunks-and-the-guest-jni)
  - [The variadic JNI bridge](#the-variadic-jni-bridge)
  - [Loading an app's libraries](#loading-an-apps-libraries)
  - [ARM64 code: translation, JIT, interpreter](#arm64-code-translation-jit-interpreter)
  - [Graphics](#graphics)
  - [The window and the display](#the-window-and-the-display)
  - [Audio, media, camera, location and the other devices](#audio-media-camera-location-and-the-other-devices)
  - [The launcher](#the-launcher)
  - [The install helper and the installed-apps registry](#the-install-helper-and-the-installed-apps-registry)
  - [Permissions (app-ops)](#permissions-app-ops)
  - [Google Play services](#google-play-services)
- [An install, step by step](#an-install-step-by-step)
- [A run, step by step](#a-run-step-by-step)
- [Data on disk](#data-on-disk)
- [Build, tests and CI](#build-tests-and-ci)
- [What is not there](#what-is-not-there)
- [Terms](#terms)

## The problem

[Mac.apk](https://github.com/kksimp/Mac.apk) runs Android apps on Apple
Silicon by reimplementing the Android framework in Java (run on a stock
HotSpot JVM) and loading an app's native libraries directly, because Apple
Silicon and Android phones are both arm64 and their calling conventions
agree. Its Bionic shim maps Android's libc calls onto macOS.

A Windows PC with an Intel or AMD processor breaks four of those assumptions:

1. **The CPU cannot execute ARM code.** Most APKs ship only ARM libraries.
2. **Two calling conventions.** Android x86_64 code uses the System V AMD64
   convention; Windows uses the Microsoft x64 convention. Every call between
   an app's native code and the host, including every JNI call in both
   directions, has to be converted.
3. **The C library.** The Bionic shim is written against POSIX and Darwin.
   Windows needs its own implementation of threads, files, sockets, `mmap`,
   signals and the rest.
4. **System services.** Cocoa, Metal, AVFoundation, WKWebView, CoreLocation
   and the other Mac frameworks need Windows counterparts.

What ports as is: almost all of the Java framework and the DEX converter.
Win.apk's own code lives under `windows/`; `runtime/` stays close to
upstream so that fixes can be merged, and every change to it is listed in
[`windows/README.md`](../windows/README.md#changes-to-the-upstream-tree).

## Three classes of apps

| Apps | How they run |
|---|---|
| Java or Kotlin only (no `.so`) | On HotSpot, as on the Mac. They need only the host layer: window, input, graphics, audio, services. |
| Ship `lib/x86_64/*.so` | The libraries are loaded natively by Win.apk's ELF loader and run at full speed. Apps are installed with the ABI list `x86_64,arm64-v8a`, so this build is preferred whenever the APK or bundle has it. |
| Ship only `lib/arm64-v8a/*.so` | The libraries are translated ahead of time to C and compiled with clang; what the translation cannot follow runs on an ARM64 JIT, with an interpreter underneath. |

32-bit ARM (`armeabi-v7a`) is not supported. 32-bit Windows and 32-bit x86
Android libraries are out of scope.

## The layers

```
+------------------------------------------------------------------+
|  The app: DEX converted to Java classes, plus its .so libraries   |
+-----------------------------+------------------------------------+
|  Android framework in Java  |  x86_64 .so      |  arm64-v8a .so  |
|  (runtime/java, from        |  loaded as is    |  AOT -> C -> DLL|
|  Mac.apk) on HotSpot        |                  |  JIT, interp.   |
+-----------------------------+------------------+-----------------+
|  Guest JNI (System V JNIEnv) <-> thunks <-> HotSpot's JNIEnv      |
|  ELF loader, linker, ELF TLS, %fs patch                          |
|  Bionic shim: Android's libc, libdl, liblog, libm, libz, ...     |
+------------------------------------------------------------------+
|  Host natives: window (Win32), GL (ANGLE/D3D11), audio (WASAPI), |
|  WebView2, Media Foundation, geolocation, pickers, clipboard...  |
+------------------------------------------------------------------+
|  winapk-runtime.exe (one process per app)  |  Win.apk.exe        |
|                                            |  (library, helper)  |
+------------------------------------------------------------------+
```

### The Android framework in Java

`runtime/java/` is upstream's reimplementation of the Android framework
(`android.*`), its DEX-to-JVM converter (`com.macapk.dex`), and the boot
loader (`com.macapk.loader`: `Boot`, `ActivityHost`, the package manager, the
class loader). It compiles unchanged with a stock JDK 21 and runs on Windows
with a handful of hooks, each listed in `windows/README.md`. Java needs a
platform resource APK (`framework-res.apk`, built from AOSP sources with
`aapt2`) for every `@android:` resource reference.

The framework's own natives are declared in about 40 Java classes
(`NativeCocoa`, `NativeAudio`, `NativeSqlite`, `MacApkWebViewBridge`,
`LocationBridge`...). The host binds each of them: the Windows implementation
where there is one, otherwise a stub that returns zero and logs its name the
first time it is called (`windows/host/host_natives.cpp`). Those log lines
(`native not implemented on Windows yet`) are how missing pieces are found.

Windows-only Java lives in `windows/java/`: `com.winapk.tools.AppInstall`
(installing an APK or bundle in the device layout) and `AppMeta` (the icon
and name the launcher shows), plus an overlay for `java.base`.

### The host

`windows/host/` is `winapk-runtime.exe`, the Windows counterpart of
upstream's `runtime/src/main.cpp`. It finds the runtime files, installs and
converts the app, creates the HotSpot JVM in-process with upstream's options
(SerialGC, a 16 MB thread stack, upstream's `java.base` changes, bytecode
verification off) and class-data sharing (below), binds the host natives, and
calls `Boot.run`. One file per
Java class: `native_cocoa.cpp` (the screen, the render tick, input delivery,
presenting frames), `win32_window.cpp` (the window itself),
`native_audio.cpp`, `native_webview.cpp`, `native_camera.cpp`,
`native_location.cpp`, `native_activity.cpp`, `native_signin.cpp` and so on.

**The Java runtime and class-data sharing** (`host/cds.h`). Win.apk ships its
own JRE, built by `windows/scripts/bake_jre.sh` from Temurin 21's modules:
the JDK's tools, JFR, JVMCI and the like left out, debug information stripped,
and upstream's `java.base`/`jdk.unsupported` overlays (`lib\java_base_patch`,
`lib\jdk_unsupported_patch`) built into those modules instead of applied with
`--patch-module` at each start, because HotSpot turns class-data sharing (CDS)
off whenever a module is patched. Two archives then spare a start the parsing
and linking of classes it loaded before: the JRE's own (`bin\server\classes.jsa`,
the JDK's classes) and the framework archive (`<data>\cds\framework-<key>.jsa`:
the JDK's and the framework's classes, `android-framework.jar`,
`macapk-loader.jar` and an unsigned bcprov, one for all apps). The runtime
drops the `--patch-module` flags only when the JRE's
`lib\winapk-baked-patches` records the same SHA-256 as the patch folders (a
plain JDK, or a patch rebuilt since, keeps them and runs without CDS). The
framework archive is dumped by `java -Xshare:dump`, in the background after the
first app start of each Win.apk and JRE version (`winapk-runtime --cds-dump`),
from the class list Win.apk ships (`lib\winapk-framework.classlist`, the classes at
least three of the ten smoke apps load) and the classes that start loaded. The key changes with the
JRE, the jars, the shipped list and the JVM options; HotSpot itself checks the
JRE and the jars when it maps the file and otherwise starts without it. A new
archive is checked whole (`-XX:+VerifySharedSpaces`) on its first use and
after a crash; one that does not map is deleted and dumped again, twice at
most. The app's own classes are never archived: HotSpot verifies every class
it dumps, and dex-converted code often does not verify. Measurements:
`docs/research/startup-cds.md`.

The runtime's manifest sets the process's ANSI code page to UTF-8 (Windows
10 1903 and later), so the JVM reads paths with any characters whole; the
steps that run in a separate JVM (install, DEX conversion) run as a child of
`winapk-runtime.exe` (`--java-main`) rather than `java.exe` for the same
reason.

### The ELF loader and linker

Windows has no `<elf.h>` and cannot `mmap` a file at 4 KiB offsets, so
`windows/src/elf_loader.cpp` loads an Android x86_64 (or arm64) shared object
by copying its `PT_LOAD` segments into one `VirtualAlloc` region. It applies
RELA, RELR and Android's packed (APS2) relocations, resolves IRELATIVE
resolvers, sets page protections, seals `PT_GNU_RELRO`, and runs `DT_INIT`
and `DT_INIT_ARRAY`.

`windows/src/linker.cpp` is the guest linker. It loads a library with its
`DT_NEEDED` tree in Bionic's order (map all, link all, constructors with
dependencies first), answers system libraries (`libc.so`, `libm.so`,
`libdl.so`, `liblog.so`, `libz.so`, `libandroid.so`, `libEGL.so`,
`libGLESv2.so`...) from the shim, keeps the registry of loaded images, and
implements the guest's `dlopen`, `android_dlopen_ext`, `dlsym` (with
`RTLD_DEFAULT` and `RTLD_NEXT`), `dladdr`, `dlerror` and `dlclose`. A
library is reference counted and unloaded when its last reference goes
(`DT_FINI_ARRAY` and `DT_FINI` run, leftover `__cxa_atexit` entries and
every thread's TLS block for it are released, its native-method thunk
cache entries dropped, the mapping freed), unless `RTLD_NODELETE`, a
pending `thread_local` destructor or another loaded library still holds it;
thunk code stays in the pool. A library the JVM loaded keeps the JVM's
reference, as on Android. A system function the shim lacks is bound
to a stub that reports its name on first call and returns 0, so the library
still loads and the log names the next function to write. A missing system
*variable* is bound to zeroed memory instead, since code reads through it.

### Thread-local storage and the %fs patch

Android x86_64 code keeps thread-local data behind the `%fs` segment
register, which 64-bit Windows user mode cannot set. Two mechanisms cover
what compiled code actually does:

- **Dynamic ELF TLS** (`elf_tls.cpp`): `__tls_get_addr`, `DTPMOD64` and
  `DTPOFF64` relocations, with a block allocated per thread and module. This
  is what libraries built for API 29 and later use. Static TLS (`TPOFF64`) is
  refused.
- **The stack-protector patch** (`fs_tls_patch.cpp`): Bionic's stack guard is
  read with `mov rax, fs:[0x28]`. The loader rewrites the segment prefix so
  the instruction reads `gs:[0x28]` instead, which on x64 Windows is
  `NT_TIB.ArbitraryUserPointer`, a per-thread slot nothing else uses. The
  instruction length does not change.

### The Bionic shim

`windows/src/mini_bionic.cpp` and `windows/src/bionic/` implement Android's
C library for guest code on top of the Windows CRT and Win32, about 520
functions, every one declared with the System V calling convention
(`WINAPK_GUEST_ABI`). By area:

| File | What it covers |
|---|---|
| `string.cpp` | `string.h`, ctype, 32-bit `wchar_t`, `strto*`, the FORTIFY `_chk` variants |
| `printf.cpp` | A printf engine that reads the System V `va_list`; the printf and scanf families; liblog's variadic calls |
| `pthread.cpp` | Mutexes, condition variables, rwlocks, once, keys and threads on `WaitOnAddress`, over Bionic's struct layouts; semaphores |
| `stdio.cpp`, `fd.cpp` | One file-descriptor table shared with Java (`NativeOs`): files, `stat` in Bionic's layout, directories, `FILE`; pipes, eventfd and epoll; `poll`, `select`, `epoll_wait` |
| `socket.cpp` | BSD sockets on Winsock with Linux's constants translated; `getaddrinfo` |
| `paths.cpp` | Android's absolute paths (`/data`, `/sdcard`, `/system/fonts`, `/dev/null`) under the Android root |
| `looper.cpp` | The NDK's `ALooper` |
| `misc.cpp` | Clocks and time, `mmap` on `VirtualAlloc` (with `PROT_NONE` as a reservation), system properties, the guest environment, `syscall` for futex and friends, `sysconf`, `uname`, `getauxval`, libm |
| `locale.cpp` | The C and C.UTF-8 locales libc++ builds on, UTF-8 conversion, `wctype.h` |
| `signal.cpp` | Signals delivered to guest handlers: a vectored exception handler maps access violations, illegal instructions, divide errors and breakpoints to SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP and calls the handler with Bionic's `siginfo_t` and `ucontext_t` (registers written back, so a handler can fix the fault or redirect `rip`); `SA_SIGINFO`, `SA_ONSTACK` with `sigaltstack`, `SA_RESETHAND`, `SA_NODEFER`, `sa_mask`; `raise`, `kill`, `tgkill`, `abort` synchronously, `pthread_kill` by suspending the target and injecting the call; a handler that declines three times leaves the fault to the crash report. Not delivered: faults in HotSpot or in Windows DLLs, faults in arm64 guest code, handlers in arm64 libraries. POSIX timers do not fire |
| `setjmp.cpp` | `setjmp` and `longjmp` for guest code |
| `cacerts.cpp` | Android's `/system/etc/security/cacerts` built from the roots Windows trusts plus the JDK's |
| `zlib.cpp` | `libz.so` on a statically linked zlib, with the guest's `z_stream` layout converted on every call |

`errno` values are Linux numbers, translated from the CRT's and Win32's.
`libjnigraphics` (`AndroidBitmap_*`) is upstream's code, extracted at build
time and wrapped in System V entry points (`jnigraphics.cpp`).

### ABI thunks and the guest JNI

Every host function guest code can call is compiled with the System V
convention, and every guest function pointer the host calls has a System V
type, so clang emits the right prologues (`windows/include/winapk/guest_abi.h`).
Calls whose shape is only known at run time go through machine code generated
on the fly (`abi_thunk.cpp`):

- **Native-method thunks.** HotSpot calls a native method with the Microsoft
  convention. A thunk built from the method's Java signature moves the
  arguments into System V registers, swaps in the guest `JNIEnv*`, and calls
  the guest function.
- **Variadic entry stubs.** clang refuses `va_start` in a System V function on
  Windows, so variadic functions the guest calls (the printf family, the
  variadic JNI calls) enter through a stub that builds the System V `va_list`
  block itself.

Guest code gets its own `JNIEnv` and `JavaVM` (`guest_jni.cpp`): a function
table with System V entry points, generated from the JDK's `jni.h`
(235 slots, `tools/gen_jni_slots.py`), each forwarding into HotSpot's table.
There is one guest env per thread, and `GetEnv`, `AttachCurrentThread` and
`DetachCurrentThread` hand back guest envs.

### The variadic JNI bridge

`Call<T>Method(env, obj, methodID, ...)` and its `...V` and `NewObject`
relatives (31 families) cannot be forwarded blindly: the arguments' types are
only known from the method's signature. `jni_variadic.cpp` asks JVMTI for the
signature (cached), reads the argument stream against it with C promotion
rules, packs the values into a `jvalue[]`, and calls HotSpot's `...A` form.

### Loading an app's libraries

Upstream's class loader funnels every `System.loadLibrary` through
`AppClassLoader.findLibrary`, which hands each `lib<name>.so` to a translator
and `System.load`s the result. On Windows (`host/guest_libs.cpp`,
`host/translator_main.cpp`, `host/guest_stub.cpp`):

1. `macapk-translator.exe` writes a copy of one small stub DLL per library,
   plus a sidecar file naming the `.so`. Each copy is a distinct module, so
   HotSpot keeps one `NativeLibrary` per app library, as a device does.
2. `System.load` calls the stub's `JNI_OnLoad`, which has the runtime load the
   ELF through the guest linker and run the guest's own `JNI_OnLoad` with the
   guest `JavaVM`.
3. Natives bound by name: HotSpot's `NativeLibraries.findEntry0` is replaced
   so that, for a stub module, the lookup is `dlsym` on the guest library.
4. A JVMTI `NativeMethodBind` hook sees every binding, and whenever the
   address is guest code it puts a thunk built from the method's signature in
   its place. The guest's `RegisterNatives` slot wraps function pointers the
   same way.

### ARM64 code: translation, JIT, interpreter

Three engines run arm64 code, behind one CPU state (`src/arm64/`):

- **Ahead-of-time translation** (`arm64_aot.cpp`, `tools/arm64_aot_main.cpp`,
  `winapk-aot.exe`). Each guest function becomes a C function with the guest
  registers as local variables, so the C compiler allocates registers and
  optimizes across the whole function. Functions are found from the symbol
  tables, the function starts in `.eh_frame_hdr` (which survive stripping)
  and, transitively, the targets of BL; a function of more than 4,000
  instructions is left to the JIT. The runtime compiles that C with
  clang (`-O2 -mllvm -fast-isel -march=native`) into `<lib>.so.aot.dll` next to the library,
  in parts of 20,000 instructions. A background worker runs them at idle
  priority and as EcoQoS, in a job object that ends its compilers with it
  and caps their memory together (half the RAM, at least 2 GB), as many at once as the PC allows: up to half the logical processors while
  the user is away, one while the user is active, none on battery saver or
  under a full-screen program, and never more than free memory allows
  (512 MB each, 20% of the RAM and at least 2 GB kept free). A library of up
  to 1,000,000 instructions is translated whole (the parts are of a fixed
  size, so that costs time, not memory); a larger one only where the JIT's
  profile (`<lib>.so.aot.profile`) says code ran, and again once the profile
  has grown by a quarter (and 200 lines), at most once an hour (the stamp's
  retry field). The running app starts the worker when its profile has
  settled or when it exits (`Cocoa_exitProcess`), and a worker started with
  the app waits a minute. Settings > Apps > "Optimize ARM apps in the
  background" (`AotBackground`) turns the worker off: none starts, and a
  running one stops before its next compile, keeping its pass. The app's
  details page shows a running worker (its mutex, and the parts in
  `.aot-tmp`), and says when a library over the threshold leaves an
  arm64-only app on a profile, where an x86_64 build would need nothing.
  A pass works in `.aot-tmp\<lib>\` from a copy of the profile and keeps
  each compiled part, so a stopped worker goes on where it was. A failed one
  waits 10 minutes, 4 times longer after each failure (at most a day), and
  stops after 4. A translation finished while the app runs is loaded at once.
  Whole translations are shared between apps through `<data>\aot-cache`,
  keyed by the library's code (its bytes and addresses) and the CPU's model
  and features, since the code is compiled for that CPU; the stamp next to
  each DLL records the CPU too, and a translation made on another CPU is not
  loaded. An app's DLL and the cache's are one file (a hard link), and the
  cache drops what no app used for 60 days. Every app's worker writes the
  cache while apps load from it, and any of them can be killed half way, so
  no file is written in place (`include/winapk/aot_files.h`): each is written
  under a name unique to its writer, flushed, then renamed; a finished DLL
  ends with a 32-byte seal (its length and hash) that the loader, the worker
  and the cache check, so a DLL cut short is never loaded, shared or taken
  for done; a good cache entry is never replaced (an app may have it mapped),
  a broken one is deleted, and temporary files a killed writer left go an
  hour later. The stamp ends with the library's id (its size and date), so
  whether a translation is current does not depend on the clock. The
  translation never has to be
  complete: a function returns to the JIT when it reaches a branch the
  translator could not follow, when a call returns somewhere else than its return address
  (an exception's landing pad, a jump into another frame), and at its entry
  when the host stack is nearly used up (a translated guest call is a host
  call, so deep guest recursion would overflow it; `Cpu::stack_floor`, an
  eighth of the stack and at least 256 KB before its end). A library that
  rewrites its own code (an inline hook, a packer decrypting its text: IC
  IVAU in its code) is handed to the JIT for good, its stamp saying
  `written`; one unloaded with `dlclose` takes its translation with it. A
  call through a register to a function of the same library (a C++ virtual
  call) finds it in the DLL's own table, cached at the call site.
  A fault inside a translated DLL marks that library's stamp `bad`, and the
  JIT runs it from the next start. A mistranslation that corrupts state
  and crashes elsewhere (in the JIT, the guest's own code, a host function,
  the JVM) is caught a level up, by the crash guard (`host/aot_guard.cpp`):
  when the app's process ends through the crash path (`crash_report.cpp`:
  a fault nobody handled, `abort()`, HotSpot's fatal error handler through
  the JVM's `abort` hook; not a normal exit, a closed window or a kill)
  twice in a row with a translation loaded, the next start disables
  translation for the app (`aot_disable`, as `WINAPK_AOT=0` does) and says
  so in its run log. A run without a crash sets the count back; two crashes
  in a row on the JIT mean it was not the translation, and the next start
  turns it on again for that build of the app. The state is
  `apps\<name>\aot-guard.txt`, written atomically (`aot_write_file`), and a
  new `kAotVersion` or another build of the APK or its libraries starts it
  over.
- **The JIT** (`arm64_jit.cpp`, dynarmic from the azahar-emu fork, 0BSD
  license, fastmem over the shared address space). The app starts on it and
  it runs whatever has no translation. Each thread has its own engine and
  code cache; code the guest writes (IC IVAU, on any thread and engine) is
  dropped from all of them. Built when CMake is given
  `-DWINAPK_ARM64_JIT=ON` (CI and releases).
- **The interpreter** (`arm64_cpu.cpp`, `arm64_simd.cpp`): the base integer
  ISA, every load/store form, exclusives and LSE atomics, scalar FP and the
  Advanced SIMD forms compilers emit. It runs the few instructions the other
  two do not handle and is the reference the others are checked against:
  `winapk-arm64-test` runs C compiled for arm64 and for x64, control-flow and
  edge cases with results worked out by hand (`arm64_flow_guest.c`), and 400
  functions of random instructions (`gen_arm64_fuzz.py`) on all three and
  compares them.

The bridge (`arm64_guest.cpp`) gives each thread its CPU state, a guest stack
and Bionic's TLS slots; converts AAPCS64 arguments and `va_list`s to System V
for calls into the shim; and turns a host call into guest code (a callback)
into interpretation through an execute-fault handler, since arm64 text is
never mapped executable.

Speed, on the project's own microbenchmark (`winapk-arm64-test` with
`test/guest/arm64_bench.c`, ratios to native x64 code): translated integer
code about 1.0x, floating point and NEON about 1.3x, calls into the shim
about 2x; the JIT roughly 3 to 6x; the interpreter about 80x. Real apps vary.

Guest code enters the system in exactly three places, so an engine can be
swapped without touching the rest: the loader (`ElfImage`), the linker's
image registry (`GuestLinker`) and native-method binding (`jni_bind`).

### Graphics

OpenGL ES runs on ANGLE over Direct3D 11, falling back to WARP (Direct3D's
software rasterizer) and then SwiftShader when there is no GPU. ANGLE is not
built here: `windows/scripts/fetch_angle.sh` takes Chromium's own
`libEGL.dll`, `libGLESv2.dll` and SwiftShader from a pinned Electron release,
checked against its SHA-256.

- **From Java** (`GLSurfaceView`, libGDX, `GLES10`...`GLES32`, `EGL14`):
  upstream's generated GL bindings are compiled unchanged
  (`host/gl/gl_bridges.cpp`) and their `gl*` and `egl*` calls land in
  forwarders (`gl_stubs.inc`, generated from the Khronos headers) that look
  ANGLE up on first use, so the runtime starts without it.
- **From native code** (`host/gl/guest_gl.cpp`): every GLES 1.1 to 3.2 and
  EGL 1.5 function wrapped for the System V convention, plus the EGL calls
  that need a window. An app's `ANativeWindow` (`host/native_window.cpp`) is
  a size and a format; a window surface on it is a pbuffer that grows with
  the window, and `eglSwapBuffers` reads it back and presents it.

Each GL view renders into its own pbuffer; after a frame the pixels are read
back and composited with the Java view tree, which is drawn in software.
Vulkan is not implemented.

### The window and the display

Each app has one Win32 window on its own UI thread (`win32_window.cpp`). The
window is the app's display: its client area in pixels is the app's surface,
the density comes from the monitor's scale, and the Configuration and
DisplayMetrics follow. Resizing, maximizing or snapping gives the app a
display of the new size and it lays itself out again; a fixed-orientation app
gets the largest box of its aspect, centred, with black bars. Frames are shown
pixel for pixel; a frame from before a resize is never stretched. The size
the user settles on is written to `library\<name>.window` for the next start.

Mouse events become `MotionEvent`s, the wheel a short drag, keys `KeyEvent`s
(Escape is BACK). A steady 60 Hz tick on a waitable timer drives the app's
renderer. `WINAPK_HEADLESS=1` runs the same path without a window, which is
how CI boots apps.

### Audio, media, camera, location and the other devices

| Android API | Windows implementation | Source |
|---|---|---|
| AudioTrack, SoundPool, MediaPlayer (audio) | A WASAPI mixer; WAV and Ogg Vorbis decoded in-process, MP3, AAC and whatever Windows has through Media Foundation | `native_audio.cpp`, `audio_engine.cpp`, `audio_decode.cpp` |
| AAudio (native) | A port of upstream's AAudio on the same mixer; write and callback modes, no capture | `native_aaudio.cpp` |
| OpenSL ES (native) | Not implemented: `slCreateEngine` reports the feature unsupported, so engines fall back to silence. The interface IDs are real | `native_opensles.cpp` |
| Microphone (AudioRecord) | WASAPI capture, resampled to the app's format | `native_audio_input.cpp` |
| Video: MediaPlayer, VideoView | `HostVideo.Player` over `video_player.cpp`: an `IMFSourceReader` demuxes and decodes (NV12, or P010 for 10-bit; own YUV-to-ARGB conversion, aperture crop, container rotation), the audio goes into the audio engine, and a Java thread pulls each picture when the audio clock says it is due and presents it on the SurfaceView's layer, the TextureView's SurfaceTexture, or the window | `video_player.cpp`, `video_decode.cpp`, `native_media_video.cpp` |
| Video: MediaCodec | `NativeMedia`'s video natives on the synchronous decoder MFTs: H.264 always, HEVC/VP9/VP8/AV1/MPEG-4 when a decoder is installed (`nativeVideoSupportedMimes` lists what `MFTEnumEx` really creates); AAC and MP3 audio decoders too. The NDK media API (`AMediaCodec`, `AMediaExtractor`) and `MediaExtractor` are not implemented; `native_media.cpp` holds only the `AMEDIAFORMAT_KEY_*` strings so libraries link | `native_media_video.cpp`, `video_decode.cpp` |
| Images | libwebp for WebP, WIC for the rest | `native_image.cpp` |
| Notifications | `NotificationManager` posts to `HostNotifications`, whose natives show Windows.UI.Notifications toasts (WinRT C ABI, combase loaded at run time) under the app's AppUserModelID, which the launcher writes on the app's Start menu shortcut; a click comes back through `HostNotifications.onActivated` and `PendingIntent.send()` | `native_notifications.cpp` |
| WebView, Custom Tabs | Microsoft Edge WebView2, shown in the app window behind the views drawn over it; `shouldInterceptRequest`, JavaScript interfaces, cookies; a Custom Tab is a tab window whose navigations are checked against the app's manifest | `native_webview.cpp`, `webview2_host.cpp`, `native_browser_tab.cpp` |
| Camera (camera2, Camera, CameraX) | Media Foundation capture, each webcam offered as a back and a front camera; a virtual camera when there is none | `native_camera.cpp` |
| Location | `Windows.Devices.Geolocation`; Windows' privacy settings are the authorization | `native_location.cpp` |
| Wi-Fi | `HostRadios` over the Native Wifi API: state, the current connection (SSID, BSSID, RSSI, frequency, link speed), the DHCP configuration from `GetAdaptersAddresses`, scans, and `WlanRegisterNotification` events pumped by one Java thread into the state, connection, signal and scan-results broadcasts. Network names need Windows' location access, as Android needs the location permission | `native_radios.cpp` |
| Bluetooth | `HostBluetooth`: the paired devices (bluetoothapis), RFCOMM sockets both ways on Winsock `AF_BTH` (the SDP query for the channel, `WSASetService` for a server's record), LE scans on the `BluetoothLEAdvertisementWatcher` and the GATT client (services, reads, writes, notifications through the CCCD, MTU) on the WinRT `Windows.Devices.Bluetooth` APIs through their C ABI, declared in the file. Permissions as AOSP's service: `BLUETOOTH_CONNECT` / `BLUETOOTH_SCAN`, location for scan results unless `neverForLocation`. Not there: classic discovery and pairing, LE advertising and the GATT server, profiles, L2CAP | `native_bluetooth.cpp`, `bluetooth_util.h` |
| Clipboard | Text, HTML, URLs, with the concealed formats for sensitive clips | `native_clipboard.cpp` |
| File and folder pickers | `IFileOpenDialog` / `IFileSaveDialog` | `native_picker.cpp` |
| Game controllers | XInput, polled | `native_gamepad.cpp` |
| Photos, wallpaper | The Pictures and Videos folders with shell thumbnails; the desktop wallpaper | `native_photos.cpp`, `native_wallpaper.cpp` |
| ICU | Windows' `icu.dll` | `native_icu.cpp` |
| SQLite | Upstream's `NativeSqlite` compiled unchanged over the SQLite amalgamation | `runtime/src/native_sqlite.cpp`, `compat/CoreFoundation` |
| `NativeActivity`, the NDK input queue, `AConfiguration` | A port of upstream's, over the guest linker and the shim's `ALooper` | `native_activity.cpp` |
| Shared storage | The volume's Download, Pictures, Documents, Music and Movies are junctions to the user's Windows folders | `user_folders.cpp` |

There is no sensor hardware behind `SensorManager`: the framework (upstream's Java) synthesizes a still, flat phone, so the accelerometer reports gravity, the gyroscope zero, and tilt-control apps keep their input instead of finding no sensors.

### The launcher

`Win.apk.exe` (`windows/launcher/launcher_main.cpp`, one file, GDI+) is the
app library, the counterpart of upstream's Swift launcher. It installs APKs
by copying them into `library\` and running `winapk-runtime --install-only`,
gives each app its shortcuts, runs apps (`winapk-runtime --apk`), shows
their logs, and holds the Store (F-Droid's `index-v1.json` summarized to
`store\catalog.tsv`, downloads checked against the index's SHA-256), the
settings and each app's details. Every user-visible string is in English and
Hebrew, and in Hebrew the windows are laid out right to left.

One `Win.apk.exe` process serves the library, the install window and the
launch splashes. A second start (Explorer opens one per selected file) hands
its files, or its wish to see the library, to the running one in a
`WM_COPYDATA` and exits, so files opened together share one install window.
The library window is created hidden and shown only when asked for: an APK
opened from Explorer shows only the install window, a shortcut only the
splash. A process whose library is hidden exits once it has nothing left to
show or wait for.

### The install helper and the installed-apps registry

An app that installs other apps (F-Droid, through `PackageInstaller`) hands
the APK to upstream's `PackageInstallerBackend`, which runs an install
helper. On Windows the helper is the launcher itself:
`Win.apk.exe install --apk <file> --source android [--requestor-label <app>] --json`
and `uninstall --package <name> ...`. It asks the user, installs into the
library as above, and keeps `installed_apps.json`, the registry the package
manager answers from, so a store sees what it installed. The output follows
upstream's contract: `[helper] progress <fraction> <stage>` lines, then one
JSON object; exit codes 0 done, 2 bad arguments, 3 declined, 5 failed, 6 not
an app.

### Permissions (app-ops)

Android's permission model is reproduced rather than bypassed. The decision
model is upstream's `SpecialAccessStore.GroupModel`, with the group table in
`runtime/java/com/macapk/loader/RuntimePermissionGroups.java`: grant, deny,
deny twice (fixed), silent grants within a group, background permissions,
legacy targets, App info rows. When an app asks, the runtime runs the launcher
(`Win.apk.exe runtime-permission`, `special-access` or `app-details`, the
verbs of upstream's macapk-installhelper), which shows the dialog and signs
the answer into `appops\app-ops.json` with an ECDSA P-256 key
(`signing-key.p256` under DPAPI, `signing-key.pub` beside it). The runtime
verifies the signature and never writes the store. The requesting app is
identified from the parent process, never from arguments. `AppOpsManager`,
`checkPermission`, `canDrawOverlays`, `areNotificationsEnabled`, the camera
and microphone classes all answer from the store.

### Google Play services

Upstream provides in-process stand-ins for Play services
(`runtime/java/com/google/android/gms/`, `runtime/java/com/macapk/gms/`):
`GoogleApiAvailability` reports them present, and the client APIs (tasks,
auth, location, ads, games, maps...) are backed by `com.macapk.gms`. On
Windows the only host piece is the sign-in window (`native_signin.cpp`):
Google's EmbeddedSetup page in a WebView2 window with a private profile and
the Android WebView user agent that makes Google serve the device flow; the
`oauth_token` cookie the finished flow sets is exchanged for a master token.
Before the window closes, the page asks `accounts.google.com/ListAccounts`
(the session's accounts, as Chrome asks) for the account's picture URL;
`AccountPhoto` saves the picture as `gms\photo-<account>.img` for the
launcher's account button. The layer's state is `gms\gms_state.properties`. `winapk-runtime --gms
<command>` runs upstream's `GmsCli` with no app, which is how the launcher's
Google account setting lists accounts, adds one and signs out.

Not Play-certified: no Google signing certificate, no installed
`com.google.android.gms` package, no Play Integrity or SafetyNet, Play
Billing answers `BILLING_UNAVAILABLE`, the Maps SDK lays out a blank map.

### Google services: cross-process Binder and the system broker

With Settings > Google account > "Use Google services (experimental)" on
(`HKCU\Software\Win.apk` `GoogleServices`, which `runtime_main` turns into
`MACAPK_BROKER=1`), the real Play services, Services Framework and Play Store
the user installed run as apps, as upstream runs them on the Mac:

- **Binder across processes** (`windows/host/binder_win.cpp`, the natives of
  upstream's `android.os.MacApkBinderTransport`). Each process serves a named
  pipe, `\\.\pipe\winapk-<user SID>-s<session>-b-<pid>-<random>`, one thread
  per connection. A binder travels as (endpoint, number) and comes back as the
  same proxy, or as the same local object in its own process; oneway calls go
  over one connection per peer, in order; a file descriptor is the open file's
  handle duplicated into the receiver, where it becomes a new CRT fd;
  `linkToDeath` waits on the peer's process handle.
- **The system broker** (`winapk-runtime --system`, upstream's
  `com.macapk.loader.SystemServer`): binds, starts, providers, broadcasts and
  jobs between apps, the accounts service, per-package uids (`AppUids`). The
  first app that finds no broker starts it (`WindowsBroker`); one per session
  (`Local\WinApk-system`, then the pipe name); it exits when no app has been
  open for a minute. It starts an app for another's request as the launcher
  would, `winapk-runtime --apk <library file>`, headless; a second
  `android:process` of a package gets its own single-instance key. When you
  open an app the broker is running headless that way (Play services binds
  the Play Store's services, for one), the new start stops the headless
  process and takes its place with a window; its clients bind again, as
  after Android stops a background process.
- **Who calls.** The calling pid is the OS's (`GetNamedPipeClientProcessId`);
  no uid travels. The broker knows a process as the package it started it as,
  or the package whose installed APK the process's command line (`--apk`)
  names (`WinProcesses`, from the process's recorded command line); a caller
  argument that names another package, or a registration as another package
  or process, is refused with a `SecurityException` and logged; the calling
  uid any app sees is the broker's answer for that pid. A Message sent
  through a Messenger is stamped with that uid (`sendingUid`), and a
  PendingIntent carries its creator's package and uid, which is how Google
  Play services' GCM registrar tells which app asks for an FCM token. An app's application
  thread takes calls from the broker's pid only. Pipes have a DACL for the
  user and SYSTEM, reject remote clients, claim their name with
  `FILE_FLAG_FIRST_PIPE_INSTANCE`, connect with `SECURITY_IDENTIFICATION`, and
  each end checks that the other is the same user's process of the same
  executable. `android:exported` is enforced as upstream does.
- **Google's packages.** `com.google.android.gms`, `.gsf` and
  `com.android.vending` get the system-app treatment (`FLAG_SYSTEM`, started
  at the broker's boot) only when their APKs' signature verifies and Google's
  certificate made it, as the signer or as an ancestor in the verified v3
  rotation lineage (`GoogleGmsCertificate.isGoogleSigner`). Win.apk ships and
  downloads none of them.
- **Signatures.** Every certificate PackageManager reports (an app's own,
  another installed app's) and every trust decision comes from
  `ApkVerification`, which runs AOSP's verifier, apksig
  (`thirdparty/apksig/`), over the APK: the v3.1 / v3 / v2 signature over the
  signed data and the content digests over the whole ZIP, the v3
  proof-of-rotation chain, or the v1 JAR signature of an old APK. An installed
  app is base.apk and every split, a bundle every APK in it; all must verify
  with the base's signer. The signer is chosen for API 36 in every process.
  With a rotation, `GET_SIGNATURES` answers the oldest certificate and
  `SigningInfo` the lineage; `checkSignatures` and `hasSigningCertificate`
  follow AOSP's lineage rules. An APK that does not verify reports no
  certificate. Verdicts are cached by path, size and time in
  `cache\apk-verify\`; the install fills it.
- **The limit.** Every app runs with the user's Windows rights and there is no
  OS sandbox between them: native code in one app's process can read or write
  another process of the same user. The checks above stop an app's Java code
  from passing itself off as another app; they are not a sandbox.

`windows/test/binder_test.cpp` with `test/java/com/macapk/loader/BinderTransportTest.java`
tests the transport and the broker's checks across three processes.

## An install, step by step

1. The launcher names the app (`install_name`: the package when the file name
   starts with one, as `com.waze_5.26.5.0-..._apkmirror.com.apkm` does, else
   the file name up to its version; a package already in the library keeps
   that app's name, so the install updates it), copies the file into
   `library\<name>.apk` (an update moves the
   previous APK aside, and puts it back if the install fails or is cancelled)
   and starts `winapk-runtime --apk <that file> --install-only`, logging to
   `logs\install-<name>.log`. The install window reads that log for its
   status line.
2. `com.winapk.tools.AppInstall` (in a child JVM) unpacks the app into the
   device layout under `android\data\app\~~<nonce>\<package>-<nonce>\`:
   `base.apk` (a hard link for a plain APK), `split_<name>.apk` chosen as
   upstream chooses them, and the native libraries of the first ABI in
   `x86_64,arm64-v8a` the app has, under `lib\<isa>\`. A record file notes
   the result so the step runs once per input file.
3. Upstream's DEX converter (`com.macapk.dex.Main`) turns the app's DEX into
   `apps\<name>\classes.jar`, with a stamp naming the converter and the APK so
   a change to either converts again.
4. If the app has only arm64 libraries, a background `winapk-runtime
   --aot-translate` starts for each: `winapk-aot` writes C, clang compiles it
   into `<lib>.so.aot.dll`. Nothing waits for it.
5. `com.winapk.tools.AppMeta` renders the launcher icon to
   `apps\<name>\icon.png`, and writes the label and `package.txt` (package,
   versionCode, versionName, the first activity's manifest
   `screenOrientation`). The launcher moves them beside the APK as
   `library\<name>.label` and `.package`.
6. The launcher makes the Start menu shortcut (and the desktop one, if the
   setting is on) and adds the app to the library.

## A run, step by step

1. The launcher (or a shortcut: `Win.apk.exe --run <library apk>`) shows the
   app's splash at once, sets `WINAPK_PHONE_SIZE` from the Screen size
   setting and the app's effective orientation, and starts
   `winapk-runtime --apk <library apk>` with its output in
   `logs\run-<name>.log` (the previous run's kept as `run-<name>.1.log`;
   `last-run.log` is a hard link to the newest). It passes
   `WINAPK_READY_EVENT`, an inherited event handle, and waits for the event
   or the process: the runtime sets the event once its window has painted
   the app's first frame (`win32_window.cpp`, `signal_launcher_ready`), and
   the splash goes. If the process ends first, the splash says the app did
   not start, with Show log and Report a problem (`report_problem`: GitHub's
   issue form prefilled through its fields' ids in the URL, and Explorer on
   the run log and the newest crash dump; nothing is uploaded).
2. The runtime takes a named mutex keyed on the app's data folder. If another
   copy holds it, the running window is brought forward, the ready event is
   set, and this process exits.
3. The install and DEX conversion are checked (cached, so this is quick), and
   shared storage is mapped to the user's folders if the setting is on.
4. HotSpot is created in-process on a thread with a large stack, with the
   C1 compiler only (a faster start than tiered C1+C2) and class-data sharing:
   the JRE's archive and the framework archive (see [The host](#the-host); the
   run log's `[win.apk] cds:` line says which, and `startup:` lines when the
   JVM was up and when the first frame came, with the working set). The first
   start without a framework archive records its classes, and 10 s after its
   first frame (or at its exit) starts the archive's dump in the background;
   JVMTI is enabled for the `NativeMethodBind` hook and signature lookups; the
   host natives are bound; SQLite's natives are registered.
5. `Boot.run` starts the Android side: the TLS trust store is set to the
   JDK's and Windows' roots, kept between starts in `%TEMP%\winapk-trust.jks`
   and rebuilt in the background once a day; the package manager reads the
   installed app, the framework resources load, `Application.onCreate`
   runs, the first activity is created and resumed, and the window opens with
   the app's icon.
6. When the app loads a native library, the path in
   [Loading an app's libraries](#loading-an-apps-libraries) runs; an arm64
   library picks up its translation DLL if one exists and the JIT otherwise
   (always the JIT when the crash guard has turned the app's translation off,
   which the run log's first `arm64 AOT:` line says).
7. The render tick drives frames; input goes to `TouchDispatcher`; permission
   requests spawn the launcher's dialogs; a resize re-lays the app out.
8. Closing the window ends the activity stack and the process. The exit code
   is 0 unless the app or the VM failed; a VM crash leaves
   `logs\hs_err_pid<n>.log`.

## Data on disk

Everything is under `%LOCALAPPDATA%\Win.apk\` (the launcher's and runtime's
default; `--data` moves it for a direct run):

| Path | What |
|---|---|
| `library\<name>.apk`, `.label`, `.package`, `.orientation`, `.window` | The user's copy of each app and the launcher's per-app files |
| `apps\<name>\` | `classes.jar`, `.macapk-dex-stamp`, `icon.png`, `label.txt`, `package.txt`; for an arm64-only app that crashed with its translation loaded, `aot-guard.txt` (the crash guard's count and whether translation is off) |
| `android\` | The Android file system: `data\app\` (installed packages, with `lib\<isa>\` and the `.so.aot.*` translation files; `lib\arm64\.aot-tmp\<lib>\` holds an unfinished translation), `data\data\<package>\` (app data), the shared-storage volume |
| `logs\` | `run-<name>.log` and `run-<name>.1.log`, `last-run.log` (a hard link to the newest run log), `install-<name>.log`, `gms.log`, `install-helper.log`, `package-backfill.log`, `hs_err_pid*.log` |
| `store\` | `catalog.tsv`, `catalog.etag`, `icons\`, `downloads\` |
| `installed_apps.json` | The installed-apps registry the package manager answers from |
| `appops\` | `app-ops.json`, `signing-key.pub`, `signing-key.p256` |
| `gms\` | `gms_state.properties`, the `accounts.present` marker |
| `app_uids.properties`, `system_ce\0\`, `installer-of-record.properties`, `run\system-ready` | With Google services on: per-package uids, the accounts service's store, which app installed which, the broker's readiness |
| `logs\system.log`, `logs\services\` | The system broker and the apps it starts as service hosts |
| `aot-cache\` | Whole arm64 translations shared between apps, `<key>.aot.dll` (kept under 2 GB, least recently used first); a `*.tmp` is a write in progress, or one a killed worker left (deleted when an hour old) |
| `cds\` | The framework archive (`host/cds.h`): `framework-<key>.jsa`, `.ok` once it has mapped whole, `.fails` (failures in a row; at two it is not made again for that key), `dump.log` (the last dump), and a `.classlist` while a first start records |
| `cache\` | The runtime's shared caches: `android-system-fonts\` (the `/system/fonts` and `fonts.xml` native text stacks such as Flutter's read), `fonts\`, `fwview\` (the framework's resolved resources), `monet\`, `apk-verify\` (APK signature verdicts) |
| `run\<package>.pid` | Each running app's run lock |
| `staging\` | The install helper's staging area |
| `updates\` | `releases.json` and `releases.etag` (GitHub's list of releases from the last update check), `Win.apk-Setup-<version>.exe` (a downloaded update, until it is installed) |

Settings live in `HKCU\Software\Win.apk`: `Theme`, `Language`, `PhoneSize`,
`Landscape` (absent means landscape; 0 means portrait), `DesktopShortcut`,
`UserFolders`, `AotBackground` (absent means on: the background arm64
translation), `InstallDetails` (the install window shows its log),
`ShortcutVersion`, `AutoUpdate` (absent means on), `GoogleServices` (absent
means off: the system broker and the real Google apps) and `LastUpdateCheck` (Unix
seconds of the last update check GitHub answered), and the library window's `WindowWidth`, `WindowHeight` and
`WindowMaximized`.

## Build, tests and CI

Win.apk builds with clang only: MSVC cannot emit System V calls, and MinGW
GCC emits broken SEH unwind data for such functions. On Windows the build is
CMake + Ninja in an MSYS2 CLANG64 shell; on Linux the code cross-compiles with
clang, lld and the mingw-w64 headers and the tests run under Wine. The Java
side is built with a stock JDK 21 (`windows/scripts/build_java.sh`);
`stage_runtime.sh` assembles a runnable folder with ANGLE, WebView2's loader
and the bundled clang; the installer is Inno Setup (`windows/package/winapk.iss`).

The unit tests, each a program under `windows/test/`, cover the loader
(`winapk-elfrun`), the thunks, the guest JNI and its variadic bridge, the
linker, the Bionic shim (a guest library exercising it), the arm64 engines
(the same C built for the host and for arm64 must agree, on all three
engines), the audio backend, the shared-folder mapping, the window geometry,
and a real HotSpot JVM loading a guest library (`winapk-jvm-run`). Two Java
tests check the permission model and the `File.separator` rewrite.

Two GitHub Actions workflows:

| Workflow | When | What |
|---|---|---|
| `ci.yml` | Pull requests touching `windows/` or `runtime/`, and by hand; a push to `main` only builds (to leave its compile cache to pull requests) | One pipeline of jobs. *What changed* decides which stages a pull request needs. The guest test libraries build on Ubuntu, and a launcher-only change gets a one-minute MinGW compile with warnings as errors. Everything else builds once on `windows-latest` (MSYS2 CLANG64, ccache) and stages the runtime. That output goes as artifacts to the jobs that run side by side: *Unit tests* (every test program), *Runtime tests* (the Java tests, GL, an APK booted headless and in a window, the GMS CLI), *App smoke test* (the ten apps of `windows/test/compat/smoke.txt`, one of each kind; it fails when one no longer draws), and *Package* (a preview folder with the JRE the build baked and the installer). The smoke test and the sweep run on that JRE, with one framework archive made first (`--cds-prepare`) and shared by the apps, and the smoke test records the apps' classes into the `framework-classlist` artifact, the source of `windows/java/winapk-framework.classlist`. The whole compatibility sweep runs only by hand (Run workflow, "Full compatibility sweep"): split over four machines (`sweep.py --shard`) and joined by *Sweep report* into one ranked report. A new push cancels the run in progress |
| `windows-release.yml` | By hand only, with a tag | Builds (stamping the tag's version into `Win.apk.exe`), packages with the JRE `bake_jre.sh` makes from Temurin 21, smoke-tests the package, builds the installer and publishes a GitHub pre-release |

[`windows/README.md`](../windows/README.md) has the commands.

The launcher's update check (Settings > Updates) reads that list of releases
from GitHub's API, takes the highest version that is not a draft (they are
all pre-releases, so `/releases/latest` would find none), and compares it with
the version in its own resource; **Update** downloads the release's
`Win.apk-Setup.exe`, checks its size (and SHA-256 when GitHub lists one), runs
it and quits so that the installer (`CloseApplications=yes`) can replace the
files. The version comes from `WINAPK_VERSION`, which the release workflow
sets from its tag; any other build is 0.1.0.

## What is not there

As of this writing: the NDK media API, OpenSL ES, Vulkan, real sensor input,
signal delivery for faults in arm64 guest code and handlers in arm64
libraries, POSIX timers, static ELF TLS, W^X thunk pools with unwind tables,
code signing, 32-bit ARM, notification action buttons,
and the Play Store features listed above. `windows/PORTING.md`, section 7,
has the finer gaps and section 8 the plan.

## Terms

- **Guest:** code loaded from an APK (x86_64 or arm64 `.so` files).
- **Host:** the Windows process, the JVM and the runtime around them.
- **Shim:** host functions that implement Android's libc (Bionic) for guest
  code.
- **Thunk:** generated code that converts a call from one calling convention
  to the other.
- **Slot:** one entry in the JNI function table.
- **A-form:** the JNI functions that take arguments as a `jvalue` array.
- **AOT:** ahead-of-time translation of arm64 code to C, compiled with clang.
