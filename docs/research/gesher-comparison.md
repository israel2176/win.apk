# Win.apk and the other runtime: what each can give the other (2026-11-23)

the other runtime (https://example.invalid/other-runtime, read at 52513f0) runs real ART ported to Windows,
Android 16's own framework.jar, HWUI+Skia on ANGLE, real Binder with servicemanager and its own system
server, and microG. Read-only comparison (code and docs; nothing of the other runtime was built or run here).

## Where Win.apk could contribute to the other runtime (best value first)

1. **Bundle installs.** the other runtime's `app/java/com/other/server/Installer.java` decides a bundle by suffix
   (.xapk/.apks), flattens entries, takes splits only named `split_*`/`config.*`: no `.apkm`, bundletool
   `.apks` splits/standalones mixed, an ".xapk" that is an APK fails, `manifest.json`'s base ignored.
   Win.apk's `windows/java/com/winapk/tools/AppInstall.java` (APK by content, base by manifest.json /
   base.apk / package / largest, bundletool layout, device split names) ports as Java into the other runtime's server.
   Not its cross-ABI library fallback.
2. **App corpus by kind.** the other runtime's e2e has 10 simple F-Droid apps, no Compose/Flutter/RN/libGDX/SDL/
   WebView-shell; Win.apk's `windows/test/compat/apps.txt` (~90) and `smoke.txt` + `sweep.py` blocker ranking.
3. **Size budget in CI** (`windows/scripts/footprint_check.py`); the other runtime has none.
4. **Differential UI oracle** (`windows/test/screens`, `windows/test/uiprobe` regress/gate): the other runtime has
   pixel/lifecycle smoke only. LayoutSweep would become a public-API test APK there.
5. **Hebrew user docs** (`docs/he/`), rewritten for the other runtime.
6. **Native fixes:** raw arm64 syscalls the other runtime answers ENOSYS (`compat/arm64/dll/guest_libc.cc`):
   membarrier, set_tid_address, rt_sigaction/procmask, mremap, prlimit64, clock_nanosleep, msync,
   mincore, set/getpriority (ptrace must not fake success); the exit hang on guest thread_local
   destructors (`windows/src/elf_tls.cpp`), to check in the other runtime.
7. **Persistent arm64 AOT cache + crash guard** (`arm64_aot.cpp`, `host/aot_guard.h`): the other runtime keeps
   translations per run; cost is clang's size against its "Light" principle.
8. Smaller: Report a problem; the frame bench.

## What the other runtime's policy would refuse

- "Report Google Play as the installer" (`InstallerOfRecord.java`): reports an installer that did not
  install the app, to pass an app's requirement; against the other runtime's CONTRIBUTING and docs/install-source.md.
- GMS stand-ins reporting Play services present; Win.apk's own sign-in window in place of the authenticator.
- The Store tab ("No store"); background Play updates ("nothing runs when no app is open"); hard-linked
  library copies of user files.

## What Win.apk should take from the other runtime

Installer download verification (digest, length, version); a signed F-Droid index; WebView assets served on
a synthetic https origin (and android_res) instead of copying pages; working OpenSL ES; software
VP8/VP9/AV1/HEVC decoders and WebM; more raw syscalls; exporting all ANGLE extension functions; its
evidence discipline (smoke vs functional, known failures); AppContainer isolation work; contract-first policy.

## Not verified

the other runtime's exit hang; other bundle paths in the other runtime; AOT gains on the other runtime's dynarmic; Control Center vs the
launcher in use; the other runtime's CI claims (taken from its docs).
