**Win.apk 1.0 beta.** The first public beta. Win.apk runs Android apps on Windows 10/11 (x64) as ordinary Windows programs: each app gets its own window, Start menu entry and notifications, with no emulator and no virtual machine. It is an unofficial port of [Mac.apk](https://github.com/kksimp/Mac.apk).

> Not code-signed yet: SmartScreen may warn. Click **More info**, then **Run anyway**.

## Download
- **`Win.apk-Setup.exe`**: the installer (per user, no administrator rights). Recommended.
- **`Win.apk-windows-x64.zip`**: portable; unzip it and run `Win.apk.exe`.

## New in 1.0
- **Android's own code for what you see.** Apps now run on Android 14's own app, view, resource and graphics layers (from the Android Open Source Project), so layouts, text, dialogs and toasts look and behave as on a phone.
- **Drawn on the GPU, as on Android.** Every window is drawn by Android's own renderer (HWUI with Skia) and composed on your graphics card, through ANGLE on Direct3D 11.
- **A new Store.** The Store item opens Win.apk's own store app inside the Win.apk window: search Google Play's catalogue and install free apps. It replaces the F-Droid catalogue of earlier versions, and updates itself with Win.apk's daily check.
- **Apps are kept apart as on Android.** With Google services on, an app reaches another app's services, data and messages only where Android would allow it (same signer, or a permission the app was granted).
- **Your Google account stays private.** The account's token is encrypted for your Windows account (DPAPI) and never handed to apps; each app gets only a short-lived token of its own.
- **One behaviour for everyone.** Win.apk has no hidden switches: what you install is what runs.
- **Faster starts.** The installer prepares the Java classes every app loads, so even the first start of an app is quicker.
- Screen readers (Narrator, NVDA) read apps' views; on a touchscreen, a text field opens Windows' touch keyboard.

## Also
- Install `.apk`, `.apkm`, `.xapk` and `.apks` files by double-click or drag and drop.
- An app that comes in parts installs whole, as on Android: drop its loose `.apk` files together, or its bundle; a part on its own is refused with the reason, and the installed app is left as it was.
- Apps that ship only 64-bit ARM code are translated for your PC in the background, at the lowest priority.
- Your PC's hardware for apps: webcam, microphone, location, Bluetooth, game controllers, and your Windows folders.
- **Experimental:** Google's own Play services and Play Store, with a guided setup (Settings > Google account > **Use Google services (experimental)**).
- English and Hebrew, light and dark.

## Known limitations
- Apps with only 32-bit ARM (`armeabi-v7a`) code do not run.
- No in-app purchases, Play Integrity or Vulkan; no sensor (accelerometer, gyroscope or other) is reported.

[Getting started](https://github.com/israel2176/win.apk/blob/main/docs/getting-started.md) · [What works](https://github.com/israel2176/win.apk/blob/main/docs/APPS.md) · [User guide](https://github.com/israel2176/win.apk/blob/main/docs/USER_GUIDE.md) · [עברית](https://github.com/israel2176/win.apk/blob/main/README.he.md). Found a problem? Use **Report a problem** in Win.apk, or [open an issue](https://github.com/israel2176/win.apk/issues/new/choose) ([how](https://github.com/israel2176/win.apk/blob/main/docs/troubleshooting.md#reporting-a-problem)).
