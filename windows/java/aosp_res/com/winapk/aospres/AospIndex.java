// The resource "index" with AOSP's views on too (-Dwinapk.aosp.res=1 and -Dwinapk.aosp.views=1):
// what Win.apk's code still asks by name (ResBackend's index* calls: the window policy of a theme,
// a style id's name, R fields by name, the app's size buckets...) answered from AOSP's own
// Resources and AssetManager, the tables they already hold, instead of Win.apk's ResourceIndex,
// which parsed the app's table and the platform's (framework-res.apk) a second time into Java
// rows. With AOSP's resources alone (views off) Win.apk's views still inflate through the
// ResourceIndex, so AospRes keeps opening that one then.
//
// Answers follow AOSP's: getIdentifier's name forms, a style's bag as AssetManager2::GetBag
// merges it (its parents' items included, so a style has no parent left to walk), the size
// configurations ActivityThread reports (AssetManager.getSizeConfigurations, the app's own), the
// locales the app ships (AssetManager.getNonSystemLocales).
package com.winapk.aospres;

import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.util.TypedValue;

import java.io.InputStream;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeSet;

final class AospIndex {
    final Configuration config;
    volatile String applicationThemeName;

    AospIndex(Configuration config) { this.config = new Configuration(config); }

    /** A style as Win.apk's callers read one: its items by attribute name ("android:x" for the
     *  platform's), the bag already merged with its parents', so no parent. */
    static final class Style {
        final Map<String, String> items;
        Style(Map<String, String> items) { this.items = items; }
    }

    // ---- names and ids ----

    /** {type, entry} of an id, as ResourceIndex.macapkTypeNameOf; null when the tables lack it. */
    static String[] typeNameOf(Resources r, int id) {
        try {
            return new String[] {r.getResourceTypeName(id), r.getResourceEntryName(id)};
        } catch (Resources.NotFoundException e) {
            return null;
        }
    }

    /** "type/name" of a platform id, as ResourceIndex.macapkFwEntryTypeName. */
    static String fwEntryTypeName(Resources r, int id) {
        if ((id >>> 24) != 0x01) return null;
        String[] tn = typeNameOf(r, id);
        return tn == null ? null : tn[0] + "/" + tn[1];
    }

    static int idOfName(Resources r, String name, String type, String pkg) {
        if (name == null || name.isEmpty()) return 0;
        return r.getIdentifier(name, type, pkg);
    }

    static int idOfFrameworkStyleName(Resources r, String name) {
        if (name == null || name.isEmpty()) return 0;
        String n = name;
        if (n.startsWith("@android:style/")) n = n.substring(15);
        else if (n.startsWith("android:style/")) n = n.substring(14);
        else if (n.startsWith("@style/")) n = n.substring(7);
        return r.getIdentifier(n, "style", "android");
    }

    /** A style id reference ("@0x7f13012e") to the name the style calls take ("AppTheme",
     *  "android:Theme.Material"); null for anything else. */
    static String styleNameForIdRef(Resources r, String ref) {
        if (ref == null) return null;
        String s = ref.trim();
        if (!(s.startsWith("@0x") || s.startsWith("@0X"))) return null;
        try {
            int id = (int) Long.parseLong(s.substring(3), 16);
            if (!"style".equals(r.getResourceTypeName(id))) return null;
            String n = r.getResourceEntryName(id);
            return "android".equals(r.getResourcePackageName(id)) ? "android:" + n : n;
        } catch (NumberFormatException | Resources.NotFoundException e) {
            return null;
        }
    }

    // ---- styles ----

    private static Method sStyleAttributes;

    /** AssetManager.getStyleAttributes: the attributes of a style's merged bag. */
    private static int[] styleAttributes(AssetManager am, int styleId) {
        try {
            Method m = sStyleAttributes;
            if (m == null) {
                m = AssetManager.class.getDeclaredMethod("getStyleAttributes", int.class);
                m.setAccessible(true);
                sStyleAttributes = m;
            }
            return (int[]) m.invoke(am, styleId);
        } catch (ReflectiveOperationException e) {
            return null;
        }
    }

    static Style styleByName(Resources r, int styleId) {
        if (styleId == 0) return null;
        int[] attrs = styleAttributes(r.getAssets(), styleId);
        if (attrs == null) return null;
        attrs = attrs.clone();
        Arrays.sort(attrs);  // the order obtainStyledAttributes takes
        Resources.Theme t = r.newTheme();
        t.applyStyle(styleId, true);
        TypedArray a = t.obtainStyledAttributes(attrs);
        Map<String, String> items = new LinkedHashMap<>();
        try {
            TypedValue v = new TypedValue();
            for (int i = 0; i < attrs.length; i++) {
                if (!a.getValue(i, v)) continue;
                String key;
                try {
                    String n = r.getResourceEntryName(attrs[i]);
                    key = "android".equals(r.getResourcePackageName(attrs[i])) ? "android:" + n : n;
                } catch (Resources.NotFoundException e) {
                    continue;
                }
                items.put(key, AospRes.rawOf(r, v));
            }
        } finally {
            a.recycle();
        }
        return new Style(items);
    }

    // ---- values ----

    static String dimenRaw(Resources r, String name, String pkg) {
        int id = r.getIdentifier(name, "dimen", pkg);
        if (id == 0) return null;
        TypedValue v = new TypedValue();
        r.getValue(id, v, true);
        return AospRes.rawOf(r, v);
    }

    /** "#rgb"/"#argb"/"#rrggbb"/"#aarrggbb", "@color/x", "@android:color/x", "@0x..", "?attr"
     *  through the theme; null when it names no colour. */
    static Integer colorExpr(Resources r, Resources.Theme theme, String raw, String pkg) {
        String s = raw.trim();
        try {
            if (s.startsWith("#")) return android.graphics.Color.parseColor(s);
            if (s.startsWith("?")) {
                if (theme == null) return null;
                int c = AospRes.INSTANCE.resolveAttrColor(theme, s, 0);
                return c == 0 ? null : c;
            }
            if (s.startsWith("@0x") || s.startsWith("@0X")) {
                return r.getColor((int) Long.parseLong(s.substring(3), 16), theme);
            }
            if (s.startsWith("@")) {
                int id = r.getIdentifier(s.substring(1), "color", pkg);
                return id == 0 ? null : r.getColor(id, theme);
            }
        } catch (RuntimeException e) {
            return null;
        }
        return null;
    }

    static InputStream openRaw(Resources r, int id) {
        try {
            return r.openRawResource(id);
        } catch (Resources.NotFoundException e) {
            return null;
        }
    }

    /** Every id of a type of the app's package whose value is a file, ascending. aapt2 numbers a
     *  package's types and entries densely from 0, so the walk stops at the first missing type and,
     *  in a type, after a run of missing entries (a shrunk entry leaves a hole). */
    static List<Integer> fileIdsOfType(Resources r, int pkgId, String type) {
        List<Integer> out = new ArrayList<>();
        TypedValue v = new TypedValue();
        for (int t = 1; t < 0x100; t++) {
            int first = (pkgId << 24) | (t << 16);
            String tn;
            try {
                tn = r.getResourceTypeName(first);
            } catch (Resources.NotFoundException e) {
                if (t > 1) break;
                continue;
            }
            if (!type.equals(tn)) continue;
            int misses = 0;
            for (int e = 0; e < 0x10000 && misses < 64; e++) {
                int id = first | e;
                try {
                    r.getValue(id, v, false);
                    misses = 0;
                    if (v.type == TypedValue.TYPE_STRING && v.string != null && v.string.toString().startsWith("res/")) {
                        out.add(id);
                    }
                } catch (Resources.NotFoundException ex) {
                    misses++;
                }
            }
            break;
        }
        return out;
    }

    /** A compiled XML resource in the text Win.apk's DOM inflaters read (ResourceIndex.openRaw's
     *  rendering: BinaryXmlParser with the ids' names), or its bytes when it is not compiled XML. */
    static InputStream openXmlText(Resources r, int id) {
        if (r == null) return null;
        byte[] bytes;
        try (InputStream in = r.openRawResource(id)) {
            bytes = in.readAllBytes();
        } catch (java.io.IOException | Resources.NotFoundException e) {
            return null;
        }
        if (!com.macapk.loader.BinaryXmlParser.isBinaryXml(bytes)) return new java.io.ByteArrayInputStream(bytes);
        try {
            org.w3c.dom.Document doc = com.macapk.loader.BinaryXmlParser.parse(bytes,
                    new com.macapk.loader.BinaryXmlParser.Names() {
                        @Override public String typeName(int resId) {
                            String[] tn = typeNameOf(r, resId);
                            return tn == null ? null : tn[0] + "/" + tn[1];
                        }
                        @Override public String enumText(int attrResId, int dataType, int data) { return null; }
                    });
            return new java.io.ByteArrayInputStream(com.macapk.loader.BinaryXmlParser.toText(doc)
                    .getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (Exception e) {
            return null;
        }
    }

    static List<String> resourceLocaleTags(AssetManager am) {
        TreeSet<String> out = new TreeSet<>();
        try {
            Method m = AssetManager.class.getDeclaredMethod("getNonSystemLocales");
            m.setAccessible(true);
            for (String tag : (String[]) m.invoke(am)) {
                if (tag != null && !tag.isEmpty()) out.add(tag);
            }
        } catch (ReflectiveOperationException e) {
            // none known
        }
        return new ArrayList<>(out);
    }

    /** The size buckets ActivityThread reports (SizeConfigurationBuckets from the app's
     *  AssetManager.getSizeConfigurations), in ResourceIndex.macapkAppSizeBuckets' shape: the
     *  -w, -h and -sw dp thresholds, the screen-size classes, and whether any names long/notlong. */
    static int[][] sizeBuckets(AssetManager am) {
        Configuration[] cfgs = new Configuration[0];
        if (am != null) try {
            Method m = AssetManager.class.getDeclaredMethod("getSizeConfigurations");
            m.setAccessible(true);
            cfgs = (Configuration[]) m.invoke(am);
        } catch (ReflectiveOperationException e) {
            cfgs = new Configuration[0];
        }
        TreeSet<Integer> w = new TreeSet<>(), h = new TreeSet<>(), sw = new TreeSet<>(), sizes = new TreeSet<>();
        boolean longSet = false;
        for (Configuration c : cfgs) {
            if (c == null) continue;
            if (c.screenWidthDp != Configuration.SCREEN_WIDTH_DP_UNDEFINED) w.add(c.screenWidthDp);
            if (c.screenHeightDp != Configuration.SCREEN_HEIGHT_DP_UNDEFINED) h.add(c.screenHeightDp);
            if (c.smallestScreenWidthDp != Configuration.SMALLEST_SCREEN_WIDTH_DP_UNDEFINED) {
                sw.add(c.smallestScreenWidthDp);
            }
            int size = c.screenLayout & Configuration.SCREENLAYOUT_SIZE_MASK;
            if (size != Configuration.SCREENLAYOUT_SIZE_UNDEFINED) sizes.add(size);
            if ((c.screenLayout & Configuration.SCREENLAYOUT_LONG_MASK) != Configuration.SCREENLAYOUT_LONG_UNDEFINED) {
                longSet = true;
            }
        }
        return new int[][] {ints(w), ints(h), ints(sw), ints(sizes), new int[] {longSet ? 1 : 0}};
    }

    private static int[] ints(TreeSet<Integer> s) {
        int[] a = new int[s.size()];
        int i = 0;
        for (int v : s) a[i++] = v;
        return a;
    }
}
