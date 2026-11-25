package org.robolectric.shadows;

import org.robolectric.res.android.CppApkAssets;
import org.robolectric.res.android.Registries;

import java.io.FileDescriptor;
import java.io.IOException;

/**
 * Java bodies for android.content.res.ApkAssets' natives (API 34), on Robolectric's port of
 * libandroidfw's ApkAssets. The framework's own resources: AOSP opens
 * /system/framework/framework-res.apk; android-all carries the same resources.arsc and res/ at its
 * root, so that path is mapped to the jar (-Daospviews.framework). Win.apk would map it to its
 * own framework-res.apk (bin/aosp/framework-res-34.apk).
 */
public final class BridgeApkAssets {
    private BridgeApkAssets() {}

    static String map(String path) {
        if ("/system/framework/framework-res.apk".equals(path)) {
            String f = System.getProperty("aospviews.framework");
            if (f != null) return f;
        }
        return path;
    }

    public static long nativeLoad(int format, String path, int flags, android.content.res.loader.AssetsProvider p)
            throws IOException {
        return ShadowArscApkAssets9.nativeLoad(format, map(path), flags, p);
    }
    public static long nativeLoadFd(int format, FileDescriptor fd, String name, int flags,
            android.content.res.loader.AssetsProvider p) throws IOException {
        return ShadowArscApkAssets9.nativeLoadFd(format, fd, name, flags, p);
    }
    // The port caches loaded ApkAssets for the process (ApkAssetsCache), as the zygote does the
    // framework's: nothing to free.
    public static void nativeDestroy(long p) {}
    public static String nativeGetAssetPath(long p) { return ShadowArscApkAssets9.nativeGetAssetPath(p); }
    public static String nativeGetDebugName(long p) { return ShadowArscApkAssets9.nativeGetAssetPath(p); }
    public static long nativeGetStringBlock(long p) { return ShadowArscApkAssets9.nativeGetStringBlock(p); }
    public static boolean nativeIsUpToDate(long p) { return true; }
    public static long nativeOpenXml(long p, String name) throws IOException { return ShadowArscApkAssets9.nativeOpenXml(p, name); }
    public static android.content.om.OverlayableInfo nativeGetOverlayableInfo(long p, String name) { return null; }
    public static boolean nativeDefinesOverlayable(long p) { return false; }

    static CppApkAssets of(long p) { return Registries.NATIVE_APK_ASSETS_REGISTRY.getNativeObject(p); }
}
