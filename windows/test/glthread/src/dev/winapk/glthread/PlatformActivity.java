package dev.winapk.glthread;

import android.app.Activity;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.util.Log;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/**
 * The platform's GLSurfaceView with its renderer set in onCreate, before the surface exists, as
 * most GL apps do. On a device: onSurfaceCreated, onSurfaceChanged(w, h) and then onDrawFrame on
 * the view's own GLThread, and onSurfaceChanged again at every resize. Logs (tag GLPROBE) what it
 * got and on which thread; clears its frames green.
 */
public class PlatformActivity extends Activity {
    static final String TAG = "GLPROBE";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        GLSurfaceView view = new GLSurfaceView(this);
        view.setEGLContextClientVersion(2);
        view.setRenderer(new Green());
        setContentView(view);
    }

    static final class Green implements GLSurfaceView.Renderer {
        private long frames;

        @Override public void onSurfaceCreated(GL10 gl, EGLConfig config) {
            Log.i(TAG, "platform onSurfaceCreated on " + Thread.currentThread().getName());
        }

        @Override public void onSurfaceChanged(GL10 gl, int w, int h) {
            Log.i(TAG, "platform onSurfaceChanged " + w + "x" + h + " on " + Thread.currentThread().getName());
            GLES20.glViewport(0, 0, w, h);
        }

        @Override public void onDrawFrame(GL10 gl) {
            GLES20.glClearColor(0f, 1f, 0f, 1f);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            long n = ++frames;
            if (n == 1 || n == 60) {
                Log.i(TAG, "platform onDrawFrame #" + n + " on " + Thread.currentThread().getName());
            }
        }
    }
}
