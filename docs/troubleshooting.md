# Troubleshooting and reporting a problem

[עברית](he/troubleshooting.md)

Win.apk is a preview: some apps will not work yet. This page helps you tell a
known limit from a new bug, and send a report we can act on.

- [First steps](#first-steps)
- [Common problems](#common-problems)
- [Reading the log](#reading-the-log)
- [Reporting a problem](#reporting-a-problem)

## First steps

1. Look the app up in [Compatibility](APPS.md): the capability table and the
   known issues by kind of app explain most failures.
2. Check which build you installed: an app with only 32-bit ARM code cannot
   run ([which build to install](arm-apps.md#which-build-to-install)).
3. Open the app's log: **Show log** in the launch window when it fails, or the
   app's details page, **Troubleshooting > Show log**. Ctrl+L in the library
   opens the newest log.

## Common problems

**Windows protected your PC (SmartScreen) when running the installer.** The
preview is not code-signed yet. Click **More info**, then **Run anyway**, if
you downloaded it from this repository's
[Releases](https://github.com/israel2176/win.apk/releases) page.

**An antivirus flags Win.apk or an app.** Win.apk writes code (the ARM
translation) and then runs it, which some scanners dislike, and the preview is
not signed. Allow it if you trust the download.

**The app does not start.** An app with only 32-bit ARM code installs but
stops as soon as it loads that code; its log says `this is a 32-bit ARM library
(armeabi-v7a)`. Otherwise the launch window turns into a message with
**Show log** and **Report a problem**. Read the end of the log
([below](#reading-the-log)). The first start of a new or updated app converts
its code and can take up to a minute.

**The install failed.** The install window says why in a sentence; **Show log**
opens the install log and **Copy details** copies what a report needs.

**The app is slow.** If it has only ARM64 code, it runs on the JIT until its
translation is ready; on a busy PC a large app's first translation can take an
hour or more. Leave the PC alone for a while, or install an x86_64 build.
`logs\aot-<app>.log` shows the progress. [More](arm-apps.md#how-arm-only-apps-run)

**No sound in a native game.** Its engine plays sound through OpenSL ES, which
is not implemented yet. AAudio and Java audio work.

**A video does not play, or plays silent.** Windows decodes the video: H.264
with AAC always; HEVC, VP9 and AV1 only with Microsoft's codec packs from the
Microsoft Store. A stream whose audio is Opus, Vorbis or FLAC plays silent in an
ExoPlayer app; video from an app's native code (`AMediaCodec`) is not there yet.

**Web pages inside the app are empty, or Google sign-in does not open.** Both
need the Microsoft Edge WebView2 Runtime. Install it from Microsoft if your PC
lacks it.

**"Sign in with Google" fails, or the app asks for Google Play services.** The
app may need Google's own Play services: see [Google services](google-services.md).
In-app purchases and Play Integrity do not work.

**The camera, location or Wi-Fi names are missing.** Windows' privacy switches
apply: Settings > Privacy & security > Camera, Microphone or Location, with
access for desktop apps on. Wi-Fi network names need location access too, as on
Android.

**The layout is wrong, or the app is sideways.** Try another **Screen size** in
Settings, or turn the app the other way on its details page (**Window >
Orientation**). Dragging the window's edge also resizes the app.

**The app asks for something Windows cannot give** (notification access,
accessibility, usage access). The dialog says so and the app is told no; the
rest of the app usually works.

**I want to start over.** Remove the app with **Also delete what the app
saved** ticked, then install it again. To reset everything, uninstall Win.apk
and delete `%LOCALAPPDATA%\Win.apk`.

## Reading the log

Each run of an app writes `%LOCALAPPDATA%\Win.apk\logs\run-<app>.log` (the run
before it is `run-<app>.1.log`). The most useful lines:

| Line | Meaning |
|---|---|
| `native not implemented on Windows yet` | A piece of the Android host that is still missing |
| `not implemented on Windows yet` (about a system function) | A C library function the app's native code needs |
| `[win.apk] cannot load <library>` or `UnsatisfiedLinkError` | A native library did not load: 32-bit ARM only (`this is a 32-bit ARM library`), packed or encrypted, or for another CPU |
| `Exception` or `FATAL` | The app crashed; the lines after it say where |
| `[win.apk] crash:` and `crash dump:` | The app's native code crashed. A minidump was written next to the log (`crash-<app>-<time>.dmp`): attach it too |
| `[win.apk] arm64 AOT: off for this app` | An ARM-only app crashed twice with its translation, so it runs on the JIT ([details](arm-apps.md#if-an-arm-app-crashes)) |
| `[win.apk]` | Any other message from Win.apk itself |

The [user guide](USER_GUIDE.md#logs) lists every log file.

## Reporting a problem

The quickest way is **Report a problem**: under the error in the launch window
or a failed install, and on the app's details page (Troubleshooting) for a
problem met inside the app. It first asks, with a link to the open reports
that name the same app, so you can add to one instead of opening a second.
Then:

- your browser opens GitHub's **An app does not work** form with Win.apk's
  version, the Windows version, the graphics adapter and the app's name,
  version and package already filled in (you need a GitHub account to post);
- Explorer opens the logs folder with the app's log selected, and its newest
  crash dump when there is one, ready to drag into the form.

Nothing is uploaded by itself. A log can contain your Windows user name and
file names: look it over first.

What makes a report useful:

- where the APK came from (F-Droid, APKMirror, the app's site) and which
  variant (x86_64, arm64-v8a, a bundle);
- the exact steps, and what you expected;
- a screenshot at the moment it goes wrong;
- **the whole log**, not a part of it.

You can write in English or in Hebrew. Security problems go to
[SECURITY.md](../SECURITY.md), not to a public issue.
