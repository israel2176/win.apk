# ARM apps and which build to install

Phones use ARM processors; a PC uses x86-64. Most Android apps are written in
Java or Kotlin and run anywhere, but many also carry *native libraries*,
machine code for one kind of processor, in the APK's `lib/<abi>/` folders.
Which of those folders an APK has decides how well it runs in Win.apk.

- [Which build to install](#which-build-to-install)
- [How ARM-only apps run](#how-arm-only-apps-run)
- [If an ARM app crashes](#if-an-arm-app-crashes)
- [Settings and variables](#settings-and-variables)

## Which build to install

| The APK or bundle has | In Win.apk | Choose it? |
|---|---|---|
| No `lib/` folder (Java or Kotlin only) | Runs directly | **Yes** |
| `lib/x86_64` | Loads it directly, at full speed | **Yes, the best choice** |
| `lib/arm64-v8a` only | Runs at once on an ARM JIT, slower, while the code is translated for your PC in the background | If there is no x86_64 build |
| `lib/armeabi-v7a` only | **Does not run** (32-bit ARM) | **No** |

In practice:

1. **Prefer an x86_64 build, or one with no native code.** On APKMirror pick
   the variant marked `x86_64` (or `noarch` / `universal`); on F-Droid, the
   build listed for x86_64. The Store in Win.apk does this for you.
2. **A bundle (`.apkm`, `.xapk`, `.apks`) that includes an x86_64 split is
   fine**: Win.apk installs the x86_64 split and leaves the ARM ones out, as a
   phone installs only its own.
3. **Otherwise take `arm64-v8a`.** It works, and gets faster after its first
   runs.
4. **Never `armeabi-v7a` alone.** If that is the only build of an app, it will
   not run in Win.apk today.

Why x86_64 first: x86_64 code runs as it is, with nothing to translate, no
waiting on the first runs and no compiler working in the background.

You can see what an APK holds by opening it as a zip file (rename a copy to
`.zip`) and looking in `lib/`.

## How ARM-only apps run

When an app has only 64-bit ARM libraries, Win.apk's translator
(`winapk-aot`) turns each library into C, and the clang compiler shipped with
Win.apk compiles it for your processor into a DLL next to the library.

- **Nothing waits for it.** The app starts at once on an ARM64 JIT, and each
  library switches to its translation as soon as it is ready, even while the
  app is running.
- **Most libraries are translated whole, once.** A library of up to 1 million
  instructions is translated in one go. A larger one (a browser engine, a big
  app's own code) is translated only where the app's code actually ran, once
  the app has been quiet for a minute or is closed, and again as you use more
  of it, at most once an hour. So the first runs of a big app are the slow
  ones. Its details page then says that an x86_64 build, if one exists, runs
  faster.
- **It keeps out of your way.** Lowest priority; at most one processor core
  while you use the PC, more only while you are away; none on battery saver or
  while a full-screen program (a game, a presentation) runs; it compiles less
  at once when memory is short. Stopped by a shutdown, it goes on where it was.
- **It is shared and cleaned up.** A library two apps share is translated once.
  The shared folder (`%LOCALAPPDATA%\Win.apk\aot-cache`) stays under 2 GB and
  drops what no app used for two months. A translation is made for your
  processor; on a different PC it is made again.
- **Some code stays on the JIT.** A library that rewrites its own code while
  it runs (some anti-cheat code, some packed apps) is not translated.
- **Failures are retried.** A translation that fails is tried again later;
  after four failures that library stays on the JIT until the app or Win.apk
  is updated. `logs\aot-<app>.log` says why.

While a translation runs, the app's details page shows **Optimizing for this
PC…** under Performance.

What it buys, measured on arm64-only F-Droid apps in CI: translated code drew
the same screens as the JIT, with less memory at peak (Jitsi Meet 516 MB
against 742 MB on the JIT, Telegram X 412 MB against 521 MB) and up to half
the time spent in ARM code. [`docs/research/aot-measurements.md`](research/aot-measurements.md)
has the details.

## If an ARM app crashes

If an ARM-only app crashes twice in a row while translated code is loaded,
Win.apk suspects the translation: from the next start that app runs on the
JIT only, slower but without it. Only real crashes count, not closing the app,
Stop, Remove or ending it in Task Manager. The app's log says so at that start:

```
[win.apk] arm64 AOT: off for this app: it crashed 2 starts in a row with translated code loaded (...); the JIT runs it. Delete ...\apps\<app>\aot-guard.txt to try the translation again.
```

To try the translation again, delete the file the line names; an update of the
app or of Win.apk also clears it. If the app then crashes twice in a row on the
JIT as well, the translation was not the cause: the log says `crashes on the
JIT too: not the translation`, and the translation is used again.

## Settings and variables

| | Effect |
|---|---|
| Settings > Apps > **Optimize ARM apps in the background** (on) | Off: nothing new is translated (a translation under way stops before its next part and resumes once this is on again); translations already made are still used |
| `WINAPK_AOT=0` | Run ARM code on the JIT only: no translation is loaded or made |
| `WINAPK_AOT_CLANG=<path to clang.exe>` | Use another clang for the translation. Without the bundled one, Win.apk also looks for `clang.exe` on the `PATH` |
| `WINAPK_ABIS=arm64-v8a` | Install an app's ARM build although it has an x86_64 one, to compare the two |
