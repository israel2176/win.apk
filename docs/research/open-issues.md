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
- Decided (November 2026): an app that comes as a bundle (.apkm/.xapk/.apks)
  is not kept whole in `library\` once installed: one copy of its APKs only.
  Fonts: Hebrew and English (Latin) matter most; the CJK font (19 MB) leaves
  the install, with a fallback for CJK text (done: Windows' own CJK fonts,
  below).
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
- Decided (November 2026): "Update apps in the background" runs only while
  Win.apk itself runs (the launcher, shown or hidden), never as a Windows
  scheduled task or a service: nothing of Win.apk runs on its own. The
  launcher opens the maintenance window every ~4 hours and at its start when
  the last is older (`LastMaintenance`), and is not kept alive for it
  (`maybe_quit` only waits for a window it opened). The task earlier builds
  made is deleted once (`MaintenanceTaskRemoved`) and by the uninstaller.
- Decided (November 2026): **every webcam is offered as both a back and a
  front camera** (`native_camera.cpp` `kRolesPerDevice`: camera 2k is BACK,
  2k+1 FRONT, one device). The owner's decision: a PC's camera serves both
  roles, so apps that ask for the rear camera (cheque scanners: camera 0,
  `Camera.open()`, CameraX's `DEFAULT_BACK_CAMERA`) work. So
  `android.hardware.camera` (a back camera), `.camera.front` and `.camera.any`
  are declared. What AOSP itself would do with a USB webcam, for the record:
  camera2 reports it as `LENS_FACING_EXTERNAL` (with
  `INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL`), and the camera service maps
  EXTERNAL to **FRONT** for the legacy API (`CameraProviderManager::
  getCameraInfoLocked`: "Map external to front for legacy API"), so camera1's
  `open()` (the first back camera) returns null and `FEATURE_CAMERA` is not
  declared on a device with only a USB camera. Copying that would break every
  app that wants the rear camera, which is why the owner chose two roles.
  (The old `Camera.java` comment saying AOSP maps EXTERNAL to BACK was wrong
  and is corrected.)
- Decided (November 2026): **no synthetic accelerometer.** The made-up "phone
  held upright" gravity (`SensorManager`'s `POSE_ACCEL_*`, from upstream) and
  everything derived from it are gone: no `TYPE_ACCELEROMETER`, `GRAVITY`,
  `LINEAR_ACCELERATION`, `ROTATION_VECTOR`, `GAME_ROTATION_VECTOR`,
  `ORIENTATION`, and no `android.hardware.sensor.accelerometer` feature, as on
  an Android device without one (`CameraOrientationTest` checks the null and
  the feature). Tilt games find no sensor. Optional, not done: Windows' own
  sensor API (the accelerometer of 2-in-1s) reported truthfully.
- Decided (November 2026): **no made-up sensors at all.** The gyroscope that
  reported 0,0,0 (upstream kept it: "a laptop on a desk does not turn") is
  gone too: a PC has none. `SensorManager`'s catalog is empty, so
  `getDefaultSensor` is null for every type (gyroscope, magnetometer, rotation
  vectors, light, proximity, step counter...), `getSensorList(TYPE_ALL)` is
  empty and no `android.hardware.sensor.*` feature is declared; the NDK's
  sensor manager (`native_sensor.cpp`) agrees. A sensor comes back only when a
  real Windows sensor backs it (`CameraOrientationTest` checks).

## Check again after each AOSP layer lands

Each of these differs from Android today and should disappear when the layer
named moves onto AOSP's code. After the switch, check it (the screens sweep or
the app); if it is still there, it is a bug in what replaced it.

**Views, step V1 first part (`WINAPK_AOSP_VIEWS=1`, with `WINAPK_AOSP_RES=1`;
`docs/research/aosp-views.md` "V1 as built"):** with the switch on, the Win.apk
code that items 1-9, 15 (its inflater and `View.measure` half), 18-20 and 22
name is not loaded at all: `View`, `ViewGroup`, `LayoutInflater`,
`ImageView`, `LinearLayout`, `RelativeLayout`, `HorizontalScrollView`,
`Spinner`, `CheckedTextView` and the rest are AOSP 14's own, constructed with
their attributes as on Android (items 2, 3 and 21: the two-argument
constructor reads the element's background, style and tag itself). Checked so
far only by the UI probe (all 2104 lines equal to the oracle, from 146 with the
resources switch alone) and the Gallery and Sudoku sweeps (279 -> 46 and
249 -> 50 lines, the rest stack-trace naming and one layout the oracle cannot
inflate): **not yet re-checked in the apps each item was found in**, and with
the switch on, dialogs, toasts and the system bars (V1's second part) and input and the
host's IME (V2) are there. Items 10-14
(text) are unchanged by it; 16 is the resources switch's.

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
| 14 | Mercantile's login banner: its second line drawn wider than its box, first letter clipped (likely the typeface bug fixed in b82a102; verify). Gmail's welcome subtitle (Hebrew, centered) clipped at its start edge the same way, November 2026 | | Mercantile, Gmail | check |
| 15 | Win.apk is lenient where Android throws: a missing `layout_width` gets a default, an unknown or R8-removed class becomes a FrameLayout, `View.measure` swallows an `onMeasure` exception, an unresolved `?attr` int is accepted. Hides real crashes. | `LayoutInflater` ~1253, ~1534; `View.java` ~5332-5360 | Waze, Bit | views |
| 16 | `Resources.getResourceEntryName/TypeName/Name` invent `id_0x...` for an unknown id instead of throwing `NotFoundException`; also answer for a land-only layout in portrait. **Gone with `WINAPK_AOSP_RES=1`** (Sudoku's sweep: throws, and 117 layouts instead of 118); still there with it off | `Resources.java` ~5626-5702 | all | resources |
| 17 | ~~`android.widget.DateTimeView` missing (notification templates become FrameLayouts)~~ Added (AOSP 14's, `DateTimeViewTest`); the screens check to confirm the templates now inflate it | `android/widget/DateTimeView.java` | Waze, Hapoalim, Mercantile | views |
| 18 | Maccabi `item_service_request_interaction` RTL: `NonScrollListView` at y=95.5 where Android has 10 (and 0 wide); moved 1 dp further after includeFontPadding | constraint direction (5) | Maccabi | views |
| 19 | `LinearLayout.setGravity` stores the raw value; AOSP adds START / TOP when a direction is missing, so with `gravity="right"` `layoutHorizontal` takes its default branch and drops `layout_marginTop` (100+ cases) | `LinearLayout.java` ~191, ~1168 | Leumi | views |
| 20 | `RelativeLayout` rule precedence backwards: ALIGN_PARENT_* applied first and overwritten by sibling rules; AOSP applies the parent rules last (a timeline line 0 dp tall, 59 on Android; 18 children in 17 layouts) | `RelativeLayout.java` ~908-940, ~1361-1380 | Leumi | views |
| 21 | `View(Context, AttributeSet)` never sets the DataBinding tag (`"layout/..._0"`) from the element, so views that check `getTag()` keep a resource id Android replaces (two views with one id; 1,259 lines in 55 layouts). Part of item 3 | `View.java` ~227-233 | Leumi | views |
| 22 | NumberPickers inside the framework DatePicker get margins 8,16,8,16 where AOSP's legacy layout has 1,0,1,0 | | Leumi | views |

**The smoke apps with AOSP's views (November 2026).** The ten apps of
`windows/test/compat/smoke.txt` booted headless under Wine (the sweep, 150
frames, CDS off, one stage), both switches on (`WINAPK_AOSP_RES=1
WINAPK_AOSP_VIEWS=1`, on 2cd3771 with V1's dialogs and bars and V2, then with
the fixes below) against both off. Off, all ten draw; **on, all ten draw too**,
each the same screen as off. The UI probe stays equal to the oracle in all 2104
lines with both switches, 162 differing off and 146 with resources alone, as
before.

| App (kind) | Off | On, at cf583c9 | On, now | Left |
|---|---|---|---|---|
| Editor (Java views) | "Access all files" dialog over the dimmed editor | the editor, no dialog (old pipeline) | **draws**: the same dialog over "Untitled.txt" | the dim is AOSP's (below) |
| Fossify Notes (Kotlin views) | draws | draws (old pipeline); spell checker NPE | **draws**, as off | none |
| Jerboa (Compose) | "What's changed" dialog | blank (the dialog threw in `onCreate`) | **draws**: "Support Jerboa / What's changed" over the Google notice, as off | none (the run of 2cd3771 caught the frame before the second dialog's first draw; the order is the window manager's, below) |
| Sgt Puzzles (JNI, Canvas) | draws | title "GameChooser" | **draws**, pixel-identical to off | none |
| Klooni (libGDX GL) | draws | blank: `skin\uiskin.json` not found | **draws**, the menu as off | none |
| Tensy (SDL, native window) | draws, though its intro is black at some frame counts | blank: `DisplayDecorationSupport` NoClassDefFoundError | **draws**, the title as off (five runs of five) | none. The "stall" was a crash in ANGLE (below) |
| PPSSPP (NativeActivity, GL) | draws (logo) | blank (GL under an opaque window) | **draws**, the same logo screen | none |
| Jitsi Meet (React Native) | draws (splash) | blank (dialog in `onCreate`) | **draws**, pixel-identical to off | none |
| Jellyfin (WebView shell, Compose setup) | draws | blank: "Autofill service could not be located" | **draws**, as off | none (`View.setRequestedFrameRate` is there now, below) |
| Open Sudoku (arm64) | "Latest changes" dialog | title "TitleScreenActivity", no dialog | **draws**: the dialog over the title screen | the dim differs from off: AOSP's here (below) |

Fixed at the root by this work (besides what ad53ceb and 2cd3771 fixed): the
autofill manager as on a device without the service;
`android.hardware.graphics.common` in the views jar; the resources bridge's
asset paths with backslashes; labels through `pm.getText` (AOSP's system
Resources hold the framework only); a SurfaceView's GL view under the windows
with AOSP's hole; display commits to the windows (`IWindow.resized`);
`CONFIG_WINDOW_CONFIGURATION` out of `DisplayConfig`'s relaunch and report
decisions; `ResourcesManager`'s three members; `Handler.executeOrSendMessage`
(AOSP's `InputMethodManager.showSoftInput`); then:

- **Tensy's "stall" was a crash** (found with `winedbg bt all` on the stalled
  process; the minidump being written under Wine kept it alive): an access
  violation in libGLESv2 inside SDL's `glDrawArrays`. SDL draws on its own
  thread into an EGL window surface on the SurfaceView; the host's render
  thread, ticking the shared GL view under it, made that view's pbuffer again
  at the rotated size (and read it back) on the same ANGLE display, whose
  Direct3D 11 device context is not safe from two threads. Off, the window
  happens to rotate before SDL's first frame; with AOSP's views it rotates
  after. `gl_tick` now leaves GL alone while an app's own window surface lives
  (`guest_window_surface_held`, 1bb3e33). This is host code and runs off too:
  there it only removes the same race (off, all ten still draw).
- **API 35 members of the views** (994c7c0): the runtime reports SDK 35 and
  the views are AOSP 14's; 45 of the 70 members android-35 adds to those
  classes are added to them at build time (`aosp_views/Api35.java`, with
  `AospAddMethods.java` as the graphics do): Compose's
  `View.setRequestedFrameRate` threw in Jerboa and Jellyfin. The 25 left name
  types AOSP 14 has no class of (credentials, `InputTransferToken`,
  `ConnectionlessHandwritingCallback`) or are `WindowManager`'s (an interface).
- **The dim behind dialogs** is AOSP's with the switches on: `PhoneWindow`
  sets `FLAG_DIM_BEHIND` and `dimAmount` from the theme's
  `backgroundDimAmount` (0.6 for the platform's `AlertDialog`, Editor; 0.32
  for Material's, Open Sudoku), and the compositor draws SurfaceFlinger's dim
  layer, black at `dimAmount` over everything below the window. Measured: the
  white under Open Sudoku's dialog is 173 (255 x 0.68), Editor's content 100
  (250 x 0.4). Win.apk's old path gives Open Sudoku 166, about 0.35, and a
  darker grey round Editor's dialog than 0.6 over its editor would.
- **The order of Jerboa's two dialogs** is the window manager's: both are
  `TYPE_APPLICATION` windows on the activity's token, and a token keeps its
  child windows in the order they were added, over its `TYPE_BASE_APPLICATION`
  window (the compositor's order). The Google notice is added first and
  "Support Jerboa" over it, as off.

Left: comparing the dim and dialog pictures with a device, not only with
Win.apk's old pipeline.

**The owner's apps with AOSP's views (November 2026).** The eight bundles of
`/opt/uip/il/` headless under Wine (900 display frames, CDS off, no network
past the sandbox's proxy, `WINAPK_PLAY_INSTALLER=1`, a fresh data folder per
mode, the install included), `WINAPK_AOSP_RES=1 WINAPK_AOSP_VIEWS=1` against
both off, on 29f911d. The system locale is the Wine prefix's (English); the
Israeli apps choose Hebrew themselves, and Waze and Moovit were run again
in Hebrew (`WINAPK_LOCALES=he-IL`). No crash in either mode, no
login reached (no credentials typed; most stop where the network does).

| App | Off | On | Compared |
|---|---|---|---|
| Maccabi | "What's new in the app" in Hebrew, the app's Arabic notice under it | the same | pixel-identical |
| Bank Leumi | blank (its React Native splash; the bundle loads, no screen within 900 frames) | the same | both blank; on presents only when something changes (1 frame against off's 116 repaints) |
| Bit (`bnhp.payments`) | **did not install**, both modes: the ".xapk" is the APK itself (fixed, 29f911d: an APK by its content, as Android's installer reads one); then "המידע לא נטען, נסה שוב" over its splash | after the fix: Bank Hapoalim's full-screen promotion ("מיליון לקוחות מקבלים מיליון מניות") with its close button | the two runs stopped at different network answers, not a layout difference; Hebrew right to left in both |
| Bank Hapoalim | the welcome screen, "כניסה לחשבון", the hoopoe illustration | the same | only the illustration's animation frame differs |
| Mercantile | the login form (ID, password, code), right to left | the same | the logo's and the animated icon's edges only (3,906 px) |
| Cal | "תקלה / ארעה תקלה בטעינת האפליקציה" (no network) | the same page | **the button differs, and on is Android's**: its style (`buttonBlackWithStateStyle`) sets 17sp and `@font/ploni_bold_aaa`; on draws it bold at 17sp, off regular and larger (Win.apk's old path drops the style's `fontFamily` and size here) |
| Moovit | the English consent screen (the bundle has no Hebrew split, so English in Hebrew too) | the same | 747 px |
| Waze | "Give Waze access to your precise location" with both steps (item 13 above is gone in both) | the same text, **the illustration smaller, and on is Android's** | the picture is `drawable/location_permission`, a 288x288 xxhdpi PNG: at 320 dpi Android scales it to 192 px, as on does (168 px drawn, its margins trimmed); off draws it unscaled (252 px). In Hebrew the screen stays left to right in both, as Android does: Waze declares `supportsRtl="false"` |

Left: Leumi's and Bit's screens past the network (a run with real network, on
Windows). Since then: `HardwareRendererObserver.nCreateObserver`, which Cal's and
Moovit's frame-metrics trackers hit, is bound through an adapter (9f0196f; the
library has the older instance form), and a run's system languages can be set
the way a device's are, `WINAPK_LOCALES=he-IL` (47df813): Waze is in Hebrew
with it, on and off alike.

**Windows measurements, November 2026 (branch `claude/relaxed-clarke-y3hjip`,
AOSP graphics on by default, resources and views off).** Seven apps from the
Desktop's Netfree folder plus two downloads, headless (`WINAPK_HEADLESS=1`,
150 frames, 300 s timeout), on real Windows. Data dir at `C:\il2\` (a long
package name with a deep data dir exceeded the 260-char MAX_PATH; fixed by
using a short root). Files on the Desktop in `נטפרי\עריכת אפליקציות\` are
unsigned (stripped META-INF); the runtime installs and runs them.

| App | Status | First frame | Peak WS | Note |
|---|---|---:|---:|---|
| Waze (`com.waze`) | blank* | 686 MB | 855 MB | Drew in 27 s on a fresh run; "blank" means the frame at t=150 was a splash |
| Moovit (`com.tranzmate`) | drew | 458 MB | 470 MB | 30.9 s; 1 exception (Facebook Ads JVM method-size limit, non-fatal) |
| Bit (`com.bnhp.payments.paymentsapp`) | drew | 408 MB | 459 MB | 19.6 s; arm64 version from Desktop; original CI download had only x86_64 libs and also hit the 260-char path limit |
| Mizrahi Tefahot (`com.MizrahiTefahot.nh`) | blank | 393 MB | 398 MB | 11.5 s; ZIP bundle renamed to .apk failed install; fixed by keeping .zip extension |
| Masad (`com.masad.nativeapp`) | drew | 707 MB | 750 MB | 18.4 s; same ZIP-rename fix |
| Yahav Bank (`il.co.yahav.mobbanking`) | blank | 357 MB | 359 MB | 20.2 s; `JSONArray.join(String)` missing (Android extension, not in Java's org.json); added to `runtime/java/org/json/JSONArray.java` |
| Meitav Trade (`com.fmr.meitavtradeapp`) | blank | 712 MB | 824 MB | 6.7 s with short root; `Failed to set up Skia Gr context`: Flutter needs `EGL_WINDOW_BIT` but gets a `1024×1024 pbuffer`; `macapkDrawFrame nonzero=0%` — not network (see below) |

* Waze draws on a first run; "blank" in repeat runs reflects the splash at frame 150.

Bugs found and fixed in this run:
- `sweep.py fetch()` always looked up `<pkg>_<version>.apk` even when apps.txt named a different file (`.zip`/`.apkm`); fixed to check the named file first.
- `JSONArray.join(String)` missing; added.
- Long package names with deep data dirs hit the 260-char MAX_PATH: `com.bnhp.payments.paymentsapp` (30 chars, appears twice) pushed the installed `base.apk` path to 263 chars. Fixed by using `C:\il2\` as sweep root. The fix for production: Win.apk uses 8.3 short names for the data root; the sweep's `--out` should be a short path.

Left: Yahav past the `JSONArray.join` fix (needs a jar rebuild to take effect); Leumi and Maccabi not measured on Windows (APKs not on hand). With a short `--out` root all seven apps install and run cleanly.

**Same seven apps in window mode, November 2026** (branch `claude/relaxed-clarke-y3hjip`;
`WINAPK_PLAY_INSTALLER=1`, real network, screenshots by the owner). Five apps reached
their first screen: Waze's location-permission activity, Moovit's home, Bit's
force-update screen, Mizrahi Tefahot's main screen with its force-update popup, and
Masad's update/dark screen. Two showed only a black window:

- **Yahav (`il.co.yahav.mobbanking`):** `MainActivity.onCreate` reaches
  `MAS.start()` → `ConfigurationManager.load()` → `JSONArray.join(String)` →
  `NoSuchMethodError`; the fix (`0cd24a3`) is on the branch but the stage was
  built before it. `runTolerant` catches the error and continues; Cordova's
  initialization never ran, so the view tree has no children and the window shows
  only the theme background (`#EBEEF3`; `macapkDrawFrame nonzero=100% drawn=0`).
  What looks "black" is a dark gray-blue background — confirmed by pixel sampling.
  **Verified on Windows (Nov 2026, rebuilt stage):** with the fix in, the window
  draws (100% nonzero). MAS completes; Cordova's WebView opens but fails with
  `net::ERR_FAILED (file:///android_asset/www/app/index.html?cordova)` — WebView2
  does not intercept `file:///android_asset/` and serve it from the APK. New bug:
  see "Cordova `file:///android_asset/` not served" below.

- **Meitav (`com.fmr.meitavtradeapp`):** Flutter fails: `Failed to set up Skia
  Gr context` / `Failed to create platform view rendering surface`; Win.apk assigns
  a `1024×1024 pbuffer` as the window surface, but Flutter's
  `gpu_surface_gl_skia` needs an `EGL_WINDOW_BIT` surface as a `SurfaceView`
  creates on Android. `macapkDrawFrame nonzero=0%` — truly black (no background
  drawn at all). The headless table's earlier note "network-dependent login screen"
  was wrong: the failure is a GL surface mismatch, present regardless of network.
  **Verified on Windows (Nov 2026):** draws — both the GL extensions fix (`02ee01d`)
  and the net_spi class-version fix (`97870b0`) were needed. The GL fix alone was
  not enough: `net_spi` compiled by JDK 23 (class version 67) caused a
  `ServiceLoader` failure during Flutter plugin registration, which detached
  `FlutterFragment` before its renderer was set. Zero "no such function here" and
  no `UnsupportedClassVersionError` with both fixes; `first frame presented (432×960)`.

## Bugs to fix on their own (not part of a layer)

- **Gmail on the owner's PC (November 2026, "Use Google services" on, a build
  older than this branch).** Three logs: two Gmail runs and Play Store. In both
  Gmail runs the broker refused the attach ("process N is not
  com.google.android.gm", 9dad890's stopgap; install notifications are another
  change), so every Google Play services bind and provider failed:
  `GmsClient: unable to connect`, `API_UNAVAILABLE`, "auth.accounts provider is
  not available", "Phenotype.API is not available". Play Store, attached, had
  none of them. What is the broker's and what was Win.apk's own:
  - *The error dialog on "add email address"* ("Gmail נתקלה בבעיה בשירותי
    Google Play"): GoogleApiAvailability's dialog for ConnectionResult 16
    (API_UNAVAILABLE), the `IllegalArgumentException` beside it is the stack
    trace GMS logs on purpose. Only the refusal. **Check again** once the
    broker learns installs.
  - *The white page after OK*: `AccountSetupFinalGmail`, setupdesign's GLIF
    landscape template, both panes `layout_width=0` with float-dimen weights;
    `macapkResolveNumberRef` read them through `getInteger`, which answered 0,
    so both panes were 0 wide. **Fixed** (LayoutInflater reads the value's own
    type, the `@0x...` form too). Reasoned from the log's view tree, not run.
    A first version also made `Resources.getInteger` read the value and throw
    for a non-integer, as AOSP; Win.apk's own `getValue` calls `getInteger`
    for values it cannot type, so that recursed and threw for drawables
    (Notes blank, Sgt Puzzles slow in CI's smoke test) and was reverted.
    Win.apk's `getInteger` still answers 0 where AOSP throws (item 15's
    family).
  - *`Calendar.getWeekData()` NoSuchMethodError* (88 times; SapiUiProvider
    "Unable to get Account", the message list's prefs). **Fixed**: the rest of
    `android.icu.util.Calendar` and `GregorianCalendar`; `IcuCalendarTest`
    passes with Android 14's ICU values.
  - *`ExtendedFloatingActionButton` IllegalAccessError on `bbqp`* (the compose
    button became a placeholder): a checkcast the translation adds names a
    package-private class R8 moved to another package. **Fixed** in the
    converter (CrossPackageTypes: such classes become public; Gallery: 2 of
    7237 classes). Root cause reasoned, the cast site itself not seen (no
    Gmail APK here); a synthetic dex checks only that the class is publicized.
  - */proc/self/status not found* (Primes). **Fixed**: Boot writes the fixed
    part, the path table adds VmSize/VmHWM/VmRSS/Threads per open (a Wine run
    of paths.cpp alone; `winapk-paths-test` checks it).
  - *`<include layout="?agLayoutSearchBar">` not resolved*. **Fixed** as AOSP's
    parseInclude; not run.
  - *"Font resource ID #0x7f090006 could not be retrieved"*: `gm3_ref_typeface_
    plain_bold` -> `google_sans_text_bold_compat`, a downloadable font
    (`com.google.android.gms.fonts`). ResourcesCompat.getFont with no callback
    throws exactly this on Android when the provider does not resolve, and
    with the broker refused `resolveContentProvider` finds no owner. The
    broker's; nothing changed. **Check again**: with the broker attached the
    fonts provider must resolve, pass the certificate check and serve the
    file over `RemoteProviders.openFile`.
  - *The welcome screen's Hebrew subtitle clipped at its start edge* (a):
    not explained. The font failure alone gives no measure/draw mismatch (a
    failed async font changes nothing; the TextView keeps one typeface), so it
    is probably the same family as item 14 (Mercantile's first letter): Win.apk's
    own TextView with AOSP's Layout. With `WINAPK_AOSP_RES=1
    WINAPK_AOSP_VIEWS=1` the TextView is AOSP's and should not show it; not
    run (no Gmail APK). Needs a reproduction: a centered `wrap_content` RTL
    TextView with the GM3 body appearance (letterSpacing).
  - Other lines, verdicts: the "cumq" `ExceptionInInitializerError` in the
    earlier run (a static init's `Method.invoke(null, ...)` NPE: a method
    static on Android is an instance one here, or a reflective lookup got
    the wrong one; not identified, absent from the later run);
    `/sys/devices/system/cpu/present` missing (WebRTC's CPU count: a real gap,
    small); `openRawResource ... missed: type=color` (log noise from colour
    lookups by name); `SplitRule#getSplitRatio` (WindowManager extensions'
    API level, a warning); Play Store's `MacApkPackageManager.installExisting
    Package`/`getHarmfulAppWarning`/`freeStorageAndNotify` NoSuchMethod and
    `SafetyCenterManager` NoClassDefFound (hidden/system APIs Play probes by
    reflection; Android throws too for a non-system app on most of them).
- **Meitav Trade black on a real PC (Flutter on Skia's GL): verified fixed on
  Windows** (`02ee01d`, `4e398e9`). "Failed to set up Skia Gr context": Skia's
  GrGLInterface asks for every function of each extension GL_EXTENSIONS lists
  and refuses the context when one is missing, and a guest saw ANGLE's whole
  list, ANGLE's and Chromium's own extensions included (more of them on a real
  GPU than under Wine). A guest is now told only the extensions whose
  functions it can call: declared by the Khronos headers or ANGLE's
  `gl2ext_angle.h` (whose `GL_ANGLE_*`/`GL_CHROMIUM_*` functions are forwarded
  too, as the other runtime does: Skia's `glBindUniformLocationCHROMIUM`, WebRender's
  `glProvokingVertexANGLE`), and every function resolving in the ANGLE loaded
  (`gen_gl_stubs.py`, `host/gl/guest_gl_extensions.cpp`; `winapk-gl-test`
  checks it: under Wine 120 of ANGLE's 124 pass, 30 of them ANGLE's or
  Chromium's). To check on a real GPU: Meitav, and a Flutter app whose Skia
  takes `GL_CHROMIUM_bind_uniform_location`. Still held back, no header
  declares them: `GL_ANGLE_shader_pixel_local_storage`,
  `GL_CHROMIUM_copy_compressed_texture` (both have functions),
  `GL_ANGLE_multiview_multisample`, `GL_ANGLE_get_serialized_context_string`;
  ANGLE's `eglext_angle.h` (`EGL_ANGLE_*`) is not imported. Additionally, `net_spi` classes compiled by JDK 23 (class
  version 67) caused a `ServiceLoader` failure during Flutter plugin registration
  that detached `FlutterFragment` before rendering started; fixed by compiling
  net_spi with `--source 21 --target 21`. Both fixes together: `first frame
  presented`, `nonzero=432×960` on the pbuffer surface. The window surface stays
  the presented pbuffer, which Flutter draws through fine.
- **Paths past 260 characters: verified fixed on Windows** (`e9ad133`). bionic's
  file calls and the ELF loader take the long-path form near MAX_PATH, and both
  exes declare longPathAware; Java already did. Tested: Bit (`com.bnhp.payments.
  paymentsapp`) with data dir at `%LOCALAPPDATA%\Win.apk\il2bit` (46 chars base),
  draws cleanly. Not covered without the long-path policy: the C++ host's own
  std::filesystem calls (the AOT cache, the install's staging), chdir.
- **Cordova `file:///android_asset/`: fixed, Yahav to check again on Windows.**
  Yahav Bank (Cordova) got `net::ERR_FAILED (file:///android_asset/www/app/
  index.html?cordova)`. The page's asset folder used to be copied to disk (up
  to 48 MB) and the copy loaded. Now nothing is copied: `file:///X` loads as
  `https://winapk-file.example/X` (`host/webview_file_urls.h`, after the other runtime's
  `FileUrls`) and each request there is answered from the APK after the app's
  `shouldInterceptRequest` (`MacApkWebViewBridge.macapkServeFile`), so
  `file:///android_res/`, a `loadDataWithBaseURL` page with a `file:` base
  and its `file:///` references load too. The URL mapping and the rule for
  which pages may read the origin have a unit test (`winapk-webview-urls-test`);
  WebView2 itself is not in the Wine prefix, so the page load is unchecked: on
  Windows, check Yahav, and that `../js/cordova.js` loads. If it fails, the run
  log's `WebView` lines (`not allowed to load local resource`, `a web page may
  not open`) and Cordova's console errors say where. Known difference: the
  page's script sees `location.protocol` `https:`, not `file:`; an app that
  branches on it (cordova-plugin-file's `Entry.toURL` checks
  `location.origin` for `file://`, measured on DOFUS Touch on the Mac) takes
  its non-file path. Not done: a `file:///` URL a script builds at run time in
  a page served from the APK stays `file:` and loads nothing; a large asset
  (a video) is read whole into memory before it is served.

**Seven apps with `WINAPK_AOSP_RES=1 WINAPK_AOSP_VIEWS=1`, November 2026**
(same branch and stage; logged via stdout capture):

| App | Normal mode | AOSP mode | Note |
|---|---|---|---|
| Waze | draws | draws | identical location-permission screen |
| Moovit | draws | draws | home screen, looks same or better |
| Bit | draws | draws | force-update screen, identical |
| Mizrahi Tefahot | draws | draws | force-update dialog, identical |
| Masad | draws | draws | InflateException on `base_dialog_fragment` (see below) |
| Yahav | draws (Cordova ERR) | draws (Cordova ERR) | `Log.println_native` NoSuchMethodError spam (see below) |
| Meitav | draws (Flutter SV) | first frame presented; canvas rndrr=null (Flutter SV draws) | canvas renderer not wired in AOSP-views path |

New bugs found with AOSP views:
- **`android.util.Log.println_native` arity (AOSP views):** AOSP 14's view classes call
  `Log.println_native(int bufID, int priority, String tag, String msg)` (4 args), but
  Win.apk's `android.util.Log` has the 3-arg form `(int priority, String tag, String msg)`.
  `NoSuchMethodError` fires on every Choreographer frame in Yahav, filling the log with
  thousands of repeated lines. Fix: add the 4-arg `println_native` to
  `runtime/java/android/util/Log.java`, forwarding to the existing 3-arg native.
- **`drawable/button_selector` NotFoundException (AOSP resources, Masad):
  not a parser difference so far.** `base_dialog_fragment` inflates
  `FibiFontButton`, whose `button_selector.xml` fails with `XmlPullParserException:
  <item> tag requires a 'drawable' attribute`. With `WINAPK_AOSP_RES=1` that
  parser is AOSP 14's own `StateListDrawable`, so a device throws the same for
  the same XML: `ResourcesImpl.loadXmlDrawable` inflates with no theme, so an
  item whose `android:drawable` is `@null`, missing, or `?attr/...` fails there
  too (the theme-less load; `?attr` inside a child `<shape>` is fine, it is
  applied after). A selector test APK (built with the SDK's tools) with child
  drawables, `@drawable`/`@color`/`@mipmap`/CSL/vector/alias references,
  `?attr` in items and in child shapes, `@null`, qualified copies (`ldrtl`,
  `night`, `anydpi-v26`, `v21`), a color selector in `drawable/` used as
  `textColor`, and obfuscated attribute names, gives AOSP's answer for every
  case with the switches on. Off, Win.apk's own parser accepts the `@null` and
  `?attr` items and answers a `drawable-night`-only drawable where AOSP finds none: the lenient side
  is the old one. So on a device Masad's code path does not reach this XML:
  look at which `base_dialog_fragment` the run picked (Win.apk's window is
  landscape by default; a `layout-land` or other qualified copy may use
  `button_selector` where the device's does not) and at the XML itself
  (`aapt2 dump xmltree --file res/drawable/button_selector.xml`). The APK is
  not on hand. Also checked, through the path a real app takes: an
  `AppCompatActivity` inflating a `Button` subclass, an `AppCompatButton`
  subclass and a plain `<Button>` with `android:background="@drawable/sel"`,
  `sel`'s item `?attr/x` from the activity's theme, and `getDrawable(id,
  theme)`, `ContextCompat`, `AppCompatResources`, `ResourcesCompat`. No layer
  drops the theme: the View's TypedArray carries it to `Resources.loadDrawable`,
  and AOSP's `ResourcesImpl.loadXmlDrawable` itself passes `null` to
  `Drawable.createFromXmlForDensity` (android-all 14's bytecode), so every case
  ends in `UnsupportedOperationException: Failed to resolve attribute ...
  theme=null`, as on a device. Masad's error is a different one: the item's
  `android:drawable` came back null, which a theme-less `?attr` never does (a
  theme that lacks the attribute would, but none is there). Still the XML is
  needed.

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
  Since (2026-11-24, from the other runtime's list): the raw table also answers the
  xattr calls (`ENOTSUP`), `eventfd2`, `epoll_create1/ctl/pwait` (arm64's
  event), `flock`, `renameat`, `fdatasync`, `timerfd`, `getrlimit`,
  `getcpu`, `socket`, `connect`, the `mlock` family, `memfd_create`; the
  stubs that misreported are real (`rt_sigaction`/`rt_sigprocmask` on the
  shim's signal state, `set_tid_address` cleared at thread end, priorities,
  `mincore` from the working set, `mremap` by copy, `getcwd`'s length,
  `clock_nanosleep`'s sign, `openat` with a directory and arm64's open
  flags, int results read as int); `winapk-arm64-test`'s
  `arm64_raw_syscalls2`. To check again on Leumi and on any app with a
  protection library: **`ptrace(PTRACE_TRACEME)` now fails with `EACCES`**
  (it returned 0): the kernel asks SELinux whether the parent, the zygote,
  may trace the app, and AOSP's `public/app.te` neverallows that for every
  domain but `crash_dump`, so a device refuses it. Read from the kernel and
  the policy, not seen on a device; if an app's anti-debugging check turns
  out to take `EACCES` for a debugger, compare with a device before
  changing it. Not changed: raw `exit` 93 ends the process, not the
  thread; `tkill`/`tgkill` answer 0 without delivering.
- **DEX conversion: fixed** (heap of its own, 1 GB, `WINAPK_DEX_HEAP_MB`;
  a JVMTI out-of-memory exit 77 retried with a larger heap; a bare exit 0
  with no whole `classes.jar.tmp` is refused with a log line). Real Windows
  not checked yet.
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
- **Installer of record: decided and done.** The owner's decision: Google
  Play is reported as the installer of every app the user installs, as
  `adb install -i com.android.vending` does, unless a real installer is
  recorded; Settings > Apps > "Report Google Play as the installer"
  (`PlayInstaller`, on by default) turns it off (`InstallerOfRecord.reported`,
  `InstallerOfRecordTest` in CI). Checked in a headless Wine boot of bit
  6.19.1: on, it stays on its splash; off, it shows "Something went wrong,
  reinstall bit from the app store". Check again on a run with real network:
  whether bit and Bank Hapoalim get past their splash (the sandbox's firewall
  stops them at "data not loaded"), and whether an app checks the initiating
  installer's certificate against Play's real one.
- **Compose text is blank on AOSP graphics when Compose calls API 35's
  `StaticLayout.Builder.setUseBoundsForWidth`**: `aosp-graphics.jar` carries
  android-all 14's (API 34) `android.text.StaticLayout`, which shadows ours
  (which has the method), so `AndroidComposeView.onMeasure` throws
  NoSuchMethodError and the screen paints empty (bit's reinstall screen
  with the setting off; with `WINAPK_AOSP_GFX=0` it draws). Not fixed.
- **Bank Hapoalim shows its reinstall dialog three times** (three window
  backgrounds stacked); not checked whether Android does too.
- **Cheque deposit (Scanovate) and the camera's orientation, focus and torch:
  written, not tried on a real webcam.** Hapoalim, Leumi and Mercantile scan
  cheques with Scanovate's scanner on the legacy `Camera` API (camera 0, a
  SurfaceView preview, NV21 callbacks, a landscape-fixed activity,
  `pictureAngle = (orientation - rotation*90) mod 360` into
  `setDisplayOrientation`, autofocus only with
  `android.hardware.camera.autofocus` and "auto" listed, 1280×720 preferred,
  blurry frames rejected natively). Done: `CameraInfo.orientation` /
  `SENSOR_ORIENTATION` are the display rotation (back) or its mirror (front)
  at the moment they are asked, so the angle is 0 in both orientations
  (`CameraOrientationTest`); focus and torch come from the driver
  (`IAMCameraControl`'s focus, the extended `FOCUSMODE`/`FOCUSSTATE`/
  `TORCHMODE`; `native_camera.cpp` "focus and torch"). Why the orientation
  follows the rotation instead of being fixed per device as on Android: the
  window's shape is fixed per activity, a cheque scanner goes from a portrait
  activity to a landscape-fixed one, and a value latched at the first would
  turn the second's picture by 270°; an app that keeps the value across its
  own rotation computes with a stale one, as with a stale display rotation
  on a phone. To check on Windows with a real webcam: (1) a UVC autofocus
  webcam (a Logitech C920-class) reports `kCtlAutofocus` (the log's
  `open(...)` line; `MACAPK_CAMERA_TRACE=1`), `autoFocus` answers within 3 s,
  and its lens position really moves under `CameraControl_Flags_Auto`
  (if a driver reports a constant position the one-shot answers "focused"
  after 600 ms without knowing: then use the extended state only); (2) a
  2-in-1's built-in camera with the extended controls: the one-shot
  (`AUTO|LOCK`) completes and FOCUSSTATE reads SEARCHING then FOCUSED; its
  TORCHMODE turns a real light on; (3) one bank app end to end: the scan
  screen upright, focus, a cheque accepted. Not done: the AF state of a
  continuous mode on a UVC webcam is "lens moving / settled", not a sharpness
  measure; LENS_INFO_MINIMUM_FOCUS_DISTANCE is left out for an autofocus
  camera (no driver gives a distance). Win.apk's own `TextureView` ignores
  both the SurfaceTexture transform and `setTransform`, so a TextureView
  preview stays upright either way; with AOSP's views a TextureView applies
  both, which then cancel as on a phone (to check).
- **What is still synthetic in the camera and sensors (for the owner):** no
  sensor is (the gyroscope that reported 0,0,0 is gone, the owner's
  decision); Tilt Control (`TiltControl`, never in
  Win.apk's UI, `MACAPK_TILT_CONTROL=1` only) drove the removed sensors and
  now does nothing; with no webcam, `CameraBridge`'s virtual camera delivers
  dark-grey frames (`MACAPK_VIRTUAL_CAMERA=0` turns it off) so preview screens
  open.
- **Cal's "error loading the app" page**: the KeyManagerFactory "X509" fix
  (c4e09b8's batch) loads the client certificate; whether the page goes away
  needs a run with real network (the sandbox's proxy breaks client-certificate
  TLS).
- **An app installed while the system broker runs: fixed, as Android does it.**
  Gmail installed with Google services already on was refused on attach
  ("process N is not com.google.android.gm") and every Google API answered
  API_UNAVAILABLE: the broker read `installed_apps.json` only at start. 9dad890
  re-read the registry when an unknown APK attached; that is replaced. Every
  commit (`install_into_library`: library, Store, Google setup, the install
  helper; `remove_app`: Remove, the helper's uninstall; the package backfill)
  now tells a running broker over Binder (`TX_PACKAGE_CHANGED`, launcher only,
  checked by the OS pid's program), which updates its table and broadcasts
  `PACKAGE_ADDED`/`REPLACED`/`MY_PACKAGE_REPLACED` or `PACKAGE_REMOVED`/
  `FULLY_REMOVED` by AMS's rules; an update now also stops the app's windowless
  processes (the broker's service hosts) instead of refusing the update as
  "running". The broker serves its name before its first registry read and
  holds every call until it is done, so nothing falls between. Verified under
  Wine by `BinderTransportTest` (all passed; on one of two runs the old
  "50 oneway calls arrive, in order" check saw 48: its COUNT call races the
  oneway connection, unrelated). **Check on the owner's PC:** install Gmail
  with Google services on while the Play Store is open, then open Gmail and
  sign in (`logs\system.log`: "package com.google.android.gm added", no
  REFUSED line). Not done: `PACKAGE_CHANGED` (no enable/disable of components
  yet) and `PACKAGE_DATA_CLEARED` (no "Clear storage" path tells the broker).
- **Google Play's automatic updates (November 2026): built, verified in
  parts, not yet on real Windows with real Play.** What stood between the
  real Play Store and a phone's auto-updates, and what was done:
  - *What Play sees* was already right: `getInstalledPackages` and
    `getPackageInfo` of other apps answer from the installed APK (its
    versionCode, its verified signers and v3 lineage with
    `GET_SIGNING_CERTIFICATES`), and the installer of record is Play
    (`InstallerOfRecord`, its test). Nothing changed there.
  - *Play's jobs died with its process* (the key gap): the app's JobScheduler
    lives in the app. Now the broker keeps them (`BrokerJobs`, persisted in
    `system_ce\0\jobs.bin`; a reboot of Windows drops the unpersisted ones)
    and starts Play, Play services or GSF headless when one is due and its
    constraints hold (network, unmetered from Windows' connection cost,
    charging from `GetSystemPowerStatus`, idle = no app open). Other apps'
    held jobs run only when the app runs (`WINAPK_JOBS_START_APPS=1` lifts
    that): each process is a JVM.
  - *Nothing runs while no app is open*: the owner's earlier rule (Google
    services only while an app is open) is kept for the broker itself, but
    Settings > Google account > "Update apps in the background" (on by
    default once Google services are set up) has the launcher, while it runs
    (shown or hidden), open a 3-minute maintenance window (`--maintenance`)
    every ~4 h on mains power with a network when no broker runs
    (`maintenance_check`, `launcher/maintenance.h`, its test in
    `launcher_state_test`), which also delivers the Play Store's
    BOOT_COMPLETED. It was a Windows scheduled task until the owner decided
    against anything running on its own (above); that task is deleted.
  - *Install path*: downgrades refused, key rotation accepted, the same rules
    for `ACTION_INSTALL_PACKAGE`; `InstallConstraints` honoured (an app open
    in a window is "interacting"; `GENTLE_UPDATE` is AOSP's;
    `commitSessionAfterInstallConstraintsAreMet` waits, then times out); the
    Play Store's update of an app open in a window stops it (exit 0, no
    error splash), any other installer's leaves it to the user (9523dd3's
    rule). Broadcasts and the broker's package table: 9523dd3.
  - *Verified*: `BrokerJobsTest`, `SessionUpdateTest` (Linux and Wine), and
    `BinderTransportTest`'s job checks under Wine (a job of a package whose
    process exited is run by a process the broker started for it). Not
    verified: the natives for metered and power (compiled only), the
    launcher's maintenance timer and the old task's removal on real Windows
    (the decision is unit-tested; the launcher only compiled), and everything
    with the real Play Store.
  - *Can't be fixed here*: Play may refuse downloads to an uncertified device
    or hide apps from it; Win.apk will not fake certification. Then the
    update comes as a file.
  - **Play Protect: the registration path is built** (November 2026). Gmail
    and Play say "This device isn't Play Protect certified"; Google's own way
    on for such a device is registering the GSF `android_id` at
    google.com/android/uncertified (the owner did it by hand, reading
    `gservices.db`). Settings > Google account > **Device ID for Google**
    (with Google services on, once there is an ID) shows it, with Copy and a
    **Register this device** link; `winapk-runtime --gms device-id` reads it
    (gservices.db through the runtime's SQLite; with no GSF database, the
    layer's own checkin). *Verified*: the launcher compiles (MinGW), the Java
    builds (`build_java.sh`), the verb's checkin path and its search for
    `gservices.db` run on a plain JVM (the SQLite read needs winapk-runtime's
    natives). *Not verified*: the row and the SQLite read on real Windows.
    **The owner's check**: Settings > Google account shows the same number he
    registered by hand; Copy, then the link opens Google's page.
  - **The owner's check on Windows** (Google services on and set up, signed
    in; a build with this change):
    1. Install an old Bit (`com.bnhp.payments.paymentsapp`) from a file. In
       Settings > Apps, "Report Google Play as the installer" on.
    2. Open Google Play once, let it settle a minute, close it. Then
       `logs\system.log` should have `jobs: com.android.vending… died holding N
       job(s); the system keeps them`. None: Play schedules no job with the
       platform JobScheduler (look for `schedule id=` in
       `logs\services\com.android.vending*.log` while Play runs) — report it.
    3. Quick path: Play > Manage apps & device > Update all (or Bit's page >
       Update). Expected: the download, then the install with no question,
       Bit closes if open, the tile shows the new version (App details), and
       `logs\install-helper.log` / `logs\install-bnhp…log` show the install.
       Bit then opens past its "update required" screen.
    4. Background path: with no app open, leave the library open (or hidden
       behind an install) and wait; or set the registry value
       `HKCU\Software\Win.apk\LastMaintenance` to 0 and start Win.apk, which
       opens the window at once. Task Scheduler should have no
       Win.apk > Background updates any more. `logs\maintenance.log`
       shows the window (`maintenance window`, `boot: system apps ready`,
       `jobs: … is due -> com.android.vending… (started for it)`), and a few
       minutes later `idle: … stopping Google services`. Look in Task Manager
       that winapk-runtime processes appear and go.
    5. If Play downloads nothing: its own screen says why (device not
       certified, "not compatible"). Note the words; do not work around it.
    6. A refused update shows in `logs\services\com.android.vending*.log` as
       `session N refused: INSTALL_FAILED_…`.

- **OpenSL ES (#132): real since the the other runtime import, not yet heard in a game.**
  `host/native_opensles.cpp` is the other runtime's `compat/media/ndk/opensles.cc`
  adapted (buffer-queue players into `audio_engine.h` streams, fd/file players
  as clip voices, recorders on `audio_capture.h`). `winapk-opensles-test`
  passes under Wine (no audio device: the mixer's silent clock). Not checked:
  any real app (no runtime build was run with it), an arm64 library's
  callbacks (the same `host_callable` path as AAudio's), the recorder filling
  buffers from a real microphone (Wine has no capture device), and the
  RECORD_AUDIO JNI call (`record_audio_held`: `SpecialAccessStore.holds` from a
  guest thread, attached as a daemon when it is not a Java thread). **Check**
  with the corpus apps that may use it: `org.ppsspp.ppsspp` (its OpenSL ES
  audio backend), `com.retroarch` (the `opensl` audio driver), and
  `net.minetest.minetest` (OpenAL Soft, whose Android backend is OpenSL ES or
  AAudio): sound plays, the log has `[opensles] CreateAudioPlayer` lines and no
  `SL_RESULT` refusals. Not done, as in the other runtime: http(s) URI players, effects
  (reverb, equalizer, virtualizer), playback rate, MIDI, the prefetch
  callback, record markers.
- **Download integrity (November 2026, from the other runtime's updater): built, partly
  verified.** The update offers no draft, and a pre-release only while the
  list has no full release; downloads the installer only with GitHub's
  `sha256:` digest of it (a release without one opens its page); keeps a
  download only at its Content-Length (`http_get`, for every download whose
  body is not content-encoded); runs an installer, also one kept from before,
  only when `updates.h` `judge_setup` passes: size, SHA-256, ProductVersion
  (`winapk.iss` now writes `VersionInfoProductTextVersion={#AppVersion}`) and
  Authenticode (WinVerifyTrust: a broken signature is refused; a code-signed
  build takes only an installer by its own signer; unsigned is said in
  Settings > Updates). The Store reads `index-v1.jar`, its JAR signature
  verified against f-droid.org's certificate (SHA-256 `43238d51…c9ccab`,
  `kFdroidSigner`, `launcher/signed_jar.h`), no longer the unsigned
  `index-v1.json`. *Verified*: `launcher_state_test` (the rules, the jar's
  layout and manifests); under Wine, `verify_signed_jar` accepts f-droid.org's
  real `index-v1.jar` (SHA1 digests) and `entry.jar` (SHA-256) and refuses a
  jar with the index changed or a file added; WinVerifyTrust reports an
  unsigned exe as unsigned and the ProductVersion reads. *Not verified*: on
  real Windows; a real Inno installer's ProductVersion (the first release built
  with the new `.iss`; one built before it would be refused, as v0.8.0's would);
  a signed installer (no build is signed). *For the owner*: every release is
  published with `--prerelease` (`windows-release.yml`); once a full release
  exists, pre-releases are no longer offered to anyone. *Later*: F-Droid's
  `index-v1.jar` is signed over SHA-1 digests; `entry.jar` + `index-v2.json`
  is SHA-256 throughout but needs the catalog's parser moved to index v2's
  layout (categories and suggested versions differ). The repository's
  timestamp is not yet compared with the last one read (F-Droid's client
  refuses an older index).
- **Explorer's thumbnails of the app files (November 2026, from the other runtime).**
  `bin\winapk-shell.dll` (`windows/shell/`, about 0.3 MB): each `.apk`,
  `.apkm`, `.xapk`, `.apks` shows its own icon, parsed natively. *Verified*:
  `winapk-apk-icon-test` (made-up APKs and bundles, truncated and corrupted
  copies, under ASan/UBSan on Linux) and real F-Droid APKs (F-Droid, NewPipe,
  Termux, Fossify Gallery, OpenSudoku, a stored `.apks` and a deflated
  `.xapk` around them) through the DLL's COM object under Wine, with an
  adaptive icon of a color and a picture layer. *Not verified*: Explorer on
  real Windows (the registration, the surrogate process, the cache, the
  installer replacing a DLL the surrogate still holds: Inno's Restart Manager
  should close it); an APKMirror `.apkm`. *Gaps*: a vector icon (or an
  adaptive one with a vector layer) shows only when the same package is
  installed (its `apps\<name>\icon.png`), else Win.apk's file icon; WebP
  needs Windows' codec (missing on some N/LTSC editions; Wine has none);
  small icons and the details view keep the type's icon (an `IExtractIcon`
  handler would be needed, as the other runtime's).
- **Proxy (November 2026).** Apps now follow Windows' proxy
  (`-Djava.net.useSystemProxies=true`, `runtime_main.cpp`); before, the JDK's
  default selector went direct whatever Windows said (checked under Wine: a
  manual proxy is returned and `<local>` bypassed only with the flag).
  *Not verified*: a PAC script or WPAD on real Windows (WinHTTP's auto-proxy
  service answers; the first lookup may take a moment). *Still differs from
  Android*: `ConnectivityManager.getDefaultProxy()` and `Proxy.getDefaultHost()`
  report none (they read `http.proxyHost`, which Android sets from its global
  proxy and Win.apk does not); the push connection (MCS) goes direct unless
  `WINAPK_PROXY`; upstream's per-app `HostProxy` file is not wired on Windows
  (its `initialize` runs only lazily from the GMS clients). No per-app proxy
  setting (the other runtime has one): decide only if an owner's need appears.

## Size and memory (decide later)

- **The arm64 translation's compiler, `clang\`, is 233 MB** (`libLLVM` 156,
  `libclang-cpp` 63): the largest part of the install, more than the AOSP
  files. Options to weigh: a trimmed LLVM build with only the AArch64->x86_64
  pieces used, downloading it on the first arm64 app, or sharing one with
  the system.
- **A bundle was stored twice: done** (the owner's decision, November 2026).
  Once installed, a bundle (`.apkm`, `.xapk`, `.apks`) is replaced in
  `library\` by `<name>.apk` and `<name>.splits\split_*.apk`, hard links to
  the installed `base.apk` and splits (`launcher/library_files.h`,
  `keep_installed_apks`); `AppInstall` given an `.apk` with a `.splits` folder
  links them all again. An update renames the app's files aside as
  `.previous` and never writes a kept file; Remove now also deletes the
  installed copy under `android\data\app\` (it was left behind). A library
  from before moves to the layout at the launcher's next start, app by app,
  once the app's installed copy is of its bundle (AppInstall's stamp: size and
  date). Measured under Wine with Cal's 25 MB `.xapk`: library plus installed
  copy 25 MB (was 50 MB); install, run, a stale record's reinstall, updates
  from a bundle and from a plain APK, the migration and Remove checked. Found
  on the way and fixed: an update whose file was dated before the last
  install's record was not installed at all (the runtime took the record as
  current; the launcher now removes it first). Left: a bundle's OBB files
  (`Android/obb/` in an `.xapk`) were never installed and now go with the
  bundle; a bundletool set's other whole-app alternatives go too.
- **CJK text is drawn with Windows' fonts** (done, November 2026, the
  owner's decision above): Noto CJK Regular (19 MB, about 11 MB of the
  installer) is no longer staged; `HostCjkFonts` puts Microsoft YaHei,
  JhengHei, Yu Gothic and Malgun Gothic (then SimSun, MingLiU, Meiryo, MS
  Gothic, Gulim) behind fonts.xml's four CJK families, in a fonts.xml of
  `cache\aosp-fonts\` that Java (through `updatableFontMap`) and native code
  (`/system/etc/fonts.xml`; `/system/fonts` finds a name in `bin\aosp\fonts`
  and then in `%WINDIR%\Fonts`, `paths.cpp`) both read. Checked under Wine
  with stand-in fonts carrying Windows' names (Wine has no CJK font) and
  with none (boxes, no failure); the UI probe's lines are the same with and
  without Noto CJK. **Check on real Windows**: that the PostScript names in
  `HostCjkFonts.TABLE` match Windows 10 and 11's files (a mismatch still
  takes the file's first face) and a Chinese, a Japanese and a Korean app.
  Known difference: CJK lines measure as Windows' faces do (taller ascent and
  descent than Noto CJK, other punctuation advances), so CJK text wraps and
  spaces differently from a phone; Latin, Hebrew, Arabic and emoji are
  untouched. Left as they were (proposal, not done): the other scripts, which
  Windows also covers (Thai, the Indic scripts, Ethiopic, Tibetan, Khmer,
  Myanmar, Sinhala and the historic ones, through Leelawadee UI, Nirmala UI,
  Ebrima, Microsoft Himalaya, Khmer UI, Myanmar Text, Segoe UI Historic), are
  154 files, 9.5 MB together, 0.004-0.5 MB each (the largest Egyptian
  Hieroglyphs and Cuneiform): little per script, and each would measure
  differently from a phone, so the same mechanism is not worth it yet; the
  emoji font (9 MB) stays, Segoe UI Emoji draws other pictures. A download
  of Noto CJK on first need (the earlier design: `updatableFontMap`, the
  font from the next start) remains possible should exact CJK metrics be
  wanted: `HostCjkFonts.setUp` already steps aside when
  `NotoSansCJK-Regular.ttc` is in the fonts folder.
- **Memory has no budget yet**: the smoke test records each app's peak working
  set; add a limit once a few CI runs show the spread.
- **Frame time has only a collapse budget** (`windows/README.md` "Frame
  time"): every run logs a `frame times` line at exit, and CI's Runtime tests
  run the frame bench (`windows/test/framebench`) on both engines into the
  summary, failing only over 50 ms p90 for the whole frame. Once a few CI
  runs show the spread on real Windows, tighten it (`report.py
  --budget-ms`).

## Found by the frame-time record, not fixed

- **Layout's p99 is 16 ms on AOSP's graphics against 7.5 on Java2D's** (the
  frame bench under Wine, `windows/README.md` "Frame time"; the heartbeat's
  full traversals?), not looked into. The frame's two `getPixels` copies that
  held AOSP's frames at 33 a second are gone (6 ms a frame, 60 a second);
  look at CI's numbers on real Windows for what is left.

## Work started and not merged

- **AOSP graphics, what is missing** (README "Milestone 2"): drawables
  inflate themselves on AOSP's classes, `<animated-vector>` included, whose
  animators now run on Win.apk's `android.animation` (checked with a test APK
  under Wine, not in the repository: morph, colour keyframes, motion along a
  path, the callbacks, the last frame); fonts androidx loads open (the
  `assetCookie`, and a compressed font read into a direct buffer: HotSpot's
  have no `array()`); an adaptive icon built before the `Application`
  exists (the window icon at start, the install's `icon.png`) is AOSP's too
  (`IconMaskThread`: AOSP's constructor took the mask from
  `getApplication().getResources()`, null then). Left:
  `<animated-image>` (unchecked), the one-copy present, Compose
  (`drawRenderNode` on a software canvas) and `SurfaceTexture` on native
  pixels. Probe with the switch on: 1942 of 2104 lines match vs 1895 on
  Java2D, no line further from Android's than Java2D's. Sweeps, lines
  differing: Gallery 279 (Java2D 301), Sudoku 225 (275); Bank Hapoalim's
  `activity_contact_before_login` 7 of 102 (Java2D 7, the same lines;
  before the font fix 70). Gallery `activity_video_player`, the one screen
  that was worse than Java2D (three hidden buttons over the bottom gradient a
  shade lighter, the backdrop grey where Android has black): fixed, an
  opaque `TextureView` with no frame reads black, as AOSP's own software path
  draws it (its `LAYER_TYPE_HARDWARE` view goes through an opaque drawing
  cache its `draw()` leaves empty; `setOpaque(false)`: transparent). On a
  device's hardware path an empty layer draws nothing; the oracle and
  Win.apk are both software. Under Wine, before -> after (`regress.py`):
  Gallery 295 -> 279 lines differing (16 fixed, 10 closer, nothing worse:
  the backdrop, the gradient, the hidden buttons and the toolbar over them
  now 0 as Android), Sudoku 225 and the probe (1942 of 2104) unchanged, the
  probe's `ours.txt` byte for byte. Left in the text rows: ExtendedFloatingActionButton's
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
- **CI's first smoke test on AOSP's graphics** (run 44643824110) failed:
  PPSSPP and Jellyfin blank, an exception in every app. Fixed in this change
  (README milestone 2, "The first smoke test on Windows"): AOSP's `Typeface`
  called `AssetManager.getAssignedPackageIdentifiers` (PPSSPP's `onCreate`
  threw), Compose called API 35's `StaticLayout.Builder.setUseBoundsForWidth`
  on AOSP 14's builder (Jellyfin's screen did not measure), `nPunchHole` is not
  in the native runtime (Tensy's `SurfaceView`), and `Typeface`'s static
  initializer read `/system/etc/fonts.xml` before it was mapped. Checked under
  Wine on a stage of this tree: Jellyfin draws its Connect screen with no
  variable and with `WINAPK_AOSP_GFX=0`, Jerboa's Compose text draws, the
  other seven draw too, and no `NoSuchMethodError`, `UnsatisfiedLinkError` or
  `fonts.xml` line is in any of the ten apps' logs.
  PPSSPP crashes under Wine in `GLRenderManager::ThreadFrame` on both
  graphics (Wine's GL), so its frame is **to see on CI**: its `onCreate` no
  longer throws. Left of Android 15's new members on the jar's classes (a
  linkage scan of the jar against the framework, and of the ten apps' DEX
  against the jar, finds no other caller): `Gainmap`'s, `LineBreakConfig`'s
  hyphenation and `merge`, `PositionedGlyphs`' fake bold and overrides,
  `BoringLayout.Metrics.getDrawingBoundingBox`, `Layout`'s other API 35
  getters (`getMaxLines`, `getEllipsize`, the indents...), `Font` and
  `FontFamily.Builder`'s variable-font ones; add each to `Api35` when an app
  calls it. `android.view.RenderNode`'s `getRotation`/`setRotation`/`isValid`
  are still missing on AOSP's `RenderNode`.
- **Next: delete the Java2D engine.** What must hold first: CI's smoke test
  green on AOSP's graphics over a few runs, and the UI gate's lines no
  further from Android than Java2D's (or each exception understood); the
  Gallery `activity_video_player` backdrop and the other gaps in the item
  above either fixed or accepted by the owner; the AOSP stand-ins for what
  Win.apk's own classes still do only on Java2D (`<animated-image>`,
  Compose's `drawRenderNode` on a software canvas, `SurfaceTexture` on native
  pixels) at least as good; the open rows of "What still reaches Java2D"
  (below) closed; and the apps on real Windows (`app-probe.yml`) looked at.
  Done: the adaptive icon before the `Application`; the processes with no app
  (an install's icon render, `--gms`, `--system`, `--verify-apk`) run on
  AOSP's class path too (`runtime_main.cpp` prepares the graphics for every
  JVM and binds the DLL in `gms_thread` and `system_thread`), so they map
  the `aosp` archive like an app. Checked under Wine: `--install-only` of
  Gallery says `AOSP graphics: on` and writes its adaptive icon (`icon.png`,
  AOSP's `AdaptiveIconDrawable`), `--gms accounts` exits 0; the DLL's load
  adds 0.25-0.5 s to each such process (0.2-0.9 s seen). Also under Wine
  (agent noapp-aosp): `--system` comes up (`[system] serving`, AOSP graphics
  on, the DLL loaded), `--gms accounts` maps the `framework-aosp` archive
  `--cds-prepare` made, and the install icons of Open Sudoku and Fossify
  Gallery match the Java2D ones but for the adaptive mask's outline; CI's
  runtime tests check the log lines, the broker, an `--install-only`'s
  `icon.png` and the archive `--gms` maps. With `-Xlog:class+load` on
  Java2D's path `--gms` and `--system` load `android.graphics.Bitmap`
  (the host natives), so they did need moving; the processes with no
  framework (DEX conversion, `--package-to`, `--aot-translate`, the
  archive's dump) load no `android.graphics`. Then: `Java2DGfx`, the
  Java2D `android.graphics`/`android.text` classes `aosp-graphics.jar`
  shadows, `WINAPK_AOSP_GFX=0`, the `java2d` archive naming and this branch's
  base-graphics step in `ci.yml` go; `GfxBackend` can then be folded away.
- **What still reaches Java2D** (AWT, ImageIO, `MacApk*` graphics classes)
  with AOSP's graphics the default. Found by a class-file scan of the staged
  `android-framework.jar`, `macapk-loader.jar` and `winapk-tools.jar` against
  `aosp-graphics.jar` (a class the jar shadows is Java2D's and never loads;
  every other class's references to `java.awt`, `javax.imageio`, `sun.java2d`
  and to members a shadowed class lacks): no reference to a member AOSP's
  classes lack (a `NoSuchMethodError` waiting), and these direct uses:

  | Caller | What for | Status |
  |---|---|---|
  | `Boot.initAwtOnAppKitMainThread` | brought AWT and its fonts up at every start (544-783 ms on the main thread under Wine, beside the resource index) | skipped on AOSP's graphics; goes with Java2D |
  | `MediaStoreProvider.photosImageSize` | a photo's width and height (ImageIO header read) | `BitmapFactory` bounds decode (Skia's codecs: PNG, JPEG, WebP, GIF, BMP, ICO; HEIF and AVIF from their boxes, `HostImageDecode`) |
  | `ViewCanvasDriver` `MACAPK_DUMP_BITMAP_AT`, `MACAPK_PUNCH_TRACE` | debug PNG dumps, the hole probe | `Bitmap.compress`, `createScaledBitmap`, `getPixel`; `GfxBackend.awtImage` removed |
  | `VideoImageSelfTest` (`MACAPK_VIDEO_IMAGE_SELFTEST=1`) | writes and reads PNGs | `Bitmap.compress` / `BitmapFactory` |
  | libjnigraphics (`src/jnigraphics.cpp`) | a native library's `AndroidBitmap_lockPixels` called `macapkArgbBuffer`, which AOSP's `Bitmap` has not: it failed on the default graphics | AOSP's `Bitmap` through `getPixels`/`setPixels` and upstream's conversion and merge; compiled, not run (no test lib under Wine on AOSP's class path: `winapk-jvm-run`'s test has Win.apk's `Bitmap`) |
  | `TextView.macapkFallbackVertical`, `GfxBackend.resolvedFont` / `textFontRenderContext` / `fallbackFontFor` / `deriveFont` | Java2D's fallback line spacing | inert on AOSP (no AWT font: `TextViewAospLayout` lays out); goes with Java2D |
  | `ClipboardManager` legacy path | text through AWT's clipboard | only with `MACAPK_CLIPBOARD_HOST=0` (a bisect gate; the host clipboard is native); text only, no images; to delete with Java2D so the JRE can lose `java.desktop` |
  | `Boot` `MACAPK_AWT_PREWARM` | an old macOS experiment, off unless set | delete with Java2D |
  | BitmapFactory on HEIC (and other WIC-only formats) | Java2D's `BitmapFactory` fell back to `NativeImageCodec` (libwebp, then WIC: HEIC with the HEIF extension) | **fixed**: `BitmapFactory`'s and `ImageDecoder`'s decoding natives go through `com.winapk.aospgfx.HostImageDecode` (`scripts/redirect_decoders.sh`): AOSP's native first, and only when it fails on a file whose ftyp brand is HEIF's or AVIF's (`HeifContainer`), `NativeImageCodec` (WIC) and a `Bitmap` from its pixels, with the Options (bounds from the boxes, sample size, density, config, mutable, inBitmap) and ImageDecoder's target size, crop, post-processor, turned upright by irot/imir (BitmapFactory: as stored, as Android). A stream or a descriptor has its first 64 bytes read ahead. No decoder in Windows (Wine; Windows without the HEIF Image Extensions / AV1 Video Extension): null / AOSP's `DecodeException`, one log line. Checked: `windows/test/heif/run.sh` (Linux, libheif standing in for WIC) and a probe APK under Wine (clean null, no crash). **Not checked**: on Windows with the extension (that WIC's HEIF decoder leaves irot to the app, as documented, and applies clap; a quarter turn it did apply is detected by size). TIFF, JPEG XR and other WIC-only formats stay undecoded, as on Android |
  | `Bitmap.createScaledBitmap`, `createBitmap(source, ..., matrix, filter)`, `BitmapFactory` with `inBitmap` | API 34's gain map natives (`nativeHasGainmap`, `nativeExtractGainmap`, `nativeSetGainmap`), which the native runtime predates | **fixed** (found with the HEIF work): each threw `UnsatisfiedLinkError` on the default graphics (seen under Wine with the probe APK); now `MissingNatives` (no gain maps) |

  Engine-neutral already (public `Bitmap`/`Canvas`/`BitmapFactory` API, so
  AOSP's classes on the default graphics; static scan only, not each run
  under Wine): image decoding (`BitmapFactory`, `ImageDecoder`: AOSP's, Skia
  in the DLL), camera and video frames (`SurfaceTexture.macapkLatchBitmap`:
  `Bitmap.setPixels`), the WebView's (WebView2 draws in its own window), the
  launcher icon (`Boot.writeLauncherIcon`: `Canvas` + `compress`, now on AOSP
  in the install process too), notification large icons (`HostNotifications`:
  `compress`), wallpaper and Monet colours (`HostWallpaper`: the host decodes
  to an ARGB `int[]`, no `Bitmap`), screenshots (`PixelCopy`, `View.draw`),
  `GLUtils.texImage2D` (`GfxBackend.argbBuffer`: `getPixels`). Clipboard
  images: none on either path. The scan tool is not in the repository (a
  page of Java over the class files' constant pools); it found nothing more.
- **AOSP resources, step R1, first part** (`WINAPK_AOSP_RES=1`, off by default;
  `docs/research/aosp-views.md` "R1 as built"): AOSP 14's `android.content.res`
  on Robolectric's libandroidfw port, android-all's API 34 table, a second
  `ResBackend` (`windows/java/aosp_res`). Off is byte-identical (probe). On,
  under Wine: the probe 146 lines from the oracle instead of 162, every
  `theme`, `styles.lib` and `drawables` line equal to it, `regress.py` 16
  fixed, 0 REGRESSED, 0 FARTHER, 0 MISSING (the first run's 2 REGRESSED lines
  were `View.draw` tinting a background without `mutate()`, which recoloured
  every drawable sharing the state AOSP's drawable cache hands out; fixed in
  `View`); Gallery 279 -> 275
  lines, Sudoku 225 -> 225, none worse; framework table 0.2 s, warm start
  +0.3-0.6 s (both tables are read: Win.apk's name index still answers the
  inflater). Left: the DOM inflater's name-based reads and
  `AnimationUtils`/`AnimatorInflater` on AOSP's parsers (untested); sets made
  up in code (no compiled form: AOSP reads no XML attributes for them); a
  window resize (AOSP's `Resources` copies the configuration where Win.apk's
  kept it by reference; untested); split APKs; CDS with the switch on; real
  Windows; more apps. Then R2 (libandroidfw native); V1 is started (below).
- **AOSP views, step V1, first part** (`WINAPK_AOSP_VIEWS=1`, off by default,
  only with `WINAPK_AOSP_RES=1`; `docs/research/aosp-views.md` "V1 as built"):
  AOSP 14's `android.view`/`android.widget`/`com.android.internal.policy` in
  `lib/aosp-views.jar` (`windows/scripts/build_aosp_views.sh`), a second
  `ViewHost` (`windows/java/aosp_views`), an Activity's window AOSP's
  `PhoneWindow` added to AOSP's `WindowManagerGlobal`, laid out and drawn in
  software by AOSP's `ViewRootImpl` on Choreographer's frames over an
  in-process window manager, session, display and IME service. Off is
  byte-identical (probe), and so is the resources switch alone. On, under Wine:
  the probe equal to the oracle in all 2104 lines (`regress.py` from the
  resources switch: 146 fixed, 0 REGRESSED, 0 FARTHER, 0 MISSING); Gallery
  279 -> 46, Sudoku 249 -> 50 lines differing, none worse. Second part done:
  dialogs and toasts on AOSP's own classes, every window drawn by its own
  `ViewRootImpl` (the first part's frames were really Win.apk's canvas driver
  drawing AOSP's decor: the session never said the app was visible) and
  composed in the window manager's order with the dim layer, the system bars as
  `InsetsState` sources (only with `MACAPK_SYSTEM_BARS=1`, Win.apk painting
  their glyphs); probe, Gallery and Sudoku byte-identical to the first part
  with the switch on and to the base with it off; Open Sudoku's dialog shows
  as off. V2 done ("V2 as built"): the host's touch, mouse, wheel, keys and
  gamepad made as Android's InputReader makes them and dispatched to each
  window's own `ViewRootImpl` (dialogs touch-modal, `ACTION_OUTSIDE` to
  popups, keys to the focused window), an in-process `IInputMethodManager` that
  binds the focused editor so Windows' IME composes and commits into it, AOSP's
  own key listeners (`android.text.method`), `IAccessibilityManager` as with no
  service on, `ITextServicesManager` with no spell checker. In Sudoku under
  Wine, scripted: the dialog's OK, the overflow menu, a dialog's `EditText`
  typed into with keys and with a Hebrew composition, a `ListPreference`, BACK;
  probe, Gallery and Sudoku byte-identical to V1 with the switch on and off.
  Sudoku's settings after picking Dark mode came back with light text on a
  white window: fixed, as Android does it, by Resources per configuration
  override (`createConfigurationContext`, `applyOverrideConfiguration`,
  `Activity.getResources()` following its base; aosp-views.md "A context's
  configuration override"). Class-data sharing with the switches on works
  (`framework-aospviews` archive, Sudoku's first frame 11.7 s against
  16-21 s without), and the host's mouse, wheel and pinch can be scripted
  with a window (`hostmouse`, `hostwheel`, `magnify`). CI runs the smoke set and the UI probe
  with both switches on, informational (ci.yml "AOSP resources and views"); their files add 21.8 MB
  installed, about 19.0 MB to the installer (29.2 and 26.4 before the wallpapers, below). A CDS dump's verification found classes that did not
  link (Dialog without WindowDecorActionBar, RevealAnimator, ServiceNotFoundException, the AIDL
  Stubs' ContextImpl): fixed; left behind services Win.apk lacks: augmented autofill, translation,
  inline suggestions, LockPatternUtils. On real Windows (CI run 44734867981) all ten smoke apps
  draw with both switches on and the UI probe equals Android's lines (0 of 2104); peak working set
  about 50 MB more per app before this round's memory fixes (themes freed, the port's
  `Theme::SetTo` fixed, the index opened once: Sudoku's live heap 103 -> 68.5 MB, off 46.4).
  Size items to decide (aosp-views.md, "Making R1 and V1/V2 the default"): net -2.5 MB installed
  and -2.5 MB installer once Win.apk's `framework-res.apk` and the shadowed classes go (its
  readers first); the Java2D fonts (6.9 MB) could go too. **Decided and done** (the owner,
  November 2026: delete them if nothing needs them, keep them if deleting would cause errors): the
  AOSP table's three `default_wallpaper.png` (7.4 MB). Nothing of Win.apk reads them, but an app
  can by `getIdentifier("default_wallpaper", "drawable", "android")`, so `build_aosp_res.sh`
  writes a 1x1 PNG over each, the table and its ids untouched: 7.4 MB less installed and under
  `xz -9e`.
  The launcher icon with the resources switch on: every adaptive icon threw (`writeLauncherIcon
  failed: NotFoundException ... mipmap/ic_launcher`) because aosp-resources.jar's
  `AdaptiveIconDrawable` (android-all's, for its private mask id) lacked aosp-graphics.jar's
  `IconMaskThread` redirect; fixed in build_aosp_res.sh: Sudoku, Gallery, Fossify Notes, Jerboa
  (adaptive) and Klooni (bitmap) render the same icon.png byte for byte as with the switches off, at
  an app's start and at `--install-only`.
  The second resource table is retired with both switches on (AospIndex: Win.apk's lookups by name
  answered from AOSP's tables; Sudoku private memory 269 MB on against 415 off, Gallery 259 against
  406; live heap 47.2/46.4 and 53.8/62.0). Before that, the remaining memory cost was the second
  resource table (AOSP's arsc in the heap beside
  Win.apk's index). Next: GL `SurfaceView`s as layers,
  spinners and toasts seen in an app, resize and several windows, an on-screen
  keyboard and a UI Automation bridge, then V3 (HWUI). Not run: CDS with the
  switch, real Windows, the smoke apps (another agent's).
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

## Contribution to the other runtime, ready for the owner to submit (2026-11-24)

`docs/research/pending/installer-bundles/`: six patches against the other runtime's origin/main 54be6270
(content-based APK/bundle detection, manifest-based split names, other apps' splits left out, bundletool
APK sets by toc.pb with bundletool's matching rules, XAPK manifest.json + OBB reported + encrypted APKM
refused clearly, ADR 0091) and PR.md. `git am` applies cleanly; tools/test_package_bundles.py passes (37).
Not run: their Windows/ART tests, real bundletool output, real XAPK/APKM files. The owner submits it.
