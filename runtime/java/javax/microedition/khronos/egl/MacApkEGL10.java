// javax.microedition.khronos.egl.MacApkEGL10 -- the EGL10 implementation
// handed back by EGLContext.getEGL().
//
// A real EGL10 over ANGLE, the way AOSP's EGLImpl is a thin JNI layer over
// libEGL (frameworks/base opengl/java/com/google/android/gles_jni/EGLImpl.java,
// android14-release), for every thread, as on Android: GLSurfaceView's GLThread,
// an app's own copy of it, an engine's render thread. It forwards to the
// android.opengl.EGL14/EGL15 natives (runtime/src/egl14_bridges.inc), which
// give any Java thread real ANGLE contexts and pbuffers:
//   - eglGetDisplay answers ONE display of our own, obtained with
//     eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, K, ...), where K is a
//     private native-display token (on the Windows host: Direct3D 11 and
//     EGL_D3D11_ONLY_DISPLAY_ANGLE; see obtainWindowsDisplay). It is never the
//     display the framework's graphics or a native engine use: ANGLE keeps one
//     device context per display, and the app's GL thread and another thread
//     on one display had to share it (ANGLE keys its platform displays on the
//     native display, among others: src/libANGLE/Display.cpp
//     GetDisplayFromNativeDisplay, so a distinct value buys a distinct display);
//   - configs are ANGLE's own (the driver's list is the device's answer),
//     including the MSAA ones Telegram's device branch asks for;
//   - eglCreateContext / eglCreatePbufferSurface / eglMakeCurrent /
//     eglSwapBuffers / eglQuery* / eglDestroy* are real;
//   - eglCreateWindowSurface dispatches like EGLImpl: a SurfaceTexture, or a
//     Surface built from one, gets a real window (MacApkGlWindow: a pbuffer
//     read back and queued into THAT SurfaceTexture at each swap); a
//     SurfaceView, its SurfaceHolder or its Surface gets the host's window
//     surface on the SurfaceView (viewWindow: a pbuffer the host reads back at
//     each swap and shows under the window, through the view's hole); any
//     other object throws EGLImpl's UnsupportedOperationException;
//   - handles compare by native handle (EGLContextImpl/EGLSurfaceImpl
//     equals), and eglGetCurrent* return the very objects the thread made
//     current (EGLImpl caches its current wrappers the same way).
// What is not real, and why:
//   - a thread with NO context of ours current, but with one of the
//     framework's, gets stable SENTINEL objects from eglGetCurrent*, and
//     eglMakeCurrent(NONE, NONE, NONE) there is a no-op, so it can never unbind
//     the framework's context; when our context is released on a thread that
//     had a FOREIGN context current before we bound, that one is re-bound. A
//     thread with nothing current at all gets EGL_NO_CONTEXT / EGL_NO_DISPLAY /
//     EGL_NO_SURFACE, as on Android (see nothingCurrent);
//   - eglCreatePixmapSurface and eglCopyBuffers: ANGLE has no pixmaps and
//     AOSP's eglCopyBuffers returns false ("TODO: Implement this").
//
// Until November 2026 a SurfaceView's own GL thread (an app's copy of
// GLSurfaceView) was refused its context here, so that it died and the view's
// renderer, found by reflection, was driven from the host's render thread
// instead: Mac.apk could not give such a thread a window. That killed every
// GL thread whose renderer the reflection could not find (an obfuscated app's,
// one held by a helper object): Waze's map stayed black. The LEGACY contract
// below is now only what a JVM without the EGL14 natives answers.
package javax.microedition.khronos.egl;

import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.EGL15;
import com.macapk.loader.NativeLog;
import com.macapk.loader.view.ViewHost;

public final class MacApkEGL10 implements EGL10 {
    private static final String TAG = "Mac.apk.EGL10";

    public static final MacApkEGL10 INSTANCE = new MacApkEGL10();

    /** Set once when the EGL14/EGL15 natives turn out to be missing (a bare
     *  JVM without the runtime's bridges): every later call degrades to the
     *  legacy contract instead of throwing UnsatisfiedLinkError into an app. */
    private static volatile boolean sRealBroken;

    private static final EGLDisplay SENTINEL_DISPLAY = new EGLDisplay() {};
    private static final EGLSurface SENTINEL_SURFACE = new EGLSurface() {};
    // A stable non-null "current context" sentinel. libGDX stores the
    // context returned by eglGetCurrentContext in onSurfaceCreated and
    // compares it each resume to detect context loss; returning a stable
    // instance means it never thinks the context was lost.
    private static final EGLContext SENTINEL_CONTEXT = new EGLContext() {
        @Override public javax.microedition.khronos.opengles.GL getGL() { return null; }
    };

    /** One row of the config table. Caveat is EGL_NONE, surface type
     *  WINDOW|PBUFFER, renderable type ES1|ES2 for every row (mirrors
     *  common Android driver configs; we truthfully do NOT advertise
     *  ES3). */
    static final class Config extends EGLConfig {
        final int id, r, g, b, a, depth, stencil;
        Config(int id, int r, int g, int b, int a, int depth, int stencil) {
            this.id = id; this.r = r; this.g = g; this.b = b; this.a = a;
            this.depth = depth; this.stencil = stencil;
        }
        int bufferSize() { return r + g + b + a; }
    }

    // Color groups in DESCENDING total-color-bits order; within a
    // group, depth then stencil ASCENDING (EGL sort rules 7/8).
    private static final Config[] CONFIGS;
    static {
        int[][] colors = { {8,8,8,8}, {8,8,8,0}, {5,6,5,0} };
        int[] depths   = { 0, 16, 24 };
        int[] stencils = { 0, 8 };
        CONFIGS = new Config[colors.length * depths.length * stencils.length];
        int i = 0, id = 1;
        for (int[] c : colors)
            for (int d : depths)
                for (int s : stencils)
                    CONFIGS[i++] = new Config(id++, c[0], c[1], c[2], c[3], d, s);
    }

    /** The EGLConfig a GLSurfaceView renderer receives in onSurfaceCreated:
     *  the table row describing the window surface the renderer really draws
     *  into, angle_glue.cpp AngleInit's config (RGBA 8888, depth 24, stencil
     *  0). AOSP never passes null there: GLThread.guardedRun calls
     *  view.mRenderer.onSurfaceCreated(gl, mEglHelper.mEglConfig)
     *  (android14-release opengl/java/android/opengl/GLSurfaceView.java:1541),
     *  the config its EGLConfigChooser picked. A Kotlin renderer declares the
     *  parameter non-null and throws on null (LibretroDroid's GLRetroView,
     *  which then never initialised its core's GL state). eglGetConfigAttrib
     *  on it answers from this table row. MACAPK_GL_SURFACE_CONFIG=0 restores
     *  the old null. */
    public static EGLConfig macapkWindowSurfaceConfig() {
        for (Config c : CONFIGS)
            if (c.r == 8 && c.g == 8 && c.b == 8 && c.a == 8 && c.depth == 24 && c.stencil == 0)
                return c;
        return CONFIGS[0];
    }

    /** Thread-local last EGL error, read-and-cleared by eglGetError
     *  (spec semantics). */
    private static final ThreadLocal<Integer> LAST_ERROR =
        ThreadLocal.withInitial(() -> EGL_SUCCESS);
    private static void setError(int e) { LAST_ERROR.set(e); }

    private MacApkEGL10() {}

    // =====================================================================
    // REAL-contract state
    // =====================================================================

    // EGL_PLATFORM_ANGLE_ANGLE and its attributes (thirdparty/angle
    // include/EGL/eglext_angle.h).
    private static final int EGL_PLATFORM_ANGLE_ANGLE = 0x3202;
    private static final int EGL_PLATFORM_ANGLE_TYPE_ANGLE = 0x3203;
    private static final int EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE = 0x3489;
    /** The private native-display token that keys our display apart from the
     *  framework's and the NDK's (both EGL_DEFAULT_DISPLAY = 0): ASCII "JE10". */
    private static final long DISPLAY_TOKEN = 0x4A453130L;
    private static final int EGL_CLIENT_APIS = 0x308D;

    /** EGLDisplayImpl: our one ANGLE display, AOSP's refcounted
     *  initialize/terminate (frameworks/native opengl/libs/EGL/
     *  egl_display.cpp: "display termination is ref-counted"). */
    static final class RealDisplay extends EGLDisplay {
        final android.opengl.EGLDisplay h;
        int refs;   // guarded by this
        RealDisplay(android.opengl.EGLDisplay h) { this.h = h; }
        @Override public boolean equals(Object o) {
            return o instanceof RealDisplay && ((RealDisplay) o).h.getNativeHandle() == h.getNativeHandle();
        }
        @Override public int hashCode() { return Long.hashCode(h.getNativeHandle()); }
    }

    /** EGLConfigImpl. One wrapper per native config, so identity is stable. */
    static final class RealConfig extends EGLConfig {
        final RealDisplay dpy;
        final android.opengl.EGLConfig h;
        RealConfig(RealDisplay d, android.opengl.EGLConfig h) { dpy = d; this.h = h; }
        @Override public boolean equals(Object o) {
            return o instanceof RealConfig && ((RealConfig) o).h.getNativeHandle() == h.getNativeHandle();
        }
        @Override public int hashCode() { return Long.hashCode(h.getNativeHandle()); }
    }

    /** EGLContextImpl: equals by native handle (EGLContextImpl.java:37-44). */
    static final class RealContext extends EGLContext {
        final RealDisplay dpy;
        final android.opengl.EGLContext h;
        final int clientVersion;
        RealContext(RealDisplay d, android.opengl.EGLContext h, int v) {
            dpy = d; this.h = h; clientVersion = v;
        }
        /** AOSP's EGLContextImpl hands back a GLImpl (GL10/GL11 over the
         *  calling thread's current context); so does this. */
        @Override public javax.microedition.khronos.opengles.GL getGL() {
            return com.google.android.gles_jni.GLImpl.INSTANCE;
        }
        @Override public boolean equals(Object o) {
            return o instanceof RealContext && ((RealContext) o).h.getNativeHandle() == h.getNativeHandle();
        }
        @Override public int hashCode() { return Long.hashCode(h.getNativeHandle()); }
    }

    /** EGLSurfaceImpl: a pbuffer, or a SurfaceTexture window whose native
     *  surface can be replaced by a resize (equals follows the CURRENT native
     *  handle; hashCode is fixed at creation, and eglGetCurrentSurface hands
     *  back this same object, so no second wrapper can disagree). */
    static final class RealSurface extends EGLSurface {
        final RealDisplay dpy;
        final android.opengl.EGLSurface pbuffer;   // null for a window
        final MacApkGlWindow window;               // null for a pbuffer
        // Win.apk: a SurfaceView window made for an app's own GL thread
        // (viewWindow below); `pbuffer` is its surface, swapped by the host.
        final boolean viewWindow;
        private final int hash;
        volatile boolean destroyed;
        RealSurface(RealDisplay d, android.opengl.EGLSurface pbuffer, MacApkGlWindow window) {
            this(d, pbuffer, window, false);
        }
        RealSurface(RealDisplay d, android.opengl.EGLSurface pbuffer, MacApkGlWindow window, boolean viewWindow) {
            dpy = d; this.pbuffer = pbuffer; this.window = window; this.viewWindow = viewWindow;
            hash = Long.hashCode(nativeSurface().getNativeHandle());
        }
        android.opengl.EGLSurface nativeSurface() {
            return window != null ? window.nativeSurface() : pbuffer;
        }
        @Override public boolean equals(Object o) {
            return o instanceof RealSurface
                && ((RealSurface) o).nativeSurface().getNativeHandle() == nativeSurface().getNativeHandle();
        }
        @Override public int hashCode() { return hash; }
    }

    /** What THIS thread has current through us, plus the foreign binding (the
     *  framework's, on OS-main) we displaced, to put back on release. */
    private static final class Current {
        RealDisplay dpy;
        RealSurface draw, read;
        RealContext ctx;
        android.opengl.EGLDisplay foreignDpy;
        android.opengl.EGLSurface foreignDraw, foreignRead;
        android.opengl.EGLContext foreignCtx;
        void clear() {
            dpy = null; draw = null; read = null; ctx = null;
            foreignDpy = null; foreignDraw = null; foreignRead = null; foreignCtx = null;
        }
    }
    private static final ThreadLocal<Current> CURRENT = ThreadLocal.withInitial(Current::new);
    /** True once this thread made a native EGL call through us; only then does
     *  eglGetError consult ANGLE's per-thread error. */
    private static final ThreadLocal<Boolean> NATIVE_USED = new ThreadLocal<>();

    private static RealDisplay sDisplay;   // guarded by MacApkEGL10.class
    private static boolean sWindowsTried;  // guarded by MacApkEGL10.class
    private static final java.util.concurrent.ConcurrentHashMap<Long, RealConfig> sConfigs =
        new java.util.concurrent.ConcurrentHashMap<>();

    private static boolean realOn() { return !sRealBroken; }

    private static void realBroken(Throwable t) {
        if (!sRealBroken) {
            sRealBroken = true;
            try {
                NativeLog.w(TAG, "EGL14/EGL15 natives unavailable (" + t
                    + "); EGL10 falls back to the legacy contract");
            } catch (Throwable ignored) {
                // NativeLog.print is itself native; in a bare JVM it is missing too.
            }
        }
    }

    /** Clears the Java-side error before a native call, so a stale Java error
     *  cannot mask what ANGLE reports for this call. */
    private static void nativeCall() {
        LAST_ERROR.set(EGL_SUCCESS);
        NATIVE_USED.set(Boolean.TRUE);
    }

    private static RealDisplay realDisplay(EGLDisplay d) {
        return (realOn() && d instanceof RealDisplay) ? (RealDisplay) d : null;
    }

    // ---- The Windows host (MACAPK_EGL_WINDOWS_BACKEND) ---------------------
    //
    // The Windows port's ANGLE (Chromium's libEGL / libGLESv2) has no Metal
    // backend: eglGetPlatformDisplay(TYPE=METAL) answers EGL_NO_DISPLAY with
    // EGL_BAD_ATTRIBUTE, so every thread fell to the legacy contract, whose
    // table has no multisample row and whose eglCreateContext always refuses.
    // Telegram's intro (IntroActivity's EGLThread: RGBA8888, depth 24,
    // EGL_SAMPLE_BUFFERS 1 / EGL_SAMPLES 4) got 0 configs and finished, so the
    // logo animation never drew. On Windows the display is the backend the host
    // renders with (windows/host/gl/gl_surface.cpp: Direct3D 11, then D3D11's
    // WARP rasterizer, then SwiftShader's Vulkan; WINAPK_ANGLE_BACKEND=d3d11 or
    // swiftshader narrows it the same way), and its configs are ANGLE's own,
    // multisample ones included.
    //
    // The native display is EGL_D3D11_ONLY_DISPLAY_ANGLE rather than the
    // private token: ANGLE on Windows validates it (Display::isValidNativeDisplay
    // accepts EGL_DEFAULT_DISPLAY, the three EGL_*_DISPLAY_ANGLE constants or a
    // window's HDC, and the token is none of them), and it still keys a display
    // apart from the host's (EGL_DEFAULT_DISPLAY), so the app's display, its
    // initialize/terminate and its errors stay its own. "0" restores the Metal
    // request, i.e. the legacy contract on Windows. macOS is unaffected.
    private static final boolean WINDOWS_HOST = System.mapLibraryName("x").endsWith(".dll");
    private static final boolean WINDOWS_BACKEND =
        WINDOWS_HOST;
    private static final int EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE = 0x3208;
    private static final int EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE = 0x3450;
    private static final int EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE = 0x3209;
    private static final int EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE = 0x320B;
    private static final int EGL_PLATFORM_ANGLE_DEVICE_TYPE_SWIFTSHADER_ANGLE = 0x3487;
    /** EGL_D3D11_ONLY_DISPLAY_ANGLE ((EGLNativeDisplayType)-3, eglext_angle.h). */
    private static final long EGL_D3D11_ONLY_DISPLAY_ANGLE = -3L;

    /** The Windows display: the first backend of the host's ladder that
     *  initializes, left initialized (the app's own eglInitialize then finds it
     *  so, as on a device where another client of libEGL got there first), or
     *  null when none does (the legacy contract answers, as before). */
    private static android.opengl.EGLDisplay obtainWindowsDisplay() {
        boolean sw = false, hw = false;
        java.util.List<long[]> ladder = new java.util.ArrayList<>();
        if (!sw) {
            ladder.add(new long[] { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
                    EGL_NONE });
        }
        if (!sw && !hw) {
            ladder.add(new long[] { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
                    EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE,
                    EGL_NONE });
        }
        if (!hw) {
            ladder.add(new long[] { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE,
                    EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_DEVICE_TYPE_SWIFTSHADER_ANGLE,
                    EGL_NONE });
        }
        for (long[] attribs : ladder) {
            android.opengl.EGLDisplay h = EGL15.eglGetPlatformDisplay(
                EGL_PLATFORM_ANGLE_ANGLE, EGL_D3D11_ONLY_DISPLAY_ANGLE, attribs, 0);
            if (h == null || h.getNativeHandle() == 0) continue;
            if (!EGL14.eglInitialize(h, null, 0, null, 0)) {
                EGL14.eglGetError();   // our probe's error, not the app's
                continue;
            }
            try {
                NativeLog.i(TAG, "EGL10 display: ANGLE "
                    + (attribs[1] == EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE ? "SwiftShader (Vulkan)"
                       : attribs.length > 3 ? "Direct3D 11 WARP" : "Direct3D 11")
                    + " (Windows host)");
            } catch (Throwable ignored) {}
            return h;
        }
        return null;
    }

    /** eglChooseConfig on the Windows display, when ANGLE has no config that
     *  meets a multisample request: the closest one it does have, the way a
     *  driver without that sample count answers -- fewer samples first
     *  (halving EGL_SAMPLES down to 2), then no multisample buffer at all.
     *  Every other attribute still has to be met. null when the request asked
     *  for no samples or nothing helps (the 0-config answer then stands). */
    private static int[] windowsMsaaFallback(RealDisplay d, int[] attribList) {
        if (!WINDOWS_BACKEND || attribList == null) return null;
        int samples = 0, sampleBuffers = 0;
        for (int i = 0; i + 1 < attribList.length && attribList[i] != EGL_NONE; i += 2) {
            if (attribList[i] == EGL_SAMPLES) samples = attribList[i + 1];
            if (attribList[i] == EGL_SAMPLE_BUFFERS) sampleBuffers = attribList[i + 1];
        }
        if (samples == EGL_DONT_CARE) samples = 0;
        if (sampleBuffers == EGL_DONT_CARE) sampleBuffers = 0;
        if (samples <= 0 && sampleBuffers <= 0) return null;
        java.util.List<int[]> tries = new java.util.ArrayList<>();
        for (int s = samples / 2; s >= 2; s /= 2) tries.add(withSamples(attribList, 1, s));
        tries.add(withSamples(attribList, 0, 0));
        int[] num = new int[1];
        for (int[] t : tries) {
            if (EGL14.eglChooseConfig(d.h, t, 0, null, 0, 0, num, 0) && num[0] > 0) return t;
        }
        EGL14.eglGetError();
        return null;
    }

    /** attribList with EGL_SAMPLE_BUFFERS / EGL_SAMPLES replaced. */
    private static int[] withSamples(int[] attribList, int sampleBuffers, int samples) {
        java.util.List<Integer> out = new java.util.ArrayList<>();
        for (int i = 0; i + 1 < attribList.length && attribList[i] != EGL_NONE; i += 2) {
            if (attribList[i] == EGL_SAMPLES || attribList[i] == EGL_SAMPLE_BUFFERS) continue;
            out.add(attribList[i]);
            out.add(attribList[i + 1]);
        }
        out.add(EGL_SAMPLE_BUFFERS); out.add(sampleBuffers);
        out.add(EGL_SAMPLES); out.add(samples);
        out.add(EGL_NONE);
        int[] a = new int[out.size()];
        for (int i = 0; i < a.length; i++) a[i] = out.get(i);
        return a;
    }

    private static RealDisplay obtainDisplay() {
        synchronized (MacApkEGL10.class) {
            if (sDisplay != null) return sDisplay;
            if (WINDOWS_BACKEND) {
                // Probed once: a machine with no usable backend keeps the
                // legacy contract without re-trying on every eglGetDisplay.
                if (sWindowsTried) return null;
                sWindowsTried = true;
                android.opengl.EGLDisplay h = obtainWindowsDisplay();
                if (h == null) return null;
                sDisplay = new RealDisplay(h);
                return sDisplay;
            }
            android.opengl.EGLDisplay h = EGL15.eglGetPlatformDisplay(
                EGL_PLATFORM_ANGLE_ANGLE, DISPLAY_TOKEN,
                new long[] {
                    EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
                    EGL_NONE,
                }, 0);
            if (h == null || h.getNativeHandle() == 0) return null;
            sDisplay = new RealDisplay(h);
            return sDisplay;
        }
    }

    private static RealConfig wrapConfig(RealDisplay d, android.opengl.EGLConfig c) {
        if (c == null) return null;
        return sConfigs.computeIfAbsent(c.getNativeHandle(), k -> new RealConfig(d, c));
    }

    private static boolean isNoSurface(EGLSurface s) {
        return s == null || s == EGL_NO_SURFACE || s == SENTINEL_SURFACE;
    }
    private static boolean isNoContext(EGLContext c) {
        return c == null || c == EGL_NO_CONTEXT || c == SENTINEL_CONTEXT;
    }

    /** The ANGLE surface for an app surface, EGL14.EGL_NO_SURFACE for "none",
     *  or null for an object we cannot bind (a destroyed or foreign surface). */
    private static android.opengl.EGLSurface nativeOf(EGLSurface s) {
        if (isNoSurface(s)) return EGL14.EGL_NO_SURFACE;
        if (s instanceof RealSurface && !((RealSurface) s).destroyed) {
            return ((RealSurface) s).nativeSurface();
        }
        return null;
    }

    private static int logBudgetReal = 24;
    private static synchronized boolean shouldLogReal() {
        if (logBudgetReal <= 0) return false;
        logBudgetReal--;
        return true;
    }

    // =====================================================================
    // EGL10
    // =====================================================================

    @Override public EGLDisplay eglGetDisplay(Object nativeDisplay) {
        if (!realOn()) return legacyGetDisplay(nativeDisplay);
        try {
            // EGLImpl: _eglGetDisplay ignores its argument and always answers
            // eglGetDisplay(EGL_DEFAULT_DISPLAY) (jni_eglGetDisplay), and
            // hands back the same cached object for the same handle.
            RealDisplay d = obtainDisplay();
            if (d != null) return d;
            NativeLog.w(TAG, "eglGetPlatformDisplay(ANGLE " + (WINDOWS_BACKEND ? "Windows" : "Metal")
                + ") failed; legacy EGL10 answers");
        } catch (Throwable t) {
            realBroken(t);
        }
        return legacyGetDisplay(nativeDisplay);
    }

    @Override public boolean eglInitialize(EGLDisplay display, int[] majorMinor) {
        RealDisplay d = realDisplay(display);
        if (d == null) return legacyInitialize(display, majorMinor);
        nativeCall();
        boolean ok = EGL14.eglInitialize(d.h, null, 0, null, 0);
        if (ok) {
            synchronized (d) { d.refs++; }
            // EGLImpl's JNI: "we're exposing only EGL 1.0" -- base[0] = 1,
            // base[1] = 0, whatever the driver speaks
            // (com_google_android_gles_jni_EGLImpl.cpp jni_eglInitialize).
            if (majorMinor != null && majorMinor.length >= 1) majorMinor[0] = 1;
            if (majorMinor != null && majorMinor.length >= 2) majorMinor[1] = 0;
        }
        return ok;
    }

    @Override public boolean eglTerminate(EGLDisplay display) {
        RealDisplay d = realDisplay(display);
        if (d == null) return legacyTerminate(display);
        // Android's libEGL refcounts initialize/terminate per display: only
        // the terminate matching the FIRST initialize reaches the driver, and
        // terminating an uninitialized display just returns true
        // (egl_display.cpp egl_display_t::terminate). The count itself lives
        // under EGL14 (the shim's __macapk_egl_display_ref, which every
        // EGL14.eglInitialize above fed), so each terminate goes down to it;
        // refs here only mirrors it for the uninitialized-display answer.
        synchronized (d) {
            if (d.refs == 0) return true;
            d.refs--;
            // The Windows host has no such shim (EGL14's display_ref lookup
            // finds nothing in ANGLE's DLLs), so every EGL14.eglTerminate would
            // reach ANGLE: the count kept here decides instead, and only the
            // last terminate goes down.
            if (WINDOWS_BACKEND && d.refs > 0) return true;
        }
        nativeCall();
        return EGL14.eglTerminate(d.h);
    }

    @Override public boolean eglChooseConfig(EGLDisplay display, int[] attribList,
            EGLConfig[] configs, int configSize, int[] numConfig) {
        RealDisplay d = realDisplay(display);
        if (d == null) return legacyChooseConfig(display, attribList, configs, configSize, numConfig);
        int size = configs == null ? 0 : Math.max(0, Math.min(configSize, configs.length));
        android.opengl.EGLConfig[] tmp = configs == null ? null : new android.opengl.EGLConfig[size];
        int[] num = new int[1];
        nativeCall();
        boolean ok = EGL14.eglChooseConfig(d.h, attribList, 0, tmp, 0, size, num, 0);
        int[] closest = (ok && num[0] == 0) ? windowsMsaaFallback(d, attribList) : null;
        if (closest != null) {
            ok = EGL14.eglChooseConfig(d.h, closest, 0, tmp, 0, size, num, 0);
            if (shouldLogReal()) {
                NativeLog.i(TAG, "eglChooseConfig " + attribsToString(attribList)
                    + ": no ANGLE config has that many samples; offering the closest, "
                    + attribsToString(closest));
            }
        }
        if (numConfig != null && numConfig.length >= 1) numConfig[0] = num[0];
        if (ok && tmp != null) {
            for (int i = 0; i < num[0] && i < size; i++) configs[i] = wrapConfig(d, tmp[i]);
        }
        if (shouldLogReal()) {
            NativeLog.i(TAG, "eglChooseConfig " + attribsToString(attribList)
                + " -> " + num[0] + " ANGLE config(s)"
                + (configs == null ? " (count query)" : ""));
        }
        return ok;
    }

    @Override public boolean eglGetConfigs(EGLDisplay display, EGLConfig[] configs,
            int configSize, int[] numConfig) {
        RealDisplay d = realDisplay(display);
        if (d == null) return EGL10.super.eglGetConfigs(display, configs, configSize, numConfig);
        int size = configs == null ? 0 : Math.max(0, Math.min(configSize, configs.length));
        android.opengl.EGLConfig[] tmp = configs == null ? null : new android.opengl.EGLConfig[size];
        int[] num = new int[1];
        nativeCall();
        boolean ok = EGL14.eglGetConfigs(d.h, tmp, 0, size, num, 0);
        if (numConfig != null && numConfig.length >= 1) numConfig[0] = num[0];
        if (ok && tmp != null) {
            for (int i = 0; i < num[0] && i < size; i++) configs[i] = wrapConfig(d, tmp[i]);
        }
        return ok;
    }

    @Override public boolean eglGetConfigAttrib(EGLDisplay display, EGLConfig config,
            int attribute, int[] value) {
        if (!realOn() || !(config instanceof RealConfig) || value == null || value.length < 1) {
            return legacyGetConfigAttrib(display, config, attribute, value);
        }
        RealConfig c = (RealConfig) config;
        nativeCall();
        return EGL14.eglGetConfigAttrib(c.dpy.h, c.h, attribute, value, 0);
    }

    @Override public EGLContext eglCreateContext(EGLDisplay display, EGLConfig config,
            EGLContext shareContext, int[] attribList) {
        RealDisplay d = realDisplay(display);
        if (d == null) {
            return legacyCreateContext(display, config, shareContext, attribList);
        }
        if (!(config instanceof RealConfig)) {
            setError(EGL_BAD_CONFIG);
            return EGL_NO_CONTEXT;
        }
        RealConfig c = (RealConfig) config;
        android.opengl.EGLContext share = (shareContext instanceof RealContext)
            ? ((RealContext) shareContext).h : EGL14.EGL_NO_CONTEXT;
        nativeCall();
        android.opengl.EGLContext h = EGL14.eglCreateContext(d.h, c.h, share, attribList, 0);
        if (h == null || h.getNativeHandle() == 0) {
            if (shouldLogReal()) {
                NativeLog.w(TAG, "eglCreateContext attribs=" + attribsToString(attribList)
                    + " -> EGL_NO_CONTEXT (ANGLE refused)");
            }
            // EGLImpl: `if (eglContextId == 0) return EGL10.EGL_NO_CONTEXT;`
            return EGL_NO_CONTEXT;
        }
        int[] v = new int[1];
        int version = EGL14.eglQueryContext(d.h, h, EGL_CONTEXT_CLIENT_VERSION, v, 0) ? v[0] : 1;
        // A successful create leaves no error for eglGetError to find; the
        // version query above must not change that either.
        EGL14.eglGetError();
        if (shouldLogReal()) {
            NativeLog.i(TAG, "eglCreateContext attribs=" + attribsToString(attribList)
                + " -> ANGLE context 0x" + Long.toHexString(h.getNativeHandle())
                + " (ES " + version + ", thread " + Thread.currentThread().getName() + ")");
        }
        return new RealContext(d, h, version);
    }

    @Override public boolean eglDestroyContext(EGLDisplay display, EGLContext context) {
        if (!realOn() || !(context instanceof RealContext)) return legacyDestroyContext(display, context);
        RealContext c = (RealContext) context;
        nativeCall();
        return EGL14.eglDestroyContext(c.dpy.h, c.h);
    }

    @Override public EGLSurface eglCreateWindowSurface(EGLDisplay display, EGLConfig config,
            Object nativeWindow, int[] attribList) {
        RealDisplay d = realDisplay(display);
        if (d == null) {
            return legacyCreateWindowSurface(display, config, nativeWindow, attribList);
        }
        // EGLImpl.eglCreateWindowSurface's dispatch, in its order.
        SurfaceTexture st;
        if (nativeWindow instanceof android.view.SurfaceView
                || nativeWindow instanceof android.view.SurfaceHolder) {
            // Win.apk: any GL thread (GLSurfaceView's own, or an app's copy of
            // it) renders into the SurfaceView, as on Android.
            EGLSurface view = viewWindow(d, config, nativeWindow, attribList);
            if (view != null) return view;
            return legacyCreateWindowSurface(display, config, nativeWindow, attribList);
        } else if (nativeWindow instanceof android.view.Surface) {
            android.view.Surface s = (android.view.Surface) nativeWindow;
            if (ViewHost.get().ownerSurfaceView(s) != null) {
                // A SurfaceView's Surface (its holder's getSurface()), as EGLImpl takes it.
                EGLSurface view = viewWindow(d, config, s, attribList);
                if (view != null) return view;
                return legacyCreateWindowSurface(display, config, nativeWindow, attribList);
            }
            st = ViewHost.get().surfaceTexture(s);
            if (st == null) return legacyCreateWindowSurface(display, config, nativeWindow, attribList);
            if (!s.isValid()) {
                // jni_eglCreateWindowSurface's not_valid_surface branch.
                throw new IllegalArgumentException(
                    "Make sure the SurfaceView or associated SurfaceHolder has a valid Surface");
            }
        } else if (nativeWindow instanceof SurfaceTexture) {
            st = (SurfaceTexture) nativeWindow;
        } else {
            throw new UnsupportedOperationException(
                "eglCreateWindowSurface() can only be called with an instance of "
                + "Surface, SurfaceView, SurfaceHolder or SurfaceTexture at the moment.");
        }
        if (!(config instanceof RealConfig)) {
            setError(EGL_BAD_CONFIG);
            return EGL_NO_SURFACE;
        }
        RealConfig c = (RealConfig) config;
        nativeCall();
        MacApkGlWindow.Created made = MacApkGlWindow.create(d.h, c.h, st);
        if (made.window == null) {
            if (made.javaError != 0) setError(made.javaError);
            if (shouldLogReal()) {
                NativeLog.w(TAG, "eglCreateWindowSurface(SurfaceTexture) -> EGL_NO_SURFACE"
                    + (made.javaError != 0 ? " (0x" + Integer.toHexString(made.javaError)
                       + ": stream abandoned or already has a producer)" : " (pbuffer refused)"));
            }
            return EGL_NO_SURFACE;
        }
        if (shouldLogReal()) {
            NativeLog.i(TAG, "eglCreateWindowSurface(SurfaceTexture " + made.window.width()
                + "x" + made.window.height() + ") -> pbuffer-backed window 0x"
                + Long.toHexString(made.window.nativeSurface().getNativeHandle()));
        }
        return new RealSurface(d, null, made.window);
    }

    // Win.apk (windows/host/gl/egl10_window.cpp): the host's window surface on
    // the SurfaceView for a Java GL thread, swapped and forgotten through it.
    // GLSurfaceView's GLThread renders that way, and so do an app's own GL
    // threads (Google Maps, Waze's map: a SurfaceView, a GLThread of the app's,
    // a real context, eglCreateWindowSurface(holder)). Null on a host without
    // it (the Mac's binding answers null for this window), and then the legacy
    // answer.
    private static native int nativeSwapSurfaceViewWindow(android.opengl.EGLDisplay dpy,
                                                          android.opengl.EGLSurface surface);
    private static native void nativeForgetSurfaceViewWindow(android.opengl.EGLSurface surface);
    private static final boolean VIEW_WINDOW = System.getProperty("winapk.android.root") != null;

    /** EGL14.eglCreateWindowSurface's question on Windows (windows/host/gl/gl_bridges.cpp): the
     *  Surface of the SurfaceView {@code win} is, or is the holder or the Surface of, or null when
     *  it is none of those. A SurfaceView's Surface gets the same window surface as here, and
     *  once it has, macapkSurfaceViewWindowMade says so, as viewWindow does. */
    static android.view.Surface macapkSurfaceViewSurface(Object win) {
        android.view.Surface sur = null;
        if (win instanceof android.view.SurfaceView) {
            sur = ((android.view.SurfaceView) win).getHolder().getSurface();
        } else if (win instanceof android.view.SurfaceHolder) {
            sur = ((android.view.SurfaceHolder) win).getSurface();
        } else if (win instanceof android.view.Surface
                && ViewHost.get().ownerSurfaceView((android.view.Surface) win) != null) {
            sur = (android.view.Surface) win;
        }
        return sur != null && sur.isValid() ? sur : null;
    }

    /** The SurfaceView of {@code sur} now shows the native window's layer through its hole. */
    static void macapkSurfaceViewWindowMade(android.view.Surface sur) {
        ViewHost.get().noteNativeWindowConsumer(sur);
    }

    private EGLSurface viewWindow(RealDisplay d, EGLConfig config, Object nativeWindow, int[] attribList) {
        if (!VIEW_WINDOW || !(config instanceof RealConfig)) return null;
        android.view.Surface sur = (nativeWindow instanceof android.view.SurfaceView)
            ? ((android.view.SurfaceView) nativeWindow).getHolder().getSurface()
            : (nativeWindow instanceof android.view.SurfaceHolder)
            ? ((android.view.SurfaceHolder) nativeWindow).getSurface()
            : (android.view.Surface) nativeWindow;
        if (sur == null || !sur.isValid()) return null;
        android.opengl.EGLSurface h;
        try {
            nativeCall();
            h = nativeCreateSurfaceViewWindow(d.h, ((RealConfig) config).h, attribList);
        } catch (UnsatisfiedLinkError notThisHost) {
            return null;
        }
        if (h == null || h.getNativeHandle() == 0) return null;
        // The SurfaceView now shows the native window's layer through its hole,
        // as for an engine that called ANativeWindow_fromSurface.
        ViewHost.get().noteNativeWindowConsumer(sur);
        if (shouldLogReal()) {
            NativeLog.i(TAG, "eglCreateWindowSurface(SurfaceView) on " + Thread.currentThread().getName()
                + " -> a window surface on the SurfaceView (0x" + Long.toHexString(h.getNativeHandle()) + ")");
        }
        return new RealSurface(d, h, null, true);
    }

    @Override public boolean eglDestroySurface(EGLDisplay display, EGLSurface surface) {
        if (!realOn() || !(surface instanceof RealSurface)) return legacyDestroySurface(display, surface);
        RealSurface s = (RealSurface) surface;
        if (s.destroyed) {
            setError(EGL_BAD_SURFACE);
            return false;
        }
        s.destroyed = true;
        nativeCall();
        if (s.viewWindow) nativeForgetSurfaceViewWindow(s.pbuffer);
        return s.window != null ? s.window.destroy() : EGL14.eglDestroySurface(s.dpy.h, s.pbuffer);
    }

    @Override public boolean eglMakeCurrent(EGLDisplay display, EGLSurface draw,
            EGLSurface read, EGLContext context) {
        if (!realOn()) return legacyMakeCurrent(display, draw, read, context);
        Current cur = CURRENT.get();
        if (!(context instanceof RealContext)) {
            if (isNoContext(context) && isNoSurface(draw) && isNoSurface(read) && cur.ctx != null) {
                return releaseCurrent(cur);
            }
            if (draw instanceof RealSurface || read instanceof RealSurface) {
                // A surface with no context is an EGL_BAD_MATCH.
                setError(EGL_BAD_MATCH);
                return false;
            }
            // Nothing of ours current: the legacy answer, which can never
            // touch the framework context on OS-main.
            return legacyMakeCurrent(display, draw, read, context);
        }
        RealContext ctx = (RealContext) context;
        android.opengl.EGLSurface d = nativeOf(draw), r = nativeOf(read);
        if (d == null || r == null) {
            setError(EGL_BAD_SURFACE);
            return false;
        }
        nativeCall();
        if (cur.ctx == null) {
            // First bind of ours on this thread: remember a foreign binding
            // (OS-main's framework context) so a release can put it back.
            android.opengl.EGLContext fc = EGL14.eglGetCurrentContext();
            if (fc != null && fc.getNativeHandle() != 0) {
                cur.foreignCtx = fc;
                cur.foreignDpy = EGL14.eglGetCurrentDisplay();
                cur.foreignDraw = EGL14.eglGetCurrentSurface(EGL_DRAW);
                cur.foreignRead = EGL14.eglGetCurrentSurface(EGL_READ);
            }
        }
        boolean ok = EGL14.eglMakeCurrent(ctx.dpy.h, d, r, ctx.h);
        if (ok) {
            cur.dpy = ctx.dpy;
            cur.ctx = ctx;
            cur.draw = (draw instanceof RealSurface) ? (RealSurface) draw : null;
            cur.read = (read instanceof RealSurface) ? (RealSurface) read : null;
        } else if (cur.ctx == null) {
            cur.clear();
        }
        return ok;
    }

    /** Unbind our context from this thread, then re-bind whatever foreign
     *  context we displaced (the framework's, on OS-main). */
    private static boolean releaseCurrent(Current cur) {
        nativeCall();
        boolean ok = EGL14.eglMakeCurrent(cur.dpy.h, EGL14.EGL_NO_SURFACE,
                                          EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
        if (cur.foreignCtx != null) {
            EGL14.eglMakeCurrent(cur.foreignDpy, cur.foreignDraw, cur.foreignRead, cur.foreignCtx);
            // The restore is ours, not the app's call: it must not change what
            // the app's eglGetError reports for the release.
            if (ok) EGL14.eglGetError();
        }
        cur.clear();
        return ok;
    }

    @Override public boolean eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
        if (!realOn() || !(surface instanceof RealSurface)) return legacySwapBuffers(display, surface);
        RealSurface s = (RealSurface) surface;
        if (s.destroyed) {
            setError(EGL_BAD_SURFACE);
            return false;
        }
        if (s.window == null) {
            if (s.viewWindow) {
                // Read back and shown as the SurfaceView's layer (the host's
                // window surface swap); it must be current, as any window's.
                nativeCall();
                int r = nativeSwapSurfaceViewWindow(s.dpy.h, s.pbuffer);
                if (r >= 0) return r == 1;
            }
            // EGL 1.4 §3.9.3: a swap on a pbuffer has no effect beyond the
            // implicit flush.
            nativeCall();
            return EGL14.eglSwapBuffers(s.dpy.h, s.pbuffer);
        }
        Current cur = CURRENT.get();
        if (cur.ctx == null || cur.draw != s) {
            // EGL 1.4 §3.9.3: EGL_BAD_SURFACE if surface is not current to the
            // calling thread (the readback needs its context current anyway).
            setError(EGL_BAD_SURFACE);
            return false;
        }
        nativeCall();
        s.window.queueFrame(cur.ctx.clientVersion);
        boolean ok = EGL14.eglSwapBuffers(s.dpy.h, s.window.nativeSurface());
        long frames = s.window.frames();
        if (frames == 1 && shouldLogReal()) {
            NativeLog.i(TAG, "eglSwapBuffers -> SurfaceTexture frame #1 ("
                + s.window.width() + "x" + s.window.height() + ")");
        }
        // A real window picks up setDefaultBufferSize at its NEXT dequeue:
        // swap the pbuffer now, re-bind it, free the old one. Skipped after a
        // failed swap, so the app's eglGetError still reports that failure.
        android.opengl.EGLSurface old = ok ? s.window.resizeIfNeeded() : null;
        if (old != null) {
            android.opengl.EGLSurface nd = nativeOf(cur.draw), nr = nativeOf(cur.read);
            EGL14.eglMakeCurrent(cur.dpy.h, nd != null ? nd : EGL14.EGL_NO_SURFACE,
                                 nr != null ? nr : EGL14.EGL_NO_SURFACE, cur.ctx.h);
            EGL14.eglDestroySurface(s.dpy.h, old);
            EGL14.eglGetError();   // bookkeeping of ours, not the app's call
            if (shouldLogReal()) {
                NativeLog.i(TAG, "SurfaceTexture window resized to "
                    + s.window.width() + "x" + s.window.height());
            }
        }
        return ok;
    }

    @Override public int eglGetError() {
        int e = LAST_ERROR.get();
        LAST_ERROR.set(EGL_SUCCESS);
        if (e != EGL_SUCCESS || !realOn() || NATIVE_USED.get() == null) return e;
        try {
            return EGL14.eglGetError();
        } catch (Throwable t) {
            realBroken(t);
            return EGL_SUCCESS;
        }
    }

    @Override public boolean eglCopyBuffers(EGLDisplay display, EGLSurface surface,
                                            Object native_pixmap) {
        return legacyCopyBuffers(display, surface, native_pixmap);
    }

    @Override public EGLSurface eglCreatePbufferSurface(EGLDisplay display, EGLConfig config,
                                                        int[] attrib_list) {
        RealDisplay d = realDisplay(display);
        if (d == null || !(config instanceof RealConfig)) {
            return legacyCreatePbufferSurface(display, config, attrib_list);
        }
        nativeCall();
        android.opengl.EGLSurface h =
            EGL14.eglCreatePbufferSurface(d.h, ((RealConfig) config).h, attrib_list, 0);
        if (h == null || h.getNativeHandle() == 0) return EGL_NO_SURFACE;
        return new RealSurface(d, h, null);
    }

    @Override public EGLSurface eglCreatePixmapSurface(EGLDisplay display, EGLConfig config,
                                                       Object native_pixmap, int[] attrib_list) {
        return legacyCreatePixmapSurface(display, config, native_pixmap, attrib_list);
    }

    @Override public boolean eglQueryContext(EGLDisplay display, EGLContext context,
                                             int attribute, int[] value) {
        if (!realOn() || !(context instanceof RealContext) || value == null || value.length < 1) {
            return legacyQueryContext(display, context, attribute, value);
        }
        RealContext c = (RealContext) context;
        nativeCall();
        return EGL14.eglQueryContext(c.dpy.h, c.h, attribute, value, 0);
    }

    @Override public boolean eglQuerySurface(EGLDisplay display, EGLSurface surface,
                                             int attribute, int[] value) {
        if (!realOn() || !(surface instanceof RealSurface) || value == null || value.length < 1) {
            return legacyQuerySurface(display, surface, attribute, value);
        }
        RealSurface s = (RealSurface) surface;
        nativeCall();
        return EGL14.eglQuerySurface(s.dpy.h, s.nativeSurface(), attribute, value, 0);
    }

    @Override public boolean eglWaitGL() {
        if (!realOn() || CURRENT.get().ctx == null) return legacyWaitGL();
        nativeCall();
        return EGL14.eglWaitGL();
    }

    @Override public boolean eglWaitNative(int engine, Object bindTarget) {
        if (!realOn() || CURRENT.get().ctx == null) return legacyWaitNative(engine, bindTarget);
        nativeCall();
        return EGL14.eglWaitNative(engine);
    }

    /** Has this thread no context current at all: none of ours, and none of
     *  the framework's either? Then eglGetCurrent* say so, as EGLImpl does. An
     *  app's GL code asks eglGetCurrentContext() whether it may query GL here,
     *  and makes a context of its own when the answer is EGL_NO_CONTEXT; told
     *  the sentinel on a thread with nothing current, Google Maps' GL constants
     *  were read with no context: glGetIntegerv left GL_MAX_VERTEX_ATTRIBS at
     *  0, and its GL thread died enabling attribute 0 of an empty table. */
    private static boolean nothingCurrent() {
        try {
            android.opengl.EGLContext c = EGL14.eglGetCurrentContext();
            return c == null || c.getNativeHandle() == 0;
        } catch (Throwable t) {
            realBroken(t);
            return false;
        }
    }

    @Override public EGLContext eglGetCurrentContext() {
        if (realOn()) {
            Current cur = CURRENT.get();
            if (cur.ctx != null) return cur.ctx;
            if (nothingCurrent()) return EGL_NO_CONTEXT;
        }
        return legacyGetCurrentContext();
    }

    @Override public EGLDisplay eglGetCurrentDisplay() {
        if (realOn()) {
            Current cur = CURRENT.get();
            if (cur.ctx != null) return cur.dpy;
            if (nothingCurrent()) return EGL_NO_DISPLAY;
        }
        return legacyGetCurrentDisplay();
    }

    @Override public EGLSurface eglGetCurrentSurface(int readdraw) {
        if (realOn()) {
            Current cur = CURRENT.get();
            if (cur.ctx != null) {
                // jni_eglGetCurrentSurface rejects anything but READ / DRAW.
                if (readdraw != EGL_READ && readdraw != EGL_DRAW) {
                    throw new IllegalArgumentException();
                }
                RealSurface s = readdraw == EGL_READ ? cur.read : cur.draw;
                return s != null ? s : EGL_NO_SURFACE;
            }
            if (nothingCurrent()) return EGL_NO_SURFACE;
        }
        return legacyGetCurrentSurface(readdraw);
    }

    @Override public String eglQueryString(EGLDisplay display, int name) {
        RealDisplay d = realDisplay(display);
        if (d == null) return legacyQueryString(display, name);
        switch (name) {
            // Android's libEGL answers these two with ITS OWN strings, not the
            // driver's (frameworks/native opengl/libs/EGL/egl_display.cpp:
            // sVendorString "Android"; sVersionString15 "1.5 Android META-EGL"
            // for a 1.5 driver, which ANGLE is).
            case EGL_VENDOR:  return "Android";
            case EGL_VERSION: return "1.5 Android META-EGL";
            case EGL_EXTENSIONS:
            case EGL_CLIENT_APIS: {
                nativeCall();
                String s = EGL14.eglQueryString(d.h, name);
                return s != null ? s : "";
            }
            default:
                nativeCall();
                return EGL14.eglQueryString(d.h, name);
        }
    }

    private static String attribsToString(int[] a) {
        if (a == null) return "null";
        StringBuilder sb = new StringBuilder("[");
        for (int i = 0; i + 1 < a.length && a[i] != EGL_NONE; i += 2) {
            if (sb.length() > 1) sb.append(' ');
            sb.append(String.format("0x%x=%d", a[i], a[i + 1]));
        }
        return sb.append(']').toString();
    }

    // Win.apk (windows/host/gl/egl10_window.cpp): the host's window surface on
    // the SurfaceView's native window for a Java GL thread, or null.
    private static native android.opengl.EGLSurface nativeCreateSurfaceViewWindow(
        android.opengl.EGLDisplay dpy, android.opengl.EGLConfig config, int[] attribList);

    // =====================================================================
    // LEGACY contract: what a JVM without the EGL14 natives answers (a plain
    // JVM's tests): no context, no surface, a config table to choose from.
    // =====================================================================

    private EGLDisplay legacyGetDisplay(Object nativeDisplay) {
        return SENTINEL_DISPLAY;
    }

    private boolean legacyInitialize(EGLDisplay display, int[] majorMinor) {
        if (majorMinor != null && majorMinor.length >= 2) {
            majorMinor[0] = 1;   // EGL 1.4
            majorMinor[1] = 4;
        }
        return true;
    }

    private boolean legacyTerminate(EGLDisplay display) { return true; }

    /** EGL-spec config matching over the table. attribList is
     *  EGL_NONE-terminated key/value pairs; null means no constraints. */
    private static boolean matches(Config c, int[] attribs) {
        if (attribs == null) return true;
        for (int i = 0; i + 1 < attribs.length && attribs[i] != EGL_NONE; i += 2) {
            int key = attribs[i], want = attribs[i + 1];
            if (want == EGL_DONT_CARE) continue;
            switch (key) {
                // Sizes: "at least" semantics; 0 matches anything.
                case EGL_RED_SIZE:       if (c.r < want) return false; break;
                case EGL_GREEN_SIZE:     if (c.g < want) return false; break;
                case EGL_BLUE_SIZE:      if (c.b < want) return false; break;
                case EGL_ALPHA_SIZE:     if (c.a < want) return false; break;
                case EGL_DEPTH_SIZE:     if (c.depth < want) return false; break;
                case EGL_STENCIL_SIZE:   if (c.stencil < want) return false; break;
                case EGL_BUFFER_SIZE:    if (c.bufferSize() < want) return false; break;
                case EGL_SAMPLES:
                case EGL_SAMPLE_BUFFERS: if (want > 0) return false; break;
                // Bitmasks: config must carry every requested bit.
                case EGL_RENDERABLE_TYPE:
                case EGL_CONFORMANT:
                    if ((want & ~(EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT)) != 0)
                        return false;
                    break;
                case EGL_SURFACE_TYPE:
                    if ((want & ~(EGL_WINDOW_BIT | EGL_PBUFFER_BIT)) != 0)
                        return false;
                    break;
                case EGL_CONFIG_ID:      if (c.id != want) return false; break;
                case EGL_CONFIG_CAVEAT:  if (want != EGL_NONE) return false; break;
                case EGL_LEVEL:          if (want != 0) return false; break;
                case EGL_TRANSPARENT_TYPE: if (want != EGL_NONE) return false; break;
                // Unknown attribute: be lenient (real drivers reject
                // with EGL_BAD_ATTRIBUTE; app forks never pass any).
                default: break;
            }
        }
        return true;
    }

    private static int logBudget = 16;
    private static synchronized boolean shouldLog() {
        if (logBudget <= 0) return false;
        logBudget--;
        return true;
    }

    private boolean legacyChooseConfig(EGLDisplay display, int[] attribList,
            EGLConfig[] configs, int configSize, int[] numConfig) {
        int matched = 0;
        for (Config c : CONFIGS) {
            if (!matches(c, attribList)) continue;
            if (configs != null && matched < configSize) configs[matched] = c;
            matched++;
        }
        int reported = (configs == null) ? matched : Math.min(matched, configSize);
        if (numConfig != null && numConfig.length >= 1) numConfig[0] = reported;
        if (shouldLog()) {
            NativeLog.i(TAG, "eglChooseConfig " + attribsToString(attribList)
                + " -> " + reported + " config(s)"
                + (configs == null ? " (count query)" : ""));
        }
        return true;
    }

    private boolean legacyGetConfigAttrib(EGLDisplay display, EGLConfig config,
            int attribute, int[] value) {
        if (value == null || value.length < 1) return true;
        Config c = (config instanceof Config) ? (Config) config : CONFIGS[0];
        switch (attribute) {
            case EGL_RED_SIZE:        value[0] = c.r; break;
            case EGL_GREEN_SIZE:      value[0] = c.g; break;
            case EGL_BLUE_SIZE:       value[0] = c.b; break;
            case EGL_ALPHA_SIZE:      value[0] = c.a; break;
            case EGL_DEPTH_SIZE:      value[0] = c.depth; break;
            case EGL_STENCIL_SIZE:    value[0] = c.stencil; break;
            case EGL_BUFFER_SIZE:     value[0] = c.bufferSize(); break;
            case EGL_CONFIG_ID:       value[0] = c.id; break;
            case EGL_CONFIG_CAVEAT:   value[0] = EGL_NONE; break;
            case EGL_SURFACE_TYPE:    value[0] = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; break;
            case EGL_RENDERABLE_TYPE: value[0] = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT; break;
            case EGL_CONFORMANT:      value[0] = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT; break;
            case EGL_SAMPLES:
            case EGL_SAMPLE_BUFFERS:  value[0] = 0; break;
            case EGL_LEVEL:           value[0] = 0; break;
            case EGL_MAX_PBUFFER_WIDTH:  value[0] = 4096; break;
            case EGL_MAX_PBUFFER_HEIGHT: value[0] = 4096; break;
            case EGL_MAX_PBUFFER_PIXELS: value[0] = 4096 * 4096; break;
            case EGL_NATIVE_RENDERABLE:  value[0] = 0; break;
            case EGL_NATIVE_VISUAL_ID:   value[0] = 0; break;
            case EGL_NATIVE_VISUAL_TYPE: value[0] = EGL_NONE; break;
            case EGL_TRANSPARENT_TYPE:   value[0] = EGL_NONE; break;
            default:                  value[0] = 0; break;
        }
        return true;
    }

    /** No EGL natives, so no context: EGL_NO_CONTEXT with EGL_BAD_CONFIG. */
    private EGLContext legacyCreateContext(EGLDisplay display, EGLConfig config,
            EGLContext shareContext, int[] attribList) {
        if (shouldLog()) {
            NativeLog.i(TAG, "eglCreateContext attribs="
                + attribsToString(attribList) + " -> EGL_NO_CONTEXT (no EGL natives in this JVM)");
        }
        setError(EGL_BAD_CONFIG);
        return EGL_NO_CONTEXT;
    }

    private boolean legacyDestroyContext(EGLDisplay display, EGLContext context) {
        return true;
    }

    /** EGL_NO_SURFACE + EGL_BAD_NATIVE_WINDOW: there is no window to make one on. */
    private EGLSurface legacyCreateWindowSurface(EGLDisplay display, EGLConfig config,
            Object nativeWindow, int[] attribList) {
        setError(EGL_BAD_NATIVE_WINDOW);
        return EGL_NO_SURFACE;
    }

    private boolean legacyDestroySurface(EGLDisplay display, EGLSurface surface) {
        return true;
    }

    private boolean legacyMakeCurrent(EGLDisplay display, EGLSurface draw,
            EGLSurface read, EGLContext context) {
        // The unbind triple (EGL_NO_SURFACE/EGL_NO_SURFACE/EGL_NO_CONTEXT, or
        // nulls, which the EGL spec equally allows) succeeds; nothing else can
        // be bound, as nothing is ever made here.
        boolean drawNone = (draw == null || draw == EGL_NO_SURFACE);
        boolean readNone = (read == null || read == EGL_NO_SURFACE);
        boolean ctxNone  = (context == null || context == EGL_NO_CONTEXT);
        if (drawNone && readNone && ctxNone) return true;
        setError(EGL_BAD_SURFACE);
        return false;
    }

    private boolean legacySwapBuffers(EGLDisplay display, EGLSurface surface) {
        return true;
    }

    // ---- The remaining legacy entry points --------------------------------
    // These are the surface/context operations that only make sense once a
    // Java-owned context or surface EXISTS. The legacy contract never creates
    // either, so the ONLY self-consistent answer for each of them is the
    // matching failure, with the error code an app's eglGetError() then
    // reads. An optimistic `return true` here would be a lie the caller acts
    // on -- a successful eglQuerySurface(EGL_WIDTH) with no surface behind it
    // hands an engine a bogus viewport, which is exactly the class of silent
    // wrong this file was rewritten (v1.0.347) to stop producing.

    /** No surface is ever created here, so there is nothing to copy FROM. */
    private boolean legacyCopyBuffers(EGLDisplay display, EGLSurface surface,
                                      Object native_pixmap) {
        setError(EGL_BAD_SURFACE);
        return false;
    }

    /** Same contract as eglCreateWindowSurface: no Java-owned surfaces. */
    private EGLSurface legacyCreatePbufferSurface(EGLDisplay display, EGLConfig config,
                                                  int[] attrib_list) {
        setError(EGL_BAD_ALLOC);
        return EGL_NO_SURFACE;
    }

    /** Same contract as eglCreateWindowSurface; there are no native pixmaps
     *  on this platform at all, hence EGL_BAD_NATIVE_PIXMAP specifically. */
    private EGLSurface legacyCreatePixmapSurface(EGLDisplay display, EGLConfig config,
                                                 Object native_pixmap, int[] attrib_list) {
        setError(EGL_BAD_NATIVE_PIXMAP);
        return EGL_NO_SURFACE;
    }

    /** No context is ever created here, so every context handle an app can
     *  hold is EGL_NO_CONTEXT or our read-only sentinel. */
    private boolean legacyQueryContext(EGLDisplay display, EGLContext context,
                                       int attribute, int[] value) {
        setError(EGL_BAD_CONTEXT);
        return false;
    }

    /** See legacyQueryContext; the surface equivalent. */
    private boolean legacyQuerySurface(EGLDisplay display, EGLSurface surface,
                                       int attribute, int[] value) {
        setError(EGL_BAD_SURFACE);
        return false;
    }

    /**
     * "Complete GL execution prior to subsequent native rendering calls."
     * True is the CORRECT answer rather than a convenient one: no GL commands
     * were ever issued through a Java-owned context, so the set of commands
     * this must wait for is empty and it is trivially complete. (ANGLE's own
     * queue is driven from the C++ side and is not what this call refers to.)
     */
    private boolean legacyWaitGL() { return true; }

    /** The mirror image of eglWaitGL, for native rendering. Same reasoning. */
    private boolean legacyWaitNative(int engine, Object bindTarget) { return true; }

    // Current-state queries. After onSurfaceCreated, libGDX caches the
    // "current context" to detect loss; a stable non-null sentinel keeps
    // it happy.
    private EGLContext legacyGetCurrentContext() { return SENTINEL_CONTEXT; }
    private EGLDisplay legacyGetCurrentDisplay() { return SENTINEL_DISPLAY; }
    private EGLSurface legacyGetCurrentSurface(int readdraw) { return SENTINEL_SURFACE; }

    private String legacyQueryString(EGLDisplay display, int name) {
        switch (name) {
            case EGL_VENDOR:     return "Mac.apk";
            case EGL_VERSION:    return "1.4 Mac.apk";
            case EGL_EXTENSIONS: return "";
            default:             return "";
        }
    }
}
