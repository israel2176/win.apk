Win.apk preview
===============

Win.apk runs Android apps on Windows 10 and 11 (x64) as ordinary Windows
programs: each app in its own window, with its own Start menu entry. It is
an unofficial port of Mac.apk and is not affiliated with the Mac.apk
project. This is an early preview.

Installing Win.apk
------------------
Win.apk-Setup.exe installs it for your user only (no administrator rights),
puts Win.apk in the Start menu and opens .apk files with it. Windows
SmartScreen may warn because the installer is not signed: click "More info",
then "Run anyway". Or unzip Win.apk-windows-x64.zip anywhere and start
Win.apk.exe from that folder.

Everything needed is included: Java (Eclipse Temurin 21, the jre folder),
ANGLE for graphics (bin\libEGL.dll, bin\libGLESv2.dll, Chromium's build;
licenses in bin\licenses) and the clang compiler that translates apps' ARM
code for your processor (the clang folder, licenses in clang\licenses). Web
pages inside apps and Google sign-in use the Microsoft Edge WebView2
Runtime, which Windows 11 and most Windows 10 PCs already have.

Adding and running apps
-----------------------
Open Win.apk: it shows your app library and the Store.
- Add an app: drag an .apk (or an .apkm, .xapk or .apks bundle) onto the
  window, click the drop zone or the "Add app" tile, or double-click the
  file in Explorer. The first install of an app converts its code, which
  takes up to a minute; a window shows the progress and the log.
- Or get one from the Store tab: the free and open-source apps of F-Droid
  (f-droid.org), usually Java-only and a good place to start. Search or
  pick a category, then click Install; the download is checked against
  F-Droid's index before it is installed. Update shows when F-Droid has a
  newer version of an app you have.
- Each app gets its own icon in the library, in the Start menu
  (Start > Win.apk Apps) and, unless turned off in the settings, on the
  desktop.
- Double-click an app to open it. Right-click for more: Get Info,
  Open in portrait / landscape, a desktop shortcut, its files and log,
  Update from APK, Reinstall and Remove. Typing searches the library.
- The gear opens the settings: theme, language (English or Hebrew), the
  phone's screen size and orientation, shortcuts, the Google account, and
  whether apps use your Windows folders. "Logs" at the bottom shows the
  logs as they are written.

In an app, the mouse is your finger and the wheel scrolls. Android's Back
is Escape, Alt+Left or the mouse's back button. Resize or maximize the
window and the app lays itself out for the new size. Close the window to
quit.

Apps ask for permissions as on Android, in a dialog over the app. Get Info
in the library, or the app's own App info screen, changes them later.

What works
----------
- Apps written in Java or Kotlin, with mouse, wheel and keyboard input.
- Games and apps that draw with OpenGL ES, from Java (GLSurfaceView,
  libGDX) or from their own native code (SDL games, NativeActivity apps).
  Graphics go through ANGLE on Direct3D 11; without a usable GPU, a
  software renderer takes over.
- Apps that ship x86_64 native libraries (lib/x86_64), loaded directly.
- Apps that only have 64-bit ARM native libraries (lib/arm64-v8a). Their
  ARM code is translated to x64 in the background (winapk-aot, compiled
  with clang for your CPU). The app starts at once on an ARM JIT, which is
  slower, and switches to the translated code as it becomes ready, so the
  first runs of a large app are the slow ones.
- Sound: AudioTrack, SoundPool and MediaPlayer audio from Java (WAV, Ogg,
  MP3, AAC and what Windows decodes), AAudio from native code, and the
  microphone.
- Saving data (files, SharedPreferences, SQLite). Downloads, Pictures,
  Documents, Music and Movies are your own Windows folders.
- Web pages inside apps (WebView) and Custom Tabs, on WebView2.
- Webcams, location (from Windows' location service), the clipboard, file
  open/save dialogs and game controllers.
- Google sign-in and the Play services apps use for licences, push
  notifications, Firebase Analytics and location. Signing in opens Google's
  own page in a window; Win.apk never sees the password. Settings > Google
  account adds or removes the account.
- English and Hebrew, laid out right to left in Hebrew.

What does not work yet
----------------------
- Apps that only have 32-bit ARM native libraries (lib/armeabi-v7a).
- Video playback (MediaPlayer video, ExoPlayer).
- OpenSL ES from native code: games whose engine uses it get no sound.
- Vulkan, and real sensor input: the accelerometer and the others report a
  phone lying still.
- Play Store features: in-app purchases, Play Integrity, Google Maps.
- Notifications: an app may post them, but they are not shown.

Where things are
----------------
Installed apps:  %LOCALAPPDATA%\Win.apk\library
App data:        %LOCALAPPDATA%\Win.apk\android
Logs:            %LOCALAPPDATA%\Win.apk\logs  (last-run.log is the app
                 opened last, install-<app>.log is an install)
Settings:        HKEY_CURRENT_USER\Software\Win.apk

Removing an app from the library keeps what it saved; uninstalling Win.apk
keeps the apps too. Delete %LOCALAPPDATA%\Win.apk to start from scratch.

Reporting a problem
-------------------
If an app does not start or misbehaves, the last log says why. Please open
an issue on GitHub (the "An app does not work" template) with the app's
name, version and where it came from, the exact steps that lead to the
problem, a screenshot of the app at that point, and last-run.log attached
whole. The repository's docs\APPS.md lists common apps and the problems
already known, and docs\USER_GUIDE.md explains everything above in more
detail. These lines in the log are the most useful:
  "native not implemented"            a Windows piece that is missing
  "not implemented on Windows yet"    a system function an app's native code needs
  "cannot load <library>"             a native library did not load
  "Exception" or "FATAL"              the app crashed

Apps run with your Windows user account's rights, without a phone-style
sandbox: install only apps you trust.

License: AGPL-3.0, like Mac.apk. Source: the repository this build came
from. Win.apk is not affiliated with the Mac.apk project.
