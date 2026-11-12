// android.graphics.HardwareBufferRenderer -- draws a RenderNode into a
// HardwareBuffer off screen (API 34). Source: frameworks/base
// graphics/java/android/graphics/HardwareBufferRenderer.java.
//
// A HardwareBuffer here is a format and size descriptor with no pixel store a
// renderer could fill (android.hardware.HardwareBuffer), so a draw cannot
// produce the picture. The request is taken and answered on the caller's
// executor with ERROR_UNKNOWN, AOSP's status for a render that failed, so code
// waiting on the result goes on to its fallback instead of hanging.
package android.graphics;

import android.hardware.HardwareBuffer;
import android.hardware.SyncFence;

import java.util.concurrent.Executor;
import java.util.function.Consumer;

public class HardwareBufferRenderer implements AutoCloseable {

    private final HardwareBuffer mHardwareBuffer;
    private final RenderRequest mRenderRequest;
    private RenderNode mContentRoot;
    private boolean mClosed;

    public HardwareBufferRenderer(HardwareBuffer buffer) {
        if (buffer == null) {
            throw new NullPointerException("buffer");
        }
        mHardwareBuffer = buffer;
        mRenderRequest = new RenderRequest();
    }

    /** The node whose content a draw renders; null clears it. */
    public void setContentRoot(RenderNode content) { mContentRoot = content; }

    /** AOSP hands out the renderer's one request, reset to its defaults. */
    public RenderRequest obtainRenderRequest() {
        mRenderRequest.reset();
        return mRenderRequest;
    }

    public boolean isClosed() { return mClosed; }

    @Override public void close() { mClosed = true; }

    /** The parameters of one draw; draw() sends it. */
    public final class RenderRequest {
        private ColorSpace mColorSpace = ColorSpace.get(ColorSpace.Named.SRGB);
        private int mTransform = 0;  // SurfaceControl.BUFFER_TRANSFORM_IDENTITY

        private RenderRequest() {}

        /**
         * Renders the content root into the buffer and reports on the executor.
         *
         * @throws IllegalStateException when the renderer is closed, as AOSP
         */
        public void draw(Executor executor, Consumer<RenderResult> renderCallback) {
            if (isClosed()) {
                throw new IllegalStateException("Attempt to draw with a HardwareBufferRenderer instance that has "
                        + "already been closed");
            }
            if (executor == null || renderCallback == null) {
                throw new NullPointerException((executor == null) ? "executor" : "renderCallback");
            }
            final RenderResult result = new RenderResult(SyncFence.createEmpty(), RenderResult.ERROR_UNKNOWN);
            executor.execute(() -> renderCallback.accept(result));
        }

        void reset() {
            mColorSpace = ColorSpace.get(ColorSpace.Named.SRGB);
            mTransform = 0;
        }

        /** The colour space to render in; null means sRGB. */
        public RenderRequest setColorSpace(ColorSpace colorSpace) {
            mColorSpace = (colorSpace == null) ? ColorSpace.get(ColorSpace.Named.SRGB) : colorSpace;
            return this;
        }

        /**
         * One of the SurfaceControl.BUFFER_TRANSFORM_* rotations (identity 0,
         * rotate 90 = 4, rotate 180 = 3, rotate 270 = 7).
         *
         * @throws IllegalArgumentException for anything else, as AOSP
         */
        public RenderRequest setBufferTransform(int bufferTransform) {
            boolean validTransform = bufferTransform == 0 || bufferTransform == 4 || bufferTransform == 3
                    || bufferTransform == 7;
            if (!validTransform) {
                throw new IllegalArgumentException("Invalid transform provided, must be one of the "
                        + "SurfaceControl.BufferTransform values");
            }
            mTransform = bufferTransform;
            return this;
        }
    }

    /** What became of a draw. */
    public static final class RenderResult {
        /** The contents were rendered. */
        public static final int SUCCESS = 0;
        /** The render failed. */
        public static final int ERROR_UNKNOWN = 1;

        private final SyncFence mFence;
        private final int mResultStatus;

        private RenderResult(SyncFence fence, int resultStatus) {
            mFence = fence;
            mResultStatus = resultStatus;
        }

        /** Signals when the buffer may be read. Never null. */
        public SyncFence getFence() { return mFence; }

        public int getStatus() { return mResultStatus; }
    }
}
