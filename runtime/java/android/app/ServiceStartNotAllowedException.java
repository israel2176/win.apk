// android.app.ServiceStartNotAllowedException -- the abstract base of the
// "you may not start this service now" exceptions (API 31).
//   AOSP: frameworks/base/core/java/android/app/ServiceStartNotAllowedException.java
//         (android14-release), mirrored member for member.
//
// Why it exists here although Mac.apk never throws it: 13 of the 62 corpus
// apps (debug/fwgap_census.py, column D) carry their OWN R8 "API stub" of this
// class, because they catch it and were built with a minSdk that predates it.
// On a device the boot classpath wins and the stub is dead code; here, with no
// framework copy, the app's stub loaded instead and our
// ForegroundServiceStartNotAllowedException could not be caught as one. The
// hierarchy is the contract: catch (ServiceStartNotAllowedException) must see
// both subclasses. (That also needs ForegroundServiceStartNotAllowedException
// re-parented onto this class; the patch is in
// docs/fwgap-new-platform-classes_2026-09-26.md.)
//
// AOSP's @hide static newInstance(boolean, String) is here too: AOSP's
// ContextImpl.startServiceCommon throws what it returns when a start is refused.
package android.app;

public abstract class ServiceStartNotAllowedException extends IllegalStateException {
    ServiceStartNotAllowedException(String message) {
        super(message);
    }

    /** {@hide} AOSP: the foreground or the background exception for a refused start. */
    public static ServiceStartNotAllowedException newInstance(boolean foreground, String message) {
        if (foreground) return new ForegroundServiceStartNotAllowedException(message);
        return new BackgroundServiceStartNotAllowedException(message);
    }

    /**
     * AOSP returns null unconditionally: "Cause" is used for clustering crash
     * reports and developers did not want it for this exception (b/210890426).
     */
    @Override
    public synchronized Throwable getCause() {
        return null;
    }
}
