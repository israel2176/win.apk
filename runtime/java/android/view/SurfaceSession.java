package android.view;

/**
 * AOSP's android.view.SurfaceSession: a connection to SurfaceFlinger that SurfaceControl.Builder
 * builds layers on. Win.apk ships AOSP's own class in lib/aosp-views.jar (its natives inert, as a
 * session has no native side here: AospViewsRewrite.INERT) and drops this one from the shipped
 * android-framework.jar (stage_runtime.sh, META-INF/winapk-aosp-shadowed.txt); it is here so that
 * the framework's SurfaceControl.Builder(SurfaceSession), which AOSP's views call, compiles with
 * AOSP's descriptor.
 */
public final class SurfaceSession {
    public SurfaceSession() {}

    /** AOSP kill(): the session's connection goes; the layers built on it stay until released. */
    public void kill() {}
}
