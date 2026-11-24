# Improvement plan (November 2026)

Ten read-only reviews of the code, each from one angle: compatibility, the
launcher's UI, startup, runtime performance, incomplete implementations,
PairIP, the upstream repository, architecture, stability, and tests. This page
keeps what they found and the order to act in. File and line references are to
`main` at `1ac1559`; check them before acting, the code moves.

What already landed from it (this change): the library window no longer
repaints 60 times a second while an app runs; the ELF loader's bounds checks
cannot be wrapped by a hostile file; a failed allocation for a missing guest
variable is reported instead of binding a null object; and every call to a
not-implemented stub is counted, with a summary at the end of the run's log
(`windows/host/stub_hits.h`).

## 1. The bug class behind most crashes: stubs that answer zero

Three seams turn "not implemented" into a stub that returns 0, all on
`make_tagged_zero_stub` (`windows/src/abi_thunk.cpp`): host natives
(`host_natives.cpp`, `report_stub`), guest system functions
(`guest_libs.cpp`, `missing_stub`) and guest system variables
(`missing_object`, 4 KB of zeros). Zero is a fine answer for `void`, `int` or
`boolean`. For a method that returns an object, a handle or a size it is not:
the caller takes `null` or 0 as a real result, and the crash or the empty
screen comes later, far from the stub, with no log line nearby. The files that
were fixed one by one (`native_opensles.cpp`, `native_sensor.cpp`,
`native_media.cpp`, AAudio input) replaced such zeros with a proper failure
value; the generic stubs still answer zero.

The plan, in order:

1. **Count and report** every stub call (done: `stub_hits.h`). Each run's log
   now ends with what the app asked for and did not get.
2. **A CI gate on the inventory.** `windows/test/apicheck/ApiCheck.java` and
   `windows/test/compat/native_gaps.py` already compute what apps reference
   and the runtime lacks, but run only by hand; the sweep parses stub lines but
   fails only on "did not draw". Run them in CI against a checked-in baseline
   and fail on a new gap, not on the known backlog. Add the boot invariant
   `declared == implemented + stubbed` per host class against a baseline file,
   so a mistyped signature in a `HostMethods` table (which silently falls back
   to a stub) is caught.
3. **Answer by the signature.** The JNI signature is known when the stub is
   made (`host_natives.cpp`, `register_host_natives`). Keep zero for `void`
   and primitives; for a reference return, throw `UnsatisfiedLinkError` naming
   the method, with an allow-list for the methods where `null` is the right
   answer. Stage it behind an environment variable, turn it on once the smoke
   apps are clean. Same idea for guest functions: a debug-mode switch that
   stops at the first call with the symbol's name.
4. **Size missing guest variables** from the defining symbol's `st_size` where
   it is known, with a guard page after the block so an oversized access
   faults at the library that made it.
5. **Show stub calls live** in debug mode, over the debug agent's pipe, in the
   Threads window.
6. **Fill the known holes:** the four Firebase C++ SDK classes in
   `kHostClasses` have no implementation at all; the GL tables
   (`gl/gl_bridges.cpp`) should be diffed against the GLES/EGL headers.

## 2. Compatibility, by apps unlocked

| Order | Gap | Where | Approach | Size |
|---|---|---|---|---|
| 1 | OpenSL ES | `native_opensles.cpp` | Done: Win.apk's own, from the spec and AOSP's behaviour, pulled by `audio_engine.h`'s WASAPI mixer, recorders on the microphone's capture (#132) | M |
| 2 | NDK `AMediaCodec` / `AMediaExtractor` | `native_media.cpp` | Bridge the NDK ABI onto the Java path's working demuxer and Media Foundation codecs (#133) | M |
| 3 | Opus, Vorbis, FLAC in `MediaCodec` | `native_media_codec.cpp`, `video_decode.cpp` | Add the three decoders beside AAC and MP3: every ExoPlayer app | M |
| 4 | `libbinder_ndk.so` exports nothing | `windows/src/linker.cpp` | Port upstream's `bionic_shim_binder_ndk.cpp` (local in-process binder) into `windows/src/bionic/`; a library calling it from `init_array` aborts today | M |
| 5 | Sensors | `native_sensor.cpp`, `android/hardware/` | `Windows.Devices.Sensors` into both the NDK and Java sensor managers | M |
| 6 | AAudio input | `native_aaudio.cpp` | WASAPI capture, as the output path | M |
| 7 | Child processes | `src/bionic/misc.cpp` | A second runtime process over shared memory and the existing Binder (#117) | L |
| 8 | 32-bit ARM, Vulkan | | New translator; new graphics path | XL |

## 3. Startup

- **Build the CDS archive when Win.apk is installed or updated.** The archive's
  key covers the framework jars and the JRE, so every update leaves the next
  launch of every app with no archive, rebuilt 10 s after that app's first
  frame. `winapk-runtime --cds-prepare` exists; the installer or the
  launcher's first start after an update should run it. Saves the cold
  penalty (0.2 to 0.4 s) after every update.
- **Start ANGLE in parallel with the JVM.** EGL and the D3D11 device are made
  on the app's first GL call (`gl/gl_surface.cpp`, `ensure_context`); both are
  independent of Java. Tens to 150 ms.
- **Load `jvm.dll` while the stamp checks run** (`runtime_main.cpp`, the main
  path before `boot_thread`). Tens of ms.
- Keep: C1 only, Serial GC, the 60 s delay before background translation.

## 4. Runtime performance

Every GL frame is drawn into a pbuffer, read back to the CPU, converted pixel
by pixel and copied two or three more times before GDI draws it.

1. Make the asynchronous (double PBO) readback the default and use it for Java
   `GLSurfaceView` too; flush the last frame from the render tick so on-demand
   UIs (Flutter) do not keep a stale one (`gl/guest_gl.cpp`,
   `gl/gl_surface.cpp`, `native_cocoa.cpp`).
2. Convert once into a pair of DIB sections and publish by swapping pointers
   (`win32_window.cpp`, `window_present`).
3. Do not hold `g_frame_lock` across the GDI blit in `paint()`.
4. Read back `GL_BGRA_EXT` and let a top-down DIB do the flip, instead of the
   scalar swap in `argb_from_rgba`.
5. Longer term: when the GL view is the only content, present ANGLE's swap
   chain to the window directly and skip the readback.

Translator changes (`.eh_frame_hdr` discovery, compile flags, cross-DLL call
caches) wait for the Windows measurements `aot-measurements.md` section 8
still owes.

## 5. Stability

- The arm64 crash guard (`aot_guard.cpp`) needs two crashes and then turns
  translation off for the whole app. Record which translated libraries were
  loaded and turn off only the guilty one.
- On a stack overflow, `note_crash` allocates on the faulting stack before the
  minidump is written, so the crash may be neither counted nor dumped. Write
  the dump first and make the counting path allocation-free.
- The vectored exception handler must stay allocation- and lock-free on the
  paths that pass HotSpot's deliberate faults; add a test.

## 6. Tests and CI

1. The completeness gate of section 1.2.
2. Cache the APK `runtime-tests` downloads from F-Droid on every run (one
   download feeds about six steps).
3. Per-app frame baselines for the ten smoke apps, beyond "more than two
   colours"; a nightly `uiprobe` job for right-to-left layout.
4. First-frame times of the smoke apps in the job summary, with budgets.
5. Report the sweep's retries instead of hiding them.
6. Compare the launcher's `kGroupRows` with `RuntimePermissionGroups.java`.

## 7. PairIP

What is there: Play Integrity and Play Licensing are answered honestly
(`IntegrityManagerImpl` reports no Play Store; licensing relays Google's own
signed answer for the signed-in account and never makes one up). The device
persona is generic, not a copy of a Google-certified device. The Java half of
PairIP's protection already works: `ClassFileRewriter` rebuilds the method
registry PairIP's VM would install, from the class files alone, and
`AppClassLoader.wirePairipRegistry` installs it. The native half does not:
`Log_registerPairipGate` answers "not here" (`native_log.cpp`), upstream's gate
is macOS-specific, and upstream's translator work on `libpairipcore.so` lives
in its JIT, which Win.apk replaced with its own translator. The upstream files
(`pairip_fake_modules.h`, `art_jni_offsets.h`, ...) are in the tree but unused.

The way forward is to run PairIP's own code in an environment that is
consistent and real, never to forge Google's answers or alter the protected
code:

1. **Diagnose:** name an integrity-protected app as such in the start error;
   port the trace switches to log `executeVM` calls on Windows and learn which
   check fails first.
2. **A consistent process:** a JNIEnv table at ART's offsets for PairIP apps
   only, `/proc/self/maps` built from the real process layout, an `environ`
   that is contiguous and stable.
3. **The startup gate:** give the startup program its observable effects (the
   string pool, the method registry) while the debugger and signature checks
   still run and `libpairipcore`'s code stays unmodified.
4. **arm64 hardening** for `libpairipcore` in the translator; start with an
   x86_64 build of the library, which needs no translator.

Out of scope: forged Play Integrity or licensing answers, patched or stripped
protection, impersonating a certified device. An app that needs an attestation
verdict will still not get one; that is the honest outcome.

## 8. Upstream

Upstream's public repository (`kksimp/Mac.apk`) has had no code change since
v1.0.2379 (2026-11-05), which Win.apk already includes. Win.apk's
`runtime/java` has every upstream class and 56 more, and is ahead in every file
compared; re-syncing it would undo fixes. The translator is a deliberate
Windows fork. The one file worth porting is
`runtime/translator/src/bionic_shim_binder_ndk.cpp` (section 2, item 4).

## 9. The launcher

Found clean: right-to-left drawing, dialogs' keyboard rules, empty and offline
states, flicker, and the four state machines. Left: an error InfoBar can only
be closed or followed with the mouse (Esc and Tab should reach it), and a run
whose ready event could not be made shows "starting" until the app exits.
