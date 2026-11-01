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
| `NativeActivity` in the manifest | A game whose whole UI is native code (many engines) | Not yet | None |
| Video playback, `MediaPlayer`, `ExoPlayer` | Video and native audio paths | Not yet: video does not play; MediaPlayer, OpenSL ES and AAudio are missing. Sound from `AudioTrack` and `SoundPool` (WAV, Ogg, MP3) works | None |
| A `WebView` in the app | Web content inside the app | Not yet | None |
| The camera, location | Hardware the PC may not have | Not yet | None |
| Sign in with Google, in-app purchases, Google Maps | Google Play services | Not there: sign-in, purchases and maps that need it do not work. The rest of the app may | Use the app's own account or an open-source build without Play services (F-Droid) |
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
| Privacy Friendly Sudoku | 101 (`org.secuso.privacyfriendlysudoku`) | F-Droid | Boots | Boots headless on every CI run (`windows-poc.yml`): DEX conversion, `Application.onCreate`, the first activity resumed and 30 frames drawn | every CI run | |
| Klooni | 860 (`dev.lonami.klooni`) | F-Droid | Works | libGDX: OpenGL ES from Java on ANGLE; draws and takes input | `windows/PORTING.md`, M2 | |
| Tensy | 2 (`se.voxelmanip.tensy`) | F-Droid | Works | SDL 3: OpenGL ES from the app's own native code through a SurfaceView; draws and takes input | `windows/PORTING.md`, M2 | |

The automated compatibility sweep (`windows/test/compat/apps.txt`, run by
`compat-sweep.yml`) boots about 40 F-Droid apps headless on every pull
request and reports which draw their first screen; its report is an artifact
of the run. The corpus, without native libraries: Coffee, the Privacy Friendly
apps (Sudoku, Notes, To-Do, Dicer, 2048, Memory, Tape Measure, Activity
Tracker), Markor, Editor, Diary and Currency (Bill Farmer), Music (Muntashir),
Noice, OpenTracks, Loop Habit Tracker, OpenTasks, QuickDic, Vanilla Music,
SatStat, PassAndroid. With x86_64 native libraries: Chess (Cocode), Tuner,
Simple Markdown, Game of Life Wallpaper, Game Counter, NextPush, Todo,
MicroREPL, TriPeaks, Klooni, Alex vs Bus, Tensy, Shader Editor, NetGuard,
KeePassDX (libre), AnkiDroid. A sweep result says the app booted, not that it
is usable: a row above is worth more.

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
