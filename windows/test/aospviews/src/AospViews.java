// AospViews -- the feasibility spike for running AOSP's own resources and views in Win.apk's JVM
// (docs/research/aosp-views.md, windows/PORTING.md M11). On a plain HotSpot JDK 21, with every
// android.* class from android-all 14 (API 34) and no Robolectric sandbox, instrumentation or
// shadow dispatch:
//
//   1. android.content.res: AOSP's Resources, ResourcesImpl, AssetManager, ApkAssets, TypedArray,
//      Theme, XmlBlock, ColorStateList. Their libandroidfw natives were rewritten at build time
//      (tools/RewriteNatives.java) into calls to the bridge (src/org/robolectric/shadows/Bridge*),
//      which runs them on Robolectric's Java port of libandroidfw. The framework's resources come
//      from android-all's own resources.arsc, the app's from the APK.
//   2. The uiprobe app's two themes are resolved through AOSP's Theme and TypedArray, line for
//      line as windows/test/uiprobe's Probes.themeValues prints them, and compared with the oracle
//      (the same code on Robolectric) when one is given.
//   3. Graphics come up as in windows/test/aospgfx (redirect agent + native runtime + fonts), then
//      AOSP's LayoutInflater (PhoneLayoutInflater) inflates the probe's layouts into AOSP's
//      LinearLayout / TextView / FrameLayout..., which are measured and laid out at 360x640 dp
//      (density 2), dumped as Probes.dump does, compared with the oracle, and drawn into an AOSP
//      Bitmap written as a PNG.
//
//   AospViews <agent> <native runtime> <icudt68l.dat> <fonts dir> <android-all.jar> <probe.apk>
//             <out dir> [oracle.txt]
//
// Each check prints [PASS] / [FAIL]; "AOSPVIEWS OK" at the end when all passed.

import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.ApplicationInfo;
import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.ResourcesImpl;
import android.content.res.TypedArray;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Typeface;
import android.text.Layout;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import android.view.DisplayAdjustments;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.lang.management.ManagementFactory;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public final class AospViews {
    static final int W_DP = 360, H_DP = 640;
    static final float DENSITY = 2f;
    static int failures;
    static final List<String> NOT_IMPLEMENTED = new ArrayList<>();


    static void check(boolean ok, String what) {
        System.out.println((ok ? "[PASS] " : "[FAIL] ") + what);
        if (!ok) failures++;
    }

    static double ms(long t0) { return (System.nanoTime() - t0) / 1e6; }

    public static void main(String[] a) throws Exception {
        if (a.length < 7) {
            System.err.println("usage: AospViews <agent> <native runtime> <icu .dat> <fonts dir> <android-all.jar>"
                    + " <probe.apk> <out dir> [oracle.txt]");
            System.exit(2);
        }
        System.out.printf("[info] java %s (%s) on %s %s%n", System.getProperty("java.version"),
                System.getProperty("java.vm.name"), System.getProperty("os.name"), System.getProperty("os.arch"));
        File out = new File(a[6]);
        out.mkdirs();
        Map<String, List<String>> oracle = a.length > 7 ? readOracle(new File(a[7])) : null;
        System.setProperty("aospviews.framework", new File(a[4]).getAbsolutePath());
        // android.os.Build reads the system properties (here: java's, aospgfx's SystemProperties
        // stand-in); android-all carries the build.prop Robolectric loads the same way. Win.apk's
        // framework already answers these (Build.java, SystemProperties).
        try (java.io.InputStream in = AospViews.class.getClassLoader().getResourceAsStream("build.prop")) {
            java.util.Properties p = new java.util.Properties();
            p.load(in);
            for (String k : p.stringPropertyNames()) System.setProperty(k, p.getProperty(k));
        }
        System.setProperty("ro.product.cpu.abilist", "x86_64");
        System.setProperty("ro.product.cpu.abilist64", "x86_64");
        System.setProperty("ro.product.cpu.abilist32", "");

        // The redirect agent first: VMRuntime.addressOf (TypedArray's arrays) is its bufferAddress.
        // Resources need nothing else native; the native runtime comes in part 3.
        System.load(new File(a[0]).getAbsolutePath());

        // --- 1. Resources: AOSP's Java over the bridge, no AOSP native library.
        long t0 = System.nanoTime();
        int classes0 = loadedClasses();
        Resources res = createResources(new File(a[5]).getAbsolutePath());
        System.out.printf("[time] framework + app resources up in %.1f ms (%d classes loaded)%n", ms(t0),
                loadedClasses() - classes0);
        String pkg = "dev.winapk.probe";
        int probeTheme = res.getIdentifier("ProbeTheme", "style", pkg);
        int libTheme = res.getIdentifier("ProbeLibTheme", "style", pkg);
        check(probeTheme != 0 && libTheme != 0, String.format("the app's themes by name: ProbeTheme=%08x ProbeLibTheme=%08x",
                probeTheme, libTheme));
        check("dev.winapk.probe:style/ProbeTheme".equals(res.getResourceName(probeTheme)),
                "and back to a name: " + res.getResourceName(probeTheme));
        int sysColor = res.getIdentifier("colorAccent", "attr", "android");
        check(sysColor == android.R.attr.colorAccent, String.format("a framework attribute by name: %08x", sysColor));

        // --- 2. The two themes, as Probes.themeValues prints them.
        List<String> themeLines = new ArrayList<>();
        t0 = System.nanoTime();
        themeValues(res, pkg, probeTheme, libTheme, themeLines);
        System.out.printf("[time] theme values in %.1f ms%n", ms(t0));
        for (String l : themeLines) System.out.println("PROBE " + l);
        compare(oracle, themeLines, "theme");

        // --- 3. Graphics, then views (on the main looper's thread, as an app's views are).
        android.os.Looper.prepareMainLooper();
        if (!loadGraphics(a)) {
            finish();
            return;
        }
        List<String> layoutLines = new ArrayList<>();
        String[] names = {"linear_weights", "frame_gravity", "relative", "text_baselines", "edit_compound"};
        // (custom_params and text_gravity are left out: their ProbeLayout and ProbeLabel are the app's
        // own code, DEX in the APK, which Win.apk converts at install; this spike loads no app code.)
        int inflated = 0, pictures = 0, samePictures = 0;
        for (String name : names) {
            int id = res.getIdentifier(name, "layout", pkg);
            for (int dir = 0; dir < 2; dir++) {
                String cs = name + (dir == 0 ? ".ltr" : ".rtl");
                layoutLines.add("case " + cs);
                t0 = System.nanoTime();
                try {
                    SpikeContext c = new SpikeContext(res, probeTheme, pkg);
                    FrameLayout root = new FrameLayout(c);
                    root.setLayoutDirection(dir == 0 ? View.LAYOUT_DIRECTION_LTR : View.LAYOUT_DIRECTION_RTL);
                    LayoutInflater.from(c).inflate(id, root, true);
                    int w = Math.round(W_DP * DENSITY), h = Math.round(H_DP * DENSITY);
                    root.measure(View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY),
                            View.MeasureSpec.makeMeasureSpec(h, View.MeasureSpec.EXACTLY));
                    root.layout(0, 0, w, h);
                    try {
                        Bitmap picture = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                        Canvas canvas = new Canvas(picture);
                        canvas.drawColor(0xFFFFFFFF);
                        root.draw(canvas);
                        try (FileOutputStream f = new FileOutputStream(new File(out, cs + ".png"))) {
                            picture.compress(Bitmap.CompressFormat.PNG, 100, f);
                        }
                        // The oracle's picture of the same case (uiprobe/build.sh writes oracle-pictures/
                        // beside oracle.txt), decoded by AOSP's BitmapFactory: pixels that differ.
                        File want = a.length > 7 ? new File(new File(a[7]).getParentFile(), "oracle-pictures/" + cs + ".png") : null;
                        if (want != null && want.isFile()) {
                            Bitmap o = android.graphics.BitmapFactory.decodeFile(want.getPath());
                            int diff = 0;
                            if (o == null || o.getWidth() != w || o.getHeight() != h) {
                                diff = -1;
                            } else {
                                int[] x = new int[w * h], y = new int[w * h];
                                picture.getPixels(x, 0, w, 0, 0, w, h);
                                o.getPixels(y, 0, w, 0, 0, w, h);
                                for (int i = 0; i < x.length; i++) if (x[i] != y[i]) diff++;
                            }
                            pictures++;
                            if (diff == 0) samePictures++;
                            System.out.printf("[info] %s: %s%n", cs, diff < 0 ? "the oracle's picture has another size"
                                    : diff + " pixels differ from the oracle's picture");
                        }
                    } catch (Throwable t) {
                        layoutLines.add("  NOT DRAWN " + t);
                        t.printStackTrace(System.out);
                    }
                    dump(root.getChildAt(0), 0, c, layoutLines);
                    inflated++;
                    System.out.printf("[time] %s inflated, measured, laid out and drawn in %.1f ms%n", cs, ms(t0));
                } catch (Throwable t) {
                    layoutLines.add("  FAILED " + t);
                    System.out.println("[info] " + cs + " failed:");
                    t.printStackTrace(System.out);
                }
            }
        }
        for (String l : layoutLines) System.out.println("PROBE " + l);
        check(inflated > 0, "layouts inflated by AOSP's LayoutInflater into AOSP's widgets: " + inflated + " of "
                + names.length * 2);
        compare(oracle, layoutLines, "layout");
        if (pictures > 0) check(samePictures == pictures, "pictures equal pixel for pixel to the oracle's: " + samePictures + " of " + pictures);
        finish();
    }

    static void finish() {
        System.out.printf("[info] classes loaded: %d; heap used %d MB; RSS %d KB%n", loadedClasses(),
                (Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory()) >> 20, rssKb());
        if (!NOT_IMPLEMENTED.isEmpty()) System.out.println("[info] Context.getSystemService asked for and not given: " + NOT_IMPLEMENTED);
        System.out.println("[info] Binder service calls (in-process stand-ins, empty answers): " + aospviews.sys.Services.calls());
        if (!aospviews.sys.Services.unknown().isEmpty()) {
            System.out.println("[info] services asked of ServiceManager with no interface known: " + aospviews.sys.Services.unknown());
        }
        System.out.println(failures == 0 ? "AOSPVIEWS OK" : "AOSPVIEWS FAILED: " + failures + " check(s)");
        System.out.flush();
        System.exit(failures == 0 ? 0 : 1);
    }

    // ---------------------------------------------------------------- resources

    /** What ResourcesManager does for an app, without the manager: the app's ApkAssets after the
     *  framework's (AssetManager.Builder adds the system ones), one configuration, one display. */
    static Resources createResources(String apk) throws Exception {
        installSystemAssets();
        ApkAssets app = ApkAssets.loadFromPath(apk);
        AssetManager am = new AssetManager.Builder().addApkAssets(app).build();
        DisplayMetrics dm = new DisplayMetrics();
        dm.setToDefaults();
        dm.density = DENSITY;
        dm.scaledDensity = DENSITY;
        dm.densityDpi = Math.round(DENSITY * DisplayMetrics.DENSITY_DEFAULT);
        dm.xdpi = dm.ydpi = dm.densityDpi;
        dm.widthPixels = Math.round(W_DP * DENSITY);
        dm.heightPixels = Math.round(H_DP * DENSITY);
        Configuration cfg = new Configuration();
        cfg.setToDefaults();
        cfg.densityDpi = dm.densityDpi;
        cfg.screenWidthDp = W_DP;
        cfg.screenHeightDp = H_DP;
        cfg.smallestScreenWidthDp = W_DP;
        cfg.orientation = Configuration.ORIENTATION_PORTRAIT;
        cfg.screenLayout = Configuration.SCREENLAYOUT_SIZE_NORMAL | Configuration.SCREENLAYOUT_LONG_NO;
        cfg.uiMode = Configuration.UI_MODE_TYPE_NORMAL | Configuration.UI_MODE_NIGHT_NO;
        cfg.touchscreen = Configuration.TOUCHSCREEN_FINGER;
        cfg.setLocale(Locale.US);
        ResourcesImpl impl = new ResourcesImpl(am, dm, cfg, new DisplayAdjustments());
        Resources r = new Resources(AospViews.class.getClassLoader());
        r.setImpl(impl);
        return r;
    }

    /** AssetManager.createSystemAssetsInZygoteLocked without its overlays: the zygote's step asks
     *  OverlayConfig for the device's static RROs, which reads the partitions through
     *  android.os.Environment and libcore's Os (getuid...). Win.apk has no overlays; its system
     *  side would do just this: the framework's ApkAssets, marked system, in AssetManager.sSystem. */
    static void installSystemAssets() throws Exception {
        long t0 = System.nanoTime();
        ApkAssets fw = ApkAssets.loadFromPath("/system/framework/framework-res.apk", ApkAssets.PROPERTY_SYSTEM);
        System.out.printf("[time] the framework's ApkAssets (android-all's 16 MB resources.arsc) loaded in %.1f ms%n", ms(t0));
        ApkAssets[] arr = {fw};
        java.lang.reflect.Field f = AssetManager.class.getDeclaredField("sSystemApkAssetsSet");
        f.setAccessible(true);
        f.set(null, new android.util.ArraySet<>(java.util.Arrays.asList(arr)));
        f = AssetManager.class.getDeclaredField("sSystemApkAssets");
        f.setAccessible(true);
        f.set(null, arr);
        java.lang.reflect.Constructor<AssetManager> k = AssetManager.class.getDeclaredConstructor(boolean.class);
        k.setAccessible(true);
        AssetManager sys = k.newInstance(true);
        sys.setApkAssets(arr, false);
        f = AssetManager.class.getDeclaredField("sSystem");
        f.setAccessible(true);
        f.set(null, sys);
    }

    /** Probes.themeValues on a bare Resources.Theme: resolveAttribute, a styled attribute array,
     *  a colour state list; then the libraries' colour state lists for the second theme (through
     *  Resources.getColorStateList: AOSP's own ComplexColor inflation, not AppCompatResources). */
    static void themeValues(Resources res, String pkg, int probeTheme, int libTheme, List<String> out) {
        String[] names = {"colorPrimary", "colorSecondary", "colorAccent", "colorControlNormal",
                          "colorControlActivated", "colorControlHighlight", "colorSurface", "colorOnSurface",
                          "android:textColorPrimary", "android:colorBackground", "android:colorAccent"};
        for (int theme = 0; theme < 2; theme++) {
            out.add("case theme." + (theme == 0 ? "platform" : "lib"));
            Resources.Theme t = res.newTheme();
            t.applyStyle(theme == 0 ? probeTheme : libTheme, true);
            for (String name : names) {
                StringBuilder sb = new StringBuilder("  ").append(name);
                try {
                    int attr = name.startsWith("android:")
                            ? res.getIdentifier(name.substring(8), "attr", "android")
                            : res.getIdentifier(name, "attr", pkg);
                    if (attr == 0) {
                        out.add(sb.append(" no such attribute").toString());
                        continue;
                    }
                    TypedValue tv = new TypedValue();
                    boolean found = t.resolveAttribute(attr, tv, true);
                    sb.append(" resolve=").append(found);
                    if (found) {
                        sb.append(" type=").append(tv.type);
                        if (tv.type >= TypedValue.TYPE_FIRST_COLOR_INT && tv.type <= TypedValue.TYPE_LAST_COLOR_INT) {
                            sb.append(" colour=").append(hex(tv.data));
                        }
                    }
                    TypedArray ta = t.obtainStyledAttributes(new int[] {attr});
                    sb.append(" styled=").append(hex(ta.getColor(0, 0x12345678)));
                    ColorStateList list = ta.getColorStateList(0);
                    sb.append(" list=").append(list == null ? "null" : hex(list.getDefaultColor()));
                    ta.recycle();
                } catch (Throwable e) {
                    sb.append(" threw ").append(e.getClass().getSimpleName());
                    e.printStackTrace(System.out);
                }
                out.add(sb.toString());
            }
            if (theme == 1) {
                String[] lists = {"mtrl_btn_text_color_selector", "mtrl_tabs_icon_color_selector",
                                  "mtrl_navigation_bar_item_tint", "mtrl_chip_background_color",
                                  "mtrl_text_btn_text_color_selector", "abc_tint_switch_track", "abc_tint_btn_checkable"};
                int[] checked = {android.R.attr.state_enabled, android.R.attr.state_checked};
                int[] enabled = {android.R.attr.state_enabled};
                for (String n : lists) {
                    StringBuilder sb = new StringBuilder("  ").append(n);
                    try {
                        ColorStateList l = res.getColorStateList(res.getIdentifier(n, "color", pkg), t);
                        sb.append(" default=").append(hex(l.getDefaultColor()))
                          .append(" enabled=").append(hex(l.getColorForState(enabled, 0)))
                          .append(" checked=").append(hex(l.getColorForState(checked, 0)));
                    } catch (Throwable e) {
                        sb.append(" threw ").append(e.getClass().getSimpleName());
                    }
                    out.add(sb.toString());
                }
            }
        }
    }

    // ---------------------------------------------------------------- graphics

    static boolean loadGraphics(String[] a) {
        try {
            long t0 = System.nanoTime();
            System.setProperty("icu.data.path", new File(a[2]).getAbsolutePath());
            System.setProperty("icu.locale.default", "en-US");
            System.load(new File(a[1]).getAbsolutePath());
            int[] c = AospGfx.agentCounts();
            System.out.printf("[time] native runtime loaded in %.1f ms; natives bound %d (+%d adapted), skipped %d%n",
                    ms(t0), c[1], c[2], c[3]);
            check(AospGfx.agentRestore(), "graphics natives bound, the JNI function table restored");
            t0 = System.nanoTime();
            int n = setUpFonts(new File(a[3]));
            System.out.printf("[time] system fonts set up in %.1f ms%n", ms(t0));
            check(n > 10, "system typefaces from fonts.xml: " + n);
            return true;
        } catch (Throwable t) {
            check(false, "graphics: " + t);
            t.printStackTrace(System.out);
            return false;
        }
    }

    // As windows/test/aospgfx: SystemFonts from the native runtime's fonts.xml.
    static int setUpFonts(File dir) throws Exception {
        String d = dir.getAbsolutePath().replace('\\', '/') + "/";
        java.lang.reflect.Method m = android.graphics.fonts.SystemFonts.class.getDeclaredMethod(
                "getSystemFontConfigInternal", String.class, String.class, String.class, String.class, Map.class,
                long.class, int.class);
        m.setAccessible(true);
        android.text.FontConfig cfg = (android.text.FontConfig) m.invoke(null, d + "fonts.xml", d,
                "/nonexistent.xml", "/nonexistent/", new HashMap<>(), 0L, 0);
        Map<String, android.graphics.fonts.FontFamily[]> fallback = android.graphics.fonts.SystemFonts.buildSystemFallback(cfg);
        Map<String, Typeface> map = android.graphics.fonts.SystemFonts.buildSystemTypefaces(cfg, fallback);
        Typeface.setSystemFontMap(map);
        return map.size();
    }

    // ---------------------------------------------------------------- views

    /**
     * The least Context AOSP's views need, found by running them: everything not overridden here
     * goes to a null base and throws, so a new need shows up by name. In Win.apk this is the
     * app's real ContextImpl (Win.apk's own today), whose getSystemService answers from Win.apk's
     * services.
     */
    static final class SpikeContext extends ContextWrapper {
        final Resources res;
        final Resources.Theme theme;
        final String pkg;
        final ApplicationInfo ai = new ApplicationInfo();
        LayoutInflater inflater;
        android.content.ContentResolver resolver;

        SpikeContext(Resources res, int themeId, String pkg) {
            super(null);
            this.res = res;
            this.pkg = pkg;
            theme = res.newTheme();
            theme.applyStyle(themeId, true);
            ai.packageName = pkg;
            ai.targetSdkVersion = 34;
            ai.minSdkVersion = 26;
            ai.flags = ApplicationInfo.FLAG_SUPPORTS_RTL;
        }

        @Override public Resources getResources() { return res; }
        @Override public Resources.Theme getTheme() { return theme; }
        @Override public ApplicationInfo getApplicationInfo() { return ai; }
        @Override public String getPackageName() { return pkg; }
        @Override public String getOpPackageName() { return pkg; }
        @Override public String getBasePackageName() { return pkg; }
        @Override public ClassLoader getClassLoader() { return AospViews.class.getClassLoader(); }
        @Override public Context getApplicationContext() { return this; }
        @Override public boolean isUiContext() { return true; }
        @Override public int getUserId() { return 0; }
        @Override public String getAttributionTag() { return null; }
        @Override public android.view.Display getDisplayNoVerify() { return null; }
        @Override public int getDisplayId() { return 0; }
        @Override public android.os.Looper getMainLooper() { return android.os.Looper.getMainLooper(); }
        @Override public int checkCallingOrSelfPermission(String p) { return android.content.pm.PackageManager.PERMISSION_DENIED; }
        @Override public int checkPermission(String p, int pid, int uid) { return android.content.pm.PackageManager.PERMISSION_DENIED; }
        @Override public int checkSelfPermission(String p) { return android.content.pm.PackageManager.PERMISSION_DENIED; }
        @Override public boolean isRestricted() { return false; }
        @Override public android.content.res.AssetManager getAssets() { return res.getAssets(); }
        @Override public android.content.ContentResolver getContentResolver() {
            if (resolver == null) resolver = new SpikeResolver(this);
            return resolver;
        }
        @Override public android.view.autofill.AutofillManager.AutofillClient getAutofillClient() { return null; }
        @Override public android.content.AutofillOptions getAutofillOptions() { return null; }
        @Override public android.content.ContentCaptureOptions getContentCaptureOptions() { return null; }
        @Override public boolean canLoadUnsafeResources() { return true; }
        @Override public android.content.AttributionSource getAttributionSource() {
            return new android.content.AttributionSource.Builder(aospviews.sys.BridgeLinux.UID).setPackageName(pkg).build();
        }
        @Override public android.content.pm.PackageManager getPackageManager() { return null; }
        @Override public java.util.concurrent.Executor getMainExecutor() { return Runnable::run; }
        @Override public void registerComponentCallbacks(android.content.ComponentCallbacks c) {}
        @Override public void unregisterComponentCallbacks(android.content.ComponentCallbacks c) {}
        @Override public int getAssociatedDisplayId() { return 0; }
        @Override public android.os.UserHandle getUser() { return android.os.UserHandle.of(0); }
        @Override public android.os.IBinder getWindowContextToken() { return null; }
        @Override public boolean isDeviceProtectedStorage() { return false; }
        @Override public boolean isCredentialProtectedStorage() { return false; }

        @Override public Object getSystemService(String name) {
            if (Context.LAYOUT_INFLATER_SERVICE.equals(name)) {
                if (inflater == null) inflater = new com.android.internal.policy.PhoneLayoutInflater(this);
                return inflater;
            }
            if (!NOT_IMPLEMENTED.contains(name)) NOT_IMPLEMENTED.add(name);
            return null;
        }

        @Override public String getSystemServiceName(Class<?> c) {
            if (c == LayoutInflater.class) return Context.LAYOUT_INFLATER_SERVICE;
            if (c == android.view.WindowManager.class) return Context.WINDOW_SERVICE;
            if (c == android.view.inputmethod.InputMethodManager.class) return Context.INPUT_METHOD_SERVICE;
            if (c == android.view.accessibility.AccessibilityManager.class) return Context.ACCESSIBILITY_SERVICE;
            if (c == android.view.autofill.AutofillManager.class) return Context.AUTOFILL_MANAGER_SERVICE;
            if (c == android.view.textclassifier.TextClassificationManager.class) return Context.TEXT_CLASSIFICATION_SERVICE;
            if (!NOT_IMPLEMENTED.contains(c.getName())) NOT_IMPLEMENTED.add(c.getName());
            return null;
        }
    }

    /** Every provider (Settings' among them: TextView asks Settings.Global for a feature flag) is an
     *  empty stand-in. In Win.apk: its own ContentResolver and settings. */
    static final class SpikeResolver extends android.content.ContentResolver {
        SpikeResolver(Context c) { super(c); }
        @Override protected android.content.IContentProvider acquireProvider(Context c, String name) {
            return aospviews.sys.Services.stub(android.content.IContentProvider.class, "provider:" + name);
        }
        @Override protected android.content.IContentProvider acquireUnstableProvider(Context c, String name) {
            return acquireProvider(c, name);
        }
        @Override public boolean releaseProvider(android.content.IContentProvider p) { return true; }
        @Override public boolean releaseUnstableProvider(android.content.IContentProvider p) { return true; }
        @Override public void unstableProviderDied(android.content.IContentProvider p) {}
    }

    /** Probes.dump's line, without the drawn colours (px=) and the picture. */
    static void dump(View v, int depth, Context c, List<String> out) {
        float d = DENSITY;
        StringBuilder sb = new StringBuilder("  ");
        for (int i = 0; i < depth; i++) sb.append("  ");
        sb.append(v.getClass().getSimpleName());
        if (v.getId() != View.NO_ID) {
            try { sb.append('#').append(c.getResources().getResourceEntryName(v.getId())); }
            catch (Throwable t) { sb.append("#?"); }
        }
        sb.append(" at ").append(dp(v.getLeft(), d)).append(',').append(dp(v.getTop(), d))
          .append(" size ").append(dp(v.getWidth(), d)).append('x').append(dp(v.getHeight(), d));
        if (v.getVisibility() != View.VISIBLE) sb.append(" vis=").append(v.getVisibility());
        sb.append(" dir=").append(v.getLayoutDirection());
        sb.append(" pad=").append(dp(v.getPaddingLeft(), d)).append(',').append(dp(v.getPaddingTop(), d))
          .append(',').append(dp(v.getPaddingRight(), d)).append(',').append(dp(v.getPaddingBottom(), d));
        if (v.getLayoutParams() instanceof ViewGroup.MarginLayoutParams) {
            ViewGroup.MarginLayoutParams m = (ViewGroup.MarginLayoutParams) v.getLayoutParams();
            sb.append(" margins=").append(dp(m.leftMargin, d)).append(',').append(dp(m.topMargin, d))
              .append(',').append(dp(m.rightMargin, d)).append(',').append(dp(m.bottomMargin, d))
              .append(" lpdir=").append(m.getLayoutDirection());
        }
        if (v instanceof TextView) {
            TextView tv = (TextView) v;
            try {
                sb.append(" baseline=").append(dp(tv.getBaseline(), d));
                Layout l = tv.getLayout();
                if (l != null) {
                    sb.append(" lines=").append(l.getLineCount());
                    int last = l.getLineCount() - 1;
                    sb.append(" texth=").append(dp(l.getHeight(), d))
                      .append(" lastw=").append(dp(l.getLineWidth(last), d))
                      .append(" ell=").append(l.getEllipsisCount(last));
                    sb.append(" text=").append(dp(l.getLineLeft(0) + tv.getTotalPaddingLeft(), d))
                      .append("..").append(dp(l.getLineRight(0) + tv.getTotalPaddingLeft(), d))
                      .append(" top=").append(dp(l.getLineTop(0) + tv.getTotalPaddingTop(), d));
                } else {
                    sb.append(" layout=null");
                }
            } catch (Throwable t) {
                sb.append(" text?").append(t.getClass().getSimpleName());
            }
        }
        if (v instanceof android.widget.Checkable) sb.append(" checked=").append(((android.widget.Checkable) v).isChecked());
        if (v instanceof TextView && ((TextView) v).getTransformationMethod()
                instanceof android.text.method.PasswordTransformationMethod) {
            sb.append(" password");
        }
        if (v instanceof TextView) sb.append(" gravity=").append(Integer.toHexString(((TextView) v).getGravity()));
        out.add(sb.toString());
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) dump(g.getChildAt(i), depth + 1, c, out);
        }
    }

    // ---------------------------------------------------------------- comparing with the oracle

    static Map<String, List<String>> readOracle(File f) throws Exception {
        Map<String, List<String>> m = new LinkedHashMap<>();
        List<String> cur = null;
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            for (String l; (l = r.readLine()) != null; ) {
                if (l.startsWith("case ")) m.put(l.substring(5), cur = new ArrayList<>());
                else if (cur != null) cur.add(l);
            }
        }
        return m;
    }

    /** Fields of a dump line that this spike prints; the oracle's drawn-colour fields go. */
    static String comparable(String l) {
        return l.replaceAll(" (px|bg|tint|bgtint|clip|scale|shift|pic)=\\S+", "").trim();
    }

    /** Lines equal (within 0.5 dp per number, as the probe's own 1 dp rule, halved) to the oracle's. */
    static void compare(Map<String, List<String>> oracle, List<String> ours, String what) {
        if (oracle == null) {
            System.out.println("[info] no oracle given: " + what + " lines printed, not compared");
            return;
        }
        int same = 0, total = 0;
        String cs = null;
        int idx = 0;
        List<String> failed = new ArrayList<>();
        for (String l : ours) {
            if (l.startsWith("case ")) {
                cs = l.substring(5);
                idx = 0;
                continue;
            }
            if (l.startsWith("  FAILED ")) {
                failed.add(cs + ": " + l.substring(9));
                continue;
            }
            List<String> o = oracle.get(cs);
            String want = o != null && idx < o.size() ? comparable(o.get(idx)) : null;
            idx++;
            total++;
            String got = comparable(l);
            if (want != null && near(want, got)) {
                same++;
            } else {
                System.out.println("DIFF " + cs + "\n  android: " + want + "\n  aosp-on-hotspot: " + got);
            }
        }
        check(same == total, what + " lines equal to the oracle (Robolectric, same code): " + same + " of " + total
                + (failed.isEmpty() ? "" : " (cases that did not inflate: " + failed.size() + ", below)"));
        for (String f : failed) System.out.println("[info] not inflated: " + f);
    }

    static boolean near(String a, String b) {
        String[] x = a.split("(?<=[ ,=x])|(?=[ ,x])"), y = b.split("(?<=[ ,=x])|(?=[ ,x])");
        if (x.length != y.length) return a.equals(b);
        for (int i = 0; i < x.length; i++) {
            if (x[i].equals(y[i])) continue;
            try {
                if (Math.abs(Double.parseDouble(x[i]) - Double.parseDouble(y[i])) <= 0.5) continue;
            } catch (NumberFormatException e) { /* not a number */ }
            return false;
        }
        return true;
    }

    // ---------------------------------------------------------------- helpers

    static String hex(int colour) { return String.format("%08x", colour); }

    static String dp(float px, float density) { return String.valueOf(Math.round(px / density * 10) / 10f); }

    static int loadedClasses() { return ManagementFactory.getClassLoadingMXBean().getLoadedClassCount(); }

    static long rssKb() {
        try (BufferedReader r = new BufferedReader(new FileReader("/proc/self/status"))) {
            for (String l; (l = r.readLine()) != null; ) {
                if (l.startsWith("VmRSS:")) return Long.parseLong(l.replaceAll("\\D", ""));
            }
        } catch (Exception e) { /* not Linux */ }
        return -1;
    }
}
