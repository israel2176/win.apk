# Contributing to Win.apk

Thanks for helping. The three easy ways in: report how an app runs, add what
you learned to [`docs/APPS.md`](docs/APPS.md), or send code. And if Win.apk is
useful to you, please **star the repository** (the ⭐ at the top of the page):
it helps other people find it and tells us what to keep working on.

- [Report a problem](#report-a-problem)
- [Add an app to the list](#add-an-app-to-the-list)
- [Send code](#send-code)
  - [Before you start](#before-you-start)
  - [The pull request](#the-pull-request)
  - [How the code is meant to work](#how-the-code-is-meant-to-work)
  - [Documentation](#documentation)
  - [Out of scope](#out-of-scope)
- [Security problems](#security-problems)

## Report a problem

[Open an issue](../../issues/new/choose) with the "An app does not work"
template. It is fine to write in Hebrew. A report that can be acted on has
four things:

1. **Versions.** Win.apk (Settings > About), Windows, the app and its version,
   and where the APK came from (the Store tab, f-droid.org, an `.apkm` from
   somewhere else). The same app from two sources can be two different builds.
2. **Steps to reproduce.** Exact steps from a fresh start, numbered, so
   someone else can follow them and see the same thing: how you installed the
   app, what you clicked, what you typed. Say whether it happens every time.
   If it needs an account or data, say what kind. Before posting, try it once
   more after right-click > Reinstall, to rule out a broken install.
3. **A real test, with a picture.** Run the app and attach a screenshot of its
   window at the point it goes wrong (Win+Shift+S, then paste into the issue),
   or a short recording for anything that moves or flickers. "It doesn't
   work" without a picture is hard to tell apart from ten other problems; a
   picture plus the log usually pins it down. For a layout or text problem
   (wrong size, Hebrew direction, missing icons) the screenshot is the
   report.
4. **The log.** `%LOCALAPPDATA%\Win.apk\logs\last-run.log` is the app opened
   most recently; `install-<app>.log` is its install. **Logs** at the bottom of
   the library opens them, and **Copy** puts one on the clipboard. Attach the
   whole log, not the last lines; a long one is fine. Look it over first: it
   can contain file names and the app's own output. A screenshot of the log is
   not a log.

The most useful lines in a log:

| Line | Meaning |
|---|---|
| `native not implemented` | A Windows piece that is still missing |
| `not implemented on Windows yet` | A system function the app's native code needs |
| `[win.apk] cannot load <library>` or `UnsatisfiedLinkError` | A native library did not load (often ARM-only: see `docs/APPS.md`) |
| `Exception` or `FATAL` | The app crashed; the lines after it say where |
| `[win.apk]` | A message from Win.apk itself |

Before opening an issue, look through [`docs/APPS.md`](docs/APPS.md) and the
open issues for the same app: add to an existing issue rather than opening a
second one. One app or one problem per issue.

Security problems go to [SECURITY.md](SECURITY.md), not to a public issue.

## Add an app to the list

[`docs/APPS.md`](docs/APPS.md) is the list of common apps, how they run, and
the problems already known. Anyone can add a row: the app, its version, where
it came from, what works and what does not, and the issue number if there is
one. A row needs a real run of that version, not a guess, and the date of the
test. Send it as a pull request, or put it in an issue and it will be copied
over.

## Send code

### Before you start

- Read [`windows/README.md`](windows/README.md) and get a build running;
  [`CLAUDE.md`](CLAUDE.md) is the map of the repository.
- Look for an open issue or pull request on the same thing. For a large change,
  open an issue first and say what you plan, so the work is not done twice.

### The pull request

1. Branch from `main`, one topic per branch. A bug fix and a refactor are two
   pull requests.
2. Commit messages are in English and say **why** the change was made, not
   only what. The first line is a short summary (`Shim: malloc_usable_size`,
   `Launcher: open apps in landscape`); the body has the reasoning and, for a
   framework change, what AOSP does.
3. Open the pull request against `main`. The template asks for what changed,
   why, and **how it was tested**: the apps you ran it on and what you saw,
   with a screenshot for anything visible (a before and after, in Hebrew too
   when it touches the library) and, for a runtime change, the log lines that
   show the fix. Link the issue it fixes (`Fixes #123`). A draft pull request
   is fine while it is in progress.
4. CI must be green: `windows-poc.yml` builds everything with clang on
   Windows, runs the unit tests and the headless boot, and packages the
   result; `compat-sweep.yml` boots the app corpus. A test for the change is
   expected where there is a place for one (`windows/test/`).
5. Reviews happen on the pull request. Keep follow-up fixes as new commits
   until it is approved; it is merged with rebase.

### How the code is meant to work

- **Behave like Android.** Framework classes under `runtime/java/android/...`
  mirror AOSP's public API and observable behaviour. Check the AOSP source for
  the same class before changing one.
- **No app-specific code.** No branches on a package name, no offsets that only
  fit one app's binary. Fix the framework, shim or host so every app gets the
  right behaviour.
- **No emulation or virtualization.** Java runs on HotSpot, x86_64 native code
  runs as is, and the bridge is at the Bionic libc ABI, not at Linux syscalls.
- **Windows code lives in `windows/`.** Change `runtime/` only where Windows
  needs a hook, and list the change in `windows/README.md`.
- **Every string the user sees is translated.** In the launcher, text goes
  through `tr()` or `fmt()` with a Hebrew entry in the table; the installer
  has an English and a Hebrew side.
- **Tell the user.** A change a user would notice goes in
  `windows/package/RELEASE_NOTES.md`.
- **License-clean dependencies.** BSD, MIT, Apache-2.0 and similar are fine.
- **Comments explain why.** Especially for ABI work: put the Bionic and Windows
  layouts side by side above a shim.
- **Style:** C++17, clang only, 120 columns, matching the file around you.

### Documentation

A change is done when the documents that describe it are updated in the same
pull request. Where things are written down:

| Document | What it covers |
|---|---|
| [`README.md`](README.md) | What Win.apk is and how to use it: install, the Store, settings, logs, what works |
| [`windows/README.md`](windows/README.md) | Building, running and testing; every change made under `runtime/` |
| [`windows/PORTING.md`](windows/PORTING.md) | Design, findings and the milestone plan |
| [`docs/APPS.md`](docs/APPS.md) | Apps that were tried, and known issues by kind of app |
| [`windows/package/RELEASE_NOTES.md`](windows/package/RELEASE_NOTES.md), [`README.txt`](windows/package/README.txt) | What a user of a release sees: new since last time, and the shipped read-me |
| [`CLAUDE.md`](CLAUDE.md) | The map of the repository and its conventions, for people and for AI assistants |

Write for the reader who was not there: say what a thing does and why, in
plain sentences, and keep the Hebrew side of a user-facing text in step with
the English.

### Out of scope

Pull requests that defeat license checks or copy protection, forge Play
Integrity or other attestation tokens, bypass account or payment security, or
help obtain apps you don't have the right to use will be closed.

## Security problems

See [SECURITY.md](SECURITY.md). Please don't open a public issue for those.
