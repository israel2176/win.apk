// ResBackend -- the one door from the rest of the framework into Win.apk's own
// resources plumbing (docs/research/aosp-views.md, step 0; windows/README.md,
// "Changes to the upstream tree").
//
// Win.apk's android.content.res declares macapk* methods and fields (and a few
// unprefixed ones, such as Theme.resolveAttrColor) that AOSP's does not have, and
// its resource table is Win.apk's own com.macapk.loader.ResourceIndex. Code
// outside the resources side reaches them only through this interface, so that
// putting AOSP's android.content.res in needs an implementation of it, not patched
// copies of AOSP classes. ArscRes, the default, calls the member it stands for, one
// for one: going through here changes nothing. With WINAPK_AOSP_RES=1 (the host puts
// lib/aosp-resources.jar first on the class path and sets -Dwinapk.aosp.res=1) the
// classes of android.content.res are AOSP 14's and the backend is that jar's
// com.winapk.aospres.AospRes (windows/java/aosp_res; step R1 of the plan).
//
// The resources side, which keeps calling itself directly: android.content.res
// and its Win.apk helpers in com.macapk.loader (ResourceIndex, ArscReader,
// BinaryXmlParser, DomXmlResourceParser, axml). The resource index is opaque
// here (an Object, null before Boot sets one), as Resources.macapkGetResourceIndex
// always handed it out.
package com.macapk.loader.res;

import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.graphics.drawable.Drawable;
import android.util.DisplayMetrics;
import android.util.TypedValue;

import java.io.FileNotFoundException;
import java.io.InputStream;
import java.util.List;
import java.util.Map;
import java.util.function.Consumer;

public interface ResBackend {

    /** The backend of this JVM: chosen once, for the whole run. */
    static ResBackend get() { return Chosen.BACKEND; }

    /** Which one, decided when first asked. */
    final class Chosen {
        private Chosen() {}

        static final ResBackend BACKEND = choose();

        private static ResBackend choose() {
            if (!"1".equals(System.getProperty("winapk.aosp.res"))) return ArscRes.INSTANCE;
            try {
                return (ResBackend) Class.forName("com.winapk.aospres.AospRes").getField("INSTANCE").get(null);
            } catch (ReflectiveOperationException e) {
                // The host sets the property only with the jar first on the class path, so this
                // is a broken stage: AOSP's android.content.res is there without its backend.
                throw new IllegalStateException("winapk.aosp.res=1 without com.winapk.aospres.AospRes", e);
            }
        }
    }

    // ---- What the loader builds the app's resources with ----

    /** The app's AssetManager over its APK, the framework's resources first (new AssetManager(apkPath)). */
    android.content.res.AssetManager openAssets(String apkPath) throws java.io.IOException;

    /** Pushes the process configuration into the system Resources (Resources.updateSystemConfiguration). */
    void updateSystemConfiguration(Configuration cfg, DisplayMetrics metrics);

    /** A TypedArray with no theme behind it, for a Context whose theme failed (new TypedArray(res, attrs)). */
    TypedArray themelessTypedArray(Resources res, int[] attrs);

    /** The same over an attribute set (new TypedArray(res, attrs, set)). */
    TypedArray themelessTypedArray(Resources res, int[] attrs, android.util.AttributeSet set);

    /** A Resources for a theme to hang from when a Context has none (new Resources(null, null, null)). */
    Resources placeholderResources();

    /** A framework attribute's id by name, 0 if none (ColorStateList.macapkFrameworkAttrId). */
    int frameworkAttrId(String name);

    /** A compiled XML resource (a layout, a menu) as the decoded text Win.apk's inflaters parse
     *  (Resources.openRawResource, which serves the index's text rendering). */
    InputStream openXmlText(Resources res, int id);

    /** After an inflater parsed openXmlText's text into doc: lets the backend tie each element to
     *  its start tag in the compiled file (CompiledXml). Today's backend does nothing. */
    void markCompiledSource(org.w3c.dom.Document doc, Resources res, int id);

    // ---- The app's resource index: open it, publish it, the package ----

    /** Opens the app's resource table for a configuration (new ResourceIndex(cfg, apkPath)). */
    Object openIndex(Configuration cfg, String apkPath);

    /** Whether o is a resource index of this backend (o instanceof ResourceIndex). */
    boolean isIndex(Object o);

    /** The index the process resolves through, or null (Resources.macapkGetResourceIndex). */
    Object resourceIndex();

    /** Publishes the index the process resolves through (Resources.macapkSetResourceIndex). */
    void setResourceIndex(Object index);

    /** Indexes the framework table ahead of use, on the caller's thread (ResourceIndex.prefetchFrameworkTable). */
    void prefetchFrameworkTable();

    /** The app's package name, for its resource lookups (Resources.macapkSetPackageName). */
    void setPackageName(String pkg);

    // ---- Lookups on an index (ResourceIndex's own methods; index first) ----

    /** The theme the index falls back to (ResourceIndex.getApplicationThemeName). */
    String indexApplicationThemeName(Object index);

    /** Sets the theme the index falls back to (ResourceIndex.setApplicationThemeName). */
    void indexSetApplicationThemeName(Object index, String name);

    /** {type, name} of a resource id, or null (ResourceIndex.macapkTypeNameOf). */
    String[] indexTypeNameOf(Object index, int id);

    /** "type/name" of a framework id, or null (ResourceIndex.macapkFwEntryTypeName). */
    String indexFwEntryTypeName(Object index, int id);

    /** A resource id by name, type and package, 0 if none (ResourceIndex.idOfName). */
    int indexIdOfName(Object index, String name, String type, String pkg);

    /** A framework style's id by name (ResourceIndex.idOfFrameworkStyleName). */
    int indexIdOfFrameworkStyleName(Object index, String name);

    /** A string resource by id (ResourceIndex.stringForId). */
    String indexStringForId(Object index, int id);

    /** An integer resource by name (ResourceIndex.integerForName). */
    Integer indexIntegerForName(Object index, String name);

    /** An integer resource by id (ResourceIndex.integerForId). */
    Integer indexIntegerForId(Object index, int id);

    /** A bool resource by id (ResourceIndex.getBoolById). */
    Boolean indexBoolById(Object index, int id);

    /** A color resource by name (ResourceIndex.getColorByName). */
    Integer indexColorByName(Object index, String name);

    /** A dimen's raw text by name (ResourceIndex.getDimenRaw). */
    String indexDimenRaw(Object index, String name);

    /** A colour expression resolved in the index (ResourceIndex.macapkResolveColorExpr). */
    Integer indexResolveColorExpr(Object index, String raw);

    /** An attribute's raw value in a named theme (ResourceIndex.resolveAttrInTheme). */
    String indexResolveAttrInTheme(Object index, String themeName, String attr);

    /** A style's name from an "@0x..." reference (ResourceIndex.styleNameForIdRefPublic). */
    String indexStyleNameForIdRef(Object index, String ref);

    /** A style by name, opaque, or null (ResourceIndex.getStyleByName). */
    Object indexStyleByName(Object index, String name);

    /** A style's parent reference, or null (ResourceIndex.StyleDef.parent). */
    String styleParent(Object style);

    /** A style's items, attribute to raw value (ResourceIndex.StyleDef.items). */
    Map<String, String> styleItems(Object style);

    /** A file resource's bytes by id (ResourceIndex.openRaw). */
    InputStream indexOpenRaw(Object index, int id) throws FileNotFoundException;

    /** A file resource's bytes by type and name (ResourceIndex.openRawByName). */
    InputStream indexOpenRawByName(Object index, String type, String name);

    /** The ids of every file resource of a type (ResourceIndex.macapkFileIdsOfType). */
    List<Integer> indexFileIdsOfType(Object index, String type);

    /** The locales the app's table has resources for (ResourceIndex.macapkResourceLocaleTags). */
    List<String> indexResourceLocaleTags(Object index);

    /** Whether a configuration selects other resources than the index did (ResourceIndex.macapkSelectionChangesFor). */
    boolean indexSelectionChangesFor(Object index, Configuration next);

    /** The app's width / height / smallest-width qualifier thresholds (ResourceIndex.macapkAppSizeBuckets). */
    int[][] indexAppSizeBuckets(Object index);

    /** Whether a dp change crosses one of the thresholds (ResourceIndex.macapkDpCrosses). */
    boolean dpCrosses(int[] thresholds, int firstDp, int secondDp);

    // ---- The current app's table, by id or name (Resources' static lookups) ----

    /** Resources.macapkIntegerForName. */
    Integer integerForName(String name);

    /** Resources.macapkIntegerForId. */
    Integer integerForId(int id);

    /** Resources.macapkStringForId. */
    String stringForId(int id);

    /** Resources.macapkResourceTypeForId. */
    String resourceTypeForId(int id);

    /** Resources.macapkBoolForId. */
    Boolean boolForId(int id);

    /** Resources.macapkColorForId. */
    Integer colorForId(int id);

    /** A colour expression ("#..", "@color/..", "?attr") in the current table (Resources.macapkResolveColorExpr). */
    Integer resolveColorExpr(String raw);

    // ---- Theme colours and values for Win.apk's chrome ----

    /** An attribute of the app's theme as a colour (Resources.macapkThemeColor). */
    int themeColor(String attrName, int fallback);

    /** A shape attribute's corner radius in dp (Resources.macapkThemeCornerDp). */
    float themeCornerDp(String shapeAttrName, float fallbackDp);

    /** A "?attr" expression's raw value in a theme (Resources.macapkThemeAttrRawFor). */
    String themeAttrRawFor(String expr, Resources.Theme theme);

    /** Theme.resolveAttrColor. */
    int resolveAttrColor(Resources.Theme theme, String attrName, int fallback);

    /** Theme.resolveAttrRaw. */
    String resolveAttrRaw(Resources.Theme theme, String attrName);

    /** Theme.macapkResolveAttrRawHonest. */
    String resolveAttrRawHonest(Resources.Theme theme, String attrName);

    /** Whether the theme's attribute-style lane is on (Resources.MACAPK_ATTRSTYLE). */
    boolean attrStyleLane();

    // ---- Drawables, tints, densities ----

    /** A framework drawable by name (Resources.macapkFrameworkDrawableByName). */
    Drawable frameworkDrawableByName(Resources res, String name);

    /** A framework drawable by name, in a theme (Resources.macapkFrameworkDrawableByName). */
    Drawable frameworkDrawableByName(Resources res, String name, Resources.Theme theme);

    /** A tint value as a ColorStateList (Resources.macapkTintListFor). */
    ColorStateList tintListFor(Resources res, String v, Resources.Theme theme);

    /** The density of the file behind an id, 0 if not known (Resources.macapkResourceDensity). */
    int resourceDensity(Resources res, int id);

    // ---- Configuration ----

    /** The phone's default configuration (Configuration.macapkPhoneDefaults). */
    Configuration phoneDefaults();

    /** Recomputes screenLayout from the configuration's dp sizes (Configuration.macapkDeriveScreenLayout). */
    void deriveScreenLayout(Configuration cfg);

    /** Edits the system Resources' configuration in place (Resources.macapkEditSystemConfiguration). */
    void editSystemConfiguration(Consumer<Configuration> edit, DisplayMetrics metrics);

    /** Configuration.CONFIG_* (ActivityInfo's on Android). */
    int CONFIG_LOCALE = Configuration.CONFIG_LOCALE;
    int CONFIG_SCREEN_LAYOUT = Configuration.CONFIG_SCREEN_LAYOUT;
    int CONFIG_SCREEN_SIZE = Configuration.CONFIG_SCREEN_SIZE;
    int CONFIG_SMALLEST_SCREEN_SIZE = Configuration.CONFIG_SMALLEST_SCREEN_SIZE;
    int CONFIG_LAYOUT_DIRECTION = Configuration.CONFIG_LAYOUT_DIRECTION;

    // ---- TypedArray ----

    /** peekValue's record, never null: no value is TYPE_NULL (TypedArray.macapkPeekRecord). */
    TypedValue peekRecord(TypedArray a, int index);

    /** Whether the index was declared @null (TypedArray.macapkDeclaredTypeNull). */
    boolean declaredTypeNull(TypedArray a, int index);
}
