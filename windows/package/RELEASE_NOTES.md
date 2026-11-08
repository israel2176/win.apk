**First public preview.** Win.apk runs Android apps on Windows 10/11 (x64) as ordinary Windows programs: each app gets its own window, Start menu entry and notifications. It is an unofficial port of [Mac.apk](https://github.com/kksimp/Mac.apk).

> Preview build, not code-signed yet: SmartScreen may warn. Click **More info**, then **Run anyway**.

## Download
- **`Win.apk-Setup.exe`**: the installer (per user, no administrator rights). Recommended.
- **`Win.apk-windows-x64.zip`**: portable; unzip it and run `Win.apk.exe`.

## Highlights
- Install `.apk`, `.apkm`, `.xapk` and `.apks` files by double-click or drag and drop, or from the built-in F-Droid Store.
- Apps that ship only 64-bit ARM code are translated for your PC in the background, at the lowest priority.
- Your PC's hardware for apps: webcam, microphone, location, Bluetooth, game controllers, and your Windows folders.
- **Experimental:** Google's own Play services and Play Store, with a guided setup (Settings > Google account); Google Play opens inside the Win.apk window.
- English and Hebrew, light and dark.

## Known limitations
- Apps with only 32-bit ARM (`armeabi-v7a`) code do not run.
- No in-app purchases, Play Integrity or Vulkan; OpenSL ES sound is missing; sensors report a phone lying still.

[Getting started](https://github.com/israel2176/win.apk/blob/main/docs/getting-started.md) · [What works](https://github.com/israel2176/win.apk/blob/main/docs/APPS.md) · [User guide](https://github.com/israel2176/win.apk/blob/main/docs/USER_GUIDE.md) · [עברית](https://github.com/israel2176/win.apk/blob/main/README.he.md). Found a problem? Use **Report a problem** in Win.apk, or [open an issue](https://github.com/israel2176/win.apk/issues/new/choose) ([how](https://github.com/israel2176/win.apk/blob/main/docs/troubleshooting.md#reporting-a-problem)).
