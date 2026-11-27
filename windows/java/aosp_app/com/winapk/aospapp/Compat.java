// What AOSP's app layer (ActivityThread, Activity, ContextImpl, LoadedApk, Instrumentation...) calls
// that Win.apk's own classes (android.content, android.os, ActivityManager...) do not have, answered
// as AOSP's would in an app process. AospAppRewrite.java (windows/scripts) sends those calls here: a
// static keeps its descriptor, an instance call passes its receiver first. Found by LinkScan
// (build_aosp_app.sh lists what is left) and by running the lifecycle.
package com.winapk.aospapp;

import android.app.ActivityThread;
import android.app.IActivityManager;
import android.app.LoadedApk;
import android.content.Intent;
import android.os.ServiceManager;

public final class Compat {
    private Compat() {}

    // ---- android.app.ActivityManager (Win.apk's keeps the public API; the service is here) ----

    /** ActivityManager.getService(): IActivityManagerSingleton over ServiceManager's "activity". */
    public static IActivityManager getService() {
        return IActivityManager.Stub.asInterface(ServiceManager.getService("activity"));
    }

    // ---- android.app.LoadedApk ----

    /** The class loader LoadedApk.createOrUpdateClassLoaderLocked makes: the host's, over the app's
     *  converted DEX (AospAppRewrite.java). */
    public static ClassLoader appClassLoader(LoadedApk apk) {
        AospApp app = AospApp.get();
        return app != null && app.classLoader != null ? app.classLoader : Compat.class.getClassLoader();
    }

    // ---- android.app.compat.CompatChanges ----

    /** CompatChanges.isChangeEnabled: every change enabled, as for the views (the app runs at the
     *  presented SDK level; com.winapk.aospviews.Compat answers the same). */
    public static boolean isChangeEnabled(long changeId) { return true; }

    // ---- android.graphics ----

    /** android.graphics.Compatibility.setTargetSdkVersion (Canvas's behaviour by the app's target
     *  SDK), when the run's graphics are AOSP's; Win.apk's Java2D graphics have no such switch. */
    public static void setTargetSdkVersion(int targetSdk) {
        if (com.macapk.loader.gfx.GfxBackend.isAosp()) android.graphics.Compatibility.setTargetSdkVersion(targetSdk);
    }

    /** ActivityManager.isStartResultFatalError: START_* codes from -100 to -1 are fatal. */
    public static boolean isStartResultFatalError(int result) { return -100 <= result && result <= -1; }

    // ---- android.os.FileUtils (hidden; AOSP's bodies) ----

    public static java.io.File newFileOrNull(String path) { return path != null ? new java.io.File(path) : null; }

    public static String[] listOrEmpty(java.io.File dir) {
        String[] l = dir != null ? dir.list() : null;
        return l != null ? l : new String[0];
    }

    public static java.io.File[] listFilesOrEmpty(java.io.File dir, java.io.FilenameFilter filter) {
        java.io.File[] l = dir != null ? dir.listFiles(filter) : null;
        return l != null ? l : new java.io.File[0];
    }

    /** FileUtils.isValidExtFilename: equal to buildValidExtFilename(name), which replaces the
     *  characters FAT and ext4 refuse and names "." and ".." and "" "(invalid)". */
    public static boolean isValidExtFilename(String name) {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")) return false;
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (c <= 0x1f || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?'
                    || c == '\\' || c == '|' || c == 0x7f) return false;
        }
        return true;
    }

    /** FileUtils.setPermissions: Windows' files have no POSIX mode; 0 is success, as Os.chmod's. */
    public static int setPermissions(String path, int mode, int uid, int gid) { return 0; }

    public static void copyFileOrThrow(java.io.File from, java.io.File to) throws java.io.IOException {
        java.nio.file.Files.copy(from.toPath(), to.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
    }

    public static void copyPermissions(java.io.File from, java.io.File to) {}

    // ---- android.os.Environment (hidden; AOSP's paths, on the external storage Win.apk maps) ----

    public static java.io.File[] buildPaths(java.io.File[] base, String... segments) {
        java.io.File[] result = new java.io.File[base.length];
        for (int i = 0; i < base.length; i++) {
            java.io.File f = base[i];
            for (String s : segments) f = f == null ? new java.io.File(s) : new java.io.File(f, s);
            result[i] = f;
        }
        return result;
    }

    private static java.io.File[] external() {
        return new java.io.File[] {android.os.Environment.getExternalStorageDirectory()};
    }

    public static java.io.File[] buildExternalStorageAppCacheDirs(String pkg) {
        return buildPaths(external(), "Android", "data", pkg, "cache");
    }

    public static java.io.File[] buildExternalStorageAppFilesDirs(String pkg) {
        return buildPaths(external(), "Android", "data", pkg, "files");
    }

    public static java.io.File[] buildExternalStorageAppMediaDirs(String pkg) {
        return buildPaths(external(), "Android", "media", pkg);
    }

    public static java.io.File[] buildExternalStorageAppObbDirs(String pkg) {
        return buildPaths(external(), "Android", "obb", pkg);
    }

    public static java.io.File getDataPreloadsFileCacheDirectory(String pkg) {
        return new java.io.File(new java.io.File(new java.io.File(android.os.Environment.getDataDirectory(),
                "preloads"), "file_cache"), pkg);
    }

    public static java.io.File getUserConfigDirectory(int userId) {
        return new java.io.File(new java.io.File(new java.io.File(android.os.Environment.getDataDirectory(),
                "misc"), "user"), Integer.toString(userId));
    }

    // ---- android.content.pm.ApplicationInfo (hidden) ----

    /** getCodePath(): scanSourceDir, the folder the APKs are in. */
    public static String getCodePath(android.content.pm.ApplicationInfo ai) {
        return ai.sourceDir != null ? new java.io.File(ai.sourceDir).getParent() : null;
    }

    public static String getBaseCodePath(android.content.pm.ApplicationInfo ai) { return ai.sourceDir; }

    /** No per-process Application classes (one process): the application's own. */
    public static String getCustomApplicationClassNameForProcess(android.content.pm.ApplicationInfo ai, String proc) {
        return ai.className;
    }

    // ---- android.os.UserHandle / UserManager (hidden) ----

    public static boolean isSameApp(int uid1, int uid2) { return uid1 % 100000 == uid2 % 100000; }

    /** UserHandle.getCacheAppGid: FIRST_APPLICATION_CACHE_GID + (appId - FIRST_APPLICATION_UID) for an
     *  app's uid, else -1. */
    public static int getCacheAppGid(int uid) {
        int appId = uid % 100000;
        return appId >= 10000 && appId <= 19999 ? 20000 + appId - 10000 : -1;
    }

    /** The one user is unlocked (Windows' user is signed in). */
    public static boolean isUserUnlocked(android.os.UserManager um, int userId) { return true; }

    public static boolean isUserUnlockingOrUnlocked(android.os.UserManager um, int userId) { return true; }

    /** Context.getDisplayId() on an Activity: the one display. */
    public static int getDisplayId(android.app.Activity a) { return 0; }

    /** SystemServiceRegistry.createServiceCache: a ContextImpl's cache for the registry's fetchers,
     *  empty (its lookups are Win.apk's Context's). */
    public static Object[] createServiceCache() { return new Object[0]; }

    // ---- android.app.ContextImpl ----

    /** ContextImpl.createAttributionSource: this process's uid and the context's package and tag,
     *  chained to the next source, with the permissions the context renounced (Activity.requestPermissions
     *  refuses to ask for one of them, as AOSP's). */
    public static android.content.AttributionSource createAttributionSource(android.content.Context ctx,
            String attributionTag, android.content.AttributionSource next, java.util.Set<String> renounced) {
        return new android.content.AttributionSource.Builder(android.os.Process.myUid())
                .setPid(android.os.Process.myPid()).setPackageName(opPackageName(ctx))
                .setAttributionTag(attributionTag).setNext(next).setRenouncedPermissions(renounced).build();
    }

    private static java.lang.reflect.Field sOpPackageName;

    /** ContextImpl.mOpPackageName, which the constructor sets before it makes the attribution source
     *  (getOpPackageName reads the source, not yet made). */
    private static String opPackageName(android.content.Context ctx) {
        try {
            if (sOpPackageName == null) {
                java.lang.reflect.Field f = ctx.getClass().getDeclaredField("mOpPackageName");
                f.setAccessible(true);
                sOpPackageName = f;
            }
            return (String) sOpPackageName.get(ctx);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(e);
        }
    }

    /** StorageManager.mkdirs (an app's external folders): vold makes them on a device; here the
     *  folders are made directly. */
    public static void mkdirs(android.os.storage.StorageManager sm, java.io.File file) {
        file.mkdirs();
    }

    // ---- android.content.pm.PackageManager's process-wide caches (hidden statics) ----

    /** PackageManager.getPackageInfoAsUserCached: the package's info as PackageManagerService gives
     *  it; here the bound app's (the one package of this process), null for any other. */
    public static android.content.pm.PackageInfo getPackageInfoAsUserCached(String pkg, long flags, int userId) {
        AospApp app = AospApp.get();
        if (app == null || !app.info.packageName.equals(pkg)) return null;
        android.content.pm.PackageInfo pi = new android.content.pm.PackageInfo();
        pi.packageName = pkg;
        pi.applicationInfo = new android.content.pm.ApplicationInfo(app.info);
        return pi;
    }

    public static android.content.pm.ApplicationInfo getApplicationInfoAsUserCached(String pkg, long flags, int userId) {
        AospApp app = AospApp.get();
        return app != null && app.info.packageName.equals(pkg) ? new android.content.pm.ApplicationInfo(app.info) : null;
    }

    /** ContextImpl.getPackageManager: the host's (AospApp.start's), one per process as
     *  ApplicationPackageManager wraps the one IPackageManager. */
    public static android.content.pm.PackageManager getPackageManager(android.content.Context ctx) {
        AospApp app = AospApp.get();
        return app != null ? app.packageManager : null;
    }

    /** ActivityInfo.getRealConfigChanged: an app targeting below API 13 also handles screen size
     *  changes itself, as it did before those flags existed. */
    public static int getRealConfigChanged(android.content.pm.ActivityInfo info) {
        return info.applicationInfo != null && info.applicationInfo.targetSdkVersion < 13
                ? info.configChanges | android.content.pm.ActivityInfo.CONFIG_SCREEN_SIZE
                        | android.content.pm.ActivityInfo.CONFIG_SMALLEST_SCREEN_SIZE
                : info.configChanges;
    }

    // ---- android.companion.virtual.VirtualDeviceManager: no virtual (streamed) devices here ----

    /** Every display is the default device's (Context.DEVICE_ID_DEFAULT). */
    public static int getDeviceIdForDisplayId(android.companion.virtual.VirtualDeviceManager vdm, int displayId) {
        return 0;
    }

    /** No virtual device exists. */
    public static boolean isValidVirtualDeviceId(android.companion.virtual.VirtualDeviceManager vdm, int deviceId) {
        return false;
    }

    // ---- android.app.Instrumentation ----

    private static boolean sBeforeOnCreateRan;

    /** ActivityThread.installContentProviders (private; handleBindApplication calls it after the
     *  Application is made and attached, when the app has providers), with the host's work right
     *  before it (AppHost.Start.afterApplicationAttach). */
    public static void installContentProviders(ActivityThread thread, android.content.Context context,
            java.util.List<android.content.pm.ProviderInfo> providers) {
        afterAttach(context);
        try {
            java.lang.invoke.MethodHandle h = java.lang.invoke.MethodHandles
                    .privateLookupIn(ActivityThread.class, java.lang.invoke.MethodHandles.lookup())
                    .findVirtual(ActivityThread.class, "installContentProviders",
                            java.lang.invoke.MethodType.methodType(void.class, android.content.Context.class,
                                    java.util.List.class));
            h.invoke(thread, context, providers);
        } catch (RuntimeException | Error e) {
            throw e;
        } catch (Throwable e) {
            throw new IllegalStateException(e);
        }
    }

    private static boolean sAfterAttachRan;

    private static void afterAttach(android.content.Context context) {
        AospApp a = AospApp.get();
        if (sAfterAttachRan || a == null || a.afterAttach == null) return;
        sAfterAttachRan = true;
        android.app.Application app = context instanceof android.app.Application
                ? (android.app.Application) context : ActivityThread.currentApplication();
        a.afterAttach.accept(app);
    }

    /** Instrumentation.callApplicationOnCreate, with the host's work right before the first one
     *  (AppHost.Start.beforeApplicationOnCreate), and AppHost.Start.afterApplicationAttach first when
     *  the app has no providers (installContentProviders is not called then). */
    public static void callApplicationOnCreate(android.app.Instrumentation instr, android.app.Application app) {
        AospApp a = AospApp.get();
        afterAttach(app);
        if (!sBeforeOnCreateRan && a != null && a.beforeOnCreate != null) {
            sBeforeOnCreateRan = true;
            a.beforeOnCreate.accept(app);
        }
        instr.callApplicationOnCreate(app);
    }

    /** FileUtils.sync: the file's data to the disk, false when that failed. */
    public static boolean sync(java.io.FileOutputStream stream) {
        try {
            if (stream != null) stream.getFD().sync();
            return true;
        } catch (java.io.IOException e) {
            return false;
        }
    }

    // ---- android.content.pm.PackageManager (hidden; the permission prompt) ----

    /** PackageManager.buildRequestPermissionsIntent: ACTION_REQUEST_PERMISSIONS with the names, for
     *  the permission controller (the task manager hands it to the host's prompt). */
    public static Intent buildRequestPermissionsIntent(android.content.pm.PackageManager pm, String[] permissions) {
        Intent intent = new Intent(android.content.pm.PackageManager.ACTION_REQUEST_PERMISSIONS);
        intent.putExtra(android.content.pm.PackageManager.EXTRA_REQUEST_PERMISSIONS_NAMES, permissions);
        return intent;
    }

    /** PackageManager.shouldShowRequestPermissionRationale: the grant store's answer (false when
     *  granted, or never asked, or fixed by the user). */
    public static boolean shouldShowRequestPermissionRationale(android.content.pm.PackageManager pm, String permission) {
        try {
            return com.macapk.loader.SpecialAccessStore.shouldShowRationale(permission);
        } catch (RuntimeException e) {
            return false;
        }
    }

    /** TextUtils.formatSimple (Win.apk's TextUtils lacks it): String.format's %s, %d, %b, %x, %c,
     *  which is all formatSimple supports. */
    public static String formatSimple(String format, Object... args) {
        return String.format(java.util.Locale.ROOT, format, args);
    }

    /** AppOpsManager.toReceiverId (Win.apk's AppOpsManager lacks it): a registration's id for the app-op
     *  attribution of a receiver, its class and identity. */
    public static String toReceiverId(Object obj) {
        return obj == null ? null : obj.getClass().getName() + "@" + System.identityHashCode(obj);
    }
}
