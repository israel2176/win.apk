A pre-release for testing: Win.apk runs Android apps on Windows 10/11 x64 as ordinary Windows programs. It is an unofficial port of Mac.apk.

## Getting started
- **Install:** run `Win.apk-Setup.exe`. It installs per user, with no administrator rights, and puts Win.apk in the Start menu. SmartScreen warns because the build is not signed: **More info**, then **Run anyway**. If you would rather not install, unzip `Win.apk-windows-x64.zip` and start `Win.apk.exe`.
- **Add apps:** get them from the **Store** tab (F-Droid's free and open-source apps: search, Install, Update), drag `.apk`, `.apkm`, `.xapk` or `.apks` files onto the Win.apk window, or double-click them in Explorer. Each app gets its own icon in the library, the Start menu and the desktop.
- **Included:** Java, ANGLE and the clang that translates ARM code. `README.txt` in the package and `docs/USER_GUIDE.md` in the repository have the details.

## What works
- **Java/Kotlin apps:** each opens in its own window, with mouse, wheel and keyboard input. Back is Escape, Alt+Left or the mouse's back button.
- **Resizable windows:** drag an edge, maximize or snap an app's window and the app lays itself out for the new size, as an Android desktop window does. An app that fixed its orientation keeps it, with black bars. Each app remembers its size; Get Info resets it.
- **Orientation:** apps open in landscape, the shape of a PC screen, unless the settings say portrait. Right-click an app > "Open in portrait" / "Open in landscape" turns just that app the other way. An app whose manifest requires one orientation always opens that way.
- **English and Hebrew:** Setup and the library follow the Windows display language; Hebrew is laid out right to left.
- **OpenGL ES:** from Java (`GLSurfaceView`, libGDX) and from an app's native code (SDL games), on ANGLE (Direct3D 11), falling back to WARP or SwiftShader without a GPU.
- **Games in native code:** `NativeActivity` apps get their window, input queue and configuration as on Android.
- **Native libraries:** apps that ship `lib/x86_64` libraries load them directly. Apps that only ship `lib/arm64-v8a` libraries have their ARM code translated to x64 in the background (`winapk-aot`, compiled with clang for your CPU). The app opens at once on an ARM JIT and switches to the translated code as it becomes ready; large libraries are translated only where the app's code ran.
- **Sound:** AudioTrack, SoundPool and MediaPlayer audio from Java (WAV, Ogg, MP3, AAC and what Windows decodes), AAudio from native code, and the microphone.
- **Video:** `MediaPlayer` and `VideoView` play MP4, MOV and other files Windows decodes (H.264 and AAC always; HEVC, VP9 and AV1 with Microsoft's codec packs), from a file or an http(s) URL, into the app's `SurfaceView` or `TextureView`, with the sound in step, pause, seeking and looping. ExoPlayer and other `MediaCodec` players decode H.264 video and AAC/MP3 audio on the same Media Foundation decoders.
- **Web content:** WebView and Custom Tabs on WebView2 (Microsoft Edge), including `shouldInterceptRequest` for Cordova and Capacitor apps.
- **Camera and location:** webcams through the Camera, camera2 and CameraX APIs (a virtual camera when there is none); location from Windows' location service, behind Windows' privacy settings.
- **Storage:** files, SharedPreferences and SQLite. Shared storage (Downloads, Pictures, Documents, Music, Movies) is your own Windows folders; a setting turns that off.
- **Permissions:** apps ask for every permission group as on Android, in a dialog over the app (Allow / Don't allow, and no more asking after a second refusal): storage and media, location (precise or approximate, and "all the time" separately), camera, microphone, contacts, calendar, phone, call logs, SMS, notifications, nearby devices, physical activity and body sensors, plus "All files access", "Media management", "Display over other apps" and "Install unknown apps". The answers survive restarts and updates. An app's App info screen, or Get Info in the library, turns each one on or off or resets them; taking a permission away stops the app, as on Android.
- **Google sign-in and Play services:** apps that sign in with Google, check a Play licence, use Firebase Analytics or Cloud Messaging, or ask Play services for location get Win.apk's own in-process stand-ins, as on the Mac. Signing in opens Google's own page in a window of its own (it needs the WebView2 Runtime); Win.apk never sees the password. Settings > Google account adds and removes the account.
- **Desktop services:** clipboard, file pickers and game controllers.

The compatibility sweep (`windows/test/compat/`) boots about 90 open-source apps headless on every pull request; its report is attached to each run.

## Not yet
- Native code for 32-bit ARM only (`armeabi-v7a`).
- Video from an app's native code (the NDK's `AMediaCodec` and `AMediaExtractor`), `MediaExtractor`, and Opus, Vorbis or FLAC audio through `MediaCodec` (an ExoPlayer stream with such audio stays silent).
- OpenSL ES from native code: engines that use it get no sound.
- Vulkan, and real sensor input (the accelerometer and the others report a phone lying still).
- Play Store features: in-app purchases, Play Integrity, the Maps SDK's map.
- Notifications are accepted but not shown.

## Reporting
Open an issue on GitHub with the "An app does not work" template. Please include the app's name, version and where it came from, the exact steps to reproduce the problem, a screenshot of the app at the point it goes wrong, and attach `%LOCALAPPDATA%\Win.apk\logs\last-run.log` (the whole file). `docs/APPS.md` in the repository lists common apps and the problems already known. If Win.apk is useful to you, a star on the repository helps.
