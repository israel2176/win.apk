# AOSP resources and views on a plain JVM: the feasibility spike

The proof for [the plan](../../../docs/research/aosp-views.md) to replace
Win.apk's own `android.content.res`, `android.view`, `android.widget` and
`android.text` with AOSP's. Like `../aospgfx/` for graphics, it runs AOSP 14's
classes (Robolectric's `android-all-14`, API 34) in a plain HotSpot JDK 21, with
no Robolectric sandbox, instrumentation or shadow dispatch at run time, and
compares what they do with `../uiprobe`'s oracle.

```sh
windows/test/aospviews/fetch.sh <deps>                    # android-all, nativeruntime, jars/ (~330 MB)
windows/test/aospviews/run.sh <deps> <probe.apk> <out> [<oracle dir>/oracle.txt]
```

`probe.apk`, `oracle.txt` and `oracle-pictures/` come from
`../uiprobe/build.sh <out>`. Linux only so far; about a minute, 512 MB heap.

## How it runs AOSP's classes

- **Natives get Java bodies at build time.** `tools/RewriteNatives.java` (ASM)
  turns each native method of a named class into a call to a static method of
  the same name and descriptor on `Bridge<Class>`. A native the bridge lacks
  throws `UnsupportedOperationException` naming it. `rewrite.log` lists each.
  - `android.content.res.AssetManager`, `ApkAssets`, `XmlBlock` and
    `StringBlock` (94 natives, libandroidfw's on Android) go to
    `src/org/robolectric/shadows/Bridge*`. Those run them on Robolectric's
    line-by-line Java port of libandroidfw (`org.robolectric.res.android` in
    `resources-4.16.jar`), mostly through Robolectric's own shadow statics.
    `nativeSetApkAssets` and `nativeApplyStyle` are written out against AOSP's
    fields, because the shadow reaches those through instrumentation.
    Robolectric's native runtime does not carry libandroidfw (measured: no
    `AssetManager2` or `ApkAssets` symbol, no `register_android_content_*`).
  - The system layer goes to `src/aospviews/sys/`: `SystemClock`, `OsConstants`,
    `MessageQueue`, `Binder`, `BinderInternal`, `EventLog`, and 8 of
    `libcore.io.Linux`'s 134 natives. Win.apk keeps its own classes for these.
    `Services` is the in-process service manager. Each AIDL service is a
    `java.lang.reflect.Proxy` that records each call and gives the type's empty
    answer.
- **ART-only `java.*` members.** `tools/RemapJdk.java` scans every class of
  `android.*` and `com.android.internal.*`. It lists each reference to a
  `java.*` member that JDK 21 lacks (21 members; `remap.log`) and sends the
  ones with a body (`src/aospviews/sys/Art.java`: `System.logW`) there.
- **What ART and the system provide.**
  - The stand-ins: `VMRuntime` (direct-buffer addresses as in aospgfx, plus
    the way back from an address to its array for `TypedArray`),
    `NativeAllocationRegistry` (a free function of 0 is skipped),
    aospgfx's `Log`, `Trace` and `SystemProperties`, and android-all's
    `build.prop` as system properties.
  - `AssetManager.sSystem` is set up without overlays.
  - `SpikeContext` is a `ContextWrapper` with no base, so every method it
    needs shows up by name. About 35 are overridden.
- **Graphics** are aospgfx's: the redirect agent, the native runtime and the
  system fonts from its `fonts.xml`.

## What it checks

1. The framework's and the app's resources come up. The app's themes resolve by
   name and back.
2. `theme.platform` and `theme.lib`, as `Probes.themeValues` prints them:
   `resolveAttribute`, `obtainStyledAttributes`, a colour state list, and the
   libraries' colour state lists through `Resources.getColorStateList`.
   They are compared line for line with the oracle.
3. The probe's framework-only layouts. AOSP's `PhoneLayoutInflater` inflates
   them into a `FrameLayout`, left to right and right to left. They are
   measured and laid out at 360x640 dp (density 2) and dumped as
   `Probes.dump` does, without the drawn-colour fields. The dump is compared
   with the oracle within 0.5 dp. Each layout is also drawn by AOSP's software
   `Canvas` into a `Bitmap`, and the picture is compared pixel for pixel with
   the oracle's.

`custom_params` and `text_gravity` are left out because they need the app's
own classes (DEX). `edit_compound` is tried and does not inflate: `EditText`'s
`Editor` needs a `PackageManager`.

Result on the 4-vCPU Linux VM, JDK 21.0.13 (see the plan for the analysis):

| | AOSP on HotSpot (this) | Win.apk today (`ours.txt`, same probe) |
|---|---|---|
| Theme lines equal to the oracle | 29 of 29 | 19 of 29 |
| Layout lines (8 cases) | 44 of 44 exactly | 30 exactly, 12 within 0.5 dp, 2 off by more than 1 dp |
| Pictures (8 cases) | 8 of 8 pixel-identical | n/a |
| Layouts not inflated | `edit_compound` (needs `PackageManager`) | |

The oracle is the same android-all on Robolectric's same libandroidfw port.
Matching it shows that a plain JVM, Win.apk's kind of process, reproduces
Robolectric exactly. It does not show that Robolectric matches a device.
`../uiprobe/README.md` lists what the oracle cannot tell.
