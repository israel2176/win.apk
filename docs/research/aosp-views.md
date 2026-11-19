# AOSP's resources and views in Win.apk: plan and feasibility

Status: plan and spike (`windows/test/aospviews/`). Step 0, the `ResBackend`
and `ViewHost` seams, is done (below); nothing else here ships yet.
The roadmap entry is `windows/PORTING.md`, M11.

## Why

Win.apk's Android framework (`runtime/java`, about 3000 files) is a clean-room
rewrite; about 40 files come from AOSP. Screens checks on real apps keep
finding the same kind of bug: part of a View, widget, inflater or resources
class is missing. Some examples:

- an XML attribute is never read (`android:enabled`, `weightSum`,
  `includeFontPadding`);
- a default style's padding is left behind;
- a `<selector>` under `res/drawable` is not made a `ColorStateList`;
- `ScrollView` measures its child with size 0.

Each fix closes one case. The plan is to replace the app-process framework
with AOSP's own code, layer by layer. Win.apk keeps its own code only where
Android talks to the system: windows, input, files, network, Binder and the
system services, Google services, and the ELF/ARM layer. Apps still run as
programs on HotSpot; nothing here is an emulator.

Drawing is already moving to AOSP: `android.graphics` on Robolectric's native
runtime (`windows/README.md`, "AOSP graphics", milestone 2, behind the
`GfxBackend` seam). This plan covers the next two layers, resources and views,
and leaves graphics to that work.

## 1. What the spike measured

`windows/test/aospviews/` runs AOSP 14's classes in a plain HotSpot JDK 21 on
Linux. They come from Robolectric's `android-all-14-robolectric-10818077`
(API 34), the jar aospgfx uses. There is no Robolectric sandbox, no
instrumentation and no shadow dispatch at run time. Its README says how. It
does three things:

1. It brings up AOSP's `Resources`, `ResourcesImpl`, `AssetManager`,
   `ApkAssets`, `TypedArray`, `Theme`, `XmlBlock` and `ColorStateList` /
   `ComplexColor` inflation, over the framework's resources (android-all's
   `resources.arsc`) and the uiprobe app's APK.
2. It resolves the probe's two themes as `Probes.themeValues` prints them.
3. It inflates the probe's framework-only layouts with AOSP's
   `PhoneLayoutInflater` into AOSP's `LinearLayout`, `FrameLayout`,
   `RelativeLayout` and `TextView`. It measures and lays them out at 360x640 dp
   and density 2, left to right and right to left, and draws each with AOSP's
   software `Canvas`.

Measured on the 4-vCPU Linux VM, JDK 21.0.13, cold JVM, no CDS. Win.apk's
numbers come from the same probe's `ours.txt` from a staged runtime under
Wine.

| | AOSP on HotSpot (spike) | Win.apk's framework today |
|---|---|---|
| Theme lines equal to the oracle | **29 of 29** | 19 of 29 (it resolves the app's `colorPrimary` and similar attributes in a platform theme, where Android finds none; colour types 28 and 29 are swapped) |
| Layout lines, 8 cases | **44 of 44 exactly** | 30 exactly, 12 within 0.5 dp, 2 off by more than 1 dp |
| Pictures, 8 cases | **8 of 8 pixel-identical** to the oracle's | not compared |
| Not inflated | `edit_compound`: `EditText`'s `Editor` needs `Context.getPackageManager()` | |

What it took:

- **Natives.** 149 natives were given Java bodies at build time
  (`tools/RewriteNatives.java`):
  - 91 of libandroidfw's, over 4 classes, on Robolectric's Java port. 3 are
    unused and left without a body.
  - 58 of the system layer: `SystemClock` 7, `OsConstants` 1, `MessageQueue`
    6, `Binder` 20, `BinderInternal` 9, `EventLog` 7, and 8 of
    `libcore.io.Linux`'s 134.
- **ART-only `java.*`.** android-all's `android.*` and `com.android.internal.*`
  classes (36,938) name 21 `java.*` / `javax.*` members that JDK 21 lacks
  (`tools/RemapJdk.java`). Only `System.logW` was hit, from
  `android.compat.Compatibility`. The others:
  - `FileDescriptor.getInt$` / `setInt$`;
  - `NioUtils.freeDirectBuffer` / `unsafeArray`;
  - `InetAddress.parseNumericAddress`;
  - `LinkedHashMap.eldest`, in `android.util.LruCache`;
  - `ZipEntry.getDataOffset`;
  - `DateFormat.set24HourTimePref`;
  - the `getCurrentSpi` of `Signature`, `Cipher` and `Mac`.
- **System services through Binder.** The calls made, all answered empty
  in-process:
  - `window`: `hasNavigationBar`, `getCurrentAnimatorScale`, `useBLAST`;
  - `accessibility`: `addClient`, `getRecommendedTimeoutMillis`,
    `getFocusStrokeWidth`, `getFocusColor`;
  - the settings provider: `call` and `query`, 10 times each. `TextView`'s
    measure reads the `settings_auto_text_wrapping` feature flag from
    `Settings.Global`.
- **Context.** About 35 `Context` methods were overridden. The ones that
  matter: resources, theme, `ApplicationInfo`, the layout inflater,
  `getSystemService`, the main looper, permission checks, the
  `ContentResolver`, `AttributionSource`, autofill and content-capture options,
  `canLoadUnsafeResources` and the display id.
- **Cost, cold, with no CDS:**
  - the framework's `ApkAssets` (the port parses the 16 MB `resources.arsc`
    eagerly): **0.9 to 1.4 s** over four runs (the shared VM was under load);
  - the app's `ApkAssets` and the `AssetManager`: about 0.4 s;
  - the theme section: 0.6 to 1.2 s;
  - the first layout: 1.4 s (class loading); the next ones 60 to 150 ms.
- **Memory and classes:**
  - Classes loaded: 875 of AOSP's. By package: `android.graphics` 177,
    `android.view` 135, `android.icu` 104, `android.os` 87, `android.text` 83,
    `android.app` 40, `android.content.res` 38, `android.util` 36,
    `android.content` 32, `android.widget` 32. Plus the port's: 86 from
    `resources`, 63 from Guava, 11 shadows.
  - Heap used at the end: 85 to 143 MB. RSS: 250 to 330 MB, with the native
    runtime's ICU and fonts mapped.

What this proves, and what it does not:

- The oracle is Robolectric running the same android-all on the same
  libandroidfw port. Matching it shows that Win.apk's kind of process (a plain
  HotSpot JVM, no instrumentation) runs AOSP's resources and views exactly as
  Robolectric does, with a small, nameable set of seams.
- It does not show that Robolectric matches a device; `windows/test/uiprobe`'s
  README lists what the oracle cannot tell.
- It does not show that this works inside `winapk-runtime.exe` on Windows, in
  a window, with input, IME, animation frames or the app's own classes.

## 2. What AOSP's versions need, and what Win.apk has

Robolectric's shadows are the map of what a JVM must replace to run AOSP's real
views (`org.robolectric:shadows-framework:4.16`, 677 shadow classes).
Win.apk cannot use shadows: it has no run-time instrumentation, and must not.
For each need, Win.apk has to supply a real class, a native or a service
instead.

| Need | On Android | Robolectric | Win.apk today | Plan |
|---|---|---|---|---|
| Resource tables, XML, themes, attribute resolution | libandroidfw (`AssetManager2`, `LoadedArsc`, `ResXMLParser`, `AttributeResolution`, `Theme`) behind 94 natives of `AssetManager`, `ApkAssets`, `XmlBlock` and `StringBlock` | Java port `org.robolectric.res.android` (`resources` jar, 400 KB + Guava), through `ShadowArscAssetManager10/14`, `ShadowArscApkAssets9`, `ShadowXmlBlock`, `ShadowStringBlock`. **Its native runtime does not carry libandroidfw** (no `AssetManager2` symbol, no `register_android_content_*`) | Its own `Resources` (8102 lines), `ArscReader`, `ResourceIndex`, `BinaryXmlParser` / `axml` | R1: the port, as the spike does. R2: libandroidfw built for Windows |
| The framework's resources at the framework's level | `/system/framework/framework-res.apk` | android-all carries `resources.arsc` and `res/` | `scripts/build_framework_res.sh` builds one from AOSP source, at API 30 by default | Build it at the level of the classes (34). The classes' `R` ids must match the table |
| System assets | `createSystemAssetsInZygoteLocked`, with the static overlays from `OverlayConfig` (partitions, `Environment`, libcore's `Os`) | shadowed | none | Win.apk's `Boot` sets `AssetManager.sSystem` with no overlays, as the spike does |
| ART: `VMRuntime` (non-movable arrays and their addresses for `TypedArray`), `NativeAllocationRegistry`, `SystemProperties`, `Log`, `Trace` | ART, libcore | `ShadowVMRuntime` and others | Its own (`dalvik.system`, `android.os`, `android.util`) | Keep Win.apk's. `VMRuntime` gains the address-to-array map |
| The system layer: clocks, `OsConstants`, libcore's `Os`, `MessageQueue`, `Binder`, `EventLog` | libutils, libcore, libbinder | shadows | Win.apk's own `android.os`, `NativeLooper`, the libcore shim | Keep Win.apk's classes. These packages stay outside the swap |
| ART-only `java.*` members (21) | libcore | shadows / the sandbox | `java_base_patch` overlays only what Win.apk's own code calls | Add them to the `java.base` overlay, or remap at build time (`RemapJdk`) |
| System services through `ServiceManager` | `system_server` over Binder | `ShadowServiceManager` + per-service shadows (`ShadowWindowManagerGlobal` hands out an `IWindowSession` proxy, `ShadowAccessibilityManager`, `ShadowInputMethodManager`, `ShadowDisplayManagerGlobal`, `ShadowSettings`) | Its own managers talk to the host directly (`WindowManagerGlobal` 264 lines, `InputMethodManager` 742, `AccessibilityManager` 197) | In-process AIDL implementations (`IWindowManager`, `IWindowSession`, `IInputMethodManager`, `IAccessibilityManager`, `IDisplayManager`, the settings provider, `IPackageManager`), registered by name. The cross-process ones go through `host/binder_win.cpp` |
| The window root | `ViewRootImpl` (no natives itself) + `WindowManagerGlobal` + `IWindowSession.addToDisplay` / `relayout` | real `ViewRootImpl`; `ShadowViewRootImpl`, `ShadowWindowManagerImpl` | `ViewRootImpl` 417 lines; frames from `ViewCanvasDriver` | AOSP's `ViewRootImpl` over a Win.apk `IWindowSession` that maps to the host window |
| Frames | `Choreographer` + `DisplayEventReceiver` (4 natives: vsync) | `ShadowChoreographer`, `ShadowDisplayEventReceiver` | `Choreographer` 916 lines, driven by the host | Bridge `DisplayEventReceiver`'s natives to the host's frame clock |
| Rendering | `ThreadedRenderer` / `HardwareRenderer` (72 natives), `RenderNode` (89), `Surface` (22), `SurfaceControl` (109, BLAST) | the native runtime has HWUI's software pipeline (`register_android_view_ThreadedRenderer`, `RenderNode`, `Surface`); `SurfaceControl` is shadowed | Java2D display lists (`MacApkDisplayList`) | Graphics milestone 2. `SurfaceControl` needs Win.apk bodies (BLAST off where `useBLAST` allows) |
| Input | `InputEventReceiver` (6 natives) + `InputChannel` (8) from the window server | `ShadowInputEventReceiver` | `TouchDispatcher`, the host's pointer and keys | Bridge the receiver's natives to the host's input queue |
| IME | `InputMethodManager` → `IInputMethodManager`, `InputConnection` | `ShadowInputMethodManager` | Its own `InputMethodManager` + host IME | `IInputMethodManager` over Win.apk's IME host |
| Accessibility | `AccessibilityManager` → `IAccessibilityManager` | `ShadowAccessibilityManager` | 197 lines | `IAccessibilityManager` now; UI Automation later |
| Text | `android.text` + minikin, ICU (`android.icu`, 104 classes loaded) | native runtime | AWT text (`MacApkFont`, `MacApkBidi`) | With graphics (milestone 2): measuring and drawing text move together |
| Package manager for widgets | `Context.getPackageManager()` (`EditText`'s `Editor` requires it) | `ShadowPackageManager` | `MacApkPackageManager` | Keep Win.apk's |

## 3. Win.apk's own additions, and the class-name collision

Collision: AOSP and Win.apk both define `android.view.View` and the rest. The
graphics rule holds: **one implementation per name per JVM, chosen at JVM
start.** The runtime puts a jar of AOSP's classes before
`android-framework.jar` on the class path when the switch is on. The first
class found wins. The packages switch as groups that close over each other.

Two groups (with text going with graphics):

- **R (resources):** `android.content.res.*`, `ColorStateList` /
  `ComplexColor` / `GradientColor` inflation, `android.util.TypedValue`.
- **V (views):** `android.view.*` (without `Surface` and `SurfaceControl`,
  which the graphics work keeps), `android.view.inputmethod` and
  `android.view.accessibility` client sides, `android.widget.*`,
  `com.android.internal.{view,widget,policy}`, `android.animation`
  (`ValueAnimator` and `Choreographer` move together), and `android.transition`.

How the names overlap today:

| Package | AOSP 14 classes | Win.apk classes | Both | Win.apk only |
|---|---|---|---|---|
| `android.content.res` | 71 | 31 | 24 | 7 (`Resources$Macapk*` drawables and others) |
| `android.view` | 1432 | 583 | 533 | 50 |
| `android.widget` | 792 | 294 | 228 | 66 (mostly anonymous classes) |
| `android.text` | 290 | 213 | 181 | 32 |
| `com.android.internal.{widget,view,policy}` | 508 | 3 | 3 | 0 |

Win.apk's own surface inside these packages:

- 852 `macapk*` methods and fields and `MacApk*` classes are declared in
  view, widget, res and text.
- 152 of them are used outside those packages, at 574 sites in 71 files.
- 291 of the sites are in `com.macapk.loader`: `ViewCanvasDriver` 56,
  `TouchDispatcher` 48, `ResourceIndex` 37, `ActivityHost` 24,
  `StatusBarDriver` 21, `Boot` 16, `MacApkPackageManager` 12,
  `WindowOverlayStack` 12.
- 136 are in `android.app`.
- The most used: `macapkDrawFrame` (21), `macapkGetResourceIndex` (15),
  `MacApkDragSession` (12), `macapkTypeNameOf` (9), `macapkThemeColor` (8),
  `macapkActiveConnection` (8).

The way through is the one graphics took: seams first.

- **`ResBackend`** (done): opening an APK's resources, the framework table, the
  resource index the loader reads (`macapkGetResourceIndex`,
  `macapkTypeNameOf`), theme colours for Win.apk's chrome.
  `runtime/java/com/macapk/loader/res/ResBackend.java`, with `ArscRes` as
  today's implementation. Measured against AOSP 14's `android.content.res`
  (android-all), the resources side has 57 Win.apk-only members used from
  outside it: 30 of `android.content.res`'s and 27 of `ResourceIndex`'s (with
  its constructor and `StyleDef`'s two fields), 10 of them reached only by
  reflection. All 224 call sites, in 27 files
  (`android.app`, `android.graphics`, `android.view`, `android.widget` and
  `com.macapk.loader`), now go through it; the two `Gravity.parse` calls
  inside `android.content.res`, a views member, go through
  `ViewHost.parseGravity`. `windows/README.md`, "Changes to the upstream
  tree", has the groups.
- **`ViewHost`**: what drives a window's views. Attach, frame, damage, touch
  and key dispatch, the IME's active connection, drag, the status bar,
  insets and the material chrome Win.apk paints. **Done** (step 0, views
  half): `com.macapk.loader.view.ViewHost`, implemented by `WinApkViewHost`.
  Counted on the class files against android-all 14 (a member Win.apk's class
  declares and AOSP's, with its superclasses and interfaces, does not, by name
  and descriptor), plus constants javac inlines: 107 such members of
  `android.view`, `android.widget` and `com.android.internal.policy` were used
  from outside them at 208 sites in 33 files. 199 sites in 29 files now go
  through `ViewHost` (by file: `TouchDispatcher` 40, `ViewCanvasDriver` 35,
  `GLSurfaceView` 19, `StatusBarDriver` 16, `Activity` 15, `ActivityHost` 13,
  `WindowOverlayStack` 7, `InputManager` 6, `HostVideo` 6, `MediaCodec` 5,
  `MacApkEGL10` 5, `DisplayConfig` 5, and 1 to 3 in 17 more). Left direct, as
  they cannot be routed or belong elsewhere: three `super()` constructor calls
  (`Window()` from `Activity`'s and `Dialog`'s windows, `Dialog`'s anonymous
  `DecorView`), `GLSurfaceView`'s three `super.` calls in the hooks it
  overrides, and `ExtractEditText.setSelection` in `InputMethodService` (AOSP
  has it on `EditText`). The UI probe, the screens sweep (Gallery, Sudoku),
  `FrameworkStubsTest` and `DamageRectTest` give identical output before and
  after (`windows/README.md`, "Changes to the upstream tree").

Each seam has a Win.apk side (today's classes) and an AOSP side. With the
switch off, every one of the 574 sites goes through a seam and nothing
changes: the probe's `ours.txt` stays the same byte for byte, as it did for
`GfxBackend`.

What survives, and where it goes:

- **`Boot`, `ActivityHost`, `android.app`.** Win.apk's `Activity` /
  `ContextImpl` stay (Activity lifecycle and process are system-facing). With
  V on, `Activity.getWindow()` makes AOSP's `PhoneWindow`. `setContentView`,
  `DecorView` and the action bar become AOSP's. `ActivityHost` calls
  `WindowManager.addView` as an Activity does on Android. Its frame and
  window logic moves to the `IWindowSession` implementation.
- **The package manager** stays Win.apk's (`MacApkPackageManager`).
  `EditText` needs it (the spike's one failure).
- **The binary-XML path.** Layouts, drawables and colours are read through
  AOSP's `XmlBlock` over libandroidfw; the spike's 8 layouts did. Win.apk's
  `axml` stays for the manifest at install (`AppMeta`, `Manifest`).
- **AppCompat handling in `LayoutInflater`.** AOSP's `LayoutInflater` has the
  `Factory2` / `setPrivateFactory` hooks that `AppCompatDelegateImpl` and
  `FragmentManager` use on Android. Win.apk's `APPCOMPAT_REWRITE` table and
  its fragment-factory work-arounds should become unnecessary. The `lib_*` and
  `m3_*` probe cases and the screens sweep must show it before they are
  deleted.
- **Host natives for windows, input and IME** (`native_cocoa.cpp`,
  `win32_window.cpp`, `pointer_input.h`) stay as they are. Their Java callers
  move behind `IWindowSession`, `InputEventReceiver` and
  `IInputMethodManager`.
- **Accessibility** goes behind `IAccessibilityManager`. AOSP's views then
  produce real `AccessibilityNodeInfo` trees, which a later UI Automation
  bridge can read.
- **App-specific rescues** (`ComposeViewFallback`, `GlideRescue`,
  `FragmentInflaterFactory`) are checked one by one. Most work around gaps
  that AOSP's code does not have.

## 4. Steps

Each step ships behind a switch, default off, falling back to today's classes.
The gate for turning a switch on for everyone is **no line further from
Android**:

- `windows/test/uiprobe/regress.py` against the oracle, with the step's
  classes before and after: no `REGRESSED`, `FARTHER` or `MISSING`;
- the screens sweep (`windows/test/screens`) through the same script;
- the smoke apps still draw.

The aospviews spike becomes a CI job on Linux (plain JVM, about a minute),
like aospgfx.

| Step | What | Switch | Needs | Gate |
|---|---|---|---|---|
| 0 | `ResBackend` and `ViewHost` seams; the 574 sites routed through them. **Done** (`ResBackend` and `ViewHost`: the probe's `ours.txt`, the Gallery and Sudoku sweeps, `FrameworkStubsTest` and `DamageRectTest` byte-identical) | none | | `ours.txt` byte-identical |
| R1 | Resources on AOSP: `android.content.res` from android-all with its natives on the libandroidfw port (the spike's bridge, our own code instead of shadow statics; Guava's `Ints` replaced); `framework-res` at API 34; `VMRuntime`'s address map | `WINAPK_AOSP_RES=1` | 0 | probe sections `theme`, `styles.lib`, `drawables`; screens sweep. Win.apk's views keep working on AOSP's `Resources` (public API + `ResBackend`) |
| R2 | libandroidfw native: `libs/androidfw` + `core/jni/android_util_AssetManager.cpp` and its small closure (libziparchive, libutils / libbase pieces), built with clang for Windows as Robolectric builds HWUI; bound like aospgfx's DLL. Removes the 0.9 to 1.4 s and the Java heap the port holds | same switch | R1 | same lines; start time no worse than today |
| T | `android.text` with graphics milestone 2 | `WINAPK_AOSP_GFX=1` | graphics | `text.layout` section |
| V1 | Views on AOSP: `android.view` / `android.widget` / `PhoneWindow` / `DecorView`, the real `ViewRootImpl` over Win.apk's `IWindowSession`, `IWindowManager` and `IAccessibilityManager` (as the spike's `Services`, with real answers), the settings provider, `DisplayEventReceiver` bridged to the host's frame clock; software drawing into the frame `Bitmap` that graphics milestone 2 presents | `WINAPK_AOSP_VIEWS=1` (needs R and graphics on) | R1, graphics M2 | all probe layouts, screens sweep, smoke apps |
| V2 | Input and IME: `InputEventReceiver` / `InputChannel` natives on the host's queue, `IInputMethodManager` + `InputConnection` over Win.apk's IME host; accessibility service | same | V1 | input tests (`HostTouchTest`), the sweep's text-entry screens |
| V3 | HWUI's own pipeline (`ThreadedRenderer` with `RenderNode` display lists), then GPU with graphics' later milestones | | V1 | frame time no worse |

Which AOSP level: the classes and `framework-res` must be the same level.
Win.apk presents a per-app SDK level (`PresentedSdk`, up to the highest the
app supports). An API 34 framework presenting 35 or 36 has no API 35 methods.
Robolectric publishes android-all for 15, 16 and 17 on Maven Central
(`16-robolectric-13921718`, `17-robolectric-16653203`). The level is chosen
once for graphics and views together, against what the native runtime binds;
the spike ran only 14.

## 5. Size, start and memory

Estimates, from the spike and the jars:

- **Jar size.** android-all's classes for these packages, zipped:
  `android.view` 2.3 MB, `android.widget` 1.6, `com.android.internal.*`
  (widget, view, policy) 1.1, `android.text` 0.4, `android.content.res` 0.2:
  **about 5.6 MB**. Win.apk's own classes in the same packages are about
  1.6 MB, and all of `android-framework.jar` is 15.6 MB. Graphics adds 0.7 MB
  of its own. A jdeps closure against `android-framework.jar` (as
  `build_aosp_gfx.sh` makes) adds whatever the views name that Win.apk lacks.
  `android.icu` was 104 of the spike's classes. Win.apk would map it to the
  JDK's ICU or ship android-all's (1597 classes, 2.7 MB zipped).
- **The libandroidfw port:** 400 KB, plus Guava (3 MB jar, 63 classes loaded)
  unless `Ints` is replaced. Native libandroidfw: a DLL of a few MB (not
  measured).
- **`framework-res` at API 34:** android-all's `resources.arsc` is 16 MB;
  Win.apk already ships a framework table.
- **Class loading.** The spike loaded 875 AOSP classes for 8 layouts. Win.apk's
  CDS class list (merged from the smoke apps) has 742 `android.*` classes at a
  start. Expect more at a real app's start (not measured). They are plain
  class files, so the framework CDS archive (`host/cds.cpp`) takes them in once
  `WINAPK_CDS_RECORD_DIR` records them.
- **Start time.** The port's parsed tables are Java objects that CDS does not
  keep, so the framework's 0.9 to 1.4 s (cold) would come back at every start. Win.apk's
  warm starts are 0.6 to 2.1 s (`startup-cds.md`), so this is the main reason
  for R2. libandroidfw maps the table and resolves lazily.
- **Memory.** Heap used was 85 to 143 MB in the spike, most of it the port's
  tables and the class metadata. Win.apk's apps run at 254 to 357 MB working
  set today. R2 moves the tables to mapped files.

## 6. Risks and unknowns

- **The port lags AOSP.** Robolectric's libandroidfw port follows the C++
  with gaps: no parent theme id, no size-and-uiMode configurations, no
  loaders or overlays, no `nativeOpenXmlAssetFd`, and themes are not freed
  (a free function of 0). It is fine as the first step and the oracle's own
  basis, but not as the end state (R2).
- **Start time** as measured above, until R2.
- **Seams before swaps.** 574 call sites into 152 Win.apk-only members encode
  fixes apps rely on (window insets, density, status bar, drag, IME, the
  material chrome Win.apk paints). Each needs a home in a seam or a service,
  or must be shown unnecessary by the probe. That is most of the work, as it
  was for graphics.
- **Services, not shadows.** Robolectric satisfies `ViewRootImpl`,
  `Choreographer`, `InsetsController`, `SurfaceControl`, IME and
  accessibility with about 15 shadows. Win.apk must implement those
  interfaces as services in its own process, with no run-time
  instrumentation. `IWindowSession.relayout` and insets are the largest. The
  spike covered none of the attach path (`ViewRootImpl.setView`), frames or
  input.
- **ART-only Java.** The 21 members found (and any reached only by reflection)
  must exist in the `java.base` overlay. Apps that reflect into hidden
  framework members get AOSP's real fields, which helps.
- **API level** (above), and keeping the framework classes, `framework-res` and
  the graphics jar on one level.
- **Licensing.** AOSP and Robolectric are Apache-2.0. Guava is Apache-2.0.
  Entries go in `THIRD_PARTY_NOTICES.txt` as for aospgfx.

Not verified by the spike:

- Windows, `winapk-runtime.exe` and Wine;
- the app's own classes, so none of AppCompat, Material, ConstraintLayout,
  RecyclerView, dialogs or the `lib_*` cases;
- `EditText`;
- the attach path, frames, animation, input, IME and accessibility;
- start time with CDS;
- memory on Windows.
