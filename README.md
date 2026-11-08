<div align="center">

### 🇮🇱 [לתיעוד בעברית — לחצו כאן](README.he.md)

</div>

# Win.apk

**Run Android apps on Windows as ordinary Windows programs. No emulator, no virtual machine.**

[![Latest release](https://img.shields.io/github/v/release/israel2176/win.apk?include_prereleases&label=release&color=6c5ce7)](https://github.com/israel2176/win.apk/releases)
[![Windows 10/11 x64](https://img.shields.io/badge/Windows-10%20%7C%2011%20x64-0078d4)](docs/getting-started.md#what-you-need)
[![License: AGPL-3.0](https://img.shields.io/badge/license-AGPL--3.0-blue)](LICENSE)
[![Sweep: 90 of 92 apps start](https://img.shields.io/badge/F--Droid%20sweep-90%2F92%20start-2ea44f)](docs/APPS.md#the-automated-sweep)

**English** · [עברית](README.he.md)

Win.apk installs Android apps (`.apk`, `.apkm`, `.xapk`, `.apks`) and runs
each one in its own window, with its own Start menu entry, taskbar icon and
Windows notifications. It is an unofficial Windows port of
[Mac.apk](https://github.com/kksimp/Mac.apk).

> [!NOTE]
> **Public preview (v0.8).** Many apps run well; others run in part or not
> yet. Check [Compatibility](docs/APPS.md) before you rely on an app, and
> please [report](docs/troubleshooting.md#reporting-a-problem) what you find.

![The Win.apk library: twelve installed Android apps, with Library, Store, Sign in and Settings in the left pane](docs/images/library.png)

## Highlights

- **Apps are Windows programs.** Each app has its own resizable window, Start
  menu shortcut and taskbar icon; its notifications are Windows notifications.
- **Install the way you expect.** Double-click or drag an `.apk` or a split
  bundle (`.apkm`, `.xapk`, `.apks`), or install from the built-in
  [F-Droid](https://f-droid.org) Store.
- **Your PC's hardware.** Webcams, the microphone, Windows location, Bluetooth
  devices you paired, Wi-Fi information, Xbox controllers, your Downloads and
  Pictures folders, and the clipboard.
- **ARM apps too.** Apps built only for 64-bit ARM phones are translated for
  your processor in the background, at the lowest priority.
- **Google sign-in built in,** and, experimentally, Google's own Play services
  and Play Store.
- **English and Hebrew,** light and dark; in Hebrew everything is laid out
  right to left.

| The Store (F-Droid) | Dark theme | Apps from the automated sweep |
|---|---|---|
| ![The Store tab: F-Droid apps as cards with Install buttons](docs/images/store.png) | ![The library in the dark theme](docs/images/dark.png) | ![First screens drawn by six F-Droid apps: Shattered Pixel Dungeon, Thunderbird, Mindustry, Lexica, DAVx5, Loop Habit Tracker](docs/images/apps.png) |

## How it differs from an emulator

Emulators and virtual machines (BlueStacks, the Android Studio emulator,
Windows Subsystem for Android) boot a whole Android system and run the apps
inside it. Win.apk re-creates only what an app needs, on top of Windows:

- **Nothing to boot and nothing in the background.** No Android system runs;
  memory goes only to the apps you open.
- **No Hyper-V or virtualization**, nothing to turn on in the BIOS.
- **Real Windows integration**: one window per app, Alt+Tab, snapping,
  Windows notifications, and your own folders as the phone's shared storage.
- **Real hardware through Windows' own APIs**, not emulated devices.

The trade-off is honest: an emulator runs more apps today.
[How Win.apk works](docs/how-it-works.md) explains it in plain terms.

## What works

The full table, checked against the code, is on
[Compatibility](docs/APPS.md#what-works). In short:

| ✅ Works | ⚠️ Partly | ❌ Not yet |
|---|---|---|
| Java and Kotlin apps | Apps with only arm64 native code: slower until translated | Apps with only 32-bit ARM code (`armeabi-v7a`) |
| x86_64 native code | `MediaCodec`: H.264 with AAC/MP3 audio | Vulkan |
| OpenGL ES 1.1-3.2 on Direct3D 11, `NativeActivity` games | Bluetooth: paired devices, RFCOMM, LE scans and GATT client | OpenSL ES (some native games are silent) |
| Sound (AudioTrack, SoundPool, AAudio output), microphone | Wi-Fi: information, no connecting | NDK media API (`AMediaCodec`) |
| Video (`MediaPlayer`, `VideoView`) on Media Foundation | Notifications: no action buttons | Real sensors (they report a phone lying still) |
| WebView and Custom Tabs on WebView2 | Google's real Play services and Play Store (experimental) | In-app purchases, Play Integrity, the Maps SDK's map |
| Webcam, location, clipboard, file pickers, Xbox controllers | Input: one finger, no multi-touch | |
| Permissions, Google sign-in, push messages (FCM) | | |

**Measured, not promised:**

- In the last automated sweep, **90 of 92** open-source F-Droid apps of every
  kind started and drew their first screen on Windows
  ([details](docs/APPS.md#the-automated-sweep)). Starting is not the same as
  every screen working.
- ARM apps use less memory once translated: at peak, Jitsi Meet 516 MB
  against 742 MB on the JIT, Telegram X 412 MB against 521 MB
  ([ARM apps](docs/arm-apps.md#how-arm-only-apps-run)).

## Install

**Requirements:** Windows 10 (1903 or later) or Windows 11, 64-bit, on an
Intel or AMD processor; about 500 MB of disk; a Direct3D 11 GPU (a software
renderer is the fallback); the Microsoft Edge WebView2 Runtime for web pages
and Google sign-in (Windows 11 and most Windows 10 PCs have it). No
administrator rights.

1. Download **`Win.apk-Setup.exe`** from the
   [latest release](https://github.com/israel2176/win.apk/releases/latest)
   (or `Win.apk-windows-x64.zip` for a portable copy).
2. Run it. **SmartScreen will warn** because the preview is not code-signed:
   click **More info**, then **Run anyway**.
3. Open Win.apk and install an app from the **Store**, or double-click an APK.

[Getting started](docs/getting-started.md) walks through it with screenshots.

### Which build of an app to install

Android apps may contain code for one kind of processor. When you download an
app yourself:

1. **x86_64**, or a build with no native code (`noarch`), is best: it runs
   directly.
2. **A bundle that includes an x86_64 split** is just as good: Win.apk picks it.
3. **arm64-v8a** works, translated, slower on its first runs.
4. **armeabi-v7a only** (32-bit ARM) does not run.

The built-in Store chooses for you. [Why](docs/arm-apps.md#which-build-to-install)

## Documentation

| | |
|---|---|
| [Getting started](docs/getting-started.md) | Requirements, installing Win.apk, your first app |
| [Compatibility](docs/APPS.md) | What works, partly works and is missing; the app sweep; apps people tried |
| [User guide](docs/USER_GUIDE.md) | Every screen and setting |
| [ARM apps](docs/arm-apps.md) | Which build to install, and how ARM code is translated |
| [Google account and services](docs/google-services.md) | Signing in, and setting up Google's Play services and Play Store |
| [Troubleshooting](docs/troubleshooting.md) | Common problems, reading a log, reporting a problem |
| [FAQ](docs/faq.md) | Short answers to common questions |
| [How it works](docs/how-it-works.md) | Win.apk explained, and how it compares with an emulator |
| [Architecture](docs/ARCHITECTURE.md), [building](windows/README.md) | For developers |

Hebrew pages: [`docs/he/`](docs/he/).

## Status and limitations

Win.apk is a preview. Expect apps that do not
start, missing features ([the list, by importance](docs/APPS.md#needs-improvement-by-importance))
and changes between releases. Two things to know before you install apps:

- **There is no sandbox between apps.** Each app runs with your Windows
  account's rights, like any program. Install only apps you trust
  ([SECURITY.md](SECURITY.md)).
- **Google's integrity checks and payments are not available** and will not
  be faked, so apps that require them (many banking and payment apps, in-app
  purchases) will not fully work.

## Contributing

The most useful help right now is trying apps: report what fails with
**Report a problem** in Win.apk, or add a row to the
[compatibility list](docs/APPS.md#adding-a-row). Code is welcome too:
[CONTRIBUTING.md](CONTRIBUTING.md) has the rules (behave like Android, no
app-specific code). Reports in Hebrew are welcome.

## Credits

- **[Mac.apk](https://github.com/kksimp/Mac.apk)** by Kaleb Simpson
  ([@kksimp](https://github.com/kksimp), Voltare) and its contributors: Win.apk
  started as a port of it, and its Android runtime (`runtime/`: the framework
  re-implementation, the DEX converter, the Play services stand-ins) comes from
  there. Win.apk is **not affiliated with or endorsed by** the Mac.apk project;
  the Mac.apk name and logo belong to it ([TRADEMARKS.md](TRADEMARKS.md)).
- **The Android Open Source Project**: framework resources, apksig, the
  Apache HTTP legacy library and the fonts.
- **What Win.apk ships**: [Eclipse Temurin](https://adoptium.net/) (OpenJDK 21),
  [ANGLE](https://chromium.googlesource.com/angle/angle) and SwiftShader (from
  Electron's build), [LLVM/clang](https://llvm.org/),
  [dynarmic](https://github.com/azahar-emu/dynarmic), the
  [Microsoft Edge WebView2 SDK](https://www.nuget.org/packages/Microsoft.Web.WebView2)'s
  loader, [Bouncy Castle](https://www.bouncycastle.org/),
  [libwebp](https://chromium.googlesource.com/webm/libwebp),
  [SQLite](https://sqlite.org/), [stb_vorbis](https://github.com/nothings/stb)
  and [zlib](https://zlib.net/). Each one's licence is in
  [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
- **[F-Droid](https://f-droid.org)**, whose catalog the Store shows; the apps
  belong to their authors.

## License

Win.apk is free software under the [GNU AGPL v3.0](LICENSE), like Mac.apk.
Anyone who distributes a build, or offers a modified version over a network,
must provide its complete source. Mac.apk's contributor licence agreement
applies only to changes sent upstream to Mac.apk, and its commercial licence
is upstream's offer for Mac.apk; neither covers Win.apk's own code. Android is a trademark of Google LLC; Win.apk is not
affiliated with Google or Microsoft.
