// ArscRes -- ResBackend on Win.apk's own resources (android.content.res over
// ArscReader and ResourceIndex): each method calls the member it stands for, and
// nothing else.
package com.macapk.loader.res;

import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.graphics.drawable.Drawable;
import android.util.DisplayMetrics;
import android.util.TypedValue;

import com.macapk.loader.ResourceIndex;

import java.io.FileNotFoundException;
import java.io.InputStream;
import java.util.List;
import java.util.Map;
import java.util.function.Consumer;

final class ArscRes implements ResBackend {

    static final ArscRes INSTANCE = new ArscRes();

    private ArscRes() {}

    private static ResourceIndex ri(Object index) { return (ResourceIndex) index; }

    // ---- What the loader builds the app's resources with ----

    @Override public android.content.res.AssetManager openAssets(String apkPath) throws java.io.IOException {
        return new android.content.res.AssetManager(apkPath);
    }

    @Override public void updateSystemConfiguration(Configuration cfg, DisplayMetrics metrics) {
        Resources.updateSystemConfiguration(cfg, metrics, null);
    }

    @Override public TypedArray themelessTypedArray(Resources res, int[] attrs) { return new TypedArray(res, attrs); }

    @Override public TypedArray themelessTypedArray(Resources res, int[] attrs, android.util.AttributeSet set) {
        return new TypedArray(res, attrs, set);
    }

    @Override public Resources placeholderResources() { return new Resources(null, null, null); }

    @Override public int frameworkAttrId(String name) { return ColorStateList.macapkFrameworkAttrId(name); }

    @Override public InputStream openXmlText(Resources res, int id) { return res.openRawResource(id); }

    @Override public void markCompiledSource(org.w3c.dom.Document doc, Resources res, int id) {}

    // ---- The app's resource index ----

    @Override public Object openIndex(Configuration cfg, String apkPath) { return new ResourceIndex(cfg, apkPath); }

    @Override public boolean isIndex(Object o) { return o instanceof ResourceIndex; }

    @Override public Object resourceIndex() { return Resources.macapkGetResourceIndex(); }

    @Override public void setResourceIndex(Object index) { Resources.macapkSetResourceIndex(index); }

    @Override public void prefetchFrameworkTable() { ResourceIndex.prefetchFrameworkTable(); }

    @Override public void setPackageName(String pkg) { Resources.macapkSetPackageName(pkg); }

    // ---- Lookups on an index ----

    @Override public String indexApplicationThemeName(Object index) { return ri(index).getApplicationThemeName(); }

    @Override public void indexSetApplicationThemeName(Object index, String name) {
        ri(index).setApplicationThemeName(name);
    }

    @Override public String[] indexTypeNameOf(Object index, int id) { return ri(index).macapkTypeNameOf(id); }

    @Override public String indexFwEntryTypeName(Object index, int id) { return ri(index).macapkFwEntryTypeName(id); }

    @Override public int indexIdOfName(Object index, String name, String type, String pkg) {
        return ri(index).idOfName(name, type, pkg);
    }

    @Override public int indexIdOfFrameworkStyleName(Object index, String name) {
        return ri(index).idOfFrameworkStyleName(name);
    }

    @Override public String indexStringForId(Object index, int id) { return ri(index).stringForId(id); }

    @Override public Integer indexIntegerForName(Object index, String name) { return ri(index).integerForName(name); }

    @Override public Integer indexIntegerForId(Object index, int id) { return ri(index).integerForId(id); }

    @Override public Boolean indexBoolById(Object index, int id) { return ri(index).getBoolById(id); }

    @Override public Integer indexColorByName(Object index, String name) { return ri(index).getColorByName(name); }

    @Override public String indexDimenRaw(Object index, String name) { return ri(index).getDimenRaw(name); }

    @Override public Integer indexResolveColorExpr(Object index, String raw) {
        return ri(index).macapkResolveColorExpr(raw);
    }

    @Override public String indexResolveAttrInTheme(Object index, String themeName, String attr) {
        return ri(index).resolveAttrInTheme(themeName, attr);
    }

    @Override public String indexStyleNameForIdRef(Object index, String ref) {
        return ri(index).styleNameForIdRefPublic(ref);
    }

    @Override public Object indexStyleByName(Object index, String name) { return ri(index).getStyleByName(name); }

    @Override public String styleParent(Object style) { return ((ResourceIndex.StyleDef) style).parent; }

    @Override public Map<String, String> styleItems(Object style) { return ((ResourceIndex.StyleDef) style).items; }

    @Override public InputStream indexOpenRaw(Object index, int id) throws FileNotFoundException {
        return ri(index).openRaw(id);
    }

    @Override public InputStream indexOpenRawByName(Object index, String type, String name) {
        return ri(index).openRawByName(type, name);
    }

    @Override public List<Integer> indexFileIdsOfType(Object index, String type) {
        return ri(index).macapkFileIdsOfType(type);
    }

    @Override public List<String> indexResourceLocaleTags(Object index) { return ri(index).macapkResourceLocaleTags(); }

    @Override public boolean indexSelectionChangesFor(Object index, Configuration next) {
        return ri(index).macapkSelectionChangesFor(next);
    }

    @Override public int[][] indexAppSizeBuckets(Object index) { return ri(index).macapkAppSizeBuckets(); }

    @Override public boolean dpCrosses(int[] thresholds, int firstDp, int secondDp) {
        return ResourceIndex.macapkDpCrosses(thresholds, firstDp, secondDp);
    }

    // ---- The current app's table ----

    @Override public Integer integerForName(String name) { return Resources.macapkIntegerForName(name); }

    @Override public Integer integerForId(int id) { return Resources.macapkIntegerForId(id); }

    @Override public String stringForId(int id) { return Resources.macapkStringForId(id); }

    @Override public String resourceTypeForId(int id) { return Resources.macapkResourceTypeForId(id); }

    @Override public Boolean boolForId(int id) { return Resources.macapkBoolForId(id); }

    @Override public Integer colorForId(int id) { return Resources.macapkColorForId(id); }

    @Override public Integer resolveColorExpr(String raw) { return Resources.macapkResolveColorExpr(raw); }

    // ---- Theme colours and values ----

    @Override public int themeColor(String attrName, int fallback) {
        return Resources.macapkThemeColor(attrName, fallback);
    }

    @Override public float themeCornerDp(String shapeAttrName, float fallbackDp) {
        return Resources.macapkThemeCornerDp(shapeAttrName, fallbackDp);
    }

    @Override public String themeAttrRawFor(String expr, Resources.Theme theme) {
        return Resources.macapkThemeAttrRawFor(expr, theme);
    }

    @Override public int resolveAttrColor(Resources.Theme theme, String attrName, int fallback) {
        return theme.resolveAttrColor(attrName, fallback);
    }

    @Override public String resolveAttrRaw(Resources.Theme theme, String attrName) {
        return theme.resolveAttrRaw(attrName);
    }

    @Override public String resolveAttrRawHonest(Resources.Theme theme, String attrName) {
        return theme.macapkResolveAttrRawHonest(attrName);
    }

    @Override public boolean attrStyleLane() { return Resources.MACAPK_ATTRSTYLE; }

    // ---- Drawables, tints, densities ----

    @Override public Drawable frameworkDrawableByName(Resources res, String name) {
        return res.macapkFrameworkDrawableByName(name);
    }

    @Override public Drawable frameworkDrawableByName(Resources res, String name, Resources.Theme theme) {
        return res.macapkFrameworkDrawableByName(name, theme);
    }

    @Override public ColorStateList tintListFor(Resources res, String v, Resources.Theme theme) {
        return res.macapkTintListFor(v, theme);
    }

    @Override public int resourceDensity(Resources res, int id) { return res.macapkResourceDensity(id); }

    // ---- Configuration ----

    @Override public Configuration phoneDefaults() { return Configuration.macapkPhoneDefaults(); }

    @Override public void deriveScreenLayout(Configuration cfg) { cfg.macapkDeriveScreenLayout(); }

    /** Win.apk's resources have one configuration, the application's: an override has no
     *  Resources of its own (what createConfigurationContext always answered). */
    @Override public Resources resourcesForConfiguration(Resources base, Configuration override) { return base; }

    @Override public void editSystemConfiguration(Consumer<Configuration> edit, DisplayMetrics metrics) {
        Resources.macapkEditSystemConfiguration(edit, metrics);
    }

    // ---- TypedArray ----

    @Override public TypedValue peekRecord(TypedArray a, int index) { return a.macapkPeekRecord(index); }

    @Override public boolean declaredTypeNull(TypedArray a, int index) { return a.macapkDeclaredTypeNull(index); }
}
