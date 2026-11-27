// NativeApkAssets -- the bodies of android.content.res.ApkAssets' natives (API 34), which
// build_aosp_res.sh's rewrite (AospResRewrite.java) points here: libandroidfw's ApkAssets
// (core/jni/android_content_res_ApkAssets.cpp), run on Robolectric's Java port of libandroidfw
// (org.robolectric.res.android). Adapted from Robolectric 4.16's ShadowArscApkAssets9
// (Apache-2.0; THIRD_PARTY_NOTICES.txt), without its shadow plumbing: AOSP's own Java holds the
// handle (mNativePtr), so nothing here reaches into an instance.
package com.winapk.aospres;

import static org.robolectric.res.android.Errors.NO_ERROR;

import android.content.om.OverlayableInfo;
import android.content.res.ApkAssets;
import android.content.res.loader.AssetsProvider;

import java.io.FileDescriptor;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.HashMap;
import java.util.Map;

import org.robolectric.res.android.Asset;
import org.robolectric.res.android.CppApkAssets;
import org.robolectric.res.android.Registries;
import org.robolectric.res.android.ResXMLTree;

public final class NativeApkAssets {
    private NativeApkAssets() {}

    // ApkAssets.PROPERTY_OVERLAY is private.
    private static final int PROPERTY_OVERLAY = 1 << 3;

    // The port's tables are Java objects that take a second to build for the framework's 16 MB
    // table, so each file is loaded once per process and kept, as the zygote keeps the
    // framework's: ApkAssets.nativeDestroy frees nothing (Robolectric's ApkAssetsCache does the
    // same).
    private static final Map<String, Long> LOADED = new HashMap<>();

    public static long nativeLoad(int format, String path, int flags, AssetsProvider provider) throws IOException {
        if (path == null) return 0;
        boolean system = (flags & ApkAssets.PROPERTY_SYSTEM) != 0;
        boolean overlay = (flags & PROPERTY_OVERLAY) != 0;
        boolean sharedLib = (flags & ApkAssets.PROPERTY_DYNAMIC) != 0;
        // An installed APK by either of its names: the loader opens the app's set by the host
        // path it was started with (AospRes.openAssets), AOSP's ResourcesManager by
        // ApplicationInfo.sourceDir, its Android name; one file, one table.
        String key = com.macapk.loader.InstalledSplits.androidPath(path) + "|" + system + "|" + overlay + "|"
                + sharedLib;
        synchronized (LOADED) {
            Long have = LOADED.get(key);
            if (have != null) return have;
            long t0 = System.nanoTime();
            CppApkAssets a = overlay ? CppApkAssets.LoadOverlay(path, system)
                    : sharedLib ? CppApkAssets.LoadAsSharedLibrary(path, system)
                    : CppApkAssets.Load(path, system);
            if (a == null) throw new IOException("Failed to load asset path " + path);
            long ptr = Registries.NATIVE_APK_ASSETS_REGISTRY.register(a);
            LOADED.put(key, ptr);
            AospRes.log(String.format("resource table %s loaded in %.0f ms", path, (System.nanoTime() - t0) / 1e6));
            return ptr;
        }
    }

    public static long nativeLoadFd(int format, FileDescriptor fd, String name, int flags, AssetsProvider provider)
            throws IOException {
        CppApkAssets a = CppApkAssets.loadArscFromFd(fd);
        if (a == null) throw new IOException("Failed to load from the file descriptor " + fd);
        return Registries.NATIVE_APK_ASSETS_REGISTRY.register(a);
    }

    public static long nativeLoadFdOffsets(int format, FileDescriptor fd, String name, long offset, long length,
            int flags, AssetsProvider provider) throws IOException {
        throw new IOException("ApkAssets from part of a file descriptor: not supported (" + name + ")");
    }

    public static void nativeDestroy(long ptr) {}

    public static String nativeGetAssetPath(long ptr) {
        return Registries.NATIVE_APK_ASSETS_REGISTRY.getNativeObject(ptr).GetPath();
    }

    public static String nativeGetDebugName(long ptr) { return nativeGetAssetPath(ptr); }

    public static long nativeGetStringBlock(long ptr) {
        return Registries.NATIVE_APK_ASSETS_REGISTRY.getNativeObject(ptr).GetLoadedArsc().GetStringPool()
                .getNativePtr();
    }

    public static boolean nativeIsUpToDate(long ptr) { return true; }

    public static long nativeOpenXml(long ptr, String fileName) throws IOException {
        if (fileName == null) return 0;
        CppApkAssets a = Registries.NATIVE_APK_ASSETS_REGISTRY.getNativeObject(ptr);
        Asset asset = a.Open(fileName, Asset.AccessMode.ACCESS_RANDOM);
        if (asset == null) throw new FileNotFoundException(fileName);
        // No DynamicRefTable: an XML file opened straight from an ApkAssets has no notion of
        // the package's references (as libandroidfw's).
        ResXMLTree tree = new ResXMLTree(null);
        if (tree.setTo(asset.getBuffer(true), (int) asset.getLength(), true) != NO_ERROR) {
            throw new FileNotFoundException("Corrupt XML binary file");
        }
        return Registries.NATIVE_RES_XML_TREES.register(tree);
    }

    // Overlays (runtime resource overlays) are a system feature Win.apk does not have.
    public static OverlayableInfo nativeGetOverlayableInfo(long ptr, String name) { return null; }

    public static boolean nativeDefinesOverlayable(long ptr) { return false; }
}
