package dev.winapk.glthread;

import android.app.Activity;
import android.content.Context;
import android.opengl.EGL14;
import android.opengl.GLES20;
import android.os.Bundle;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

import java.lang.ref.WeakReference;

import javax.microedition.khronos.egl.EGL10;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.egl.EGLContext;
import javax.microedition.khronos.egl.EGLDisplay;
import javax.microedition.khronos.egl.EGLSurface;

/**
 * An app's own copy of GLSurfaceView's machinery, as map engines and games ship it: a plain
 * SurfaceView ({@link MapSurface}) whose SurfaceHolder.Callback is a separate object
 * ({@link Gl}), a renderer whose methods are not named onDrawFrame (an obfuscated app's), and a
 * GLThread of its own that is started when the renderer is set, before the surface exists, and
 * points back at its view, as AOSP's GLThread does (mGLSurfaceViewWeakRef). The thread makes its
 * context and a window surface on the SurfaceView's holder (EGL10; {@link Fork14Activity}: EGL14 on
 * the holder's Surface) and draws there. Logs (tag GLPROBE); clears its frames blue (red for EGL14).
 *
 * Beside it a loader thread ({@link Loader}) uploads textures all the time on a context that shares
 * the GL thread's, current on a 1x1 pbuffer, as Flutter's IO thread does beside its raster thread
 * and a game's loading thread beside its renderer: from before the window surface exists, across
 * the resize, at which the GL thread destroys its window surface and makes a new one (AOSP's
 * GLThread does the same on a size change, and Flutter's Impeller). Nothing but the app's own
 * threads may draw on the app's display meanwhile (the host's render tick once did, and City Car
 * crashed in the Direct3D driver at a resize).
 */
public class ForkActivity extends Activity {
    static final String TAG = PlatformActivity.TAG;

    /** The renderer, with its methods named as an obfuscator leaves them. */
    interface R {
        void a(int w, int h);
        void b();
        void c();
    }

    boolean egl14() { return false; }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        MapSurface view = new MapSurface(this, egl14());
        view.gl.set(new Clear(egl14()));
        setContentView(view);
    }

    static final class Clear implements R {
        private final String name;
        private final float red, blue;
        private long frames;

        Clear(boolean egl14) {
            name = egl14 ? "fork14" : "fork";
            red = egl14 ? 1f : 0f;
            blue = egl14 ? 0f : 1f;
        }

        @Override public void b() {
            Log.i(TAG, name + " created on " + Thread.currentThread().getName());
        }

        @Override public void a(int w, int h) {
            Log.i(TAG, name + " changed " + w + "x" + h + " on " + Thread.currentThread().getName());
            GLES20.glViewport(0, 0, w, h);
        }

        @Override public void c() {
            GLES20.glClearColor(red, 0f, blue, 1f);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            long n = ++frames;
            if (n == 1 || n == 60) Log.i(TAG, name + " drew #" + n + " on " + Thread.currentThread().getName());
        }
    }

    static final class MapSurface extends SurfaceView {
        final Gl gl;

        MapSurface(Context c, boolean egl14) {
            super(c);
            gl = new Gl(this, egl14);
            getHolder().addCallback(gl);
        }
    }

    /** The holder's callback and the GL thread's owner. */
    static final class Gl implements SurfaceHolder.Callback {
        private final MapSurface view;
        private final boolean egl14;
        private Thread thread;
        R renderer;
        boolean hasSurface, sizeChanged;
        int width, height;

        Gl(MapSurface view, boolean egl14) {
            this.view = view;
            this.egl14 = egl14;
        }

        void set(R r) {
            renderer = r;
            thread = new GlThread(new WeakReference<>(view), this);
            thread.start();
        }

        @Override public synchronized void surfaceCreated(SurfaceHolder h) {
            hasSurface = true;
            notifyAll();
        }

        @Override public synchronized void surfaceChanged(SurfaceHolder h, int format, int w, int hh) {
            width = w;
            height = hh;
            sizeChanged = true;
            notifyAll();
        }

        @Override public synchronized void surfaceDestroyed(SurfaceHolder h) {
            hasSurface = false;
            notifyAll();
        }

        SurfaceHolder holder() { return view.getHolder(); }
        boolean egl14() { return egl14; }
    }

    static final class GlThread extends Thread {
        private final WeakReference<MapSurface> viewRef;   // AOSP's mGLSurfaceViewWeakRef
        private final Gl gl;

        GlThread(WeakReference<MapSurface> viewRef, Gl gl) {
            super("GLThread " + (gl.egl14() ? "fork14" : "fork"));
            this.viewRef = viewRef;
            this.gl = gl;
        }

        @Override public void run() {
            try {
                if (gl.egl14()) run14(); else run10();
            } catch (Throwable t) {
                Log.e(TAG, getName() + " died: " + t);
            }
        }

        /** Waits for a surface with a size; answers the size, and whether it is new. */
        private int[] await() throws InterruptedException {
            synchronized (gl) {
                while (!gl.hasSurface || gl.width <= 0 || gl.height <= 0) gl.wait();
                boolean changed = gl.sizeChanged;
                gl.sizeChanged = false;
                return new int[] {gl.width, gl.height, changed ? 1 : 0};
            }
        }

        private void run10() throws InterruptedException {
            EGL10 egl = (EGL10) EGLContext.getEGL();
            EGLDisplay dpy = egl.eglGetDisplay(EGL10.EGL_DEFAULT_DISPLAY);
            egl.eglInitialize(dpy, new int[2]);
            int[] spec = {EGL10.EGL_RED_SIZE, 8, EGL10.EGL_GREEN_SIZE, 8, EGL10.EGL_BLUE_SIZE, 8,
                          EGL10.EGL_RENDERABLE_TYPE, 4, EGL10.EGL_NONE};
            EGLConfig[] configs = new EGLConfig[1];
            int[] n = new int[1];
            if (!egl.eglChooseConfig(dpy, spec, configs, 1, n) || n[0] < 1) throw new RuntimeException("no config");
            EGLContext ctx = egl.eglCreateContext(dpy, configs[0], EGL10.EGL_NO_CONTEXT,
                    new int[] {0x3098 /* EGL_CONTEXT_CLIENT_VERSION */, 2, EGL10.EGL_NONE});
            if (ctx == null || ctx == EGL10.EGL_NO_CONTEXT) {
                throw new RuntimeException("createContext failed: " + egl.eglGetError());
            }
            Log.i(TAG, getName() + " has a context");
            new Loader(gl.egl14() ? "fork14" : "fork", () -> {
                EGLContext shared = egl.eglCreateContext(dpy, configs[0], ctx,
                        new int[] {0x3098 /* EGL_CONTEXT_CLIENT_VERSION */, 2, EGL10.EGL_NONE});
                EGLSurface pb = egl.eglCreatePbufferSurface(dpy, configs[0],
                        new int[] {EGL10.EGL_WIDTH, 1, EGL10.EGL_HEIGHT, 1, EGL10.EGL_NONE});
                return shared != null && shared != EGL10.EGL_NO_CONTEXT && pb != null && pb != EGL10.EGL_NO_SURFACE
                        && egl.eglMakeCurrent(dpy, pb, pb, shared);
            }).start();
            EGLSurface surface = null;
            boolean created = false;
            for (;;) {
                int[] s = await();
                if (surface == null || s[2] == 1) {
                    if (surface != null) {
                        egl.eglMakeCurrent(dpy, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_CONTEXT);
                        egl.eglDestroySurface(dpy, surface);
                    }
                    surface = egl.eglCreateWindowSurface(dpy, configs[0], gl.holder(), null);
                    if (surface == null || surface == EGL10.EGL_NO_SURFACE) {
                        throw new RuntimeException("eglCreateWindowSurface failed: " + egl.eglGetError());
                    }
                    if (!egl.eglMakeCurrent(dpy, surface, surface, ctx)) throw new RuntimeException("makeCurrent");
                    if (!created) {
                        created = true;
                        gl.renderer.b();
                    }
                    gl.renderer.a(s[0], s[1]);
                }
                gl.renderer.c();
                egl.eglSwapBuffers(dpy, surface);
                Thread.sleep(16);
            }
        }

        private void run14() throws InterruptedException {
            android.opengl.EGLDisplay dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
            EGL14.eglInitialize(dpy, new int[2], 0, new int[2], 1);
            int[] spec = {EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                          EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_NONE};
            android.opengl.EGLConfig[] configs = new android.opengl.EGLConfig[1];
            int[] n = new int[1];
            if (!EGL14.eglChooseConfig(dpy, spec, 0, configs, 0, 1, n, 0) || n[0] < 1) {
                throw new RuntimeException("no config");
            }
            android.opengl.EGLContext ctx = EGL14.eglCreateContext(dpy, configs[0], EGL14.EGL_NO_CONTEXT,
                    new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
            if (ctx == null || ctx.equals(EGL14.EGL_NO_CONTEXT)) {
                throw new RuntimeException("createContext failed: " + EGL14.eglGetError());
            }
            Log.i(TAG, getName() + " has a context");
            new Loader(gl.egl14() ? "fork14" : "fork", () -> {
                android.opengl.EGLContext shared = EGL14.eglCreateContext(dpy, configs[0], ctx,
                        new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
                android.opengl.EGLSurface pb = EGL14.eglCreatePbufferSurface(dpy, configs[0],
                        new int[] {EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE}, 0);
                return shared != null && !shared.equals(EGL14.EGL_NO_CONTEXT) && pb != null
                        && !pb.equals(EGL14.EGL_NO_SURFACE) && EGL14.eglMakeCurrent(dpy, pb, pb, shared);
            }).start();
            android.opengl.EGLSurface surface = null;
            boolean created = false;
            for (;;) {
                int[] s = await();
                if (surface == null || s[2] == 1) {
                    if (surface != null) {
                        EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                        EGL14.eglDestroySurface(dpy, surface);
                    }
                    surface = EGL14.eglCreateWindowSurface(dpy, configs[0], gl.holder().getSurface(),
                            new int[] {EGL14.EGL_NONE}, 0);
                    if (surface == null || surface.equals(EGL14.EGL_NO_SURFACE)) {
                        throw new RuntimeException("eglCreateWindowSurface failed: " + EGL14.eglGetError());
                    }
                    if (!EGL14.eglMakeCurrent(dpy, surface, surface, ctx)) throw new RuntimeException("makeCurrent");
                    if (!created) {
                        created = true;
                        gl.renderer.b();
                    }
                    gl.renderer.a(s[0], s[1]);
                }
                gl.renderer.c();
                EGL14.eglSwapBuffers(dpy, surface);
                Thread.sleep(16);
            }
        }
    }

    /** Makes a context sharing the GL thread's current on this thread; false when it cannot. */
    interface Bind {
        boolean bind();
    }

    /** Uploads a 256x256 texture again and again, waiting for each (glFinish), until the process ends. */
    static final class Loader extends Thread {
        private final String name;
        private final Bind bind;

        Loader(String renderer, Bind bind) {
            super("GLThread " + renderer + " loader");
            this.name = renderer + " loader";
            this.bind = bind;
            setDaemon(true);
        }

        @Override public void run() {
            try {
                if (!bind.bind()) throw new RuntimeException("no shared context on a pbuffer");
                int[] tex = new int[1];
                GLES20.glGenTextures(1, tex, 0);
                GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, tex[0]);
                java.nio.ByteBuffer px = java.nio.ByteBuffer.allocateDirect(256 * 256 * 4);
                for (long n = 1;; n++) {
                    px.put((int) (n % px.capacity()), (byte) n).rewind();
                    GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, 256, 256, 0, GLES20.GL_RGBA,
                            GLES20.GL_UNSIGNED_BYTE, px);
                    GLES20.glFinish();
                    int err = GLES20.glGetError();
                    if (err != GLES20.GL_NO_ERROR) {
                        throw new RuntimeException("glTexImage2D: 0x" + Integer.toHexString(err));
                    }
                    if (n == 1 || n == 300) {
                        Log.i(TAG, name + " uploaded #" + n + " on " + Thread.currentThread().getName());
                    }
                    Thread.sleep(1);
                }
            } catch (Throwable t) {
                Log.e(TAG, name + " died: " + t);
            }
        }
    }
}
