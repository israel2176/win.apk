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
- [Updates](#updates)
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

- double-click an app file in Explorer (see [Shortcuts and file types](#shortcuts-and-file-types)).
  Only the install window opens, not the library;
- drag one or more files onto the library window;
- click **Add app** in the library's toolbar (or press Ctrl+O) and choose files;
- click **Install** on an app's card in the [Store](#the-store);
- run `Win.apk.exe <file>` from a command line, which is the same as
  double-clicking it.

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

Installs run one at a time, in a small install window:

- While it works, the window shows the app's icon and name (the file's name
  until the app's own is read, near the end), a moving bar and one line on
  what it is doing: **Installing…**, then **Converting the app's code. This
  happens once.**, then **Getting the app's name and icon…**.
- **Show details** opens the install's log as it is written; the window
  remembers whether it was open.
- Files opened while an install is under way, from Explorer or by dragging,
  join the same window: the one being installed is at the top, the others
  below it with their state.
- **Cancel** (or closing the window) stops the install after asking. What was
  not installed yet is left out; an app that was being updated keeps its
  previous version.
- When it is done: **Open** starts the app, **Close** closes the window, and
  **Show in library** opens the library with the app selected. With several
  apps each one has its own **Open**.
- When it fails: one sentence on why, **Show log** (the install log, in the
  window) and **Copy details** (the reason, Win.apk's version and the end of
  the log, for a bug report).

Two apps with the same library name (the app's label) cannot coexist: a
second install of the same app replaces the first and keeps the app's data.
An app's details page has **Update from a file** to do the same deliberately.

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

The translation keeps out of your way. It runs at the lowest priority and
uses at most one processor core while you are using the PC, more only while
you are away, and none on battery saver or while a full-screen program (a
game, a presentation) runs. It never takes the memory other programs need:
it compiles less at once when memory is short. If it is stopped, by closing
the app, shutting down or ending the process, it goes on where it was the
next time. If a translation fails it is tried again later, and after four
failures the library stays on the JIT until the app or Win.apk is updated
(the app's `aot-<app>.log` in the logs folder says why).

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
  and the install; then it says **Installed**, and the app opens from the
  library.
- **Update** appears when F-Droid has a newer version (by version code) of an
  app you have with the same package name, however you installed it. The
  app's data stays.
- Click a card for the app's details page: whether it is in your library, its
  version, size, license and categories, and a link to its F-Droid page.
  **Back** (the arrow, Esc or Alt+Left) returns to the Store.

The first time, the Store downloads F-Droid's index (tens of megabytes) and
keeps a summary of it in `%LOCALAPPDATA%\Win.apk\store\catalog.tsv`. After
that it opens at once and checks for a newer index in the background when
the saved one is more than six hours old; **F5** or the refresh button
checks now.

The Store needs an internet connection and reaches only f-droid.org. The
downloaded APK is deleted after the install.

## Running an app

Open an app by double-clicking its tile (or selecting it and pressing Enter),
from its right-click menu, or with its Start menu or desktop shortcut.

A launch window with the app's icon shows at once and stays until the app's
own window is up, the way a phone shows a splash screen. A start that takes
long says why: **Converting the app's code. This happens once.** when the
app's code changed, or **Still starting** after twenty seconds. Esc or the
✕ in its corner cancels the start. If the app ends before its window is up,
or later with an error, the same window turns into a short message
(**Wiz didn't start**, **Wiz stopped with an error**) with **Show log**.

The library marks a running app with a green badge on its tile and counts
them under its title. **Stop** in the tile's right-click menu, or on the
app's details page, closes the app at once.

In an app:

| You do | The app sees |
|---|---|
| Click, drag | A finger touch and drag |
| Mouse wheel | A short scroll gesture |
| Keyboard | Key presses, including typing into text fields |
| **Escape**, **Alt+Left**, the mouse's back button | Android's Back |
| Close the window | The app quits |
| Resize, maximize or snap the window | A display of the new size; the app lays itself out again |

Video plays: `MediaPlayer` and `VideoView` show MP4, MOV and other files
Windows can decode (H.264 and AAC always; HEVC, VP9 and AV1 once Microsoft's
codec packs are installed from the Microsoft Store), from a file or a web
address, with the sound in step, seeking and looping. Players built on
`MediaCodec`, such as ExoPlayer, decode H.264 video with AAC or MP3 audio;
a stream whose audio is Opus, Vorbis or FLAC stays silent, so pick one with
AAC audio where the app lets you.

An app runs once at a time. Starting it again (from a shortcut or the
library) brings the running window to the front.

Closing the library while an app it started is still running, or while an
install is under way, only hides it: Win.apk stays until those finish, so it
can still report an app that stops with an error.

Apps run with your Windows user account's permissions. There is no
phone-style sandbox: install only apps you trust ([`SECURITY.md`](../SECURITY.md)).

### Library keyboard shortcuts

| Keys | Action |
|---|---|
| Arrow keys, Home, End, Page Up/Down | Move between apps |
| Enter | Open the selected app |
| Alt+Enter | The selected app's details |
| Menu key, Shift+F10 | The selected app's right-click menu |
| Typing, Ctrl+F | In the Store: search it; Esc shows everything again |
| Ctrl+O | Add an app from a file |
| Ctrl+1, Ctrl+2 | The Library and the Store |
| Ctrl+, | Settings |
| Ctrl+L | The log window, on the newest log |
| F5 | In the Store: check F-Droid for new apps |
| Esc, Backspace, Alt+Left, the mouse's back button | On a details page: back |
| Tab, Space, Enter | On a page: move between its controls and use them |

## Managing an app

Right-click an app's tile for its menu: the app's icon, name and version at
the top, then **Open**, **Stop** (while it runs), **App details** and
**Remove**, each with its icon and shortcut. It follows the theme and, in
Hebrew, opens to the left.

**App details** (also Alt+Enter) is a page in the library, with a back arrow:

| Section | What it holds |
|---|---|
| Window | **Orientation**: the settings' default, portrait or landscape for this app only (an app whose manifest fixes one says so instead). **Window size**: the size the app's window was last given, with **Reset** |
| Permissions | A switch per permission the app was asked for or granted, and **Reset permissions**; see [Permissions](#permissions) |
| Shortcut | **Desktop shortcut**: adds or removes the app's desktop shortcut. The Start menu one is always there |
| Troubleshooting | **Log**: **Show log** opens the log window on the app's last run (or its install, before the first run) |
| Storage | **The app's file**: its size, and **Show** opens Explorer on it. **Converted code**: the app's folder under `apps\` |
| Manage | **Stop** (while it runs), **Update from a file** (installs a newer, or any other, version over this one; what the app saved stays; refused while the app runs) and **Remove** |

Each action has one place: the details page, plus the right-click menu for
the four above. An app's log is on its details page (**Troubleshooting >
Show log**); Ctrl+L opens the log window on the newest log.

## Settings

**Settings**, at the bottom of the navigation pane (or Ctrl+,), is a page of
its own. Each setting takes effect the next time an app opens unless noted.

| Section | Setting | What it does |
|---|---|---|
| Appearance | **Theme** | Match Windows, Light or Dark. Applies at once |
| Appearance | **Language** | Match Windows, English or Hebrew. Applies at once; see [Hebrew and right-to-left](#hebrew-and-right-to-left) |
| Apps | **Screen size** | The phone screen apps see, in dp (density-independent pixels, as on a phone). **Auto** is 360 × 800 dp, or the size given by `WINAPK_PHONE_SIZE` when that variable is set. Presets range from 320 × 480 to a 10-inch tablet |
| Apps | **Orientation** | Portrait or landscape, for every app that has no choice of its own. The default is landscape, the shape of a PC screen |
| Apps | **Use my Windows folders** | On: apps' shared storage (Downloads, Pictures, Documents, Music, Movies) is your own Windows folders. Off: each is an empty folder of Win.apk's own, and your files stay where they are. See [Shared storage](#shared-storage) |
| Apps | **Desktop shortcut for new apps** | Every app gets a Start menu shortcut; this adds one on the desktop too |
| Apps | **Open .apk, .apkm, .xapk and .apks files with Win.apk** | Makes Win.apk the default program for the four formats |
| Google account | **Sign in**, **Add another account**, **Sign out** | See [Google account](#google-account) |
| Troubleshooting | **Logs** | Opens the `logs` folder in Explorer, to attach files to a bug report |
| Troubleshooting | **App data** | Opens `%LOCALAPPDATA%\Win.apk` |
| Troubleshooting | **Clear library** | Uninstalls every app and removes its shortcuts (app data is kept) |
| Updates | **Check now** | Asks GitHub for the newest release at once. The row shows this copy's version and the last check's result: up to date, a newer version, or could not check. See [Updates](#updates) |
| Updates | **Check for updates automatically** | On (the default): about once a day, while the library is open, Win.apk checks for a newer release in the background and says so in the InfoBar |
| Updates | **Update** | Shown when a newer version is out, with **What's new in** that version below it. Downloads the installer, then runs it and closes Win.apk |
| About | | The version, the source code (github.com/israel2176/win.apk) and the licence (GNU AGPL v3.0), with links; Win.apk is forked from Mac.apk |

Settings are stored under `HKEY_CURRENT_USER\Software\Win.apk`.

## Orientation and window size

An app opens in the orientation decided by these rules, first match wins:

1. The orientation the app's manifest fixes for its first activity (a
   portrait-only game always opens portrait).
2. The app's own choice on its details page (**Window > Orientation**).
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
opens, before the Screen size setting. The app's details page shows it and
can reset it; choosing another orientation for the app also forgets it.

## Hebrew and right-to-left

Win.apk speaks English and Hebrew. When the Windows display language is
Hebrew, the installer and the library open in Hebrew; otherwise pick
**Language > עברית** in the settings, and the library switches at once. In
Hebrew every window of Win.apk is laid out right to left: the library, its
pages, the install window, the launch window, the permission questions, menus
and dialogs. So is the installer, title bar included (its icon on the right,
Close on the left, Finish at the bottom left). Logs stay left to right.

An app's window follows Windows' reading layout for right-to-left users: its
title bar is mirrored, while the app draws its own layout as it would on a
phone.

## Shortcuts and file types

- Every installed app gets a shortcut in **Start > Win.apk Apps** with its
  own icon. The shortcut starts the app through Win.apk, with the launch
  window.
- A desktop shortcut is added too when **Desktop shortcut for new apps** is
  on; an app's details page adds or removes it later.
- The installer makes `.apk` files open with Win.apk. The setting
  **Open .apk, .apkm, .xapk and .apks files with Win.apk** extends that to the
  other three, and takes the `.apk` association back if another program took
  it. Double-clicking such a file opens the install window.
- When Win.apk is updated, it writes every app's Start menu and desktop
  shortcuts again once, so they carry what the new version needs.

## Permissions

Apps ask for permissions as on Android. When an app requests a dangerous
permission group, a small window opens over the app with its icon and name,
one question ("Allow Wiz to access this PC's location?"), at most a line on
what it means on a PC, and two buttons:

- **Allow** grants the group. **Don't allow** denies it; a second refusal
  denies it for good and the app is not asked again. Enter means Don't allow,
  so a stray key never grants anything; Esc or closing the window answers
  nothing.
- Location has a **Precise location** switch: off, Allow grants only the
  approximate location. An app that wants location in the background is
  asked for that separately once the foreground permission is granted.
- A permission in a group that already has a granted member is granted
  without asking, as Android does.
- Apps built for Android 5 and older hold what they declare, as on a device.

The groups: storage and media (photos and videos, music and audio, files),
location, camera, microphone, contacts, calendar, phone, call logs, SMS,
notifications, nearby devices, physical activity and body sensors.

The special access a device grants in its Settings app has the same window
here: **All files access**, **Media management**, **Display over other apps**
and **Install unknown apps**. For access Windows cannot provide (notification
access, accessibility services, usage access, Do Not Disturb access), the
dialog says so and grants nothing. For what Win.apk never restricts (battery
optimization, exact alarms, modifying system settings), it says the app
already has it.

Your answers are kept per app, signed, in `%LOCALAPPDATA%\Win.apk\appops`,
and survive restarts and updates. Removing the app forgets them. To change a
permission later, use the app's own **App info** screen (an app that opens
its settings page lands there: a switch per permission, then **Done**) or
**Permissions** on the app's details page in the library, where **Reset
permissions** forgets them all. Turning a permission off stops the app if it
is running, as Android does.

Granting a permission does not create the data behind it: a PC has no
contacts, SIM, SMS or sensors, so an app that is granted those finds them
empty. The camera and microphone use the PC's webcam and microphone, behind
Windows' own privacy switches (Settings > Privacy). Location comes from
Windows' location service; when Windows denies it, Win.apk opens the Windows
location settings page once per run.

## Notifications

An app's notifications appear as Windows notifications, with the app's name
and icon, and stay in the Action Center until dismissed. Notifications on a
low-importance channel go to the Action Center without popping up. Clicking
one brings the app forward and opens what a tap would open on Android, as
long as the app is running. Apps made for Android 13 or later ask for the
notification permission first; Windows' own notification settings apply on
top of that. Action buttons on notifications are not shown.

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
on this PC; apps that used it ask again.

The account belongs to Win.apk, not to one app: every app on the PC uses it.
An app's **Sign in with Google** opens the same sign-in window when no account
is signed in yet, and the account added there is the one the app signs in
with (and the one Settings shows). With an account, the app shows its chooser.
The round button at the end of the toolbar (on the left in Hebrew) shows the
signed-in account's profile picture, as a browser's profile button does (the
account's initial when Google gave no picture at sign-in), or an outline of a
person when there is none; it opens Settings. The picture is fetched once, at
sign-in, and kept in `%LOCALAPPDATA%\Win.apk\gms` until you sign out; an
account signed in before this version shows its picture after signing out and
in again.

Google sign-in needs the WebView2 Runtime (a 2022 or later version). Without
it the sign-in is cancelled and the log says why.

## Updates

Win.apk's releases are published on GitHub
([github.com/israel2176/win.apk/releases](https://github.com/israel2176/win.apk/releases)).
**Settings > Updates** shows the version you have and what the last check
found; **Check now** checks at once. With **Check for updates automatically**
on (the default), the library checks about once a day when it opens and every
hour while it stays open, once a day has passed since the last check. The
check downloads only GitHub's list of releases and sends nothing about you or
your apps (the request names itself `Win.apk-updater`). Versions are compared
by their numbers: `v0.3.0-preview` is newer than 0.2.0.

When a newer version is out, the InfoBar says so with **What's new**, which
opens Settings at Updates: the new version's row with **Update**, and its
release notes as plain text under **What's new in** that version, with a link
to its page on GitHub. **Update** downloads `Win.apk-Setup.exe` from the
release (the button shows how far it is), checks that it is complete (its
size, and its SHA-256 when GitHub gives one), runs it and closes Win.apk so
that the installer can replace it. Your apps and their data are kept. The
update does not start while an app is running or an install is under way:
close the app or wait, then press **Update** again. A downloaded installer is
kept in `%LOCALAPPDATA%\Win.apk\updates` until the update has been installed.

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

The log window (an app's details page, **Troubleshooting > Show log**, or Ctrl+L) shows
any of Win.apk's logs and follows the end as it is written. Pick the log at
the top left ("Wiz · last run", "Wiz · install", "Google sign-in"…), search
it (every match is marked; Enter or F3 goes to the next, Shift+F3 back),
**Copy** it to the clipboard, or **Open folder** to see the file in Explorer
(`%LOCALAPPDATA%\Win.apk\logs`). Error lines are in red. The install window
and the launch window open the right log themselves when something fails.

| File | What it records |
|---|---|
| `run-<app>.log` | That app's last run; `run-<app>.1.log` is the run before it |
| `last-run.log` | The app opened most recently (a second name for its `run-<app>.log`) |
| `install-<app>.log` | That app's install or update |
| `gms.log` | The Google account operations run from the settings |
| `install-helper.log`, `package-backfill.log` | Installs an app itself asked for (F-Droid installing another app), and library maintenance |
| `hs_err_pid<n>.log` | A crash report from the Java VM, if the VM itself crashed |
| `crash-<app>-<date>.dmp` | A minidump of the app's process when its native code crashed (the last three per app); attach it to an issue with the log |

## Removing apps and uninstalling

- **Remove** (on the app's details page or its right-click menu) asks first,
  then deletes the app's APK, converted code, icon and shortcuts, and forgets
  its permissions. What the app saved stays, so installing it again picks up
  where it left off, unless you tick **Also delete what the app saved**:
  then its data folder (`android\data\data\<package>`) and its folders in
  shared storage (`Android\data` and `Android\obb`) go too, as uninstalling
  does on a phone. Your own files in your Windows folders are never touched.
- **Settings > Troubleshooting > Clear library** uninstalls every app and
  removes its shortcuts (app data is kept).
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
| The last update check and a downloaded update | `%LOCALAPPDATA%\Win.apk\updates` (`releases.json`, `Win.apk-Setup-<version>.exe`) |
| Shared ARM translations | `%LOCALAPPDATA%\Win.apk\aot-cache` |
| Settings | `HKEY_CURRENT_USER\Software\Win.apk` (`Theme`, `Language`, `PhoneSize`, `Landscape`, `DesktopShortcut`, `UserFolders`, `InstallDetails`, `AutoUpdate`, `LastUpdateCheck`, and the library window's size) |
| Start menu shortcuts | `Start > Win.apk Apps` |

`<app>` is the app's package name (`com.waze`) when the file it came from
is named after it, as APK download sites name them, else the file's name up
to its version (`WhatsApp_2.24.1.apk` gives `WhatsApp`). Apps installed
before this keep their folders.

## Command line

```bat
Win.apk.exe                          :: the library
Win.apk.exe app.apk [more files]     :: the install window only, for these files
Win.apk.exe --run <library apk>      :: open an installed app, with its launch window (what its shortcuts do)
```

One Win.apk serves everything: a second start hands its files, or its wish
to see the library, to the one already running, so files opened together
share one install window.

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

**The app does not start, or stops with an error.** Click **Show log** in
the launch window, or **Show log** on the app's details page.
The most useful lines:

| Line | Meaning |
|---|---|
| `native not implemented on Windows yet` | A piece of the Android host that is still missing |
| `not implemented on Windows yet` (about a system function) | A C library function the app's native code needs that the Bionic shim does not have yet |
| `[win.apk] cannot load <library>` or `UnsatisfiedLinkError` | A native library did not load: 32-bit ARM only, packed or encrypted, or for another CPU |
| `Exception` or `FATAL` | The app crashed; the lines after it say where |
| `[win.apk] crash:` and `crash dump:` | The app's native code crashed: the library and offset, and the minidump written for it, which is worth attaching too |
| `[win.apk]` | A message from Win.apk itself |

Then look in [`APPS.md`](APPS.md) for the app or the kind of problem, and
[open an issue](../../../issues/new/choose) with the app's name, version and
source, the exact steps, a screenshot and the whole log.

**The install failed.** The install window says why in a sentence; **Show
log** opens `install-<app>.log`, and **Copy details** copies what a bug
report needs. An APK with only 32-bit ARM libraries cannot be
installed; a Store entry that says the APK has no native code for this PC
needs the full bundle instead.

**The app is slow.** If it has only ARM64 libraries, it runs on the JIT until
its translation is ready. The translation goes faster while you leave the
PC alone: on a busy PC, a large app's first translation can take an hour or
more. `logs\aot-<app>.log` shows its progress and how many compilers it runs
at once, and why. Check that the `clang` folder is next
to `bin` in Win.apk's folder.

**No sound from a native game.** Games whose engine plays audio through
OpenSL ES get no sound yet; AAudio works.

**Video does not play.** `MediaPlayer`, `VideoView` and ExoPlayer play what
Windows decodes: H.264 with AAC always; HEVC, VP9 and AV1 only with
Microsoft's codec packs from the Store. A stream whose audio is Opus, Vorbis
or FLAC plays without sound in an ExoPlayer app (pick one with AAC), and
video from an app's native code (the NDK's `AMediaCodec`) is not there yet.

**Windows Defender or SmartScreen warns.** Preview builds are not
code-signed, and Win.apk writes and then runs translated code, which some
scanners flag. Allow it if you trust the download.

**Text or layout is wrong in the app.** Try another Screen size preset, or
turn the app the other way (its details page, **Window > Orientation**). A
screenshot plus the log makes a good report.

**The app asks for something Windows cannot give** (notification access,
accessibility, usage access). The window explains this in a sentence and the
app is told no; the rest of the app usually works.
