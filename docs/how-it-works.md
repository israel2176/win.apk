# How Win.apk works

An explanation for users. The developer reference, layer by layer, is
[ARCHITECTURE.md](ARCHITECTURE.md).

## Not an emulator

The usual way to run Android apps on a PC is to run all of Android: an
emulator or a virtual machine boots a phone's operating system (BlueStacks,
the Android Studio emulator, Windows Subsystem for Android, Waydroid on
Linux), and the apps live inside it, in one big window or a separate world.

Win.apk does what [Mac.apk](https://github.com/kksimp/Mac.apk) does on the
Mac: it re-creates only what an *app* needs, on top of Windows. No Android
system boots, nothing runs in the background when no app is open, and each app
becomes a normal Windows program.

| | An Android emulator or VM | Win.apk |
|---|---|---|
| What runs | A whole Android system, then the apps inside it | Only the app, as a Windows process |
| Before the first app | Android boots | Nothing to boot |
| Memory | Reserved for the whole Android system | Only what each open app uses |
| Virtualization (Hyper-V, BIOS) | Usually required | Not used |
| Windows | One window for the whole phone, or a window per app drawn by the VM | A window per app: Start menu, taskbar, Alt+Tab, snapping |
| Files | A separate disk image | Your Downloads, Pictures, Documents, Music and Videos folders |
| Notifications | Inside Android | Windows notifications |
| Hardware | Emulated or passed through | The PC's own webcam, microphone, location, Bluetooth, Wi-Fi and controllers, through Windows' APIs |
| App coverage | Close to a phone's | Lower today: see [Compatibility](APPS.md) |

The last row matters: an emulator runs more apps today. Win.apk trades some
compatibility for apps that feel like part of Windows and cost nothing while
they are closed.

## What happens when you install an app

1. **The file is kept.** Win.apk copies the APK or bundle into its library,
   so you can delete the download. For a bundle it picks the parts this PC
   needs (its language, screen density and processor), as a phone does.
2. **The code is converted.** Android apps are compiled to Dalvik bytecode
   (DEX). Win.apk converts it once into Java bytecode, which a standard Java
   VM runs. This is why the first install takes up to a minute.
3. **The app gets its place in Windows.** Its icon and name go into the
   library, the Start menu and, if you like, the desktop.
4. **ARM code is translated.** If the app has native code only for ARM phones,
   Win.apk starts translating it for your processor in the background
   ([ARM apps](arm-apps.md)).

## What happens when you open it

- **The Java side** runs on a standard Java VM (Eclipse Temurin 21, included)
  against a re-implementation of Android's framework (the Activity, View,
  Resources... classes), which comes from Mac.apk.
- **The native side** (`.so` libraries in the APK) is loaded by Win.apk's own
  loader. Android libraries expect Android's C library and Linux's calling
  conventions; Win.apk provides a Windows version of that library and bridges
  the two conventions. ARM code runs translated or on an ARM JIT.
- **Graphics** go through ANGLE, which turns the app's OpenGL ES into
  Direct3D 11, the graphics API every Windows GPU supports.
- **Everything else** is Windows: WASAPI for sound, Media Foundation for video
  and webcams, WebView2 (Microsoft Edge) for web pages, the Windows location
  service, Windows' Bluetooth and Wi-Fi APIs, Windows notifications.

## Permissions and privacy

Apps ask for permissions as on Android, and you answer in a Win.apk window.
Your answers are stored per app and survive restarts and updates. Windows'
own privacy switches (camera, microphone, location) still apply on top.

There is one important difference from a phone: **there is no sandbox
between apps.** Each app runs with your Windows account's rights, like any
program you install. Install only apps you trust. See [SECURITY.md](../SECURITY.md).

Win.apk has no telemetry and uploads no logs or crash reports by itself. Its
own connections are GitHub (the daily update check, which can be turned off),
f-droid.org (the Store) and Google (when you sign in, or an app uses Google's
services). The apps you run talk to their own servers, as on a phone.

## Where things are kept

| What | Where |
|---|---|
| Win.apk | `%LOCALAPPDATA%\Programs\Win.apk`, or the folder you unzipped |
| Your apps and their data | `%LOCALAPPDATA%\Win.apk` |
| Logs | `%LOCALAPPDATA%\Win.apk\logs` |
| Settings | `HKEY_CURRENT_USER\Software\Win.apk` |

The [user guide](USER_GUIDE.md#where-things-are) has the full list.
