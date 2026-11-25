package dev.winapk.surfaces;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.SurfaceTexture;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.Log;
import android.view.PixelCopy;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.TextureView;
import android.widget.GridLayout;
import android.widget.ImageView;
import android.widget.TextView;

/**
 * One screen of what draws outside a view's own canvas, each in a quarter of the window, each
 * logging what it got (tag SURFACES):
 *   top left      a TextureView a thread draws into with lockCanvas (a moving blue square)
 *   top right     a SurfaceView on top of the window (setZOrderOnTop), drawn with lockCanvas (red)
 *   bottom left   an ImageView of a HARDWARE bitmap (green), or of the bitmap it was made from
 *   bottom right  the window's PixelCopy, once a second after the first second
 * Under each, a label. The checks are the log's lines and the frame's pixels.
 */
public class SurfacesActivity extends Activity {
    private static final String TAG = "SURFACES";
    private volatile boolean mRunning = true;
    private int W, H;
    private final Handler mMain = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        GridLayout grid = new GridLayout(this);
        grid.setColumnCount(2);
        W = getResources().getDisplayMetrics().widthPixels / 2;
        H = getResources().getDisplayMetrics().heightPixels / 2 - 40;

        final TextureView texture = new TextureView(this);
        texture.setSurfaceTextureListener(new Listener(this, texture));
        grid.addView(texture, cell());

        SurfaceView top = new SurfaceView(this);
        top.setZOrderOnTop(true);
        top.getHolder().setFormat(PixelFormat.TRANSLUCENT);
        top.getHolder().addCallback(new OnTop());
        grid.addView(top, cell());

        ImageView hardware = new ImageView(this);
        Bitmap soft = Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888);
        soft.eraseColor(Color.GREEN);
        Bitmap hw = soft.copy(Bitmap.Config.HARDWARE, false);
        Log.i(TAG, "hardware bitmap " + (hw == null ? "null" : hw.getConfig()));
        hardware.setImageBitmap(hw != null ? hw : soft);
        hardware.setScaleType(ImageView.ScaleType.FIT_XY);
        grid.addView(hardware, cell());

        ImageView copy = new ImageView(this);
        copy.setScaleType(ImageView.ScaleType.FIT_XY);
        grid.addView(copy, cell());

        TextView label = new TextView(this);
        label.setText("TextureView | SurfaceView on top | HARDWARE bitmap | PixelCopy");
        grid.addView(label, new GridLayout.LayoutParams(spec(), GridLayout.spec(0, 2)));
        setContentView(grid);

        HandlerThread copyThread = new HandlerThread("pixelcopy");
        copyThread.start();
        mMain.postDelayed(new CopyTask(this, copy, new Handler(copyThread.getLooper())), 1000);
    }

    /** Draws the TextureView's frames with lockCanvas, as a video or camera producer posts them. */
    // Static: JDK 21's javac gives an inner class's constructor a MethodParameters entry with no
    // name for the outer instance, which the SDK's d8 cannot read.
    private static final class Producer implements Runnable {
        private final SurfacesActivity mA;
        private final TextureView mView;
        private final int mW, mH;
        Producer(SurfacesActivity a, TextureView view, int w, int h) { mA = a; mView = view; mW = w; mH = h; }
        @Override public void run() {
            int frame = 0;
            Paint p = new Paint();
            while (mA.mRunning) {
                Canvas c = mView.lockCanvas();
                if (c == null) break;
                c.drawColor(Color.WHITE);
                p.setColor(Color.BLUE);
                int x = (frame * 4) % Math.max(1, mW - 60);
                c.drawRect(x, mH / 3f, x + 60, mH / 3f + 60, p);
                mView.unlockCanvasAndPost(c);
                if (++frame == 30) Log.i(TAG, "texture drew 30 frames");
                try { Thread.sleep(16); } catch (InterruptedException e) { break; }
            }
        }
    }

    /** A PixelCopy of the window once a second, shown in the last quarter. */
    private static final class CopyTask implements Runnable, PixelCopy.OnPixelCopyFinishedListener {
        private final SurfacesActivity mA;
        private final ImageView mView;
        private final Handler mHandler;
        private Bitmap mDst;
        private int mW, mH;
        CopyTask(SurfacesActivity a, ImageView view, Handler handler) { mA = a; mView = view; mHandler = handler; }
        @Override public void run() {
            if (!mA.mRunning) return;
            mW = mA.getWindow().getDecorView().getWidth();
            mH = mA.getWindow().getDecorView().getHeight();
            if (mW > 0 && mH > 0) {
                mDst = Bitmap.createBitmap(mW, mH, Bitmap.Config.ARGB_8888);
                PixelCopy.request(mA.getWindow(), mDst, this, mHandler);
            }
            mA.mMain.postDelayed(this, 1000);
        }
        @Override public void onPixelCopyFinished(int result) {
            final Bitmap dst = mDst;
            int px = result == PixelCopy.SUCCESS ? dst.getPixel(mW / 4, mH * 3 / 4) : 0;
            Log.i(TAG, "pixelcopy result " + result + " pixel at the hardware bitmap " + Integer.toHexString(px));
            if (result == PixelCopy.SUCCESS) mA.mMain.post(new Show(mView, dst));
        }
    }

    private static final class Show implements Runnable {
        private final ImageView mView;
        private final Bitmap mBitmap;
        Show(ImageView view, Bitmap bitmap) { mView = view; mBitmap = bitmap; }
        @Override public void run() { mView.setImageBitmap(mBitmap); }
    }

    private GridLayout.LayoutParams cell() {
        GridLayout.LayoutParams lp = new GridLayout.LayoutParams(spec(), spec());
        lp.width = W;
        lp.height = H;
        return lp;
    }

    /** The SurfaceView on top: one red frame drawn with lockCanvas when its surface is there. */
    private static final class Listener implements TextureView.SurfaceTextureListener {
        private final SurfacesActivity mA;
        private final TextureView mView;
        Listener(SurfacesActivity a, TextureView view) { mA = a; mView = view; }
        @Override public void onSurfaceTextureAvailable(SurfaceTexture st, int tw, int th) {
            Log.i(TAG, "texture available " + tw + "x" + th);
            new Thread(new Producer(mA, mView, tw, th), "texture-producer").start();
        }
        @Override public void onSurfaceTextureSizeChanged(SurfaceTexture st, int tw, int th) {}
        @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture st) {
            mA.mRunning = false;
            return true;
        }
        @Override public void onSurfaceTextureUpdated(SurfaceTexture st) {}
    }

    private static final class OnTop implements SurfaceHolder.Callback {
        @Override public void surfaceCreated(SurfaceHolder holder) {
            Canvas c = holder.lockCanvas();
            if (c == null) {
                Log.i(TAG, "surface on top: no canvas");
                return;
            }
            c.drawColor(Color.RED);
            holder.unlockCanvasAndPost(c);
            Log.i(TAG, "surface on top drew");
        }
        @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {}
        @Override public void surfaceDestroyed(SurfaceHolder holder) {}
    }

    private static GridLayout.Spec spec() { return GridLayout.spec(GridLayout.UNDEFINED); }

    @Override
    protected void onDestroy() {
        mRunning = false;
        super.onDestroy();
    }
}
