// android.app.AppComponentFactory (AOSP API 28).
//
// The manifest-pluggable hook AOSP's ActivityThread/Instrumentation route
// component construction through: getFactory(pkg).instantiateApplication/
// instantiateActivity/instantiateService/instantiateProvider/
// instantiateReceiver/instantiateClassLoader. Apps opt in to a custom
// factory via <application android:appComponentFactory="...">, and
// androidx.core auto-merges androidx.core.app.CoreComponentFactory into
// virtually every AndroidX app's manifest. CoreComponentFactory overrides
// these methods and calls super.instantiateXxx(...) for the real
// construction step, so the default bodies below must exist with these
// EXACT signatures even before any Mac.apk call site routes through this
// class -- otherwise resolving CoreComponentFactory's super call throws
// NoSuchMethodError the first time anything invokes it.
//
// Previously this was a v1.0.92 auto-generated empty stub (ctor only, zero
// methods). Mac.apk's own loader (ActivityHost.performLaunch, Boot's
// Application/ContentProvider bring-up, ServiceRegistry) still does direct
// reflection today rather than consulting this factory or parsing the
// manifest's android:appComponentFactory attribute -- routing those call
// sites through here, and reading the manifest attribute, is a separate,
// larger cross-file change tracked outside this file. This class itself is
// now filled in to be AOSP-correct and safe to route through once that
// lands.
//
// Ref: https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/main/core/java/android/app/AppComponentFactory.java
package android.app;

import android.content.BroadcastReceiver;
import android.content.ContentProvider;
import android.content.Intent;
import android.content.pm.ApplicationInfo;

public class AppComponentFactory {

    // AOSP exposes this as the factory ActivityThread falls back to for any
    // package that declares no custom android:appComponentFactory.
    public static final AppComponentFactory DEFAULT = new AppComponentFactory();

    public AppComponentFactory() {}

    public ClassLoader instantiateClassLoader(ClassLoader cl, ApplicationInfo aInfo) {
        return cl;
    }

    // AOSP's default bodies deliberately use the deprecated Class#newInstance()
    // rather than Constructor#newInstance(): it requires a public no-arg
    // constructor and rethrows InstantiationException/IllegalAccessException
    // unwrapped, instead of Constructor#newInstance()'s
    // InvocationTargetException wrapping. Overriders that call
    // super.instantiateXxx(...) and catch these specific checked types (the
    // androidx.core.app.CoreComponentFactory pattern: wrap-and-rethrow around
    // the super call) depend on that exact exception shape, so we mirror
    // AOSP's choice verbatim rather than "modernizing" it.

    /** The instantiation primitive, shared by every construction site that must
     *  honour MACAPK_ACTIVITYTHREAD_INSTANTIATE -- including
     *  LoadedApk.createAppFactory, which instantiates the APP'S OWN factory
     *  class and is therefore a <clinit> trigger in its own right.
     *
     *  That sharing is not tidiness. Measured on the Play-built Minecraft: the
     *  app's declared androidx.core.app.CoreComponentFactory is ITSELF a
     *  pairip-protected class, so createAppFactory is the FIRST site whose
     *  <clinit> enters the guest VM. An "mh" arm that covered only
     *  instantiateApplication left that site on the 10-frame Class.newInstance
     *  path and silently produced a VOID A/B -- the arm confirmed as engaged
     *  while the frames it was supposed to change were untouched. */
    @SuppressWarnings("deprecation")
    static Object macapkNewInstance(Class<?> c)
            throws InstantiationException, IllegalAccessException {
        return c.newInstance();
    }

    public Application instantiateApplication(ClassLoader cl, String className)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return (Application) macapkNewInstance(cl.loadClass(className));
    }

    @SuppressWarnings("deprecation")
    public Activity instantiateActivity(ClassLoader cl, String className, Intent intent)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return (Activity) cl.loadClass(className).newInstance();
    }

    @SuppressWarnings("deprecation")
    public BroadcastReceiver instantiateReceiver(ClassLoader cl, String className, Intent intent)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return (BroadcastReceiver) cl.loadClass(className).newInstance();
    }

    @SuppressWarnings("deprecation")
    public Service instantiateService(ClassLoader cl, String className, Intent intent)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return (Service) cl.loadClass(className).newInstance();
    }

    @SuppressWarnings("deprecation")
    public ContentProvider instantiateProvider(ClassLoader cl, String className)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return (ContentProvider) cl.loadClass(className).newInstance();
    }
}
