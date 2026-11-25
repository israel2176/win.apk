// NativeAssetManager -- the bodies of android.content.res.AssetManager's natives (API 34), which
// build_aosp_res.sh's rewrite (AospResRewrite.java) points here. On Android they are libandroidfw's
// AssetManager2, AttributeResolution and Theme behind core/jni/android_util_AssetManager.cpp; here
// they run on Robolectric's line-by-line Java port of that code (org.robolectric.res.android).
// Adapted from Robolectric 4.16's ShadowArscAssetManager10 and ShadowArscAssetManager14 (Apache-2.0;
// THIRD_PARTY_NOTICES.txt) without the shadow plumbing: AOSP's own Java holds each handle, the
// framework's ApkAssets is opened by AospRes, and TypedArray's two output arrays come back from
// their addresses through Compat (Win.apk's VMRuntime has no native heap to point into).
package com.winapk.aospres;

import static org.robolectric.res.android.ApkAssetsCookie.K_INVALID_COOKIE;
import static org.robolectric.res.android.ApkAssetsCookie.kInvalidCookie;
import static org.robolectric.res.android.Asset.SEEK_CUR;
import static org.robolectric.res.android.Asset.SEEK_END;
import static org.robolectric.res.android.Asset.SEEK_SET;
import static org.robolectric.res.android.AttributeResolution10.ApplyStyle;
import static org.robolectric.res.android.AttributeResolution10.ResolveAttrs;
import static org.robolectric.res.android.AttributeResolution10.RetrieveAttributes;
import static org.robolectric.res.android.Errors.NO_ERROR;
import static org.robolectric.res.android.Registries.NATIVE_RES_XML_PARSERS;
import static org.robolectric.res.android.Registries.NATIVE_RES_XML_TREES;
import static org.robolectric.res.android.Util.ATRACE_NAME;
import static org.robolectric.res.android.Util.CHECK;
import static org.robolectric.res.android.Util.JNI_FALSE;
import static org.robolectric.res.android.Util.JNI_TRUE;
import static org.robolectric.res.android.Util.isTruthy;

import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.util.SparseArray;
import android.util.TypedValue;
import java.io.FileDescriptor;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import org.robolectric.res.android.ApkAssetsCookie;
import org.robolectric.res.android.Asset;
import org.robolectric.res.android.AssetDir;
import org.robolectric.res.android.CppApkAssets;
import org.robolectric.res.android.CppAssetManager;
import org.robolectric.res.android.CppAssetManager2;
import org.robolectric.res.android.CppAssetManager2.ResolvedBag;
import org.robolectric.res.android.CppAssetManager2.ResourceName;
import org.robolectric.res.android.CppAssetManager2.Theme;
import org.robolectric.res.android.DynamicRefTable;
import org.robolectric.res.android.Ref;
import org.robolectric.res.android.Registries;
import org.robolectric.res.android.ResStringPool;
import org.robolectric.res.android.ResTable_config;
import org.robolectric.res.android.ResXMLParser;
import org.robolectric.res.android.ResXMLTree;
import org.robolectric.res.android.ResourceTypes.Res_value;
import org.robolectric.util.PerfStatsCollector;

public final class NativeAssetManager {
  private NativeAssetManager() {}


  // Offsets into the outValues array populated by the methods below. outValues is a uint32_t
  // array, but each logical element takes up 7 uint32_t-sized physical elements.
  // Keep these in sync with android.content.res.TypedArray java class
  private static final int STYLE_NUM_ENTRIES = 7;
  private static final int STYLE_TYPE = 0;
  private static final int STYLE_DATA = 1;
  private static final int STYLE_ASSET_COOKIE = 2;
  private static final int STYLE_RESOURCE_ID = 3;
  private static final int STYLE_CHANGING_CONFIGURATIONS = 4;
  private static final int STYLE_DENSITY = 5;
  private static final int STYLE_SOURCE_STYLE_RESOURCE_ID = 6;


  //  @RealObject  //  protected AssetManager realObject;

  // #define ATRACE_TAG ATRACE_TAG_RESOURCES
  // #define LOG_TAG "asset"
  //
  // #include <inttypes.h>
  // #include <linux/capability.h>
  // #include <stdio.h>
  // #include <sys/stat.h>
  // #include <sys/system_properties.h>
  // #include <sys/types.h>
  // #include <sys/wait.h>
  //
  // #include <private/android_filesystem_config.h> // for AID_SYSTEM
  //
  // #include "android-base/logging.h"
  // #include "android-base/properties.h"
  // #include "android-base/stringprintf.h"
  // #include "android_runtime/android_util_AssetManager.h"
  // #include "android_runtime/AndroidRuntime.h"
  // #include "android_util_Binder.h"
  // #include "androidfw/Asset.h"
  // #include "androidfw/AssetManager.h"
  // #include "androidfw/AssetManager2.h"
  // #include "androidfw/AttributeResolution.h"
  // #include "androidfw/MutexGuard.h"
  // #include "androidfw/ResourceTypes.h"
  // #include "core_jni_helpers.h"
  // #include "jni.h"
  // #include "nativehelper/JNIHelp.h"
  // #include "nativehelper/ScopedPrimitiveArray.h"
  // #include "nativehelper/ScopedStringChars.h"
  // #include "nativehelper/String.h"
  // #include "utils/Log.h"
  // #include "utils/misc.h"
  // #include "utils/String.h"
  // #include "utils/Trace.h"
  //
  // extern "C" int capget(cap_user_header_t hdrp, cap_user_data_t datap);
  // extern "C" int capset(cap_user_header_t hdrp, const cap_user_data_t datap);
  //
  // using ::android::base::StringPrintf;
  //
  // namespace android {
  //
  // // ----------------------------------------------------------------------------
  //

  // static class typedvalue_offsets_t {
  //   jfieldID mType;
  //   jfieldID mData;
  //   jfieldID mString;
  //   jfieldID mAssetCookie;
  //   jfieldID mResourceId;
  //   jfieldID mChangingConfigurations;
  //   jfieldID mDensity;
  // }
  // static final typedvalue_offsets_t gTypedValueOffsets = new typedvalue_offsets_t();
  //
  // static class assetfiledescriptor_offsets_t {
  //   jfieldID mFd;
  //   jfieldID mStartOffset;
  //   jfieldID mLength;
  // }
  // static final assetfiledescriptor_offsets_t gAssetFileDescriptorOffsets = new
  // assetfiledescriptor_offsets_t();
  //
  // static class assetmanager_offsets_t
  // {
  //   jfieldID mObject;
  // };
  // // This is also used by asset_manager.cpp.
  // static final assetmanager_offsets_t gAssetManagerOffsets = new assetmanager_offsets_t();
  //
  // static class apkassetsfields {
  //   jfieldID native_ptr;
  // }
  // static final apkassetsfields gApkAssetsFields = new apkassetsfields();
  //
  // static class sparsearray_offsets_t {
  //   jclass classObject;
  //   jmethodID constructor;
  //   jmethodID put;
  // }
  // static final sparsearray_offsets_t gSparseArrayOffsets = new sparsearray_offsets_t();
  //
  // static class configuration_offsets_t {
  //   jclass classObject;
  //   jmethodID constructor;
  //   jfieldID mSmallestScreenWidthDpOffset;
  //   jfieldID mScreenWidthDpOffset;
  //   jfieldID mScreenHeightDpOffset;
  // }
  // static final configuration_offsets_t gConfigurationOffsets = new configuration_offsets_t();
  //
  // jclass g_stringClass = nullptr;
  //
  // // ----------------------------------------------------------------------------


  // Java asset cookies have 0 as an invalid cookie, but TypedArray expects < 0.
  static int ApkAssetsCookieToJavaCookie(ApkAssetsCookie cookie) {
    return cookie.intValue() != kInvalidCookie ? (cookie.intValue() + 1) : -1;
  }

  static ApkAssetsCookie JavaCookieToApkAssetsCookie(int cookie) {
    return ApkAssetsCookie.forInt(cookie > 0 ? (cookie - 1) : kInvalidCookie);
  }


  // This is called by zygote (running as user root) as part of preloadResources.
  // static void NativeVerifySystemIdmaps(JNIEnv* /*env*/, jclass /*clazz*/) {


  static int CopyValue(
      /*JNIEnv* env,*/ ApkAssetsCookie cookie,
      Res_value value,
      int ref,
      int type_spec_flags,
      ResTable_config config,
      TypedValue out_typed_value) {
    out_typed_value.type = value.dataType;
    out_typed_value.assetCookie = ApkAssetsCookieToJavaCookie(cookie);
    out_typed_value.data = value.data;
    out_typed_value.string = null;
    out_typed_value.resourceId = ref;
    out_typed_value.changingConfigurations = type_spec_flags;
    if (config != null) {
      out_typed_value.density = config.density;
    }
    return ApkAssetsCookieToJavaCookie(cookie);
  }

  //  @Override
  //  protected int addAssetPathNative(String path) {
  //    throw new UnsupportedOperationException(); // todo
  //  }



  // ----------------------------------------------------------------------------

  // interface AAssetManager {}
  //
  // // Let the opaque type AAssetManager refer to a guarded AssetManager2 instance.
  // static class GuardedAssetManager implements AAssetManager {
  //   CppAssetManager2 guarded_assetmanager = new CppAssetManager2();
  // }

  static CppAssetManager2 NdkAssetManagerForJavaObject(
      /* JNIEnv* env,*/ AssetManager jassetmanager) {
    // long assetmanager_handle = env.GetLongField(jassetmanager, gAssetManagerOffsets.mObject);
    long assetmanager_handle = Compat.longField(jassetmanager, "mObject");
    CppAssetManager2 am =
        Registries.NATIVE_ASSET_MANAGER_REGISTRY.getNativeObject(assetmanager_handle);
    if (am == null) {
      throw new IllegalStateException("AssetManager has been finalized!");
    }
    return am;
  }

  static CppAssetManager2 AssetManagerForJavaObject(/* JNIEnv* env,*/ AssetManager jassetmanager) {
    return NdkAssetManagerForJavaObject(jassetmanager);
  }

  static CppAssetManager2 AssetManagerFromLong(long ptr) {
    // return *AssetManagerForNdkAssetManager(reinterpret_cast<AAssetManager>(ptr));
    return Registries.NATIVE_ASSET_MANAGER_REGISTRY.getNativeObject(ptr);
  }

  static ParcelFileDescriptor ReturnParcelFileDescriptor(
      /* JNIEnv* env,*/ Asset asset, long[] out_offsets) throws FileNotFoundException {
    final Ref<Long> start_offset = new Ref<>(0L);
    final Ref<Long> length = new Ref<>(0L);
    FileDescriptor fd = asset.openFileDescriptor(start_offset, length);
    // asset.reset();

    if (fd == null) {
      throw new FileNotFoundException(
          "This file can not be opened as a file descriptor; it is probably compressed");
    }

    long[] offsets =
        out_offsets; // reinterpret_cast<long*>(env.GetPrimitiveArrayCritical(out_offsets, 0));
    if (offsets == null) {
      // close(fd);
      return null;
    }

    offsets[0] = start_offset.get();
    offsets[1] = length.get();

    // env.ReleasePrimitiveArrayCritical(out_offsets, offsets, 0);

    // jniCreateFileDescriptor(env, fd);
    // if (file_desc == null) {
    //   close(fd);
    //   return null;
    // }

    // TODO: consider doing this
    // return new ParcelFileDescriptor(file_desc);
    return ParcelFileDescriptor.open(asset.getFile(), ParcelFileDescriptor.MODE_READ_ONLY);
  }


  // static jint NativeGetGlobalAssetCount(JNIEnv* /*env*/, jobject /*clazz*/) {
  public static int getGlobalAssetCount() {
    return Asset.getGlobalCount();
  }

  // static jobject NativeGetAssetAllocations(JNIEnv* env, jobject /*clazz*/) {
  public static String getAssetAllocations() {
    String alloc = Asset.getAssetAllocations();
    if (alloc.length() <= 0) {
      return null;
    }
    return alloc;
  }

  // static jint NativeGetGlobalAssetManagerCount(JNIEnv* /*env*/, jobject /*clazz*/) {
  public static int getGlobalAssetManagerCount() {
    // TODO(adamlesinski): Switch to AssetManager2.
    return CppAssetManager.getGlobalCount();
  }

  // static jlong NativeCreate(JNIEnv* /*env*/, jclass /*clazz*/) {
  public static long nativeCreate() {
    return Registries.NATIVE_ASSET_MANAGER_REGISTRY.register(new CppAssetManager2());
  }

  // static void NativeDestroy(JNIEnv* /*env*/, jclass /*clazz*/, jlong ptr) {
  public static void nativeDestroy(long ptr) {
    Registries.NATIVE_ASSET_MANAGER_REGISTRY.unregister(ptr);
  }

  // static void NativeSetApkAssets(JNIEnv* env, jclass /*clazz*/, jlong ptr,
  //                                jobjectArray apk_assets_array, jboolean invalidate_caches) {
  public static void nativeSetApkAssets(
      long ptr,
      android.content.res.ApkAssets[] apk_assets_array,
      boolean invalidate_caches) {
    ATRACE_NAME("AssetManager::SetApkAssets");

    int apk_assets_len = apk_assets_array.length;
    List<CppApkAssets> apk_assets = new ArrayList<>();
    // apk_assets.reserve(apk_assets_len);
    for (int i = 0; i < apk_assets_len; i++) {
      android.content.res.ApkAssets apkAssets =
          apk_assets_array[i]; // env.GetObjectArrayElement(apk_assets_array, i);
      if (apkAssets == null) {
        throw new NullPointerException(String.format("ApkAssets at index %d is null", i));
      }

      long apk_assets_native_ptr = Compat.longField(apkAssets, "mNativePtr");
      // if (env.ExceptionCheck()) {
      //   return;
      // }
      apk_assets.add(Registries.NATIVE_APK_ASSETS_REGISTRY.getNativeObject(apk_assets_native_ptr));
    }

    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    assetmanager.SetApkAssets(apk_assets, invalidate_caches);
  }

  // static void NativeSetConfiguration(JNIEnv* env, jclass /*clazz*/, jlong ptr, jint mcc, jint
  // mnc,
  //                                    jstring locale, jint orientation, jint touchscreen, jint
  // density,
  //                                    jint keyboard, jint keyboard_hidden, jint navigation,
  //                                    jint screen_width, jint screen_height,
  //                                    jint smallest_screen_width_dp, jint screen_width_dp,
  //                                    jint screen_height_dp, jint screen_layout, jint ui_mode,
  //                                    jint color_mode, jint major_version) {

  // static jobject NativeGetAssignedPackageIdentifiers(JNIEnv* env, jclass /*clazz*/, jlong ptr) {

  // API 34 passes the grammatical gender too; the port's ResTable_config has no field for it, so
  // it selects nothing (a -feminine qualifier never matches), as in Robolectric.
  public static void nativeSetConfiguration(long ptr, int mcc, int mnc, String locale, int orientation,
      int touchscreen, int density, int keyboard, int keyboard_hidden, int navigation, int screen_width,
      int screen_height, int smallest_screen_width_dp, int screen_width_dp, int screen_height_dp,
      int screen_layout, int ui_mode, int color_mode, int grammaticalGender, int major_version) {
    ResTable_config configuration = new ResTable_config();
    configuration.mcc = (short) (mcc);
    configuration.mnc = (short) (mnc);
    configuration.orientation = (byte) (orientation);
    configuration.touchscreen = (byte) (touchscreen);
    configuration.density = (short) (density);
    configuration.keyboard = (byte) (keyboard);
    configuration.inputFlags = (byte) (keyboard_hidden);
    configuration.navigation = (byte) (navigation);
    configuration.screenWidth = (short) (screen_width);
    configuration.screenHeight = (short) (screen_height);
    configuration.smallestScreenWidthDp = (short) (smallest_screen_width_dp);
    configuration.screenWidthDp = (short) (screen_width_dp);
    configuration.screenHeightDp = (short) (screen_height_dp);
    configuration.screenLayout = (byte) (screen_layout);
    configuration.uiMode = (byte) (ui_mode);
    configuration.colorMode = (byte) (color_mode);
    configuration.sdkVersion = (short) (major_version);
    if (locale != null) {
      configuration.setBcp47Locale(locale);
    }
    // Java keeps the round qualifier in screenLayout's bits 8-9; the C++ in screenLayout2.
    configuration.screenLayout2 = (byte) ((screen_layout & 0x300) >> 8);
    AssetManagerFromLong(ptr).SetConfiguration(configuration);
  }

  public static SparseArray<String> nativeGetAssignedPackageIdentifiers(
      long ptr, boolean includeOverlays, boolean includeLoaders) {
    SparseArray<String> sparse_array = new SparseArray<>();
    AssetManagerFromLong(ptr).ForEachPackage(
        (String package_name, byte package_id) -> sparse_array.put(package_id, package_name));
    return sparse_array;
  }

  // Whether a ResourcesLoader added a table at run time: Win.apk has no loaders.
  public static boolean nativeContainsAllocatedTable(long ptr) { return false; }

  // API 34 adds the uiMode configurations to the same list; the port knows the size ones.
  public static Configuration[] nativeGetSizeAndUiModeConfigurations(long ptr) {
    return nativeGetSizeConfigurations(ptr);
  }

  public static void nativeSetResourceResolutionLoggingEnabled(long ptr, boolean enabled) {}

  public static String nativeGetLastResourceResolution(long ptr) { return null; }

  public static int[] nativeAttributeResolutionStack(long ptr, long themePtr, int xmlStyle, int defStyleAttr,
      int defStyleRes) {
    return new int[0];
  }

  // AOSP frees a theme through NativeAllocationRegistry with this C function. The port's themes
  // live in a Java registry, so the "function" is Java code registered with Win.apk's
  // NativeAllocationRegistry (aosp-graphics.jar) under a token: once a Theme is unreachable its
  // entry leaves the registry, as NativeThemeDestroy deletes it on ART. They used to stay, one per
  // Theme ever made: thousands in a minute of an app, 20 MB and more of the Java heap.
  private static final long THEME_FREE_TOKEN = 1L;  // no C function lives at address 1
  private static final long THEME_FREE = registerThemeFree();

  private static long registerThemeFree() {
    try {
      Class.forName("libcore.util.NativeAllocationRegistry")
          .getMethod("registerJavaFreeFunction", long.class, java.util.function.LongConsumer.class)
          .invoke(null, THEME_FREE_TOKEN,
              (java.util.function.LongConsumer) p -> Registries.NATIVE_THEME9_REGISTRY.unregister(p));
      return THEME_FREE_TOKEN;
    } catch (ReflectiveOperationException | RuntimeException e) {
      return 0;  // a registry without Java free functions: themes stay, as before
    }
  }

  public static long nativeGetThemeFreeFunction() { return THEME_FREE; }

  /** AssetManager2's Theme::Rebase: the theme cleared, then the styles applied again in order. */
  public static void nativeThemeRebase(long ptr, long themePtr, int[] styleIds, boolean[] force, int count) {
    Registries.NATIVE_THEME9_REGISTRY.getNativeObject(themePtr).Clear();
    for (int i = 0; i < count; i++) {
      nativeThemeApplyStyle(ptr, themePtr, styleIds[i], force[i]);
    }
  }

  // AssetManager2::GetParentThemeResourceId is newer than the port: no parent known.
  public static int nativeGetParentThemeIdentifier(long ptr, int resid) { return 0; }

  // Overlays (runtime resource overlays) are a system feature Win.apk does not have.
  public static java.util.Map<String, String> nativeGetOverlayableMap(long ptr, String packageName) {
    return java.util.Collections.emptyMap();
  }

  public static String nativeGetOverlayablesToString(long ptr, String packageName) { return ""; }

  /** An asset path as APK entries spell it. On Windows java.io.File joins segments with a
   *  backslash, and engines that build asset paths with File (libGDX's FileHandle) hand those to
   *  open() and list(); on a device the separator is '/', the only one zip entries use (Win.apk's
   *  own AssetManager does the same, macapkEntryPath). */
  private static String entryPath(String path) {
    return path.indexOf('\\') >= 0 ? path.replace('\\', '/') : path;
  }

  // static jobjectArray NativeList(JNIEnv* env, jclass /*clazz*/, jlong ptr, jstring path) {
  public static String[] nativeList(long ptr, String path)
      throws IOException {
    if (path == null) {
      // This will throw NPE.
      return null;
    }

    path = entryPath(path);
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    AssetDir asset_dir = assetmanager.OpenDir(path);
    if (asset_dir == null) {
      throw new FileNotFoundException(path);
    }

    int file_count = asset_dir.getFileCount();

    String[] array = new String[file_count]; // env.NewObjectArray(file_count, g_stringClass, null);
    // if (array == null) {
    //   return null;
    // }

    for (int i = 0; i < file_count; i++) {
      String java_string = asset_dir.getFileName(i).string();

      // Check for errors creating the strings (if malformed or no memory).
      // if (env.ExceptionCheck()) {
      //   return null;
      // }

      // env.SetObjectArrayElement(array, i, java_string);
      array[i] = java_string;

      // If we have a large amount of string in our array, we might overflow the
      // local reference table of the VM.
      // env.DeleteLocalRef(java_string);
    }
    return array;
  }

  // static jlong NativeOpenAsset(JNIEnv* env, jclass /*clazz*/, jlong ptr, jstring asset_path,
  //                              jint access_mode) {
  public static long nativeOpenAsset(long ptr, String asset_path, int access_mode)
      throws FileNotFoundException {
    if (asset_path == null) {
      // This will throw NPE.
      return 0;
    }

    asset_path = entryPath(asset_path);
    ATRACE_NAME(String.format("AssetManager::OpenAsset(%s)", asset_path));

    if (access_mode != Asset.AccessMode.ACCESS_UNKNOWN.mode()
        && access_mode != Asset.AccessMode.ACCESS_RANDOM.mode()
        && access_mode != Asset.AccessMode.ACCESS_STREAMING.mode()
        && access_mode != Asset.AccessMode.ACCESS_BUFFER.mode()) {
      throw new IllegalArgumentException("Bad access mode");
    }

    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    Asset asset = assetmanager.Open(asset_path, Asset.AccessMode.fromInt(access_mode));
    if (!isTruthy(asset)) {
      throw new FileNotFoundException(asset_path);
    }
    return Registries.NATIVE_ASSET_REGISTRY.register(asset);
  }

  // static jobject NativeOpenAssetFd(JNIEnv* env, jclass /*clazz*/, jlong ptr, jstring asset_path,
  //                                  jlongArray out_offsets) {
  public static ParcelFileDescriptor nativeOpenAssetFd(
      long ptr, String asset_path, long[] out_offsets) throws IOException {
    if (asset_path == null) {
      // This will throw NPE.
      return null;
    }

    asset_path = entryPath(asset_path);
    ATRACE_NAME(String.format("AssetManager::OpenAssetFd(%s)", asset_path));

    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    Asset asset = assetmanager.Open(asset_path, Asset.AccessMode.ACCESS_RANDOM);
    if (!isTruthy(asset)) {
      throw new FileNotFoundException(asset_path);
    }
    return ReturnParcelFileDescriptor(asset, out_offsets);
  }

  // static jlong NativeOpenNonAsset(JNIEnv* env, jclass /*clazz*/, jlong ptr, jint jcookie,
  //                                 jstring asset_path, jint access_mode) {
  public static long nativeOpenNonAsset(
      long ptr, int jcookie, String asset_path, int access_mode)
      throws FileNotFoundException {
    ApkAssetsCookie cookie = JavaCookieToApkAssetsCookie(jcookie);
    if (asset_path == null) {
      // This will throw NPE.
      return 0;
    }

    ATRACE_NAME(String.format("AssetManager::OpenNonAsset(%s)", asset_path));

    if (access_mode != Asset.AccessMode.ACCESS_UNKNOWN.mode()
        && access_mode != Asset.AccessMode.ACCESS_RANDOM.mode()
        && access_mode != Asset.AccessMode.ACCESS_STREAMING.mode()
        && access_mode != Asset.AccessMode.ACCESS_BUFFER.mode()) {
      throw new IllegalArgumentException("Bad access mode");
    }

    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    Asset asset;
    if (cookie.intValue() != kInvalidCookie) {
      asset = assetmanager.OpenNonAsset(asset_path, cookie, Asset.AccessMode.fromInt(access_mode));
    } else {
      asset = assetmanager.OpenNonAsset(asset_path, Asset.AccessMode.fromInt(access_mode));
    }

    if (!isTruthy(asset)) {
      throw new FileNotFoundException(asset_path);
    }
    return Registries.NATIVE_ASSET_REGISTRY.register(asset);
  }

  // static jobject NativeOpenNonAssetFd(JNIEnv* env, jclass /*clazz*/, jlong ptr, jint jcookie,
  //                                     jstring asset_path, jlongArray out_offsets) {
  public static ParcelFileDescriptor nativeOpenNonAssetFd(
      long ptr, int jcookie, String asset_path, long[] out_offsets)
      throws IOException {
    ApkAssetsCookie cookie = JavaCookieToApkAssetsCookie(jcookie);
    if (asset_path == null) {
      // This will throw NPE.
      return null;
    }

    ATRACE_NAME(String.format("AssetManager::OpenNonAssetFd(%s)", asset_path));

    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    Asset asset;
    if (cookie.intValue() != kInvalidCookie) {
      asset = assetmanager.OpenNonAsset(asset_path, cookie, Asset.AccessMode.ACCESS_RANDOM);
    } else {
      asset = assetmanager.OpenNonAsset(asset_path, Asset.AccessMode.ACCESS_RANDOM);
    }

    if (!isTruthy(asset)) {
      throw new FileNotFoundException(asset_path);
    }
    return ReturnParcelFileDescriptor(asset, out_offsets);
  }

  // static jlong NativeOpenXmlAsset(JNIEnv* env, jobject /*clazz*/, jlong ptr, jint jcookie,
  //                                 jstring asset_path) {
  public static long nativeOpenXmlAsset(long ptr, int jcookie, String asset_path)
      throws FileNotFoundException {
    ApkAssetsCookie cookie = JavaCookieToApkAssetsCookie(jcookie);
    if (asset_path == null) {
      // This will throw NPE.
      return 0;
    }

    ATRACE_NAME(String.format("AssetManager::OpenXmlAsset(%s)", asset_path));

    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    Asset asset;
    if (cookie.intValue() != kInvalidCookie) {
      asset = assetmanager.OpenNonAsset(asset_path, cookie, Asset.AccessMode.ACCESS_RANDOM);
    } else {
      Ref<ApkAssetsCookie> cookieRef = new Ref<>(cookie);
      asset = assetmanager.OpenNonAsset(asset_path, Asset.AccessMode.ACCESS_RANDOM, cookieRef);
      cookie = cookieRef.get();
    }

    if (!isTruthy(asset)) {
      throw new FileNotFoundException(asset_path);
    }

    // May be nullptr.
    DynamicRefTable dynamic_ref_table = assetmanager.GetDynamicRefTableForCookie(cookie);

    ResXMLTree xml_tree = new ResXMLTree(dynamic_ref_table);
    int err = xml_tree.setTo(asset.getBuffer(true), (int) asset.getLength(), true);
    // asset.reset();

    if (err != NO_ERROR) {
      throw new FileNotFoundException("Corrupt XML binary file");
    }
    return NATIVE_RES_XML_TREES.register(xml_tree);
  }

  // static jint NativeGetResourceValue(JNIEnv* env, jclass /*clazz*/, jlong ptr, jint resid,
  //                                    jshort density, jobject typed_value,
  //                                    jboolean resolve_references) {
  public static int nativeGetResourceValue(
      long ptr,
      int resid,
      short density,
      TypedValue typed_value,
      boolean resolve_references) {
    return PerfStatsCollector.getInstance()
        .measure(
            "binary nativeGetResourceValue",
            () -> {
              CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
              final Ref<Res_value> value = new Ref<>(null);
              final Ref<ResTable_config> selected_config = new Ref<>(null);
              final Ref<Integer> flags = new Ref<>(0);
              ApkAssetsCookie cookie =
                  assetmanager.GetResource(
                      resid, false /*may_be_bag*/, density, value, selected_config, flags);
              if (cookie.intValue() == kInvalidCookie) {
                return ApkAssetsCookieToJavaCookie(K_INVALID_COOKIE);
              }

              final Ref<Integer> ref = new Ref<>(resid);
              if (resolve_references) {
                cookie = assetmanager.ResolveReference(cookie, value, selected_config, flags, ref);
                if (cookie.intValue() == kInvalidCookie) {
                  return ApkAssetsCookieToJavaCookie(K_INVALID_COOKIE);
                }
              }
              return CopyValue(
                  cookie, value.get(), ref.get(), flags.get(), selected_config.get(), typed_value);
            });
  }

  // static jint NativeGetResourceBagValue(JNIEnv* env, jclass /*clazz*/, jlong ptr, jint resid,
  //                                       jint bag_entry_id, jobject typed_value) {
  public static int nativeGetResourceBagValue(
      long ptr, int resid, int bag_entry_id, TypedValue typed_value) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    ResolvedBag bag = assetmanager.GetBag(resid);
    if (bag == null) {
      return ApkAssetsCookieToJavaCookie(K_INVALID_COOKIE);
    }

    final Ref<Integer> type_spec_flags = new Ref<>(bag.type_spec_flags);
    ApkAssetsCookie cookie = K_INVALID_COOKIE;
    Res_value bag_value = null;
    for (ResolvedBag.Entry entry : bag.entries) {
      if (entry.key == bag_entry_id) {
        cookie = entry.cookie;
        bag_value = entry.value;

        // Keep searching (the old implementation did that).
      }
    }

    if (cookie.intValue() == kInvalidCookie) {
      return ApkAssetsCookieToJavaCookie(K_INVALID_COOKIE);
    }

    final Ref<Res_value> value = new Ref<>(bag_value);
    final Ref<Integer> ref = new Ref<>(resid);
    final Ref<ResTable_config> selected_config = new Ref<>(null);
    cookie = assetmanager.ResolveReference(cookie, value, selected_config, type_spec_flags, ref);
    if (cookie.intValue() == kInvalidCookie) {
      return ApkAssetsCookieToJavaCookie(K_INVALID_COOKIE);
    }
    return CopyValue(cookie, value.get(), ref.get(), type_spec_flags.get(), null, typed_value);
  }

  // static jintArray NativeGetStyleAttributes(JNIEnv* env, jclass /*clazz*/, jlong ptr, jint resid)
  // {
  public static int[] nativeGetStyleAttributes(
      long ptr, int resid) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    ResolvedBag bag = assetmanager.GetBag(resid);
    if (bag == null) {
      return null;
    }

    int[] array = new int[bag.entry_count];
    // if (env.ExceptionCheck()) {
    //   return null;
    // }

    for (int i = 0; i < bag.entry_count; i++) {
      int attr_resid = bag.entries[i].key;
      // env.SetIntArrayRegion(array, i, 1, &attr_resid);
      array[i] = attr_resid;
    }
    return array;
  }

  // static jobjectArray NativeGetResourceStringArray(JNIEnv* env, jclass /*clazz*/, jlong ptr,
  //                                                  jint resid) {
  public static String[] nativeGetResourceStringArray(long ptr, int resid) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    ResolvedBag bag = assetmanager.GetBag(resid);
    if (bag == null) {
      return null;
    }

    String[] array = new String[bag.entry_count];
    if (array == null) {
      return null;
    }

    for (int i = 0; i < bag.entry_count; i++) {
      ResolvedBag.Entry entry = bag.entries[i];

      // Resolve any references to their final value.
      final Ref<Res_value> value = new Ref<>(entry.value);
      final Ref<ResTable_config> selected_config = new Ref<>(null);
      final Ref<Integer> flags = new Ref<>(0);
      final Ref<Integer> ref = new Ref<>(0);
      ApkAssetsCookie cookie =
          assetmanager.ResolveReference(entry.cookie, value, selected_config, flags, ref);
      if (cookie.intValue() == kInvalidCookie) {
        return null;
      }

      if (value.get().dataType == Res_value.TYPE_STRING) {
        CppApkAssets apk_assets = assetmanager.GetApkAssets().get(cookie.intValue());
        ResStringPool pool = apk_assets.GetLoadedArsc().GetStringPool();

        String java_string;
        String str_utf8 = pool.stringAt(value.get().data);
        if (str_utf8 != null) {
          java_string = str_utf8;
        } else {
          java_string = pool.stringAt(value.get().data);
        }

        // // Check for errors creating the strings (if malformed or no memory).
        // if (env.ExceptionCheck()) {
        //   return null;
        // }

        // env.SetObjectArrayElement(array, i, java_string);
        array[i] = java_string;

        // If we have a large amount of string in our array, we might overflow the
        // local reference table of the VM.
        // env.DeleteLocalRef(java_string);
      }
    }
    return array;
  }

  // static jintArray NativeGetResourceStringArrayInfo(JNIEnv* env, jclass /*clazz*/, jlong ptr,
  //                                                   jint resid) {
  public static int[] nativeGetResourceStringArrayInfo(long ptr, int resid) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    ResolvedBag bag = assetmanager.GetBag(resid);
    if (bag == null) {
      return null;
    }

    int[] array = new int[bag.entry_count * 2];
    // if (array == null) {
    //   return null;
    // }

    int[] buffer = array; // reinterpret_cast<int*>(env.GetPrimitiveArrayCritical(array, null));
    // if (buffer == null) {
    //   return null;
    // }

    for (int i = 0; i < bag.entry_count; i++) {
      ResolvedBag.Entry entry = bag.entries[i];
      final Ref<Res_value> value = new Ref<>(entry.value);
      final Ref<ResTable_config> selected_config = new Ref<>(null);
      final Ref<Integer> flags = new Ref<>(0);
      final Ref<Integer> ref = new Ref<>(0);
      ApkAssetsCookie cookie =
          assetmanager.ResolveReference(entry.cookie, value, selected_config, flags, ref);
      if (cookie.intValue() == kInvalidCookie) {
        // env.ReleasePrimitiveArrayCritical(array, buffer, JNI_ABORT);
        return null;
      }

      int string_index = -1;
      if (value.get().dataType == Res_value.TYPE_STRING) {
        string_index = value.get().data;
      }

      buffer[i * 2] = ApkAssetsCookieToJavaCookie(cookie);
      buffer[(i * 2) + 1] = string_index;
    }
    // env.ReleasePrimitiveArrayCritical(array, buffer, 0);
    return array;
  }

  // static jintArray NativeGetResourceIntArray(JNIEnv* env, jclass /*clazz*/, jlong ptr, jint
  // resid) {
  public static int[] nativeGetResourceIntArray(long ptr, int resid) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    ResolvedBag bag = assetmanager.GetBag(resid);
    if (bag == null) {
      return null;
    }

    int[] array = new int[bag.entry_count];
    // if (array == null) {
    //   return null;
    // }

    int[] buffer = array; // reinterpret_cast<int*>(env.GetPrimitiveArrayCritical(array, null));
    // if (buffer == null) {
    //   return null;
    // }

    for (int i = 0; i < bag.entry_count; i++) {
      ResolvedBag.Entry entry = bag.entries[i];
      final Ref<Res_value> value = new Ref<>(entry.value);
      final Ref<ResTable_config> selected_config = new Ref<>(null);
      final Ref<Integer> flags = new Ref<>(0);
      final Ref<Integer> ref = new Ref<>(0);
      ApkAssetsCookie cookie =
          assetmanager.ResolveReference(entry.cookie, value, selected_config, flags, ref);
      if (cookie.intValue() == kInvalidCookie) {
        // env.ReleasePrimitiveArrayCritical(array, buffer, JNI_ABORT);
        return null;
      }

      if (value.get().dataType >= Res_value.TYPE_FIRST_INT
          && value.get().dataType <= Res_value.TYPE_LAST_INT) {
        buffer[i] = value.get().data;
      }
    }
    // env.ReleasePrimitiveArrayCritical(array, buffer, 0);
    return array;
  }

  // static jint NativeGetResourceArraySize(JNIEnv* /*env*/, jclass /*clazz*/, jlong ptr, jint
  // resid) {
  public static int nativeGetResourceArraySize(long ptr, int resid) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    ResolvedBag bag = assetmanager.GetBag(resid);
    if (bag == null) {
      return -1;
    }
    return bag.entry_count;
  }

  // static jint NativeGetResourceArray(JNIEnv* env, jclass /*clazz*/, jlong ptr, jint resid,
  //                                    jintArray out_data) {
  public static int nativeGetResourceArray(
      long ptr, int resid, int[] out_data) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    ResolvedBag bag = assetmanager.GetBag(resid);
    if (bag == null) {
      return -1;
    }

    int out_data_length = out_data.length;
    // if (env.ExceptionCheck()) {
    //   return -1;
    // }

    if (bag.entry_count > out_data_length * STYLE_NUM_ENTRIES) {
      throw new IllegalArgumentException("Input array is not large enough");
    }

    int[] buffer =
        out_data; // reinterpret_cast<int*>(env.GetPrimitiveArrayCritical(out_data, null));
    if (buffer == null) {
      return -1;
    }

    for (int i = 0; i < bag.entry_count; i++) {
      ResolvedBag.Entry entry = bag.entries[i];
      final Ref<Res_value> value = new Ref<>(entry.value);
      final Ref<ResTable_config> selected_config = new Ref<>(new ResTable_config());
      selected_config.get().density = 0;
      final Ref<Integer> flags = new Ref<>(bag.type_spec_flags);
      final Ref<Integer> ref = new Ref<>(0);
      ApkAssetsCookie cookie =
          assetmanager.ResolveReference(entry.cookie, value, selected_config, flags, ref);
      if (cookie.intValue() == kInvalidCookie) {
        // env.ReleasePrimitiveArrayCritical(out_data, buffer, JNI_ABORT);
        return -1;
      }

      // Deal with the special @null value -- it turns back to TYPE_NULL.
      if (value.get().dataType == Res_value.TYPE_REFERENCE && value.get().data == 0) {
        value.set(Res_value.NULL_VALUE);
      }

      int offset = i * STYLE_NUM_ENTRIES;
      buffer[offset + STYLE_TYPE] = value.get().dataType;
      buffer[offset + STYLE_DATA] = value.get().data;
      buffer[offset + STYLE_ASSET_COOKIE] = ApkAssetsCookieToJavaCookie(cookie);
      buffer[offset + STYLE_RESOURCE_ID] = ref.get();
      buffer[offset + STYLE_CHANGING_CONFIGURATIONS] = flags.get();
      buffer[offset + STYLE_DENSITY] = selected_config.get().density;
      // cursor += STYLE_NUM_ENTRIES;
    }
    // env.ReleasePrimitiveArrayCritical(out_data, buffer, 0);
    return bag.entry_count;
  }

  // static jint NativeGetResourceIdentifier(JNIEnv* env, jclass /*clazz*/, jlong ptr, jstring name,
  //                                         jstring def_type, jstring def_package) {
  public static int nativeGetResourceIdentifier(
      long ptr, String name, String def_type, String def_package) {
    if (name == null) {
      // This will throw NPE.
      return 0;
    }

    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    return assetmanager.GetResourceId(name, def_type, def_package);
  }

  // static jstring NativeGetResourceName(JNIEnv* env, jclass /*clazz*/, jlong ptr, jint resid) {
  public static String nativeGetResourceName(long ptr, int resid) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    CppAssetManager2.ResourceName name = new ResourceName();
    if (!assetmanager.GetResourceName(resid, name)) {
      return null;
    }

    StringBuilder result = new StringBuilder();
    if (name.package_ != null) {
      result.append(name.package_ /*, name.package_len*/);
    }

    if (name.type != null /*|| name.type16 != null*/) {
      if (!(result.length() == 0)) {
        result.append(":");
      }

      // if (name.type != null) {
      result.append(name.type /*, name.type_len*/);
      // } else {
      //   result.append( /*util.Utf16ToUtf8(StringPiece16(*/ name.type16 /*, name.type_len))*/);
      // }
    }

    if (name.entry != null /*|| name.entry16 != null*/) {
      if (!(result.length() == 0)) {
        result.append("/");
      }

      // if (name.entry != null) {
      result.append(name.entry /*, name.entry_len*/);
      // } else {
      //   result.append( /*util.Utf16ToUtf8(StringPiece16(*/ name.entry16 /*, name.entry_len)*/);
      // }
    }
    return result.toString();
  }

  // static jstring NativeGetResourcePackageName(JNIEnv* env, jclass /*clazz*/, jlong ptr, jint
  // resid) {
  public static String nativeGetResourcePackageName(long ptr, int resid) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    final ResourceName name = new ResourceName();
    if (!assetmanager.GetResourceName(resid, name)) {
      return null;
    }

    if (name.package_ != null) {
      return name.package_;
    }
    return null;
  }

  // static jstring NativeGetResourceTypeName(JNIEnv* env, jclass /*clazz*/, jlong ptr, jint resid)
  // {
  public static String nativeGetResourceTypeName(long ptr, int resid) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    final ResourceName name = new ResourceName();
    if (!assetmanager.GetResourceName(resid, name)) {
      return null;
    }

    if (name.type != null) {
      return name.type;
      // } else if (name.get().type16 != null) {
      //   return name.get().type16; // env.NewString(reinterpret_cast<jchar*>(name.type16),
      // name.type_len);
    }
    return null;
  }

  // static jstring NativeGetResourceEntryName(JNIEnv* env, jclass /*clazz*/, jlong ptr, jint resid)
  // {
  public static String nativeGetResourceEntryName(long ptr, int resid) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    final ResourceName name = new ResourceName();
    if (!assetmanager.GetResourceName(resid, name)) {
      return null;
    }

    if (name.entry != null) {
      return name.entry;
      // } else if (name.entry16 != null) {
      //   return name.entry16; // env.NewString(reinterpret_cast<jchar*>(name.entry16),
      // name.entry_len);
    }
    return null;
  }

  // static jobjectArray NativeGetLocales(JNIEnv* env, jclass /*class*/, jlong ptr,
  //                                      jboolean exclude_system) {
  public static String[] nativeGetLocales(long ptr, boolean exclude_system) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    Set<String> locales =
        assetmanager.GetResourceLocales(exclude_system, true /*merge_equivalent_languages*/);

    String[] array =
        new String[locales.size()]; // env.NewObjectArray(locales.size(), g_stringClass, null);
    // if (array == null) {
    //   return null;
    // }

    int idx = 0;
    for (String locale : locales) {
      if (locale == null) {
        return null;
      }
      // env.SetObjectArrayElement(array, idx++, java_string);
      array[idx++] = locale;
      // env.DeleteLocalRef(java_string);
    }
    return array;
  }

  static Configuration ConstructConfigurationObject(/* JNIEnv* env,*/ ResTable_config config) {
    // jobject result =
    //     env.NewObject(gConfigurationOffsets.classObject, gConfigurationOffsets.constructor);
    Configuration result = new Configuration();
    // if (result == null) {
    //   return null;
    // }

    result.smallestScreenWidthDp = config.smallestScreenWidthDp;
    result.screenWidthDp = config.screenWidthDp;
    result.screenHeightDp = config.screenHeightDp;
    return result;
  }

  // static jobjectArray NativeGetSizeConfigurations(JNIEnv* env, jclass /*clazz*/, jlong ptr) {
  public static Configuration[] nativeGetSizeConfigurations(long ptr) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    Set<ResTable_config> configurations =
        assetmanager.GetResourceConfigurations(true /*exclude_system*/, false /*exclude_mipmap*/);

    Configuration[] array = new Configuration[configurations.size()];
    // env.NewObjectArray(configurations.size(), gConfigurationOffsets.classObject, null);
    // if (array == null) {
    //   return null;
    // }

    int idx = 0;
    for (ResTable_config configuration : configurations) {
      Configuration java_configuration = ConstructConfigurationObject(configuration);
      // if (java_configuration == null) {
      //   return null;
      // }

      // env.SetObjectArrayElement(array, idx++, java_configuration);
      array[idx++] = java_configuration;
      // env.DeleteLocalRef(java_configuration);
    }
    return array;
  }

  // static void NativeApplyStyle(JNIEnv* env, jclass /*clazz*/, jlong ptr, jlong theme_ptr,
  //                              jint def_style_attr, jint def_style_resid, jlong xml_parser_ptr,
  //                              jintArray java_attrs, jlong out_values_ptr, jlong out_indices_ptr)
  // {
  public static void nativeApplyStyle(
      long ptr,
      long theme_ptr,
      int def_style_attr,
      int def_style_resid,
      long xml_parser_ptr,
      int[] java_attrs,
      long out_values_ptr,
      long out_indices_ptr) {
    PerfStatsCollector.getInstance()
        .measure(
            "applyStyle",
            () ->
                nativeApplyStyle_measured(
                    ptr,
                    theme_ptr,
                    def_style_attr,
                    def_style_resid,
                    xml_parser_ptr,
                    java_attrs,
                    out_values_ptr,
                    out_indices_ptr));
  }

  private static void nativeApplyStyle_measured(
      long ptr,
      long theme_ptr,
      int def_style_attr,
      int def_style_resid,
      long xml_parser_ptr,
      int[] java_attrs,
      long out_values_ptr,
      long out_indices_ptr) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    Theme theme = Registries.NATIVE_THEME9_REGISTRY.getNativeObject(theme_ptr);
    CHECK(theme.GetAssetManager() == assetmanager);
    // (void) assetmanager;

    ResXMLParser xml_parser =
        xml_parser_ptr == 0 ? null : NATIVE_RES_XML_PARSERS.getNativeObject(xml_parser_ptr);
    // int[] out_values = reinterpret_cast<int*>(out_values_ptr);
    // int[] out_indices = reinterpret_cast<int*>(out_indices_ptr);
    int[] out_values = (int[]) Compat.objectForAddress(out_values_ptr);
    int[] out_indices = (int[]) Compat.objectForAddress(out_indices_ptr);

    int attrs_len = java_attrs.length;
    int[] attrs =
        java_attrs; // reinterpret_cast<int*>(env.GetPrimitiveArrayCritical(java_attrs, null));
    // if (attrs == null) {
    //   return;
    // }

    ApplyStyle(
        theme,
        xml_parser,
        def_style_attr,
        def_style_resid,
        attrs,
        attrs_len,
        out_values,
        out_indices);
    // env.ReleasePrimitiveArrayCritical(java_attrs, attrs, JNI_ABORT);
  }

  // static jboolean NativeResolveAttrs(JNIEnv* env, jclass /*clazz*/, jlong ptr, jlong theme_ptr,
  //                                    jint def_style_attr, jint def_style_resid, jintArray
  // java_values,
  //                                    jintArray java_attrs, jintArray out_java_values,
  //                                    jintArray out_java_indices) {
  public static boolean nativeResolveAttrs(
      long ptr,
      long theme_ptr,
      int def_style_attr,
      int def_style_resid,
      int[] java_values,
      int[] java_attrs,
      int[] out_java_values,
      int[] out_java_indices) {
    int attrs_len = java_attrs.length;
    int out_values_len = out_java_values.length;
    if (out_values_len < (attrs_len * STYLE_NUM_ENTRIES)) {
      throw new IndexOutOfBoundsException("outValues too small");
    }

    int[] attrs =
        java_attrs; // reinterpret_cast<int*>(env.GetPrimitiveArrayCritical(java_attrs, null));
    if (attrs == null) {
      return JNI_FALSE;
    }

    int[] values = null;
    int values_len = 0;
    if (java_values != null) {
      values_len = java_values.length;
      values =
          java_values; // reinterpret_cast<int*>(env.GetPrimitiveArrayCritical(java_values, null));
      if (values == null) {
        // env.ReleasePrimitiveArrayCritical(java_attrs, attrs, JNI_ABORT);
        return JNI_FALSE;
      }
    }

    int[] out_values = out_java_values;
    // reinterpret_cast<int*>(env.GetPrimitiveArrayCritical(out_java_values, null));
    if (out_values == null) {
      // env.ReleasePrimitiveArrayCritical(java_attrs, attrs, JNI_ABORT);
      // if (values != null) {
      //   env.ReleasePrimitiveArrayCritical(java_values, values, JNI_ABORT);
      // }
      return JNI_FALSE;
    }

    int[] out_indices = null;
    if (out_java_indices != null) {
      int out_indices_len = out_java_indices.length;
      if (out_indices_len > attrs_len) {
        out_indices = out_java_indices;
        // reinterpret_cast<int*>(env.GetPrimitiveArrayCritical(out_java_indices, null));
        if (out_indices == null) {
          // env.ReleasePrimitiveArrayCritical(java_attrs, attrs, JNI_ABORT);
          // if (values != null) {
          //   env.ReleasePrimitiveArrayCritical(java_values, values, JNI_ABORT);
          // }
          // env.ReleasePrimitiveArrayCritical(out_java_values, out_values, JNI_ABORT);
          return JNI_FALSE;
        }
      }
    }

    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    Theme theme = Registries.NATIVE_THEME9_REGISTRY.getNativeObject(theme_ptr);
    CHECK(theme.GetAssetManager() == assetmanager);
    // (void) assetmanager;

    boolean result =
        ResolveAttrs(
            theme,
            def_style_attr,
            def_style_resid,
            values,
            values_len,
            attrs,
            attrs_len,
            out_values,
            out_indices);
    // if (out_indices != null) {
    //   env.ReleasePrimitiveArrayCritical(out_java_indices, out_indices, 0);
    // }

    // env.ReleasePrimitiveArrayCritical(out_java_values, out_values, 0);
    // if (values != null) {
    //   env.ReleasePrimitiveArrayCritical(java_values, values, JNI_ABORT);
    // }
    // env.ReleasePrimitiveArrayCritical(java_attrs, attrs, JNI_ABORT);
    return result ? JNI_TRUE : JNI_FALSE;
  }

  // static jboolean NativeRetrieveAttributes(JNIEnv* env, jclass /*clazz*/, jlong ptr,
  //                                          jlong xml_parser_ptr, jintArray java_attrs,
  //                                          jintArray out_java_values, jintArray out_java_indices)
  // {
  public static boolean nativeRetrieveAttributes(
      long ptr,
      long xml_parser_ptr,
      int[] java_attrs,
      int[] out_java_values,
      int[] out_java_indices) {
    int attrs_len = java_attrs.length;
    int out_values_len = out_java_values.length;
    if (out_values_len < (attrs_len * STYLE_NUM_ENTRIES)) {
      throw new IndexOutOfBoundsException("outValues too small");
    }

    int[] attrs =
        java_attrs; // reinterpret_cast<int*>(env.GetPrimitiveArrayCritical(java_attrs, null));
    if (attrs == null) {
      return JNI_FALSE;
    }

    int[] out_values = out_java_values;
    // reinterpret_cast<int*>(env.GetPrimitiveArrayCritical(out_java_values, null));
    if (out_values == null) {
      // env.ReleasePrimitiveArrayCritical(java_attrs, attrs, JNI_ABORT);
      return JNI_FALSE;
    }

    int[] out_indices = null;
    if (out_java_indices != null) {
      int out_indices_len = out_java_indices.length;
      if (out_indices_len > attrs_len) {
        out_indices = out_java_indices;
        // reinterpret_cast<int*>(env.GetPrimitiveArrayCritical(out_java_indices, null));
        if (out_indices == null) {
          // env.ReleasePrimitiveArrayCritical(java_attrs, attrs, JNI_ABORT);
          // env.ReleasePrimitiveArrayCritical(out_java_values, out_values, JNI_ABORT);
          return JNI_FALSE;
        }
      }
    }

    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    ResXMLParser xml_parser = NATIVE_RES_XML_PARSERS.getNativeObject(xml_parser_ptr);

    boolean result =
        RetrieveAttributes(assetmanager, xml_parser, attrs, attrs_len, out_values, out_indices);

    // if (out_indices != null) {
    //   env.ReleasePrimitiveArrayCritical(out_java_indices, out_indices, 0);
    // }
    // env.ReleasePrimitiveArrayCritical(out_java_values, out_values, 0);
    // env.ReleasePrimitiveArrayCritical(java_attrs, attrs, JNI_ABORT);
    return result;
  }

  // static jlong NativeThemeCreate(JNIEnv* /*env*/, jclass /*clazz*/, jlong ptr) {
  public static long nativeThemeCreate(long ptr) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    return Registries.NATIVE_THEME9_REGISTRY.register(assetmanager.NewTheme());
  }

  // static void NativeThemeDestroy(JNIEnv* /*env*/, jclass /*clazz*/, jlong theme_ptr) {


  // static void NativeThemeApplyStyle(JNIEnv* env, jclass /*clazz*/, jlong ptr, jlong theme_ptr,
  //                                   jint resid, jboolean force) {
  public static void nativeThemeApplyStyle(
      long ptr, long theme_ptr, int resid, boolean force) {
    // AssetManager is accessed via the theme, so grab an explicit lock here.
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    Theme theme = Registries.NATIVE_THEME9_REGISTRY.getNativeObject(theme_ptr);
    CHECK(theme.GetAssetManager() == assetmanager);
    // (void) assetmanager;
    theme.ApplyStyle(resid, force);

    // TODO(adamlesinski): Consider surfacing exception when result is failure.
    // CTS currently expects no exceptions from this method.
    // std::string error_msg = StringPrintf("Failed to apply style 0x%08x to theme", resid);
    // throw new IllegalArgumentException(error_msg.c_str());
  }

  // static void NativeThemeCopy(JNIEnv* env, jclass /*clazz*/, jlong dst_theme_ptr,
  //                             jlong src_theme_ptr) {

  // static void NativeThemeCopy(JNIEnv* env, jclass /*clazz*/, jlong dst_asset_manager_ptr,
  //     jlong dst_theme_ptr, jlong src_asset_manager_ptr, jlong src_theme_ptr) {
  public static void nativeThemeCopy(
      long dst_asset_manager_ptr,
      long dst_theme_ptr,
      long src_asset_manager_ptr,
      long src_theme_ptr) {
    Theme dst_theme = Registries.NATIVE_THEME9_REGISTRY.getNativeObject(dst_theme_ptr);
    Theme src_theme = Registries.NATIVE_THEME9_REGISTRY.getNativeObject(src_theme_ptr);
    if (dst_asset_manager_ptr != src_asset_manager_ptr) {
      CppAssetManager2 dst_assetmanager = AssetManagerFromLong(dst_asset_manager_ptr);
      CHECK(dst_theme.GetAssetManager() == dst_assetmanager);
      // (void) dst_assetmanager;

      CppAssetManager2 src_assetmanager = AssetManagerFromLong(src_asset_manager_ptr);
      CHECK(src_theme.GetAssetManager() == src_assetmanager);
      // (void) src_assetmanager;

      dst_theme.SetTo(src_theme);
    } else {
      dst_theme.SetTo(src_theme);
    }
  }

  // static void NativeThemeClear(JNIEnv* /*env*/, jclass /*clazz*/, jlong theme_ptr) {

  // static jint NativeThemeGetAttributeValue(JNIEnv* env, jclass /*clazz*/, jlong ptr, jlong
  // theme_ptr,
  //                                          jint resid, jobject typed_value,
  //                                          jboolean resolve_references) {
  public static int nativeThemeGetAttributeValue(
      long ptr,
      long theme_ptr,
      int resid,
      TypedValue typed_value,
      boolean resolve_references) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    Theme theme = Registries.NATIVE_THEME9_REGISTRY.getNativeObject(theme_ptr);
    CHECK(theme.GetAssetManager() == assetmanager);
    // (void) assetmanager; // huh?

    final Ref<Res_value> value = new Ref<>(null);
    final Ref<Integer> flags = new Ref<>(null);
    ApkAssetsCookie cookie = theme.GetAttribute(resid, value, flags);
    if (cookie.intValue() == kInvalidCookie) {
      return ApkAssetsCookieToJavaCookie(K_INVALID_COOKIE);
    }

    final Ref<Integer> ref = new Ref<>(0);
    if (resolve_references) {
      final Ref<ResTable_config> selected_config = new Ref<>(null);
      cookie = theme.GetAssetManager().ResolveReference(cookie, value, selected_config, flags, ref);
      if (cookie.intValue() == kInvalidCookie) {
        return ApkAssetsCookieToJavaCookie(K_INVALID_COOKIE);
      }
    }
    return CopyValue(cookie, value.get(), ref.get(), flags.get(), null, typed_value);
  }

  // static void NativeThemeDump(JNIEnv* /*env*/, jclass /*clazz*/, jlong ptr, jlong theme_ptr,
  //                             jint priority, jstring tag, jstring prefix) {
  public static void nativeThemeDump(
      long ptr, long theme_ptr, int priority, String tag, String prefix) {
    CppAssetManager2 assetmanager = AssetManagerFromLong(ptr);
    Theme theme = Registries.NATIVE_THEME9_REGISTRY.getNativeObject(theme_ptr);
    CHECK(theme.GetAssetManager() == assetmanager);
    // (void) assetmanager;
    // (void) theme;
    // (void) priority;
    // (void) tag;
    // (void) prefix;
  }

  // static jint NativeThemeGetChangingConfigurations(JNIEnv* /*env*/, jclass /*clazz*/,
  //                                                  jlong theme_ptr) {
  public static int nativeThemeGetChangingConfigurations(long theme_ptr) {
    Theme theme = Registries.NATIVE_THEME9_REGISTRY.getNativeObject(theme_ptr);
    return theme.GetChangingConfigurations();
  }

  // static void NativeAssetDestroy(JNIEnv* /*env*/, jclass /*clazz*/, jlong asset_ptr) {
  public static void nativeAssetDestroy(long asset_ptr) {
    Registries.NATIVE_ASSET_REGISTRY.unregister(asset_ptr);
  }

  // static jint NativeAssetReadChar(JNIEnv* /*env*/, jclass /*clazz*/, jlong asset_ptr) {
  public static int nativeAssetReadChar(long asset_ptr) {
    Asset asset = Registries.NATIVE_ASSET_REGISTRY.getNativeObject(asset_ptr);
    byte[] b = new byte[1];
    int res = asset.read(b, 1);
    return res == 1 ? (int) (b[0]) & 0xff : -1;
  }

  // static jint NativeAssetRead(JNIEnv* env, jclass /*clazz*/, jlong asset_ptr, jbyteArray
  // java_buffer,
  //                             jint offset, jint len) {
  public static int nativeAssetRead(long asset_ptr, byte[] java_buffer, int offset, int len)
      throws IOException {
    if (len == 0) {
      return 0;
    }

    int buffer_len = java_buffer.length;
    if (offset < 0
        || offset >= buffer_len
        || len < 0
        || len > buffer_len
        || offset > buffer_len - len) {
      throw new IndexOutOfBoundsException();
    }

    // ScopedByteArrayRW byte_array(env, java_buffer);
    // if (byte_array.get() == null) {
    //   return -1;
    // }

    Asset asset = Registries.NATIVE_ASSET_REGISTRY.getNativeObject(asset_ptr);
    // sint res = asset.read(byte_array.get() + offset, len);
    int res = asset.read(java_buffer, offset, len);
    if (res < 0) {
      throw new IOException();
    }
    return res > 0 ? res : -1;
  }

  // static jlong NativeAssetSeek(JNIEnv* env, jclass /*clazz*/, jlong asset_ptr, jlong offset,
  //                              jint whence) {
  public static long nativeAssetSeek(long asset_ptr, long offset, int whence) {
    Asset asset = Registries.NATIVE_ASSET_REGISTRY.getNativeObject(asset_ptr);
    return asset.seek((offset), (whence > 0 ? SEEK_END : (whence < 0 ? SEEK_SET : SEEK_CUR)));
  }

  // static jlong NativeAssetGetLength(JNIEnv* /*env*/, jclass /*clazz*/, jlong asset_ptr) {
  public static long nativeAssetGetLength(long asset_ptr) {
    Asset asset = Registries.NATIVE_ASSET_REGISTRY.getNativeObject(asset_ptr);
    return asset.getLength();
  }

  // static jlong NativeAssetGetRemainingLength(JNIEnv* /*env*/, jclass /*clazz*/, jlong asset_ptr)
  // {
  public static long nativeAssetGetRemainingLength(long asset_ptr) {
    Asset asset = Registries.NATIVE_ASSET_REGISTRY.getNativeObject(asset_ptr);
    return asset.getRemainingLength();
  }
}
