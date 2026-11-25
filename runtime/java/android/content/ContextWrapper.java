// ContextWrapper -- forwarding base for Activity, Service, Application.
// Real Android delegates almost everything to a "base context" set by
// the framework's Activity instantiation code. Mac.apk's Activity
// instantiates with `attachBaseContext(applicationContext)`; everything
// resolves the app context.
package android.content;

import android.content.res.AssetManager;
import android.content.res.Resources;

import java.io.File;

public class ContextWrapper extends Context {
    private Context baseContext;

    public ContextWrapper(Context base) { this.baseContext = base; }
    protected void attachBaseContext(Context base) { this.baseContext = base; }
    public Context getBaseContext() { return baseContext; }
    /** The base Context itself, past any getBaseContext() override: WhatsApp's
     *  AppShell returns null there until its own attachBaseContext has run, and
     *  Boot must still dispatch that attachBaseContext (com.macapk.loader.Boot). */
    public final Context macapkRawBaseContext() { return baseContext; }

    @Override public String getPackageName() {
        if (baseContext != null) {
            String p = baseContext.getPackageName();
            if (p != null && !p.isEmpty()) return p;
        }
        // A real Context always has a package name; fall back to the process
        // Application singleton rather than "" (Unity caches this as its app
        // identifier; an empty one crashes its PlayerPrefs path build).
        android.app.Application a = com.macapk.loader.app.Apps.current();
        String p = (a != null) ? a.getPackageName() : null;
        return (p != null) ? p : "";
    }
    @Override public Resources getResources() {
        return baseContext != null ? baseContext.getResources() : null;
    }
    @Override public AssetManager getAssets() {
        return baseContext != null ? baseContext.getAssets() : null;
    }
    @Override public Context getApplicationContext() {
        return baseContext != null ? baseContext.getApplicationContext() : this;
    }
    @Override public ClassLoader getClassLoader() {
        return baseContext != null ? baseContext.getClassLoader()
                                   : getClass().getClassLoader();
    }
    @Override public File getFilesDir() {
        return baseContext != null ? baseContext.getFilesDir() : new File(".");
    }
    @Override public File getCacheDir() {
        return baseContext != null ? baseContext.getCacheDir() : new File(".");
    }
    @Override public File getDir(String name, int mode) {
        return baseContext != null ? baseContext.getDir(name, mode) : new File(".");
    }

    /** Forward to baseContext, but fall back to Context's own
     *  switch (which provides stubs for JobScheduler, ActivityManager,
     *  LAYOUT_INFLATER_SERVICE) when baseContext is null or returns
     *  null. Without the fallback, Application (which sets
     *  baseContext=null in its no-arg ctor) returns null for every
     *  system service and apps that cast the result NPE. */
    private static final ThreadLocal<java.util.IdentityHashMap<ContextWrapper, String>> sActiveGetService =
        ThreadLocal.withInitial(java.util.IdentityHashMap::new);

    @Override public Object getSystemService(String name) {
        // v1.0.102 diag: trace the CW path so we can localize where a
        // getSystemService("jobscheduler") request is going.
        Object svc = null;
        String via = "super";
        if (baseContext != null) {
            // A wrapper this runtime builds AROUND an app context (the
            // createConfigurationContext and storage-context wrappers) has the
            // app as its base, where AOSP's would be a ContextImpl that never
            // calls back into the app. An Application whose getSystemService
            // asks such a wrapper of itself (Google Play services'
            // GmsApplication) then cycled app -> wrapper -> app until a
            // StackOverflowError killed the thread. Re-entering the same wrapper
            // for the same name on one thread can only be that cycle: answer it
            // from the innermost base context, the ContextImpl's place.
            java.util.IdentityHashMap<ContextWrapper, String> active = sActiveGetService.get();
            if (name != null && name.equals(active.get(this))) {
                Context inner = baseContext;
                while (inner instanceof ContextWrapper && ((ContextWrapper) inner).baseContext != null) {
                    inner = ((ContextWrapper) inner).baseContext;
                }
                return inner instanceof ContextWrapper ? super.getSystemService(name)
                                                       : inner.getSystemService(name);
            }
            String outer = active.put(this, name);
            try {
                svc = baseContext.getSystemService(name);
            } finally {
                if (outer != null) active.put(this, outer); else active.remove(this);
            }
            via = "baseContext(" + baseContext.getClass().getSimpleName() + ")";
        }
        if (svc == null) {
            svc = super.getSystemService(name);
            via += "->super";
        }
        if ("jobscheduler".equals(name) || "1".equals(System.getenv("MACAPK_TRACE_GETSVC"))) {
            try {
                com.macapk.loader.NativeLog.i("ContextWrapper",
                    getClass().getSimpleName() + ".getSystemService(\"" + name + "\") -> "
                    + (svc == null ? "null" : svc.getClass().getName())
                    + " via " + via);
            } catch (Throwable ignored) {}
        }
        return svc;
    }
    @Override public SharedPreferences getSharedPreferences(String name, int mode) {
        return baseContext != null ? baseContext.getSharedPreferences(name, mode) : null;
    }
    // Forward to the base context's populated ApplicationInfo /
    // PackageManager. Without these overrides, ContextWrapper inherited
    // Context's empty stubs, which broke cocos2d-x's
    // Cocos2dxHelper.nativeSetApkPath -- it passed null sourceDir to
    // native code, so the engine had no APK path to mmap-load asset
    // resources from, causing every texture lookup to silently fail.
    @Override public android.content.pm.ApplicationInfo getApplicationInfo() {
        // getApplicationInfo() is called on hot paths (per-child resource /
        // theme resolution runs it every frame). An unconditional INFO log here
        // floods the launcher log -- tens of thousands of lines/min on a View
        // app -- which both buries real signal and adds per-call string +
        // I/O pressure. Gate it behind an env var (default silent); the real
        // diagnostic value (which Context subclass is asked) is preserved when
        // MACAPK_TRACE_APPINFO is set.
        if (System.getenv("MACAPK_TRACE_APPINFO") != null) {
            com.macapk.loader.NativeLog.i("Mac.apk.CW",
                "getApplicationInfo() on " + this.getClass().getSimpleName());
        }
        return baseContext != null
            ? baseContext.getApplicationInfo()
            : super.getApplicationInfo();
    }
    @Override public android.content.pm.PackageManager getPackageManager() {
        android.content.pm.PackageManager pm = baseContext != null
            ? baseContext.getPackageManager()
            : super.getPackageManager();
        // On real Android getPackageManager() is never null for an
        // attached context, so a null here is always a framework lie
        // that will NPE somewhere downstream (appcompat probes it
        // during attachBaseContext2, e.g.). Name the context class so
        // the next bring-up doesn't have to rediscover the chain.
        if (pm == null) {
            com.macapk.loader.NativeLog.w("Mac.apk.CW",
                "getPackageManager() -> null on " + getClass().getName()
                + " (base=" + (baseContext == null ? "null"
                              : baseContext.getClass().getName()) + ")");
        }
        return pm;
    }
    // cocos2d-x v2's Cocos2dxActivity uses getPackageCodePath() to
    // hand the APK path to native code. Activity inherits this from
    // ContextWrapper; without delegation the call walks up the
    // ContextWrapper chain to Context's default empty-string return,
    // and cocos's s_pZipFile is never initialized.
    @Override public String getPackageCodePath() {
        return baseContext != null
            ? baseContext.getPackageCodePath()
            : super.getPackageCodePath();
    }
    @Override public String getPackageResourcePath() {
        return baseContext != null
            ? baseContext.getPackageResourcePath()
            : super.getPackageResourcePath();
    }

    // ---- AOSP ContextWrapper delegation: theme + content resolver -------
    // aosp-mirror/platform_frameworks_base android16-release
    // core/java/android/content/ContextWrapper.java:
    //   136-138  getContentResolver()  -> mBase.getContentResolver()
    //   156-158  setTheme(int resid)   -> mBase.setTheme(resid)
    //   168-170  getTheme()            -> mBase.getTheme()
    // A ContextWrapper on Android owns NO theme and NO ContentResolver of its
    // own; both are the base context's. Inheriting Context's concrete versions
    // gave every plain wrapper a private Theme built from
    // macapkSelectDefaultTheme() and a private ContentResolver, so a wrapper
    // resolved ?attr against Theme.DeviceDefault instead of the app's real
    // theme and observers registered on it never saw a notifyChange made
    // through the activity.
    // The null-base fallback to super is the Application no-arg-ctor window
    // (before attachBaseContext installs the terminator).
    @Override public ContentResolver getContentResolver() {
        return baseContext != null ? baseContext.getContentResolver()
                                   : super.getContentResolver();
    }
    @Override public void setTheme(int resid) {
        if (baseContext != null) baseContext.setTheme(resid);
        else super.setTheme(resid);
    }
    @Override public Resources.Theme getTheme() {
        return baseContext != null ? baseContext.getTheme() : super.getTheme();
    }

    // ---- AOSP ContextWrapper: device-/credential-protected storage flavor is
    // the base context's. A ContextWrapper owns no storage of its own, so it
    // delegates all four to mBase (android14-release ContextWrapper.java). This
    // matters so a ContextThemeWrapper / createConfigurationContext wrapper
    // around a device-protected StorageContext still reports DE and still
    // resolves file APIs to the DE area, rather than inheriting Context's
    // credential-protected defaults.
    @Override public boolean isDeviceProtectedStorage() {
        return baseContext != null ? baseContext.isDeviceProtectedStorage() : super.isDeviceProtectedStorage();
    }
    @Override public boolean isCredentialProtectedStorage() {
        return baseContext != null ? baseContext.isCredentialProtectedStorage() : super.isCredentialProtectedStorage();
    }
    @Override public Context createDeviceProtectedStorageContext() {
        return baseContext != null ? baseContext.createDeviceProtectedStorageContext()
                                   : super.createDeviceProtectedStorageContext();
    }
    @Override public Context createCredentialProtectedStorageContext() {
        return baseContext != null ? baseContext.createCredentialProtectedStorageContext()
                                   : super.createCredentialProtectedStorageContext();
    }

    // ---- AOSP ContextWrapper: starts, services and binds are the base context's (ContextImpl's,
    // which go to the activity manager). With no base yet, Context's own (the application's).
    @Override public void startActivity(Intent intent) {
        if (baseContext != null) baseContext.startActivity(intent);
        else super.startActivity(intent);
    }
    @Override public void startActivity(Intent intent, android.os.Bundle options) {
        if (baseContext != null) baseContext.startActivity(intent, options);
        else super.startActivity(intent, options);
    }
    @Override public void startActivities(Intent[] intents) {
        if (baseContext != null) baseContext.startActivities(intents);
        else super.startActivities(intents);
    }
    @Override public void startActivities(Intent[] intents, android.os.Bundle options) {
        if (baseContext != null) baseContext.startActivities(intents, options);
        else super.startActivities(intents, options);
    }
    @Override public ComponentName startService(Intent service) {
        if (baseContext != null) return baseContext.startService(service);
        else return super.startService(service);
    }
    @Override public ComponentName startForegroundService(Intent service) {
        if (baseContext != null) return baseContext.startForegroundService(service);
        else return super.startForegroundService(service);
    }
    @Override public boolean stopService(Intent service) {
        if (baseContext != null) return baseContext.stopService(service);
        else return super.stopService(service);
    }
    @Override public boolean bindService(Intent service, ServiceConnection conn, int flags) {
        if (baseContext != null) return baseContext.bindService(service, conn, flags);
        else return super.bindService(service, conn, flags);
    }
    @Override public boolean bindService(Intent service, ServiceConnection conn, BindServiceFlags flags) {
        if (baseContext != null) return baseContext.bindService(service, conn, flags);
        else return super.bindService(service, conn, flags);
    }
    @Override public boolean bindService(Intent service, int flags, java.util.concurrent.Executor executor, ServiceConnection conn) {
        if (baseContext != null) return baseContext.bindService(service, flags, executor, conn);
        else return super.bindService(service, flags, executor, conn);
    }
    @Override public boolean bindService(Intent service, BindServiceFlags flags, java.util.concurrent.Executor executor, ServiceConnection conn) {
        if (baseContext != null) return baseContext.bindService(service, flags, executor, conn);
        else return super.bindService(service, flags, executor, conn);
    }
    @Override public boolean bindIsolatedService(Intent service, int flags, String instanceName, java.util.concurrent.Executor executor, ServiceConnection conn) {
        if (baseContext != null) return baseContext.bindIsolatedService(service, flags, instanceName, executor, conn);
        else return super.bindIsolatedService(service, flags, instanceName, executor, conn);
    }
    @Override public boolean bindIsolatedService(Intent service, BindServiceFlags flags, String instanceName, java.util.concurrent.Executor executor, ServiceConnection conn) {
        if (baseContext != null) return baseContext.bindIsolatedService(service, flags, instanceName, executor, conn);
        else return super.bindIsolatedService(service, flags, instanceName, executor, conn);
    }
    @Override public boolean bindServiceAsUser(Intent service, ServiceConnection conn, int flags, android.os.UserHandle user) {
        if (baseContext != null) return baseContext.bindServiceAsUser(service, conn, flags, user);
        else return super.bindServiceAsUser(service, conn, flags, user);
    }
    @Override public void unbindService(ServiceConnection conn) {
        if (baseContext != null) baseContext.unbindService(conn);
        else super.unbindService(conn);
    }
    @Override public void updateServiceGroup(ServiceConnection conn, int group, int importance) {
        if (baseContext != null) baseContext.updateServiceGroup(conn, group, importance);
        else super.updateServiceGroup(conn, group, importance);
    }

    // ---- AOSP ContextWrapper: broadcasts and receivers are the base context's too.
    @Override public void sendBroadcast(Intent intent) {
        if (baseContext != null) baseContext.sendBroadcast(intent);
        else super.sendBroadcast(intent);
    }
    @Override public void sendBroadcast(Intent intent, String receiverPermission) {
        if (baseContext != null) baseContext.sendBroadcast(intent, receiverPermission);
        else super.sendBroadcast(intent, receiverPermission);
    }
    @Override public void sendBroadcast(Intent intent, String receiverPermission, android.os.Bundle options) {
        if (baseContext != null) baseContext.sendBroadcast(intent, receiverPermission, options);
        else super.sendBroadcast(intent, receiverPermission, options);
    }
    @Override public void sendBroadcastAsUser(Intent intent, android.os.UserHandle user) {
        if (baseContext != null) baseContext.sendBroadcastAsUser(intent, user);
        else super.sendBroadcastAsUser(intent, user);
    }
    @Override public void sendBroadcastAsUser(Intent intent, android.os.UserHandle user, String receiverPermission) {
        if (baseContext != null) baseContext.sendBroadcastAsUser(intent, user, receiverPermission);
        else super.sendBroadcastAsUser(intent, user, receiverPermission);
    }
    @Override public void sendBroadcastWithMultiplePermissions(Intent intent, String[] receiverPermissions) {
        if (baseContext != null) baseContext.sendBroadcastWithMultiplePermissions(intent, receiverPermissions);
        else super.sendBroadcastWithMultiplePermissions(intent, receiverPermissions);
    }
    @Override public void sendStickyBroadcast(Intent intent) {
        if (baseContext != null) baseContext.sendStickyBroadcast(intent);
        else super.sendStickyBroadcast(intent);
    }
    @Override public void sendStickyBroadcast(Intent intent, android.os.Bundle options) {
        if (baseContext != null) baseContext.sendStickyBroadcast(intent, options);
        else super.sendStickyBroadcast(intent, options);
    }
    @Override public void sendStickyBroadcastAsUser(Intent intent, android.os.UserHandle user) {
        if (baseContext != null) baseContext.sendStickyBroadcastAsUser(intent, user);
        else super.sendStickyBroadcastAsUser(intent, user);
    }
    @Override public void removeStickyBroadcast(Intent intent) {
        if (baseContext != null) baseContext.removeStickyBroadcast(intent);
        else super.removeStickyBroadcast(intent);
    }
    @Override public void removeStickyBroadcastAsUser(Intent intent, android.os.UserHandle user) {
        if (baseContext != null) baseContext.removeStickyBroadcastAsUser(intent, user);
        else super.removeStickyBroadcastAsUser(intent, user);
    }
    @Override public void sendOrderedBroadcast(Intent intent, String receiverPermission) {
        if (baseContext != null) baseContext.sendOrderedBroadcast(intent, receiverPermission);
        else super.sendOrderedBroadcast(intent, receiverPermission);
    }
    @Override public void sendOrderedBroadcast(Intent intent, String receiverPermission, android.os.Bundle options) {
        if (baseContext != null) baseContext.sendOrderedBroadcast(intent, receiverPermission, options);
        else super.sendOrderedBroadcast(intent, receiverPermission, options);
    }
    @Override public void sendOrderedBroadcast(Intent intent, String receiverPermission, android.os.Bundle options, android.content.BroadcastReceiver resultReceiver, android.os.Handler scheduler, int initialCode, String initialData, android.os.Bundle initialExtras) {
        if (baseContext != null) baseContext.sendOrderedBroadcast(intent, receiverPermission, options, resultReceiver, scheduler, initialCode, initialData, initialExtras);
        else super.sendOrderedBroadcast(intent, receiverPermission, options, resultReceiver, scheduler, initialCode, initialData, initialExtras);
    }
    @Override public void sendOrderedBroadcast(Intent intent, String receiverPermission, String receiverAppOp, android.content.BroadcastReceiver resultReceiver, android.os.Handler scheduler, int initialCode, String initialData, android.os.Bundle initialExtras) {
        if (baseContext != null) baseContext.sendOrderedBroadcast(intent, receiverPermission, receiverAppOp, resultReceiver, scheduler, initialCode, initialData, initialExtras);
        else super.sendOrderedBroadcast(intent, receiverPermission, receiverAppOp, resultReceiver, scheduler, initialCode, initialData, initialExtras);
    }
    @Override public void sendOrderedBroadcast(Intent intent, int initialCode, String receiverPermission, String receiverAppOp, android.content.BroadcastReceiver resultReceiver, android.os.Handler scheduler, String initialData, android.os.Bundle initialExtras, android.os.Bundle options) {
        if (baseContext != null) baseContext.sendOrderedBroadcast(intent, initialCode, receiverPermission, receiverAppOp, resultReceiver, scheduler, initialData, initialExtras, options);
        else super.sendOrderedBroadcast(intent, initialCode, receiverPermission, receiverAppOp, resultReceiver, scheduler, initialData, initialExtras, options);
    }
    @Override public void sendOrderedBroadcast(Intent intent, String receiverPermission, android.content.BroadcastReceiver resultReceiver, android.os.Handler scheduler, int initialCode, String initialData, android.os.Bundle initialExtras) {
        if (baseContext != null) baseContext.sendOrderedBroadcast(intent, receiverPermission, resultReceiver, scheduler, initialCode, initialData, initialExtras);
        else super.sendOrderedBroadcast(intent, receiverPermission, resultReceiver, scheduler, initialCode, initialData, initialExtras);
    }
    @Override public void sendOrderedBroadcastAsUser(Intent intent, android.os.UserHandle user, String receiverPermission, android.content.BroadcastReceiver resultReceiver, android.os.Handler scheduler, int initialCode, String initialData, android.os.Bundle initialExtras) {
        if (baseContext != null) baseContext.sendOrderedBroadcastAsUser(intent, user, receiverPermission, resultReceiver, scheduler, initialCode, initialData, initialExtras);
        else super.sendOrderedBroadcastAsUser(intent, user, receiverPermission, resultReceiver, scheduler, initialCode, initialData, initialExtras);
    }
    @Override public void sendStickyOrderedBroadcast(Intent intent, android.content.BroadcastReceiver resultReceiver, android.os.Handler scheduler, int initialCode, String initialData, android.os.Bundle initialExtras) {
        if (baseContext != null) baseContext.sendStickyOrderedBroadcast(intent, resultReceiver, scheduler, initialCode, initialData, initialExtras);
        else super.sendStickyOrderedBroadcast(intent, resultReceiver, scheduler, initialCode, initialData, initialExtras);
    }
    @Override public void sendStickyOrderedBroadcastAsUser(Intent intent, android.os.UserHandle user, android.content.BroadcastReceiver resultReceiver, android.os.Handler scheduler, int initialCode, String initialData, android.os.Bundle initialExtras) {
        if (baseContext != null) baseContext.sendStickyOrderedBroadcastAsUser(intent, user, resultReceiver, scheduler, initialCode, initialData, initialExtras);
        else super.sendStickyOrderedBroadcastAsUser(intent, user, resultReceiver, scheduler, initialCode, initialData, initialExtras);
    }
    @Override public Intent registerReceiver(android.content.BroadcastReceiver receiver, android.content.IntentFilter filter) {
        if (baseContext != null) return baseContext.registerReceiver(receiver, filter);
        else return super.registerReceiver(receiver, filter);
    }
    @Override public Intent registerReceiver(android.content.BroadcastReceiver receiver, android.content.IntentFilter filter, int flags) {
        if (baseContext != null) return baseContext.registerReceiver(receiver, filter, flags);
        else return super.registerReceiver(receiver, filter, flags);
    }
    @Override public Intent registerReceiver(android.content.BroadcastReceiver receiver, android.content.IntentFilter filter, String broadcastPermission, android.os.Handler scheduler) {
        if (baseContext != null) return baseContext.registerReceiver(receiver, filter, broadcastPermission, scheduler);
        else return super.registerReceiver(receiver, filter, broadcastPermission, scheduler);
    }
    @Override public Intent registerReceiver(android.content.BroadcastReceiver receiver, android.content.IntentFilter filter, String broadcastPermission, android.os.Handler scheduler, int flags) {
        if (baseContext != null) return baseContext.registerReceiver(receiver, filter, broadcastPermission, scheduler, flags);
        else return super.registerReceiver(receiver, filter, broadcastPermission, scheduler, flags);
    }
    @Override public void unregisterReceiver(android.content.BroadcastReceiver receiver) {
        if (baseContext != null) baseContext.unregisterReceiver(receiver);
        else super.unregisterReceiver(receiver);
    }
}
