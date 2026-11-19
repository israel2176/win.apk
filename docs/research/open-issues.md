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
- The owner reads Hebrew only: answers to the owner are in Hebrew.

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
| 17 | `android.widget.DateTimeView` missing (notification templates become FrameLayouts) | | Waze, Hapoalim, Mercantile | views |
| 18 | Maccabi `item_service_request_interaction` RTL: `NonScrollListView` at y=95.5 where Android has 10 (and 0 wide); moved 1 dp further after includeFontPadding | constraint direction (5) | Maccabi | views |
| 19 | `LinearLayout.setGravity` stores the raw value; AOSP adds START / TOP when a direction is missing, so with `gravity="right"` `layoutHorizontal` takes its default branch and drops `layout_marginTop` (100+ cases) | `LinearLayout.java` ~191, ~1168 | Leumi | views |
| 20 | `RelativeLayout` rule precedence backwards: ALIGN_PARENT_* applied first and overwritten by sibling rules; AOSP applies the parent rules last (a timeline line 0 dp tall, 59 on Android; 18 children in 17 layouts) | `RelativeLayout.java` ~908-940, ~1361-1380 | Leumi | views |
| 21 | `View(Context, AttributeSet)` never sets the DataBinding tag (`"layout/..._0"`) from the element, so views that check `getTag()` keep a resource id Android replaces (two views with one id; 1,259 lines in 55 layouts). Part of item 3 | `View.java` ~227-233 | Leumi | views |
| 22 | NumberPickers inside the framework DatePicker get margins 8,16,8,16 where AOSP's legacy layout has 1,0,1,0 | | Leumi | views |

## Bugs to fix on their own (not part of a layer)

- **Exit hangs.** After a guest library aborts in a `thread_local` destructor
  at process exit (Realm: `GetEnv` on a detached thread), the process hangs;
  Android runs no thread_local destructors at exit. Skip guest destructors
  while the process is exiting (`windows/src/elf_tls.cpp` ~67
  `free_thread_blocks`, check `RtlDllShutdownInProgress` or a flag set by the
  exit paths). Also `LayoutSweep`'s `Runtime.halt(0)` goes through
  ExitProcess; it could end through `NativeCocoa.exitProcess` (TerminateProcess)
  like the runtime. Seen in bit, Waze, Maccabi sweeps.
- **DEX conversion's exit code.** An OOM-killed converter reported 0 under
  Wine and the run failed with "cannot replace classes.jar (File not found)"
  (`runtime_main.cpp` ~530-547): check that the temporary jar exists before
  trusting rc 0.
- **The dex conversion's heap is unlimited** (the JVM default, a quarter of
  RAM): 0.4-1.4 GB on a 16 GB machine. Give it a limit of its own.
- **The runtime's heap (`-Xmx1g`, `runtime_main.cpp` ~1492) filled after about
  3,100 layout inflations** in Mercantile's sweep: a leak in Win.apk or the
  app holding views; not found.
- **AppsFlyer** looks up `com.appsflyer.internal.AFi1fSDKT861` where the class
  it loaded is `...T8615` (one character short); the framework values it
  reads match Android; not found (bit).
- **`HucsConnection.getRequestProperties`** returns the JDK's map with a
  null-key status line; Android's never has one (Dynatrace NPE, caught; bit).
- **No `icu.dll`, a crash.** `HostIcu` throws "Windows' ICU (icu.dll) is not
  available" (`windows/host/native_icu.cpp` ~77) and `MeasureFormat` has no
  fallback, so a Hermes app calling `Intl.NumberFormat` dies (Leumi, under
  Wine; Windows 10 before 1903 has no `icu.dll` either). Fall back to
  `java.text`/ICU4J for plain number and unit skeletons. And the abort that
  follows an uncaught C++ exception in arm64 code ends in an access violation
  in the host (`winapk-runtime.exe+0x3551b7`, "a signal handler in an arm64
  library cannot be called for an x86_64 fault"), not a clean abort.
- **Reanimated reflects on ART's `Field.accessFlags`**, which HotSpot lacks
  (`NoSuchFieldException`, caught; Leumi).
- **Leumi's own React Native screens were not seen**: blocked by the ICU crash
  under Wine. Re-run on Windows, or after the fallback. (The Hermes "Maximum
  call stack size exceeded (native stack depth)" that left it blank before was
  the arm64 guest-stack bug, fixed in 58c443a: it hit Flutter and Hermes alike.)
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

- **AOSP graphics, what is missing** (README "Milestone 2"): AOSP drawables
  inflating themselves from XML (`com.android.internal.R.styleable`,
  `TypedArray.getComplexColor`), vector gradient fills, AnimatedVectorDrawable
  animators, `<animated-image>`, an app's own font families (`res/font`, only
  one file chosen), the one-copy present, Compose (`drawRenderNode` on a
  software canvas) and `SurfaceTexture` on native pixels. Probe with the switch
  on: 1904 of 2104 lines match vs 1895 on Java2D (TextView on AOSP's Layout;
  1656 before it). Left in the text rows: ExtendedFloatingActionButton's
  `getGravity()` (11 where Android has START|CENTER_VERTICAL, both paths).
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
- **Releases leave the AOSP files out** (`WINAPK_STAGE_AOSP_GFX=0` in
  `windows-release.yml`) until the switch draws apps as well as Java2D.

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
