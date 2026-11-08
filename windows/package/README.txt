Win.apk preview
===============

Win.apk runs Android apps on Windows 10 (1903 or later) and 11, 64-bit, as
ordinary Windows programs: each app in its own window, with its own Start
menu entry and Windows notifications. There is no emulator and no virtual
machine. Win.apk is an unofficial port of Mac.apk and is not affiliated with
or endorsed by the Mac.apk project. This is an early preview.

Documentation, in English and Hebrew: https://github.com/israel2176/win.apk

Installing Win.apk
------------------
Win.apk-Setup.exe installs it for your user only (no administrator rights),
puts it in the Start menu and opens .apk files with it. Windows SmartScreen
warns because the preview is not code-signed: click "More info", then "Run
anyway". Or unzip Win.apk-windows-x64.zip anywhere and start Win.apk.exe.

Included: Java (built from Eclipse Temurin 21, the jre folder), ANGLE for graphics
(bin\libEGL.dll, bin\libGLESv2.dll), and the clang compiler that translates
apps' ARM code for your processor (the clang folder). Web pages inside apps
and Google sign-in use the Microsoft Edge WebView2 Runtime, which Windows 11
and most Windows 10 PCs already have.

Adding and running apps
-----------------------
- From the Store tab: F-Droid's free and open-source apps, the easiest place
  to start. Search, then Install.
- From a file: double-click an .apk, .apkm, .xapk or .apks in Explorer, drag
  it onto the library, or click "Install app". Prefer an x86_64 build of an app
  (or one with no native code), then arm64-v8a. An app with only 32-bit ARM
  code (armeabi-v7a) does not run.
- Double-click an app to open it. The mouse is your finger, the wheel
  scrolls, and Escape is Android's Back. Resize the window and the app lays
  itself out again.
- Right-click an app for Open, Stop, App details and Remove. App details holds
  the rest: orientation, permissions, the log, Report a problem.
- Settings: theme, language (English or Hebrew), app window size and default
  orientation,
  your Windows folders, the ARM translation, the Google account, updates.

What works, and what does not yet: docs/APPS.md in the repository, one table
checked against the code. In short, Java and Kotlin apps, OpenGL ES games,
sound and video, WebView, webcams, location, Bluetooth, notifications and
Google sign-in work; 32-bit ARM code, Vulkan, OpenSL ES sound, the NDK media
API, real sensors, in-app purchases and Play Integrity do not.

Where things are
----------------
Your apps and their data:  %LOCALAPPDATA%\Win.apk
Logs:                      %LOCALAPPDATA%\Win.apk\logs (run-<app>.log is an
                           app's last run, install-<app>.log its install)
Settings:                  HKEY_CURRENT_USER\Software\Win.apk

Removing an app keeps what it saved unless you tick "Also remove accounts and
data saved by the app"; uninstalling Win.apk keeps the apps too. Delete %LOCALAPPDATA%\Win.apk
to start from scratch.

Reporting a problem
-------------------
Use "Report a problem" in Win.apk: under an error, or on an app's details
page. It opens GitHub's form with the versions filled in and the logs folder
with the app's log selected; nothing is sent until you post it. Include where
the APK came from, the steps, a screenshot and the whole log. Hebrew is fine.
docs/troubleshooting.md explains the common log lines.

Apps run with your Windows account's rights, without a phone-style sandbox:
install only apps you trust.

License
-------
Win.apk is free software under the GNU AGPL v3.0 (LICENSE.txt), like Mac.apk;
its source is at https://github.com/israel2176/win.apk . The third-party
components it ships and their licences are listed in THIRD_PARTY_NOTICES.txt;
full texts are also in jre\legal, bin\licenses and clang\licenses.
