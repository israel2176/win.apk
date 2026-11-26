# What Android apps ask for that Win.apk does not answer

Status: a survey, November 2026. Nothing here is fixed; each row says what Win.apk does today, what
Android does, which of the surveyed apps reach it, and how bad it is. The full generated lists are in
[`api-gaps-lists.md`](api-gaps-lists.md); the tools that make them are in `windows/scripts/api_gaps/`.

## What was measured

- **The runtime**: the jars, natives and JRE of `runtime-stage`, the artifact of main's newest green CI
  run when this was written (ci.yml run 45853914292, commit `f4cb4f4`), not a local build: what ships,
  after `stage_runtime.sh` dropped the framework classes AOSP's jars replace. Class path order as
  `runtime_main.cpp` `jvm_options` builds it: `aosp-app.jar`, `aosp-views.jar`, `aosp-resources.jar`,
  `aosp-graphics.jar`, `android-framework.jar`, `macapk-loader.jar`, `bcprov`; `java.*` from the shipped
  `jre/` with the `java_base_patch` overlay first. The first jar that has a class wins.
- **The API**: every public and protected class, method and field of the SDK's `android.jar` for levels
  34, 35 and 36 (`platform-34-ext7_r03`, `platform-35_r02`, `platform-36_r02`), each with the level it was
  added at from 36's `data/api-versions.xml`. Win.apk tells an app it runs on
  `max(clamp(targetSdk, 29, 36), minSdk)` (`PresentedSdk.java`), so an app targeting 36 sees a level-36
  device and takes its libraries' `SDK_INT >= 35` and `>= 36` branches; most of the framework underneath
  is AOSP 14's (level 34).
- **What AOSP really does**: android-all 14 (`14-robolectric-10818077`, AOSP's own class bodies) is the
  reference a body is compared with to call it a stub.
- **The apps**: the eleven of `windows/test/compat/smoke.txt` and `windows/test/screens/gate.txt`, at
  their pinned F-Droid versions (Gallery and OpenSudoku checked against gate.txt's SHA-256):

  | App | targetSdk | Presented level | Kind |
  |---|---:|---:|---|
  | org.billthefarmer.editor 198 | 35 | 35 | Java, no native code |
  | org.fossify.notes 13 | 36 | 36 | Kotlin, views |
  | org.fossify.gallery 30 | 36 | 36 | Kotlin, views, media3 |
  | com.jerboa 88 | 36 | 36 | Compose |
  | name.boyle.chris.sgtpuzzles 149 | 36 | 36 | JNI C, Canvas |
  | dev.lonami.klooni 860 | 29 | 29 | libGDX |
  | se.voxelmanip.tensy 2 | 35 | 35 | SDL, native GL |
  | org.ppsspp.ppsspp 119030000 | 35 | 35 | NativeActivity |
  | org.jitsi.meet 26000004 | 35 | 35 | React Native (Hermes) |
  | org.jellyfin.mobile 2070399 | 36 | 36 | WebView shell, media3 |
  | org.moire.opensudoku 20260923 | 37 | 36 | arm64 only |

  An app's use is read from its dex: every framework class, method and field an **instruction** names
  (invoke, field access, new-instance, const-class, check-cast, instance-of), with the calling package,
  plus a heuristic for reflection (`Class.forName`/`getMethod` near a framework class name). A use is
  not proof of a call: AndroidX guards most of them by `SDK_INT`, and the severity below says when the
  guard lets Win.apk's presented level through. For the worst rows the call site was read in the
  app's code (converted with `com.macapk.dex.Main`).
- **The runtime's own evidence**: the logs of the same build's smoke run (PR CI run 45849815823, the
  same code as `f4cb4f4`): its "no answer in this process" lines and `stub_hits.h`'s summary.

### Severity

- **Certain crash**: the app's code reaches it on Win.apk's presented level with nothing to catch it:
  `NoSuchMethodError`, `NoClassDefFoundError`, `AbstractMethodError` or a null where Android never
  returns one. "When ..." says what the user does to get there.
- **Wrong answer**: the call returns, but not what a phone returns: an empty list, `false`, a no-op,
  a constant. The app goes on with a wrong picture of the device.
- **Unused**: nothing in the surveyed apps reaches it (or only on a level Win.apk never presents).
  Still a gap for other apps.

## The twenty that matter most

In order: certain crashes the surveyed apps reach first, then wrong answers that many apps see.

| # | Gap | Win.apk today | Android | Apps | Severity |
|---:|---|---|---|---|---|
| 1 | `Activity.startIntentSenderForResult` (and `ContextImpl.startIntentSender`) | AOSP's `Activity` calls `IntentSender.getTarget()` and `getWhitelistToken()`, which Win.apk's `IntentSender` (android-framework.jar) does not have: `NoSuchMethodError` | Sends the IntentSender and returns its result | 9 of 11 reference it (AndroidX `ActivityResultContracts.StartIntentSenderForResult`); Gallery and Notes use it for `MediaStore.createDeleteRequest`/`createWriteRequest`/`createTrashRequest` | Certain crash, when the app asks the user to confirm deleting or editing a shared photo or file, a Google sign-in, an in-app update... |
| 2 | `ContentResolver.getResourceId` (hidden) behind `ImageView.setImageURI` | AOSP's `ImageView.getDrawableFromUri` calls it for `android.resource://` URIs; Win.apk's `ContentResolver` lacks it and `ContentResolver$OpenResourceIdResult`: `NoSuchMethodError` | Loads the resource | 10 of 11 reference `setImageURI` | Certain crash, when an `android.resource://` URI is set (a `content://` or `file://` one works) |
| 3 | `ContextImpl.grantUriPermission`/`revokeUriPermission`/`checkUriPermission` | Call `ContentProvider.getUriWithoutUserId`/`getUserIdFromUri`, missing from Win.apk's `ContentProvider`: `NoSuchMethodError` | Grants or checks the URI permission | Gallery (its cropper and Fossify Commons' sharing) | Certain crash, when the app shares or crops a file through a `FileProvider` URI it grants |
| 4 | `ActivityOptions.setAllowPassThroughOnTouchOutside` (36) | Missing (AOSP 14's `ActivityOptions`) | Sets the option | Jerboa: androidx.browser's `CustomTabsIntent.Builder.build()` calls it whenever `SDK_INT >= 36` (read in Jerboa's code) | Certain crash, when Jerboa opens a link |
| 5 | `BackEvent.getFrameTimeMillis` and `BackEvent(FFFIJ)` (36) | Missing (AOSP 14's `BackEvent`) | Returns the event's frame time | Gallery, Notes (both opt in to `enableOnBackInvokedCallback`), Jellyfin: androidx.activity's `BackEventCompat` reads it on `SDK_INT >= 36` from `onBackStarted`, which AOSP 14's `ViewRootImpl` sends for the Back key | Certain crash in the activities that opt in, when the user presses Back (Esc); to be confirmed by a run |
| 6 | `Activity.setTaskDescription` with a bitmap icon | AOSP's `Activity` calls `ActivityManager.getLauncherLargeIconSizeInner`, which Win.apk's `ActivityManager` lacks: `NoSuchMethodError` | Scales the icon and sets the task's | Gallery, Notes (Fossify Commons' recents icon, when the user picked another app icon colour) | Certain crash, with a bitmap icon |
| 7 | `View.startDragAndDrop` and a drop into a view | AOSP's `View`/`ViewRootImpl` call `ClipData.prepareToLeaveProcess(Z)`/`prepareToEnterProcess`, which Win.apk's `ClipData` lacks: `NoSuchMethodError` | Starts the drag; delivers the drop | PPSSPP (androidx.core's `ViewCompat.startDragAndDrop`); every app with draggable text (`TextView`, Compose) | Certain crash, when a drag starts |
| 8 | `Activity.createPendingResult` | Calls `new PendingIntent(IIntentSender)`, which Win.apk's `PendingIntent` lacks: `NoSuchMethodError` | A PendingIntent that delivers to `onActivityResult` | None of the 11 | Certain crash when called (unused here) |
| 9 | `getSystemService(TELECOM_SERVICE)` | `null`: no case in `Context.getSystemService` though `TelecomManager` is in the jar (`Context.java:1601`) | Never null on a phone | 6 name the class; Jitsi registers a `PhoneAccount` through it | Certain crash in Jitsi's call integration (an NPE); wrong answer elsewhere |
| 10 | `AccessibilityNodeInfo` level-36 members (`getChecked`, `getExpandedState`, `isFieldRequired`, `getSupplementalDescription`, `addLabeledBy`...) | Missing (AOSP 14's) | Present | Notes, Gallery, Jellyfin, Puzzles (androidx.core's `AccessibilityNodeInfoCompat` on `SDK_INT >= 36`) | Certain crash where AndroidX reads them (`isChecked()`, `toString()`), as a screen reader's tree or a log does |
| 11 | In-process services with no answer (`IPackageManager` 2 of 206 methods, `IActivityTaskManager` 3 of 95, `INotificationManager` 4 of 148, `IWindowManager` 16 of 144...) | The generated stub returns null/0/false and logs "no answer in this process, the empty one" | system_server answers | Every app: 8 such methods in every smoke run (below) | Wrong answer today; a certain crash for the AOSP code that dereferences a null one |
| 12 | `android.icu` (Win.apk's own small copy) | 671 members and 100 classes of level 24 missing, among them `HebrewCalendar`, `IslamicCalendar`, `UCharacter.UnicodeBlock`, `Normalizer`, `DateIntervalFormat`, `MessagePattern`; `DateFormat.setContext` a no-op | ICU4J in full | `DateFormat.setContext` in 4 (androidx.core); no app here uses the missing classes | Wrong answer here; a certain crash for a Hebrew-calendar app |
| 13 | `AudioManager` | A new object per `getSystemService` with its own volume array (7 of 15); not Windows' volume; `isMusicActive`, `isWiredHeadsetOn`... false | The device's audio state | All 11 | Wrong answer |
| 14 | `PowerManager` wake locks, `FLAG_KEEP_SCREEN_ON` | Counted, but nothing tells Windows (no `SetThreadExecutionState` in `windows/host`) | Keeps the screen on | Jellyfin, Jitsi, Gallery (video) | Wrong answer: the PC sleeps during a video or a call |
| 15 | `KeyguardManager.isDeviceSecure`, `BiometricManager.canAuthenticate` | `UserVerificationBridge.available()` is hard-coded false: never secure, no hardware | Windows Hello would answer | Gallery, Notes (their app lock) | Wrong answer: a lock that needs a secure device cannot be turned on |
| 16 | `MediaController.TransportControls` (Win.apk's) | Every method a no-op | Drives the session | Jellyfin | Wrong answer: media keys and a controller do nothing |
| 17 | `MediaCodec.detachOutputSurface` (35), `setOnFrameRenderedListener` | No-ops | Detach the surface; report rendered frames | Jerboa, Gallery, Jellyfin (media3 on 35+) | Wrong answer: video keeps drawing to the old surface |
| 18 | `ShortcutManager` | `setDynamicShortcuts` false, lists empty, pinning unsupported | Launcher shortcuts | Gallery, Notes, PPSSPP | Wrong answer (no Windows equivalent is offered) |
| 19 | `NsdManager` | Every discover/register/resolve fails | mDNS (Windows has DnsServiceBrowse) | Puzzles, Jellyfin, PPSSPP | Wrong answer: no server found on the LAN |
| 20 | `TelephonyManager.getNetworkOperatorName` | `"Mac.apk"` (`TelephonyManager.java:274`) | `""` with no SIM | Jitsi (react-native-device-info) | Wrong answer that leaks upstream's name |

Beyond these, the one systemic point: **Win.apk presents level 35 or 36 to apps targeting them, while
`android.app`, `android.view` and `android.content.res` are AOSP 14's.** 120 members of level 36 and 226
of 35 are missing from classes that exist (below), and AndroidX built against 35/36 calls them behind an
`SDK_INT` check that Win.apk's presented level passes. Rows 4, 5 and 10 are that case; every library
update adds more.

## 1. Missing classes

1071 public classes of the SDK have no class on the class path; 539 of them were added at 34, 35 or 36
(by level: 205, 150, 184). Those are listed by package in the lists file; `ApiGapScan.java`'s
`classes.tsv` has every level. By area:

| Area | Classes missing | Win.apk today | Android | Apps | Severity |
|---|---:|---|---|---|---|
| `android.health.connect` (34-36) | 208 | None | Health Connect | none | Unused |
| `android.app.appsearch` | 85 | None | AppSearch | none | Unused |
| `android.net.wifi` (aware, rtt, hotspot2, p2p UPnP, `LocalOnlyHotspot`) | 85 | None | Present (hardware-gated) | none | Unused |
| `android.icu.*` (level 24) | 138 | Win.apk's own small `android.icu`; `HebrewCalendar`, `IslamicCalendar`, `JapaneseCalendar`..., `UCharacter$UnicodeBlock`, `UScript`, `Normalizer`, `MessagePattern`, `DateIntervalFormat`, `StringSearch` missing | ICU4J | none here | Unused here; a certain crash for a calendar or text app that uses them |
| `android.adservices.*`, `android.app.sdksandbox` | 98 | None | Privacy Sandbox | none | Unused |
| `android.media.tv`, `android.media.quality`, `android.telephony.mbms`, `android.telephony.ims`, `android.net.ipsec`, `android.net.eap`, `android.net.vcn`, `android.net.sip`, `android.net.rtp`, `android.mtp` | 223 | None | Present (often hardware-gated) | none | Unused |
| `android.graphics.pdf` (35-36: `PdfRendererPreV`, the editing classes) | 19 | None (`PdfRenderer` itself is there, 12 members short) | Present | none | Unused |
| `android.ranging` (36) | 28 | None | Present | none | Unused |
| `android.app.assist.AssistStructure` and its nodes (23) | 3 | None, so AOSP's `AutofillManager`'s augmented paths and `ActivityThread`'s assist request do not link | Present | none | Unused |
| `android.telecom.Call`, `InCallService`, `CallScreeningService` | 13 | None | Present | none | Unused |
| `java.security.acl.*`, `java.util.jar.Pack200`, `java.lang.Compiler` (level 1) | 12 | Gone from JDK 21 | In Android's libcore | none | Unused; a certain crash for an old library that names them |
| `android.app.Notification$WearableExtender`, `$CarExtender`, `$Action$WearableExtender` | 6 | None | Present | none (AndroidX's compat classes write the extras themselves) | Unused |
| `android.app.MediaRouteButton`, `MediaRouteActionProvider` | 2 | None | Present | none | Unused |

No surveyed app names a missing SDK class from code that runs. Their failed class lookups are
libraries' optional paths: libGDX's `android.support.v4.app.Fragment` (Klooni, never used),
androidx.webkit's `WebViewFactory` reflection (Jellyfin, a fallback), React Native's install-referrer
client (Jitsi, logs "unavailable").

## 2. Missing methods and fields

### 2a. SDK members missing from classes that exist

2186 methods and fields of the SDK do not resolve (and 2473 compile-time constants, which javac copies
into the app, so only reflection misses them). Of levels 34-36: 186, 226 and 120. The largest groups:
`android.system.OsConstants` (374 non-constant fields of level 21: Android sets them at run time, so an
app's `getstatic` links to them) and `android.system.Os` (63 methods: `mmap`, `kill`, `isatty`, `bind`,
`getxattr`...), `android.icu` (above), `WifiManager` (59), `LinkProperties`, `NfcAdapter`,
`AccessibilityNodeInfo` (13, all 36), `Activity` (10, all 35-36: `ComponentCaller`, the
`requestPermissions(String[], int, int)` overloads), `Layout` (11).

What the surveyed apps call and do not find:

| Member | Level | Win.apk | Android | Apps (caller) | Severity |
|---|---:|---|---|---|---|
| `ActivityOptions.setAllowPassThroughOnTouchOutside(Z)` | 36 | Missing | Present | Jerboa (androidx.browser, unconditional on 36) | Certain crash, opening a link |
| `BackEvent.getFrameTimeMillis()` | 36 | Missing | Present | Gallery, Notes, Jellyfin (androidx.activity) | Certain crash on Back in opted-in activities |
| `AccessibilityNodeInfo.getChecked()`, `getExpandedState()`, `isFieldRequired()`, `getSupplementalDescription()` | 36 | Missing | Present | Gallery, Notes, Jellyfin, Puzzles (androidx.core) | Certain crash where AndroidX reads them; not on the first screen |
| `AccessibilityNodeInfo.addLabeledBy(View, int)` | 36 | Missing | Present | Puzzles (androidx.core) | Certain crash when called |
| `AccessibilityNodeInfo$AccessibilityAction.ACTION_SET_EXTENDED_SELECTION` | 36.1 | Missing | Present from 36.1 | Gallery | Unused (Win.apk never presents 36.1) |
| `Drawable.getOpticalInsets` (by reflection) | hidden | Present (AOSP's graphics) | Present | AppCompat | none (a false lead of the reflection heuristic) |

### 2b. The runtime's own links

AOSP's classes call members of Win.apk's classes of the same name that Win.apk's do not have.
`windows/scripts/LinkScan.java` over the shipped jars finds 682 such references (450 methods, 125
fields, 107 classes); most sit in code no app reaches (`ActivityThread`'s memory dumps, `UiAutomation`,
backup agents, `IPackageManager$Stub`'s parcel code, Win.apk's own `Macapk*` helpers inside classes AOSP
now shadows). The ones under public API an app calls:

| Public method (AOSP's) | What it calls that is missing | Apps | Severity |
|---|---|---|---|
| `Activity.startIntentSenderForResult`, `startIntentSenderForResultInner`; `ContextImpl.startIntentSender` | `IntentSender.getTarget()`, `getWhitelistToken()` | 9 | Certain crash |
| `ImageView.setImageURI` with `android.resource://` | `ContentResolver.getResourceId(Uri)`, `ContentResolver$OpenResourceIdResult` | 10 | Certain crash for that scheme |
| `Context.grantUriPermission`, `revokeUriPermission`, `checkUriPermission` (ContextImpl) | `ContentProvider.getUriWithoutUserId`, `getUserIdFromUri` | Gallery | Certain crash |
| `Activity.setTaskDescription` with a bitmap | `ActivityManager.getLauncherLargeIconSizeInner(Context)` | Gallery, Notes | Certain crash |
| `View.startDragAndDrop`; a drop (`ViewRootImpl`) | `ClipData.prepareToLeaveProcess(Z)`, `prepareToEnterProcess(AttributionSource)` | PPSSPP; text views | Certain crash |
| `Activity.createPendingResult` | `PendingIntent(IIntentSender)` | none | Certain crash when called |
| `Context.startService` refused in the background (ContextImpl) | `ServiceStartNotAllowedException.newInstance` | none seen | Certain crash instead of the exception |
| `SearchView` with a searchable's action keys, its suggestions | `SearchableInfo.findActionKey`, `SearchableInfo$ActionKeyInfo`, `SearchManager.getSuggestions`, `SearchableInfo.getProviderContext` | none (apps use AppCompat's `SearchView`) | Certain crash when used |
| `VideoView` with subtitles or metadata | `MediaPlayer.addSubtitleSource`, `getMediaTimeProvider`, `getMetadata(ZZ)`, `setSubtitleAnchor` | none | Certain crash when used |
| `RemoteViews` adapters, `AppWidgetHostView` | `Context.getServiceDispatcher`, `AppWidgetManager.bindRemoteViewsService`, `Context.createApplicationContext` | none | Unused (no widget host) |
| `ActivityChooserModel` | `ActivityManager.checkComponentPermission` | none | Certain crash when used |

### 2c. Present but not the same

- `JobService.onBind` is `final` on Android and absent from Win.apk's `JobService`, so the app's
  `JobService` inherits `Service.onBind`, abstract: an `AbstractMethodError` if anything binds it.
  Win.apk runs jobs by calling `onStartJob` directly (`ActiveServices.jobBind`), so not today.
- 29 `android.icu` methods (`Collator`, `DateFormat`, `NumberFormat`, `Normalizer2`) are abstract in
  Win.apk's classes and concrete in the SDK: a subclass written against the SDK fails with
  `AbstractMethodError`.
- 74 SDK supertypes are missing: `LinkProperties`, `NetworkCapabilities`, `IpPrefix`, `LinkAddress`,
  `MacAddress`, `CaptivePortal`... are not `Parcelable`, so putting one in a `Bundle` or `Intent` throws;
  `android.icu` classes are not `Freezable`/`Serializable`.
- `android.net.ParseException(String)` is not public, nor `URLDecoder()` and `Modifier()` (JDK 21's).

## 3. Methods that exist but answer nothing

2088 SDK methods resolve to a body that throws `UnsupportedOperationException`, says it is not
supported, returns a constant (null, 0, false, an empty collection) or does nothing, where AOSP 14's own
body does more (or AOSP 14 has none: a 35/36 member Win.apk answers with a constant). Not all are
gaps: a no-op `Trace.beginSection`, `StrictMode.setThreadPolicy` or `MotionEvent.recycle` is right for
Win.apk. The ones the surveyed apps call, judged:

| Method | Win.apk | Android | Apps | Severity |
|---|---|---|---|---|
| `TelecomManager.placeCall`, `registerPhoneAccount` | Throw `UnsupportedOperationException` (and `getSystemService` gives null first) | Place the call; register | Jitsi | Certain crash if reached |
| `MediaController.TransportControls.*` | No-ops | Drive the session | Jellyfin | Wrong answer |
| `MediaCodec.detachOutputSurface` (35), `setOnFrameRenderedListener`, `subscribeToVendorParameters` | No-ops | Work | Jerboa, Gallery, Jellyfin, Jitsi | Wrong answer |
| `AudioManager.getStreamMaxVolume` (15), `isStreamMute`, `isMusicActive`, `isSpeakerphoneOn`, `isWiredHeadsetOn`, `isBluetoothA2dpOn`, `getRingerMode`, `setSpeakerphoneOn`, `setMicrophoneMute`, `startBluetoothSco` | Constants and no-ops | The device's audio routing | Jitsi (WebRTC audio), Jellyfin, OpenSudoku | Wrong answer: a call's speaker and mute buttons do nothing |
| `AnimatorSet.reverse`, `setCurrentPlayTime`, `setTarget`; `ValueAnimator.setFrameDelay` (Win.apk's `android.animation`) | No-ops | Run | Jellyfin | Wrong answer: an animation that should run backwards does not move |
| `PowerManager.isPowerSaveMode`, `isInteractive`, `isDeviceIdleMode`, `getCurrentThermalStatus`, `isIgnoringBatteryOptimizations` | false / 0 | The device's state | All 11 | Wrong answer, mostly harmless |
| `ShortcutManager.*`, `AppWidgetManager.updateAppWidget` | false / empty / no-ops | Launcher shortcuts, widgets | Gallery, Notes, PPSSPP | Wrong answer |
| `RoleManager.isRoleAvailable`, `isRoleHeld`, `createRequestRoleIntent` | false / null | Roles | Gallery, Notes | Wrong answer |
| `MediaProjectionManager.createScreenCaptureIntent`, `getMediaProjection` | An empty Intent / null | Screen capture | Jitsi (screen sharing) | Wrong answer: screen sharing cannot start |
| `NoiseSuppressor.create`, `AcousticEchoCanceler.create` | null | The effect (null on a device without it) | Jitsi | Wrong answer (a device without them answers the same) |
| `UsbManager`, `UsbDeviceConnection.*` | No devices, every transfer fails | USB host | Tensy (SDL's HIDAPI) | Wrong answer: no USB gamepad through SDL's USB path |
| `ConsumerIrManager.*`, `FingerprintManager.*`, `Vibrator.vibrate` | No hardware | Hardware or nothing | PPSSPP, Gallery, Notes, Klooni, Tensy | Wrong answer that a phone without the hardware also gives |
| `MediaStore.Images/Video.Thumbnails.getThumbnail`, `ThumbnailUtils.createVideoThumbnail`, `ExifInterface.getThumbnail` | null | A thumbnail | Editor, Gallery, Jitsi | Wrong answer: blank thumbnails |
| `MediaBrowserService.onBind`, `notifyChildrenChanged` | null / no-op | Serves media browsers | Jellyfin | Wrong answer (no Android Auto, no Windows media browser) |
| `StatFs.getBlockSizeLong` | A fixed block size; the counts are real | The volume's | 4 | Wrong answer only in rounding |
| `MediaCodecInfo$CodecCapabilities.isFeatureSupported` | false for every feature | Per codec | media3, WebRTC | Wrong answer: no adaptive playback, no tunneling (deliberate for secure playback) |
| `Spatializer.*`, `PlaybackSession.report*`, `TrafficStats.*`, `Trace.*`, `StrictMode.*` | No-ops / false | Real | most | Right for Win.apk |

## 4. System services

Of the 103 public service names of levels 34-36, Win.apk answers 73 (7 through `AospViewHost`, the rest
in `Context.getSystemService`'s switch, `runtime/java/android/content/Context.java:1226-1603`) and returns
null for 30. The full table (every name, what it returns, where, which apps name it) is in the lists
file; `windows/scripts/api_gaps/services_map.py` makes it. In short:

| Name | Level | Win.apk | Android | Apps | Severity |
|---|---:|---|---|---|---|
| `telecom` | 21 | null (`TelecomManager` exists, no case reaches it) | Never null | 6 name it; Jitsi uses it | Certain crash in Jitsi's call integration |
| `carrier_config`, `netstats`, `crossprofileapps`, `euicc`, `vpn_management`, `domain_verification`, `bugreport`, `persistent_data_block` | 23-35 | null, though each has a class in the jar | Never null | none | Unused (Context.java's own comment records Aurora Store's NPE on `domain_verification`) |
| `people`, `overlay`, `statusbar`, `app_search`, `blob_store`, `connectivity_diagnostics`, `telephony_ims`, `media_communication`, `display_hash`, `healthconnect`, `contact_keys`, `tethering`, `app_function` | 30-36 | null, no class | Never null | none (string matches only) | Unused |
| `tv_input`, `wifiaware`, `wifirtt`, `tv_interactive_app`, `device_lock`, `media_quality`, `tv_ad`, `satellite` | 21-36 | null | null on a phone without the feature | — | As on a phone |
| `audio`, `power`, `keyguard`, `biometric`, `phone`, `activity`, `print`, `servicediscovery`, `shortcut`, `wallpaper`, `media_projection`, `sensor`, `device_policy` | 1-29 | Objects whose answers differ from a phone's (rows 13-20 above; `print` writes a PDF into the app's files with no dialog; `ActivityManager.clearApplicationUserData` returns true and deletes nothing; `getAppTasks` is empty; no sensors at all) | Real | 2-11 each | Wrong answer |
| `ContentCaptureManager`, `TranslationManager`, `UiTranslationManager` by class | 29-31 | `getSystemServiceName(Class)` maps them to the wrong name: null | The manager | none | Unused |

## 5. In-process services that answer empty

`com.winapk.aospapp.gen.*Impl` and `com.winapk.aospviews.gen.*Impl` are the AIDL stubs Win.apk answers
AOSP's own framework with; a method its handler class lacks returns its type's empty value and logs
`<Interface>.<method>: no answer in this process, the empty one` once.
`windows/scripts/api_gaps/UnansweredScan.java` lists them from the jars:

| Interface | Answered / methods | Hit in every smoke run |
|---|---:|---|
| `android.content.pm.IPackageManager` | 2 / 206 | — (Win.apk's own `PackageManager` answers apps; this serves AOSP's internals) |
| `android.app.IActivityTaskManager` | 3 / 95 | — |
| `android.app.IActivityManager` | 29 / 249 | `setRenderThread` |
| `android.app.IActivityClientController` | 21 / 66 | `setTaskDescription`, `reportSizeConfigurations`; once: `reportActivityFullyDrawn` |
| `android.app.INotificationManager` | 4 / 148 | — |
| `android.view.IWindowManager` | 16 / 144 | — |
| `android.view.IWindowSession` | 11 / 42 | `setOnBackInvokedCallbackInfo`; once: `updatePointerIcon`, `onRectangleOnScreenRequested` |
| `android.hardware.display.IDisplayManager` | 5 / 55 | `registerCallbackWithEventMask`, `getOverlaySupport` |
| `android.hardware.input.IInputManager` | 7 / 67 | — |
| `com.android.internal.view.IInputMethodManager` | 6 / 31 | `addClient`, `isImeTraceEnabled`; once: `getImeTrackerService` |
| `android.view.accessibility.IAccessibilityManager` | 10 / 40 | — |
| `com.android.internal.textservice.ITextServicesManager` | 6 / 6 | — |

Each run of the ten smoke apps logs the same eight; all are harmless as answered (a render thread's
priority, a task's colour, a display callback, `addClient`). What matters:

- **`IWindowSession.setOnBackInvokedCallbackInfo`** (every app): the system never learns which window
  has a back callback. On a phone that drives the predictive back animation and whether Back goes to the
  app; in Win.apk the Back key reaches `ViewRootImpl` directly, so today nothing is lost.
- **`IWindowSession.updatePointerIcon`**: the pointer's shape never changes over a link or a text field
  (an AOSP view asks for `TYPE_HAND`, `TYPE_TEXT`). Wrong answer on a desktop, where the pointer is the
  main input.
- **`IPackageManager` and `IActivityTaskManager`** answer almost nothing: any AOSP code path that asks
  them (rather than Win.apk's own `PackageManager`) gets null and may dereference it. None was hit in the
  smoke runs; the first screens never ask.

## 6. Native code of apps

The NDK's libraries as Win.apk answers them, from AOSP's own version scripts (`*.map.txt`,
android16-release) and the sources: `windows/scripts/api_gaps/ndk_gaps.py`. The full per-symbol tables are in
the lists file.

**How an import resolves.** Every NDK library a `.so` needs (libc, libandroid, libvulkan...) is one
"system" node (`windows/src/linker.cpp:35`, `:208`), so loading never fails; each symbol is then looked up
by name in one flat table (`windows/host/guest_libs.cpp:179`), whatever library or version it was asked
of. A miss binds a weak symbol to 0, a variable to a shared zeroed page, and a function to a stub that logs
its first call ("guest called a system function not implemented on Windows yet") and returns 0 in rax
and xmm0 (`windows/src/elf_loader.cpp:433`, `windows/host/guest_libs.cpp:70`,
`windows/src/abi_thunk.cpp:255`), writing no out-parameter and no errno. `dlsym` of a missing name
returns NULL, as on a phone.

| Library | Public symbols | Implemented | Stubbed | Missing | Notes |
|---|---:|---:|---:|---:|---|
| libc | 1443 | 848 | 55 | 540 | Of 34-37: 5, 15, 7 and 2 missing |
| libm | 286 | 133 | 0 | 153 | `fe*` (floating-point environment) missing |
| libandroid | 401 | 147 | 41 | 213 | Nothing of 34-36 (7, 22, 49: performance hints, input receivers, display LUTs) |
| libmediandk | 364 | 162 | 36 | 166 | `AMediaCodec_*` answers "no codec"; `AMediaMuxer_*` missing; 60 codec-capability functions of 36 |
| libaaudio | 78 | 36 | 20 | 22 | `AAudioStream_read` always 0 frames (no input) |
| libnativewindow | 31 | 6 | 9 | 16 | One `ANativeWindow` for every Surface (below); `ANativeWindow_getBuffersDefaultDataSpace` (34) a stub |
| libjnigraphics | 39 | 3 | 0 | 36 | Only `AndroidBitmap_getInfo`/`lockPixels`/`unlockPixels`; no `AImageDecoder` |
| libEGL, libGLESv1_CM | 73, 278 | 52, 184 | 2, 0 | 19, 94 | |
| libGLESv2, libGLESv3, libOpenSLES | 204, 418, 56 | all but 2 | 0, 2, 0 | 0 | Complete |
| libvulkan, libcamera2ndk, libbinder_ndk, libamidi, libsync, libOpenMAXAL, libneuralnetworks, libicu | 251, 72, 150, 14, 3, 44, 69, 306 | 0 | 0 | all | Nothing |

What the ten apps with native code import (Editor has none), judged:

| Symbol | Win.apk | Android | Apps | Severity |
|---|---|---|---|---|
| `ANativeWindow_fromSurface` (behind the no-op `ANativeWindow_release`) | One window for every Surface (`windows/host/native_window.cpp:35`, `:66-79`) | One per Surface | Gallery (android-gif-drawable's `GifTextureView`), Tensy | Wrong answer: two native surfaces draw into the same window |
| An ELF executable (`libpuzzlesgen.so`, run as a child process) | Not runnable: no ELF program runner, the exec family fails with ENOSYS | Runs it | Puzzles | Certain failure of what it is for (the symbol, `__libc_init`, is the least of it) |
| `tmpnam` | Missing: NULL | A temporary path | PPSSPP | Certain crash if reached (NULL used as a path); off its normal paths |
| `swprintf` | Missing: 0, nothing written | Formats | libc++ (`std::to_wstring`), PPSSPP, libjxl in Gallery | Wrong answer |
| `feclearexcept`, `fetestexcept`, `fesetround`, `fegetenv`, `fesetenv`, `fegetround` | Missing: 0 | The FP environment | React Native in Jitsi, OpenCV in OpenSudoku | Wrong answer (no FP exception is ever seen; a rounding mode never changes) |
| `pthread_setschedparam` | 0, nothing changed | EPERM for real-time priority | WebRTC in Jitsi, Tensy | Wrong answer: the audio thread thinks it got real-time priority |
| `chmod` | 0, also for a missing path | ENOENT | PPSSPP | Wrong answer |
| `syslog` | Drops the line | logcat | libc++abi (the reason of an abort), libjxl, WebRTC, PPSSPP | Wrong answer: the reason a native library aborted is lost from the log |
| `AMediaCodec_*`, `AMediaExtractor_*`, `AMediaMuxer_*` | "No codec", no tracks, missing | Work | OpenCV in OpenSudoku | Unused (OpenCV's video I/O is not on its camera path) |
| `fputwc`, `getwc`, `ungetwc` | Missing | Wide stdio | libc++ in Jitsi, OpenSudoku | Unused (no wide streams); `getwc`'s 0 would spin a read loop |
| `pthread_*_destroy`, `getuid` (10100), `getpagesize`, `__register_atfork` | Constants | As here | many | Right for Win.apk |

Not imported by these apps but notable: `regexec` always answers "no match", every Vulkan, camera2 NDK
and AImageDecoder call is missing, and nothing the NDK added at 34, 35 or 36 is implemented (libbinder_ndk's
41 `APersistableBundle_*` of 35, libnativewindow's 4 parcel functions of 34). A game or a camera app
built on those fails at its first call.

### Java natives bound to a zero stub

`windows/host/host_natives.cpp` binds every native method of its 51 host classes; one no provider table
implements gets a stub returning zero. The smoke log says "1064 implemented, 46 stubbed";
`windows/scripts/api_gaps/host_natives.py` names 16 of them from the sources (it matches by name and
signature across classes, so it undercounts):

| Native | Severity |
|---|---|
| `NativeMedia.nativeDemux*` (8: sample rate, channels, format, csd-0, current sample, time, advance, seek) | Unused (Win.apk's `MediaExtractor` is `HostMedia`'s demuxer) |
| `NativeGles.init`, `beginFrame`, `endFrame` | Unused (upstream's Metal-era path) |
| `com.google.firebase.app.internal.cpp.*` (4: the Firebase C++ SDK's callbacks) | Wrong answer for a game built with Firebase's C++ SDK: its callbacks never arrive |
| `NativeCocoa.debugQuitGesture` | Unused |

## Not covered, and how sure this is

- **A reference is not a call.** The severities of the top rows were checked by reading the call site
  in the app (rows 1, 4, 5, 6) or by AOSP's code path (rows 2, 3, 7); row 5 still wants a run to confirm
  that the opted-in activity registers a callback. The smoke run only reaches each app's first screen,
  which none of these sits on.
- **Bodies**: the stub test is a pattern (a constant, an empty body, an `UnsupportedOperationException`)
  compared with AOSP 14's body; a stub that does a little work (a hard-coded `"Mac.apk"`) passes as real
  unless it returns a literal. The services table was read by hand.
- **Reflection** is found by a heuristic: a class-name string and the next identifier in a method that
  calls `getMethod`/`getDeclaredField`. It finds the classes reliably and the members loosely.
- **Level 36.1 and 37** are not in the SDK jars used; Win.apk never presents them.
- **Native**: see section 6's own limits.

## Running it again

```
windows/scripts/api_gaps/run_scan.sh <work dir> <runtime-stage dir> <apk>...
python3 -I windows/scripts/api_gaps/services_map.py --sdk <android-34> --sdk <android-35> --sdk <android-36> --apks <dir>
python3 -I windows/scripts/api_gaps/ndk_gaps.py --so-root <dir of <package>/*.so> --cache <dir>
python3 -I windows/scripts/api_gaps/host_natives.py <repo> <runtime-stage>/lib
java -cp asm.jar windows/scripts/LinkScan.java <the stage's jars> ...
```

`run_scan.sh` fetches the SDK jars (dl.google.com), android-all 14 and ASM (Maven Central), runs
`ApiGapScan.java` (classes, members, stubs, and with `dex_refs.py`'s output every app reference that fails
or reaches a stub), `UnansweredScan.java` and `report.py`, which writes the tables of `api-gaps-lists.md`.
The APKs are F-Droid's, at the versions `windows/test/compat/apps.txt` pins.
