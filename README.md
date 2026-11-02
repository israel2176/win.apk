# Win.apk

**Run Android apps on Windows 10 and 11 (x64) as ordinary Windows programs.**

[![Latest release](https://img.shields.io/github/v/release/israel2176/win.apk?include_prereleases&label=release&color=6c5ce7)](../../releases)
[![Platform: Windows 10/11 x64](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-0078d4)](#requirements)
[![License: AGPL-3.0](https://img.shields.io/badge/license-AGPL--3.0-blue)](LICENSE)
[![Star the repository](https://img.shields.io/github/stars/israel2176/win.apk?style=flat&label=stars&color=f1c40f)](../../stargazers)

Win.apk installs Android apps (`.apk`, `.apkm`, `.xapk`, `.apks`) and runs
each one in its own window, with its own Start menu entry. There is no
emulator and no virtual machine: an app's Java code runs on a standard Java
VM, its native libraries load into the same process, and its graphics go
through ANGLE on Direct3D 11.

Win.apk is an unofficial port of [Mac.apk](https://github.com/kksimp/Mac.apk)
to Windows x64. It is not affiliated with or endorsed by the Mac.apk project.

> **Early preview.** Only a few apps have been used by hand, and most features
> have not yet been tried with real apps on a real PC. See
> [What works](#what-works) and [`docs/APPS.md`](docs/APPS.md) before you try
> an app.

![The Win.apk library: a drop zone for installing apps above a grid of installed apps](docs/images/library.png)

## Contents

- [The problem it solves](#the-problem-it-solves)
- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Install](#install)
- [First run](#first-run)
- [What works](#what-works)
- [Where your data lives](#where-your-data-lives)
- [Getting help](#getting-help)
- [Documentation](#documentation)
- [Contributing](#contributing)
- [Credits](#credits)
- [License](#license)

## The problem it solves

Android apps are built for phones: ARM processors, a Linux kernel, Android's
own C library (Bionic) and its Java framework. A Windows PC has none of these.
The usual answer is an emulator or a virtual machine running a whole Android
system, which is heavy, slow to start, and keeps the apps in a separate box.

Win.apk takes the other route, the one Mac.apk took on Apple Silicon: it
re-creates the Android application environment on top of Windows. Each app
becomes a normal Windows process with a normal window, a taskbar icon and a
Start menu shortcut, and it shares the user's Downloads, Pictures and other
folders.

## How it works

| Stage | What happens |
|---|---|
| **Install** (once per app) | The APK or bundle is unpacked into the layout an Android device uses. Its DEX bytecode is converted to Java class files. Its icon and name are extracted for the library. ARM-only native libraries start being translated to x64 in the background. |
| **Java code** | Runs on a standard HotSpot JVM (bundled Eclipse Temurin 21) against a reimplementation of the Android framework in Java (from Mac.apk). |
| **Native code** (`.so` files) | x86_64 libraries are loaded by Win.apk's own ELF loader and call a Windows implementation of Android's C library (the Bionic shim). Calls between the Microsoft and System V calling conventions are bridged by generated thunks. ARM64-only libraries are translated to C and compiled with clang for the PC's CPU; until the translation is ready they run on an ARM64 JIT. |
| **Window and devices** | A Win32 window per app. OpenGL ES on ANGLE (Direct3D 11, with software fallbacks), audio on WASAPI, web content on WebView2, webcams and media decoding on Media Foundation, location from Windows, file pickers, clipboard, game controllers. |
| **Permissions** | Apps ask for permissions as on Android, in a Windows dialog. The answers are signed and stored per app. |

[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) explains each layer.

## Requirements

- Windows 10 or 11, 64-bit (x64). Windows on ARM is untested.
- A GPU with Direct3D 11 is best. Without one, a software renderer takes over.
- For web content inside apps and for Google sign-in: the Microsoft Edge
  WebView2 Runtime, which Windows 11 and most Windows 10 PCs already have.
- Disk space for Win.apk (it bundles a Java runtime, ANGLE and a clang
  compiler) and for the apps you install.

Nothing else needs to be installed.

## Install

1. Download `Win.apk-Setup.exe` from the newest [release](../../releases).
2. Run it. It installs for your user only, into
   `%LOCALAPPDATA%\Programs\Win.apk`, and needs no administrator rights.
3. Windows SmartScreen may warn that the installer is not signed. Click
   **More info**, then **Run anyway**.

The installer adds Win.apk to the Start menu, makes `.apk` files open with it,
and speaks English or Hebrew according to the Windows display language.

**Portable:** download `Win.apk-windows-x64.zip` instead, unzip it anywhere,
and start `Win.apk.exe`.

**Uninstall:** Windows Settings > Apps > Installed apps > Win.apk. Your apps
and their data stay in `%LOCALAPPDATA%\Win.apk`; delete that folder to remove
everything.

## First run

1. Open Win.apk. It shows your library, empty at first, and the **Store**
   tab with F-Droid's free and open-source apps.
2. Add an app: drop an APK on the window, pick one in the Store and click
   **Install**, or double-click an `.apk` in Explorer. The first install of an
   app converts its code, which takes up to a minute.
3. Double-click the app to open it. In the app, the mouse is your finger,
   the wheel scrolls, and Escape is Android's Back.

The [user guide](docs/USER_GUIDE.md) covers everything else: the Store,
per-app actions, settings, orientation and window size, Hebrew, permissions,
the Google account, logs and troubleshooting.

## What works

Win.apk is a preview. Most of what it can do has been written and passes
automated tests on Windows, but has not yet been used with real apps on real
PCs. The table says how far each part has been checked:

- **Used:** a person ran an app that needs it and it worked
  ([`docs/APPS.md`](docs/APPS.md) has the apps).
- **Boots:** apps that need it draw their first screen in the automated
  sweep, headless; nobody has used them.
- **Untested:** the code is there and its unit tests pass on Windows, but no
  app has been run with it on a real PC. Expect it to fail; an issue with the
  log helps most here.

| Part | Status | Notes |
|---|---|---|
| Java and Kotlin apps in their own window, mouse, wheel and keyboard | Used | Klooni, Tensy |
| OpenGL ES from Java (`GLSurfaceView`, libGDX) | Used | Klooni |
| OpenGL ES from native code (SDL), `lib/x86_64` libraries | Used | Tensy |
| Installing and starting ordinary apps | Boots | The sweep of about 90 F-Droid apps; most draw their first screen, which does not mean they are usable |
| Apps with only `lib/arm64-v8a` libraries (ARM translated to x64) | Boots | A few apps in the sweep; slow until the translation is ready |
| `NativeActivity` games | Boots | |
| Sound: AudioTrack, SoundPool, MediaPlayer audio, AAudio, the microphone | Untested | Unit tests decode WAV, Ogg and MP3 and play them |
| Video: `MediaPlayer`, `VideoView`, ExoPlayer (H.264 + AAC) | Untested | Added in [#60](https://github.com/israel2176/win.apk/pull/60); the unit test plays a one-second clip. Not in the v0.6.0 download |
| Notifications as Windows notifications | Untested | Added in [#59](https://github.com/israel2176/win.apk/pull/59). Not in the v0.6.0 download |
| Bluetooth (paired devices, RFCOMM, LE, GATT client) and Wi-Fi state | Untested | Added in [#61](https://github.com/israel2176/win.apk/pull/61); never run against a real radio. Not in the v0.6.0 download |
| Native crash handlers, `dlclose`, minidumps | Untested | Added in [#62](https://github.com/israel2176/win.apk/pull/62). Not in the v0.6.0 download |
| WebView and Custom Tabs on WebView2 | Untested | |
| Camera, location, clipboard, file pickers, game controllers | Untested | |
| Permission dialogs | Untested | |
| Google sign-in and the Play services stand-ins | Untested | |
| Files, SharedPreferences, SQLite, shared storage in your Windows folders | Boots | Apps in the sweep that store data start |

**Not there at all:** 32-bit ARM only apps (`lib/armeabi-v7a`); Vulkan;
OpenSL ES from native code (no sound); the NDK media API (`AMediaCodec`),
`MediaExtractor`, Opus/Vorbis/FLAC through `MediaCodec`; real sensors (they
report a still, flat phone); in-app purchases, Play Integrity and the Maps
SDK's map; connecting the PC to a Wi-Fi network; Bluetooth pairing,
discovery, advertising, the GATT server, audio and HID profiles; notification
action buttons.

If something marked Used or Boots does not work for you, or something marked
Untested does, please [open an issue](../../issues/new/choose) or add a row to
[`docs/APPS.md`](docs/APPS.md). The [architecture document](docs/ARCHITECTURE.md)
says where each part is in the code.

## Where your data lives

| What | Where |
|---|---|
| Win.apk itself | `%LOCALAPPDATA%\Programs\Win.apk` (installer) or the folder you unzipped |
| Your copy of each APK, with the app's name and per-app choices | `%LOCALAPPDATA%\Win.apk\library` |
| Each app's converted code, icon and name | `%LOCALAPPDATA%\Win.apk\apps\<app>` |
| What the apps save (the Android file system) | `%LOCALAPPDATA%\Win.apk\android` |
| Logs | `%LOCALAPPDATA%\Win.apk\logs` |
| Settings | `HKEY_CURRENT_USER\Software\Win.apk` |
| Start menu shortcuts | Start > Win.apk Apps |

The [user guide](docs/USER_GUIDE.md#where-things-are) has the full list.

## Getting help

1. Look in [`docs/APPS.md`](docs/APPS.md) for the app or the kind of problem.
2. Open **Logs** at the bottom of the library and read the end of
   `last-run.log`; the [troubleshooting section](docs/USER_GUIDE.md#troubleshooting)
   says what the common lines mean.
3. [Open an issue](../../issues/new/choose) with the "An app does not work"
   template: the app's name, version and source, the exact steps, a screenshot,
   and the whole log. Hebrew is fine.

Security problems: see [`SECURITY.md`](SECURITY.md), not a public issue.

## Documentation

| Document | For whom | What it covers |
|---|---|---|
| [`docs/USER_GUIDE.md`](docs/USER_GUIDE.md) | Users | Installing apps, the Store, settings, permissions, the Google account, logs, troubleshooting, the command line |
| [`docs/APPS.md`](docs/APPS.md) | Users | Known issues by kind of app, and apps people have tried |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Developers and the curious | How Win.apk works: each layer, an install and a run step by step, the data layout, the build and CI |
| [`windows/README.md`](windows/README.md) | Developers | Building, running and testing from source; every change made to the upstream `runtime/` tree |
| [`windows/PORTING.md`](windows/PORTING.md) | Developers | Design decisions, technical findings, known gaps and the milestone plan |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Contributors | Reporting problems, adding apps to the list, sending code |

## Contributing

Report how an app runs, add a row to [`docs/APPS.md`](docs/APPS.md), or send
code: [`CONTRIBUTING.md`](CONTRIBUTING.md) has the rules (behave like Android,
no app-specific code, Windows code in `windows/`). If Win.apk is useful to
you, a star on the repository helps other people find it.

## Credits

Win.apk started as a port of [Mac.apk](https://github.com/kksimp/Mac.apk) by
Kaleb Simpson / Voltare, and its Android runtime (`runtime/`) comes from
there. Win.apk is not affiliated with or endorsed by the Mac.apk project; the
Mac.apk name and logo belong to their owners ([`TRADEMARKS.md`](TRADEMARKS.md)).

It also builds on [ANGLE](https://chromium.googlesource.com/angle/angle),
[Eclipse Temurin](https://adoptium.net/), [LLVM](https://llvm.org/) clang,
[dynarmic](https://github.com/azahar-emu/dynarmic) and the other components
listed in [`THIRD_PARTY_NOTICES.txt`](THIRD_PARTY_NOTICES.txt).

## License

[AGPL-3.0](LICENSE), like Mac.apk. Anyone who distributes a build, or offers a
modified version over a network, must provide the complete corresponding
source.
