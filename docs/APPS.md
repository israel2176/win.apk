# Apps and known issues

What is known about running particular apps in Win.apk, and the problems that
apply to whole kinds of apps. Check here before
[opening an issue](../../../issues/new/choose); add what you learn, as a pull
request or in an issue (see [CONTRIBUTING.md](../CONTRIBUTING.md), "Add an app
to the list"). If an app you care about runs well, a ⭐ star on the repository
tells us so.

- [Known issues by kind of app](#known-issues-by-kind-of-app)
- [Apps](#apps)
  - [Adding a row](#adding-a-row)

**Status words used below:** *Works* (the main screens are usable), *Partly*
(opens, but something in the notes does not work), *Boots* (draws its first
screen in an automated run; nobody has used it), *Does not start* (installs,
then shows no window or crashes at once), *Does not install*.

## Known issues by kind of app

These come from how Win.apk is built; they are not specific to one app. The
first column says what to look for in the app or its log.

| Sign | What it is | Status | Workaround |
|---|---|---|---|
| The APK has only `lib/armeabi-v7a` | 32-bit ARM native code only | Not supported; the app does not run | Look for an x86_64 or arm64-v8a build of the same app (F-Droid often has one) |
| The APK has only `lib/arm64-v8a` | 64-bit ARM native code only | Runs: the libraries are translated to x64 in the background by `winapk-aot` with clang. The app starts at once on an ARM JIT and is slow until the translation of each library is ready; large libraries are translated only where the app's code ran | Give it a minute on first run. Set `WINAPK_AOT_CLANG` to another `clang.exe` if the bundled one is missing |
| `NativeActivity` in the manifest | A game whose whole UI is native code (many engines) | Runs: the app gets its window, input queue and configuration as on Android | |
| Video playback, `MediaPlayer`, `VideoView`, `ExoPlayer` | Video decoding on Media Foundation | `MediaPlayer` and `VideoView` play a local file or an http(s) URL into the app's `SurfaceView` or `TextureView`, with the sound in step, seeking and looping: MP4/MOV/MKV with H.264 and AAC always, HEVC/VP9/AV1 with Microsoft's codec packs. ExoPlayer and other `MediaCodec` players decode H.264 (and the pack codecs) with AAC or MP3 audio. Not yet: the NDK media API (`AMediaCodec`), `MediaExtractor`, and Opus/Vorbis/FLAC audio through `MediaCodec` (such a stream stays silent: pick one with AAC) | None |
| A game in native code has no sound | OpenSL ES from native code | Not yet: the OpenSL ES engine reports itself unsupported, so engines that use it are silent. AAudio works | None |
| A `WebView` in the app, a Custom Tab | Web content inside the app | Runs on Microsoft Edge WebView2 (needs the WebView2 Runtime, which Windows 11 and most Windows 10 PCs have). Without it, links open in the system browser | Install the WebView2 Runtime |
| The camera, location | The PC's webcam and location service | Run: webcams through Media Foundation (a virtual camera when there is none), location from Windows' location service, both behind Windows' privacy settings (Settings > Privacy) | Turn the Windows privacy switch on |
| The accelerometer and other sensors | Hardware a PC usually lacks | The sensors exist but report a still, flat phone (gravity on the accelerometer, zero rotation), so tilt controls do nothing | None |
| Sign in with Google, Play licence checks, push messages | Google Play services | Run through Win.apk's own in-process stand-ins (as on the Mac). Sign-in opens Google's page in a WebView2 window; Settings > Google account adds the account | None |
| In-app purchases, Play Integrity, Google Maps | Play Store features | Not there: purchases report billing unavailable, there is no attestation, and the Maps SDK lays out a blank map. The rest of the app may work | Use an open-source build without Play services (F-Droid) where there is one |
| `not implemented on Windows yet` in the log | A libc function the app's native code needs that the Bionic shim does not have yet | Open an issue with the log: adding one is usually small | None |
| `native not implemented` in the log | A piece of the Android host that is still missing | Open an issue with the log | None |
| `[win.apk] cannot load <library>` in the log | A native library the loader could not load (packed, encrypted, or for another ABI) | Open an issue with the log and the APK's `lib/` folders | None |
| The app looks like a phone turned on its side, or opens narrow when you wanted it wide | Apps open in landscape by default, the shape of a PC screen; an app whose manifest fixes portrait opens portrait | Expected | Right-click the app > "Open in portrait" or "Open in landscape", for that app only; Settings > Screen > Orientation for all of them. An app that requires one orientation always gets it, and its menu entry says so |
| Windows Defender or SmartScreen warns about Win.apk or an app | Win.apk writes and then runs translated code, and the preview builds are not code-signed | Known; the preview builds are not signed yet | Allow it |

## Apps

One row per app and version that someone actually ran; say what does not
work in the notes. Newest test first within an app.

| App | Version | Source | Status | Notes | Tested | Issue |
|---|---|---|---|---|---|---|
| Privacy Friendly Sudoku | 101 (`org.secuso.privacyfriendlysudoku`) | F-Droid | Boots | Boots headless on every CI run (`ci.yml`, Runtime tests): DEX conversion, `Application.onCreate`, the first activity resumed and 30 frames drawn | every CI run | |
| Klooni | 860 (`dev.lonami.klooni`) | F-Droid | Works | libGDX: OpenGL ES from Java on ANGLE; draws and takes input | `windows/PORTING.md`, section 8 (window, input and rendering) | |
| Tensy | 2 (`se.voxelmanip.tensy`) | F-Droid | Works | SDL 3: OpenGL ES from the app's own native code through a SurfaceView; draws and takes input | `windows/PORTING.md`, section 8 (window, input and rendering) | |

The automated compatibility sweep (`windows/test/compat/apps.txt`) boots about
90 F-Droid apps headless and reports which draw their first screen and what
blocks the rest. It runs when asked for (`ci.yml` by hand, on four machines),
and its report is an artifact of the run. Every pull request that touches the
runtime boots ten of them instead, one of each kind (`smoke.txt`), and fails
when one no longer draws. The corpus is
pinned to one version per app and covers: apps without native libraries
(Coffee, the Privacy Friendly apps, Markor, Bill Farmer's Editor, Diary and
Currency, Noice, OpenTracks, Loop Habit Tracker, OpenTasks, QuickDic, Vanilla
Music, SatStat, PassAndroid and others); apps with x86_64 libraries (Klooni,
Tensy, Alex vs Bus, Shader Editor, NetGuard, KeePassDX, AnkiDroid and
others); games in native code (Shattered Pixel Dungeon, Mindustry, Unciv,
DroidFish, PPSSPP, RetroArch, Minetest, Dolphin); Kotlin and Compose apps
(the Fossify apps, Pachli, Jerboa, Breezy Weather, Tasks, Auxio, LibreTube,
F-Droid, Aurora Store, Aegis, Thunderbird, NewPipe, AntennaPod);
cross-platform frameworks (LocalSend on Flutter, Jitsi Meet on React Native,
Jellyfin and AndBible on WebView); and native-heavy apps (VLC, Organic Maps,
Sky Map, ConnectBot, Trime, AnySoftKeyboard, MuPDF, Termux-style terminals,
Gadgetbridge, OpenVPN, Open Camera, OpenKeychain, DAVx5, AdAway, Key Mapper,
Tremotesf). Where F-Droid builds per ABI the x86_64 build is pinned, and a
few arm64-v8a-only builds exercise the ARM translation. A sweep result says
the app booted, not that it is usable: a row above is worth more.

The arm64 translation was also run (2026-11-06) on the arm64-v8a libraries
of 28 F-Droid apps, headless, twice on the JIT alone and twice translated:
VLC, Jitsi Meet (React Native), LocalSend and Lichess (Flutter), Godot
editor and Revengate (Godot), OsmAnd, Telegram X, Minetest, Tensy, TIC-80,
Tux Paint, Wesnoth, Simon Tatham's Puzzles, Just Player, LibreOffice
Viewer, Kodi, Freedoom, MuPDF, Open Sudoku, GCompris, SimpleX. Each drew
the same on both engines (the translated runs with 5-30% less memory and up
to half the time in arm64 code). PPSSPP, Linphone and Emacs crash at start
in `winapk-runtime.exe` itself, GNU Backgammon aborts, AAAAXY (Go) stops
with "unaligned sysNoHugePageOS" and Firefox (Fennec) draws nothing: the same
on the JIT, so not the translation.

### Adding a row

Copy this line into the table, newest test first for the same app:

```
| App name | version (`package.name`) | F-Droid / APKMirror / the app's site | Works / Partly / Does not start | what works, what does not, in one line | YYYY-MM-DD | #123 |
```

- **Version:** as the app's own About screen or its F-Droid page shows it,
  and the package name, so two apps with the same name are not confused.
- **Status:** one of the words at the top of this page.
- **Tested:** the date, and the Win.apk version if it is not the latest
  release.
- **Issue:** the issue number for anything that does not work, so the row and
  the fix stay linked.
