# Win.apk: notes for Claude

Runs Android apps on Windows x64 as normal Windows programs: Java on HotSpot,
x86_64 `.so` files loaded by our own ELF loader over a Bionic shim, graphics on
ANGLE (D3D11). Started as a port of Mac.apk; its `runtime/` comes from there.
Read this file instead of re-mapping the repository; open
`docs/ARCHITECTURE.md` (how the layers work, an install and a run step by
step), `docs/USER_GUIDE.md` (what the user sees and every setting),
`windows/README.md` (build, tests, env vars, the list of `runtime/` changes)
or `windows/PORTING.md` (design decisions, findings and the milestone plan,
~1100 lines: grep it, don't read it whole) only when a task needs them. The
docs must match the code: a change to a flag, a file name, a setting or what
works is also a change to the document that names it.

## Layout

| Path | What |
|---|---|
| `windows/src/`, `windows/include/winapk/` | ELF loader, linker, ELF TLS, `%fs` patch, ABI thunks, guest JNI. `include/winapk/aot_stamp.h`: a library's translation stamp (`<lib>.so.aot.stamp`), its kinds as an enum, the line's one parser and writer and the rules the loader, the worker and the launcher go by. `src/bionic/` + `mini_bionic.cpp`: the Bionic libc shim (~900 functions, `sysv_abi`) |
| `windows/host/` | The runtime host: `runtime_main.cpp` (winapk-runtime.exe: args `--apk --data --jdk --lib --install-only`, `--gms <cmd>` runs `com.macapk.gms.GmsCli` with no app, `--system` the system broker (upstream's `SystemServer`), `--aot-translate` the background arm64 translation, `--verify-apk <file>` an app file's package and whether Google signed it: `windows/java/com/winapk/tools/ApkCheck.java`), `win32_window.cpp` (window, input; `pointer_input.h` holds the input decisions that need no window: touch contacts as Android pointers, the Ctrl+wheel pinch, which characters go with a key), `native_*.cpp` (the Java natives, one file per class: `native_cocoa.cpp` = window/phone size/icon, `native_audio.cpp` = AudioTrack/SoundPool/MediaPlayer audio on WASAPI, `native_aaudio.cpp` = AAudio (real), `native_opensles.cpp` = OpenSL ES (a stub: the engine reports unsupported), `native_media.cpp` = only the NDK media key strings (no `AMediaCodec`), `native_media_video.cpp` + `video_player.cpp` + `video_decode.cpp` = MediaPlayer/VideoView and MediaCodec video on Media Foundation (`HostVideo`, `NativeMedia`), `native_media_codec.cpp` + `media_encode.cpp` + `mp4_writer.cpp` = `HostMedia`: MediaExtractor's demuxer (`video_decode.h`'s `Demuxer`), MediaCodec's H.264 and AAC encoders, MediaMuxer's MP4 writer (no Media Foundation), under MediaRecorder too, `native_activity.cpp` = NativeActivity + the NDK input queue and AConfiguration, `native_webview.cpp` + `webview2_host.cpp` = WebView on WebView2, `native_camera.cpp`, `native_location.cpp`, `native_radios.cpp` (Wi-Fi, the paired devices, the default-route adapter for ConnectivityManager), `native_bluetooth.cpp` (RFCOMM sockets, LE scans, the GATT client), `native_notifications.cpp` = notifications as Windows toasts under the app's AppUserModelID (the launcher writes it on the shortcuts), `native_signin.cpp` = the Google sign-in window on WebView2, `binder_win.cpp` = cross-process Binder over named pipes (`MacApkBinderTransport`, `WinProcesses`; `--system` runs the system broker for "Use Google services"), clipboard, picker, gamepad, ICU, images, photos, wallpaper), `gl/`, `user_folders.cpp` (shared storage junctions), `cds.cpp` + `cds_util.cpp` (class-data sharing: drops `--patch-module` when the JRE has the patches baked in, the shared framework archive `<data>\cds\`, `--cds-prepare`/`--cds-dump`) |
| `windows/launcher/launcher_main.cpp` | `Win.apk.exe`, the library app (one file, GDI+ custom-drawn). See below. Its state machines are headers beside it, with no lock and no Windows call so that `test/launcher_state_test.cpp` drives them: `app_runs.h` (an app's run: starting, running, ready, ended; `RunTable`, under `g_proc_lock`), `installs.h` (an install: queued, installing, committed, ended; `InstallTable`, under `g_installs_lock`; Cancel is refused from commit on), `updates.h` (the update check, what is known of the newest release, the installer's download; `UpdateTracker`, the library's thread only) |
| `windows/java/` | Windows-only Java (`com/winapk/tools/AppMeta.java`: writes `icon.png` + `label.txt` at install; `java_base_patch/`: Win.apk's own `java.base` overlay, `java/io/WinNTFileSystem.java` nests a drive-absolute child under its parent as Android does; with `java/io/File.java`, `sun/nio/fs/WindowsPath.java` and `jdk/internal/util/WinApkPaths.java` it keeps Android paths (`/data/user/0/<pkg>`) and opens the host file they name; compiled with upstream's `runtime/java_base_patch`) |
| `windows/scripts/` | `build_java.sh` (builds `runtime/java` with a stock JDK; bcprov unsigned), `stage_runtime.sh` (runnable `bin/`+`lib/`+`clang/`), `bake_jre.sh` (the shipped `jre/`: Temurin 21's jmods with the patch classes baked in, trimmed, its CDS archive), `merge_classlists.py` (`windows/java/winapk-framework.classlist` from the smoke apps), `fetch_angle.sh`, `fetch_webview2.sh`, `stage_clang.sh`, `build_framework_res.sh` |
| `docs/` | `APPS.md` (Compatibility: the one ✅/⚠️/❌ table of what works, checked against the code; the sweep; known issues; apps tried), `USER_GUIDE.md` (every user-visible behaviour and setting), `getting-started.md`, `arm-apps.md` (which build to install, the translation), `google-services.md`, `troubleshooting.md` (log lines, reporting), `faq.md`, `how-it-works.md` (for users), `he/` (Hebrew getting started, Google services, troubleshooting, FAQ; `README.he.md` is the Hebrew front page), `ARCHITECTURE.md` (the layers, install and run step by step, data on disk, CI), `images/` (screenshots, taken under Wine) |
| `windows/test/` | Unit tests (`*_test.cpp`; `binder_test.cpp` + `java/com/macapk/loader/BinderTransportTest.java` the cross-process Binder and broker test), guest libs (`guest/`), Wine runners (`run_*_wine.sh`), `compat/` (app sweep: `apps.txt`, `sweep.py`; `smoke.txt` the ten apps every PR boots) |
| `windows/package/` | Installer (`winapk.iss`, Inno Setup; English or Hebrew by the Windows display language, UTF-8 with BOM), `README.txt` (shipped with `LICENSE.txt` and `THIRD_PARTY_NOTICES.txt`) and `RELEASE_NOTES.md` (short: the release page's text; details belong in `docs/`) |
| `windows/CMakeLists.txt` | All Windows targets. Pulls `runtime/src/native_methods.cpp` (GL bridges) and `native_sqlite.cpp` from upstream |
| `runtime/java/` | The Android framework in Java (~3000 files) + `com/macapk/loader/` (Boot, ActivityHost, package manager...). Names say macapk: that is upstream's, keep them |
| `runtime/src/`, `runtime/translator/` | Upstream native sources; Windows uses only the two files above |
| `scripts/build_framework_res.sh` | The platform resource APK (called by `windows/scripts/`) |
| `site/` | The project's web page on GitHub Pages (`index.html`, `style.css`, `site.js`: every text in English and Hebrew, the language from `?lang=`, the visitor's switch or the browser's; Hebrew is right to left). The download buttons are `releases/latest/download/...` links, so they need a release that is not a pre-release. `pages.yml` adds the screenshots from `docs/images/` and the launcher's icon when it deploys |
| `.github/workflows/` | `ci.yml` (one pipeline on PRs and by hand: what changed, then one Windows build with ccache whose exes and staged runtime (`stage/`) feed unit tests, runtime tests, a 10-app smoke test that fails when one no longer draws, and packaging side by side; the whole app sweep (4 shards) only by hand; a push to `main` only builds, to warm the cache), `windows-release.yml` (release, by hand only), `pages.yml` (deploys `site/` to GitHub Pages on a push to `main` that touches it), `app-probe.yml` (by hand only, not a test: the F-Droid apps of its list, each a minute in a window on the staged runtime of the branch's newest CI build (`.github/probe/stage.sh`; `main`'s when the branch has none), with screenshots, the log and CPU use as artifacts; `.github/probe/fdroid_fetch.py` fetches the APK) |

Data at run time: `%LOCALAPPDATA%\Win.apk\` (`library\` APKs + `.label`, `apps\<name>\`, `android\` (the Android filesystem: the app sees it by device names -- its data dir `/data/user/0/<pkg>`, shared storage `/storage/emulated/0` -- which the host maps here, `windows/src/bionic/paths.cpp` for native code and the java.base overlay's `jdk.internal.util.WinApkPaths` for java.io/java.nio), `logs\run-<name>.log` (each app's last run; `.1.log` the one before; `last-run.log` is a hard link to the newest), `logs\install-<name>.log`, `logs\google-setup.log` (the Google services setup's checks), `logs\crash-<name>-<time>.dmp` (minidumps, the last three per app), `store\` the Store tab's F-Droid catalog.tsv + icons, `installed_apps.json` the registry, `appops\` the signed permission grants: app-ops.json, signing-key.pub, signing-key.p256 under DPAPI, `gms\` the Google account and its tokens: gms_state.properties, the `accounts.present` marker the launcher checks, `photo-<account>.img` the account's picture (saved at sign-in, drawn in the account item at the foot of the left pane), `aot-cache\` whole arm64 translations shared between apps, `cds\` the framework class-data sharing archive `framework-<key>.jsa` (+ `.ok`, `.fails`, `dump.log`, `patch-digests.txt`; host/cds.h), `cache\` the runtime's caches (`android-system-fonts\`, `fonts\`, `fwview\`, `monet\`, `apk-verify\` the APK signature verdicts, `applinks\<package>.properties` the App Link verdicts per domain, fetched only when a sign-in needs one), `run\<package>.pid` each running app's run lock, `staging\` the install helper's, with Google services on `app_uids.properties`, `system_ce\`, `installer-of-record.properties`, `run\`, `logs\system.log` and `logs\services\` (the system broker's), `updates\` the update check's `releases.json` (+ `.etag`) and a downloaded `Win.apk-Setup-<version>.exe`, `apps\<name>\notifications\` the large icons of notifications on show, `apps\<name>\aot-guard.txt` the arm64 translation's crash guard: two crashes in a row with translated code loaded turn translation off for the app, `host/aot_guard.h`). Settings: `HKCU\Software\Win.apk` (`Theme`, `Language`, `PhoneSize`, `Landscape` (absent = 1, landscape; 0 = portrait), `DesktopShortcut`, `ShortcutVersion` (below `kShortcutVersion`, now 2, the launcher rewrites every app's shortcuts once at start, and deletes the ones older builds made for Google's three apps), `UserFolders` (absent = 1: shared storage is the user's Windows folders; the runtime reads it too), `AotBackground` (absent = 1: Settings > Apps > "Optimize ARM apps in the background"; 0 = no background arm64 translation worker, `aot_background_on`), `AutoUpdate` (absent = 1: the daily update check), `LastUpdateCheck` (Unix seconds of the last check GitHub answered), `GoogleServices` (absent = 0; Settings > Google account > "Use Google services (experimental)": the runtime sets `MACAPK_BROKER=1`), `WindowWidth`, `WindowHeight`, `WindowMaximized` (the library window, in DIPs)). Per app, beside its APK: `library\<name>.label`, `.package` (package, versionCode, versionName, and the launched activity's manifest `screenOrientation`), `.orientation` (`portrait` / `landscape`, the right-click override for that app only; wins over `Landscape`, but an orientation the manifest fixes wins over both), `.window` (dp, the window size once the user resizes it; wins over the size setting, dropped when the orientation is flipped). Runtime env: `WINAPK_READY_EVENT=<handle>` (the launch handshake: an inherited event the runtime sets once the app's window has painted its first frame, or a second start brought the running copy forward; `win32_window.h`), `WINAPK_START_WINDOW=<hwnd>` (the launcher's starting window: the app's window opens over it), `WINAPK_PHONE_SIZE=WxH` (dp), `WINAPK_RESIZABLE=0` (the window letterboxes instead of resizing the app), `WINAPK_HEADLESS=1`, `WINAPK_HEADLESS_FRAME`, `WINAPK_HEADLESS_EXIT_AFTER_FRAMES`, `WINAPK_AOT=0` (arm64 code on the JIT only), `WINAPK_CDS=0` (no class-data sharing: the old `--patch-module` flags), `WINAPK_EMBED_PARENT=<hwnd>` (the launcher's Google Play panel: the app's window becomes a borderless child filling it; `win32_window.cpp`).

## The launcher (`windows/launcher/launcher_main.cpp`)

Sections in order: paths, settings (registry), Windows integration (file types,
shortcuts), library, running/installing (`start_runtime` gives a child only the
handles it lists; threads post `kInstalled`, `kInstallStarted`, `kRunReady`,
`kRunExited`), the store (WinHTTP, F-Droid's index-v1.json -> `store\catalog.tsv`,
icon and download threads posting `kStore*`), software updates (GitHub's
releases list, the newest non-draft release by version number, its
`Win.apk-Setup.exe`; threads posting `kUpdate*`), theme + drawing helpers, state,
layout, painting (the InfoBar replaces a status bar; `info()` may carry one link, e.g. "What's new"), dialogs, actions (install
with Cancel, launch, Remove with "also remove accounts and data saved by the app"), pages
(Settings, App details, a Store app's details, the Google services setup (`show_gms_setup`: watches Downloads, installs only Google-signed copies of the three Google apps; Google Play in the pane leads there or opens the Play Store): `Row`/`Section`/`Page`, drawn in
the content layer by `paint_page`, `open_page`; Settings > Updates is
`update_sections`, with the check's state in `g_update`, an `UpdateTracker` (`updates.h`)), install window, log window
(RichEdit), launch splash, drag and drop (`IDropTarget`), main `wnd_proc`, the
installed-apps registry, the install helper (`install`/`uninstall` verbs),
permissions (`namespace appops`: the signed app-ops store and the
`runtime-permission`, `special-access`, `app-details` questions the runtime asks
through, upstream's macapk-installhelper verbs, drawn by `run_prompt`, for every
dangerous group: `kGroupRows` must match
`runtime/java/com/macapk/loader/RuntimePermissionGroups.java`; the details
page's Permissions rows, which stop a running app on revoke), single instance
(`forward_to_running`, `maybe_quit`), `wWinMain`.

- One process serves the library, the install window and the splashes. A
  second `Win.apk.exe` hands its files (or "show the library") to the first
  in a `WM_COPYDATA` and exits; Explorer's one-process-per-file starts meet on
  the `Local\WinApk-launcher` mutex. The library window always exists but is
  shown only when asked for: an APK from Explorer shows only the install
  window, `--run` only the app's starting window. The hidden process quits when nothing is
  left (`maybe_quit`).
- Launching: `launch()` shows the starting window at once (`show_splash`: a
  normal window with the app's name, icon and AppUserModelID, of the size the
  app's window opens at, `app_window_size` over `host/window_fit.h`, the
  runtime's own arithmetic); `run_thread` passes it as `WINAPK_START_WINDOW`,
  so the runtime opens the app's window over it, and `WINAPK_READY_EVENT`,
  and waits on that or the process. Ready brings the app's window forward and
  closes the starting window; an exit first turns it into the error with
  Show log. The run's state is
  `g_runs` (`app_runs.h`), changed only through `begin_run`, `stop_run`,
  `expect_run_end`, `finish_run` and `run_thread`; what its end says is
  `RunEnd::kind()`. Installs likewise: `g_installs` (`installs.h`), through
  `begin_install`, `cancel_install`, `finish_install`, `install_thread` and
  `install_into_library`; `g_jobs` is only the install window's record of the
  batch.
- One home per action: the tile's menu is Open, Stop, App details, Remove;
  everything else per app is on the App details page, its log too (Show log,
  in the log window; Ctrl+L opens the newest); Settings > Troubleshooting >
  Logs opens the logs folder; the failure states of the install window and the
  splash have Show log, and Report a problem as a link under their sentence
  (`report_problem`: asks in a `Prompt`, then the issue form prefilled by URL
  and the log selected in Explorer; nothing uploaded), which the details page
  has too. Don't add a second way to an action.
- Menus are the launcher's own (`run_menu`, `PopupMenu`: themed, a glyph per
  item, an app header), not `TrackPopupMenu`; only the Store's many-column
  category list and the log picker still use the system menu.
- Dialogs are the launcher's own too (`Prompt`, in the dialogs section:
  `run_dialog` for its questions, modal to the window asking; `run_prompt`
  for an app's, over the app's window), not TaskDialog or MessageBox: the
  accent button first, Enter on Cancel for anything destructive, Esc no answer.
- Points from screen coordinates (WM_NCHITTEST's) go through `client_point`:
  in a mirrored window ScreenToClient already mirrors x, as mouse messages do.
- An install's library name (`install_name`) is the package when the file
  name starts with one (`com.waze_5.26..._apkmirror.com.apkm` -> `com.waze`),
  else the file name up to its version; a package the library has keeps its
  app's name, so the file updates it.
- The version is `launcher.rc`'s VS_VERSION_INFO (`version()`): 0.1.0 unless
  CMake's `WINAPK_VERSION` (the release workflow passes its tag; `ci.yml` the newest
  release tag + `-dev.<run>`, which the update check counts as that release) writes
  `winapk_version.h`. The update check compares it with the newest release's tag.
- Settings > Google account asks `winapk-runtime --gms accounts|signin-window|signout`
  (`run_gms`, stdout captured, stderr in `logs\gms.log`); exit codes 0 done,
  1 error, 2 cancelled.
- Every user-visible string goes through `tr(L"English")` or
  `fmt(L"... {0} ...", {args})`; add the Hebrew to the table in `tr()`.
- Hebrew = RTL: windows get `WS_EX_LAYOUTRTL`, `buffered_paint` mirrors the
  layout, and `draw_text` / `draw_image` un-mirror text and pictures. Always
  draw text with `draw_text` and images with `draw_image`, never directly.
  Layout and hit-testing code stays left-to-right.
- Icons are glyphs from Segoe Fluent Icons / MDL2 Assets (`kGlyph*`).

## Build and check

- Toolchain: **clang only** (MSVC cannot emit System V calls). CI builds on
  `windows-latest` with MSYS2 CLANG64.
- Quick launcher-only compile check on Linux (no full CMake needed):
  `apt-get install g++-mingw-w64-x86-64-posix`, then
  `x86_64-w64-mingw32-windres windows/launcher/launcher.rc -O coff -o l.res` and
  `x86_64-w64-mingw32-g++-posix -std=c++17 -Wall -Wextra -Wno-missing-field-initializers -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -municode -mwindows -static windows/launcher/launcher_main.cpp l.res -o Win.apk.exe -lcomctl32 -lshell32 -lole32 -luuid -lgdiplus -ldwmapi -luxtheme -lgdi32 -lwinhttp -lbcrypt -lcrypt32`.
- Looking at the UI: `apt-get install wine64 xvfb xdotool`; Wine is in
  `/usr/lib/wine` (`wine64`). Run under `Xvfb :9`, screenshot with
  `import -window root`. Wine lacks Segoe fonts, so icon glyphs show as boxes.
  For the README screenshots (`docs/images/`), stand-in fonts were dropped into
  the prefix's `C:\windows\Fonts`: Open Sans instanced and renamed "Segoe UI" /
  "Segoe UI Semibold" (fontTools), and a "Segoe Fluent Icons" built from
  Fluent UI System Icons SVGs on the same code points (it needs name IDs 3, 4
  and 6 or Wine's GDI+ will not find it). Wine's GDI+ does no bidi, so Hebrew
  screenshots come out wrong: take those on Windows.
- Full build and tests: `windows/README.md`, "Build and run".

## Conventions

- Win.apk code goes in `windows/`. Touch `runtime/` only for a needed Windows
  hook, and add it to the list in `windows/README.md`.
- Match the surrounding style: C++17, 120 columns, comments say why.
- Don't add app-specific hacks; fix the framework, shim or host.
- PRs go to `main`; CI must be green. Releases only via the manual
  `windows-release.yml` run.
