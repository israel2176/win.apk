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
| `windows/src/`, `windows/include/winapk/` | ELF loader, linker, ELF TLS, `%fs` patch, ABI thunks, guest JNI. `src/bionic/` + `mini_bionic.cpp`: the Bionic libc shim (~520 functions, `sysv_abi`) |
| `windows/host/` | The runtime host: `runtime_main.cpp` (winapk-runtime.exe: args `--apk --data --jdk --lib --install-only`, `--gms <cmd>` runs `com.macapk.gms.GmsCli` with no app, `--aot-translate` the background arm64 translation), `win32_window.cpp` (window, input), `native_*.cpp` (the Java natives, one file per class: `native_cocoa.cpp` = window/phone size/icon, `native_audio.cpp` = AudioTrack/SoundPool/MediaPlayer audio on WASAPI, `native_aaudio.cpp` = AAudio (real), `native_opensles.cpp` = OpenSL ES (a stub: the engine reports unsupported), `native_media.cpp` = only the NDK media key strings (no `AMediaCodec`), `native_media_video.cpp` + `video_player.cpp` + `video_decode.cpp` = MediaPlayer/VideoView and MediaCodec video on Media Foundation (`HostVideo`, `NativeMedia`), `native_activity.cpp` = NativeActivity + the NDK input queue and AConfiguration, `native_webview.cpp` + `webview2_host.cpp` = WebView on WebView2, `native_camera.cpp`, `native_location.cpp`, `native_radios.cpp` (Wi-Fi, the paired devices), `native_bluetooth.cpp` (RFCOMM sockets, LE scans, the GATT client), `native_notifications.cpp` = notifications as Windows toasts under the app's AppUserModelID (the launcher writes it on the shortcuts), `native_signin.cpp` = the Google sign-in window on WebView2, clipboard, picker, gamepad, ICU, images, photos, wallpaper), `gl/`, `user_folders.cpp` (shared storage junctions) |
| `windows/launcher/launcher_main.cpp` | `Win.apk.exe`, the library app (one file, GDI+ custom-drawn). See below |
| `windows/java/` | Windows-only Java (`com/winapk/tools/AppMeta.java`: writes `icon.png` + `label.txt` at install; `java_base_patch/`: Win.apk's own `java.base` overlay, `java/io/WinNTFileSystem.java` nests a drive-absolute child under its parent as Android does, compiled with upstream's `runtime/java_base_patch`) |
| `windows/scripts/` | `build_java.sh` (builds `runtime/java` with a stock JDK), `stage_runtime.sh` (runnable `bin/`+`lib/`+`clang/`), `fetch_angle.sh`, `fetch_webview2.sh`, `stage_clang.sh`, `build_framework_res.sh` |
| `docs/` | `USER_GUIDE.md` (every user-visible behaviour and setting), `ARCHITECTURE.md` (the layers, install and run step by step, data on disk, CI), `APPS.md` (known issues by kind of app, apps tried), `images/` (README screenshots) |
| `windows/test/` | Unit tests (`*_test.cpp`), guest libs (`guest/`), Wine runners (`run_*_wine.sh`), `compat/` (app sweep: `apps.txt`, `sweep.py`; `smoke.txt` the ten apps every PR boots) |
| `windows/package/` | Installer (`winapk.iss`, Inno Setup; English or Hebrew by the Windows display language, UTF-8 with BOM), `README.txt` and `RELEASE_NOTES.md` shipped in the package |
| `windows/CMakeLists.txt` | All Windows targets. Pulls `runtime/src/native_methods.cpp` (GL bridges) and `native_sqlite.cpp` from upstream |
| `runtime/java/` | The Android framework in Java (~3000 files) + `com/macapk/loader/` (Boot, ActivityHost, package manager...). Names say macapk: that is upstream's, keep them |
| `runtime/src/`, `runtime/translator/` | Upstream native sources; Windows uses only the two files above |
| `scripts/build_framework_res.sh` | The platform resource APK (called by `windows/scripts/`) |
| `.github/workflows/` | `ci.yml` (one pipeline on PRs and by hand: what changed, then one Windows build with ccache whose exes and staged runtime (`stage/`) feed unit tests, runtime tests, a 10-app smoke test that fails when one no longer draws, and packaging side by side; the whole app sweep (4 shards) only by hand; a push to `main` only builds, to warm the cache), `windows-release.yml` (release, by hand only) |

Data at run time: `%LOCALAPPDATA%\Win.apk\` (`library\` APKs + `.label`, `apps\<name>\`, `android\`, `logs\run-<name>.log` (each app's last run; `.1.log` the one before; `last-run.log` is a hard link to the newest), `logs\install-<name>.log`, `logs\crash-<name>-<time>.dmp` (minidumps, the last three per app), `store\` the Store tab's F-Droid catalog.tsv + icons, `installed_apps.json` the registry, `appops\` the signed permission grants: app-ops.json, signing-key.pub, signing-key.p256 under DPAPI, `gms\` the Google account and its tokens: gms_state.properties, the `accounts.present` marker the launcher checks, `photo-<account>.img` the account's picture (saved at sign-in, drawn in the toolbar's account button), `aot-cache\` whole arm64 translations shared between apps, `staging\` the install helper's, `updates\` the update check's `releases.json` (+ `.etag`) and a downloaded `Win.apk-Setup-<version>.exe`, `apps\<name>\notifications\` the large icons of notifications on show, `apps\<name>\aot-guard.txt` the arm64 translation's crash guard: two crashes in a row with translated code loaded turn translation off for the app, `host/aot_guard.h`). Settings: `HKCU\Software\Win.apk` (`Theme`, `Language`, `PhoneSize`, `Landscape` (absent = 1, landscape; 0 = portrait), `DesktopShortcut`, `ShortcutVersion` (below `kShortcutVersion` the launcher rewrites every app's shortcuts once at start), `UserFolders` (absent = 1: shared storage is the user's Windows folders; the runtime reads it too), `InstallDetails` (the install window shows its log), `AutoUpdate` (absent = 1: the daily update check), `LastUpdateCheck` (Unix seconds of the last check GitHub answered), `WindowWidth`, `WindowHeight`, `WindowMaximized` (the library window, in DIPs)). Per app, beside its APK: `library\<name>.label`, `.package` (package, versionCode, versionName, and the launched activity's manifest `screenOrientation`), `.orientation` (`portrait` / `landscape`, the right-click override for that app only; wins over `Landscape`, but an orientation the manifest fixes wins over both), `.window` (dp, the window size once the user resizes it; wins over the size setting, dropped when the orientation is flipped). Runtime env: `WINAPK_READY_EVENT=<handle>` (the launch handshake: an inherited event the runtime sets once the app's window has painted its first frame, or a second start brought the running copy forward; `win32_window.h`), `WINAPK_PHONE_SIZE=WxH` (dp), `WINAPK_RESIZABLE=0` (the window letterboxes instead of resizing the app), `WINAPK_HEADLESS=1`, `WINAPK_HEADLESS_FRAME`, `WINAPK_HEADLESS_EXIT_AFTER_FRAMES`, `WINAPK_AOT=0` (arm64 code on the JIT only).

## The launcher (`windows/launcher/launcher_main.cpp`)

Sections in order: paths, settings (registry), Windows integration (file types,
shortcuts), library, running/installing (`start_runtime` gives a child only the
handles it lists; threads post `kInstalled`, `kInstallStarted`, `kRunReady`,
`kRunExited`), the store (WinHTTP, F-Droid's index-v1.json -> `store\catalog.tsv`,
icon and download threads posting `kStore*`), software updates (GitHub's
releases list, the newest non-draft release by version number, its
`Win.apk-Setup.exe`; threads posting `kUpdate*`), theme + drawing helpers, state,
layout, painting (the InfoBar replaces a status bar; `info()` may carry one link, e.g. "What's new"), dialogs, actions (install
with Cancel, launch, Remove with "also delete what the app saved"), pages
(Settings, App details, a Store app's details: `Row`/`Section`/`Page`, drawn in
the content layer by `paint_page`, `open_page`; Settings > Updates is
`update_sections`, with the check's state in `g_update`), install window, log window
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
  window, `--run` only the splash. The hidden process quits when nothing is
  left (`maybe_quit`).
- Launching: `launch()` shows the splash at once; `run_thread` passes
  `WINAPK_READY_EVENT` and waits on it or the process. Ready closes the splash;
  an exit first turns it into the error with Show log.
- One home per action: the tile's menu is Open, Stop, App details, Remove;
  everything else per app is on the App details page, its log too (Show log,
  in the log window; Ctrl+L opens the newest); Settings > Troubleshooting >
  Logs opens the logs folder; the failure states of the install window and the
  splash have Show log. Don't add a second way to an action.
- Menus are the launcher's own (`run_menu`, `PopupMenu`: themed, a glyph per
  item, an app header), not `TrackPopupMenu`; only the Store's many-column
  category list and the log picker still use the system menu.
- An install's library name (`install_name`) is the package when the file
  name starts with one (`com.waze_5.26..._apkmirror.com.apkm` -> `com.waze`),
  else the file name up to its version; a package the library has keeps its
  app's name, so the file updates it.
- The version is `launcher.rc`'s VS_VERSION_INFO (`version()`): 0.1.0 unless
  CMake's `WINAPK_VERSION` (the release workflow passes its tag) writes
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
