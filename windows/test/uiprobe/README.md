# UI probe: Win.apk's framework against Android's

Layouts are inflated, measured and laid out twice, by the same code
(`src/dev/winapk/probe/Probes.java`): once inside Win.apk, as a small APK, and
once on the real AOSP framework, under Robolectric with native graphics. Every
view's place comes out as a line of text, and `compare.py` prints the lines
that differ. Each one is a place where Win.apk's framework does not do what
Android does, found without opening an app.

```sh
windows/test/uiprobe/build.sh <out>                       # probe.apk + the classes
windows/test/uiprobe/run_oracle.sh <out> <robolectric>    # <out>/oracle.txt
windows/test/uiprobe/run_ours.sh <out> <winapk-runtime.exe> [--lib <dir>]   # <out>/ours.txt
python windows/test/uiprobe/compare.py <out>/oracle.txt <out>/ours.txt
```

- `<out>` must be a path without spaces or non-ASCII characters (aapt2 and
  Robolectric both fail on those).
- `<robolectric>` is a folder with Robolectric's jars:
  `mvn dependency:copy-dependencies -DoutputDirectory=<robolectric>` on a pom
  that depends on `org.robolectric:robolectric` and `junit:junit` and lists
  Google's Maven repository. The first run downloads the Android framework
  Robolectric runs (about 200 MB) into `<out>/home/.m2`.
- Needs an Android SDK (build-tools and a platform) and a JDK 17.

A new case is a layout under `res/layout/` plus its name in `Probes.LAYOUTS`;
each runs left to right and right to left. A widget or a LayoutParams class
of the probe's own (`ProbeLabel`, `ProbeLayout`) stands for what apps do that
the framework's own widgets do not: a default style, a constructor that reads
its attributes.

Numbers are in dp, and two lines count as equal within 1 dp: the fonts are
hinted a little differently and each side rounds a pixel its own way.
