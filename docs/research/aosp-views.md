# AOSP's resources and views in Win.apk: plan and feasibility

Status: plan and spike (`windows/test/aospviews/`). Step 0, the `ResBackend`
and `ViewHost` seams, is done (below). Step R1, the first part, is built behind
`WINAPK_AOSP_RES=1`, and steps V1 (but its surfaces) and V2 behind `WINAPK_AOSP_VIEWS=1`
(both off by default; "R1 as built", "V1 as built" and "V2 as built", after section 4).
Nothing else here ships yet.

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
| R1 | **First part built** ("R1 as built", below). Resources on AOSP: `android.content.res` from android-all with its natives on the libandroidfw port (the spike's bridge, our own code instead of shadow statics; Guava's `Ints` replaced); `framework-res` at API 34; `VMRuntime`'s address map | `WINAPK_AOSP_RES=1` | 0 | probe sections `theme`, `styles.lib`, `drawables`; screens sweep. Win.apk's views keep working on AOSP's `Resources` (public API + `ResBackend`) |
| R2 | libandroidfw native: `libs/androidfw` + `core/jni/android_util_AssetManager.cpp` and its small closure (libziparchive, libutils / libbase pieces), built with clang for Windows as Robolectric builds HWUI; bound like aospgfx's DLL. Removes the 0.9 to 1.4 s and the Java heap the port holds | same switch | R1 | same lines; start time no worse than today |
| T | `android.text` with graphics milestone 2 | `WINAPK_AOSP_GFX=1` | graphics | `text.layout` section |
| V1 | **First part built** ("V1 as built", below). Views on AOSP: `android.view` / `android.widget` / `PhoneWindow` / `DecorView`, the real `ViewRootImpl` over Win.apk's `IWindowSession`, `IWindowManager` and `IAccessibilityManager` (as the spike's `Services`, with real answers), the settings provider, `DisplayEventReceiver` bridged to the host's frame clock; software drawing into the frame `Bitmap` that graphics milestone 2 presents | `WINAPK_AOSP_VIEWS=1` (needs R and graphics on) | R1, graphics M2 | all probe layouts, screens sweep, smoke apps |
| V2 | Input and IME: `InputEventReceiver` / `InputChannel` natives on the host's queue, `IInputMethodManager` + `InputConnection` over Win.apk's IME host; accessibility service | same | V1 | input tests (`HostTouchTest`), the sweep's text-entry screens |
| V3 | HWUI's own pipeline (`ThreadedRenderer` with `RenderNode` display lists), then GPU with graphics' later milestones | | V1 | frame time no worse |

Which AOSP level: the classes and `framework-res` must be the same level.
Win.apk presents a per-app SDK level (`PresentedSdk`, up to the highest the
app supports). An API 34 framework presenting 35 or 36 has no API 35 methods.
Robolectric publishes android-all for 15, 16 and 17 on Maven Central
(`16-robolectric-13921718`, `17-robolectric-16653203`). The level is chosen
once for graphics and views together, against what the native runtime binds;
the spike ran only 14.

### R1 as built (first part)

`WINAPK_AOSP_RES=1` puts `lib\aosp-resources.jar` (1.3 MB, 579 classes; made by
`windows/scripts/build_aosp_res.sh`) first on the class path, before
`aosp-graphics.jar`, and the framework's resources are android-all 14's own
table as `bin\aosp\framework-res-34.apk` (21 MB). It needs AOSP's graphics on
(the default). Off, nothing changes: the probe's `ours.txt` is byte-identical.

- **AOSP's classes.** All of `android.content.res` from android-all,
  `android.app.WindowConfiguration` (Configuration's; no Win.apk class uses a
  member AOSP's lacks), `com.android.internal.R` (android-all's, matching the
  table) and `AdaptiveIconDrawable` (aosp-graphics.jar's copy has an API 36 id
  compiled in), plus a jdeps closure of what they name that Win.apk lacks.
- **Natives.** 92 of `AssetManager`, `ApkAssets`, `XmlBlock` and `StringBlock`
  run on Robolectric's port through our own bridge
  (`windows/java/aosp_res/com/winapk/aospres/Native*.java`, adapted from its
  shadows without their plumbing); `nativeLoadEmpty` and
  `nativeOpenXmlAssetFd` (loaders) throw by name. Guava is not shipped: the
  port's 13 Guava types map to small stand-ins. `VMRuntime.addressOf` in these
  classes gives a number per array and the bridge maps it back (Win.apk's
  `VMRuntime` is untouched).
- **The member scan.** Win.apk's framework calls no `android.content.res`
  member AOSP lacks except through `ResBackend` (5 new methods for the
  loader's `AssetManager`, the system configuration, a theme-less
  `TypedArray`, a placeholder `Resources`, framework attribute ids, and 2 for
  the XML text the DOM inflaters read). What AOSP's classes call that Win.apk's
  lack is rewritten at build time (`windows/scripts/AospResRewrite.java`) to
  `Compat` (`activityInfoConfigNativeToJava`, `RESOURCES_SDK_INT` = 34,
  `noteResourceMismatch`, `TextUtils.emptyIfNull` / `formatSimple`,
  `ActivityThread.isSystem`, `DisplayMetrics.noncompat*`), or added to Win.apk's
  class where AOSP has it (`LocaleList`'s three).
- **Attribute sets.** AOSP casts every `AttributeSet` to `XmlBlock.Parser`;
  Win.apk's inflater hands views a set over a DOM element. Each element now
  knows its compiled file and start tag (`com.macapk.loader.res.CompiledXml`),
  and `AttrSets` opens an AOSP parser on the same start tag, so the compiled
  attribute values are what AOSP resolves.
- **Assets.** AOSP's decoders pass an `AssetInputStream`'s native `Asset*` to
  the native runtime; the port's assets are Java objects, so the seven graphics
  classes that test for one are copied in with the test made false
  (`ImageDecoder.createFromAsset` reads the stream).
- **What stays Win.apk's.** Its name-based index (`ResourceIndex`) still opens
  and answers the index lookups the views and the DOM inflater make (style
  items, raw values by name, the decoded layout text); theme colours for the
  chrome are answered on an AOSP `Theme`. That half goes with V1.

Measured under Wine (4-vCPU VM, CDS off: the stage's JRE was not baked):

| | switch off | switch on |
|---|---|---|
| Probe lines differing from the oracle (of 2104) | 162 | **146** |
| `theme.platform` (11 lines) / `theme.lib` (18) equal to the oracle | 4 / 18 | **11 / 18** |
| `styles.lib` (227) | 224 | **227** |
| `drawables.platform` / `.material` / `.appcompat` / `.probe` | 67 / 186 / 89 / 8 | **71 / 188 / 89 / 8** (all) |
| `regress.py` off -> on | | 16 fixed, 0 REGRESSED, 0 FARTHER, 0 MISSING |
| Screens sweep, Gallery (lines differing of 3042) | 279 | 275 (4 fixed, 2 closer, none worse) |
| Screens sweep, Sudoku (of 1396) | 225 | 225 (none worse) |
| Framework table load / app table | | 176-252 ms / 4-10 ms |
| Warm start, to `Application init returned` | 3.29, 3.34 s | 3.63, 3.94 s |

The port parses the framework table lazily here: 0.2 s, not the 0.9-1.4 s the
spike measured for its whole first theme. The start still pays for both
tables (AOSP's and Win.apk's index), about 0.3-0.6 s.

A first run had 2 REGRESSED lines in `icons.rtl`: `ImageView#c`'s and `#k`'s
orange `bg_oval` came out blue. Not a channel swap: `View#j` in the same layout
draws `bg_oval` with `backgroundTint="#FF1A73E8"`, and Win.apk's `View.draw`
set that tint on the drawable without `mutate()`. AOSP's drawable cache hands
every `getDrawable` of an id the same `ConstantState`, so the tint recoloured
every later `bg_oval` (Win.apk's own resources inflated a new one each time,
which hid the bug). `View` now mutates before tinting, as AOSP's
`applyBackgroundTint` does; with the switch off the probe is unchanged.

Sudoku's sweep also shows two of `open-issues.md`'s item 16 gone:
`getResourceEntryName` of an unknown id throws `NotFoundException`, and a
land-only layout no longer answers in portrait.

Left for R1's second part: the DOM inflater's own
name-based reads (the index) and `AnimationUtils` / `AnimatorInflater`, whose
parsers now come from AOSP's `getAnimation` (spelled as AOSP spells values,
untested); `CompiledXml` for a set made up in code (`AttrSets` logs it and
AOSP reads no XML attributes for it); the resize path, untested (Win.apk's
`Resources` kept the process `Configuration` by reference; AOSP's copies it); split APKs (added, not tested);
CDS with the switch on (its own archive, `framework-aospres-<key>.jsa`, not
run: no baked JRE here); real Windows; more apps than the probe and two sweeps.

### V1 as built (first part)

`WINAPK_AOSP_VIEWS=1` (with `WINAPK_AOSP_RES=1`; the host turns it off with a
log line when AOSP's resources or graphics are off, or the jar is missing) puts
`lib\aosp-views.jar` (6.0 MB, 3260 classes; made by
`windows/scripts/build_aosp_views.sh`) first on the class path, before
`aosp-resources.jar`, and sets `-Dwinapk.aosp.views=1`; `ViewHost` is then
`com.winapk.aospviews.AospViewHost` (`windows/java/aosp_views`). Off, nothing
changes: the probe's `ours.txt` is byte-identical with the switch off, and with
`WINAPK_AOSP_RES=1` alone.

- **AOSP's classes.** All of `android.view.**`, `android.widget.**` and
  `com.android.internal.{view,widget,policy}.**` from android-all 14 (2645
  classes), but the ones the host's plumbing is built on, which stay Win.apk's:
  `Surface`, `SurfaceControl`, `SurfaceView`, `SurfaceHolder`, `TextureView`
  (GL and video surfaces), `MotionEvent`, `KeyEvent`, `KeyCharacterMap`,
  `VelocityTracker`, `InputDevice`, `InputQueue` (the host's input), and
  `ThreadedRenderer` (aosp-graphics.jar's; its nested classes are android-all's).
  A few classes outside those packages are AOSP's too, where the views need
  members Win.apk's lack and nothing of Win.apk needs one AOSP's lacks:
  `StateSet`, `android.R.styleable`, `AllCapsTransformationMethod`,
  `android.text.Selection`, `android.window`'s back dispatch and
  `SurfaceSyncGroup`. Then a jdeps closure of what they name that the
  framework lacks, bounded to AOSP's own plumbing (`android.window`,
  `com.android.internal.*`, `android.hardware.display`/`input`, the missing
  `android.os`/`util`/`text` classes, `AnimationHandler`,
  `PropertyInvalidatedCache`, `com.google.android.collect`; never
  `android.app`, `android.content.pm`/`res`, ICU, media or telephony): about
  600 classes. `android.animation` and `android.transition` stay Win.apk's for
  now (its `ValueAnimator` runs on AOSP's `Choreographer`'s public API).
- **Natives.** `DisplayEventReceiver`'s four run on a Java frame clock
  (`NativeDisplayEventReceiver`: a `winapk-vsync` thread ticking at the host's
  display rate, posting `dispatchVsync` to the receiver's looper as an
  asynchronous message, so it passes `ViewRootImpl`'s traversal barrier, with
  the frame's `VsyncEventData`). `InputEventReceiver`'s six record the window's
  receiver (V2 feeds it). Nine release-only natives (`SurfaceSession`,
  `InputChannel`, the input handles) answer zero; the rest
  (`CompositionSamplingListener`, `MotionPredictor`, `InputEventSender`,
  `ScreenCapture`, `TunnelModeEnabledListener`,
  `SurfaceControlHdrLayerInfoListener`, one of `DisplayManagerGlobal`'s) throw
  `UnsupportedOperationException` by name.
- **The member gap** (`windows/scripts/LinkScan.java`, run by the build). What
  AOSP's classes call that Win.apk's lack is sent at build time
  (`AospViewsRewrite.java`) to `com.winapk.aospviews.Compat`: about 60 members,
  `Context`'s hidden ones (display id, autofill and content-capture options,
  `getNextAutofillId`...), `ApplicationInfo.hasRtlSupport`, `StrictMode`,
  `Trace`, `TextUtils` and `Build` statics, `CompatChanges.isChangeEnabled`
  (true: the app's target SDK is the presented one), `SystemProperties.find`
  (no handle: `PropertyInvalidatedCache` queries every time),
  `HardwareRenderer.setFPSDivisor` and `RenderNode`'s position listeners
  (render-thread features of HWUI), `TextUtils.toUpperCase`; `new Surface()` is
  made invalid until `copyFrom`, as AOSP's is. What Win.apk's own kept classes
  need of AOSP's (its `SurfaceView` reads `View.nativeHandle`, `macapkAttached`
  and `Display.macapkRealWidthPx`) is added to AOSP's classes at build time.
  205 member references of AOSP's classes still do not link; they are on paths
  the probe, the two sweeps and the two apps did not take (`RemoteViews`,
  `LockPatternUtils`, translation, content capture, drag, `Magnifier`...), and
  fail by name if one runs. On Win.apk's side (`android-framework.jar` and
  `macapk-loader.jar` against the views jar), what is left unlinked is in
  classes AOSP's replace, behind `ViewHost` (`WinApkViewHost`), or input's
  (`TouchDispatcher`'s back dispatch, V2).
- **System services**, registered by name in `ServiceManager` (which now keeps
  what is added in the process): `window` (`IWindowManager`: `openSession`,
  animator scale, touch mode, no navigation bar, no BLAST), its
  `IWindowSession` (`addToDisplayAsUser`, `relayout`: the frame from AOSP's own
  `WindowLayout.computeFrames` over the display, a `MergedConfiguration` with
  the window bounds, and a Win.apk `SurfaceControl` of the frame's size),
  `display` (`IDisplayManager`: the one display's `DisplayInfo`, so
  `DisplayManagerGlobal` and `Display` are AOSP's) and `input_method`
  (`IInputMethodManager`: no input methods yet). Each is a subclass of its AIDL
  `Stub` generated at build time (16 of `IWindowManager`'s 144 methods
  answered, 10 of `IWindowSession`'s 42, 4 of `IDisplayManager`'s 55, 3 of
  `IInputMethodManager`'s 31); a method with no answer returns its type's zero
  and logs `[aosp-views] <Service>.<method>: no answer in this process` once.
  A `java.lang.reflect.Proxy`, as the spike used, cannot stand in: it loads
  every type an interface names, and `IWindowManager` names classes no app
  process has.
- **The window.** `Activity` (Win.apk's) gets AOSP's `PhoneWindow` from
  `ViewHost.newActivityWindow` (callback, private inflater factory, a local
  window manager over `WindowManagerImpl`, not hardware accelerated) and hands
  `setContentView`, `findViewById` and the rest to it, as AOSP's Activity does;
  `ActivityHost`'s resume (`latchWindowAdded`) adds the decor to AOSP's
  `WindowManagerGlobal` as `TYPE_BASE_APPLICATION`. `ViewRootImpl` then
  relayouts through the session, copies its `Surface` from the control and
  draws in software: `lockCanvas` is a `Canvas` on a frame `Bitmap` of the
  frame's size (AOSP's graphics) and `unlockCanvasAndPost` presents it to the
  host (`NativeCocoa.presentBitmap`), as a `SurfaceView`'s Surface already did.
  The host window and its render view are made once (`AospWindows`), for the
  size and the headless frame count; a resize goes to every window as
  `IWindow.resized` (not tested). `getSystemService` asks
  `ViewHost.systemService` first: `window`, `layout_inflater`
  (`PhoneLayoutInflater`, one per context), `input_method`, `accessibility`,
  `captioning`, `textclassification` and `textservices` are AOSP's managers.

Measured under Wine (4-vCPU VM, CDS off: the stage's JRE was not baked), one
stage, the UI probe and the screens sweep:

| | switch off | `WINAPK_AOSP_RES=1` | `+ WINAPK_AOSP_VIEWS=1` |
|---|---|---|---|
| Probe lines differing from the oracle (of 2104) | 162 | 146 | **0** (`ours.txt` byte-identical to `oracle.txt`) |
| `regress.py` to the views run | 162 fixed | 146 fixed | 0 REGRESSED, 0 FARTHER, 0 MISSING |
| Screens sweep, Gallery (lines differing) | 279 of 3042 | 275 | **46 of 2970** (233 fixed, 17 changed, none worse) |
| Screens sweep, Sudoku | 249 of 1396 | 249 | **50 of 1306** (199 fixed, 26 changed, none worse) |
| Probe: `Application init returned` / first frame | 9.7 s / 19.5 s | 10.5 s / 22.2 s | 11.0 s / 19.0 s |
| Open Sudoku started: first frame, working set | 10.5 s, 430 MB | | 11.8 s, 471 MB |

Per section of the probe (lines differing, off / resources / views; every
other section is 0 in all three): `theme.platform` 7 / 0 / 0, `styles.lib`
3 / 0 / 0, `drawables.*` 6 / 0 / 0, `lib_images` 24 / 24 / 0, the three
`*_list` 20 each / 20 / 0, the `*_dialog` four 11, 11, 5, 5 / the same / 0,
the `*_appbar` three 6, 6, 4 / the same / 0, `edit_compound` 4 / 4 / 0,
`icons` 4 / 4 / 0, `lib_constraint` 2 / 2 / 0, `m3dark_material` 4 / 4 / 0.
The oracle is Robolectric on the same android-all, so this shows that the same
code runs the same way inside Win.apk; it does not show that Robolectric
matches a phone. The timings are one run each.

What the sweeps still differ in: stack-trace lines where both sides throw
alike (Robolectric's `__constructor__` names, the JDK's line numbers,
`TypedValue.toString`: Win.apk's `TypedValue` is still its own) and, in
Gallery, `pager_photo_item` (30 lines), which the oracle cannot inflate
(Robolectric loads the app's arm64 `libpl_droidsonroids_gif.so`) and Win.apk
does. `LayoutSweep.java` needed no change: it runs inside Win.apk on AOSP's
views through their public API.

A correction to this first part, found while doing the second: the session's
`addToDisplay` answered `ADD_OKAY` without `ADD_FLAG_APP_VISIBLE`, so every
`ViewRootImpl` saw its app as not visible and relayouted its window `GONE`; the
frames this part reported (the probe's "UI probe" window, Open Sudoku's title
screen) were AOSP's `DecorView` drawn and presented by Win.apk's own canvas
driver, which `ActivityHost`'s empty-window path still installed for an
activity with no Win.apk content root. The probe's and the sweeps' lines were
not affected (they inflate and draw detached views). Both are fixed below.

#### Second part: dialogs, the windows composed, the system bars

- **The window really is `ViewRootImpl`'s.** `addToDisplay` answers
  `ADD_OKAY | ADD_FLAG_APP_VISIBLE | ADD_FLAG_IN_TOUCH_MODE`, as
  WindowManagerService does for a visible activity's window, and
  `Activity.macapkInstallContentDirect` does nothing with the switch on, so no
  Win.apk canvas driver draws the decor a second time. Every window draws in
  software: `ThreadedRenderer.sRendererEnabled` is off for the process, as AOSP
  turns it off for a low-end persistent process (`WindowManagerGlobal.addView`
  gives an app's windows `FLAG_HARDWARE_ACCELERATED`, and `ViewRootImpl` would
  otherwise make a `ThreadedRenderer`, step V3). `HardwareRenderer.isDrawingEnabled`
  answers true (the native runtime has no such native). The window manager's
  configuration is its own state, changed by a resize or the bars, not
  re-read from a window's resources at each relayout (that made a new
  configuration, and so a new traversal, every frame).
- **Dialogs are AOSP's classes**, in `aosp-views.jar`: `android.app.Dialog`,
  `AlertDialog`, `ProgressDialog`, `DatePickerDialog`, `TimePickerDialog`,
  `Presentation`, `android.text.method.CharacterPickerDialog` and
  `com.android.internal.app.AlertController` / `MicroAlertController`. The
  choice: a dialog on Android is a `PhoneWindow` added to the window manager,
  and Win.apk's `Dialog` is its own `Window` subclass drawn by Win.apk's
  overlay stack, which cannot live under AOSP's `ViewRootImpl`; bridging it would
  mean keeping that second window system alive. AOSP's take nothing new into the
  closure (it stays bounded), and LinkScan finds nothing of Win.apk's that needs
  a member they lack but Win.apk's own leftovers (`Dialog$DialogWindow`,
  `AlertDialog$2`..., never loaded) and `WindowOverlayStack` (Win.apk's overlay
  path, not used with the switch). AppCompat's and Material's dialogs
  (`AppCompatDialog extends Dialog`) come with them.
- **Toasts.** AOSP's `Toast` asks `INotificationManager`; `ToastService` answers
  its toast half in the process as NotificationManagerService and SystemUI's
  ToastUI do: one at a time, a text toast shown by AOSP's `ToastPresenter`
  (a `TYPE_TOAST` window, gravity `config_toastDefaultGravity`, y offset
  `toast_y_offset`), a custom toast by its own `TN`, for 2000 or 3500 ms.
  (Without it, `Toast.show` of the first part failed: `INotificationManager`
  was not in the jar.) Not checked in an app yet.
- **The compositor** (`Compositor`). Each window's `SurfaceControl` carries a
  `MacapkLayerPost` (Win.apk's `SurfaceControl`/`Surface`, copied with the layer
  into `ViewRootImpl`'s Surface), so a window's posted frame comes here instead
  of going to the host; the frame the host presents is then the windows bottom
  to top, each at its frame less its surface insets, WindowManagerService's dim
  layer (black at `dimAmount`, over the display) under a `FLAG_DIM_BEHIND`
  window, and the bars' glyphs on top. Order: the activity's
  `TYPE_BASE_APPLICATION` window first (its WindowToken keeps it at the bottom
  of its windows, whenever its dialogs were added: Open Sudoku shows its dialog
  in `onCreate`, before the activity's window is added), then the other
  application windows and sub-windows in add order, then system types. One
  window filling the display is presented as it is, as before. A window
  layer's canvas comes back in its initial state at each lock, as AOSP's native
  `lockCanvas` gives one (`ViewRootImpl` translates by the surface insets after
  every lock; a floating window's content walked off its layer frame by frame).
  The relayout sizes a layer with AOSP's `WindowLayout.computeSurfaceSize`
  (a dialog's is its frame plus 64 px of surface insets each side).
- **The system bars** (`Bars`). Which bars exist stays Win.apk's decision:
  none by default (the owner's; `StatusBarDriver.systemBarsEnabled`), both with
  `MACAPK_SYSTEM_BARS=1`. Their sizes are AOSP's own: the status bar
  `SystemBarUtils.getStatusBarHeight` (`status_bar_height_default`, no cutout),
  the navigation bar `navigation_bar_height` at the bottom, `navigation_bar_width`
  on the side Win.apk puts it. They are `InsetsState` sources the session
  reports to every window (addToDisplay, relayout, and
  `IWindow.insetsControlChanged` when they change), each visible as the top
  full-screen application window asks (`updateRequestedVisibleTypes`: `hide()`
  and `FLAG_FULLSCREEN` arrive that way), so `WindowLayout`, `InsetsController`,
  `DecorView` and the app's `fitsSystemWindows` / `WindowInsets` code compute
  what they do on a phone; the configuration's app bounds and `DisplayInfo`'s
  app size are the display less the navigation bar, and `ViewHost`'s decor
  answers (which `DisplayConfig` subtracts for `screenHeightDp`) are the bars'
  sizes. AOSP's `DecorView` draws the bars' background colour; Win.apk paints
  only the glyphs over it (`StatusBarDriver.paintStatusGlyphs` /
  `paintNavGlyphs`, `paint`'s and `paintSystemNavStrip`'s own second halves),
  dark on the window's `APPEARANCE_LIGHT_*_BARS`, as SystemUI does. With
  `MACAPK_SYSTEM_BARS=1` Open Sudoku's dialog is laid out between them
  (frame `0,48 - 720,1184`: 24 dp and 48 dp at density 2), its activity window
  edge to edge with the decor's insets.

Measured under Wine, the base being the first part (cf583c9) on one stage:

| | switch off | `WINAPK_AOSP_RES=1 WINAPK_AOSP_VIEWS=1` |
|---|---|---|
| Probe lines differing from the oracle (of 2104) | 162, `ours.txt` byte-identical to the base | 0, byte-identical to the base (and to `oracle.txt`) |
| Gallery sweep | 279 of 3042, byte-identical to the base | 46 of 2970, byte-identical to the base |
| Sudoku sweep | 249 of 1396, byte-identical to the base | 50 of 1306; `regress.py` base -> now: 0 REGRESSED, FARTHER or MISSING, 2 changed (an exception's `TypedValue@<hash>`, different in every run) |
| Off with `MACAPK_SYSTEM_BARS=1` (Open Sudoku's frame) | pixel-identical to the base | |
| Open Sudoku started | its "Latest changes" dialog over the dimmed title screen, as off | |

The probe has no dialog lines of its own: its `*_dialog` cases inflate a
dialog's layout detached, and those were already equal to the oracle.

Left for V1, in order:

- **SurfaceView, GLSurfaceView, TextureView, WebView** inside an AOSP view
  tree: a `SurfaceView`'s GL frames show under the windows. After each layout
  of an activity window the host window's content is Win.apk's shared GL view
  while a SurfaceView is shown in the tree (`AospWindows.showSurfaceBelow`),
  the compositor's frame then starts clear instead of black, the SurfaceView
  punches AOSP's hole (`clearSurfaceViewPort`'s condition), and the host
  composites the frame over the GL frame: Klooni (libGDX) and PPSSPP
  (NativeActivity) draw as with the switch off. It is not yet one layer of the
  compositor's among the windows. SDL's native window (Tensy) stalls after its
  rotation (`open-issues.md`); `TextureView` and video were not run.
- Display commits (a window resize, an app's requested orientation) reach the
  windows: `ViewHost.scheduleLayout` sends `IWindow.resized` to each window
  whose frame changes; `ViewRootImpl.updateConfiguration` then needs
  `android.app.ResourcesManager`'s three members, which the views jar has with
  AOSP's bodies for a process with no activity override configurations.
- Spinners' drop-downs and toasts in an app: windows like the dialog and the
  overflow menu (which V2 opened in Sudoku), not yet seen opening in an app.
  The platform action bar (`Activity.initWindowDecorActionBar`) is not made.
- The compositor redraws the whole frame from every window's layer on each
  post (a full-frame blit per window), with no damage tracking.
- Resizing (`IWindow.resized`), several activity windows, an activity's finish
  (`activityWindowRemoved`), configuration changes: written, not tested.
  `CompatChanges` answers true for every change, which is wrong for an app
  whose target SDK is below a change's. The other inset sources a phone has
  (tappable element, mandatory gestures, cutout) are not reported.
- CDS with the switch on (`framework-aospviews-<key>.jsa`), real Windows. The
  smoke apps: `open-issues.md`, "The smoke apps with AOSP's views".

V3: HWUI's own pipeline (`ThreadedRenderer`).

### V2 as built

Input, the input method and accessibility, with the switch on
(`windows/java/aosp_views`: `AospInput`, `InputRouter`, `ImeService`,
`AccessibilityService`, `TextServicesService`, `InjectInput`). Off, nothing of
it runs: `ViewHost`'s defaults keep Win.apk's `TouchDispatcher`.

- **The host's events, made as InputReader makes them.** `ViewHost.newTouchDispatcher`
  gives `AospInput`, a `TouchDispatcher` subclass that `Boot` registers with the
  window (`NativeCocoa.registerTouchDispatcher`), so `win32_window.cpp` calls the
  same entries as before. Each builds the event Android's mappers would:
  touch contacts as pointers of one `SOURCE_TOUCHSCREEN` gesture
  (`TouchInputMapper`); the mouse as `SOURCE_MOUSE` with `DOWN` then
  `BUTTON_PRESS`, `BUTTON_RELEASE` then `UP`, hover moves between and
  `HOVER_EXIT` on leaving (`CursorInputMapper`); a wheel step `ACTION_SCROLL`
  with `AXIS_VSCROLL`/`AXIS_HSCROLL`; Ctrl+wheel's pinch as a two-finger
  gesture; keys from `VIRTUAL_KEYBOARD` with the down time a key's up keeps and
  the repeat count (`KeyboardInputMapper`), the character the host's layout
  made stamped on the event. Display pixels: the host's points flipped to a
  top-left origin and times the backing scale. The gamepad
  (`GamepadBridge`) hands its keys and sticks over through `ViewHost.deliverInput`.
- **The window manager's dispatch** (`InputRouter`, what InputDispatcher does,
  for one display): a touch gesture goes to the window it went down in, top
  first in the compositor's order, where a window is a target when it is
  touchable and contains the point or is touch-modal (a dialog takes every
  touch; its own `Dialog.onTouchEvent` cancels it on one outside, as on a
  phone); a window above that asked for `FLAG_WATCH_OUTSIDE_TOUCH` gets
  `ACTION_OUTSIDE` (a `PopupWindow` dismisses on it). Hover and scroll go to the
  window under the pointer; keys and joysticks to the focused window, the
  topmost focusable one, which hears `onFocusEvent` when it changes (what
  `ViewRootImpl` turns into `windowFocusChanged`, and the input method's start).
  The event reaches the window's own `WindowInputEventReceiver` (recorded by
  `NativeInputEventReceiver` per `InputChannel`, set in `addToDisplay`) through
  `InputEventReceiver.dispatchInputEvent`, on the main looper, in the window's
  coordinates: `ViewRootImpl`'s input stages, the decor, `Activity`'s
  `Window.Callback` (`dispatchTouchEvent`, `dispatchKeyEvent`... now AOSP's
  bodies under the switch: the window's `superDispatch*` first).
- **The input method.** `ImeService` answers `IInputMethodManager` as a device
  with no input method enabled does (`InputBindResult.NO_IME`, `showSoftInput`
  false: no on-screen keyboard), and keeps the editor that
  `startInputOrWindowGainedFocus` names and its `IRemoteInputConnection`. The
  host's keyboard keys go to the focused view as a hardware keyboard's do, typed
  by AOSP's own key listeners (`android.text.method` is AOSP's under the
  switch: Win.apk's `QwertyKeyListener` leaves keys to its own text path).
  Windows' IME (Hebrew, Chinese...) composes into the editor itself:
  `hostImeInline` is true while an editor is bound, and the composition and the
  result go to its `InputConnection` as an input method's `setComposingText`
  and `commitText` (`RemoteInputConnectionImpl.getInputConnection`); the
  candidate window goes under the caret (`hostImeAnchor`).
- **Accessibility.** `AccessibilityService` answers `IAccessibilityManager` as
  `AccessibilityManagerService` does with no service on: state 0, no services,
  the focus highlight from the framework's resources. The host has no UI
  Automation bridge yet, so `isEnabled()` is false, as with TalkBack off.
- **Text services.** `TextServicesService`: no spell checker (an `EditText`
  asks on focus; a null service threw).
- **AOSP code that input reaches,** each with LinkScan's count down by its
  entry: `RippleDrawable`'s patterned ripple is started only on a hardware
  canvas (AOSP skips drawing it on a software one, and building its
  `RuntimeShader` threw: the native runtime's SkSL is older than AOSP 14's);
  `AudioManager`'s navigation sounds and fallback-key hooks, ICU's
  `getCombiningClass` (read off `java.text.Normalizer`'s canonical reordering),
  `hasBinaryProperty` (`Character`'s emoji data), `getIntPropertyValue`'s word
  break (WordIterator), `getDecimalSeparatorString`, `getIcuDateFormatSymbols`
  and API 29's `Spannable.removeSpan(Object, int)` (LinkMovementMethod) go to `Compat`. `MotionEvent` and `KeyEvent`
  gained the members `ViewRootImpl` and `View` call (`copy`, `split`,
  `getId`, display ids, `isTouchEvent`...).
- **Scripted input.** `MACAPK_INJECT_FILE`'s verbs (`tap`, `swipe`, `key`,
  `text`...) go through `AospInput` too (`InjectInput`), and `imetext <text>`
  (new, both paths) composes and commits as Windows' IME does.

Checked (Wine, 360x640 dp at 320 dpi, Sudoku): a tap on the "Latest changes"
dialog's OK removes the dialog's window; "Puzzle Lists", then the MENU key opens
the overflow menu (a `PopupWindow`, type 1002, over the activity); "Add folder"
opens a dialog whose `EditText` takes focus; `text Hello` types through the
key listeners and `imetext שלום` composes and commits Hebrew into the field
(right to left after the Latin text); Save closes it; Settings, a
`ListPreference` screen, picking an entry and BACK all work. `HostTouchTest` passes on both
class paths. The probe and the Gallery and Sudoku sweeps are byte-identical to
V1's, on and off (on, Sudoku's but for two `TypedValue@<hash>` names in stack
traces, which change from run to run).

The editor actions come as with a phone's hardware keyboard: Enter in a
single-line field is `TextView`'s own `onEditorAction` (not yet checked in an
app). Left for V2: an on-screen keyboard (none: the host's keyboard is the
input method), drag and drop, pointer capture and
`PointerIcon` (the cursor stays the arrow), `InputEventSender` (not used by
apps), touch-mode changes reported to the window manager (`setInTouchMode` is
answered empty), a UI Automation bridge for accessibility, spell checking.
Seen in Sudoku and not chased: after picking Dark mode the settings screen
recreates with light text on a white window background; its folder list stays
empty after Save (its database upgrade fails at start, `duplicate column
name: rating_level`, with the switch off too).

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
