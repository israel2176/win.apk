# Contributing to Win.apk

Thanks for helping. The three ways in: report how an app runs, add what you
learned to [`docs/APPS.md`](docs/APPS.md), or send code. If Win.apk is useful
to you, a star on the repository helps other people find it.

- [Report a problem](#report-a-problem)
- [Add an app to the list](#add-an-app-to-the-list)
- [Send code](#send-code)
- [Security problems](#security-problems)

## Report a problem

[Open an issue](../../issues/new/choose) with the "An app does not work"
template. Hebrew is fine. A report that can be acted on has four things:

1. **Versions.** Win.apk (Settings > About), Windows, the app and its version,
   and where the APK came from (the Store tab, f-droid.org, an `.apkm` from
   somewhere else). The same app from two sources can be two different
   builds.
2. **Steps to reproduce.** Numbered, from a fresh start: how you installed
   the app, what you clicked, what you typed, whether it happens every time.
   Try once more after right-click > **Reinstall** first, to rule out a
   broken install.
3. **A screenshot** of the app at the point it goes wrong (Win+Shift+S), or
   a short recording for anything that moves or flickers. For a layout or
   text problem the screenshot is the report.
4. **The log**, attached whole: `%LOCALAPPDATA%\Win.apk\logs\last-run.log`
   for a run, `install-<app>.log` for an install. **Logs** at the bottom of
   the library opens them and **Copy** puts one on the clipboard. Look it
   over first; it can contain file names and the app's own output.

The [user guide's troubleshooting section](docs/USER_GUIDE.md#troubleshooting)
explains the common log lines. Before opening an issue, look through
[`docs/APPS.md`](docs/APPS.md) and the open issues for the same app, and add
to an existing issue rather than opening a second one. One app or one problem
per issue.

## Add an app to the list

[`docs/APPS.md`](docs/APPS.md) is the list of apps people have tried, how
they run, and the problems already known. Anyone can add a row: the app, its
version and package name, where it came from, what works and what does not,
the date, and the issue number if there is one. A row needs a real run of
that version, not a guess. Send it as a pull request, or put it in an issue
and it will be copied over.

## Send code

### Before you start

- Read [`windows/README.md`](windows/README.md) and get a build running.
  [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) explains how the pieces fit
  and [`CLAUDE.md`](CLAUDE.md) is the map of the repository.
- Look for an open issue or pull request on the same thing. For a large
  change, open an issue first and say what you plan.

### The pull request

1. Branch from `main`, one topic per branch. A bug fix and a refactor are two
   pull requests.
2. Commit messages are in English and say **why** the change was made. The
   first line is a short summary (`Shim: malloc_usable_size`, `Launcher: open
   apps in landscape`); the body has the reasoning and, for a framework
   change, what AOSP does.
3. Open the pull request against `main`. The template asks what changed, why,
   and how it was tested: the apps you ran it on, a screenshot for anything
   visible (in Hebrew too when it touches the library), and for a runtime
   change the log lines that show the fix. Link the issue (`Fixes #123`).
4. CI must be green: `ci.yml` builds everything with clang on Windows, then
   runs the tests, the headless boot and a smoke test of ten apps side by
   side and packages the result. Before a broad change to the framework or
   the shim, run the whole corpus too: Actions > CI > Run workflow on your
   branch. Add a test where there is a place for one
   (`windows/test/`).
5. Reviews happen on the pull request. Keep follow-up fixes as new commits
   until it is approved; it is merged with rebase.

### How the code is meant to work

- **Behave like Android.** Framework classes under `runtime/java/android/...`
  mirror AOSP's public API and observable behaviour. Check the AOSP source
  for the same class before changing one.
- **No app-specific code.** No branches on a package name, no offsets that
  only fit one app's binary. Fix the framework, shim or host so every app
  gets the right behaviour.
- **No emulation or virtualization.** Java runs on HotSpot, x86_64 native
  code runs as is, and the bridge is at the Bionic libc ABI, not at Linux
  syscalls.
- **Windows code lives in `windows/`.** Change `runtime/` only where Windows
  needs a hook, and list the change in `windows/README.md` under "Changes to
  the upstream tree".
- **Every string the user sees is translated.** In the launcher, text goes
  through `tr()` or `fmt()` with a Hebrew entry in the table; the installer
  has an English and a Hebrew side.
- **License-clean dependencies.** BSD, MIT, Apache-2.0 and similar.
- **Comments explain why.** For ABI work, put the Bionic and Windows layouts
  side by side above a shim.
- **Style:** C++17, clang only, 120 columns, matching the file around you.

### Documentation

A change is done when the documents that describe it are updated in the same
pull request:

| Document | What it covers |
|---|---|
| [`README.md`](README.md) | What Win.apk is, what it solves, how it works at a glance, what works |
| [`docs/USER_GUIDE.md`](docs/USER_GUIDE.md) | What a user does and sees: installing, the Store, settings, permissions, logs, the command line |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | How the layers work, an install and a run step by step, the data on disk |
| [`docs/APPS.md`](docs/APPS.md) | Apps that were tried, and known issues by kind of app |
| [`windows/README.md`](windows/README.md) | Building, running and testing; every change made under `runtime/` |
| [`windows/PORTING.md`](windows/PORTING.md) | Design decisions, findings, known gaps and the milestone plan |
| [`windows/package/RELEASE_NOTES.md`](windows/package/RELEASE_NOTES.md), [`README.txt`](windows/package/README.txt) | What a user of a release sees: what works, what is new, the shipped read-me |
| [`CLAUDE.md`](CLAUDE.md) | The map of the repository and its conventions, for people and for AI assistants |

Write for the reader who was not there: say what a thing does and why, in
plain sentences, and keep the Hebrew side of a user-facing text in step with
the English. Every claim in the documents must match the code.

### Out of scope

Pull requests that defeat license checks or copy protection, forge Play
Integrity or other attestation tokens, bypass account or payment security, or
help obtain apps you do not have the right to use will be closed.

## Security problems

See [`SECURITY.md`](SECURITY.md). Please do not open a public issue for those.
