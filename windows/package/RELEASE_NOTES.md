A pre-release for testing: Win.apk runs Android apps on Windows 10/11 x64.

## Getting started
- **Install:** run `Win.apk-Setup.exe`. It installs per user, with no administrator rights, and puts Win.apk in the Start menu. If you'd rather not install, unzip `Win.apk-windows-x64.zip` and start `Win.apk.exe`.
- **Add apps:** get them from the **Store** tab (F-Droid's free and open-source apps: search, Install, Update), drag `.apk` files onto the Win.apk window, or double-click them in Explorer. Each app gets its own icon in the library, the Start menu and the desktop.
- **Included:** Java and ANGLE. `README.txt` has the details.

## What works
- **Java/Kotlin apps:** each opens in its own window, with mouse, wheel and keyboard input. Back is Escape, Alt+Left or the mouse's back button.
- **Resizable windows:** drag an edge, maximize or snap an app's window and the app lays itself out for the new size, as an Android desktop window does (an app that fixed its orientation keeps it, with black bars). Each app remembers its size; Get Info resets it.
- **In Hebrew or English:** Setup and the library follow the Windows display language; Hebrew is laid out right to left.
- **OpenGL ES:** works from Java (`GLSurfaceView`, libGDX) and from an app's native code through a SurfaceView (SDL games). It runs on ANGLE (Direct3D 11), falling back to WARP or SwiftShader without a GPU.
- **Native libraries:** apps that ship `lib/x86_64` libraries load them natively. Apps that only ship `lib/arm64-v8a` libraries are supported: their ARM code is translated to x64 in the background (`winapk-aot`, compiled with clang for your CPU, in parallel) and runs at close to native speed. Apps open at once on an ARM JIT and switch to the translated code as it becomes ready; large libraries are translated only where the app's code ran. Installs use the ABIs `x86_64,arm64-v8a`.
- **Sound from Java:** AudioTrack and SoundPool, for WAV, Ogg and MP3.
- **Storage:** files, SharedPreferences and SQLite.
- **Permissions:** apps ask for every permission as on Android, in a Windows dialog over the app (Allow / Don't allow, and "don't ask again" after a second refusal): storage and media, location (precise or approximate, and "all the time" separately), camera, microphone, contacts, calendar, phone, call logs, SMS, notifications, nearby devices, physical activity and body sensors, plus the special access of "All files access", "Media management", "Display over other apps" and "Install unknown apps". Nothing is granted without asking; the answers survive restarts and updates. An app's App info screen, or Get Info in the library, turns each one on or off or resets them, and an app whose permission is taken away is stopped, as on Android.
- **Desktop services:** clipboard, file pickers and game controllers.

In the automated sweep of open-source apps, about 33 of 38 draw their first screen.

## Not yet
- Native code for 32-bit ARM only (`armeabi-v7a`).
- NativeActivity games.
- MediaPlayer and native audio (OpenSL ES and AAudio).
- Video, camera, WebView and location.
- Google Play services.

## Reporting
Please include the app's name and version, and attach `%LOCALAPPDATA%\Win.apk\logs\last-run.log`.
