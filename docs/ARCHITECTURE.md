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
loader (`com.macapk.loader`: `Boot`, `ActivityHost` (the host's side of an
app's starts and its window), the package manager, the class loader). It compiles unchanged with a stock JDK 21 and runs on Windows
with a handful of hooks, each listed in `windows/README.md`.

Three of its layers are AOSP 14's own classes instead, each in a jar put
before `android-framework.jar` on the class path, so that its classes win over
Win.apk's of the same names: the graphics (`lib\aosp-graphics.jar`, below),
the resources (`lib\aosp-resources.jar`: `android.content.res` on Robolectric's
Java port of libandroidfw, with android-14's own platform table,
`bin\aosp\framework-res-34.apk`, for every `@android:` reference) and the views
(`lib\aosp-views.jar`: `android.view`, `android.widget`, `PhoneWindow`,
`ViewRootImpl`, the dialogs, over an in-process window manager,
`windows/java/aosp_views`). `windows/scripts/build_aosp_gfx.sh`,
`build_aosp_res.sh` and `build_aosp_views.sh` make them from android-all 14;
`docs/research/aosp-views.md` has how the last two were built. Since November
2026 the resources and views are the only ones: Win.apk's own classes of those
packages are dropped from the shipped `android-framework.jar` (their sources
stay, as the rest of the framework compiles against them), and the rest of the
framework reaches what differs through two seams, `ResBackend` and `ViewHost`.

The framework's own natives are declared in about 40 Java classes
(`NativeCocoa`, `NativeAudio`, `NativeSqlite`, `MacApkWebViewBridge`,
`LocationBridge`...). The host binds each of them: the Windows implementation
where there is one, otherwise a stub that returns zero and logs its name the
first time it is called (`windows/host/host_natives.cpp`). Those log lines
(`native not implemented on Windows yet`) are how missing pieces are found.
Every call to such a stub, and to the stubs that stand in for system functions
a guest library imports (`windows/host/guest_libs.cpp`), is also counted
(`windows/host/stub_hits.h`); when the process exits the log ends with the ones
this run reached and how often, the busiest first, so a report says which
missing piece an app kept asking for and not only which it touched once.

Windows-only Java lives in `windows/java/`: `com.winapk.tools.AppInstall`
(installing an APK or bundle in the device layout) and `AppMeta` (the icon
and name the launcher shows), plus an overlay for `java.base`.

### The host

`windows/host/` is `winapk-runtime.exe`, the Windows counterpart of
upstream Mac.apk's `runtime/src/main.cpp`. It finds the runtime files, installs and
converts the app, creates the HotSpot JVM in-process with upstream's options
(SerialGC, a 16 MB thread stack, upstream's `java.base` changes, bytecode
verification off) and class-data sharing (below), opens every package of the Java
runtime's modules to the apps' reflection, as ART has no modules (`open_platform_packages`:
Gson's `setAccessible` on `java.lang.ref.Reference.referent` works as on a phone), binds the
host natives, and calls `Boot.run`. One file per
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
the JDK's classes) and the framework archive (`<data>\cds\framework-<graphics>-<key>.jsa`:
the JDK's and the framework's classes, `android-framework.jar`,
`macapk-loader.jar` and an unsigned bcprov, and with AOSP's graphics
`aosp-graphics.jar`, `aosp-resources.jar` and `aosp-views.jar`, one for all
apps (the `aospres`, `aospviews` and `java2d` ones of older builds are
deleted by the next dump); an install,
`--gms` and the system broker map the one an app's start does). The runtime
drops the `--patch-module` flags only when the JRE's
`lib\winapk-baked-patches` records the same SHA-256 as the patch folders (a
plain JDK, or a patch rebuilt since, keeps them and runs without CDS). The
framework archive is dumped by `java -Xshare:dump` before any app starts, as
Android compiles its framework at an update: by Win.apk's installer
(`winapk-runtime --cds-prepare`, hidden, at the end of the setup), else beside
the first app install (`--install-only` starts `winapk-runtime --cds-dump` in the
background at below-normal priority and does not wait for it; an app start that
finds no archive while that dump runs waits for it, 30 s at most, and maps it), from
the class list Win.apk ships (`lib\winapk-framework.classlist`, the classes at
least three of the ten smoke apps load). Only when neither made it does an app
start record the classes it loads and dump in the background after its first
frame (`winapk-runtime --cds-dump`): a second JVM of about 200 MB beside the app
(the dump's heap is its own, `-Xmx96m`), which made a first start's peak twice a
second's (measured on windows-latest) while it was the rule. The key changes with the
graphics, the JRE, the jars, the shipped list and the JVM options; HotSpot itself checks the
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
`libGLESv2.so`...) from the shim, looks symbols up in Bionic's order (the
`RTLD_GLOBAL` libraries, then the library's tree breadth-first with the shim
at the place of its first system library, so an allocator an app lists
before `libc.so` is the one its libraries call), keeps the registry of
loaded images, and
implements the guest's `dlopen`, `android_dlopen_ext`, `dlsym` (with
`RTLD_DEFAULT` and `RTLD_NEXT`; a name is looked up in the library's hash
table, symbols and names as they are in memory at the call, as bionic does, so
a packed library that decrypts its `.dynstr` in its constructors is found),
`dladdr`, `dlerror` and `dlclose`. A
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
  refused. C++ `thread_local` destructors run when their thread ends (an FLS
  callback, or `pthread_exit`), not when the process exits: as on Android,
  where `exit()` runs none, so the FLS callback that `ExitProcess` makes on
  the exiting thread does nothing once ntdll says the process is shutting
  down (`RtlDllShutdownInProgress`) or the C runtime's `exit()` has begun.
  Realm's destructor called JNI on a thread the JVM had detached there and
  the process hung.
- **The stack-protector patch** (`fs_tls_patch.cpp`): Bionic's stack guard is
  read with `mov rax, fs:[0x28]`. The loader rewrites the segment prefix so
  the instruction reads `gs:[0x28]` instead, which on x64 Windows is
  `NT_TIB.ArbitraryUserPointer`, a per-thread slot nothing else uses. The
  instruction length does not change. Libraries that ship their code
  encrypted and decrypt it from a constructor (IBM Trusteer's anti-fraud
  SDK in banking apps does) show the scan only ciphertext, so a vectored
  exception handler catches a guard load that faults on FS's zero base
  (a read of address 0x28), rewrites its prefix the same way and resumes it.

### The Bionic shim

`windows/src/mini_bionic.cpp` and `windows/src/bionic/` implement Android's
C library for guest code on top of the Windows CRT and Win32, about 900
functions, every one declared with the System V calling convention
(`WINAPK_GUEST_ABI`). By area:

| File | What it covers |
|---|---|
| `string.cpp` | `string.h`, ctype, 32-bit `wchar_t`, `strto*`, the FORTIFY `_chk` variants |
| `printf.cpp` | A printf engine that reads the System V `va_list`; the printf and scanf families; liblog's variadic calls |
| `pthread.cpp` | Mutexes, condition variables, rwlocks, once, keys and threads on `WaitOnAddress`, over Bionic's struct layouts; cleanup handlers (`pthread_exit` runs the pending ones); semaphores |
| `stdio.cpp`, `fd.cpp` | One file-descriptor table shared with Java (`NativeOs`): files, `stat` in Bionic's layout, directories, `FILE`, hard links; Bionic's `fts` over the same calls (it never changes directory: always `FTS_NOCHDIR`); `popen` and `system`, which find no shell to run (the command exits 127, as when `/system/bin/sh` cannot be run); pipes, eventfd and epoll; named pipes (`mkfifo`: a marker file `stat` reports as a FIFO, opened as the process's one pipe buffer for that path); `poll`, `select`, `epoll_wait`. A wait sleeps until something it waits for changes: pipes, eventfds and the like wake it through one readiness counter (`WaitOnAddress`); a wait that includes sockets sleeps in `WSAPoll` on them plus a loopback datagram socket of its own that the counter's changes send a byte to; an armed timerfd shortens the sleep to its expiry. Nothing is polled on a period. epoll's `EPOLLET` is edge-triggered (an event once when it appears, again after a new edge: more written to a pipe or eventfd, a socket read that took bytes, a send that would have blocked, `EPOLL_CTL_MOD`), `EPOLLONESHOT` reports once until `EPOLL_CTL_MOD`, and an interest goes when its descriptor closes |
| `socket.cpp` | BSD sockets on Winsock with Linux's constants translated; `getaddrinfo`; `getifaddrs`, `if_nametoindex`, `if_indextoname` and `if_nameindex` over one list of the adapters that are up (Linux-shaped names, Windows' interface indexes). Socket options: the ones Winsock has are mapped, among them multicast membership in all its forms (`ip_mreq`, `ip_mreqn`, `MCAST_JOIN_GROUP` and the other RFC 3678 requests), `SO_LINGER`, `SO_REUSEPORT` on datagram sockets (Windows' `SO_REUSEADDR`), the TCP keep-alive times, `TCP_USER_TIMEOUT`, path-MTU discovery as "don't fragment"; options that only tune (`SO_PRIORITY`, `TCP_QUICKACK`, `IP_RECVTOS`...) succeed with one log line each; any other fails with `ENOPROTOOPT`, so code that asks for a feature (`UDP_SEGMENT`) takes its fallback |
| `paths.cpp` | Android's absolute paths (`/data`, `/sdcard`, `/system/fonts`, `/dev/null`) under the Android root; `/proc/self` is `/proc/<pid>` there, where Boot writes the process's `cmdline` and an open writes its `status` and its `maps` (the text native code reads, `GuestLinker::proc_maps`: the process's `app_process64` and `linker64`, then the guest libraries); `/system/bin/app_process64`, `/system/bin/linker64` and `/apex/com.android.runtime/bin/linker64` are files with the process's ELF header, x86_64's, and there is no 32-bit `/system/bin/linker`, as on a 64-bit-only device (`include/winapk/process_image.h`; app packers choose their loader's build by them); the characters of an Android file name that Windows forbids, stored as U+F000 + the character as WSL does |
| `looper.cpp` | The NDK's `ALooper` |
| `misc.cpp` | Clocks and time, `strptime`, `mmap` on `VirtualAlloc` (with `PROT_NONE` as a reservation; a file mapped `MAP_SHARED`, or read-only, is a view of the file (`MapViewOfFile`), also `MAP_FIXED` into an untouched reservation, so writes reach the file and every mapping sees them; a writable private one is a copy); `mprotect`, `munmap` and `madvise(MADV_DONTNEED)` go region by region (`VirtualQuery`), as a range may hold reserved and committed pages of several allocations: opening a reserved page commits it, `PROT_NONE` leaves a reserved one as it is and keeps a committed one's contents, `MADV_DONTNEED` decommits and commits again in a mapping made here, system properties, the guest environment (`setenv`, `putenv`, `clearenv`), `syscall` for futex and friends and for raw `mmap`, `mprotect` and `munmap` (Gecko's allocator), `sysconf`, `uname`, `getauxval`, `sched_getaffinity` (the process's affinity), `fork`/`vfork` (-1, `ENOSYS`: a Windows process cannot be copied) and `waitpid` (`ECHILD`), `random(3)` as Bionic's (NetBSD's) generator, libm (the `long double` rounding functions on binary128), libandroid's `ATrace` (tracing off) |
| `locale.cpp` | The C and C.UTF-8 locales libc++ builds on, UTF-8 conversion, `wctype.h` |
| `signal.cpp` | Signals delivered to guest handlers: a vectored exception handler maps access violations, illegal instructions, divide errors and breakpoints to SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP and calls the handler with Bionic's `siginfo_t` and `ucontext_t` (registers written back, so a handler can fix the fault or redirect `rip`); `SA_SIGINFO`, `SA_ONSTACK` with `sigaltstack`, `SA_RESETHAND`, `SA_NODEFER`, `sa_mask`; `raise`, `kill`, `tgkill`, `abort` synchronously (`abort()`'s log line carries the message `android_set_abort_message` kept, libc++abi's "terminating due to uncaught exception ..." among them), `pthread_kill` by suspending the target and injecting the call when it is in guest code, else by posting the signal to it and waking the wait it sleeps in (a futex, condition variable, semaphore, `pthread_join`, poll, read, sleep), which then runs the handler and returns `EINTR` or waits on (`SA_RESTART`) as Linux's call would, so a Windows lock the target holds is never under a handler; a handler that declines three times leaves the fault to the crash report. Not delivered: faults in HotSpot or in Windows DLLs, faults in arm64 guest code, handlers in arm64 libraries. POSIX timers do not fire |
| `setjmp.cpp` | `setjmp` and `longjmp` for guest code |
| `cpuinfo.cpp` | `/proc/cpuinfo` (a block per logical processor, the flags under Linux's names for what CPUID reports, the topology Windows reports) and `/sys/devices/system/cpu/{possible,present,online}`, as an x86_64 device's kernel shows them: files under the Android root, written when a path names one |
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
`DetachCurrentThread` hand back guest envs. A few slots do what ART does where
HotSpot differs: `GetMethodID` finds an inherited constructor, `GetFieldID`
knows libcore's renamed `FileDescriptor.descriptor`, and `FindClass` inside a
library's `JNI_OnLoad` leaves the class uninitialized (HotSpot registers the
library only after `JNI_OnLoad`, so an initializer calling one of its natives
would fail).

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
5. `System.load` of an ELF that reaches the JDK itself, a call the rewriter
   never routed to a stub (by reflection, as protection layers with encrypted
   strings make it, or from a class of a dex loader): HotSpot's
   `NativeLibraries.load` is replaced too, and for an ELF it opens the file
   through the guest linker, runs its `JNI_OnLoad` (once per library, whichever
   way it was loaded) and gives HotSpot the guest library as the handle, bound
   to the caller's class loader as on Android. Any other file is the JDK's.

An app that calls `findLibrary` itself (through `BaseDexClassLoader`, as
Gecko's loader and `GameActivity` do) gets Android's answer, not the stub:
`lib<name>.so` in its native library directory, by its Android name
(`/data/app/.../lib/x86_64/lib<name>.so`). `System.load` of that name finds
the same stub module, and the guest linker takes an Android path for
`dlopen`, as the shim does for `open` (`src/bionic/paths.cpp`).

A dex class loader the app creates with a library search path (`DexClassLoader`,
`PathClassLoader`, `DelegateLastClassLoader`, `InMemoryDexClassLoader`) looks
there first, as Android's `DexPathList` does, and then in the app's own
libraries. Play services' modules need it: Google Maps loads
`CronetDynamite.apk` with the path `<that apk>!/lib/x86_64`. A library inside
an APK is copied out once to `<data dir>/code_cache/macapk-native-libs/`, with
the libraries beside it that it needs, because the ELF loader maps files; it
then gets a stub like any ELF the app loads by path, and the JVM binds it to
the loader whose class called `System.loadLibrary`.

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
  (512 MB each, 20% of the RAM and at least 2 GB kept free). A library the
  app has never run code in (no profile, no stamp) is not translated
  (`aot_stamp_needs_work`; Android's install-time dexopt likewise compiles
  only what a profile names). A library of up
  to 1,000,000 instructions is translated whole (the parts are of a fixed
  size, so that costs time, not memory); a larger one only where the JIT's
  profile (`<lib>.so.aot.profile`) says code ran, and again once the profile
  has grown by a quarter (and 200 lines), at most once an hour (the stamp's
  retry field). The running app starts the worker when its profile has
  settled or when it exits (`Cocoa_exitProcess`), and a worker started with
  the app waits a minute. Settings > Apps > "Speed up ARM apps"
  (`AotBackground`) turns the worker off: none starts, and a
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
  translation for the app (`aot_disable`) and says
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
(4 MiB, or the size `pthread_create` asked for when larger, up to 64 MiB) and
Bionic's TLS slots; `pthread_getattr_np` answers guest code with that stack,
the one its sp is on, as Bionic does (Dart, ART and V8 check their sp against
it); converts AAPCS64 arguments and `va_list`s to System V
for calls into the shim (a `va_list` by the shim function's address, so a
function that takes one is never a no-op shared with other names:
`register_shim_functions` refuses one that is, with a log line); and turns a host call into guest code (a callback)
into interpretation through an execute-fault handler, since arm64 text is
never mapped executable. A raw system call (`SVC #0`, or the `syscall()`
import) carries arm64's numbers, which `arm64_abi.cpp` maps onto the shim:
memory (`mmap` 222, `munmap` 215, `mprotect` 226, through the same wrappers
as the imports, so an executable mapping becomes guest code, but never a
host image's code: an arm64 caller's `mprotect` or `MAP_FIXED` over an
executable section of the exe, a DLL or a translation is `EACCES` with one
log line, as `include/winapk/host_code.h` explains; `mremap` 216,
which moves a private mapping by copying it), `futex`, the ids, clocks and
sleeps, files (`openat` relative to a directory descriptor, with arm64's
`O_DIRECTORY`/`O_NOFOLLOW` values, which the `open`/`openat` imports convert
too; `read`, `pread64`, `readlinkat`, `fstatat` with arm64's `struct stat`,
`flock`, `renameat`, `fdatasync`, the xattr calls 5-16, `ENOTSUP`...),
`epoll` with arm64's 16-byte `struct epoll_event`, `eventfd2`, `timerfd`,
`memfd_create`, `socket`, `connect`, `getrlimit`, `getcpu`, the `mlock`
family, `mincore`, `uname`, `ptrace`, `prlimit64` and others whose structs
both ABIs share. Where the kernel's interface is not libc's the kernel's is
kept: `getcwd` returns the length, `getpriority` 20 - nice, `clock_nanosleep`
`-errno`, `rt_sigaction` takes the kernel's `struct sigaction` and
`rt_sigaction`/`rt_sigprocmask` act on the shim's signal state, and
`set_tid_address` clears and wakes its word when the thread ends. A
shim function that returns an `int` is read as one. Anything else is
`-ENOSYS`, with one log line per number. Protection libraries make their
calls this way so that nothing hooking libc sees them (Bank Leumi's
`libe689.so` maps memory with SVC 222).

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
  ANGLE up on first use, so the runtime starts without it. A renderer runs
  on its own GL thread, as on Android: `GLSurfaceView` is AOSP's, whose
  GLThread makes an EGL10 context (`MacApkEGL10`, real for every thread) and
  a window surface on the view's holder, and an app's own GL thread does the
  same on EGL10 or EGL14; that window surface is the host's window surface on
  the SurfaceView, below.
- **From native code** (`host/gl/guest_gl.cpp`): every GLES 1.1 to 3.2 and
  EGL 1.5 function wrapped for the System V convention, the Khronos and
  ANGLE/Chromium extensions' functions too (GLES 1's those ANGLE has:
  `glDrawTex*OES`, `glBindFramebufferOES`...; `OES_fixed_point`'s `gl*xOES`
  are not ANGLE's and stay missing) (`third_party/angle/`'s
  `gl2ext_angle.h`), plus the EGL calls that need a window. `GL_EXTENSIONS`
  and `EGL_EXTENSIONS` name only the extensions whose every function the app
  can call and the ANGLE loaded resolves (`host/gl/guest_gl_extensions.cpp`):
  Skia and Flutter give up on GL when a listed one's function is missing. An app's `ANativeWindow` (`host/native_window.cpp`) is
  a size and a format; a window surface on it is a pbuffer that grows with
  the window, and `eglSwapBuffers` reads it back and presents it.

All of an app's GL, from Java or native code, is on one ANGLE display of the
app's own (`host/gl/gl_surface.cpp`), drawn by the app's own threads; the host
draws nothing on it, because ANGLE takes no lock between contexts that share
nothing on one Direct3D 11 device. A window surface's frame is read back at
the app's swap, on its thread, and becomes a layer under the app's windows
(below); until a SurfaceView's producer has drawn, the render tick shows black
in its place, as AOSP's SurfaceView shows its background layer. Vulkan is not
implemented.

### The view tree's frames

Every window of an app (the activity's, a dialog's, a popup's, a toast's) is
AOSP's `PhoneWindow` added to AOSP's `WindowManagerGlobal`, and its
`ViewRootImpl` measures, lays out and draws it on `Choreographer`'s frames, as on
a phone: `requestLayout` and `invalidate` schedule a traversal, and a view that
cannot be seen asks for nothing. The frames come from a Java vsync clock at the
host display's rate (`NativeDisplayEventReceiver`). Each window draws as on a
phone with hardware acceleration: `ThreadedRenderer` records the views'
`RenderNode` display lists, and HWUI's render thread replays them with Skia's
GPU backend (`SkiaOpenGLPipeline`, Ganesh on GLES over ANGLE) into the window's
surface. That is `bin\aosp-hwui.dll`, built from AOSP 14's own HWUI, Skia,
minikin, HarfBuzz, FreeType, ICU 72 and their closure by
`windows/hwui_build/` (`fetch.sh`, `build.sh`: Android.bp flattened to CMake,
`patch_src.py`'s edits for clang on MinGW-w64, the `glue/`), with its own JNI
on AOSP's classes. Each window's `SurfaceControl` is a layer in the DLL
(`HwuiLayers`, `glue/layers.cpp`): HWUI draws into a pbuffer of the window's
size, and the DLL's compositor, on the render thread, draws the layers bottom
to top as `Compositor` (`windows/java/aosp_views`) orders them as
WindowManagerService would, with the dim layer under a `FLAG_DIM_BEHIND`
window, into the host window's ANGLE surface, placed where the window puts the
phone (`host/hwui_host.cpp`, `window_gpu_layout`); SurfaceFlinger's part. A
buffer layer an app builds under a window's `SurfaceControl` (the text
magnifier's popup, a drag shadow) is a layer as well, which Win.apk's
`SurfaceControl` keeps in SurfaceFlinger's tree (parent, position, z, shown, as
Transactions set them) and `Compositor` draws over its window. A
window drawn in software (`hardwareAccelerated="false"`) posts its bitmap to
its layer. What the host has as pixels (an EGL window surface read back, a SurfaceView's
`lockCanvas`, an app's native window, a video frame) is a host layer under the
windows, seen through the hole a SurfaceView punches, or over them for
`setZOrderOnTop`. `PixelCopy` reads a window's layer back; a `HARDWARE` bitmap
is a heap bitmap with its pixels. ANGLE's compiled programs and Skia's shaders
are kept between runs in the app's cache folder (`EGL_ANDROID_blob_cache`,
HWUI's shader cache). The window's size and insets come from an in-process
window session (`IWindowSession.relayout` through AOSP's own
`WindowLayout.computeFrames`). `docs/research/aosp-views.md`, "V1 as built" and
"V2 as built", has the views' details and what is not there yet.

Win.apk's own view classes, which apps ran before, drew the tree into one
bitmap through `com.macapk.loader.ViewCanvasDriver`, and the window blitted
frames with GDI; neither runs any more.

### The window and the display

Each app has one Win32 window on its own UI thread (`win32_window.cpp`). The
window is the app's display: its client area in pixels is the app's surface,
the density comes from the scale the window shows the phone at (the
monitor's), and the Configuration and DisplayMetrics follow. Resizing,
maximizing or snapping gives the app a display of the new size and it lays
itself out again; maximizing, restoring and another monitor's scale also give
it a new density, delivered as Android delivers one (a configuration change
with a new `densityDpi`: resources reloaded, activities relaunched unless they
declare `density`; `DisplayConfig`), so the frames stay pixel for pixel; a fixed-orientation app
gets the largest box of its aspect, centred, with black bars. Frames are shown
pixel for pixel; a frame from before a resize is never stretched. The size
the user settles on is written to `library\<name>.window` for the next start.

Input leaves the window through `WindowInput` (`win32_window.h`), already in
Android's terms, and reaches `com.macapk.loader.TouchDispatcher` through
`native_cocoa.cpp`. The decisions that need no window are in
`host/pointer_input.h`, which `test/window_test.cpp` checks:

| Windows | Android | Java entry |
|---|---|---|
| The left mouse button | One finger on the touchscreen (`SOURCE_TOUCHSCREEN`) | `dispatch` |
| A touchscreen's contacts, a pen (`WM_POINTER*`; the frame's `WM_NCPOINTER*` stay Windows') | One pointer each, with Android's ids and indices: `ACTION_DOWN`, `ACTION_POINTER_DOWN`, one `ACTION_MOVE` per digitizer frame with every position, `ACTION_POINTER_UP`, `ACTION_UP`, `ACTION_CANCEL`; a pen is `TOOL_TYPE_STYLUS` / `TOOL_TYPE_ERASER` with pressure and `BUTTON_STYLUS_PRIMARY` (`TouchTracker`) | `dispatchHostTouch` |
| The wheel, a touchpad's two-finger scroll | `ACTION_SCROLL` from `SOURCE_MOUSE` (`AXIS_VSCROLL` / `AXIS_HSCROLL`); when no view handles it, the short finger drag every wheel step used to be | `dispatchHostWheel` |
| Ctrl+wheel, which is also a precision touchpad's pinch | Two fingers moving apart or together around the pointer (`PinchSynth`) | `dispatchHostPinch` |
| The right and the middle button, the pointer with no button down | A mouse (`SOURCE_MOUSE`): `BUTTON_SECONDARY` / `BUTTON_TERTIARY` with `ACTION_BUTTON_PRESS` / `_RELEASE`, which the framework answers with the context click; `ACTION_HOVER_ENTER` / `MOVE` / `EXIT`, at most one per 16 ms | `dispatchHostMouse`, `dispatchHostMouseExit` (upstream's) |
| Keys | `KeyEvent`s (Escape is BACK) carrying the character Windows says the key types: the `WM_CHAR` behind the key-down, so the layout, AltGr and Caps Lock are in it (`plan_key_down`, `HeldKeys`) | `dispatchHostKey` |
| What a dead key and the next key make, characters beyond U+FFFF, `WM_CHAR` with no key | Text: `InputConnection.commitText`, or a key event per UTF-16 unit for a target without a connection | `dispatchHostText` |
| Windows' text services (TSF: today's IMEs, handwriting, dictation, reconversion, the touch keyboard's suggestions) | The focused editor is the window's TSF text store (`tsf_store.h`, over `text_input_model.h`'s document): its kind as input scopes (`EditorInfo.inputType`), a copy `WindowsTextInput` sends whenever it changes, and where its `Layout` drew each character for the candidate window. Each TSF lock that wrote is one edit for the editor, applied as an input method's `setComposingRegion` / `setComposingText` (with the text service's display attributes as spans) / `commitText` / `setSelection`; the editor's own changes reach the text services as TSF's text and selection changes. Keys reach the text services first. A password's document is empty and read-only, and no input method composes in it | `WindowsTextInput`, `native_text_input.cpp` |
| `showSoftInput` / `hideSoftInput`, a tap on a text field | Windows' touch keyboard (`InputPane.TryShow`/`TryHide`), on a PC with a touchscreen and for a field touched with a finger or pen; Windows declines when its own setting says so (a keyboard attached) | `WindowsTextInput.softInput`, `input_pane.h` |
| An input method's composition (`WM_IME_*`: an IMM32 IME, or any IME when TSF cannot be set up) | `setComposingText` / `commitText` / `finishComposingText` on the focused field's `InputConnection`; without one, Windows draws the composition and the result arrives as characters. The candidate window goes under the focused `TextView`'s caret | `dispatchHostComposingText`, `hostImeInline`, `hostImeAnchor` |

One stream at a time owns the app's touchscreen (the left button, the other
buttons, the contacts, or a pinch), so the app never sees two gestures
interleaved. A touch gesture with one pointer takes the mouse's path through
`TouchDispatcher` (the Back glyph, dialogs and popups, drag and drop); from a
second pointer on, a gesture the activity owns goes to the activity with all
its pointers, and one a floating window owns keeps following its first
pointer only, because that router is single-pointer.

A steady 60 Hz tick on a waitable timer drives the app's
renderer. `WINAPK_HEADLESS=1` runs the same path without a window, which is
how CI boots apps.

### Audio, media, camera, location and the other devices

| Android API | Windows implementation | Source |
|---|---|---|
| AudioTrack, SoundPool, MediaPlayer (audio) | A WASAPI mixer; WAV, Ogg Vorbis (stb_vorbis) and Ogg Opus (libopus, `soft_codecs.h`; pre-skip and end trimmed) decoded in-process, MP3, AAC and whatever Windows has through Media Foundation | `native_audio.cpp`, `audio_engine.cpp`, `audio_decode.cpp` |
| AAudio (native) | A port of upstream's AAudio on the same mixer; write and callback modes, no capture | `native_aaudio.cpp` |
| OpenSL ES (native) | Win.apk's own, from the OpenSL ES 1.0.1 spec and Android 14's behaviour (frameworks/wilhelm): one shared engine, whose callback thread runs every callback (buffer queue, play, prefetch, object, recorder) one at a time, in order, with no lock held; players are `audio::Source`s the mixer pulls from a ring kept about two device periods (~25 ms) ahead, the app's PCM (8-bit, 16/24/32-bit, float, up to 8 channels) converted and resampled band-limited by `audio_pcm.h`; file players (fd, file, http(s)) are decoded whole by `audio_decode.cpp` when first paused or played, as Android prepares them, with prefetch events, seek and loop; decoding into a buffer queue with the PCM metadata keys; recorders fill the app's queued buffers from the microphone's capture (`audio_capture.h`), realized only when RECORD_AUDIO is held as AudioRecord asks it. Guest callbacks through `arm64::host_callable`. No effects, MIDI or 3D | `native_opensles.cpp` |
| Microphone (AudioRecord) | WASAPI capture, resampled to the app's format | `native_audio_input.cpp` |
| Video: MediaPlayer, VideoView | `HostVideo.Player` over `video_player.cpp`: an `IMFSourceReader` demuxes and decodes (NV12, or P010 for 10-bit; own YUV-to-ARGB conversion, aperture crop, container rotation), the audio goes into the audio engine, and a Java thread pulls each picture when the audio clock says it is due and presents it on the SurfaceView's layer, the TextureView's SurfaceTexture, or the window | `video_player.cpp`, `video_decode.cpp`, `native_media_video.cpp` |
| Video: MediaCodec | `NativeMedia`'s video natives on the synchronous decoder MFTs: H.264 always, HEVC/VP9/VP8/AV1/MPEG-4 when a decoder is installed (`nativeVideoSupportedMimes` lists what `MFTEnumEx` really creates). Where Media Foundation has no VP8, VP9 or AV1 decoder, or its decoder refuses the stream before its first picture, the built-in software ones take it, as Android falls back on its c2.android codecs (`soft_codecs.h`: libvpx and dav1d with their x86-64 SIMD, threaded over the PC's cores), listed with their profile/level tables (`nativeVideoCapabilities`) and the log line `[media] ...: decoding with libvpx VP9`. Their pictures stay in the decoder's own (pooled) buffers until converted: to ARGB for the Surface (`yuv_convert.cpp`, the stream's matrix and range, 10-bit dithered), or without a Surface into MediaCodec's output buffer and Image as I420, or P010 when the app asked for it (`nativeVidFrameYuv`), with the stride, crop and colour in the output format. AAC and MP3 audio decoders are Media Foundation's, Opus is libopus (48 kHz, multistream, surround in Android's channel order, the pre-skip, gain and seek pre-roll applied). The libraries are built from their releases, checked against their SHA-256 (libwebm against its tag's commit), by `windows/cmake/soft_codecs.cmake`, with the configuration `windows/scripts/soft_codecs_config.sh` made from their own configure steps. The NDK media API is `ndk_media_codec.cpp` on the same `Decoder`, `AudioDecoder` and `Demuxer`, with no JVM: `AMediaCodec` decoders (named as `MediaCodecList` names them, eight input and eight output buffers, the output format announced before the first buffer of each size, an empty end-of-stream buffer last; I420 or P010 buffers, 16-bit PCM, or pictures shown on the native window; the asynchronous callbacks on a thread of the codec's own), `AMediaExtractor` on a file, URL, descriptor range or `AMediaDataSource` (the last two copied to a temporary file first); no encoder. `native_media.cpp` has `AMediaFormat` and the `AMEDIAFORMAT_KEY_*` strings | `native_media_video.cpp`, `video_decode.cpp`, `ndk_media_codec.cpp` |
| Video and audio out: MediaCodec encoders, MediaMuxer, MediaRecorder | `HostMedia`'s natives. Encoders: the synchronous (software) H.264 and AAC encoder MFTs, listed in `MediaCodecList` only when `MFTEnumEx` creates them (some Server and N editions have none: `createEncoderByType` then throws `IOException`). H.264: up to 1920x1088, baseline/main/high, VBR or CBR, no B-frames, input as NV12 or I420 buffers or an input Surface; output is Annex-B with the SPS/PPS first as a codec-config buffer and as `csd-0`/`csd-1`. AAC-LC: 44100 or 48000 Hz, mono or stereo, 96 to 192 kbit/s, raw frames with the AudioSpecificConfig as `csd-0`. An input Surface (`createInputSurface`) takes Canvas drawing (`Surface.macapkSetFrameSink`) and a camera2 session's frames (it is an `ImageReader`'s Surface, which the session feeds); OpenGL ES drawing on an EGL window surface made on it is not read back, and the legacy `Camera` has no path to it. `MediaMuxer` writes MP4 and 3GPP with a writer of its own (`mp4_writer.cpp`, no Media Foundation: one progressive `mdat`, the `moov` at `stop()`; `avcC`, `esds`, `ctts` and an edit list for reordered frames, rotation, location) and refuses WebM, Ogg and HEIF. `MediaRecorder` is those three plus `AudioRecord`: AAC in MP4/3GPP/ADTS, H.264 from `VideoSource.SURFACE`; AMR and the other formats fail in `prepare()` | `native_media_codec.cpp`, `media_encode.cpp`, `mp4_writer.cpp` |
| MediaExtractor, MediaMetadataRetriever | `video_decode.h`'s `Demuxer`: an `IMFSourceReader` with no decoder, every audio and video track as the container holds it (H.264/HEVC as Annex-B with the parameter sets in `csd-0`/`csd-1`, AAC raw with its config), sync flags, the three seek modes. A file that starts with an EBML header (WebM, Matroska) is read by libwebm's mkvparser instead (`webm_demux.h`), with the tracks' formats as AOSP's `MatroskaExtractor` gives them (VP9's and AV1's CodecPrivate, Opus' OpusHead, codec delay and seek pre-roll in `csd-0..2`, Vorbis' headers, `avcC`/`hvcC` as Annex-B) and seeks through its Cues. A path or an http(s) URL is opened by name; a FileDescriptor range, a `content://` Uri and a `MediaDataSource` are copied to a temporary file first. The retriever answers duration, tracks, size, rotation and bit rate from it and decodes frames (for a path or URL) with the player's source reader; it reads no tags | `native_media_codec.cpp`, `video_decode.cpp` |
| Images | libwebp for WebP, WIC for the rest | `native_image.cpp` |
| Notifications | `NotificationManager` posts to `HostNotifications`, whose natives show Windows.UI.Notifications toasts (WinRT C ABI, combase loaded at run time) under the app's AppUserModelID, which the launcher writes on the app's Start menu shortcut; a click comes back through `HostNotifications.onActivated` and `PendingIntent.send()` | `native_notifications.cpp` |
| WebView, Custom Tabs, App Link sign-in | Microsoft Edge WebView2, shown in the app window behind the views drawn over it; `shouldInterceptRequest`, JavaScript interfaces, cookies; `file:` URLs on a synthetic https origin answered from the APK's assets and resources and the app's files (`webview_file_urls.h`, `MacApkWebViewBridge.macapkServeFile`); a Custom Tab is a tab window whose navigations are checked against the app's manifest; a browser sign-in whose `redirect_uri` is one of the app's verified App Links (or its own link scheme) opens in the same window, which hands back only that redirect (`applink_match.h`) | `native_webview.cpp`, `webview_file_urls.h`, `webview2_host.cpp`, `native_browser_tab.cpp` |
| Camera (camera2, Camera, CameraX) | Media Foundation capture, each webcam offered as a back and a front camera (so apps that want the rear camera work); a virtual camera when there is none. The sensor orientation is the display rotation (back) or its mirror (front), read when asked, and previews get AOSP's rotation and mirror transforms. Focus and torch are the driver's: `IAMCameraControl`'s focus or the extended `FOCUSMODE`/`FOCUSSTATE`/`TORCHMODE` controls, run on the capture thread between frames; without them a fixed-focus camera with no flash. Preview and analysis frames, and stills as JPEG (encoded in Java from the captured frame); no video recording. `ACTION_IMAGE_CAPTURE` opens the host's camera window (`CameraCaptureHandler`, `nativeCapture`: a Win32 window on its own thread, owned by the app's and disabling it; WIC encodes the JPEG, which Java writes to `EXTRA_OUTPUT`). An app's native code reaches the same cameras through libcamera2ndk and `AImageReader` (`ndk_camera.cpp`), over `camera_host.h`: a session's frames go to the readers (scaled, YUV, RGBA or JPEG) and the native window, and its callbacks run on a thread of the device's own | `native_camera.cpp`, `ndk_camera.cpp` |
| Location | `Windows.Devices.Geolocation`; Windows' privacy settings are the authorization | `native_location.cpp` |
| Connectivity | `ConnectivityManager`, `NetworkCapabilities` and `LinkProperties` answer from the adapter that carries the default route (`HostRadios.defaultNetwork`: `GetBestInterfaceEx` for a public address, then that adapter in `GetAdaptersAddresses`): Ethernet, Wi-Fi, mobile broadband or a tunnel as the transport and the legacy network type, its addresses, DNS servers, MTU and the JDK's name for it; no default route is "no network". Polled every two seconds for `NetworkCallback`. "Validated" means only that there is a default route, and no connection is reported as metered. `NsdManager` (mDNS service discovery) is not implemented: every request reports `FAILURE_INTERNAL_ERROR` to its listener | `native_radios.cpp` |
| Proxy | Windows' own: the JVM runs with `java.net.useSystemProxies=true`, so java.net's default `ProxySelector` asks WinHTTP per address (a manual proxy with its exceptions, a PAC script, WPAD); an app's own selector or `http.proxyHost` comes first. `ConnectivityManager.getDefaultProxy` reports none | `runtime_main.cpp` (`jvm_options`) |
| Wi-Fi | `HostRadios` over the Native Wifi API: state, the current connection (SSID, BSSID, RSSI, frequency, link speed), the DHCP configuration from `GetAdaptersAddresses`, scans, and `WlanRegisterNotification` events pumped by one Java thread into the state, connection, signal and scan-results broadcasts. Network names need Windows' location access, as Android needs the location permission | `native_radios.cpp` |
| Bluetooth | `HostBluetooth`: the paired devices (bluetoothapis), RFCOMM sockets both ways on Winsock `AF_BTH` (the SDP query for the channel, `WSASetService` for a server's record), LE scans on the `BluetoothLEAdvertisementWatcher` and the GATT client (services, reads, writes, notifications through the CCCD, MTU) on the WinRT `Windows.Devices.Bluetooth` APIs through their C ABI, declared in the file. Permissions as AOSP's service: `BLUETOOTH_CONNECT` / `BLUETOOTH_SCAN`, location for scan results unless `neverForLocation`. Not there: classic discovery and pairing, LE advertising and the GATT server, profiles, L2CAP | `native_bluetooth.cpp`, `bluetooth_util.h` |
| Clipboard | Text, HTML, URLs, with the concealed formats for sensitive clips | `native_clipboard.cpp` |
| File and folder pickers | `IFileOpenDialog` / `IFileSaveDialog` | `native_picker.cpp` |
| Game controllers | XInput, polled | `native_gamepad.cpp` |
| Photos, wallpaper | The Pictures and Videos folders with shell thumbnails; the desktop wallpaper | `native_photos.cpp`, `native_wallpaper.cpp` |
| ICU (`android.icu`) | AOSP's own ICU4J, as apps have it on a device: every calendar (Hebrew, Islamic, Chinese...), the number, date, plural, measure and relative-time formatters, collation, normalization, transliteration, Bidi; on ICU 72's data, the same file the graphics DLL reads (`bin\aosp\icu\icudt72l.dat`), and beside it the time zone tables a device's tz module gives (`zoneinfo64.res` and three more, a newer tzdata) | `lib/aosp-graphics.jar` (`aosp_gfx_take.sh`), `windows/java/aosp_gfx/android/icu/platform/AndroidDataFiles.java` |
| SQLite | Upstream's `NativeSqlite` compiled unchanged over the SQLite amalgamation | `runtime/src/native_sqlite.cpp`, `compat/CoreFoundation` |
| `NativeActivity`, the NDK input queue, `AConfiguration` | A port of upstream's, over the guest linker and the shim's `ALooper` | `native_activity.cpp` |
| Shared storage | The volume's Download, Pictures, Documents, Music and Movies are junctions to the user's Windows folders | `user_folders.cpp` |

There is no sensor hardware behind `SensorManager`: no accelerometer is reported (nor gravity, rotation vectors or orientation, which are fused from one), as on an Android device without one, and no gyroscope or other sensor either: the list is empty, for Java and for the NDK's sensor manager (`native_sensor.cpp`). Win.apk dropped the synthetic "phone held upright" gravity and the zero-reporting gyroscope the framework used to report.

### The launcher

`Win.apk.exe` (`windows/launcher/launcher_main.cpp`, one file, GDI+; its state
machines in headers beside it: `app_runs.h`, `installs.h`, `updates.h`, `store_app.h`) is the
app library, the counterpart of upstream's Swift launcher. It installs APKs
by copying them into `library\` and running `winapk-runtime --install-only`
(a bundle then kept as hard links to the APKs it installed, `library_files.h`),
gives each app its shortcuts, runs apps (`winapk-runtime --apk`), shows
their logs, and shows the Store (Win.apk's own store app, `app.apkstore`, from
israel2176/win.apk-store) inside its window, as Google Play is shown with Google
services on: the runtime's window as a borderless child of a panel over the
content area (`WINAPK_EMBED_PARENT`). The store app is a system component, no
tile and no shortcut: at the Store's first opening, and when the daily update
check finds a newer release, the release's `apk-store.apk` is downloaded, kept
only at the size and SHA-256 digest GitHub's REST API lists for it
(`launcher/store_app.h`), and installed quietly; its "Install unknown apps" is
allowed from its first install. It also holds the settings and each app's
details. Every user-visible string is in English and
Hebrew, and in Hebrew the windows are laid out right to left.

One `Win.apk.exe` process serves the library, the install window and the
launch splashes. A second start (Explorer opens one per selected file) hands
its files, or its wish to see the library, to the running one in a
`WM_COPYDATA` and exits, so files opened together share one install window.
The library window is created hidden and shown only when asked for: an APK
opened from Explorer shows only the install window, a shortcut only the
splash. A process whose library is hidden exits once it has nothing left to
show or wait for.

The app file type (`WinApk.Package`) carries a thumbnail handler,
`bin\winapk-shell.dll` (`windows/shell/`), registered per user by the
installer and by the launcher at each start: Explorer shows each `.apk`,
`.apkm`, `.xapk` and `.apks` file's own app icon. It parses the file itself
(`apk_icon.cpp`: the zip's directory, the binary manifest's
`<application android:icon>`, `resources.arsc`'s files for that resource per
density, an adaptive icon's two layers when they are pictures or colors; a
bundle's `icon.png` or its base APK), bounded in what it reads, and decodes
with WIC; nothing of the APK runs and no JVM starts, and the shell runs it in
its isolated surrogate process. A vector icon, which only Android's code
draws, falls back to the `apps\<name>\icon.png` the install rendered for the
same package, as does a picture no installed WIC codec reads. An adaptive
icon is composed as AOSP's `AdaptiveIconDrawable` draws it (the middle 72 of
108 dp, under the platform's default circle mask), premultiplied while scaled
and composed, and handed back as a straight-alpha `WTSAT_ARGB` bitmap of at
most 1024 px (`apk_thumbnail.cpp`, from Microsoft's thumbnail handler
documentation).

### The install helper and the installed-apps registry

An app that installs other apps (the Store, through `PackageInstaller`) hands
the APK to upstream's `PackageInstallerBackend`, which runs an install
helper. On Windows the helper is the launcher itself:
`Win.apk.exe install --apk <file> --source android [--requestor-label <app>] --json`
and `uninstall --package <name> ...`. It plays Android's package installer:
an asking app whose "Install unknown apps" special access is off first gets
the blocked-source dialog, whose Settings is that switch
(`appops::installs_allowed_from_caller`); then "Do you want to install this
app?"; then, alone in the helper's process, the install window with the
install's steps and Cancel, and its end, "App installed." with Open and Done
or "App not installed." with the reason (`helper_install_ui`). It installs into
the library as above (`install_into_library`, cancelled through the same
`InstallTable`), and keeps `installed_apps.json`, the registry the package
manager answers from, so a store sees what it installed. A row's `installedAt`
(Android's first install time, and the key of the app's signed grants) is kept
across updates. The output follows upstream's contract: `[helper] progress
<fraction> <stage>` lines, then one JSON object, then, when the install window
shows the end, `[helper] exit <code>`: `PackageInstallerBackend.runHelper`
returns on that line, so the store gets its status when the install ends, as
on a device, while the window stays for Open or Done. Exit codes 0 done, 2 bad
arguments, 3 declined or cancelled (`STATUS_FAILURE_ABORTED`,
`RESULT_CANCELED`), 5 failed, 6 not an app. The genuine Play Store (Google
signed) installs with no question and no window.

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
launcher's account item (the foot of its pane). A window that ends without a
token says why as a code (`signInWindowError`: the WebView2 HRESULT, the
page's `COREWEBVIEW2_WEB_ERROR_STATUS` and HTTP status, `TIMEOUT`), which
`--gms signin-window` prints as its `ERROR` line and the launcher shows. `winapk-runtime --gms
<command>` runs upstream's `GmsCli` with no app, which is how the launcher's
Google account setting lists accounts, adds one and signs out.

**Who holds the master token.** As on Android, where only the account's
authenticator (Play services) holds it and an app gets scoped tokens through
`AccountManager.getAuthToken`, no app's process reads it. The layer's state is
two files in `gms\`: `gms_state.properties`, what any process may know (which
accounts are signed in, their non-secret data, the checkin identity), and
`tokens.dat`, the master token and the cached tokens as text sealed with DPAPI
for the Windows user and Win.apk's entropy (`host/data_protection.h`,
`com.macapk.loader.DataProtection`). Only the account's owner side opens
`tokens.dat` (`GmsStore.becomeOwner`): `--gms` (the launcher's Settings and
its sign-in window), the system broker, and the **token service**,
`winapk-runtime --gms token-service` (`GmsTokenService`). An app's process
sends every request that needs the master token or the token cache to the
token service over Binder (`GmsTokens`): a scoped token
(`getAuthToken`, the Play licence check), a token minted for the app itself
(Sign in with Google's ID token and server auth code: the service reads the
caller's package and signing certificate from the APK its `--apk` names, and
refuses a request naming another), the cache (peek, set, invalidate; a cached
`app:<package>:` token is that app's only), the account id, and the sign-in
window's `oauth_token`, which becomes the master token in the service; the app
gets the e-mail back. The first request starts the service (`logs\gms-tokens.log`),
which exits after five idle minutes. An older install's plain tokens in
`gms_state.properties` are moved into `tokens.dat` by the first owner that
reads the file: the launcher runs `--gms accounts` at its start while there is
no `tokens.dat` (`seal_gms_tokens`), and an app's process that still finds
none has the service do it before it reads.
What this does not do: every app runs with the user's rights and its native
code is not confined, so code that sets out to can still read the user's DPAPI
secrets or start a runtime naming another app's APK; real isolation needs a
sandbox for apps, which Win.apk does not have yet.

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
  `android:process` of a package gets its own single-instance key, so there
  is one process per (package, process name): a start that finds the key
  held exits (`kExitAlreadyRunning`) and the broker waits for the running
  copy to attach. A process it started exits when the broker does. One that
  crashes again within two minutes of its last crash is a bad process, as
  ActivityManager's AppErrors marks it: the broker does not start it again
  for anyone until the user opens the app or the package is updated
  (`ProcessCrashes`). When you
  open an app the broker is running headless that way (Play services binds
  the Play Store's services, for one), the new start stops the headless
  process and takes its place with a window; its clients bind again, as
  after Android stops a background process.
- **Packages installed, updated or removed while the broker runs.** The
  broker reads `installed_apps.json` once, as it starts (its name is served
  first, and every call waits for that read). After that it learns a package
  as PackageManagerService does, at the commit: once the registry holds the
  change, `install_into_library` (every install and update: the library's,
  the Store's own app, the Google services setup's, the install helper's for an
  app that installs another) and `remove_app` (Remove, the helper's
  uninstall) call `SystemServer`'s `TX_PACKAGE_CHANGED` with the package and
  the APK the registry names (`windows/include/winapk/package_events.h`),
  and wait for the broker to have it: an app started right after is known.
  The broker takes that call only from the launcher, by what Windows says
  the calling pid runs (`Win.apk.exe` above the runtime's `bin\`,
  `WinProcesses.isLauncher`; the broker's pipe accepts that one program
  besides the runtime). It then updates its package table, the bundles and
  the account authenticators, and broadcasts as ActivityManager does:
  `PACKAGE_ADDED` (`EXTRA_REPLACING` on an update), `PACKAGE_REPLACED` and
  `MY_PACKAGE_REPLACED` (to the updated package alone, its process started
  for a manifest receiver), or `PACKAGE_REMOVED` (not to the removed package)
  and, with its data, `PACKAGE_FULLY_REMOVED`, with the package's uid. The
  implicit ones reach running processes only: every dynamic receiver, and
  the manifest receivers of an app that targets API 25 or lower, or for
  `PACKAGE_FULLY_REMOVED`, an exempt action (otherwise the intent carries
  `FLAG_RECEIVER_REGISTERED_ONLY`, which the app's delivery honours). An
  app's own `PACKAGE_*` broadcast is refused, and an installing app attached
  to the broker does not announce its install itself. A package's running
  processes are stopped by the launcher before it touches the files: Remove
  stops every runtime of the app; an update stops the ones with no window
  (the broker's service hosts, Google's three apps) and waits for the user
  to close an app open in a window. With no broker running nothing is sent.
- **Google accounts with the broker.** The accounts service mirrors
  `gms\` (Win.apk's one Google account store) into its table: the master
  token as the row's password and the gaia id as `GoogleUserId`, where Play
  services looks. The broker is one of the account's owners, so it reads the
  master token itself, and its table (`system_ce\0\accounts_ce.dat`) is
  sealed with DPAPI as `tokens.dat` is (`AccountStore.sealedAt`; an older
  install's plain `accounts_ce.properties` is moved into it once). It follows the file as other
  processes write it (Settings' `--gms signin-window` / `signout`, an app's
  sign-in) and broadcasts `LOGIN_ACCOUNTS_CHANGED`. `AccountManager.addAccount`
  for `com.google` (the Play Store's "Sign in") opens Win.apk's sign-in
  window in the caller's process instead of Play services' authenticator,
  whose sign-in activity would have to start in Play services' process with
  the authenticator's response binder.
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
  executable (the broker also takes the launcher's package changes, above).
- **What a caller may reach** (`BrokerPermissions`, as AOSP 14's
  system_server decides it, against the package the caller's pid is). A
  service, provider or receiver of another app must be exported, and its
  `android:permission` held (`checkComponentPermission`; the same app, or a
  package sharing its uid, always passes). A package holds a permission it
  requests when it is normal; dangerous, when the user granted it in the
  signed app-ops store (or it targets API 22 or lower); signature, when its
  verified signer (or its rotation lineage) is the declaring package's
  (`ApkVerification`); `signature|privileged` and its kin also when it is one
  of Google's three apps verified as Google's. The platform's own permissions
  and their levels come from `bin\aosp\framework-res-34.apk`'s manifest; no
  app has the platform key. A permission nobody declares is nobody's. A
  provider opens only with its read or write permission, a path-permission's,
  or a URI grant under its authority (else AOSP's `SecurityException`); then
  every query (read), insert, update, delete (write) and `openFile` (by its
  mode) is checked by `RemoteProviders.Transport` in the provider's process,
  which asks the broker (`CHECK_PROVIDER`, the caller by the pid the OS gives
  that pipe): its permission, path-permissions, URI grants. URI grants
  (`FLAG_GRANT_READ/WRITE_URI_PERMISSION`, prefix grants, an intent's data
  and ClipData) are made as the broker hands an intent to another app's
  service or an explicit broadcast, and by `Context.grantUriPermission`, when
  the provider allows granting and the granter has access; they last while
  the broker runs. A broadcast's required permissions (`sendBroadcast`'s
  `receiverPermission`) leave out a package that lacks them; a receiver's own
  permission, manifest or registered, is checked in its process against the
  sender the broker names (`CHECK_PERMISSION`). An app's own check of a
  caller (`checkCallingPermission`, `Context.checkPermission` for another
  process) is the broker's answer for that process too. Failures are what Android
  gives: `SecurityException` with AOSP's message for a bind, start, stop or
  provider, a broadcast skipped with a `Permission Denial` line in
  `logs\system.log`. Google Play services' `com.google.android.c2dm.permission.SEND`
  (signature) is held by Google's apps because their signers match, which
  is how FCM messages reach an app's receiver.
- **Google's packages.** `com.google.android.gms`, `.gsf` and
  `com.android.vending` get the system-app treatment (`FLAG_SYSTEM`, started
  at the broker's boot) only when their APKs' signature verifies and Google's
  certificate made it, as the signer or as an ancestor in the verified v3
  rotation lineage (`GoogleGmsCertificate.isGoogleSigner`). Win.apk ships
  none of them. The launcher's setup page downloads the versions
  `windows/launcher/google_apps.h` pins (the first install only: the Play
  Store then updates itself and Play services) with apkeep
  (`bin\tools\apkeep.exe`, `windows/scripts/fetch_apkeep.sh`) from APKPure
  into `downloads\google\`, or watches the Downloads folder for a copy the
  user fetched from APKMirror, and installs a file only when
  `winapk-runtime --verify-apk` (`ApkCheck`) says Google signed it, its native
  code (if any) has an x86_64 build, and its minSdkVersion is not above
  `PresentedSdk`'s ceiling. The launcher treats the three as system
  components: no tile or shortcut, only the setup page shows them.
- **Google Play in the launcher's window.** The pane's Google Play starts the
  Play Store with `WINAPK_EMBED_PARENT=<hwnd>`, the handle of a panel the
  launcher keeps over its content area, and `WINAPK_PHONE_SIZE` the panel's
  size in dp. The runtime then creates its window as a borderless child of
  that panel (`win32_window.cpp`, "embedded in the launcher") instead of a
  top-level window: no work-area fitting, frame or remembered size, no
  minimizing, no foreground. The launcher sizes the child with the panel
  (asynchronously, so a busy app never blocks it) and the app follows the box
  as it follows a resized window; Windows joins the two processes' input
  queues, as for any parent and child, and the launcher hands the child the
  keyboard focus when Google Play is the view. Another tab hides the panel
  and the app keeps running; closing the library window stops it. A runtime
  that could not make the child opens its own window, and the launcher, which
  finds its panel empty when the ready event fires, says so.
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
tests the transport and the broker's checks across three processes
(`BrokerPermissionsTest.java` the permission rules on APKs it signs), and the
package changes with a copy of the test program as `Win.apk.exe` above a
`bin\` copy: a package added while the broker runs, replaced, removed, and
the change refused from anyone but the launcher.

## An install, step by step

1. The launcher reads the file first (`winapk-runtime --verify-apk`, one file
   at a time: its package and versionCode) and names the app
   (`install_name`: the package when the file name starts with one, as
   `com.waze_5.26.5.0-..._apkmirror.com.apkm` does, else the file name up to
   its version; then `launcher/installs.h` `place_install`: a package already
   in the library keeps that app's name, so the install updates it, a file of
   another package never takes the name of an app it is not, and a lower
   versionCode than the installed app's installs only after the user's yes),
   copies the file into
   `library\<name>.<ext>` (an update renames the app's files aside as
   `<file>.previous`, whatever their extension, deletes them once the new one
   is installed, and puts them back if the install fails or is cancelled; an
   install cut off by a crash or the power is undone the same way at the
   launcher's next start, `recover_interrupted_installs`; a kept file is never
   written, since it is the installed APK itself: `launcher/library_files.h`),
   removes the install's record so that the install runs whatever the file's
   date, and starts `winapk-runtime --apk <that file> --install-only`, logging
   to `logs\install-<name>.log`. The install window reads that log for its
   status line. Several `.apk` files opened at once (dropped or chosen
   together) are first asked which app each is a part of
   (`winapk-runtime --apk-set-to`, AppInstall `--set`), and those of one
   package become one install, as `adb install-multiple` makes them one
   session: the base is copied in as `library\<name>.apk` and the others as
   `library\<name>.splits\split_<split name>.apk`, the layout step 2 installs a
   kept bundle from (`install_files`, `launcher/installs.h`
   `group_install_sets`). Files Explorer opens one by one ("Open" on several)
   arrive one by one, and each is its own install.
2. `com.winapk.tools.AppInstall` (in a child JVM) unpacks the app into the
   device layout under `android\data\app\~~<nonce>\<package>-<nonce>\`:
   `base.apk` (a hard link for a plain APK), `split_<name>.apk` chosen as
   upstream chooses them. Before the installed copy is touched, the APKs are
   checked as one session the way PackageInstallerSession checks one
   (`validateApkInstallLocked`, AOSP 14): one base, each split once, every APK
   of the base's package and version code, and a base that declares
   `android:isSplitRequired` with its splits (those of every type its
   `android:requiredSplitTypes` lists). A set that fails exits 3 with
   Android's error (`INSTALL_FAILED_INVALID_APK: Full install must include a
   base package`, `INSTALL_FAILED_MISSING_SPLIT: Missing split for <package>`,
   `Inconsistent package`/`version`, `Split <name> was defined multiple
   times`) and the app's install from before stays as it was; the install
   window says why in a sentence. Each APK is then named by its manifest, as
   the device names a staged APK: `base.apk`, `split_<split name>.apk`. Then
   the native libraries of the first ABI in
   `x86_64,arm64-v8a` the app has, under `lib\<isa>\`. Libraries a later ABI
   has and that one lacks (an arm64-only SDK in an x86_64 build) go to
   `lib\arm64\` with the arm64 libraries they need (`DT_NEEDED`, followed):
   `findLibrary` looks there for a library `lib\x86_64\` lacks, the guest
   linker gives an arm64 library the arm64 copies of its dependencies even
   where an x86_64 library of that name is loaded, and the background
   translation sees only those. A record file notes the result so the step
   runs once per input file.
3. `com.winapk.tools.AppMeta`, in the runtime's own JVM (the framework, its
   graphics and resources), checks the signature, renders the launcher icon to
   `apps\<name>\icon.png`, and writes the label (resolved from the app's
   resources through `ResBackend`) and `package.txt` (package, versionCode,
   versionName, the first activity's manifest `screenOrientation`). It runs
   beside steps 4 and 5, and is done long before the conversion, so that the
   install window and the library's tile show a new app's own icon and name
   while it is converted, as Android's installer does (`pending_look` in the
   launcher). When this Win.apk has no framework archive yet, its dump starts
   here too, in the background (above), and the install does not wait for it.
4. If the app has only arm64 libraries, a background `winapk-runtime
   --aot-translate` starts for each: `winapk-aot` writes C, clang compiles it
   into `<lib>.so.aot.dll`. Nothing waits for it.
5. Upstream's DEX converter (`com.macapk.dex.Main`) turns the app's DEX into
   `apps\<name>\classes.jar`, reading the class hierarchy from
   `libndroid-framework.jar` (the framework the app links against, not an
   Android SDK the PC may have), with a stamp naming the converter and the APK
   so a change to either converts again. It runs in a JVM of its own with a
   1 GB heap, on HotSpot's C1 compiler alone for up to 40 MB of DEX (the
   conversion is short, and C2's warm-up took half its time: Jerboa 4.6 -> 2.3
   s; `dex_jit_options`); one that runs out of it (seen through JVMTI, even when the
   converter caught the error and left a class out) converts again on a
   larger heap, and a jar is kept only when it is a whole zip, whatever the
   exit code says (under Wine a killed converter reported 0). The launcher then moves the label and
   `package.txt` beside the APK as `library\<name>.label` and `.package`.
   A bundle (`.apkm`, `.xapk`, `.apks`) is then replaced by
   `library\<name>.apk` and `library\<name>.splits\split_*.apk`: hard links to
   the installed `base.apk` and splits, once AppInstall's stamp
   (`<copy>\.installed`) says the installed copy was made from that very file,
   so the library keeps one copy of the app (`keep_installed_apks`,
   `launcher/library_files.h`). AppInstall, given an `.apk` with a `.splits`
   folder beside it, links them all again, so a reinstall (a stale record)
   has the whole app. A library from before this layout moves to it at the
   launcher's next start, app by app, once each app's installed copy is of its
   bundle (else at the start after its next run).
6. The launcher makes the Start menu shortcut (and the desktop one, if the
   setting is on), adds the app to the library and writes the registry
   (`installed_apps.json`). With a system broker running (Google services
   on), it then tells the broker of the new or updated package
   (`TX_PACKAGE_CHANGED`, above), which broadcasts `PACKAGE_ADDED`. Before
   step 1 an update stops what runs of the app with no window (the broker's
   service hosts, Google's apps); an app open in a window is left for the
   user to close first.

## A run, step by step

1. The launcher (or a shortcut: `Win.apk.exe --run <library apk>`) shows the
   app's starting window at once: a normal window with the app's name and
   icon on its frame and the icon in its middle, as Android's starting window
   is, of the size the app's window will open at (`host/window_fit.h`, the
   arithmetic the runtime sizes its window with) and with the app's
   AppUserModelID, so the taskbar counts it as the app. It sets
   `WINAPK_PHONE_SIZE` from the App window size
   setting and the app's effective orientation, passes the starting window
   as `WINAPK_START_WINDOW` (the runtime opens its window over it, so the
   app's window takes its place), and starts
   `winapk-runtime --apk <library apk>` with its output in
   `logs\run-<name>.log` (the previous run's kept as `run-<name>.1.log`;
   `last-run.log` is a hard link to the newest). It passes
   `WINAPK_READY_EVENT`, an inherited event handle, and waits for the event
   or the process: the runtime sets the event once its window has painted
   the app's first frame (`win32_window.cpp`, `signal_launcher_ready`), and
   the starting window goes. If the process ends first, that window says the app did
   not start, with Show log and Report a problem (`report_problem`: GitHub's
   issue form prefilled through its fields' ids in the URL, and Explorer on
   the run log and the newest crash dump; nothing is uploaded). A run that
   ends as a crash (`include/winapk/app_exit.h`: the framework's crash exit,
   a native fault, `abort()`) gets Android's crash dialog instead, before or
   after the window came up (`show_crash_dialog`: "has stopped", or "keeps
   stopping" within a minute of the last, App info, Close app, those two
   links). Input given to the starting window (a click, a key) that the app
   has not shown a window for five seconds on asks "isn't responding", Close
   app or Wait (InputDispatcher's no-focused-window ANR).
2. The runtime takes a named mutex keyed on the app's data folder. If another
   copy holds it, the running window is brought forward, the ready event is
   set, and this process exits.
3. The install and DEX conversion are checked (cached, so this is quick), and
   shared storage is mapped to the user's folders if the setting is on.
4. HotSpot is created in-process on a thread with a large stack, with the
   C1 compiler only (a faster start than tiered C1+C2) and class-data sharing:
   the JRE's archive and the framework archive (see [The host](#the-host); the
   run log's `[win.apk] cds:` line says which, and `startup:` lines when the
   JVM was up and when the first frame came, with the working set). The
   archive is normally there before the first start (the installer and the
   first app install make it); a start without one records its classes, and
   10 s after its first frame (or at its exit) starts the archive's dump in
   the background;
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
8. Closing the window (`WM_CLOSE`; `WM_ENDSESSION` at sign-out or shutdown)
   hides it at once and runs upstream's close path on a thread of its own
   (`native_cocoa.cpp`, `close_thread`): `ActivityHost.macapkTerminate` has
   the task manager remove the task (the top paused first, then `onStop` and
   `onDestroy` for every activity from the top down), and waits up to two
   seconds, ticking the renderer meanwhile; then
   the process ends, and the run log says
   `[win.apk] close: the app was paused, stopped and destroyed in <n> ms; ended`.
   An app whose main thread is stuck gets
   `close: the app did not answer in time; ended` (a watchdog thread that
   uses no Java ends it half a second after the two); a second close request
   ends it at once, and the launcher's Stop ends the process from outside
   with no lifecycle, as Force stop does. `SharedPreferences.apply()` writes
   the file before it returns, so nothing is queued at that point. Minimizing
   the window is the task going to the back
   (`ActivityHost.macapkHostMinimized`: `onPause`, `onStop`); restoring it
   brings the task forward (`macapkTaskToFront`: `onRestart`, `onStart`,
   `onResume`), the same transitions as Back at the task's root. The exit
   code is 0 unless the app or the VM failed (always 0 once the user closed
   the window); a VM crash leaves `logs\hs_err_pid<n>.log`. An exception
   nothing catches, on any of the app's threads, leaves its looper as on
   Android (Win.apk's own `winapk-*`/`macapk-*` threads log it and go on), and
   the uncaught handler (`Boot`, RuntimeInit's KillApplicationHandler) logs
   `FATAL EXCEPTION` and ends the process with `kExitAppCrashed`. Input the
   main thread has not taken five seconds after the host gave it
   (`InputTimeout`, in `aosp-views.jar`) asks the launcher's "isn't responding"
   (`Win.apk.exe app-not-responding`, over the app's window); Close app ends
   the process with `kExitClosedNotResponding`, which the launcher takes as
   the user's word.
   The lifecycle itself is AOSP's: `Boot` starts the app on AOSP 14's own
   `ActivityThread` (`lib\aosp-app.jar`) through `com.macapk.loader.app.AppHost`,
   whose in-process activity manager and task manager send it the client
   transactions system_server would (`docs/research/aosp-app.md`); the
   `ActivityHost` entries above hand the window's close, minimize and restore
   to that task manager. Services, broadcasts and the app's
   providers run through `ActivityThread` too (the activity manager's
   `ActiveServices` and `ActiveBroadcasts`, `installContentProviders`); a
   service or receiver of another app is the system broker's. The framework
   has no `Activity`, `Application`, `ActivityThread`, `Service` or
   `BroadcastReceiver` of its own: it is compiled against AOSP's
   (`windows/scripts/aosp_app_take.sh`), and asks the process's Application
   through `com.macapk.loader.app.Apps`.

## Data on disk

Everything is under `%LOCALAPPDATA%\Win.apk\` (the launcher's and runtime's
default; `--data` moves it for a direct run):

| Path | What |
|---|---|
| `library\<name>.apk`, `.splits\`, `.label`, `.package`, `.orientation`, `.window` | The user's copy of each app and the launcher's per-app files. The `.apk` is the installed `base.apk` under `android\data\app\` (a hard link, one copy on disk; an update renames the app's files aside and copies the new file in, so none is written in place); an app that came as a bundle (`.apkm`, `.xapk`, `.apks`) also has `<name>.splits\split_*.apk`, hard links to its installed splits. A bundle is in the library only while it installs: its members cannot be links, so keeping it took its size twice (a 191 MB Waze `.xapk` took about 380 MB). What goes with it is what was never installed: its `manifest.json` and icon, a bundletool set's whole-app alternatives |
| `apps\<name>\` | `classes.jar`, `.macapk-dex-stamp`, `icon.png`, `label.txt`, `package.txt`; for an arm64-only app that crashed with its translation loaded, `aot-guard.txt` (the crash guard's count and whether translation is off) |
| `android\` | The Android file system: `data\app\` (installed packages, with `lib\<isa>\` and the `.so.aot.*` translation files; `lib\arm64\.aot-tmp\<lib>\` holds an unfinished translation), `data\data\<package>\` (app data), `dev\socket\` (the socket files that stand for abstract-namespace `LocalSocket` names), the shared-storage volume |
| `logs\` | `run-<name>.log` and `run-<name>.1.log`, `last-run.log` (a hard link to the newest run log), `install-<name>.log`, `gms.log`, `gms-tokens.log` (the Google token service), `install-helper.log`, `package-backfill.log`, `hs_err_pid*.log` |
| `store-app\` | The Store's app: `installed.sha256` (the SHA-256 of the release asset installed, for the daily check), and `apk-store.apk` while it downloads or waits to be installed. An older install's `store\` (F-Droid's catalog) is removed at start |
| `installed_apps.json` | The installed-apps registry the package manager answers from |
| `appops\` | `app-ops.json`, `signing-key.pub`, `signing-key.p256` |
| `gms\` | `gms_state.properties` (the signed-in accounts and what any process may know), `tokens.dat` (the master token and cached tokens, sealed with DPAPI; only the account's owner side opens it), the `accounts.present` marker, `photo-<account>.img` |
| `app_uids.properties`, `system_ce\0\`, `installer-of-record.properties`, `run\system-ready` | With Google services on: per-package uids, the accounts service's store (`accounts_ce.dat`, sealed with DPAPI), which app installed which, the broker's readiness |
| `logs\system.log`, `logs\services\` | The system broker and the apps it starts as service hosts |
| `aot-cache\` | Whole arm64 translations shared between apps, `<key>.aot.dll` (kept under 2 GB, least recently used first); a `*.tmp` is a write in progress, or one a killed worker left (deleted when an hour old) |
| `cds\` | The framework archive (`host/cds.h`): `framework-<graphics>-<key>.jsa` (`aosp`; `aospres`, `aospviews` with AOSP's resources or views; each kept), `.ok` once it has mapped whole, `.fails` (failures in a row; at two it is not made again for that key), `dump.log` (the last dump), a `.classlist` while a first start records, and `patch-digests.txt` (the patch folders' digests under a stamp of their listing, so a start does not hash them again) |
| `cache\` | The runtime's shared caches: `aosp-fonts\` (the `fonts.xml` on Windows' CJK fonts that `/system/etc/fonts.xml` is, for AOSP's graphics and native text stacks such as Flutter's), `fwview\` (the framework's resolved resources), `monet\`, `apk-verify\` (APK signature verdicts), `applinks\<package>.properties` (App Link verdicts per domain, written only when a sign-in needed one: 7 days, an unreachable site 1 hour, dropped when the APK changes) |
| `run\<package>.pid` | Each running app's run lock |
| `staging\` | The install helper's staging area |
| `updates\` | `releases.json` and `releases.etag` (GitHub's list of releases from the last update check), `Win.apk-Setup-<version>.exe` (a downloaded update, until it is installed) |

What is shared and what is per app: `cds\` (about 40 MB) and `cache\` (about
16 MB) are made once for all apps; each app adds its `apps\<name>\classes.jar`
(a few MB to a few tens), its installed copy under `android\data\app\` (the
APK again, and an arm64 app's extracted libraries) and its data. The program
folder (`%LOCALAPPDATA%\Programs\Win.apk\`, about 545 MB, 80 MB of it
AOSP graphics' files) holds one copy of everything else:
nothing of it is copied per app. `windows/README.md`, "Footprint",
has the measurements and the budget CI holds them to.

An app sees its files by Android's names, not by these: its data directory
is `/data/user/0/<package>` (here `android\data\data\<folder>\`, by its 8.3
short name), shared storage `/storage/emulated/0` and `/sdcard`
(`android\sdcard\`), and any other `/x` is `android\x`. Guest native code
goes through the Bionic shim's table (`windows/src/bionic/paths.cpp`); java.io
and java.nio through the java.base overlay's `jdk.internal.util.WinApkPaths`,
which asks the same table (`windows/host/java_io_open.cpp`), so a `File` keeps
its Android path while its opens, attributes and listings reach the host file.
An Android file name may hold characters Windows forbids (`< > : " | ? *`,
control characters, a final dot or space): the table stores each as the
private-use character U+F000 + the character, the mapping WSL and Cygwin use
(YouTube's `<account>||.entitystore` database, with U+F07C for each `|`
on disk), and directory listings under an Android path (the shim's `readdir`
and `scandir`, `File.list`, a `DirectoryStream`) turn the names back. Host
paths (`C:\...`) are never rewritten.

Settings live in `HKCU\Software\Win.apk`: `Theme`, `Language`, `PhoneSize`,
`Landscape` (absent means landscape; 0 means portrait), `DesktopShortcut`,
`UserFolders`, `AotBackground` (absent means on: the background arm64
translation),
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

The GitHub Actions workflows:

| Workflow | When | What |
|---|---|---|
| `ci.yml` | Pull requests touching `windows/` or `runtime/`, and by hand; a push to `main` only builds (to leave its compile cache to pull requests) and fills the UI gate's caches | One pipeline of jobs. *What changed* decides which stages a pull request needs. The guest test libraries build on Ubuntu, and a launcher-only change gets a one-minute MinGW compile with warnings as errors. Everything else builds once on `windows-latest` (MSYS2 CLANG64, ccache) and stages the runtime. That output goes as artifacts to the jobs that run side by side: *Unit tests* (every test program), *Runtime tests* (the Java tests, GL, an APK booted headless and in a window, class-data sharing, a stage missing a graphics file stopping with its error line, the framework tests that draw in `winapk-runtime --java-test`, the GMS CLI), *App smoke test* (the ten apps of `windows/test/compat/smoke.txt`, one of each kind, on AOSP's graphics; it fails when one no longer draws or ran on Java2D's), *AOSP graphics* (AOSP 14's `android.graphics` on Robolectric's native Skia and minikin inside `winapk-runtime.exe`, `windows/test/aospgfx/`: milestone 1 of drawing with Android's own graphics, in `windows/README.md`), and *Package* (a preview folder with the JRE the build baked and the installer). The smoke test and the sweep run on that JRE, with one framework archive made first (`--cds-prepare`) and shared by the apps, and the smoke test records the apps' classes into the `framework-classlist` artifact, the source of `windows/java/winapk-framework.classlist`. The whole compatibility sweep runs only by hand (Run workflow, "Full compatibility sweep"): split over four machines (`sweep.py --shard`) and joined by *Sweep report* into one ranked report. The *UI gate* holds a change to the framework, the host or the loader to "only closer to Android, never further": on Ubuntu, Android's lines for the UI probe and the apps of `windows/test/screens/gate.txt` (Robolectric; cached by what they are made from, so built again only when the probe, the screens oracle or the pinned APKs change) and the base commit's framework jars; then on Windows, one job per target, Win.apk's lines at the base (`main`'s own CI build of that commit while it is kept, else the head's build with the base's jars) and at the head, side by side, each on its commit's default graphics (the summary names which: Java2D at a base from before AOSP's became the default), and `windows/test/uiprobe/regress.py`, which fails the job on a line that matched Android before and not now, moved further from it, or is gone. A new push cancels the run in progress |
| `windows-release.yml` | By hand only, with a tag | Builds (stamping the tag's version into `Win.apk.exe`), packages with the JRE `bake_jre.sh` makes from Temurin 21, smoke-tests the package, builds the installer and publishes a GitHub release (a pre-release when its `prerelease` input is ticked) |
| `ui-sweep.yml` | By hand only, with an F-Droid package or an APK URL, and a branch | Not a test: the screens sweep of one app (`windows/test/screens/screens.sh`), Android's side on Ubuntu and Win.apk's on Windows (the branch's newest CI build), compared; `oracle.txt`, `ours.txt`, `diff.txt`, `summary.txt` and both sides' pictures are the artifact |
| `pages.yml` | A push to `main` touching `site/`, `docs/images/` or the launcher's icon, and by hand | Deploys the project's web page (`site/`, with the screenshots and icon copied in) to GitHub Pages |

[`windows/README.md`](../windows/README.md) has the commands.

The launcher's update check (Settings > Updates) reads that list of releases
from GitHub's API, takes the highest version that is not a draft (and not a
pre-release once there is a full release; `/releases/latest` would skip
pre-releases altogether), and compares it with the version in its
own resource; **Update** downloads the release's `Win.apk-Setup.exe` only when
GitHub lists its SHA-256 digest, keeps the download only at its
Content-Length (a malformed or contradictory one refuses the response,
`updates.h` `expected_body`), and runs it (a copy kept from before too) only when its size,
SHA-256 and ProductVersion (`VersionInfoProductTextVersion`, the full version)
are the release's and its Authenticode signature, if it has one, verifies (a
code-signed build takes only an installer by the same signer;
`launcher/updates.h` `judge_setup`: the signer is the certificate's subject and
issuer), then quits so that
the installer (`CloseApplications=yes`) can replace the files. The version comes from `WINAPK_VERSION`, which the release workflow
sets from its tag; a `ci.yml` build is the newest release tag + `-dev.<run>`
(compared as that release); a local build is 0.1.0.

## What is not there

As of this writing: the NDK media API, Vulkan, real sensor input,
signal delivery for faults in arm64 guest code and handlers in arm64
libraries, POSIX timers, static ELF TLS, W^X thunk pools with unwind tables,
code signing, 32-bit ARM,
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
