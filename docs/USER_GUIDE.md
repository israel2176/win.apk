# Win.apk user guide

How to install, run and manage Android apps with Win.apk. This guide describes
what each part does, not where it sits on the screen: the library's layout may
change between releases while the behaviour stays.

- [Installing apps](#installing-apps)
- [The Store](#the-store)
- [Running an app](#running-an-app)
- [Managing an app](#managing-an-app)
- [Settings](#settings)
- [Orientation and window size](#orientation-and-window-size)
- [Hebrew and right-to-left](#hebrew-and-right-to-left)
- [Shortcuts and file types](#shortcuts-and-file-types)
- [Permissions](#permissions)
- [Google account](#google-account)
- [Shared storage](#shared-storage)
- [Logs](#logs)
- [Removing apps and uninstalling](#removing-apps-and-uninstalling)
- [Where things are](#where-things-are)
- [Command line](#command-line)
- [Troubleshooting](#troubleshooting)

## Installing apps

Win.apk installs four formats:

| File | What it is |
|---|---|
| `.apk` | A plain Android package |
| `.apkm` | A split-APK bundle from APKMirror |
| `.xapk` | A split-APK bundle from APKPure |
| `.apks` | A split-APK bundle made with bundletool |

A bundle holds a base APK and split APKs (languages, screen densities, CPU
architectures). Win.apk picks the splits the app needs here, the way the
Android installer does.

Any of these starts an install:

- drag one or more files onto the Win.apk window;
- click the drop zone or the **Add app** tile and choose files;
- double-click an app file in Explorer (see [Shortcuts and file types](#shortcuts-and-file-types));
- click **Install** in the [Store](#the-store);
- run `Win.apk.exe <file>` from a command line, which installs the app and
  then opens it.

What happens during an install:

1. Win.apk copies the file into its library
   (`%LOCALAPPDATA%\Win.apk\library`), so you can delete the download.
2. The runtime unpacks the app the way an Android device does, converts its
   DEX code to Java classes, and extracts its icon and name. This takes up to
   a minute for a typical app, longer for very large ones, and happens once
   per app version.
3. If the app has only ARM64 native libraries, their translation to x64
   starts in the background (see [ARM-only apps](#arm-only-apps)).
4. The app gets a tile in the library, a Start menu shortcut under
   **Start > Win.apk Apps** and, unless turned off in the settings, a desktop
   shortcut.

Installs run one at a time. A window shows the queue, each app's progress and
its install log as it is written; you can hide it, and the library keeps
showing the progress on the app's tile. If an install fails, the window says
so and offers the log.

Two apps with the same library name (the app's label) cannot coexist: a
second install of the same app replaces the first and keeps the app's data.
The right-click menu's **Update from APK** does the same deliberately.

### Native code and CPU architectures

Android phones use ARM processors; a PC uses x86-64. An app's native code
(`lib/<abi>/*.so` inside the APK) falls into one of these cases:

| The APK has | What Win.apk does |
|---|---|
| No native libraries (Java or Kotlin only) | Runs directly |
| `lib/x86_64` | Loads those libraries directly, at full speed. Win.apk prefers this build whenever the APK or bundle has it |
| Only `lib/arm64-v8a` | Translates the libraries to x64 in the background; the app runs on an ARM64 JIT until the translation is ready |
| Only `lib/armeabi-v7a` (32-bit ARM) | Not supported. The app does not run; look for an x86_64 or arm64-v8a build |

#### ARM-only apps

When an app has only 64-bit ARM libraries, Win.apk's translator
(`winapk-aot`) turns each library into C, and the bundled clang compiles it
for your CPU into a DLL next to the library. Nothing waits for this: the app
starts at once on an ARM64 JIT, which is slower, and each library switches to
its translation as soon as it is ready, even while the app is running. Large
libraries are translated only where the app's code actually ran, and more as
you use the app, so the first runs of a big app are the slow ones. Whole
translations are shared between apps that ship the same library.

The translation uses the clang shipped in Win.apk's `clang` folder. To use a
different one, set the environment variable `WINAPK_AOT_CLANG` to its
`clang.exe`; without the bundled one, Win.apk also looks for `clang.exe` on
the `PATH`.

## The Store

The **Store** tab is [F-Droid](https://f-droid.org)'s catalog of free and
open-source apps, usually Java-only and a good place to start. It lists the
apps that can run here: those without native code, with an x86_64 build, or
with an arm64-v8a build (translated). Where F-Droid offers one build per CPU
architecture, the x86_64 one is used. Type to search, or pick a category.

- **Install** downloads the APK from f-droid.org, checks it against the
  SHA-256 hash in F-Droid's index (a file that does not match is not
  installed) and installs it like a dropped file. The card shows the download
  and the install; then its button says **Open**.
- **Update** appears when F-Droid has a newer version (by version code) of an
  app you have with the same package name, however you installed it. The
  app's data stays.
- Click a card for the app's version, size, license and a link to its
  F-Droid page.

The first time, the Store downloads F-Droid's index (tens of megabytes) and
keeps a summary of it in `%LOCALAPPDATA%\Win.apk\store\catalog.tsv`. After
that it opens at once and checks for a newer index in the background when
the saved one is more than six hours old; **F5** checks now. While the
library is empty, Win.apk opens on the Store.

The Store needs an internet connection and reaches only f-droid.org. The
downloaded APK is deleted after the install.

## Running an app

Open an app by double-clicking its tile, pressing Enter on it, or using its
Start menu or desktop shortcut. The library shows which apps are running;
**Stop** closes the selected one.

In an app:

| You do | The app sees |
|---|---|
| Click, drag | A finger touch and drag |
| Mouse wheel | A short scroll gesture |
| Keyboard | Key presses, including typing into text fields |
| **Escape**, **Alt+Left**, the mouse's back button | Android's Back |
| Close the window | The app quits |
| Resize, maximize or snap the window | A display of the new size; the app lays itself out again |

An app runs once at a time. Starting it again (from a shortcut or the
library) brings the running window to the front.

Apps run with your Windows user account's permissions. There is no
phone-style sandbox: install only apps you trust ([`SECURITY.md`](../SECURITY.md)).

### Library keyboard shortcuts

| Keys | Action |
|---|---|
| Arrow keys | Move between apps |
| Enter | Open the selected app |
| Delete | Remove the selected app (asks first) |
| Typing, Ctrl+F | Search the library; Esc shows every app again |
| Ctrl+O | Add an app from a file |
| Ctrl+I | Get Info for the selected app |
| Ctrl+1, Ctrl+2 | The Library and Store tabs |
| F5 | Refresh the library, or check F-Droid for new apps in the Store |
| Ctrl+, | Settings |

## Managing an app

Right-click an app's tile for:

| Action | What it does |
|---|---|
| **Open** | Starts the app |
| **Get Info** | The app's details, permissions, orientation and window size, where its files are and how big they are, its install log |
| **Open in portrait** / **Open in landscape** | Turns this one app the other way from the settings' default. For an app whose manifest fixes its orientation the entry is disabled and says so |
| **Create desktop shortcut** | Adds a desktop shortcut with the app's icon |
| **Show the APK in Explorer** | Opens the library folder with the app's APK selected |
| **Open the app's files** | Opens the app's folder under `apps\` |
| **Show the last log** | Opens the log viewer on `last-run.log` |
| **Update from APK** | Installs a newer (or any other) version over this one. The app's saved data stays. Refused while the app runs |
| **Reinstall** | Converts the app again from its APK, for a broken install |
| **Remove** | Deletes the app and its shortcuts. What it saved stays, so installing it again picks up where it left off |

**Get Info** also lets you change the app's permissions (see
[Permissions](#permissions)), choose its orientation, and reset the window
size it remembered.

## Settings

The gear opens the settings. Each one takes effect the next time an app
opens unless noted.

| Section | Setting | What it does |
|---|---|---|
| Appearance | **Theme** | Match Windows, Light or Dark. Applies at once |
| Appearance | **Language** | Match Windows, English or Hebrew. Applies at once; see [Hebrew and right-to-left](#hebrew-and-right-to-left) |
| File types | **Open Android app files with Win.apk** | Makes Win.apk the default program for `.apk`, `.apkm`, `.xapk` and `.apks` |
| Screen | **Screen size** | The phone screen apps see, in dp (density-independent pixels, as on a phone). **Auto** is 360 × 800 dp, or the size given by `WINAPK_PHONE_SIZE` when that variable is set. Presets range from 320 × 480 to a 10-inch tablet |
| Screen | **Orientation** | Portrait or landscape, for every app that has no choice of its own. The default is landscape, the shape of a PC screen |
| Shortcuts | **Desktop shortcut for new apps** | Every app gets a Start menu shortcut; this adds one on the desktop too |
| Shortcuts | **Recreate** | Makes every app's Start menu shortcut again, with its icon |
| Logs | **View logs**, **Open folder** | The log viewer and the logs folder |
| Google account | **Sign in**, **Add another account**, **Sign out** | See [Google account](#google-account) |
| App data | **Use my Windows folders** | On: apps' shared storage (Downloads, Pictures, Documents, Music, Movies) is your own Windows folders. Off: each is an empty folder of Win.apk's own, and your files stay where they are. See [Shared storage](#shared-storage) |
| App data | **Open folder** | Opens `%LOCALAPPDATA%\Win.apk` |
| About | **Version**, **Remove all apps** | The version, and a button that removes every app (their data stays) |

Settings are stored under `HKEY_CURRENT_USER\Software\Win.apk`.

## Orientation and window size

An app opens in the orientation decided by these rules, first match wins:

1. The orientation the app's manifest fixes for its first activity (a
   portrait-only game always opens portrait).
2. The app's own choice from right-click > **Open in portrait / landscape**,
   or Get Info.
3. The **Orientation** setting (landscape by default).

The **Screen size** setting gives the phone's dimensions in dp; the
orientation decides which is the width. The app's window opens at that size,
scaled for your monitor. If the window would not fit the monitor's work area,
the app keeps its dp size and is drawn smaller.

The window is the app's display. Dragging an edge, maximizing or snapping
gives the app a display of the new size, and it lays itself out again, as an
Android desktop window does. An app that fixed its orientation keeps its
shape in a window of the other one: it is drawn in the largest box of its
aspect, centred, with black bars around it.

The size you settle on is remembered per app and used the next time the app
opens, before the Screen size setting. Get Info shows it and can reset it;
choosing another orientation for the app also forgets it.

## Hebrew and right-to-left

Win.apk speaks English and Hebrew. When the Windows display language is
Hebrew, the installer and the library open in Hebrew; otherwise pick
**Language > עברית** in the settings, and the library switches at once. In
Hebrew every window of the library is laid out right to left: the library,
the settings, Get Info, the install window, menus and dialogs. Logs stay left
to right.

An app's window follows Windows' reading layout for right-to-left users: its
title bar is mirrored, while the app draws its own layout as it would on a
phone.

## Shortcuts and file types

- Every installed app gets a shortcut in **Start > Win.apk Apps** with its
  own icon. The shortcut starts the app through Win.apk.
- A desktop shortcut is added too when **Desktop shortcut for new apps** is
  on, and can be added later from the right-click menu.
- The installer makes `.apk` files open with Win.apk. The setting
  **Open Android app files with Win.apk** extends that to `.apkm`, `.xapk` and
  `.apks`, and takes the `.apk` association back if another program took it.
  Double-clicking such a file installs the app and opens it.
- **Shortcuts > Recreate** rebuilds every Start menu shortcut, for example
  after moving Win.apk.

## Permissions

Apps ask for permissions as on Android. When an app requests a dangerous
permission group, a Windows dialog opens over the app:

- **Allow** grants the group. **Don't allow** denies it; a second refusal
  denies it for good and the app is not asked again.
- Location offers **Only approximate**, and an app that wants location in the
  background is asked for that separately once the foreground permission is
  granted.
- A permission in a group that already has a granted member is granted
  without asking, as Android does.
- Apps built for Android 5 and older hold what they declare, as on a device.

The groups: storage and media (photos and videos, music and audio, files),
location, camera, microphone, contacts, calendar, phone, call logs, SMS,
notifications, nearby devices, physical activity and body sensors.

The special access a device grants in its Settings app has the same dialog
here: **All files access**, **Media management**, **Display over other apps**
and **Install unknown apps**. For access Windows cannot provide (notification
access, accessibility services, usage access, Do Not Disturb access), the
dialog says so and grants nothing. For what Win.apk never restricts (battery
optimization, exact alarms, modifying system settings), it says the app
already has it.

Your answers are kept per app, signed, in `%LOCALAPPDATA%\Win.apk\appops`,
and survive restarts and updates. Removing the app forgets them. To change a
permission later, use the app's own **App info** screen (an app that opens
its settings page lands there) or **Get Info > Permissions** in the library:
click a permission to turn it on or off, or **Reset** them all. Turning a
permission off stops the app if it is running, as Android does.

Granting a permission does not create the data behind it: a PC has no
contacts, SIM, SMS or sensors, so an app that is granted those finds them
empty. The camera and microphone use the PC's webcam and microphone, behind
Windows' own privacy switches (Settings > Privacy). Location comes from
Windows' location service; when Windows denies it, Win.apk opens the Windows
location settings page once per run.

## Google account

Apps that use Google Play services get Win.apk's own in-process stand-ins,
inherited from Mac.apk: Google sign-in, Play licence checks, Firebase Cloud
Messaging (push), Firebase Analytics and the fused location provider work
without the Play Store. In-app purchases, Play Integrity attestation and the
Maps SDK's map do not: there is no Google signing certificate and no installed
Play services package, so an app that checks for them finds none.

**Settings > Google account** lists the accounts on this PC, adds one and
signs out. Signing in opens Google's own sign-in page in a window of its own
(Microsoft Edge WebView2, in a private profile). Win.apk never sees the
password; it keeps the token Google hands back, in
`%LOCALAPPDATA%\Win.apk\gms`. Signing out forgets the account and its tokens
on this PC; apps that used it ask again. Apps can also open the same sign-in
window themselves, through their "Add an account" flow.

Google sign-in needs the WebView2 Runtime (a 2022 or later version). Without
it the sign-in is cancelled and the log says why.

## Wi-Fi and Bluetooth

Apps see the PC's Wi-Fi: whether it is on, the current network with its
signal and address, and scan results. Network names and scan results need
location access for desktop apps in Windows' privacy settings, the same way
Android needs the location permission. An app cannot connect the PC to a
network.

Bluetooth works with devices you have paired in Windows Settings: an app can
open a serial (RFCOMM) connection to one, or listen for one to connect to the
PC, scan for Bluetooth LE devices and connect to them (reading, writing and
notifications), after it asks for the Nearby devices permission. Apps cannot
pair or discover classic devices themselves, advertise, act as a GATT server,
or use the audio and input profiles.

## Shared storage

On a phone, apps share folders such as Download and Pictures. Here those
folders are your own Windows folders: `/sdcard/Download`, `Pictures`,
`Documents`, `Music` and `Movies` are links to Windows' Downloads, Pictures,
Documents, Music and Videos, made before an app starts. Anything an app had
saved there earlier is moved into your folders first, never over a file of
yours (a name clash comes in as `name (from Android).ext`). `DCIM` stays
Win.apk's own.

**Settings > App data > Use my Windows folders** turns this off: each folder
becomes an empty one of Win.apk's own again, and your files stay where they
are.

## Logs

**Logs** at the bottom of the library opens the log viewer, which shows any
of Win.apk's logs and follows the end as it is written. **Copy** puts the log
on the clipboard, **Notepad** opens it there, **Folder** opens the logs
folder (`%LOCALAPPDATA%\Win.apk\logs`).

| File | What it records |
|---|---|
| `last-run.log` | The app opened most recently |
| `install-<app>.log` | That app's install or reinstall |
| `gms.log` | The Google account operations run from the settings |
| `install-helper.log`, `package-backfill.log` | Installs an app itself asked for (F-Droid installing another app), and library maintenance |
| `hs_err_pid<n>.log` | A crash report from the Java VM, if the VM itself crashed |

## Removing apps and uninstalling

- **Remove** (right-click, or Delete) deletes the app's APK, converted code,
  icon and shortcuts, and forgets its permissions. What the app saved under
  `android\` stays, so installing it again picks up where it left off.
- **Settings > About > Remove all apps** does that for every app.
- Uninstalling Win.apk (Windows Settings > Apps) removes the program, the
  Start menu shortcuts and the `.apk` association, and keeps
  `%LOCALAPPDATA%\Win.apk`. Delete that folder to remove the apps, their data,
  the Store's catalog and the Google account.

## Where things are

| What | Where |
|---|---|
| Win.apk itself | `%LOCALAPPDATA%\Programs\Win.apk`, or the folder you unzipped |
| Your copy of each APK | `%LOCALAPPDATA%\Win.apk\library\<app>.apk` |
| Per-app choices beside the APK | `<app>.label` (name), `<app>.package` (package, version, manifest orientation), `<app>.orientation` (your choice), `<app>.window` (remembered window size) |
| Each app's converted code, icon and name | `%LOCALAPPDATA%\Win.apk\apps\<app>` |
| The Android file system: apps' data, the installed packages | `%LOCALAPPDATA%\Win.apk\android` |
| Logs | `%LOCALAPPDATA%\Win.apk\logs` |
| The Store's catalog and icons | `%LOCALAPPDATA%\Win.apk\store` |
| The installed-apps registry | `%LOCALAPPDATA%\Win.apk\installed_apps.json` |
| Permission grants | `%LOCALAPPDATA%\Win.apk\appops` |
| The Google account and its tokens | `%LOCALAPPDATA%\Win.apk\gms` |
| Shared ARM translations | `%LOCALAPPDATA%\Win.apk\aot-cache` |
| Settings | `HKEY_CURRENT_USER\Software\Win.apk` |
| Start menu shortcuts | `Start > Win.apk Apps` |

## Command line

```bat
Win.apk.exe                          :: the library
Win.apk.exe app.apk                  :: install app.apk, then open it
Win.apk.exe --run <library apk>      :: open an installed app (what its shortcuts do)
```

The runtime can also be run directly, for testing:

```bat
bin\winapk-runtime.exe --apk <file.apk|.apkm|.xapk|.apks> [--data <dir>] [--jdk <dir>] [--lib <dir>] [--install-only]
bin\winapk-runtime.exe [--data <dir>] [--jdk <dir>] [--lib <dir>] --gms <command> [args]
```

| Option | Meaning |
|---|---|
| `--apk` | The app to install and run |
| `--data` | The data folder (default `%LOCALAPPDATA%\Win.apk`) |
| `--jdk` | The Java runtime (default: the `jre` folder next to `bin`, else `WINAPK_JAVA_HOME` or `JAVA_HOME`) |
| `--lib` | The folder with the framework jars (default: `lib` next to `bin`) |
| `--install-only` | Install and convert the app, write its icon and name, and exit |
| `--gms <command>` | Run a Google Play services command with no app: `status`, `selftest`, `accounts`, `signin-window`, `signout`, `checkin`, `license <package> <versionCode>`, `gcmtoken <package>` and the rest of `com.macapk.gms.GmsCli` |

Exit codes: 0 done, 2 wrong arguments, 3 when `--install-only` finds the app
running. `--gms` exits 0 done, 1 error (an `ERROR` line on standard output),
2 cancelled or wrong arguments.

Environment variables the launcher and the runtime read:

| Variable | Effect |
|---|---|
| `WINAPK_PHONE_SIZE=412x915` | The phone size in dp. The launcher sets it from the Screen size setting; set it yourself for the **Auto** preset or a direct run |
| `WINAPK_RESIZABLE=0` | The window letterboxes the phone instead of resizing the app |
| `WINAPK_DENSITY=<dpi>` | Overrides the density the app sees |
| `WINAPK_AOT_CLANG=<path to clang.exe>` | The compiler for ARM translation |
| `WINAPK_ABIS=arm64-v8a` | Install an app's ARM64 build although it has an x86_64 one (for comparing the two) |
| `WINAPK_ALLOW_SECOND_INSTANCE=1` | Allow a second copy of a running app |
| `WINAPK_WEBVIEW2=0` | Do not use WebView2; web links open in the system browser |
| `WINAPK_ANGLE_BACKEND=d3d11` or `swiftshader` | Force a graphics backend |
| `WINAPK_PROXY=socks5://host:port` | A SOCKS proxy for the push (Firebase Cloud Messaging) connection |
| `WINAPK_HEADLESS=1` | Run without a window, for tests |
| `WINAPK_HEADLESS_FRAME=frame.bmp` | Save the last frame the app drew |
| `WINAPK_HEADLESS_EXIT_AFTER_FRAMES=30` | Exit that many frames after the first |

[`windows/README.md`](../windows/README.md) lists the variables meant for
development and testing.

## Troubleshooting

**The app does not start, or stops with an error.** Open **Logs** and read
the end of `last-run.log`. The most useful lines:

| Line | Meaning |
|---|---|
| `native not implemented on Windows yet` | A piece of the Android host that is still missing |
| `not implemented on Windows yet` (about a system function) | A C library function the app's native code needs that the Bionic shim does not have yet |
| `[win.apk] cannot load <library>` or `UnsatisfiedLinkError` | A native library did not load: 32-bit ARM only, packed or encrypted, or for another CPU |
| `Exception` or `FATAL` | The app crashed; the lines after it say where |
| `[win.apk]` | A message from Win.apk itself |

Then look in [`APPS.md`](APPS.md) for the app or the kind of problem, and
[open an issue](../../../issues/new/choose) with the app's name, version and
source, the exact steps, a screenshot and the whole log.

**The install failed.** The install window offers the log
(`install-<app>.log`). An APK with only 32-bit ARM libraries cannot be
installed; a Store entry that says the APK has no native code for this PC
needs the full bundle instead.

**The app is slow.** If it has only ARM64 libraries, it runs on the JIT until
its translation is ready; give it a few minutes on the first runs and watch
the log for the translation finishing. Check that the `clang` folder is next
to `bin` in Win.apk's folder.

**No sound from a native game.** Games whose engine plays audio through
OpenSL ES get no sound yet; AAudio works.

**Video does not play.** Video playback is not implemented yet. Audio from
MediaPlayer plays.

**Windows Defender or SmartScreen warns.** Preview builds are not
code-signed, and Win.apk writes and then runs translated code, which some
scanners flag. Allow it if you trust the download.

**Text or layout is wrong in the app.** Try another Screen size preset, or
turn the app the other way (right-click > Open in portrait / landscape). A
screenshot plus the log makes a good report.

**The app asks for something Windows cannot give** (notification access,
accessibility, usage access). The dialog explains this and the app is told
no; the rest of the app usually works.
