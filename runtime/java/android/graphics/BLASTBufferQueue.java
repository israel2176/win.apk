package android.graphics;

import android.view.Surface;
import android.view.SurfaceControl;

import java.util.function.Consumer;

/**
 * AOSP's android.graphics.BLASTBufferQueue, Win.apk's own (left out of lib/aosp-graphics.jar by
 * aosp_gfx_take.sh, as SurfaceTexture is): on Android, the buffer queue of a BLAST layer, whose
 * buffers reach SurfaceFlinger in transactions on that layer. Here the layer's buffers are the
 * graphics DLL's layer behind the SurfaceControl (the window manager's compositor gives every buffer
 * layer built with a parent one: SurfaceControl.MacapkLayerTree), which HWUI draws into through the
 * Surface createSurface gives, and the compositor composes as it composes the windows. So update is
 * the layer's buffer size, and a transaction merged with the next frame is applied now, as that frame
 * is being drawn (a Magnifier shows and moves its popup so). Its natives (libgui's BLASTBufferQueue)
 * are not in the DLL. ViewRootImpl's own windows do not use it (the window session never answers
 * with BLAST), as on Android before BLAST.
 */
public final class BLASTBufferQueue {
    /** AOSP's native object: nonzero until destroy, which callers read as "alive". */
    public long mNativeObject;

    private static final java.util.concurrent.atomic.AtomicLong sIds = new java.util.concurrent.atomic.AtomicLong();
    private final String mName;
    private SurfaceControl mSurfaceControl;
    private int mWidth, mHeight, mFormat;
    private TransactionHangCallback mHangCallback;

    public BLASTBufferQueue(String name, SurfaceControl sc, int width, int height, int format) {
        this(name, true);
        update(sc, width, height, format);
    }

    public BLASTBufferQueue(String name, boolean updateDestinationFrame) {
        mName = name;
        mNativeObject = sIds.incrementAndGet();
    }

    public void destroy() {
        mNativeObject = 0;
        mSurfaceControl = null;
    }

    /** A Surface that draws into the layer: HWUI finds the layer by the Surface's layer post. */
    public Surface createSurface() {
        Surface surface = new Surface();
        SurfaceControl sc = mSurfaceControl;
        if (sc != null && sc.isValid()) surface.copyFrom(sc);
        return surface;
    }

    public Surface createSurfaceWithHandle() {
        return createSurface();
    }

    /** AOSP: the next frame's transaction goes to the callback instead of SurfaceFlinger. Frames
     *  reach the compositor here without one, so the callback gets an empty transaction now. */
    public boolean syncNextTransaction(boolean acquireSingleBuffer, Consumer<SurfaceControl.Transaction> callback) {
        if (callback != null) callback.accept(new SurfaceControl.Transaction());
        return true;
    }

    public boolean syncNextTransaction(Consumer<SurfaceControl.Transaction> callback) {
        return syncNextTransaction(true, callback);
    }

    public void stopContinuousSyncTransaction() {}

    public void clearSyncTransaction() {}

    /** The layer the queue feeds and its buffers' size and format. */
    public void update(SurfaceControl sc, int width, int height, int format) {
        mSurfaceControl = sc;
        mWidth = width;
        mHeight = height;
        mFormat = format;
        if (sc != null && sc.isValid()) new SurfaceControl.Transaction().setBufferSize(sc, width, height).apply();
    }

    /** AOSP: t is applied with the frame `frameNumber`, the one being drawn when a frame callback
     *  calls this; here, at once. t is left empty, as merging leaves it. */
    public void mergeWithNextTransaction(SurfaceControl.Transaction t, long frameNumber) {
        if (t != null) new SurfaceControl.Transaction().merge(t).apply();
    }

    /** AOSP: a native transaction (HWUI's); none is made here. */
    public void mergeWithNextTransaction(long nativeTransaction, long frameNumber) {}

    public void applyPendingTransactions(long frameNumber) {}

    public long getLastAcquiredFrameNum() { return 0; }

    public boolean isSameSurfaceControl(SurfaceControl sc) {
        SurfaceControl mine = mSurfaceControl;
        return mine != null && sc != null && mine.isSameSurface(sc);
    }

    /** AOSP: the transactions waiting for frames up to frameNumber; none wait here. */
    public SurfaceControl.Transaction gatherPendingTransactions(long frameNumber) {
        return new SurfaceControl.Transaction();
    }

    public void setTransactionHangCallback(TransactionHangCallback hangCallback) {
        mHangCallback = hangCallback;
    }

    public interface TransactionHangCallback {
        void onTransactionHang(String reason);
    }
}
