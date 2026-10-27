# Win.apk

**Run Android apps on Windows 10 and 11 (x64), as ordinary Windows programs.**
No emulator and no virtual machine: an app's Java code runs on a real Java VM,
its x86_64 native libraries load directly, and it draws through ANGLE on
Direct3D 11.

<div dir="rtl" lang="he">

**בעברית:** Win.apk מריצה אפליקציות Android ב-Windows 10 ו-11 (x64) כתוכנות Windows רגילות, בלי אמולטור.
הממשק זמין גם בעברית, מימין לשמאל: הוא נפתח בעברית כש-Windows בעברית, ואפשר לבחור שפה בהגדרות.

</div>

> **Early preview.** Many Java/Kotlin apps already run; native-heavy apps and
> games are partly there. See [what works](#what-works) before you try.

## Getting started

1. Download `Win.apk-Setup.exe` from the [Releases](../../releases) page and run
   it. It installs for your user, without administrator rights. (Or unzip
   `Win.apk-windows-x64.zip` and start `Win.apk.exe`.)
2. Drag an `.apk` onto the Win.apk window, or double-click it in Explorer.
   Bundles (`.apkm`, `.xapk`, `.apks`) work too. The first install of an app
   takes up to a minute.
3. Double-click the app in the library, or use its Start menu or desktop
   shortcut.

Java and ANGLE are included; nothing else needs to be installed. Apps from
[F-Droid](https://f-droid.org) are a good place to start: free, open source
and usually Java-only.

## The library

`Win.apk.exe` is where apps are installed and opened.

- **Drop zone** to install apps, with a window that shows each install's
  progress and its log as it is written.
- **App grid** with the apps' own icons, search as you type, and a dot
  under every app that is running.
- **Right-click an app** for Get Info, a desktop shortcut, its files and log,
  updating it from a newer APK, reinstalling it or removing it.
- **Log viewer** that follows a log live, with copy, Notepad and folder
  buttons.
- **Settings:** language (English or Hebrew, right to left), light or dark
  theme, the phone's screen size and orientation, `.apk` file association and
  desktop shortcuts.

In an app, the mouse is your finger and the wheel scrolls. Android's Back is
Escape, Alt+Left or the mouse's back button.

## What works

| Works | Not yet |
|---|---|
| Java/Kotlin apps, in their own window, with mouse, wheel and keyboard | Apps with ARM-only native code (`lib/arm64-v8a` only) |
| OpenGL ES from Java (`GLSurfaceView`, libGDX) and from native code (SDL games) | `NativeActivity` games |
| Apps that ship `lib/x86_64` native libraries | MediaPlayer, OpenSL ES and AAudio |
| Sound from Java (AudioTrack, SoundPool: WAV, Ogg, MP3) | Video, camera, WebView, location |
| Files, SharedPreferences, SQLite, clipboard, file pickers, game controllers | Google Play services |

Why the split: Android phones are ARM, Windows PCs are x64. Java code and
x86_64 libraries run directly; ARM-only native code needs binary translation,
which is the last step of the plan.

## Where things are

| What | Where |
|---|---|
| Installed apps | `%LOCALAPPDATA%\Win.apk\library` |
| App data | `%LOCALAPPDATA%\Win.apk\android` |
| Logs | `%LOCALAPPDATA%\Win.apk\logs` (`last-run.log`, `install-<app>.log`) |

Removing an app keeps what it saved. Delete `%LOCALAPPDATA%\Win.apk` to start
from scratch.

## Reporting a problem

[Open an issue](../../issues/new) with the app's name and version, what
happened, and `last-run.log` (Logs in the library, or the folder above). The
most useful lines are `native not implemented`, `not implemented on Windows
yet`, `Exception` and `FATAL`.

## Building from source

Clang only, on Windows ([MSYS2](https://www.msys2.org/) CLANG64) or cross-compiled
on Linux and run under Wine:

```sh
pacman -S mingw-w64-clang-x86_64-{clang,cmake,ninja}
cmake -S windows -B build -G Ninja && cmake --build build
```

[`windows/README.md`](windows/README.md) covers the full build, running an APK
from the command line, the tests and the release workflow.
[`windows/PORTING.md`](windows/PORTING.md) holds the design, the findings and
the milestone plan.

## Repository layout

| Path | What it is |
|---|---|
| `windows/` | Win.apk itself: the ELF loader and Bionic shim, the JNI bridge, the Windows host (window, input, ANGLE, audio), the runtime, the library app (`windows/launcher/`), the installer and the tests |
| `runtime/` | The Android framework in Java and the shared native sources, from Mac.apk; built for Windows by `windows/scripts/build_java.sh` |
| `scripts/build_framework_res.sh` | Builds the platform resource APK |
| `.github/workflows/` | Checks on pull requests, and the release workflow (run by hand) |

## Credits and license

Win.apk started as a port of [Mac.apk](https://github.com/kksimp/Mac.apk) by
Kaleb Simpson / Voltare, and its Android runtime (`runtime/`) comes from there.
It is not affiliated with or endorsed by the Mac.apk project. The Mac.apk name
and logo belong to their owners ([`TRADEMARKS.md`](TRADEMARKS.md)).

Licensed under the [AGPL-3.0](LICENSE). Third-party components and their
licenses are listed in [`THIRD_PARTY_NOTICES.txt`](THIRD_PARTY_NOTICES.txt).
