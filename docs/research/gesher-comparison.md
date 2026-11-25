# Win.apk and the other runtime: what each can give the other (2026-11-23)

The sections below compare code only. To compare the two running, see
[Running both](#running-both-comparisonyml) at the end.

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
  openat only cwd/absolute. **Imported 2026-11-24:** all of these are in Win.apk's raw table, and the
  stubs above are real (ptrace's TRACEME now EACCES, as SELinux answers on a device: neither Win.apk's
  old 0 nor the other runtime's 0-then-EPERM); see open-issues.md. the other runtime's NTFS-EA `user.*` xattrs
  (compat/posix/src/xattr.cc) were not ported: Win.apk's xattr calls still answer ENOTSUP.
- **Exit hang: likely in the other runtime by another path.** compat/elf/tls.cc ArenaDestructor (FLS callback) takes
  g_mutex with no exiting check; a thread killed at ExitProcess while holding g_mutex (dlopen, thread start)
  blocks it. Win.apk's elf_tls.cpp process_exiting guard covers exactly this.
- **WebView:** the other runtime serves android_asset/android_res on https://other-file.example via deferred
  WebResourceRequested; Win.apk (since 21cfd06) serves them on https://winapk-file.example the same way, android_res included; both run shouldInterceptRequest
  on a worker pool.
- **OpenSL ES:** the other runtime real (players, recorders); Win.apk stub. Since: `native_opensles.cpp` is Win.apk's own, written from the spec and AOSP's behaviour (players pulled by the mixer, http players, decoding to a queue).
- **Media:** the other runtime software libvpx/dav1d/libhevc/libavc, opus, libwebm, MP4/WebM/Ogg/ADTS muxers.
  Win.apk decodes avc/hevc/vp8/vp9/av1/mp4v/h263/mpeg2 through Media Foundation only (needs the PC's
  extensions), encoders are MF's, own MP4 writer.
- **Update check:** Win.apk does verify size and sha256 digest (earlier claim wrong); missing vs the other runtime: no
  digest -> size only, no product-version check, pre-releases not skipped, a cached copy re-run on size only.
- **GL extensions:** Win.apk listed only callable extensions (safer; hid ANGLE_/CHROMIUM_); the other runtime exports
  all, and a declared function ANGLE lacks aborts on call. Taken (November 2026): Win.apk now forwards
  `gl2ext_angle.h`'s functions as the other runtime does and lists an extension only when every function resolves in
  the ANGLE loaded (`host/gl/guest_gl_extensions.cpp`).
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

the other runtime better here: TSF text input + touch keyboard, a proxy per app
(its panel and command line), one window per task, panel test suite.

Imported since: per-APK icons in Explorer (`windows/shell/`, a thumbnail handler that parses the file
itself where the other runtime's icon handler runs its runtime per file), and apps follow Windows' proxy
(`java.net.useSystemProxies`; before it they went direct whatever Windows said). A per-app proxy was not
added: Windows' proxy setting is the one place, as a phone's network settings are.
Screen readers were first imported from the other runtime's UI Automation bridge, then replaced (November 2026)
by Win.apk's own, written from Microsoft's and AOSP's documentation alone (`windows/host/uia_tree.h`,
`uia_provider.cpp`, `HostAccessibility.java`, `aosp_views/AccessibilityTree.java`): snapshots sent on
the views' own accessibility events instead of polling frames, events only while a client listens, the
Text, ExpandCollapse and Selection patterns, names as TalkBack speaks them.

## Running both (`comparison`)

`.github/workflows/comparison` (Actions, "comparison", Run workflow; by hand only, gates
nothing) runs the same apps in both runtimes on one `windows-latest` runner per app. Inputs: `apps`
(packages of `.github/compare/apps.txt`, empty for all, or `package:versionCode|latest` for others),
`seconds` per run (60), `display` (360x640 dp) and `other` (a release tag, `latest`, or `ci`).

- **Win.apk**: the branch's newest CI `runtime-stage` (`.github/probe/stage.sh`, as the app probe),
  with its own JRE, run as the launcher runs it: `winapk-runtime --apk <apk> --data <dir>`, after
  `--install-only`; `WINAPK_PHONE_SIZE=<display>`.
- **the other runtime**: `other-setup-<version>.exe` and `SHA256SUMS.txt` of its newest public release (what
  `publish-release.yml` publishes, `tools/ci/publish_release.py`; GitHub's "latest", no drafts or
  pre-releases), checked against each other (`.github/compare/other_fetch.sh`). With no release, the
  same two files from the `other-setup` artifact of its newest green `ci.yml` run on `main` (the
  installer its e2e job tests). Installed as its e2e installs it (`/VERYSILENT /SUPPRESSMSGBOXES
  /NORESTART`, default tasks), then `other-app install <apk>` and `other-app launch <package> --window`
  (the Start menu shortcut's command, `tools/package/other-app.ps1`), `OTHER_DISPLAY=<display>@160`.
  Nothing of the other runtime is built or changed.
- **Apps** (`.github/compare/apps.txt`, F-Droid, version and SHA-256 pinned): Java views (Editor,
  Currency), Kotlin views (Fossify Notes), Compose (Jerboa), Flutter (LocalSend), React Native (Jitsi
  Meet), a WebView shell (Jellyfin), JNI C on a Canvas (Puzzles), libGDX (Klooni), SDL2 (Tensy),
  NativeActivity (PPSSPP), arm64-only (OpenSudoku). Editor, Currency and Puzzles are in the other runtime's own
  e2e set with the same SHA-256. F-Droid has no bundles, so bundle installs are not compared.

Per app the job installs, then runs **cold** (nothing of the runtime running: the other runtime's device shut
down after the install started it) and **warm** (again right after the cold run closed: the other runtime's
device still up, as within its 600 s idle timeout; Win.apk with the class-data archive and arm64
translations its first run left). Which runtime goes first alternates from app to app, so neither
always meets the colder disk cache. Both are on C: (the other runtime installs per user there and wants the APK
on its drive), both get the same window size, the same seconds and the same measuring script,
`.github/compare/measure.ps1`. The table (`compare.md`, `compare.json`, the job summary; per app
screenshots and logs in `compare-<package>`) has, cold / warm:

| Column | What |
|---|---|
| Installs | The install command's exit 0 (the other runtime: and its "installed <package>" line, as its e2e checks) and its seconds |
| Draws (cold) | At the end of the cold run, the app's window (the largest visible window of the run's processes) read back with PrintWindow and from the screen (the screen's read only while no window of another process, or a console, covers the window: `screen_covered` in the JSON), `measure.ps1`'s `CmpWin.Judge`: "yes" from 0.5% of pixels away from the most common color (the other runtime's e2e threshold), or for a sparse screen from 0.4% of neighbouring pixel pairs differing with 32 colors or more away from the background (anti-aliased text and icons; a blank window, a hairline or a lone progress ring stays "NO"). The cell shows the share; `edges_pct` and `colors` are in the JSON. "exited" when the runtime ended by itself |
| First frame | Seconds from the launch command to the runtime's own first-frame log line (Win.apk `[win.apk] first frame presented`; the other runtime `first frame drawn (N presented)`) |
| First non-blank | Seconds to the first 1-second sample whose window read back non-blank: the same measure for both, coarser |
| Peak MB | The highest sum, over 1-second samples, of the working sets of every process whose image is in the runtime's folder and of their descendants (WebView2's included). the other runtime's device processes count; its waiting launcher (the `com.other.app.Launcher` VM, which a Start menu launch detaches and ends) is kept apart as `peak_launcher_mb` in the JSON. A descendant counts only when it was created after its parent (a recycled process id once summed the whole runner: LocalSend's 3358 MB in run 45513190823). The JSON also has `processes` (each process's image, role from its command line, own peak, first and last second seen) and `at_peak` (the sum at the peak by role, at `peak_at_s`), shown under the table as "Memory at the peak" |
| Idle CPU % | The same processes' CPU time over the last 30 s of the run, in % of one core; empty when the app ended before |
| Closes on WM_CLOSE | The launch command ended within 20 s of WM_CLOSE to the app's windows |

Why the rule is more than the share: in run 45513190823 Fossify Notes was "NO (0.31%)" in both
runtimes, yet both had drawn its real first screen, an empty note ("Insert text here") under a
toolbar of three icons with no title, as a device shows it on first start (`MainActivity` hides the
tab strip while only the first note exists; its toolbar has no title): 0.31% of the pixels, but
0.58% edges and 90 (Win.apk) / 145 (the other runtime) colors. The same run's the other runtime warm screen reads were a
console of log lines over the app (29% "drawn" from the screen, 0.32% from PrintWindow); they no
longer count. Re-judged by the new arithmetic, every other app of that run keeps its verdict; only
the other runtime's Jellyfin, "yes" from its screen reads alone (PrintWindow read 0%: its "WebView not
supported" dialog, then a crash box, over a uniform window), now depends on whose windows those
were, which a new run's `screen_covered` tells.

What stays unequal: an app whose manifest fixes landscape turns in Win.apk and not in
the other runtime (it does not rotate windows yet); the other runtime's cold start boots its device, Win.apk has none; the
runner's screen is at 100%, so 1 dp is 1 pixel in both. Nobody uses the apps: a drawn first screen
is not a working app.
