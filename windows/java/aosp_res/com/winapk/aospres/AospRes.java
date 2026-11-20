// AospRes -- ResBackend on AOSP 14's own android.content.res (WINAPK_AOSP_RES=1; docs/research/
// aosp-views.md, step R1).
//
// With the switch on, lib/aosp-resources.jar is first on the class path: Resources, ResourcesImpl,
// AssetManager over ApkAssets, TypedArray, Theme, XmlBlock and ColorStateList / ComplexColor
// inflation are AOSP's, their natives run on Robolectric's Java port of libandroidfw
// (Native*.java), and the framework's own resources are android-all 14's table
// (bin/aosp/framework-res-34.apk, -Dwinapk.aosp.res.framework). Win.apk's framework keeps
// calling Resources, Theme and TypedArray through their public API; what it asks beyond that goes
// through ResBackend, answered here:
//
//   * the loader's AssetManager (openAssets: the framework's ApkAssets, then the app's APK and its
//     splits) and the system Resources' configuration;
//   * theme colours and values for Win.apk's chrome, by attribute name, on an AOSP Theme with the
//     application theme applied;
//   * TypedArray records, framework drawables, tints and densities, on AOSP's classes.
//
// Win.apk's own name-based resource index (com.macapk.loader.ResourceIndex over ArscReader) is
// still opened and answers the index lookups (index*): the inflater and the views still read
// style items and raw values by name from it. That is the part of today's resources the views
// layer (V1) retires; here it stays, so Win.apk's views keep working on AOSP's Resources.
package com.winapk.aospres;

import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.util.DisplayMetrics;
import android.util.TypedValue;

import com.macapk.loader.InstalledSplits;
import com.macapk.loader.ResourceIndex;
import com.macapk.loader.res.ResBackend;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.function.Consumer;

public final class AospRes implements ResBackend {
    public static final AospRes INSTANCE = new AospRes();

    private AospRes() {}

    static void log(String msg) { System.err.println("[aosp-res] " + msg); }

    private static volatile Object index;            // the ResourceIndex Boot publishes
    private static volatile String packageName = "";
    private static volatile AssetManager appAssets;  // what openAssets made for the app
    private static volatile List<ApkAssets> appApkAssets = new ArrayList<>(); // the app's APKs, loaded once
    private static volatile Configuration appConfig; // the configuration the index was opened for
    private static volatile Resources appResources;  // over appAssets, for the chrome's theme lookups
    private static volatile Resources.Theme appTheme;
    private static volatile String appThemeName;

    // ---- The framework's ApkAssets, once per process ----

    private static boolean systemReady;

    /** AssetManager.createSystemAssetsInZygoteLocked without its overlays: that step asks
     *  OverlayConfig for the device's static runtime resource overlays (the partitions, through
     *  android.os.Environment and libcore's Os). Win.apk has none, so the system AssetManager is
     *  the framework's table alone, marked as the system's. */
    private static synchronized void installSystemAssets() {
        if (systemReady) return;
        String path = System.getProperty("winapk.aosp.res.framework");
        if (path == null || !new File(path).isFile()) {
            throw new IllegalStateException("winapk.aosp.res.framework names no file: " + path);
        }
        try {
            long t0 = System.nanoTime();
            ApkAssets fw = ApkAssets.loadFromPath(path, ApkAssets.PROPERTY_SYSTEM);
            ApkAssets[] arr = {fw};
            set("sSystemApkAssetsSet", new android.util.ArraySet<>(Arrays.asList(arr)));
            set("sSystemApkAssets", arr);
            Constructor<AssetManager> k = AssetManager.class.getDeclaredConstructor(boolean.class);
            k.setAccessible(true);
            AssetManager sys = k.newInstance(true);
            sys.setApkAssets(arr, false);
            set("sSystem", sys);
            systemReady = true;
            log(String.format("the framework's resources (%s) up in %.0f ms", new File(path).getName(),
                    (System.nanoTime() - t0) / 1e6));
        } catch (IOException | ReflectiveOperationException e) {
            throw new IllegalStateException("the framework's resources did not load from " + path, e);
        }
    }

    private static void set(String field, Object value) throws ReflectiveOperationException {
        Field f = AssetManager.class.getDeclaredField(field);
        f.setAccessible(true);
        f.set(null, value);
    }

    // ---- What the loader builds the app's resources with ----

    @Override public AssetManager openAssets(String apkPath) throws IOException {
        installSystemAssets();
        long t0 = System.nanoTime();
        AssetManager.Builder b = new AssetManager.Builder();
        List<ApkAssets> loaded = new ArrayList<>();
        for (File f : InstalledSplits.of(apkPath)) loaded.add(ApkAssets.loadFromPath(f.getPath()));
        for (ApkAssets a : loaded) b.addApkAssets(a);
        AssetManager am = b.build();
        appApkAssets = loaded;
        appAssets = am;
        log(String.format("the app's resources (%s) up in %.0f ms", new File(apkPath).getName(),
                (System.nanoTime() - t0) / 1e6));
        return am;
    }

    @Override public void updateSystemConfiguration(Configuration cfg, DisplayMetrics metrics) {
        Resources.updateSystemConfiguration(cfg, metrics, null);
    }

    @Override public TypedArray themelessTypedArray(Resources res, int[] attrs) {
        return res.newTheme().obtainStyledAttributes(attrs);
    }

    @Override public TypedArray themelessTypedArray(Resources res, int[] attrs, AttributeSet set) {
        return res.obtainAttributes(set, attrs);
    }

    @Override public Resources placeholderResources() { return Resources.getSystem(); }

    @Override public int frameworkAttrId(String name) {
        return name == null ? 0 : Resources.getSystem().getIdentifier(name, "attr", "android");
    }

    /** The index's text rendering, as Win.apk's Resources.openRawResource served it: AOSP's gives
     *  the compiled bytes, as on a device, which Win.apk's DOM inflaters cannot read. */
    @Override public InputStream openXmlText(Resources res, int id) {
        Object i = index;
        InputStream in = null;
        try {
            if (i != null) in = ri(i).openRaw(id);
        } catch (FileNotFoundException e) {
            in = null;
        }
        if (in == null) throw new Resources.NotFoundException("openXmlText(0x" + Integer.toHexString(id) + ")");
        return in;
    }

    /** The compiled file behind doc, and each element's start tag in it: the text keeps every
     *  element of the file in document order (BinaryXmlParser.toText), so the n-th element of the
     *  DOM is the n-th start tag. AttrSets reads an element's attributes from there. */
    @Override public void markCompiledSource(org.w3c.dom.Document doc, Resources res, int id) {
        if (doc == null || res == null) return;
        byte[] bytes;
        try (InputStream in = res.openRawResource(id)) {
            bytes = in.readAllBytes();
        } catch (IOException | RuntimeException e) {
            return;
        }
        if (!com.macapk.loader.BinaryXmlParser.isBinaryXml(bytes)) return;
        String key = com.macapk.loader.BinaryXmlParser.COMPILED_KEY;
        doc.setUserData(key, bytes, null);
        org.w3c.dom.NodeList all = doc.getElementsByTagName("*");
        for (int n = 0; n < all.getLength(); n++) all.item(n).setUserData(key, n, null);
    }

    // ---- The app's resource index (Win.apk's, by name) ----

    private static ResourceIndex ri(Object index) { return (ResourceIndex) index; }

    @Override public Object openIndex(Configuration cfg, String apkPath) {
        appConfig = new Configuration(cfg);
        return new ResourceIndex(cfg, apkPath);
    }

    @Override public boolean isIndex(Object o) { return o instanceof ResourceIndex; }

    @Override public Object resourceIndex() { return index; }

    @Override public void setResourceIndex(Object i) { index = i; }

    @Override public void prefetchFrameworkTable() { ResourceIndex.prefetchFrameworkTable(); }

    @Override public void setPackageName(String pkg) { packageName = pkg != null ? pkg : ""; }

    @Override public String indexApplicationThemeName(Object i) { return ri(i).getApplicationThemeName(); }

    @Override public void indexSetApplicationThemeName(Object i, String name) { ri(i).setApplicationThemeName(name); }

    @Override public String[] indexTypeNameOf(Object i, int id) { return ri(i).macapkTypeNameOf(id); }

    @Override public String indexFwEntryTypeName(Object i, int id) { return ri(i).macapkFwEntryTypeName(id); }

    @Override public int indexIdOfName(Object i, String name, String type, String pkg) {
        return ri(i).idOfName(name, type, pkg);
    }

    @Override public int indexIdOfFrameworkStyleName(Object i, String name) { return ri(i).idOfFrameworkStyleName(name); }

    @Override public String indexStringForId(Object i, int id) { return ri(i).stringForId(id); }

    @Override public Integer indexIntegerForName(Object i, String name) { return ri(i).integerForName(name); }

    @Override public Integer indexIntegerForId(Object i, int id) { return ri(i).integerForId(id); }

    @Override public Boolean indexBoolById(Object i, int id) { return ri(i).getBoolById(id); }

    @Override public Integer indexColorByName(Object i, String name) { return ri(i).getColorByName(name); }

    @Override public String indexDimenRaw(Object i, String name) { return ri(i).getDimenRaw(name); }

    @Override public Integer indexResolveColorExpr(Object i, String raw) { return ri(i).macapkResolveColorExpr(raw); }

    @Override public String indexResolveAttrInTheme(Object i, String themeName, String attr) {
        return ri(i).resolveAttrInTheme(themeName, attr);
    }

    @Override public String indexStyleNameForIdRef(Object i, String ref) { return ri(i).styleNameForIdRefPublic(ref); }

    @Override public Object indexStyleByName(Object i, String name) { return ri(i).getStyleByName(name); }

    @Override public String styleParent(Object style) { return ((ResourceIndex.StyleDef) style).parent; }

    @Override public Map<String, String> styleItems(Object style) { return ((ResourceIndex.StyleDef) style).items; }

    @Override public InputStream indexOpenRaw(Object i, int id) throws FileNotFoundException { return ri(i).openRaw(id); }

    @Override public InputStream indexOpenRawByName(Object i, String type, String name) {
        return ri(i).openRawByName(type, name);
    }

    @Override public List<Integer> indexFileIdsOfType(Object i, String type) { return ri(i).macapkFileIdsOfType(type); }

    @Override public List<String> indexResourceLocaleTags(Object i) { return ri(i).macapkResourceLocaleTags(); }

    @Override public boolean indexSelectionChangesFor(Object i, Configuration next) {
        return ri(i).macapkSelectionChangesFor(next);
    }

    @Override public int[][] indexAppSizeBuckets(Object i) { return ri(i).macapkAppSizeBuckets(); }

    @Override public boolean dpCrosses(int[] thresholds, int firstDp, int secondDp) {
        return ResourceIndex.macapkDpCrosses(thresholds, firstDp, secondDp);
    }

    // ---- The current app's table, on AOSP's Resources ----

    /** The app's Resources for the lookups below: over the AssetManager openAssets made, in the
     *  configuration the index was opened for, with the system's metrics. */
    private static Resources app() {
        Resources r = appResources;
        if (r != null) return r;
        AssetManager am = appAssets;
        if (am == null) return null;
        synchronized (AospRes.class) {
            if (appResources == null) {
                Configuration c = appConfig != null ? appConfig : Resources.getSystem().getConfiguration();
                appResources = new Resources(am, Resources.getSystem().getDisplayMetrics(), c);
            }
            return appResources;
        }
    }

    private interface Lookup<T> { T get(Resources r) throws Resources.NotFoundException; }

    private static <T> T look(Lookup<T> l) {
        Resources r = app();
        if (r == null) return null;
        try {
            return l.get(r);
        } catch (Resources.NotFoundException | UnsupportedOperationException e) {
            return null;
        }
    }

    @Override public Integer integerForName(String name) {
        return name == null ? null : look(r -> {
            int id = r.getIdentifier(name, "integer", packageName);
            return id == 0 ? null : r.getInteger(id);
        });
    }

    @Override public Integer integerForId(int id) { return look(r -> r.getInteger(id)); }

    @Override public String stringForId(int id) { return look(r -> r.getString(id)); }

    @Override public String resourceTypeForId(int id) { return look(r -> r.getResourceTypeName(id)); }

    @Override public Boolean boolForId(int id) { return look(r -> r.getBoolean(id)); }

    @Override public Integer colorForId(int id) { return look(r -> r.getColor(id, null)); }

    @Override public Integer resolveColorExpr(String raw) {
        Object i = index;
        return i == null || raw == null ? null : ri(i).macapkResolveColorExpr(raw);
    }

    // ---- Theme colours and values for Win.apk's chrome ----

    /** An AOSP Theme with the application theme (the index's, by name) applied. */
    private static Resources.Theme appTheme() {
        Resources r = app();
        Object i = index;
        if (r == null || i == null) return null;
        String name = ri(i).getApplicationThemeName();
        Resources.Theme t = appTheme;
        if (t != null && java.util.Objects.equals(name, appThemeName)) return t;
        t = r.newTheme();
        int id = styleId(r, name);
        if (id != 0) t.applyStyle(id, true);
        appThemeName = name;
        appTheme = t;
        return t;
    }

    /** A style's id from the index's spelling of its name ("AppTheme", "@style/AppTheme",
     *  "@android:style/Theme.Material", "android:Theme.Material"). */
    private static int styleId(Resources r, String name) {
        if (name == null || name.isEmpty()) return 0;
        String n = name.startsWith("@") ? name.substring(1) : name;
        String pkg = packageName;
        if (n.startsWith("android:")) {
            pkg = "android";
            n = n.substring("android:".length());
        }
        if (n.startsWith("style/")) n = n.substring("style/".length());
        int id = r.getIdentifier(n, "style", pkg);
        return id != 0 || pkg.equals("android") ? id : r.getIdentifier(n, "style", "android");
    }

    /** An attribute's id by Win.apk's spelling: "colorPrimary" (the app's first, then the
     *  platform's), "android:colorPrimary", "?attr/colorPrimary", "?android:attr/colorPrimary". */
    static int attrId(Resources r, String name) {
        if (name == null || name.isEmpty()) return 0;
        String n = name;
        if (n.startsWith("?") || n.startsWith("@")) n = n.substring(1);
        boolean platform = false;
        if (n.startsWith("android:")) {
            platform = true;
            n = n.substring("android:".length());
        }
        if (n.startsWith("attr/")) n = n.substring("attr/".length());
        if (!platform && !packageName.isEmpty()) {
            int id = r.getIdentifier(n, "attr", packageName);
            if (id != 0) return id;
        }
        return r.getIdentifier(n, "attr", "android");
    }

    @Override public int themeColor(String attrName, int fallback) {
        Resources.Theme t = appTheme();
        return t == null ? fallback : resolveAttrColor(t, attrName, fallback);
    }

    @Override public float themeCornerDp(String shapeAttrName, float fallbackDp) {
        Resources.Theme t = appTheme();
        if (t == null) return fallbackDp;
        Resources r = t.getResources();
        TypedValue tv = new TypedValue();
        int shape = attrId(r, shapeAttrName);
        int corner = attrId(r, "cornerSize");
        if (shape == 0 || corner == 0 || !t.resolveAttribute(shape, tv, true) || tv.resourceId == 0) return fallbackDp;
        TypedArray a = t.obtainStyledAttributes(tv.resourceId, new int[] {corner});
        try {
            TypedValue v = a.peekValue(0);
            if (v == null || v.type != TypedValue.TYPE_DIMENSION) return fallbackDp;
            return a.getDimension(0, 0f) / r.getDisplayMetrics().density;
        } finally {
            a.recycle();
        }
    }

    @Override public String themeAttrRawFor(String expr, Resources.Theme theme) {
        return theme == null ? null : resolveAttrRaw(theme, expr);
    }

    @Override public int resolveAttrColor(Resources.Theme theme, String attrName, int fallback) {
        Resources r = theme.getResources();
        int id = attrId(r, attrName);
        if (id == 0) return fallback;
        TypedValue tv = new TypedValue();
        if (!theme.resolveAttribute(id, tv, true)) return fallback;
        if (tv.type >= TypedValue.TYPE_FIRST_COLOR_INT && tv.type <= TypedValue.TYPE_LAST_COLOR_INT) return tv.data;
        if (tv.resourceId != 0) {
            try {
                return r.getColorStateList(tv.resourceId, theme).getDefaultColor();
            } catch (RuntimeException ignored) {
                // not a colour: the caller's fallback, as before
            }
        }
        return fallback;
    }

    @Override public String resolveAttrRaw(Resources.Theme theme, String attrName) {
        Resources r = theme.getResources();
        int id = attrId(r, attrName);
        TypedValue tv = new TypedValue();
        if (id == 0 || !theme.resolveAttribute(id, tv, false)) return null;
        return rawOf(r, tv);
    }

    // AOSP's theme is one merged bag with no fallback to the manifest theme, so the "honest"
    // answer (the applied styles only) is the same answer.
    @Override public String resolveAttrRawHonest(Resources.Theme theme, String attrName) {
        return resolveAttrRaw(theme, attrName);
    }

    @Override public boolean attrStyleLane() { return !"0".equals(System.getenv("MACAPK_ATTRSTYLE")); }

    /** A value in the text spelling Win.apk's index uses for style items (ArscReader.formatValue),
     *  which the callers parse. */
    static String rawOf(Resources r, TypedValue tv) {
        int data = tv.data;
        switch (tv.type) {
            case TypedValue.TYPE_NULL: return data == TypedValue.DATA_NULL_EMPTY ? "@empty" : "@null";
            case TypedValue.TYPE_STRING: return tv.string == null ? "" : tv.string.toString();
            case TypedValue.TYPE_INT_BOOLEAN: return data != 0 ? "true" : "false";
            case TypedValue.TYPE_INT_DEC: return Integer.toString(data);
            case TypedValue.TYPE_INT_HEX: return String.format("0x%x", data);
            case TypedValue.TYPE_FLOAT: return fmt(Float.intBitsToFloat(data));
            case TypedValue.TYPE_REFERENCE: return data == 0 ? "@null" : "@" + nameOf(r, data, false);
            case TypedValue.TYPE_ATTRIBUTE: return "?" + nameOf(r, data, true);
            case TypedValue.TYPE_INT_COLOR_ARGB8: return String.format("#%08x", data);
            case TypedValue.TYPE_INT_COLOR_RGB8: return String.format("#%06x", data & 0xffffff);
            case TypedValue.TYPE_INT_COLOR_ARGB4:
                return String.format("#%x%x%x%x", (data >>> 28) & 0xf, (data >>> 20) & 0xf, (data >>> 12) & 0xf,
                        (data >>> 4) & 0xf);
            case TypedValue.TYPE_INT_COLOR_RGB4:
                return String.format("#%x%x%x", (data >>> 20) & 0xf, (data >>> 12) & 0xf, (data >>> 4) & 0xf);
            case TypedValue.TYPE_DIMENSION:
                return fmt(TypedValue.complexToFloat(data)) + DIM_UNITS[data & TypedValue.COMPLEX_UNIT_MASK];
            case TypedValue.TYPE_FRACTION:
                return fmt(TypedValue.complexToFloat(data) * 100)
                        + ((data & TypedValue.COMPLEX_UNIT_MASK) == 0 ? "%" : "%p");
            default: return Integer.toString(data);
        }
    }

    private static final String[] DIM_UNITS = {
        "px", "dp", "sp", "pt", "in", "mm", "??", "??", "??", "??", "??", "??", "??", "??", "??", "??",
    };

    private static String fmt(float f) {
        if (f == (long) f) return String.format(java.util.Locale.ROOT, "%.1f", f);
        return Float.toString(f);
    }

    /** "type/name", "pkg:type/name" for another package; an attribute without its "attr/". */
    private static String nameOf(Resources r, int id, boolean attr) {
        try {
            String pkg = r.getResourcePackageName(id);
            String tn = r.getResourceTypeName(id) + "/" + r.getResourceEntryName(id);
            if (attr && tn.startsWith("attr/")) tn = tn.substring("attr/".length());
            return (pkg.equals(packageName) ? "" : pkg + ":") + tn;
        } catch (Resources.NotFoundException e) {
            return String.format("0x%08x", id);
        }
    }

    // ---- Drawables, tints, densities ----

    @Override public Drawable frameworkDrawableByName(Resources res, String name) {
        return frameworkDrawableByName(res, name, null);
    }

    @Override public Drawable frameworkDrawableByName(Resources res, String name, Resources.Theme theme) {
        if (res == null || name == null) return null;
        int id = res.getIdentifier(name, "drawable", "android");
        if (id == 0) id = themedIndicator(res, name, theme);
        if (id == 0) return null;
        try {
            return res.getDrawable(id, theme);
        } catch (Resources.NotFoundException e) {
            return null;
        }
    }

    /** Win.apk's own framework drawables have names AOSP's table lacks: the check box and radio
     *  button indicators it synthesized ("btn_check_material", "btn_radio_material"). On AOSP's
     *  resources they are what the theme's Widget.Material.CompoundButton styles name,
     *  ?android:attr/listChoiceIndicatorMultiple and ...Single (btn_check_material_anim...). */
    private static int themedIndicator(Resources res, String name, Resources.Theme theme) {
        String attr = name.equals("btn_check_material") ? "listChoiceIndicatorMultiple"
                : name.equals("btn_radio_material") ? "listChoiceIndicatorSingle" : null;
        if (attr == null) return 0;
        Resources.Theme t = theme != null ? theme : appTheme();
        int a = res.getIdentifier(attr, "attr", "android");
        TypedValue tv = new TypedValue();
        if (t == null || a == 0 || !t.resolveAttribute(a, tv, true)) return 0;
        return tv.resourceId;
    }

    @Override public ColorStateList tintListFor(Resources res, String v, Resources.Theme theme) {
        if (res == null || v == null || v.isEmpty()) return null;
        try {
            if (v.startsWith("#")) return ColorStateList.valueOf(android.graphics.Color.parseColor(v));
            int id;
            if (v.startsWith("?")) {
                TypedValue tv = new TypedValue();
                int attr = attrId(res, v);
                if (theme == null || attr == 0 || !theme.resolveAttribute(attr, tv, true)) return null;
                if (tv.type >= TypedValue.TYPE_FIRST_COLOR_INT && tv.type <= TypedValue.TYPE_LAST_COLOR_INT) {
                    return ColorStateList.valueOf(tv.data);
                }
                id = tv.resourceId;
            } else {
                id = refId(res, v);
            }
            return id == 0 ? null : res.getColorStateList(id, theme);
        } catch (RuntimeException e) {
            return null;
        }
    }

    /** "@[pkg:]type/name" or "@0x7f010203" to its id. */
    private static int refId(Resources res, String v) {
        String n = v.startsWith("@") ? v.substring(1) : v;
        if (n.startsWith("0x")) {
            try {
                return (int) Long.parseLong(n.substring(2), 16);
            } catch (NumberFormatException e) {
                return 0;
            }
        }
        String pkg = packageName;
        int colon = n.indexOf(':');
        if (colon > 0) {
            pkg = n.substring(0, colon);
            n = n.substring(colon + 1);
        }
        int slash = n.indexOf('/');
        if (slash <= 0) return 0;
        return res.getIdentifier(n.substring(slash + 1), n.substring(0, slash), pkg);
    }

    @Override public int resourceDensity(Resources res, int id) {
        try {
            TypedValue tv = new TypedValue();
            res.getValue(id, tv, true);
            return tv.density;
        } catch (Resources.NotFoundException e) {
            return 0;
        }
    }

    // ---- Configuration ----

    /** The phone's configuration Win.apk starts from (Configuration.macapkPhoneDefaults): AOSP's
     *  defaults with the values Win.apk's own Configuration starts with, since AOSP's are
     *  "undefined" where a process is handed a populated one by the system server. */
    @Override public Configuration phoneDefaults() {
        Configuration c = new Configuration();
        c.setToDefaults();
        c.orientation = Configuration.ORIENTATION_PORTRAIT;
        c.screenLayout = Configuration.SCREENLAYOUT_SIZE_NORMAL | Configuration.SCREENLAYOUT_LONG_NO
                | defaultLayoutDir();
        c.screenWidthDp = 360;
        c.screenHeightDp = 640;
        c.smallestScreenWidthDp = 360;
        c.densityDpi = DisplayMetrics.DENSITY_DEVICE_STABLE;
        c.uiMode = Configuration.UI_MODE_TYPE_NORMAL | Configuration.UI_MODE_NIGHT_NO;
        c.touchscreen = Configuration.TOUCHSCREEN_FINGER;
        boolean keys = hostKeyboard();
        c.keyboard = keys ? Configuration.KEYBOARD_QWERTY : Configuration.KEYBOARD_NOKEYS;
        c.keyboardHidden = Configuration.KEYBOARDHIDDEN_NO;
        c.hardKeyboardHidden = keys ? Configuration.HARDKEYBOARDHIDDEN_NO : Configuration.HARDKEYBOARDHIDDEN_YES;
        c.navigation = Configuration.NAVIGATION_NONAV;
        c.navigationHidden = Configuration.NAVIGATIONHIDDEN_YES;
        c.colorMode = 0;
        c.fontWeightAdjustment = 0;
        return c;
    }

    /** Win.apk's InputManager.MACAPK_HOST_KEYBOARD (whether the PC's keyboard is the app's), by
     *  reflection: this class is compiled against AOSP's InputManager, which has no such field. */
    private static boolean hostKeyboard() {
        try {
            return android.hardware.input.InputManager.class.getField("MACAPK_HOST_KEYBOARD").getBoolean(null);
        } catch (ReflectiveOperationException e) {
            return false;
        }
    }

    private static int defaultLayoutDir() {
        try {
            return com.macapk.loader.gfx.GfxBackend.get().isLocaleRtl(java.util.Locale.getDefault())
                    ? Configuration.SCREENLAYOUT_LAYOUTDIR_RTL : Configuration.SCREENLAYOUT_LAYOUTDIR_LTR;
        } catch (Throwable t) {
            return Configuration.SCREENLAYOUT_LAYOUTDIR_LTR;
        }
    }

    /** Configuration.macapkDeriveScreenLayout: screenLayout from the dp sizes, through AOSP's own
     *  Configuration.reduceScreenLayout, keeping the direction bits. */
    @Override public void deriveScreenLayout(Configuration cfg) {
        if ("0".equals(System.getenv("MACAPK_SCREENLAYOUT_DERIVE"))) return;
        int w = cfg.screenWidthDp, h = cfg.screenHeightDp;
        if (w <= 0 || h <= 0) return;
        int dir = cfg.screenLayout & Configuration.SCREENLAYOUT_LAYOUTDIR_MASK;
        int seed = Configuration.SCREENLAYOUT_LONG_YES | Configuration.SCREENLAYOUT_SIZE_XLARGE;
        cfg.screenLayout = Configuration.reduceScreenLayout(seed, Math.max(w, h), Math.min(w, h)) | dir;
    }

    private static final Object SYSTEM_EDIT_LOCK = new Object();

    @Override public void editSystemConfiguration(Consumer<Configuration> edit, DisplayMetrics metrics) {
        synchronized (SYSTEM_EDIT_LOCK) {
            Resources sys = Resources.getSystem();
            Configuration next = new Configuration(sys.getConfiguration());
            if (edit != null) edit.accept(next);
            Resources.updateSystemConfiguration(next, metrics, null);
        }
    }

    // ---- TypedArray ----

    @Override public TypedValue peekRecord(TypedArray a, int index) {
        TypedValue v = a.peekValue(index);
        return v != null ? v : new TypedValue();
    }

    // AOSP's attribute resolution records no entry for an attribute written @null (TYPE_NULL with
    // DATA_NULL_UNDEFINED): it reads as absent, the same as one never written.
    @Override public boolean declaredTypeNull(TypedArray a, int index) { return false; }

    // ---- A context's configuration override (ResourcesManager's part) ----

    /** One override's Resources: what AOSP's ResourcesManager keeps per ResourcesKey (here the
     *  base and the override; the APKs are the app's own), shared by every context asking for it. */
    private static final class Overridden {
        final Resources base;
        final Configuration override;
        final Resources res;
        Overridden(Resources base, Configuration override, Resources res) {
            this.base = base;
            this.override = override;
            this.res = res;
        }
    }

    private static final List<Overridden> sOverridden = new ArrayList<>();
    private static boolean sFollowing;

    /** ResourcesManager.generateConfig: the base's configuration, updated from the override. */
    private static Configuration merged(Resources base, Configuration override) {
        Configuration c = new Configuration(base.getConfiguration());
        c.updateFrom(override);
        return c;
    }

    @Override public Resources resourcesForConfiguration(Resources base, Configuration override) {
        if (base == null || override == null) return base;
        Configuration want = merged(base, override);
        if (want.equals(base.getConfiguration())) return base;
        synchronized (sOverridden) {
            for (Overridden o : sOverridden) {
                if (o.base == base && o.override.equals(override)) return o.res;
            }
            // ResourcesManager.createResourcesImpl: an AssetManager of its own over the same
            // ApkAssets (they are shared; only the configuration differs), and a ResourcesImpl for
            // the merged configuration.
            AssetManager.Builder b = new AssetManager.Builder();
            for (ApkAssets a : appApkAssets) b.addApkAssets(a);
            android.content.res.ResourcesImpl impl = new android.content.res.ResourcesImpl(b.build(),
                    base.getDisplayMetrics(), want, base.getDisplayAdjustments());
            Resources res = new Resources(base.getClassLoader());
            res.setImpl(impl);
            sOverridden.add(new Overridden(base, new Configuration(override), res));
            followConfigurationChanges();
            return res;
        }
    }

    /** ResourcesManager.applyConfigurationToResources: when the application's configuration
     *  changes (rotation, size, locale, night mode), each override's Resources take the new base
     *  updated from their override. Win.apk tells the Application's callbacks after updating its
     *  Resources, as ActivityThread does after ResourcesManager. */
    private static void followConfigurationChanges() {
        if (sFollowing) return;
        android.content.Context app;
        try {  // Win.apk's Application.macapk_get (this jar compiles against android-all's)
            app = (android.content.Context) android.app.Application.class.getMethod("macapk_get").invoke(null);
        } catch (ReflectiveOperationException e) {
            return;
        }
        if (app == null) return;
        sFollowing = true;
        app.registerComponentCallbacks(new android.content.ComponentCallbacks() {
            @Override public void onConfigurationChanged(Configuration newConfig) {
                synchronized (sOverridden) {
                    for (Overridden o : sOverridden) {
                        Configuration want = merged(o.base, o.override);
                        if (!want.equals(o.res.getConfiguration())) {
                            o.res.updateConfiguration(want, o.base.getDisplayMetrics());
                        }
                    }
                }
            }

            @Override public void onLowMemory() {}
        });
    }
}
