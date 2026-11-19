package org.robolectric.shadows;

import android.content.res.ApkAssets;
import android.content.res.Configuration;
import android.os.ParcelFileDescriptor;
import android.util.SparseArray;
import android.util.TypedValue;

import org.robolectric.res.android.AttributeResolution10;
import org.robolectric.res.android.CppApkAssets;
import org.robolectric.res.android.CppAssetManager2;
import org.robolectric.res.android.Registries;
import org.robolectric.res.android.ResXMLParser;

import java.io.IOException;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;

/**
 * Java bodies for android.content.res.AssetManager's 55 natives (API 34), which RewriteNatives
 * points here. AOSP runs them in libandroidfw (core/jni/android_util_AssetManager.cpp over
 * AssetManager2); here they run on Robolectric's line-by-line Java port of the same code
 * (org.robolectric.res.android), mostly through the static methods Robolectric's own shadow
 * (ShadowArscAssetManager10/14) gives each native. This class sits in the shadows' package only to
 * reach those protected statics. The few that shadow reaches through Robolectric's instrumentation
 * (Shadow.extract of an ApkAssets, ShadowVMRuntime) are written out here against AOSP's own fields.
 */
public final class BridgeAssetManager {
    private BridgeAssetManager() {}


    public static long nativeCreate() { return ShadowArscAssetManager10.nativeCreate(); }
    public static void nativeDestroy(long p) { ShadowArscAssetManager10.nativeDestroy(p); }

    private static Field apkNativePtr;

    /** The shadow finds each ApkAssets' native object through its shadow; AOSP's field holds it. */
    public static void nativeSetApkAssets(long p, ApkAssets[] assets, boolean invalidateCaches) {
        try {
            if (apkNativePtr == null) {
                Field f = ApkAssets.class.getDeclaredField("mNativePtr");
                f.setAccessible(true);
                apkNativePtr = f;
            }
            List<CppApkAssets> list = new ArrayList<>(assets.length);
            for (ApkAssets a : assets) {
                if (a == null) throw new NullPointerException("ApkAssets is null");
                list.add(Registries.NATIVE_APK_ASSETS_REGISTRY.getNativeObject(apkNativePtr.getLong(a)));
            }
            ShadowArscAssetManager10.AssetManagerFromLong(p).SetApkAssets(list, invalidateCaches);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(e);
        }
    }

    public static void nativeSetConfiguration(long p, int mcc, int mnc, String locale, int orientation, int touchscreen,
            int density, int keyboard, int keyboardHidden, int navigation, int screenWidth, int screenHeight,
            int smallestScreenWidthDp, int screenWidthDp, int screenHeightDp, int screenLayout, int uiMode,
            int colorMode, int grammaticalGender, int majorVersion) {
        ShadowArscAssetManager14.nativeSetConfiguration(p, mcc, mnc, locale, orientation, touchscreen, density,
                keyboard, keyboardHidden, navigation, screenWidth, screenHeight, smallestScreenWidthDp,
                screenWidthDp, screenHeightDp, screenLayout, uiMode, colorMode, grammaticalGender, majorVersion);
    }

    public static SparseArray<String> nativeGetAssignedPackageIdentifiers(long p, boolean overlays, boolean loaders) {
        return ShadowArscAssetManager10.nativeGetAssignedPackageIdentifiers(p, overlays, loaders);
    }

    // libandroidfw answers whether a loader added a table at run time; there are no loaders here.
    public static boolean nativeContainsAllocatedTable(long p) { return false; }

    public static String[] nativeList(long p, String path) throws IOException { return ShadowArscAssetManager10.nativeList(p, path); }
    public static long nativeOpenAsset(long p, String name, int mode) throws IOException {
        return ShadowArscAssetManager10.nativeOpenAsset(p, name, mode);
    }
    public static ParcelFileDescriptor nativeOpenAssetFd(long p, String name, long[] out) throws IOException {
        return ShadowArscAssetManager10.nativeOpenAssetFd(p, name, out);
    }
    public static long nativeOpenNonAsset(long p, int cookie, String name, int mode) throws IOException {
        return ShadowArscAssetManager10.nativeOpenNonAsset(p, cookie, name, mode);
    }
    public static ParcelFileDescriptor nativeOpenNonAssetFd(long p, int cookie, String name, long[] out) throws IOException {
        return ShadowArscAssetManager10.nativeOpenNonAssetFd(p, cookie, name, out);
    }
    public static long nativeOpenXmlAsset(long p, int cookie, String name) throws IOException {
        return ShadowArscAssetManager10.nativeOpenXmlAsset(p, cookie, name);
    }
    public static int nativeGetResourceValue(long p, int resid, short density, TypedValue v, boolean resolve) {
        return ShadowArscAssetManager10.nativeGetResourceValue(p, resid, density, v, resolve);
    }
    public static int nativeGetResourceBagValue(long p, int resid, int bagEntry, TypedValue v) {
        return ShadowArscAssetManager10.nativeGetResourceBagValue(p, resid, bagEntry, v);
    }
    public static int[] nativeGetStyleAttributes(long p, int resid) { return ShadowArscAssetManager10.nativeGetStyleAttributes(p, resid); }
    public static String[] nativeGetResourceStringArray(long p, int resid) {
        return ShadowArscAssetManager10.nativeGetResourceStringArray(p, resid);
    }
    public static int[] nativeGetResourceStringArrayInfo(long p, int resid) {
        return ShadowArscAssetManager10.nativeGetResourceStringArrayInfo(p, resid);
    }
    public static int[] nativeGetResourceIntArray(long p, int resid) { return ShadowArscAssetManager10.nativeGetResourceIntArray(p, resid); }
    public static int nativeGetResourceArraySize(long p, int resid) { return ShadowArscAssetManager10.nativeGetResourceArraySize(p, resid); }
    public static int nativeGetResourceArray(long p, int resid, int[] out) {
        return ShadowArscAssetManager10.nativeGetResourceArray(p, resid, out);
    }
    public static int nativeGetResourceIdentifier(long p, String name, String type, String pkg) {
        return ShadowArscAssetManager10.nativeGetResourceIdentifier(p, name, type, pkg);
    }
    public static String nativeGetResourceName(long p, int resid) { return ShadowArscAssetManager10.nativeGetResourceName(p, resid); }
    public static String nativeGetResourcePackageName(long p, int resid) {
        return ShadowArscAssetManager10.nativeGetResourcePackageName(p, resid);
    }
    public static String nativeGetResourceTypeName(long p, int resid) { return ShadowArscAssetManager10.nativeGetResourceTypeName(p, resid); }
    public static String nativeGetResourceEntryName(long p, int resid) { return ShadowArscAssetManager10.nativeGetResourceEntryName(p, resid); }
    public static String[] nativeGetLocales(long p, boolean excludeSystem) { return ShadowArscAssetManager10.nativeGetLocales(p, excludeSystem); }
    public static Configuration[] nativeGetSizeConfigurations(long p) { return ShadowArscAssetManager10.nativeGetSizeConfigurations(p); }
    // API 34 adds uiMode to the same list; the port knows only the size one, which is what it holds.
    public static Configuration[] nativeGetSizeAndUiModeConfigurations(long p) {
        return ShadowArscAssetManager10.nativeGetSizeConfigurations(p);
    }
    public static void nativeSetResourceResolutionLoggingEnabled(long p, boolean on) {}
    public static String nativeGetLastResourceResolution(long p) { return null; }
    public static int[] nativeAttributeResolutionStack(long p, long theme, int xmlStyle, int defStyleAttr, int defStyleRes) {
        return new int[0];
    }

    /** As the shadow's, but the two output arrays come by address from our VMRuntime stand-in. */
    public static void nativeApplyStyle(long p, long themePtr, int defStyleAttr, int defStyleRes, long xmlParserPtr,
            int[] inAttrs, long outValuesAddress, long outIndicesAddress) {
        CppAssetManager2 am = ShadowArscAssetManager10.AssetManagerFromLong(p);
        CppAssetManager2.Theme theme = Registries.NATIVE_THEME9_REGISTRY.getNativeObject(themePtr);
        if (theme.GetAssetManager() != am) throw new IllegalStateException("theme of another AssetManager");
        ResXMLParser xml = xmlParserPtr == 0 ? null : Registries.NATIVE_RES_XML_PARSERS.getNativeObject(xmlParserPtr);
        int[] outValues = (int[]) dalvik.system.VMRuntime.objectForAddress(outValuesAddress);
        int[] outIndices = (int[]) dalvik.system.VMRuntime.objectForAddress(outIndicesAddress);
        AttributeResolution10.ApplyStyle(theme, xml, defStyleAttr, defStyleRes, inAttrs, inAttrs.length, outValues,
                outIndices);
    }

    public static boolean nativeResolveAttrs(long p, long theme, int defStyleAttr, int defStyleRes, int[] inValues,
            int[] inAttrs, int[] outValues, int[] outIndices) {
        return ShadowArscAssetManager10.nativeResolveAttrs(p, theme, defStyleAttr, defStyleRes, inValues, inAttrs,
                outValues, outIndices);
    }
    public static boolean nativeRetrieveAttributes(long p, long xmlParser, int[] inAttrs, int[] outValues, int[] outIndices) {
        return ShadowArscAssetManager10.nativeRetrieveAttributes(p, xmlParser, inAttrs, outValues, outIndices);
    }
    public static long nativeThemeCreate(long p) { return ShadowArscAssetManager10.nativeThemeCreate(p); }
    // AOSP frees a theme through NativeAllocationRegistry with this C function; the port's themes
    // live in a Java registry instead. 0 = nothing to call (our NativeAllocationRegistry skips it),
    // so a theme stays in that registry: a leak per Theme until a real free function exists.
    public static long nativeGetThemeFreeFunction() { return 0; }
    public static void nativeThemeApplyStyle(long p, long theme, int resid, boolean force) {
        ShadowArscAssetManager10.nativeThemeApplyStyle(p, theme, resid, force);
    }
    /** AssetManager2's Theme::Rebase: clear, then the styles again in order. */
    public static void nativeThemeRebase(long p, long theme, int[] styleIds, boolean[] force, int count) {
        ShadowArscAssetManager10.nativeThemeClear(theme);
        for (int i = 0; i < count; i++) ShadowArscAssetManager10.nativeThemeApplyStyle(p, theme, styleIds[i], force[i]);
    }
    public static void nativeThemeCopy(long dstAm, long dst, long srcAm, long src) {
        ShadowArscAssetManager10.nativeThemeCopy(dstAm, dst, srcAm, src);
    }
    public static int nativeThemeGetAttributeValue(long p, long theme, int resid, TypedValue v, boolean resolve) {
        return ShadowArscAssetManager10.nativeThemeGetAttributeValue(p, theme, resid, v, resolve);
    }
    public static void nativeThemeDump(long p, long theme, int prio, String tag, String prefix) {
        ShadowArscAssetManager10.nativeThemeDump(p, theme, prio, tag, prefix);
    }
    public static int nativeThemeGetChangingConfigurations(long theme) {
        return ShadowArscAssetManager10.nativeThemeGetChangingConfigurations(theme);
    }
    // AssetManager2::GetParentThemeResourceId is newer than the port: no parent known.
    public static int nativeGetParentThemeIdentifier(long p, int resid) { return 0; }

    public static void nativeAssetDestroy(long a) { ShadowArscAssetManager10.nativeAssetDestroy(a); }
    public static int nativeAssetReadChar(long a) { return ShadowArscAssetManager10.nativeAssetReadChar(a); }
    public static int nativeAssetRead(long a, byte[] b, int off, int len) throws IOException {
        return ShadowArscAssetManager10.nativeAssetRead(a, b, off, len);
    }
    public static long nativeAssetSeek(long a, long off, int whence) { return ShadowArscAssetManager10.nativeAssetSeek(a, off, whence); }
    public static long nativeAssetGetLength(long a) { return ShadowArscAssetManager10.nativeAssetGetLength(a); }
    public static long nativeAssetGetRemainingLength(long a) { return ShadowArscAssetManager10.nativeAssetGetRemainingLength(a); }
    // Overlays (RROs) are a system feature Win.apk does not have.
    public static Map nativeGetOverlayableMap(long p, String pkg) { return Collections.emptyMap(); }
    public static String nativeGetOverlayablesToString(long p, String pkg) { return ""; }
    public static int getGlobalAssetCount() { return ShadowArscAssetManager10.getGlobalAssetCount(); }
    public static String getAssetAllocations() { return ShadowArscAssetManager10.getAssetAllocations(); }
    public static int getGlobalAssetManagerCount() { return ShadowArscAssetManager10.getGlobalAssetManagerCount(); }
}
