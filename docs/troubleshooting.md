# Troubleshooting and reporting a problem

**🇮🇱 [לדף הזה בעברית — לחצו כאן](he/troubleshooting.md)**

Win.apk does not run every app yet. This page helps you tell a
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

**Windows protected your PC (SmartScreen) when running the installer.** Win.apk
is not code-signed yet. Click **More info**, then **Run anyway**, if
you downloaded it from this repository's
[Releases](https://github.com/israel2176/win.apk/releases) page.

**An antivirus flags Win.apk or an app.** Win.apk writes code (the ARM
translation) and then runs it, which some scanners dislike, and Win.apk is
not signed yet. Allow it if you trust the download.

**The app does not start.** An app with only 32-bit ARM code installs but
stops as soon as it loads that code; its log says `this is a 32-bit ARM library
(armeabi-v7a)`. Otherwise the launch window turns into a message with
**Show log** and **Report a problem**. Read the end of the log
([below](#reading-the-log)). The first start of a new or updated app converts
its code and can take up to a minute.

**"Wiz has stopped" or "Wiz keeps stopping".** The app crashed: an error it
did not handle, or a fault in its native code. Its log ends with
`FATAL EXCEPTION` (and the error and where it happened) or a `crash dump:` line.
**Show log** and **Report a problem** are under the title; report it with the
log. [More](USER_GUIDE.md#when-an-app-crashes-or-stops-responding)

**"Wiz isn't responding".** The app was given a click, a key or text and did
not take it for five seconds: its main thread is busy or stuck. **Wait** gives
it more time; **Close app** ends it. The log has `ANR: input event ... not
taken by the main thread` from `Win.apk.InputTimeout`. If it happens each time
at the same place, report it with the log.

**The install failed.** The install window says why in a sentence; **Show log**
opens the install log and **Copy details** copies what a report needs.

**The app is slow.** If it has only ARM64 code, it runs on the JIT until its
translation is ready; on a busy PC a large app's first translation can take an
hour or more. Leave the PC alone for a while, or install an x86_64 build.
`logs\aot-<app>.log` shows the progress. [More](arm-apps.md#how-arm-only-apps-run)

**No sound in a native game.** AAudio, OpenSL ES and Java audio all play
through Windows' default output device. If a game is still silent, look in its
log for `[opensles]` or `[aaudio]` lines that say what it asked for and did not
get (an effect, a MIDI player), and report it with the log.

**A video does not play, or plays silent.** Windows decodes the video: H.264
with AAC always; HEVC only with Microsoft's codec pack from the Microsoft Store.
In apps that decode for themselves (ExoPlayer and others), VP8, VP9 and AV1
video and Opus sound play without any codec pack, on decoders built into
Win.apk; they are slower than the codec packs, so a 1080p VP9 or AV1 video can
stutter on a slow PC (the log says `decoding with libvpx VP9` or `dav1d AV1`).
A video played by `MediaPlayer` or `VideoView` still needs the codec packs for
VP9 and AV1, and does not play from a WebM file. A stream whose audio is Vorbis
or FLAC plays silent in an ExoPlayer app; video from an app's native code
(`AMediaCodec`) is not there yet.

**A phone photo (HEIC) or an AVIF picture does not show.** Windows decodes
them: HEIC/HEIF needs Microsoft's HEIF Image Extensions, AVIF its AV1 Video
Extension, both from the Microsoft Store. Without them the app is told the
picture cannot be read, and its log says `HostImageDecode: HEIF picture:
Windows has no decoder for it`.

**An app cannot record, or its recording is empty.** Apps record sound as AAC
and video as H.264 into MP4 files, with Windows' own encoders; a few Windows
editions (N editions without the Media Feature Pack, some Server installs) have
none, and the app is then told so. Recording a screen or a game drawn with
OpenGL, and recording in AMR, WebM or Ogg, are not there yet.

**Web pages inside the app are empty, or Google sign-in does not open.** Both
need the Microsoft Edge WebView2 Runtime. Install it from Microsoft if your PC
lacks it.

**"Sign in with Google" fails, or the app asks for Google Play services.** The
app may need Google's own Play services: see [Google services](google-services.md).
In-app purchases and Play Integrity do not work.

**The Play Store keeps showing "Server error" (DF-DFERH-01).** Play services
has not checked in with Google yet, so the store has no device id (its log says
`Unexpected android-id = 0`). The check-in's own log is
`logs\services\com.google.android.gms.log`: look for `Checkin failed`. A
`PKIX path building failed` there means a server's certificate was refused;
the line tagged `MacApkTrust` just before it names the server, the certificates
it sent and who issued them, which is what to attach to a report.

**Gmail or the Play Store says "This device isn't Play Protect certified".**
Win.apk is not a certified device and does not pretend to be. Register its
device ID with your Google account on Google's own page: **Settings > Google
account > Device ID for Google**, **Copy**, then **Register this device**
([the steps](google-services.md#play-protect-certification)). No such row:
Google services are off, or have not checked in with Google yet (open the
Play Store once).

**The camera, location or Wi-Fi names are missing.** Windows' privacy switches
apply: Settings > Privacy & security > Camera, Microphone or Location, with
access for desktop apps on. Wi-Fi network names need location access too, as on
Android.

**The layout is wrong, or the app is sideways.** Try another **App window size** in
Settings, or turn the app the other way on its details page (**Window >
Orientation**). Dragging the window's edge also resizes the app.

**The app asks for something Windows cannot give** (notification access,
accessibility, usage access). The dialog says so and the app is told no; the
rest of the app usually works.

**I want to start over.** Remove the app with **Also remove accounts and
data saved by the app** ticked, then install it again. To reset everything, uninstall Win.apk
and delete `%LOCALAPPDATA%\Win.apk`.

## Reading the log

Each run of an app writes `%LOCALAPPDATA%\Win.apk\logs\run-<app>.log` (the run
before it is `run-<app>.1.log`). The most useful lines:

| Line | Meaning |
|---|---|
| `native not implemented on Windows yet` | A piece of the Android host that is still missing |
| `not implemented on Windows yet` (about a system function) | A C library function the app's native code needs |
| `not implemented on Windows yet, called this run (<n>):` | At the end of the log: every missing piece the app reached in this run, with how many times it called it, the busiest first. The lines above name each one once, when it is first called; this list says which ones the app kept asking for. Include it when you report a problem. An app that was stopped by force (Task Manager, Stop) ends without it |
| `there is no /system/bin/sh on Windows` | The app's native code ran a shell command (`popen`, `system`). There is no Android shell, so the command reports "not found" (exit status 127), as on a phone without one |
| `fork(): the app tried to start a child process` (also `posix_spawn()`, `exec()`, `clone()`) | The app's native code tried to run part of itself in another process (Firefox-family browsers do). Win.apk does not support that yet, so such an app usually ends at start |
| `[win.apk] <call>(): ... the call fails with <error>` | The app's native code used a C library call Win.apk does not do yet: a pseudo-terminal (`openpty()`), watching files (`inotify_init()`), `signalfd()`, `iconv_open()`, `regcomp()`, `symlink()`, `mremap()` of a file's mapping, `mlockall()`, `glob()`, `nftw()`, `splice()`, `shmget()`, `gethostbyname_r()`. The call fails the way Android reports such a failure, and the line is written once. Most apps carry on another way; when the feature that needs it does not work, this line says why |
| `sendmsg(): ancillary data ... is not sent` | The app's native code passed a file descriptor or credentials over a socket; only the message's bytes arrive |
| `the app set an alarm timer; its signal (SIGALRM) is not delivered yet`, `guest created a POSIX timer; timers do not fire yet` | The app's native code set a timer that would interrupt it with a signal. The timer is accepted and never fires, so something that waits for it does not happen |
| `TextToSpeech: there is no speech engine here yet` | The app asked Android to read text aloud. Nothing is spoken; the app is told that the utterance failed |
| `MidiManager: there are no MIDI devices here yet` | The app tried to open a MIDI device; it is told that the open failed |
| `[win.apk] cannot load <library>` or `UnsatisfiedLinkError` | A native library did not load: 32-bit ARM only (`this is a 32-bit ARM library`), packed or encrypted, or for another CPU |
| `FATAL EXCEPTION: <thread>` | The app crashed: an exception nothing caught, on that thread; the lines after it say what and where. The app then ends, as on a phone, and the launcher shows **Wiz has stopped** |
| `Exception` | An error the app reported; it may have handled it |
| `Win.apk.InputTimeout: ANR:` | The app did not take the input it was given for five seconds; the launcher asked **Wiz isn't responding**, and the next line says the answer (`Close app` or `Wait`) |
| `[win.apk] crash:` and `crash dump:` | The app's native code crashed. A minidump was written next to the log (`crash-<app>-<time>.dmp`): attach it too |
| `[win.apk] guest called abort(); abort message: <text>` | The app's native code ended the app on purpose, and the text says why (what Android shows as "Abort message" in a crash report): `terminating due to uncaught exception of type ...` is a C++ error nothing caught. A `crash dump:` line follows |
| `[win.apk] android.icu: Windows' ICU (icu.dll) is not available; ...` | This Windows has no `icu.dll` (Windows 10 before version 1903, or Wine). Apps keep working: numbers are still written the language's way, but units are written with their international symbols ("5 km", "3 h") rather than in the language's own words, and some locale details are generic |
| `[win.apk] arm64 AOT: off for this app` | An ARM-only app crashed twice with its translation, so it runs on the JIT ([details](arm-apps.md#if-an-arm-app-crashes)) |
| `[win.apk] AOSP graphics:` | `on` is Android's own graphics and text, the only drawing code Win.apk has. `[win.apk] error: AOSP graphics: ... is missing` or `does not load`, then `cannot start`, means this copy of Win.apk lacks those files or could not load them, and nothing starts: reinstall Win.apk |
| `[win.apk] cds:` and `[cds]` | Class-data sharing: which archives the start maps, and HotSpot's reason when one does not map (Win.apk then deletes it and makes it again; the start goes on without it) |
| `[E] launcher: could not start ...` or `could not copy ...` | The library itself failed before Win.apk's runtime ran, with Windows' own reason: the runtime could not be started (an antivirus blocked it, a damaged install: reinstall Win.apk), or the app file could not be copied into the library (a full disk, a file another program holds) |
| `[win.apk] audio:` | The sound output: `no output device` (sound plays silently and Win.apk looks for a device again every few seconds), and when the device went away or is no longer the default, the new default being opened |
| `[win.apk] microphone: cannot open` | The app asked for the microphone and Windows did not give it, with the step that failed. `IAudioClient` usually means microphone access is off for desktop apps in Windows Settings > Privacy & security > Microphone |
| `[win.apk] close:` | The window was closed: `the app was paused, stopped and destroyed in <n> ms` is the normal end; `the app did not answer in time` means its main thread was busy or stuck, so it was ended without the chance to save |
| `[win.apk] java.nio sockets: <folder> refuses AF_UNIX connections, using <folder>` | Windows would not connect the local sockets Java makes in the temporary folder (seen where a filter driver guards `%LOCALAPPDATA%`), so Win.apk pointed Java at another folder. Nothing to do; without it apps that use Ktor or another `java.nio` selector failed at start with `Unable to establish loopback connection` |
| `[win.apk] startup:` | When the Java VM was up and when the first frame came, with the memory then; then how busy the whole PC was until that first frame, the app's own share, and the other programs that used the most processor time meanwhile (`busiest others`). A slow start with the PC near 100% busy and another program at the top (an antivirus scan, Windows Update) was slowed by that program, not by the app |
| `[win.apk] webview2:` | The WebView2 Runtime found (`found in <n> ms`), and when an app first shows a web page, its folder and how long WebView2 took to start (`environment ready in <n> ms`; the first time on a PC it sets up its profile and takes longer). The app's own screen does not wait for it |
| `[win.apk] frame times (<graphics> graphics):` | At the end of the log: how many frames the app drew and how long they took, in milliseconds (the median, the slowest 10% and 1%, and the slowest): `layout` and `draw` are the app's screen being laid out and drawn, `pixels` and `present` reading the picture and copying it to the window, `total` the whole frame. The first second and a half of each window is left out. A slow or jerky app shows it here; include it when you report one. An app that was stopped by force ends without it |
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
- a screenshot at the moment it goes wrong; for text or pictures that look
  blurry or broken, press **Ctrl+Shift+F12** in the app's window instead: it
  saves the app's own picture and the window as shown, side by side, in the
  logs folder (`snapshot-...-frame.png`, `snapshot-...-window.png`), and writes
  the numbers between the two into the log;
- **the whole log**, not a part of it.

You can write in English or in Hebrew. Security problems go to
[SECURITY.md](../SECURITY.md), not to a public issue.
