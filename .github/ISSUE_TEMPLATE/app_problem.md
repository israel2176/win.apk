---
name: An app does not work
about: An app that does not install, does not start, crashes, or misbehaves in Win.apk
title: "<App name> <version>: <what goes wrong in a few words>"
labels: app
---

<!--
Thanks for the report. Fill in what you can; the log and the exact steps are
what make a report fixable. It is fine to write in Hebrew (אפשר לכתוב בעברית).
Before posting, search the open issues and docs/APPS.md for the same app.
-->

## Versions

- **Win.apk:** <!-- Settings > About, or the installer's file name, e.g. 0.1.0-preview -->
- **Windows:** <!-- e.g. Windows 11 23H2, x64 -->
- **GPU / graphics:** <!-- e.g. Intel UHD 620, or "none / remote desktop" -->
- **App and version:** <!-- e.g. Markor 2.12.4 -->
- **Where the APK came from:** <!-- the Store tab (F-Droid), f-droid.org, an .apkm/.xapk from ..., the app's own site -->

## What happens

<!-- One or two sentences: what you expected, and what you got instead. -->

## Steps to reproduce

<!--
Exact steps from a fresh start, so someone else can follow them and get the
same result. Say whether it happens every time or only sometimes.
-->

1. Install the app (how: Store / drag the APK / double-click)
2. Open it
3. ...

## Screenshot or recording

<!--
Attach a screenshot of the app's window at the point it goes wrong (Win+Shift+S,
then paste here), or a short recording for anything that moves. A screenshot of
the Win.apk library with the error message counts too. Please do not post a
screenshot in place of the log: both are needed.
-->

## Log

<!--
Attach %LOCALAPPDATA%\Win.apk\logs\last-run.log (open it from Logs at the
bottom of the library; Copy puts it on the clipboard). If installing failed,
attach logs\install-<app>.log instead. Paste it between the ``` lines below or
attach it as a file; a long log is fine. Look it over first: it can contain
file names and the app's own output.
-->

```
(paste last-run.log here)
```

## Checklist

- [ ] I tried it again after a fresh install of the app (right-click > Reinstall)
- [ ] I checked `docs/APPS.md` and the open issues for this app
- [ ] The log is attached, not only a screenshot
