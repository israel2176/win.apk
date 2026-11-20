# Open issues, decisions and unfinished work

The memory between sessions: what was found and not fixed, what to check again
once a layer moves onto AOSP's code, and work that was started and not merged.
Read it at the start of a session on this project; update it in the same
change that fixes, verifies or adds an item. Found by the screens checks of
November 2026 (Maccabi, Cal, Moovit, Bit, Bank Hapoalim, Waze, Mercantile; the
agents' work folders were `/opt/uip/il-work/<app>/`, gone with the container).

## The owner's decisions (keep to them)

- Win.apk stays a program on the PC, not a virtual machine or an emulator.
- Positions, spacing and text metrics are **not hand-tuned**. They come right
  by running Android's own code: the layers move onto AOSP one by one
  (graphics and text: `windows/README.md` "Milestone 2"; resources and views:
  `docs/research/aosp-views.md`). A spot fix is only for something that
  breaks an app (a crash, an empty screen, a control that does not work), and
  then it copies what AOSP does. A text-metric "fix" that matched one app and
  broke another (the Paint top/bottom clamp, reverted in 75e88df) is the
  example of what not to do.
- Every change is gated: `windows/test/uiprobe/regress.py` (no REGRESSED,
  FARTHER or MISSING line), now also CI's UI gate. Heavy runs go to GitHub
  Actions (`ci.yml` UI gate, `ui-sweep.yml`), not the local machine.
- Win.apk stays light (`windows/README.md` "Footprint", CI's budget): one copy
  of every AOSP file per install, and a replaced Win.apk layer is deleted.
- The owner reads Hebrew only: answers to the owner are in Hebrew, with no
  English words at all where avoidable (not even "CI", "PR" or file names).
- Order agreed (November 2026): AOSP drawables and icons under the switch,
  and the missing-`icu.dll` crash; then make `WINAPK_AOSP_GFX` the default and
  **remove the old Java2D graphics engine** (and the Win.apk code each AOSP
  layer replaces). The PR is not merged soon: an engine change needs more
  testing first (apps on real Windows, the UI gate on more apps). **The
  default is flipped** on this branch (AOSP's graphics unless
  `WINAPK_AOSP_GFX=0`), so that CI's smoke test and UI gate test the new
  engine; deleting Java2D is the next step (below).

## Check again after each AOSP layer lands

Each of these differs from Android today and should disappear when the layer
named moves onto AOSP's code. After the switch, check it (the screens sweep or
the app); if it is still there, it is a bug in what replaced it.

| # | What | Where today | Found in | Layer |
|---|---|---|---|---|
| 1 | `ImageView` never sets a bitmap drawable's bounds (`getDrawable().getBounds()` 0x0; pixels right; AOSP `configureBounds`) | `android/widget/ImageView.java` bitmap path | all seven apps | views |
| 2 | The 2-argument `View(Context, AttributeSet)` constructor does not install the element's / `style=` background, so `Snackbar$SnackbarLayout` throws ("requires colorOnSurface") and becomes a FrameLayout; `Snackbar.make` would then hit a ClassCastException | `android/view/View.java` ~252, attributes applied by the inflater after construction | Maccabi, Cal, Bit, Mercantile | views |
| 3 | Attributes applied after construction in general (the root of 2 and of the typeface bug fixed in b82a102) | `LayoutInflater.applyAttribute` | all | views |
| 4 | `HorizontalScrollView.onLayout` in RTL: a scrollable TabLayout strip at x=-538 instead of 8 (AOSP: forceLeftGravity, `mScrollX = scrollRange` on first RTL layout) | `HorizontalScrollView.java` ~114 | Maccabi | views |
| 5 | Layout direction resolved before the view is attached (dir=1 on never-measured views; a ConstraintLayout child with its own direction resolves start/end the other way on the first pass) | `View.java` ~3800-3830, ~3956; `ViewGroup.addView` does not reset RTL properties | Maccabi, Hapoalim | views |
| 6 | `supportsRtl=false` RTL compatibility mode for margins missing (marginRight 20 + marginEnd 26: Android 20, Win.apk 26) | `ViewGroup.java` ~223 `resolveLayoutDirection` | Cal | views |
| 7 | `marginLeft` + `marginEnd`: the inflater resolves relative margins at once and zeroes the left one (Android 77, Win.apk 0) | `LayoutInflater.applyMargins` ~1885-1902 | Hapoalim | views |
| 8 | Views a custom view inflates in its constructor get plain TextView/RadioButton, not the Material ones (lineHeight and tint lost) | nested inflater has no factory; cause not confirmed | Moovit | views |
| 9 | Spinner / CheckedTextView miss their default background's padding; `CALCustomSeekBar` loses its right padding | not traced | Moovit, Cal | views |
| 10 | Text in an app's own font: Cal's Ploni lines 0.5-2 dp shorter than Android; cause unknown (not the top/bottom clamp) | text metrics | Cal | graphics + text |
| 11 | ~~Multi-line TextView 2-2.5 dp per line short with `WINAPK_AOSP_GFX=1`~~ Fixed: with the switch on TextView lays out through AOSP's own Layout (`TextViewAospLayout`); all of lib_text_lines matches. Still Win.apk's with the switch off (Java2D) | `TextView` line code (Java2D path) | uiprobe | text |
| 12 | `Html.fromHtml` ignores its flags (paragraph separators) and calls `TagHandler` only on close, for known tags, with a null XMLReader | `android/text/Html.java` ~68-78, ~230-250 | Waze | text |
| 13 | Waze's location-permission screen shows "1. Select Precise" without the second step; Html/StaticLayout give all lines on a plain JVM, so Compose text or the app's choice; not found | | Waze | check |
| 14 | Mercantile's login banner: its second line drawn wider than its box, first letter clipped (likely the typeface bug fixed in b82a102; verify) | | Mercantile | check |
| 15 | Win.apk is lenient where Android throws: a missing `layout_width` gets a default, an unknown or R8-removed class becomes a FrameLayout, `View.measure` swallows an `onMeasure` exception, an unresolved `?attr` int is accepted. Hides real crashes. | `LayoutInflater` ~1253, ~1534; `View.java` ~5332-5360 | Waze, Bit | views |
| 16 | `Resources.getResourceEntryName/TypeName/Name` invent `id_0x...` for an unknown id instead of throwing `NotFoundException`; also answer for a land-only layout in portrait | `Resources.java` ~5626-5702 | all | resources |
| 17 | ~~`android.widget.DateTimeView` missing (notification templates become FrameLayouts)~~ Added (AOSP 14's, `DateTimeViewTest`); the screens check to confirm the templates now inflate it | `android/widget/DateTimeView.java` | Waze, Hapoalim, Mercantile | views |
| 18 | Maccabi `item_service_request_interaction` RTL: `NonScrollListView` at y=95.5 where Android has 10 (and 0 wide); moved 1 dp further after includeFontPadding | constraint direction (5) | Maccabi | views |
| 19 | `LinearLayout.setGravity` stores the raw value; AOSP adds START / TOP when a direction is missing, so with `gravity="right"` `layoutHorizontal` takes its default branch and drops `layout_marginTop` (100+ cases) | `LinearLayout.java` ~191, ~1168 | Leumi | views |
| 20 | `RelativeLayout` rule precedence backwards: ALIGN_PARENT_* applied first and overwritten by sibling rules; AOSP applies the parent rules last (a timeline line 0 dp tall, 59 on Android; 18 children in 17 layouts) | `RelativeLayout.java` ~908-940, ~1361-1380 | Leumi | views |
| 21 | `View(Context, AttributeSet)` never sets the DataBinding tag (`"layout/..._0"`) from the element, so views that check `getTag()` keep a resource id Android replaces (two views with one id; 1,259 lines in 55 layouts). Part of item 3 | `View.java` ~227-233 | Leumi | views |
| 22 | NumberPickers inside the framework DatePicker get margins 8,16,8,16 where AOSP's legacy layout has 1,0,1,0 | | Leumi | views |

## Bugs to fix on their own (not part of a layer)

- **Exit hangs: fixed, the sweeps to check again** (bit, Waze, Maccabi). A
  guest library aborted in a `thread_local` destructor at process exit
  (Realm: `GetEnv` on a detached thread) and the process hung: `ExitProcess`
  calls the exiting thread's FLS callbacks, and `elf_tls.cpp` ran the guest
  destructors from there. Android runs no thread_local destructors at exit;
  `free_thread_blocks` now does nothing once ntdll says the process is
  shutting down (`RtlDllShutdownInProgress`, Wine's too) or the C runtime's
  `exit()` began (it also took `g_lock` there, which a killed
  thread may hold). `winapk-linker-test` starts itself twice with destructors
  pending on two threads and leaves by `exit()` and by `ExitProcess`: the old
  code printed the main thread's destructor, the new one none. A thread that
  ends on its own still runs them. Not changed: `LayoutSweep`'s
  `Runtime.halt(0)` still goes through ExitProcess (harmless now).
- **Raw arm64 system calls: fixed, Leumi to check again.** Leumi's
  protection library `libe689.so` (arm64) maps memory with a raw `SVC` 222
  (`mmap`), which the bridge answered `-ENOSYS`. `arm64_abi.cpp`'s
  `linux_syscall` now answers `mmap` 222, `munmap` 215, `mprotect` 226 (the
  import wrappers, so executable mappings register as guest code), `mremap`
  216 (ENOMEM, as the shim's) and the libc calls whose structs both ABIs
  share (ids, clocks, sleeps, `readlinkat`, `pread64`, `fcntl`, `ioctl`,
  `uname`, `ptrace`, `prlimit64`, `inotify_*`...), and the `syscall()`
  import passes a sixth argument. `winapk-arm64-test`'s
  `arm64_raw_syscalls`. Not answered: `getdents64`, `clone`, sockets; what
  Leumi's library calls next is not known.
- **DEX conversion's exit code.** An OOM-killed converter reported 0 under
  Wine and the run failed with "cannot replace classes.jar (File not found)"
  (`runtime_main.cpp` ~530-547): check that the temporary jar exists before
  trusting rc 0.
- **The dex conversion's heap is unlimited** (the JVM default, a quarter of
  RAM): 0.4-1.4 GB on a 16 GB machine. Give it a limit of its own.
- **The runtime's heap (`-Xmx1g`, `runtime_main.cpp` ~1492) filled after about
  3,100 layout inflations** in Mercantile's sweep: a leak in Win.apk or the
  app holding views; not found.
- ~~**AppsFlyer** looks up `com.appsflyer.internal.AFi1fSDKT861` where the class
  it loaded is `...T8615`~~ Fixed: the name's length is `35 + (getElapsedCpuTime()
  > 0)`, and `Process.getElapsedCpuTime()` was a constant 0. Found by running
  bit's AppsFlyer loaders (`AFi1hSDK`, `AFa1jSDK` static init) on a plain JVM
  over the converted classes with a JDI trace of every framework call they make;
  the same trace found `Process.getGidForName("")` (0, Android -1) and
  `ExpandableListView.getPackedPositionForChild` (type in bit 62, Android 63)
  derailing `AFa1jSDK`'s payload. All three fixed (`FrameworkStubsTest`); with
  them `AFi1hSDK.getCurrencyIso4217Code` returns `AFi1fSDKT8615`. A headless
  boot of bit under Wine with these jars: `AFi1fSDKT8615` loaded and, for the
  first time, AppsFlyer's second loader `AFa1cSDKSS` and its eleven classes
  (none in the November screens run).
- ~~**`HucsConnection.getRequestProperties`** returns the JDK's map with a
  null-key status line~~ Fixed: the JDK's map, after an error answer and
  `disconnect()`, held its request line as a key with a null value; the
  request side now answers as OkHttp's (`HucFollow.requestProperties`:
  `IllegalStateException` once sent, the app's fields only;
  `windows/test/java/com/macapk/net/HttpRequestPropertiesTest.java`). In a
  headless boot of bit under Wine Dynatrace now gets the exception, logs "can't
  calculate request size" and goes on, as on a phone.
- **No `icu.dll`, a crash: fixed (both halves), Leumi to check again.**
  `HostIcu` now answers from Java when the host has no ICU (`IcuFallback`:
  the locale's `java.text` number, CLDR root's unit symbols and territory
  lists; `windows/test/java/com/macapk/icu/HostIcuFallbackTest.java`), so
  Hermes' `Intl.NumberFormat {style: "unit"}` no longer throws under Wine or
  on Windows 10 before 1903. Units read "5 km" in every language there, not
  the language's own words: right only with `icu.dll`. The access violation
  after the uncaught C++ exception was not the abort at all: the shim's
  `vsyslog` shared the no-op function with `openlog`, `syslog`, `closelog`
  and `android_set_abort_message`, and the arm64 bridge converts a `va_list`
  by the function's address, so libc++abi's `openlog("libc++abi", 0, 0)` had
  its 0 read as a `va_list` (`gr_offs`, offset 0x18). `vsyslog` has its own
  function now, `register_shim_functions` refuses a shared one, and `abort()`'s
  line carries the abort message (`guest called abort(); abort message:
  terminating due to uncaught exception of type ...`), then the minidump. Not
  done: an arm64 library's own `SIGABRT` handler (Bugsnag's) is still not
  called from x86_64 code (one log line).
- **Reanimated reflects on ART's `Field.accessFlags`**, which HotSpot lacks
  (`NoSuchFieldException`, caught; Leumi). Looked into, not to be added; what
  happens is harmless. Reanimated (`ReanimatedUIManagerFactory.create`,
  `ReanimatedNativeHierarchyManager`'s constructor) clears FINAL in
  `accessFlags` of React Native's `UIManagerModule.mUIImplementation` and
  `NativeViewHierarchyManager.mLayoutAnimator` (private final instance
  fields), catches the exception, prints its stack trace and then calls
  `Field.set` anyway, which HotSpot allows after `setAccessible(true)` on a
  final instance field of an ordinary class (only static finals and the
  fields of records and hidden classes are trusted finals): the replacement is
  made, the log has a stack trace. `windows/test/java/FinalFieldReflectionTest.java`
  (CI) pins both halves. Why the overlay offers no `accessFlags`: HotSpot
  finds `Field`'s fields by name (`javaClasses.cpp`: clazz, name, type, slot,
  modifiers, trustedFinal, signature, annotations), so `modifiers` cannot be
  renamed and an added `accessFlags` would be a second int, not ART's alias;
  making it mean what ART's does (a write changes `getModifiers()`) would
  have `getModifiers()` lie to the JDK's own field accessor factory, which
  decides read-only from it, and still could not make a static final
  writable (the method handle is resolved from the VM's flags). Every member
  of `Field` is also hidden from reflection (`Reflection.fieldFilterMap`,
  kept in the overlay), and `Field` is a VM well-known class in the shipped
  JRE's class-data archive: a copy of JDK 21's `Field.java` in the overlay,
  for no gain here. Not checked: whether Android 14's hidden-API lists block
  `Field.accessFlags` for Leumi's target SDK too (then a phone throws the same
  exception).
- **Leumi's own React Native screens** (fixed, not yet re-checked by the
  screens check): after the ICU fallback, J2V8's V8 (`libj2v8.so`, arm64)
  stopped at `Check failed: page_allocator_->SetPermissions(address, size,
  access)`. The shim's `mprotect` made one `VirtualProtect` call for the whole
  range, which Windows (and Wine) refuses when any page is only reserved (V8's
  `PROT_NONE` guard pages beside a code chunk, over pages it never opened) or
  the range reaches into a second `VirtualAlloc` allocation; `madvise
  (MADV_DONTNEED)` cleared the whole range when its first page was writable,
  a fault on reserved pages after it. `mprotect`, `munmap`, `madvise` and a
  `MAP_FIXED` `mmap`'s protection now go region by region
  (`windows/src/bionic/misc.cpp`, `protect_range`; bionic_test's
  `test_page_permissions`, which fails on the old code from its guard-page
  check on). A/B under Wine with no `icu.dll`, same stage and data: the old
  shim aborts in that check right after `libj2v8.so` loads; the new one runs
  on past J2V8 and shows the app's own
  first screen, a React Native modal (the notifications consent sheet, in
  Hebrew). Seen in that boot: react-native-skia's view threw `Skia Metal
  backend unavailable; no CPU shader fallback` in `onSizeChanged` (fixed: the
  message was Win.apk's own, upstream's `MacApkSkia.isAvailable()` throwing on
  its first call where there is no Mac Skia bridge, i.e. always on Windows;
  it answers false now and `RenderEffect`/`RuntimeShader` are made, drawn
  without Skia; only on the Java2D graphics, AOSP's have their own classes;
  not a GL/EGL answer; `MacApkSkiaTest`), and Reanimated's `accessFlags`
  above. Next: the screens
  check, then past the consent sheet. (The Hermes "Maximum call stack size
  exceeded (native stack depth)" that left it blank before was the arm64
  guest-stack bug, fixed in 58c443a: it hit Flutter and Hermes alike.)
- **Installer of record.** Bit and Bank Hapoalim stop at "reinstall from the
  store" because `getInstallSourceInfo()` names no store (correct for a
  sideloaded APK, as on Android). An idea, not decided: record
  `com.android.vending` for apps installed from the launcher's Google Play
  panel.
- **Bank Hapoalim shows its reinstall dialog three times** (three window
  backgrounds stacked); not checked whether Android does too.
- **Cal's "error loading the app" page**: the KeyManagerFactory "X509" fix
  (c4e09b8's batch) loads the client certificate; whether the page goes away
  needs a run with real network (the sandbox's proxy breaks client-certificate
  TLS).

## Size and memory (decide later)

- **The arm64 translation's compiler, `clang\`, is 233 MB** (`libLLVM` 156,
  `libclang-cpp` 63): the largest part of the install, more than the AOSP
  files. Options to weigh: a trimmed LLVM build with only the AArch64->x86_64
  pieces used, downloading it on the first arm64 app, or sharing one with
  the system.
- **Every app is stored twice**: `library\<name>.apk` and the installed copy
  under `android\data\app\` (6-19 MB each in the measured apps).
- **CJK and emoji fonts** (Noto CJK Regular 19 MB, Color Emoji 9 MB) could be
  fetched on first need into the data folder.
- **Memory has no budget yet**: the smoke test records each app's peak working
  set; add a limit once a few CI runs show the spread.

## Work started and not merged

- **AOSP graphics, what is missing** (README "Milestone 2"): drawables
  inflate themselves on AOSP's classes, `<animated-vector>` included, whose
  animators now run on Win.apk's `android.animation` (checked with a test APK
  under Wine, not in the repository: morph, colour keyframes, motion along a
  path, the callbacks, the last frame); fonts androidx loads open (the
  `assetCookie`, and a compressed font read into a direct buffer: HotSpot's
  have no `array()`). Left: `AdaptiveIconDrawable` before the `Application`
  exists (the window icon at boot; Win.apk's inflater builds it),
  `<animated-image>` (unchecked), the one-copy present, Compose
  (`drawRenderNode` on a software canvas) and `SurfaceTexture` on native
  pixels. Probe with the switch on: 1942 of 2104 lines match vs 1895 on
  Java2D, no line further from Android's than Java2D's. Sweeps, lines
  differing: Gallery 295 (Java2D 301), Sudoku 225 (275); Bank Hapoalim's
  `activity_contact_before_login` 7 of 102 (Java2D 7, the same lines;
  before the font fix 70). Worse than Java2D: only Gallery
  `activity_video_player`, three hidden buttons over the bottom gradient a
  shade lighter, because both paths draw the backdrop under it grey where
  Android has black (the `TextureView` with no video: a views/video item,
  not the switch's). Left in the text rows: ExtendedFloatingActionButton's
  `getGravity()` (11 where Android has START|CENTER_VERTICAL, both paths).
  Not yet run: the screens check of more apps with the switch on, and apps
  on real Windows.
- **AOSP graphics is the default** (`host/aosp_gfx.h`, `kAospGfxByDefault`;
  `WINAPK_AOSP_GFX=0` is the way back, as is a stage without the files, each
  with its `AOSP graphics: off` line). Class-data sharing is on for it now:
  one archive per graphics, `cds\framework-aosp-<key>.jsa` and
  `framework-java2d-<key>.jsa`, neither's dump deleting the other's
  (`cds_util.h`, `stale_archive_file`; `cds_test`). `--cds-prepare` takes the
  graphics an app's start would (the same key, checked under Wine). Releases
  ship the files now (`windows-release.yml` no longer sets
  `WINAPK_STAGE_AOSP_GFX=0`). Checked under Wine on a stage of this tree: the
  probe with no variable is byte for byte the earlier switch-on run (1942 of
  2104), with `WINAPK_AOSP_GFX=0` byte for byte Java2D's (1895); the second
  start maps the AOSP archive; JVM to first frame 10.4-10.7 s with it, 11.8 s
  with class-data sharing off as before (Java2D: 12.4 s, 13.3 s). **To see on
  CI's next run**: the smoke test's ten apps all say `AOSP graphics: on` and
  draw (the step fails otherwise); the runtime tests' archive step (AOSP's
  archive made and mapped, a Java2D start making its own, AOSP's kept); the
  UI gate's summary saying "base Java2D, head AOSP" (the old-vs-new
  comparison) and what it calls REGRESSED, FARTHER or MISSING: each such line
  is an AOSP-path bug to fix or a Java2D line that was right by accident to
  look at, not a reason to flip back. Then the `framework-classlist`
  artifact, recorded on AOSP's graphics, should replace
  `windows/java/winapk-framework.classlist` (today's was recorded on Java2D:
  for the probe it lists about 270 Java2D-only classes, `java.awt`, Marlin,
  ImageIO, and misses about 235 of AOSP's; the first start on a machine adds
  its own app's anyway, so only `--cds-prepare`'s archive is short).
- **Next: delete the Java2D engine.** What must hold first: CI's smoke test
  green on AOSP's graphics over a few runs, and the UI gate's lines no
  further from Android than Java2D's (or each exception understood); the
  Gallery `activity_video_player` backdrop and the other gaps in the item
  above either fixed or accepted by the owner; the AOSP stand-ins for what
  Win.apk's own classes still do only on Java2D (`AdaptiveIconDrawable`
  before the `Application`, `<animated-image>`, Compose's `drawRenderNode`
  on a software canvas, `SurfaceTexture` on native pixels) at least as good;
  the processes with no app (`--gms`, `--system`, the install helper's icon
  render) moved onto the AOSP class path too, since today they still run on
  Java2D's and so map the `java2d` archive, which on a user's machine is made
  only by an app start with `WINAPK_AOSP_GFX=0` (the system broker starts
  without the framework archive meanwhile: the JRE's own still maps); and the
  apps on real Windows (`app-probe.yml`) looked at. Then: `Java2DGfx`, the
  Java2D `android.graphics`/`android.text` classes `aosp-graphics.jar`
  shadows, `WINAPK_AOSP_GFX=0`, the `java2d` archive naming and this branch's
  base-graphics step in `ci.yml` go; `GfxBackend` can then be folded away.
- **Screens checks not finished**: Bank Leumi (stopped by the usage limit
  before its boot), Pango and Paybox (not on APKPure; the owner offered their
  other repository or a Play download).
- **CI's UI gate** (b190783): its first run (run 44569377329, on 57a8987)
  passed for the UI probe (base and head identical), but the Gallery and
  Sudoku jobs were cancelled at the 25-minute limit. Cause: the base (`main`,
  1ac1559) has no `LayoutSweep` (it came in this branch, 73a8623), so its
  runtime ignored `MACAPK_LAYOUT_SWEEP` and ran the app itself (Gallery's
  MainActivity, Sudoku's title screen; 0 `PROBE` lines, frame #87600 at the
  cancel) with nothing to end it before `SCREENS_TIMEOUT`'s 3600 s. The head's
  sweeps finished by themselves in 31 s and 23 s (DEX conversion 11 s and
  9 s), so neither the exit hang, the conversion nor the two sides in
  parallel cost the time. Fixed: `gate_side.sh` exits 3 for a runtime without
  `LayoutSweep` and the job then checks the head alone (`gate.py report
  --no-base`); every run is ended after 240 s with no new `PROBE` line
  (`RUN_UNTIL_STALL`) and at 480 s. To verify on the next run: the two app
  jobs finish in a few minutes, their summary says "no layout sweep" while
  `main` lacks it, and compares base and head once `main` has it. Then watch
  for noise between two runs of the same code.

## Deferred by the owner: a live comparison

The screens sweep sees only layout XML files: Compose, Flutter and React
Native screens are built by running code, so it cannot see them (Leumi's main
screens, Bit's new ones, Maccabi's Flutter home). Agreed for later, after the
engine change: a live comparison in CI, the same app run on an Android emulator
(the reference) and in Win.apk on Windows, the same scripted steps (each
reachable activity and deep link, taps), comparing screenshots and each
screen's tree (React Native's real Views, Compose's semantics tree, Flutter's
accessibility semantics). Limits: no account behind logins, server data must
match, banking apps may refuse an emulator.

## Making the screens check work on an app (lessons)

What every per-app check needed, worth folding into `windows/test/screens`:
split APKs (density and language splits hold drawables and dimens the base
table lacks: add them to the oracle's AssetManager, run Win.apk on the
`.xapk`); the oracle with a plain `Application` (apps' own `onCreate` fails
under Robolectric); relocating library packages R8 merged into the app's jar
(Guava, BouncyCastle, kotlin) away from Robolectric's; not instrumenting an
app's own `android.support.*` classes; `LayoutSweep`'s AppCompat table trying
`android.support.v7.widget.*`; a skipped oracle test reported as a failure;
counting "LOADED but its constructor threw" as a throw; Compose views need an
attached window to measure; the oracle in chunks and a smaller converter heap
on a small machine.
