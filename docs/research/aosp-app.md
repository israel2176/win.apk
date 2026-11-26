# AOSP's app layer in Win.apk: plan and first step

Status: steps A0 to A4 built ("A1 as built" after section 4, "A2 as built",
"A3 as built", "A4 as built"): every
app's process starts on AOSP 14's own `ActivityThread` (`lib/aosp-app.jar`,
staged by `stage_runtime.sh` and shipped), through the `AppHost` seam, with no
switch (a project rule: a layer lands as the only path). A4 built ("A4
as built"): Win.apk's own `Activity`, `Application`, `ActivityThread`,
`Instrumentation`, `LoadedApk` and framework `Fragment` classes are deleted,
the framework is compiled against AOSP's, `ActivityHost` keeps only the host's
side, and `Boot`'s own start is gone. Services, broadcasts and the app's
providers run through `ActivityThread` over the in-process activity manager.

The model is `aosp-views.md` (resources and views). This layer comes after
them and assumes AOSP's graphics, resources and views are all on, as they are
becoming the default.

## Why

An app's process on Android is driven by `ActivityThread`: system_server sends
it client transactions (`LaunchActivityItem`, `ResumeActivityItem`,
`PauseActivityItem`...), its `TransactionExecutor` walks each activity through
the lifecycle states in between, `Instrumentation` calls the callbacks,
`ContextImpl` and `LoadedApk` make the contexts, the `Application` and the
class loader, and `Activity` reports back through `ActivityClient`.

Win.apk's own version of all this is a clean-room rewrite spread over
`android.app.Activity` (5854 lines), `Application` (1341, which is also
Win.apk's `ContextImpl`: Win.apk has no class of that name), a 208-line
`ActivityThread` shim, `Instrumentation` (550), `LoadedApk` (225), and
`com.macapk.loader.ActivityHost` (5344 lines: the "task stack", a LIFO list,
and every transition) and `Boot` (4846). Its comments carry 57 `MACAPK_*`
environment switches that each restore an earlier lifecycle behaviour, one
per fix: the same pattern as the views had, where every screens check found
one more ordering or state that differs from Android. Moving onto
`ActivityThread` makes the order of callbacks, the saved state, results,
configuration changes, relaunches and the `Application`/`Context` objects
AOSP's own, with the system side (which activity, which task, when to stop)
kept in Win.apk as the one thing a single-process PC program has to supply.

## 1. What was measured

On the 4-vCPU Linux VM, JDK 25 (`--release 21` classes), android-all
`14-robolectric-10818077`, the stage built from this branch by
`build_java.sh`, `build_aosp_gfx.sh`, `build_aosp_res.sh` and
`build_aosp_views.sh`. The Win.apk side is `android-framework.jar` and
`macapk-loader.jar` of the same stage.

**Class names.** android-all 14's `android/app/` has 1100 class files at its
top level (235 outer classes), 47 more in `android.app.servertransaction`.
Win.apk's `android-framework.jar` has 244 at the top of `android/app/`, 207 of
them under a name AOSP has too, 37 only Win.apk's (`Activity$MacApkHostResult`,
`Activity$ActivityWindow`, `Application$AppBaseContext`, `ServiceRegistry`,
`MacApkToolbarActionBar`, `MemoryBudgetManager`...). The views jar already
carries AOSP's `Dialog`, `AlertDialog` and the picker dialogs; the resources
jar `WindowConfiguration`.

**What the app layer asks the system** (the AIDL calls AOSP 14's app-layer
classes in `aosp-app.jar` make, counted on their bytecode by
`tools/AidlCalls`, a scratch ASM scan; the interfaces' sizes from android-all):

| Interface | Methods it has | Methods the app layer calls | Answered in A1 |
|---|---:|---:|---:|
| `IActivityManager` ("activity") | 249 | 35 (`attachApplication`, receivers, services, providers, URI permissions, `startInstrumentation`...) | 2 |
| `IActivityTaskManager` ("activity_task") | 95 | 32 (`startActivity`, `startActivities`, tasks, assist) | 2 |
| `IActivityClientController` (through `ActivityClient`) | 66 | 66 (every one: idle, resumed, paused, stopped, destroyed, finish, orientation, task description, PiP...) | 17 |
| `IPackageManager` ("package", `ActivityThread.getPackageManager()`) | 206 | 8 | 2 |
| `IApplicationThread` (the reverse: system to app) | 61 | the system side calls `bindApplication` and `scheduleTransaction` | AOSP's own |
| others | | `IPermissionManager` 1, `IWindowManager` 2, `IContentCaptureManager` 1, `IVoiceInteractionManagerService` 1, `IVoiceInteractor` 9 | 0 |

ServiceManager names looked up by name in these classes: `activity_task`,
`package`, `permissionmgr`, `connectivity`, `content_capture`, `window`,
`voiceinteraction` (and `activity` through `ActivityManager.getService`).

**The gap, AOSP to Win.apk** (`windows/scripts/LinkScan.java` on
`aosp-app.jar` first, as the host would put it): with only the classes taken
and their closure, 275 member references and 92 classes did not link. After
A1's rewrite (below): **159 members and 43 classes**, all on paths the
lifecycle test does not take (services, receivers, providers, backup agents,
`UiAutomation`, instrumentation, `RemoteServiceException`s, assist,
`SplashScreen`, voice interaction, heap dumps). The largest groups left:
`Debug$MemoryInfo` (48, `dumpMemInfo`), `ActionBar`'s hidden members were
closed by taking AOSP's `ActionBar`, `Service` (8: `attach`,
`createServiceBaseContext`... step A3), `ContentProvider` (5, A3),
`UiAutomation` (7, instrumentation only).

**The gap, Win.apk to AOSP** (Win.apk's own classes calling a member of a
Win.apk `android.app` class that AOSP's lacks, counted per call site by
`tools/SiteScan`, a LinkScan variant, against the jar first; classes the jar
replaces are not counted):

| | Members | Call sites | Files |
|---|---:|---:|---:|
| Before A1's forwards | 58 | 196 | 47 |
| After them (`Application.macapk_get`, `getInstance`, `macapkAppTargetSdk`, `macapkTargetSdkKnown` added to AOSP's `Application`, 80 sites) | 54 | 116 | 16 |

The 116 left are all in `com.macapk.loader`: `ActivityHost` 65, `Boot` 16,
`DisplayConfig` 12, `TouchDispatcher` 8, then 1 or 2 in `StatusBarDriver`,
`SpecialAccessHandler`, `GamepadBridge`, `ViewCanvasDriver`,
`InstallIntentHandler`, `DocumentPickerHandler`, `DeviceCredentialHandler`,
`CameraCaptureHandler`. By owner: `Activity` 94 sites (`macapkContentView` 13,
`macapkDispatchFragmentState` 9, `macapkRecordOrientation` 6,
`macapkOwnsWindowBox` 6, `macapkClaimHostResult` 6, `macapkWindowToken` 4...),
`Application` 19 (the `macapkInit*`/`macapkSet*` Boot uses to build Win.apk's
Application, which goes when AOSP's `LoadedApk` makes it). Beyond the members
that do not link, Win.apk's loader references the classes the jar replaces at
352 sites in 31 files (`ActivityHost` 145, `Boot` 34, `TouchDispatcher` 30,
`PackageInstallerBackend` 23, `DisplayConfig` 17, `SpecialAccessHandler` 15),
and the rest of the framework at 217 sites in 64 files (`PreferenceActivity`
34, `PackageManager` 25, `NativeActivity` 11, `Context` 9): most link (public
API), and each is behaviour to check, not code to port. `ActivityHost` itself
is called from 43 files at 230 sites (`TouchDispatcher` 41, `Activity` 39,
`MacApkPackageManager` 13, `Boot` 13, `HostUrlIntake` 10, `View` 10,
`SurfaceView` 10).

**What Win.apk's own lifecycle adds that apps rely on**, by `ActivityHost`'s
public entries (45 `macapk*` methods there, 81 in `Activity`, 19 in
`Application`): the start's resolution (implicit starts through the package
manager, `ActivityNotFoundException`'s two messages, `<activity-alias>`, the
exported-only rule for targetSdk 34); the host's hand-offs (a URL to the
browser, `openHostUrlGated`; the camera, document picker, device credential
and permission windows answered as an activity result,
`MacApkHostResult`); the window (focus, orientation requests forwarded to the
host window, secure content, which activity owns the window box); the task
and the host window (minimize = task to back: `onPause`, `onStop`; restore =
to front; close = the task removed, `macapkTerminate`); recreate on a
configuration change; fragments' state dispatch and `ViewTree*Owner`s for
Compose. Each of these is either system-side policy that moves into the
in-process task manager (the start's resolution, the host hand-offs, the
task's moves, focus, orientation), or an app-side gap AOSP's code does not
have (fragment dispatch, view-tree owners, recreate).

**Cost of A1's jar and test** (measured):

- `aosp-app.jar`: 601 classes (321 taken, 260 of closure, ours), 3.0 MB
  uncompressed, **1.3 MB** as a jar, 1.25 MB under `xz -9e` (the
  installer's estimate). Win.apk classes it shadows: 60 (0.24 MB, 0.11 MB
  compressed), never loaded with it on.
- The test (cold JVM, no CDS): an Application and two activities, their
  windows added and removed through `ViewRootImpl`, the whole run 1.3 s of
  which 0.64 s from `AospApp.start` to the task's end; 111 classes loaded
  from `aosp-app.jar` (463 from the views jar, 154 resources, 216 graphics,
  194 Win.apk's framework, 49 the loader); heap 42 MB after the first young
  collection.

Not measured: an app's start in `winapk-runtime.exe` (A1's second part),
memory on Windows, CDS with the jar.

## 2. What AOSP's app layer needs, and what Win.apk has

| Need | On Android | Win.apk today | Plan |
|---|---|---|---|
| The process's start | Zygote forks, `ActivityThread.main`: main looper, `attach`, `IActivityManager.attachApplication`; AMS answers `bindApplication`, ATMS schedules the first launch | `Boot.run`: makes Win.apk's `Application` (`macapkInit*`), `ActivityHost.start(intent)` | `AospApp.start` (A1): the services registered, AOSP's `ActivityThread` attached (`winapkAttach`, added at build time: `attach` is private); the in-process activity manager binds the app and the task manager launches. Boot calls it instead of `ActivityHost.start` under the switch (A1, second part) |
| The task, its activities and their states | ActivityTaskManagerService, `Task`, `ActivityRecord`, `ActivityStarter`, `ActivityTaskSupervisor` in system_server (in android-all too: `com.android.server.wm`, 671 classes, entangled with WindowManagerService's hierarchy) | `ActivityHost`'s LIFO list | `com.winapk.aospapp.Tasks`: one task, the transactions in the order the supervisor sends them (A1: standard launch mode, results, finish); launch modes and flags in AOSP's `ActivityStarter` rules, the task to back and front, relaunch (A2). Running AOSP's own ATMS in-process is not proposed: it needs WMS's window containers |
| `IActivityClientController` | system_server | `ActivityHost` and `Activity`'s own fields | `ActivityClientService` (17 of 66 in A1; the rest in A2: task description to the host window's title and icon, orientation to the host, `moveActivityTaskToBack`, `navigateUpTo`, `onBackPressed`) |
| Contexts | `ContextImpl` over `LoadedApk`; managers from `SystemServiceRegistry`'s fetchers over system_server's binders | `Application` is the context; managers built by `Context`/`Application.getSystemService` against the host | AOSP's `ContextImpl`, with `getSystemService`/`getSystemServiceName` and the permission checks given Win.apk's `Context` bodies at build time (its managers talk to the host; its permission answers come from the signed grants), and `getPackageManager` the host's `MacApkPackageManager` |
| The class loader | `LoadedApk` → `ApplicationLoaders` → `PathClassLoader` over the APK | `AppClassLoader` over the converted DEX | `LoadedApk.createOrUpdateClassLoaderLocked` gives the host's loader (rewrite), then makes the app's `AppComponentFactory` with it as AOSP does |
| Resources per context | `ResourcesManager` (activity tokens, override configurations) | `ResBackend`/`AospRes`, one per configuration override | AOSP's `ResourcesManager` (taken), over the same port; `AospRes`'s own override cache then goes (A2) |
| The package manager the layer asks directly | `IPackageManager` | `MacApkPackageManager` (a `PackageManager`) | `PackageService` answers the 8 calls from the bound app's `ApplicationInfo` (2 in A1) |
| Services, receivers, providers | `ActivityThread.handleCreateService`, `LoadedApk.ReceiverDispatcher`, `installContentProviders`, through AMS | `ServiceRegistry`, `ContentProviderRegistry`, Win.apk's broadcasts | A3: through `ActivityThread` and an in-process AMS; `Service` and `ContentProvider` become AOSP's |
| Process-wide plumbing | DDM, the binder GC watcher, `VMRuntime`'s process state, `StrictMode` defaults, the graphics driver's environment, the network security config | none, or the host's | Answered as nothing (`INERT_CLASSES`, `INERT_METHODS`), as a user build without developer options does; the TLS trust stays Boot's |
| Graphics at bind | `Typeface.setSystemFontMap`, `HardwareRenderer` setup, `Canvas` compatibility | `AospGfx` at start | The font map stays the graphics' (the zygote's part); HWUI's setup waits for V3; `Compatibility.setTargetSdkVersion` runs when AOSP's graphics are on |

## 3. The collision, and the seams

The rule is the views': one implementation per name per JVM, chosen at JVM
start, AOSP's jar first. Kept Win.apk's in A1: `android.content` (`Context`,
`ContextWrapper`, `Intent`...: the app-visible base and the host's managers),
`ActivityManager` (its public API answers from the host; `getService()` goes
to the in-process service through `Compat`), `Service`, `IntentService`,
`NativeActivity`, `PendingIntent`, the notifications, every manager. Taken
alone from an outer class Win.apk keeps: `ActivityManager$TaskDescription`
(AOSP's `Activity` builds one from its theme). Taken because AOSP's code
reaches hidden members: `ActionBar` (and the views jar's
`WindowDecorActionBar` with it).

A0, the seam, as `ViewHost` and `ResBackend` were: an `AppHost` interface in
`com.macapk.loader` that the 116 Win.apk sites (and the 230 calls into
`ActivityHost`) go through, implemented by today's classes, so that with the
switch off nothing changes byte for byte and with it on the AOSP side answers
from `ActivityThread`'s records. A1 added four statics to AOSP's `Application`
at build time instead (`ApplicationStatics`), which is enough for the test;
A4 replaced them with `com.macapk.loader.app.Apps`.

## 4. Steps

Each behind `WINAPK_AOSP_APP=1` (off by default, and only with the graphics,
resources and views on), gated as the views were: `regress.py` on the UI
probe and the screens sweep with no `REGRESSED`, `FARTHER` or `MISSING`, the
smoke apps still drawing, and lifecycle traces compared with a device's (the
probe gains a lifecycle section; Robolectric's `ActivityScenario` is not an
oracle for order, the CTS lifecycle tests' expectations are).

| Step | What | Gate |
|---|---|---|
| A0 | `AppHost` seam: the 116 sites and `ActivityHost`'s 230 callers through it; switch off byte-identical | `ours.txt`, sweeps, `HostTouchTest` identical |
| A1 | **First part built** (below): `aosp-app.jar`, the rewrite, the in-process activity manager, task manager, client controller; the lifecycle test. **Second part**: the host switch in `host/aosp_gfx.cpp` (class path, `-Dwinapk.aosp.app=1`, the log line, CDS archive name), `Boot` starting the app through `AospApp.start` with its `ApplicationInfo`, configuration, `MacApkPackageManager`, `AppClassLoader` and launch intent; the window path (`ActivityThread.handleResumeActivity` adds the decor, as the views already expect); the run's end when the task empties | the probe and the ten smoke apps with all four switches; the lifecycle test |
| A2 | The task as ATMS keeps it: launch modes and intent flags (`ActivityStarter`'s rules), `startActivities`, results across finishes, task to back/front (minimize, restore), close, configuration changes and relaunch (`ActivityRelaunchItem`, `configChanges`), `TopResumedActivityChangeItem` and window focus, orientation and task description to the host window, Back (`onBackPressed` through the client controller); the host hand-offs (`MacApkHostResult`) as activities of the system's | probe lifecycle section, sweeps, smoke apps, the apps tried in `docs/APPS.md` |
| A3 | Services, broadcasts and providers through `ActivityThread` and the in-process AMS; `Service`, `ContentProvider` AOSP's; `ServiceRegistry`, `ContentProviderRegistry` retired | apps with services (Jellyfin, Jitsi), WorkManager |
| A4 | Delete what the layer replaced: Win.apk's `Activity`, `Application`, `Instrumentation`, `LoadedApk`, `ActivityThread`, the framework `Fragment`s (60 classes) and `ActivityHost`, and the `MACAPK_*` switches that guarded its fixes | footprint, CI |

## A1 as built (first part)

`windows/scripts/build_aosp_app.sh <stage> [deps]` takes from android-all 14
`android.app.servertransaction.*`, `ActivityThread`, `ClientTransactionHandler`,
`ConfigurationController`, `Activity`, `ActivityClient`, `ActivityTaskManager`,
`Instrumentation`, `ContextImpl`, `LoadedApk`, `Application`,
`ResourcesManager`, the AIDL interfaces between the app and the system, the
framework `Fragment`s and `LoaderManager`, the transition coordinators,
`ActivityOptions`, `TaskStackBuilder`, `ActionBar`,
`ActivityManager$TaskDescription` and a few more (321 classes), then a jdeps
closure of what they name that Win.apk lacks, bounded to AOSP's plumbing
(never `SystemServiceRegistry` or a manager). `windows/java/aosp_app`
(`com.winapk.aospapp`) is compiled into it:

- `AospApp.start(applicationInfo, configuration, resolver, classLoader,
  packageManager, launchIntent, onTaskEmpty)`: registers the services and
  attaches a new `ActivityThread` on the main looper's thread.
- `ActivityManagerService` (`IActivityManager`): `attachApplication` calls
  `bindApplication` with the host's `ApplicationInfo` and configuration, then
  the task manager launches the first activity.
- `ActivityTaskService` (`IActivityTaskManager`): `startActivity` resolved by
  the host, `START_INTENT_NOT_RESOLVED` when nothing matches (so
  `Instrumentation` throws `ActivityNotFoundException` as on Android).
- `ActivityClientService` (`IActivityClientController`): the reports and
  `finishActivity`, the questions answered on the system thread.
- `Tasks`: one task on a `winapk-system` thread, as system_server's binder
  threads: a start pauses the resumed activity first and launches the new
  one (launch + resume in one transaction) when it reports paused; the paused
  one stops when the new one is idle; a finish pauses, resumes the one under
  it with its results first (`ActivityResultItem` before
  `ResumeActivityItem`), and destroys the finishing one when the one under it
  is idle; the last one destroyed ends the task.
- `PackageService` (`IPackageManager`): the uid's package, the app's info.
- `SliceService` (`ISliceManager`, "slice"): what AOSP's `SliceManager` asks,
  answered as a phone with no slice host: pins (the first reaches the
  provider's `onSlicePinned`), grants, `hasSliceAccess` false. The
  `android.app.slice` package is AOSP's, taken with the other platform classes
  apps extend that Win.apk had none of (`aosp_app_take.sh`, `PLATFORM_TAKE`).
- `SafetyCenterService` (`ISafetyCenterManager`, "safety_center"): what AOSP's
  `SafetyCenterManager` asks, answered as a phone whose config disables Safety
  Center: `isSafetyCenterEnabled` false, a source's data dropped, nothing to
  read back. `android.safetycenter` is taken whole (`PLATFORM_TAKE`).
- `Compat`: below. (`ApplicationStatics`, the four statics added to AOSP's
  `Application`, went at A4: the framework asks `com.macapk.loader.app.Apps`.)

`windows/scripts/AospAppRewrite.java` (ASM, as `AospViewsRewrite`) then:
generates each service as a subclass of its AIDL `Stub`; makes 2 natives inert
(`ActivityThread.nPurgePendingResources`, `nInitZygoteChildHeapProfiling`) and
leaves 1 throwing by name (`Activity`'s); sends 33 members Win.apk's
classes lack to `Compat` with AOSP's bodies (`FileUtils`, `Environment`'s app
paths, `ApplicationInfo.getCodePath`, `UserHandle`, `CompatChanges` (every
change on, as the views answer), `isStartResultFatalError`...); answers zero
for calls into 12 classes of system plumbing (`DdmHandleAppName`,
`RuntimeInit`, `BinderInternal`, `VMDebug`, `GraphicsEnvironment`,
`AppCompatCallbacks`, `SystemServiceRegistry`,
`NetworkSecurityConfigProvider`...) and for 58 named members (`VMRuntime`'s
process state, `StrictMode`'s defaults and checks, an `Intent`'s parcel hooks,
`Os.chown`/`setxattr`, HWUI's setup, the system font map); reads 12 hidden
`ApplicationInfo`/`InstrumentationInfo` fields as null; gives `ContextImpl`'s
service lookups and permission checks Win.apk's `Context` bodies and its
`getPackageManager` and `createAttributionSource` `Compat`'s; gives
`LoadedApk` the host's class loader; empties
`ActivityThread.initZipPathValidatorCallback` and
`Activity.notifyVoiceInteractionManagerServiceActivityEvent`; adds
`ActivityThread.winapkAttach`, the four `Application` statics and four
zero-answering members AOSP's views call on an `Application`/`Activity`
(autofill and content-capture options).

Found and fixed in the other layers on the way (separate commits): the
libandroidfw port's `LoadedPackage.CollectConfigurations` walked its type map
by index and threw for every APK (`AssetManager.getSizeConfigurations`, which
`ActivityThread` asks at each launch; `AospResRewrite` points it at
`PortFixes`, the C++ loop); `AnimationHandler` (views jar) called
`Animator.getBackgroundPauseDelay`, which Win.apk's `Animator` lacks, at every
window's removal (`ViewRootImpl.doDie`).

`windows/test/aospapp/run.sh <stage> <deps> [out] [whole framework jar]` (the
last for a stage `stage_runtime.sh` filtered) builds the jar, the test's host
library (`plainjvm_host.cpp`: the log funnel, `setenv`, a headless 360x640 dp
display, and the host's own graphics redirect, `host/aosp_gfx_redirect.cpp`,
binding Robolectric's Linux native runtime onto AOSP's graphics as
`winapk-runtime` does) and `AospAppLifecycleTest`, and runs it. The order it
checks, and gets:

```
App.onCreate
First.onCreate First.onStart First.onResume
First.onPause                                   (First starts Second for a result)
Second.onCreate Second.onStart Second.onResume
First.onStop First.onSaveInstanceState          (once Second is idle)
Second.onPause                                  (Second finishes)
First.onRestart First.onStart First.onActivityResult First.onResume
Second.onStop Second.onDestroy                  (once First is idle)
First.onPause First.onStop First.onDestroy      (First finishes)
task empty
```

with each activity's window added to and removed from AOSP's
`WindowManagerGlobal` and the views' in-process window manager. Two things the
first runs showed are AOSP's own behaviour, kept: an activity on
`Theme.NoDisplay` that does not finish before `onResume` ends throws
(`performResume`), and an activity whose successor finishes before it is idle
is never stopped.

## A1 as built (second part): every app on it

- **The seam (A0).** `com.macapk.loader.app.AppHost` (in the loader) is the
  runtime's door into the app layer, `com.winapk.aospapp.AospAppHost` (in the
  jar) its one implementation: `start`, and the host window's entries (closed:
  every activity destroyed top first, waited for; minimized: the task behind;
  restored: in front, or the launcher activity again on an empty task),
  `configurationChanged`. `ActivityHost.macapkTerminate`,
  `macapkHostMinimized` and `macapkTaskToFront`, which `native_cocoa.cpp`
  calls, hand over to it; `DisplayConfig` hands it the display's changes.
- **The start.** `Boot` builds the app's `ApplicationInfo` and its
  `MacApkPackageManager` (which resolves every intent the task manager is
  asked to start), the host's input, accessibility and gamepad bridges, and
  the first intent (the launcher's, `MACAPK_LAUNCH_ACTIVITY`, or one another
  app or a URL handed over), then `AppHost.start` on the app's main thread and
  the main looper. The app's providers are installed by Win.apk's installer
  right before `Application.onCreate`, where `handleBindApplication` installs
  them (step A3 makes them AOSP's). The layout sweep (`MACAPK_LAYOUT_SWEEP`)
  runs after the bind, with no activity.
- **The host.** `aosp_app_prepare` (`host/aosp_gfx.cpp`) puts
  `lib\aosp-app.jar` first on the class path of every JVM with the framework;
  `stage_runtime.sh` builds it and drops the classes it shadows from the
  shipped `android-framework.jar`; the installer takes it with the stage
  (`footprint_check.py`: a part of its own, 2 MB).
- **The windows.** An activity's window is added by AOSP's
  `handleResumeActivity` straight to the window manager: the views' window
  session now makes the host surface for the first application window and
  follows an activity window's SurfaceViews (GL frames under the windows).
- **What a real app's start reached**, each answered as AOSP's would be:
  BlockGuard's policies (none), `FileUtils.sync`, `StructStat.st_mtim`,
  `Activity.getDlWarning`, Android's typed XML for SharedPreferences' file
  (`Xml.newFastSerializer`, in the resources' rewrite), `startActivities`
  (SGT Puzzles), Up navigation (`navigateUpTo`, `shouldUpRecreateTask`), Back
  at a launcher-started root (the task behind, as Android 12 does).

Checked under Wine (the runtime built from the tree with clang-mingw, a stage
of its own, headless, 150 frames): all ten smoke apps draw their first screen
(Jerboa's notice dialog, Klooni's menu over its GL view, SGT Puzzles' chooser,
Editor's dialog, Fossify Notes, Jellyfin, Jitsi's splash, PPSSPP's logo,
Tensy, Open Sudoku's "Latest changes"), the UI probe's lines are byte for byte
those of the same probe on a stage without the app layer, and the plain-JVM
lifecycle test passes.

## A2 as built, A3 begun

- **Launch modes and flags** (`Tasks.start`, from `ActivityStarter` and
  `Task.performClearTop` for the one task): `singleTop` and
  `FLAG_ACTIVITY_SINGLE_TOP` give the top record the intent;
  `FLAG_ACTIVITY_CLEAR_TOP`, `singleTask` and `singleInstance` finish what is
  above an instance and give it the intent (a standard activity without
  `SINGLE_TOP` is finished and made again); `REORDER_TO_FRONT` moves it up;
  `NEW_TASK|CLEAR_TASK` finishes the whole task first. A reused record gets
  `NewIntentItem` when resumed, else its intents at its resume.
- **Results, Up and Back.** Results are delivered before resume
  (`ActivityResultItem`); `navigateUpTo` and `shouldUpRecreateTask` as
  `Task.navigateUpTo`; Back at a launcher-started root moves the task behind
  (Android 12's rule), elsewhere the activity finishes.
- **Configuration.** The display's changes (`DisplayConfig`: a resize, a
  turn, density) go to the process (`ConfigurationChangeItem`) and to each
  activity: relaunched (`ActivityRelaunchItem`) unless its `configChanges`
  covers the change, else `ActivityConfigurationChangeItem`. A stopped
  activity (behind another, or the task minimized) is told only when it
  comes back, as `ensureActivityConfiguration` skips an invisible one: a
  relaunch then carries its pending results and intents and resumes it.
- **The host's hand-offs.** A start no activity of the app answers, or
  another package's, goes to the host (`ActivityHost.resolveImplicitToHost`:
  the camera, pickers, installs, URLs, the launcher); their results come back
  through `HostResult` as `ActivityResultItem`s, with the host's window
  covering the task meanwhile (`AppHost.hostCover`). `requestPermissions`
  (`ACTION_REQUEST_PERMISSIONS`) asks the launcher's prompt and answers with
  the result intent Android's permission controller gives.
- **Starts from outside.** A URL the host hands over (a sign-in's redirect
  from the browser tab, `HostUrlIntake`) or another app's start
  (`IntentInbox`) reaches `AppHost.deliver`: a MAIN/LAUNCHER intent brings the
  task to the front, anything else starts as `ActivityStarter` starts it,
  with the host's window in front. One that opened the app is the first
  activity's intent; the launcher's stays the one an emptied task starts.
- **Before the Application.** On the main thread before the bind, as
  `handleBindApplication`'s `setupGraphicsSupport`: `TMPDIR` and
  `java.io.tmpdir` on the app's cache, then pairip's gate; before its
  `onCreate`: a SoLoader superpack's libraries exposed, the providers, the
  FCM channel when Win.apk's push backend is on, the broker attached.
- **Receivers, broadcasts and services** at first went to Win.apk's own
  dispatch through `ContextImpl` (A3's first part); "A3 as built" below has
  them on `ActivityThread`.

Checked under Wine: the ten smoke apps draw, Open Sudoku resized to landscape
is relaunched and redrawn, `--install-only`, `--gms`, `--verify-apk` and
`--system` answer as before. Not yet: `ActivityThread`'s own services,
receivers and providers over the in-process activity manager (A3's second
part), and the deletions (A4).

## A3 as built

- **Services** (`com.winapk.aospapp.ActiveServices`, as ActivityManagerService's
  ActiveServices): `Service` and `IntentService` are AOSP's; `ContextImpl`'s
  `startService`, `stopService`, `bindService` and their kin reach the
  in-process activity manager, which keeps a record per component (started or
  not, its bindings per intent and the connections on each) and runs the
  app's service through `ActivityThread`'s create, args, bind, unbind and
  stop; `Service.stopSelf` is `stopServiceToken` (only the newest start
  stops it). A create goes to the main queue running `handleCreateService`,
  in order with the client transactions after it. Another app's service is
  the system broker's (`SystemClient`), as before. The binds the host waits
  for (one the broker makes into this app for another, a job's, an account
  authenticator's, the measurement service) are `AppHost.bindServiceNow` and
  `jobBind`: `handleCreateService` and `handleBindService` on the main thread,
  waited for from another. Win.apk's `Service`, `IntentService` and
  `ServiceRegistry` are deleted.
- **Broadcasts** (`ActiveBroadcasts`, as the broadcast queue):
  `BroadcastReceiver` is AOSP's; registered receivers through their
  `LoadedApk.ReceiverDispatcher` (`scheduleRegisteredReceiver`), the
  manifest's through `ActivityThread.handleReceiver` (`scheduleReceiver`),
  stickies, ordered broadcasts one receiver at a time, the result carried by
  `finishReceiver` to the result receiver. Other apps hear a broadcast through
  the broker; one from another app (`AppHost.deliverBroadcast`) reaches
  exported receivers only. The battery sticky and the minute's `TIME_TICK`,
  which a device's system_server sends, are sent from there. Win.apk's
  receiver registry and `BroadcastReceiver` are deleted.
- **Providers**: `Boot.appProviders` lists the app's providers for the
  process (enabled, with authorities, of this process, in initOrder),
  `bindApplication` carries them and `ActivityThread.installContentProviders`
  creates, attaches and publishes them before `Application.onCreate`; the
  activity manager registers what is published in `ContentProviderRegistry`
  (which Win.apk's `ContentResolver` reads) and answers `getContentProvider`.
  `ContentProvider` stays Win.apk's, with AOSP's `Transport` shape
  (`getIContentProvider` over `ContentProviderNative`): its in-process checks
  also stand in for providers of another package that live in the app's
  process (the system's media, documents, settings, installed by `Boot`),
  which AOSP's `Transport`, deciding by the calling uid, would not check in
  one process. `attachInfo` runs `onCreate`, as AOSP's does.
- **Contexts**: Win.apk's `ContextWrapper` hands starts, services, binds,
  broadcasts and receivers to its base context as AOSP's does (an
  `Application`'s `startActivity` had nowhere to go after A4).

The lifecycle test checks a started service that stops itself, a bound one
through connect, unbind and destroy, a receiver of the application's
context, a manifest receiver's ordered broadcast and its result, and a
manifest provider created before `Application.onCreate` and queried through
the app's `ContentResolver`. Checked under Wine: the ten smoke apps draw with
the error lines of before, the service host step, `--install-only`, `--gms`,
`--verify-apk`, `--system`, and `BinderTransportTest` in
`winapk-binder-test.exe`. Not checked: an app attached to a running broker
under the harness (the broker answers "not" an app the launcher did not
register).

## A4 as built

- **Deleted:** Win.apk's `android/app/Activity.java`, `Application.java`,
  `ActivityThread.java`, `Instrumentation.java`, `LoadedApk.java`,
  `ActionBar.java`, `ActivityOptions.java`, `DirectAction.java`, the
  framework `Fragment`, `FragmentManager`, `FragmentManagerImpl`,
  `FragmentTransaction`, `FragmentBreadCrumbs`, `DialogFragment`,
  `ListFragment`, `ListActivity`, `LoaderManager`, `PictureInPictureParams`,
  `PictureInPictureUiState`, `SharedElementCallback`, `TaskStackBuilder`,
  `VoiceInteractor` (22 files, about 12,300 lines), `MacApkToolbarActionBar`,
  `FragmentInflaterFactory`, `FragmentLifecycle`, `AutoRefresh`, and
  `ActivityHost`'s activity stack, lifecycle, window policy, focus and
  view-tree lifecycle code (5,375 lines to about 1,300), `Boot`'s own start
  (846 lines) and `DisplayConfig`'s own delivery.
- **The build:** `build_java.sh` compiles the framework against android-all's
  classes of `windows/scripts/aosp_app_take.sh`, the list `build_aosp_app.sh`
  takes them by, as for the graphics. The framework's 118 sites that asked
  Win.apk's `Application` statically ask `com.macapk.loader.app.Apps`
  (`ActivityThread.currentApplication()` and its `ApplicationInfo`).
- **What the old stack did, now the task manager's:** the window's
  orientation per activity (`Tasks.windowOnStart`, `windowOnRevealed`,
  `requestOrientation`: the box owner is the topmost activity whose theme
  occludes, read as `ActivityRecord.occludesParent` reads it; the host turns
  the window through `AppHost.Start.orientation`); the close (`Tasks.terminate`
  pauses the visible top first, as `finishIfPossible` does, so the host takes
  the window off screen then, and the wait keeps ticking the renderer); a dying
  secondary process's activities and a leaving one's owed starts
  (`AppHost.finishActivities`, `pendingStarts`, for `MacApkProcessExit`);
  FLAG_SECURE (`AospWindows.secureChanged`); locale and night mode changes
  (`LocaleManager`, `Boot`'s appearance flip) through
  `AppHost.configurationChanged`. `Context.startActivity` on a context of
  Win.apk's own hands the start to the application's context, a new task.
- **Tests:** `--java-test` puts `aosp-app.jar` on the class path, and CI's
  framework tests do too (a `View` names `Application`).

Checked under Wine: the ten smoke apps draw (Tensy now turns the window for
SDL's `SENSOR_LANDSCAPE`, as a device does), the UI probe's 2166 lines are
those of the base, the service host and `--system` start, `--install-only`,
`--gms` and `--verify-apk` answer as before, Open Sudoku resized to landscape
is relaunched; on Linux the lifecycle and accessibility tests and the CI's
plain-JVM framework tests pass.

## 5. Size, start and memory

- **Disk.** `aosp-app.jar` 1.3 MB (1.25 MB in the installer, measured); the
  Win.apk classes it replaces 0.11 MB compressed, plus `ActivityHost` and the
  lifecycle parts of `Boot` at A4 (not measured). Net about +1.1 MB.
- **Start.** The test's bind and two activities take 0.64 s cold without CDS
  on Linux (measured); in `winapk-runtime.exe` the framework archive takes
  the jar's classes like the rest (the jar is on the class path its key is
  made of): under Wine, a third start of Fossify Notes mapped an archive
  holding `ActivityThread`, the client transactions and `com.winapk.aospapp`.
  The dump verifies every class it archives, so a class AOSP's `Activity`
  names must be there: `ToolbarActionBar` is taken for that. `handleBindApplication`
  does more than Win.apk's start (a system context, `ResourcesManager`'s
  bookkeeping); expected under 0.1 s warm, to be measured on Windows.
- **Memory.** 42 MB of heap after the first collection in the test, with the
  graphics, resources and views; the app layer's own share not separated
  (estimated small: two `ContextImpl`s and an `ActivityClientRecord` per
  activity).

## 6. Risks and unknowns

- **The system side is Win.apk's own code.** AOSP's ATMS cannot run here
  (WindowManagerService's containers), so launch modes, flags and task rules
  are rewritten in `Tasks` from `ActivityStarter`; that is policy code to
  copy carefully, not app-process code, and the only part of this layer that
  is not AOSP's.
- **Win.apk's additions.** 116 call sites (and 230 into `ActivityHost`) carry
  the host hand-offs, the window box, orientation, focus and minimize/restore;
  each needs a home in the seam or the task manager (A0, A2).
- **Behaviour apps depend on today.** Win.apk's `Activity` has 81 `macapk*`
  members and 57 `MACAPK_*` switches, each a fix found in an app; with AOSP's
  `Activity` most become unnecessary, some move to the system side; the
  screens sweep and the smoke apps must show which.
- **Intents.** An `Intent`'s parcel hooks are inert here (no process
  boundary), including `migrateExtraStreamToClipData`, which on Android
  grants `EXTRA_STREAM`'s URIs through `ClipData`; Win.apk's own URI grants
  (`MacApkUriGrants`) must still see them (A2/A3).
- **Two package managers.** The context's is `MacApkPackageManager`; the
  layer's `IPackageManager` answers 2 of the 8 calls it makes.
- **API level.** As for the views: AOSP 14's app layer presenting SDK 35/36.

Not verified:

- `winapk-runtime.exe`, Windows, Wine: no host switch yet;
- a real app's classes through `AppClassLoader`, AppCompat's and androidx's
  `FragmentActivity`/`ComponentActivity` on AOSP's `Activity`;
- a window drawn and presented (the test's activities have no content and
  present nothing), input, configuration changes, relaunch;
- services, receivers, providers;
- start time and memory on Windows, CDS with the jar.
