# Win.apk and the other runtime: what each can give the other (2026-11-23)

the other runtime (https://example.invalid/other-runtime, read at 52513f0) runs real ART ported to Windows,
Android 16's own framework.jar, HWUI+Skia on ANGLE, real Binder with servicemanager and its own system
server, and microG. Read-only comparison (code and docs; nothing of the other runtime was built or run here).

## Where Win.apk could contribute to the other runtime (best value first)

1. **Bundle installs.** the other runtime's `app/java/com/other/server/Installer.java` decides a bundle by suffix
   (.xapk/.apks), flattens entries, takes splits only named `split_*`/`config.*`: no `.apkm`, bundletool
   `.apks` splits/standalones mixed, an ".xapk" that is an APK fails, `manifest.json`'s base ignored.
   Win.apk's `windows/java/com/winapk/tools/AppInstall.java` (APK by content, base by manifest.json /
   base.apk / package / largest, bundletool layout, device split names) ports as Java into the other runtime's server.
   Not its cross-ABI library fallback.
2. **App corpus by kind.** the other runtime's e2e has 10 simple F-Droid apps, no Compose/Flutter/RN/libGDX/SDL/
   WebView-shell; Win.apk's `windows/test/compat/apps.txt` (~90) and `smoke.txt` + `sweep.py` blocker ranking.
3. **Size budget in CI** (`windows/scripts/footprint_check.py`); the other runtime has none.
4. **Differential UI oracle** (`windows/test/screens`, `windows/test/uiprobe` regress/gate): the other runtime has
   pixel/lifecycle smoke only. LayoutSweep would become a public-API test APK there.
5. **Hebrew user docs** (`docs/he/`), rewritten for the other runtime.
6. **Native fixes:** raw arm64 syscalls the other runtime answers ENOSYS (`compat/arm64/dll/guest_libc.cc`):
   membarrier, set_tid_address, rt_sigaction/procmask, mremap, prlimit64, clock_nanosleep, msync,
   mincore, set/getpriority (ptrace must not fake success); the exit hang on guest thread_local
   destructors (`windows/src/elf_tls.cpp`), to check in the other runtime.
7. **Persistent arm64 AOT cache + crash guard** (`arm64_aot.cpp`, `host/aot_guard.h`): the other runtime keeps
   translations per run; cost is clang's size against its "Light" principle.
8. Smaller: Report a problem; the frame bench.

## What the other runtime's policy would refuse

- "Report Google Play as the installer" (`InstallerOfRecord.java`): reports an installer that did not
  install the app, to pass an app's requirement; against the other runtime's CONTRIBUTING and docs/install-source.md.
- GMS stand-ins reporting Play services present; Win.apk's own sign-in window in place of the authenticator.
- The Store tab ("No store"); background Play updates ("nothing runs when no app is open"); hard-linked
  library copies of user files.

## What Win.apk should take from the other runtime

Installer download verification (digest, length, version); a signed F-Droid index; WebView assets served on
a synthetic https origin (and android_res) instead of copying pages; working OpenSL ES; software
VP8/VP9/AV1/HEVC decoders and WebM; more raw syscalls; exporting all ANGLE extension functions; its
evidence discipline (smoke vs functional, known failures); AppContainer isolation work; contract-first policy.

## Not verified

the other runtime's exit hang; other bundle paths in the other runtime; AOT gains on the other runtime's dynarmic; Control Center vs the
launcher in use; the other runtime's CI claims (taken from its docs).

## Code-verified (2026-11-23, code only, the other runtime 52513f0)

- **Bundles: confirmed.** Installer.java:410 suffix-only; no apkm/manifest.json/standalones anywhere;
  extractZip flattens; base = first non-split in listFiles order; companionSplits only split_*/config.*;
  an ".xapk" that is an APK throws. Win.apk's AppInstall handles all, but installs every bundletool split
  (no toc.pb device selection) and takes the largest standalone, not the ABI's.
- **arm64: confirmed.** the other runtime: dynarmic, in-memory only, exclusives via callbacks, exact NaN, whole-cache
  clear on a generation bump, no IC IVAU handling, fastmem failure = guest crash. Win.apk: dynarmic JIT +
  persistent clang AOT shared by code hash (2 GB, 60 days), crash guard, inline exclusives, 64-byte IC IVAU
  invalidation broadcast. Not measured.
- **Raw syscalls: mixed.** The 12 are ENOSYS in the other runtime; in Win.apk several are stubs: rt_sigaction/procmask
  return 0 doing nothing, ptrace returns 0 (the other runtime's -ENOSYS is the more honest answer; ours should be
  reviewed), setpriority no-op, getpriority 0, mincore all-resident, mremap ENOMEM, set_tid_address ignores
  the address. Real: msync, clock_nanosleep (errno sign bug on the raw path), prlimit64 old limit.
  the other runtime has, Win.apk lacks: xattr 5-16, epoll_ctl/pwait, flock, renameat, fdatasync, timerfd 85-87,
  getrlimit, getcpu, socket, connect, mlock family, memfd_create. Win.apk raw getcwd returns the pointer,
  openat only cwd/absolute.
- **Exit hang: likely in the other runtime by another path.** compat/elf/tls.cc ArenaDestructor (FLS callback) takes
  g_mutex with no exiting check; a thread killed at ExitProcess while holding g_mutex (dlopen, thread start)
  blocks it. Win.apk's elf_tls.cpp process_exiting guard covers exactly this.
- **WebView:** the other runtime serves android_asset/android_res on https://other-file.example via deferred
  WebResourceRequested; Win.apk copies pages (48 MB cap), no android_res; both run shouldInterceptRequest
  on a worker pool.
- **OpenSL ES:** the other runtime real (players, recorders); Win.apk stub.
- **Media:** the other runtime software libvpx/dav1d/libhevc/libavc, opus, libwebm, MP4/WebM/Ogg/ADTS muxers.
  Win.apk decodes avc/hevc/vp8/vp9/av1/mp4v/h263/mpeg2 through Media Foundation only (needs the PC's
  extensions), encoders are MF's, own MP4 writer.
- **Update check:** Win.apk does verify size and sha256 digest (earlier claim wrong); missing vs the other runtime: no
  digest -> size only, no product-version check, pre-releases not skipped, a cached copy re-run on size only.
- **GL extensions:** Win.apk lists only callable extensions (safer; hides ANGLE_/CHROMIUM_); the other runtime exports
  all, and a declared function ANGLE lacks aborts on call.
- **Tests:** the other runtime e2e 10 apps incl. TIC-80 (SDL2); no Compose/Flutter/RN/libGDX/WebView-shell; no size
  budget; no view-tree oracle. Win.apk 92 apps, smoke 10 per PR, footprint and UI gate in CI.
- **Hebrew:** both full (the other runtime 231 keys, Win.apk 431).
- **Package events/jobs:** the other runtime real PMS broadcasts and persisted jobs (written in place, no temp+rename).

## Where Win.apk is ahead in user experience (code-checked, 2026-11-23)

- App window: size from the monitor's work area and scaling, WM_DPICHANGED kept (window_fit.h,
  win32_window.cpp); per-app orientation, remembered size/maximized, phone sizes, letterbox. the other runtime:
  fixed 400x800 dp at 160 dpi, no monitor scaling, OTHER_DISPLAY env only.
- Launcher: per-app Stop, details page with permissions (revoke stops the app), logs, Report a problem,
  Threads, cancellable install queue, .apkm; splash at the window's size with a ready event. the other runtime's
  PowerShell/WPF panel: device start/stop only, no per-app stop, no log viewer, install not cancellable,
  launch through powershell + dalvikvm + system server, splash polls every 200 ms.
- Failure: the splash shows the reason, Show log, Report a problem; the other runtime a message box.
- Troubleshooting: minidumps (last three per app), stub-hit counts at exit, Threads view; the other runtime none.
- Idle: nothing runs with Google services off; broker exits 60 s after the last app; the other runtime keeps its device
  600 s. AOT worker at idle priority.
- Input: gamepad, Ctrl+wheel/touchpad pinch; the other runtime neither.
- F-Droid store tab with SHA-256 checks; interrupted-update recovery; one copy of bundles.
- Google: real Play services/Play (higher ceiling, not yet proven) vs microG + Aurora.
- Real commercial Israeli apps run on real Windows.

the other runtime better here: UI Automation accessibility bridge, TSF text input + touch keyboard, per-APK icons in
Explorer, proxy settings, one window per task, panel test suite.
