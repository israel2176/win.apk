package android.view;

import android.graphics.Matrix;
import android.graphics.Rect;
import android.graphics.Region;
import android.os.Parcel;
import android.os.Parcelable;
import java.io.Closeable;

/**
 * AOSP-faithful (minimal) android.view.SurfaceControl. Real Android uses this as
 * a SurfaceFlinger layer handle plus a batched Transaction for compositor-side
 * geometry/visibility. Win.apk's SurfaceFlinger is the in-process window manager's
 * compositor (com.winapk.aospviews.Compositor, layers in the graphics DLL): a window's
 * control is laid out by the window manager, and a layer built as a child of another
 * (SurfaceControl.Builder.setParent: the text Magnifier's popup, a drag shadow) is kept
 * here as SurfaceFlinger keeps it -- its parent, position, z-order and visibility, set
 * by Transactions -- and composed over its window: a buffer layer with a parent gets a
 * layer of its own when built (MacapkLayerTree). The other Transaction setters chain and
 * record nothing (no crop, alpha, matrix or color layers are composed). Mirrors
 * frameworks/base/core/java/android/view/SurfaceControl.java.
 */
public final class SurfaceControl implements Parcelable {
    private String mName;
    private int mWidth;
    private int mHeight;
    private boolean mValid = true;
    /** AOSP's native ASurfaceControl, which HardwareRenderer.setSurfaceControl reads (to give HWUI
     *  a window's SurfaceControl, BLAST's): none here, the window's layer is its Surface's
     *  (Win.apk's windows/hwui_build compositor), so HWUI is given none. */
    public final long mNativeObject = 0;

    // ---- the layer tree (what SurfaceFlinger keeps of each layer) ------------------------

    /** AOSP's Builder flags this class reads. */
    public static final int HIDDEN = 0x4;
    public static final int FX_SURFACE_NORMAL = 0x0;
    public static final int FX_SURFACE_EFFECT = 0x20000;
    public static final int FX_SURFACE_BLAST = 0x40000;
    public static final int FX_SURFACE_CONTAINER = 0x80000;
    public static final int FX_SURFACE_MASK = 0xF0000;

    /** The layer itself, shared by every handle to it (copyFrom, the copy constructor): its
     *  parent, its position in the parent, its z among the parent's children, whether it is
     *  shown, whether it holds buffers (a container or effect layer does not). Guarded by
     *  sTreeLock: transactions apply on any thread (a Magnifier's on HWUI's render thread). */
    private static final class Layer {
        final int id = sLayerIds.incrementAndGet();
        SurfaceControl parent;
        float x, y;
        int z;
        boolean shown;
        boolean buffer = true;
        /** The handles that name it and are not released; the last release ends the layer. */
        int handles = 1;
        java.lang.ref.WeakReference<View> owner;
        java.util.ArrayList<OnReparentListener> reparentListeners;
    }
    private static final java.util.concurrent.atomic.AtomicInteger sLayerIds =
            new java.util.concurrent.atomic.AtomicInteger();
    private static final Object sTreeLock = new Object();
    private Layer mLayer = new Layer();
    private String mCallsite;

    /** Win.apk framework-internal: the compositor that composes the layers built as children
     *  (com.winapk.aospviews.Compositor, which sets itself with macapkSetLayerTree). */
    public interface MacapkLayerTree {
        /** A buffer layer with a parent was built: it gets a layer to draw into (its layer
         *  post, macapkSetLayerPost), hidden until a transaction shows it if the builder said so. */
        void layerBuilt(SurfaceControl sc);
        /** A transaction moved, showed, hid, resized, reparented or removed a child layer. */
        void treeChanged();
        /** The last handle to a child buffer layer was released: its layer goes. */
        void layerReleased(SurfaceControl sc);
    }
    private static volatile MacapkLayerTree sTree;
    public static void macapkSetLayerTree(MacapkLayerTree tree) { sTree = tree; }

    /** Win.apk framework-internal, for the compositor: the layer's parent (a handle to it),
     *  null for a window's own layer or one reparented to none. */
    public SurfaceControl macapkParent() {
        synchronized (sTreeLock) { return mLayer.parent; }
    }

    /** Win.apk framework-internal, for the compositor: {x, y, z, shown (1/0), id} of the layer,
     *  its position in its parent rounded to pixels. */
    public int[] macapkPlacement() {
        synchronized (sTreeLock) {
            Layer l = mLayer;
            return new int[] {Math.round(l.x), Math.round(l.y), l.z, l.shown ? 1 : 0, l.id};
        }
    }

    SurfaceControl(String name, int width, int height) {
        mName = name;
        mWidth = width;
        mHeight = height;
    }

    /** AOSP SurfaceControl(SurfaceControl other, String callsite): a second handle to the same
     *  layer. */
    public SurfaceControl(SurfaceControl other, String callsite) {
        mValid = false;
        copyFrom(other, callsite);
    }

    /** AOSP's public SurfaceControl(): an empty handle, not valid until copyFrom or
     *  readFromParcel points it at a layer (ViewRootImpl's mSurfaceControl, which
     *  IWindowSession.relayout fills). */
    public SurfaceControl() {
        mName = "SurfaceControl";
        mValid = false;
    }

    /** AOSP copyFrom(SurfaceControl other, String callsite): this handle now names the
     *  same layer as `other` (the in-process window session's relayout answer). */
    public void copyFrom(SurfaceControl other, String callsite) {
        if (mValid) releaseHandle();
        mName = other.mName;
        mWidth = other.mWidth;
        mHeight = other.mHeight;
        mValid = other.mValid;
        mLayerPost = other.mLayerPost;
        mCallsite = callsite;
        synchronized (sTreeLock) {
            mLayer = other.mLayer;
            if (mValid) mLayer.handles++;
        }
    }

    /** Win.apk framework-internal: where a Surface copied from this layer hands each frame it
     *  posts, in place of presenting it to the host window (the in-process window manager of
     *  AOSP's views composes its windows' layers; -Dwinapk.aosp.views=1). Null: the Surface
     *  presents as before. Copied with the layer by copyFrom. */
    private Surface.MacapkLayerPost mLayerPost;
    public void macapkSetLayerPost(Surface.MacapkLayerPost post) { mLayerPost = post; }
    public Surface.MacapkLayerPost macapkLayerPost() { return mLayerPost; }

    private int mTransformHint;
    /** AOSP's buffer transform hint: recorded, as there is no compositor to rotate for. */
    public void setTransformHint(int transformHint) { mTransformHint = transformHint; }
    public int getTransformHint() { return mTransformHint; }

    /** AOSP SurfaceControl.rotationToBufferTransform: Surface.ROTATION_* to the
     *  NATIVE_WINDOW_TRANSFORM_* a buffer is rotated by (0, ROT_90 4, ROT_180 3, ROT_270 7). */
    public static int rotationToBufferTransform(int rotation) {
        switch (rotation) {
            case Surface.ROTATION_0: return 0;
            case Surface.ROTATION_90: return 4;
            case Surface.ROTATION_180: return 3;
            case Surface.ROTATION_270: return 7;
        }
        return 0;
    }

    /** AOSP release(): this handle no longer names the layer; a layer built as a child of
     *  another goes with its last handle (the compositor's MacapkLayerTree.layerReleased; on
     *  Android SurfaceFlinger keeps it while its parent does, which shows only a layer no
     *  handle can change any more). */
    public void release() {
        if (!mValid) return;
        mValid = false;
        releaseHandle();
    }

    private void releaseHandle() {
        boolean last;
        synchronized (sTreeLock) {
            last = --mLayer.handles == 0 && mLayer.buffer;
        }
        MacapkLayerTree tree = sTree;
        if (last && tree != null && mLayerPost != null) tree.layerReleased(this);
    }

    public boolean isValid() { return mValid; }
    public String getName() { return mName; }
    public int getWidth() { return mWidth; }
    public int getHeight() { return mHeight; }

    /** AOSP getLayerId(): the layer's id in the compositor (SurfaceFlinger's sequence there),
     *  -1 for a handle that names none. */
    public int getLayerId() {
        if (!mValid) return -1;
        synchronized (sTreeLock) { return mLayer.id; }
    }

    /** AOSP isSameSurface(SurfaceControl): whether both handles name one layer. */
    public boolean isSameSurface(SurfaceControl other) {
        synchronized (sTreeLock) { return other.mLayer == mLayer; }
    }

    /** AOSP getLocalOwnerView(): the view the builder named as the layer's owner, if alive. */
    public View getLocalOwnerView() {
        synchronized (sTreeLock) {
            return mLayer.owner != null ? mLayer.owner.get() : null;
        }
    }

    /** AOSP getCallsite() (package-private there): who made this handle, for the leak report. */
    String getCallsite() { return mCallsite; }

    /** AOSP setUnreleasedWarningCallSite(String): names the caller its CloseGuard reports when
     *  the handle is collected unreleased; no CloseGuard is armed here, the name is kept. */
    public void setUnreleasedWarningCallSite(String callsite) {
        if (!isValid()) return;
        mCallsite = callsite;
    }

    /** AOSP OnReparentListener: told when a transaction that reparents the layer is applied. */
    public interface OnReparentListener {
        void onReparent(Transaction transaction, SurfaceControl parent);
    }

    /** AOSP addOnReparentListener(OnReparentListener). */
    public boolean addOnReparentListener(OnReparentListener listener) {
        synchronized (sTreeLock) {
            if (mLayer.reparentListeners == null) mLayer.reparentListeners = new java.util.ArrayList<>(1);
            return mLayer.reparentListeners.add(listener);
        }
    }

    /** AOSP removeOnReparentListener(OnReparentListener). */
    public boolean removeOnReparentListener(OnReparentListener listener) {
        synchronized (sTreeLock) {
            return mLayer.reparentListeners != null && mLayer.reparentListeners.remove(listener);
        }
    }

    /** AOSP dumpDebug(ProtoOutputStream, long): SurfaceControlProto's hash code, name and layer id. */
    public void dumpDebug(android.util.proto.ProtoOutputStream proto, long fieldId) {
        long token = proto.start(fieldId);
        proto.write(0x10500000001L, System.identityHashCode(this));
        proto.write(0x10900000002L, mName);
        proto.write(0x10500000003L, getLayerId());
        proto.end(token);
    }

    /** AOSP addJankDataListener(OnJankDataListener, SurfaceControl): SurfaceFlinger's frame
     *  timeline reports jank to it. There is no frame timeline here (see JankData): it is never
     *  called, as on a device whose compositor reports none. */
    public static void addJankDataListener(OnJankDataListener listener, SurfaceControl surface) {}

    /** AOSP removeJankDataListener(OnJankDataListener). */
    public static void removeJankDataListener(OnJankDataListener listener) {}

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {
        dest.writeString(mName);
        dest.writeInt(mWidth);
        dest.writeInt(mHeight);
    }

    public static final Parcelable.Creator<SurfaceControl> CREATOR =
            new Parcelable.Creator<SurfaceControl>() {
        @Override public SurfaceControl createFromParcel(Parcel in) {
            return new SurfaceControl(in.readString(), in.readInt(), in.readInt());
        }
        @Override public SurfaceControl[] newArray(int size) {
            return new SurfaceControl[size];
        }
    };

    /** AOSP SurfaceControl.readFromParcel(Parcel): re-point THIS handle at the
     *  layer described by the parcel, rather than allocating a new one (that is
     *  CREATOR's job). AOSP throws when the handle is already assigned a native
     *  object; ours has no native object to leak, so it simply overwrites. */
    public void readFromParcel(Parcel in) {
        if (in == null) {
            throw new IllegalArgumentException("source must not be null");
        }
        if (mValid) releaseHandle();
        synchronized (sTreeLock) { mLayer = new Layer(); }
        mName = in.readString();
        mWidth = in.readInt();
        mHeight = in.readInt();
        mValid = true;
    }

    /** AOSP SurfaceControl.TrustedPresentationThresholds -- the "is this layer
     *  really visible to the user" thresholds a trusted-presentation callback is
     *  armed with. Deprecated at API 35 in favour of
     *  android.window.TrustedPresentationThresholds. The constructor's argument
     *  validation is AOSP's own. */
    @Deprecated
    public static final class TrustedPresentationThresholds {
        private final float mMinAlpha;
        private final float mMinFractionRendered;
        private final int mStabilityRequirementMs;

        public TrustedPresentationThresholds(float minAlpha, float minFractionRendered,
                int stabilityRequirementMs) {
            if (!(minAlpha > 0f && minAlpha <= 1f)) {
                throw new IllegalArgumentException("minAlpha must be greater than 0 and "
                        + "less than or equal to 1");
            }
            if (!(minFractionRendered > 0f && minFractionRendered <= 1f)) {
                throw new IllegalArgumentException("minFractionRendered must be greater "
                        + "than 0 and less than or equal to 1");
            }
            if (stabilityRequirementMs < 1) {
                throw new IllegalArgumentException("stabilityRequirementMs must be at "
                        + "least 1");
            }
            mMinAlpha = minAlpha;
            mMinFractionRendered = minFractionRendered;
            mStabilityRequirementMs = stabilityRequirementMs;
        }
        /** @hide */ public float macapkMinAlpha() { return mMinAlpha; }
        /** @hide */ public float macapkMinFractionRendered() { return mMinFractionRendered; }
        /** @hide */ public int macapkStabilityRequirementMs() { return mStabilityRequirementMs; }
    }

    /** AOSP SurfaceControl.TransactionCommittedListener: told once a transaction
     *  has been latched by the compositor. Mac.apk applies transactions
     *  synchronously (apply() is a no-op because macOS composites the window
     *  itself), so "committed" is true the moment apply() returns -- see
     *  Transaction.apply(). */
    public interface TransactionCommittedListener {
        void onTransactionCommitted();
    }

    /** AOSP SurfaceControl.JankData: per-frame jank classification a
     *  compositor reports back. Mac.apk has no SurfaceFlinger frame timeline, so
     *  no JankData is ever produced (see AttachedSurfaceControl.
     *  registerOnJankDataListener); the type exists because the listener
     *  signature names it. */
    public static class JankData {
        public static final int JANK_NONE = 0x0;
        public static final int JANK_COMPOSER = 0x1;
        public static final int JANK_APPLICATION = 0x2;
        public static final int JANK_OTHER = 0x4;

        private final long mVsyncId;
        private final int mJankType;
        private final long mScheduledAppFrameTimeNanos;
        private final long mActualAppFrameTimeNanos;
        /** AOSP 14's fields (FrameTracker reads them): the same values as the getters. */
        public final long frameVsyncId;
        public final int jankType;

        /** @hide -- only the framework produces JankData. */
        JankData(long vsyncId, int jankType, long scheduledAppFrameTimeNanos,
                long actualAppFrameTimeNanos) {
            mVsyncId = vsyncId;
            mJankType = jankType;
            mScheduledAppFrameTimeNanos = scheduledAppFrameTimeNanos;
            mActualAppFrameTimeNanos = actualAppFrameTimeNanos;
            frameVsyncId = vsyncId;
            this.jankType = jankType;
        }

        /** AOSP 14's JankData(long frameVsyncId, int jankType). */
        public JankData(long frameVsyncId, int jankType) {
            this(frameVsyncId, jankType, 0, 0);
        }
        public long getVsyncId() { return mVsyncId; }
        public int getJankType() { return mJankType; }
        public long getScheduledAppFrameTimeNanos() { return mScheduledAppFrameTimeNanos; }
        public long getActualAppFrameTimeNanos() { return mActualAppFrameTimeNanos; }
    }

    /** AOSP SurfaceControl.OnJankDataListener. */
    public interface OnJankDataListener {
        void onJankDataAvailable(java.util.List<JankData> jankData);
    }

    /** AOSP SurfaceControl.OnJankDataListenerRegistration: the handle returned
     *  by AttachedSurfaceControl.registerOnJankDataListener. */
    public static class OnJankDataListenerRegistration {
        /** @hide */ OnJankDataListenerRegistration() {}
        /** AOSP: deliver any buffered jank data now. Nothing is ever buffered
         *  here (no frame timeline exists), so this is the same no-op AOSP
         *  performs for a registration with no pending data. */
        public void flush() {}
        /** AOSP: stop reporting after the given vsync id. */
        public void removeAfter(long afterVsync) {}
    }

    /** AOSP SurfaceControl.RefreshRateRange: a range of refresh rates, in Hz (DisplayInfo's
     *  app-requested range, the display mode's). AOSP's class. */
    public static final class RefreshRateRange implements Parcelable {
        public static final String TAG = "RefreshRateRange";
        public static final float FLOAT_TOLERANCE = 0.01f;
        public float min;
        public float max;

        public RefreshRateRange() {}

        public RefreshRateRange(float min, float max) {
            if (min < 0 || max < 0 || min > max + FLOAT_TOLERANCE) {
                android.util.Log.e(TAG, "Wrong values for min and max when initializing RefreshRateRange : "
                        + min + " " + max);
                this.min = this.max = 0;
                return;
            }
            if (min > max) {
                float t = min;
                min = max;
                max = t;
            }
            this.min = min;
            this.max = max;
        }

        @Override public boolean equals(Object other) {
            if (other == this) return true;
            if (!(other instanceof RefreshRateRange)) return false;
            RefreshRateRange r = (RefreshRateRange) other;
            return min == r.min && max == r.max;
        }

        @Override public int hashCode() { return java.util.Objects.hash(min, max); }

        @Override public String toString() { return "(" + min + " " + max + ")"; }

        public void copyFrom(RefreshRateRange other) {
            min = other.min;
            max = other.max;
        }

        @Override public void writeToParcel(Parcel dest, int flags) {
            dest.writeFloat(min);
            dest.writeFloat(max);
        }

        @Override public int describeContents() { return 0; }

        public static final Parcelable.Creator<RefreshRateRange> CREATOR =
                new Parcelable.Creator<RefreshRateRange>() {
            @Override public RefreshRateRange createFromParcel(Parcel in) {
                return new RefreshRateRange(in.readFloat(), in.readFloat());
            }
            @Override public RefreshRateRange[] newArray(int size) { return new RefreshRateRange[size]; }
        };
    }

    // AOSP-conformance (framework_lint vs android-30 stub jar): AOSP does not
    // mark this final. Final-when-AOSP-is-not is the dangerous direction -- an
    // app compiled against real Android that subclasses this fails to resolve
    // against our jar. See docs/aosp-conformance-audit_2026-08-23.md.
    public static class Builder {
        private String mName = "SurfaceControl";
        private int mWidth;
        private int mHeight;
        /** AOSP's default: HIDDEN, until setHidden(false) or a transaction shows the layer. */
        private int mFlags = HIDDEN;
        private SurfaceControl mParent;
        private View mLocalOwnerView;
        private String mCallsite = "SurfaceControl.Builder";

        public Builder() {}
        /** AOSP Builder(SurfaceSession): a layer of that client of SurfaceFlinger; one client here. */
        public Builder(SurfaceSession session) {}

        public Builder setName(String name) { mName = name; return this; }
        public Builder setBufferSize(int width, int height) {
            if (width < 0 || height < 0) {
                throw new IllegalArgumentException("width and height must be positive");
            }
            mWidth = width; mHeight = height; return this;
        }
        public Builder setFormat(int format) { return this; }
        public Builder setProtected(boolean protectedContent) { return this; }
        public Builder setSecure(boolean secure) { return this; }
        public Builder setOpaque(boolean opaque) { return this; }
        public Builder setHidden(boolean hidden) {
            mFlags = hidden ? (mFlags | HIDDEN) : (mFlags & ~HIDDEN);
            return this;
        }
        public Builder setParent(SurfaceControl parent) { mParent = parent; return this; }
        public Builder setMetadata(int key, int data) { return this; }
        public Builder setEffectLayer() { mWidth = mHeight = 0; return setFlags(FX_SURFACE_EFFECT, FX_SURFACE_MASK); }
        public Builder setColorLayer() { return setEffectLayer(); }
        public Builder setBLASTLayer() { return setFlags(FX_SURFACE_BLAST, FX_SURFACE_MASK); }
        public Builder setContainerLayer() {
            mWidth = mHeight = 0;
            return setFlags(FX_SURFACE_CONTAINER, FX_SURFACE_MASK);
        }
        /** AOSP setFlags(int): all of the flags, as given. */
        public Builder setFlags(int flags) { mFlags = flags; return this; }
        private Builder setFlags(int flags, int mask) { mFlags = (mFlags & ~mask) | flags; return this; }
        public Builder setCallsite(String callsite) { mCallsite = callsite; return this; }
        public Builder setLocalOwnerView(View view) { mLocalOwnerView = view; return this; }

        public SurfaceControl build() {
            int type = mFlags & FX_SURFACE_MASK;
            if ((mWidth > 0 || mHeight > 0) && (type == FX_SURFACE_EFFECT || type == FX_SURFACE_CONTAINER)) {
                throw new IllegalStateException("Only buffer layers can set a valid buffer size.");
            }
            SurfaceControl sc = new SurfaceControl(mName, mWidth, mHeight);
            sc.mCallsite = mCallsite;
            Layer l = sc.mLayer;
            synchronized (sTreeLock) {
                l.parent = mParent;
                l.shown = (mFlags & HIDDEN) == 0;
                l.buffer = type != FX_SURFACE_EFFECT && type != FX_SURFACE_CONTAINER;
                if (mLocalOwnerView != null) l.owner = new java.lang.ref.WeakReference<>(mLocalOwnerView);
            }
            MacapkLayerTree tree = sTree;
            if (tree != null && mParent != null && l.buffer) tree.layerBuilt(sc);
            return sc;
        }
    }

    /**
     * A batch of compositor changes, applied together by apply(). What the compositor here
     * composes -- a layer's parent, position, z-order, visibility and buffer size -- is recorded
     * and applied to the layer tree (Win.apk's SurfaceFlinger: see the class note); the other
     * setters chain and record nothing.
     */
    public static class Transaction implements Closeable, Parcelable {
        public Transaction() {}

        /** One recorded change; answers whether it changed a child layer (one the compositor
         *  places over its window), so that the compositor composes again. */
        private interface Op {
            boolean run(java.util.List<Runnable> after);
        }
        private java.util.ArrayList<Op> mOps;

        private Transaction op(SurfaceControl sc, Op op) {
            if (sc == null) throw new IllegalArgumentException("Invalid SurfaceControl");
            synchronized (this) {
                if (mOps == null) mOps = new java.util.ArrayList<>(4);
                mOps.add(op);
            }
            return this;
        }

        /** The recorded changes, in order, on the layer tree; the compositor told once. */
        private void applyOps() {
            java.util.ArrayList<Op> ops;
            synchronized (this) {
                ops = mOps;
                mOps = null;
            }
            if (ops == null) return;
            boolean changed = false;
            java.util.List<Runnable> after = new java.util.ArrayList<>(0);
            synchronized (sTreeLock) {
                for (Op o : ops) changed |= o.run(after);
            }
            for (Runnable r : after) r.run();
            MacapkLayerTree tree = sTree;
            if (changed && tree != null) tree.treeChanged();
        }

        public void apply() { applyOps(); macapkFireCommitted(); }
        public void apply(boolean sync) { apply(); }
        public void applyAsyncUnsafe() { apply(); }

        /** AOSP clear(): the recorded changes are dropped. */
        public void clear() {
            synchronized (this) { mOps = null; }
        }

        public Transaction setVisibility(SurfaceControl sc, boolean visible) {
            return visible ? show(sc) : hide(sc);
        }
        public Transaction show(SurfaceControl sc) {
            return op(sc, after -> { sc.mLayer.shown = true; return sc.mLayer.parent != null; });
        }
        public Transaction hide(SurfaceControl sc) {
            return op(sc, after -> { sc.mLayer.shown = false; return sc.mLayer.parent != null; });
        }
        public Transaction setPosition(SurfaceControl sc, float x, float y) {
            return op(sc, after -> {
                sc.mLayer.x = x;
                sc.mLayer.y = y;
                return sc.mLayer.parent != null;
            });
        }
        public Transaction setScale(SurfaceControl sc, float scaleX, float scaleY) { return this; }
        /** AOSP setBufferSize(SurfaceControl, int, int): the size of the layer's buffers (BLAST's
         *  update gives it too). */
        public Transaction setBufferSize(SurfaceControl sc, int w, int h) {
            return op(sc, after -> {
                sc.mWidth = w;
                sc.mHeight = h;
                return sc.mLayer.parent != null;
            });
        }
        public Transaction setLayer(SurfaceControl sc, int z) {
            return op(sc, after -> { sc.mLayer.z = z; return sc.mLayer.parent != null; });
        }
        public Transaction setRelativeLayer(SurfaceControl sc, SurfaceControl relativeTo, int z) {
            return this;
        }
        public Transaction setLayerStack(SurfaceControl sc, int layerStack) { return this; }
        public Transaction setTransparentRegionHint(SurfaceControl sc, Region region) { return this; }
        public Transaction setAlpha(SurfaceControl sc, float alpha) { return this; }
        public Transaction setMatrix(SurfaceControl sc, float dsdx, float dtdx,
                float dtdy, float dsdy) { return this; }
        public Transaction setMatrix(SurfaceControl sc, Matrix matrix, float[] float9) { return this; }
        public Transaction setGeometry(SurfaceControl sc, Rect sourceCrop, Rect destFrame,
                int orientation) { return this; }
        public Transaction setWindowCrop(SurfaceControl sc, Rect crop) { return this; }
        public Transaction setWindowCrop(SurfaceControl sc, int width, int height) { return this; }
        public Transaction setCornerRadius(SurfaceControl sc, float cornerRadius) { return this; }
        public Transaction setBackgroundBlurRadius(SurfaceControl sc, int radius) { return this; }
        public Transaction setColor(SurfaceControl sc, float[] color) { return this; }
        public Transaction setOpaque(SurfaceControl sc, boolean isOpaque) { return this; }
        public Transaction setSecure(SurfaceControl sc, boolean isSecure) { return this; }
        /** AOSP reparent(SurfaceControl, SurfaceControl): the layer moves under newParent, or out of
         *  the tree (null: no longer composed); its reparent listeners are told when it applies. */
        public Transaction reparent(SurfaceControl sc, SurfaceControl newParent) {
            return op(sc, after -> {
                boolean was = sc.mLayer.parent != null;
                sc.mLayer.parent = newParent;
                if (sc.mLayer.reparentListeners != null) {
                    for (OnReparentListener l : new java.util.ArrayList<>(sc.mLayer.reparentListeners)) {
                        after.add(() -> l.onReparent(this, newParent));
                    }
                }
                return was || newParent != null;
            });
        }

        /** AOSP remove(SurfaceControl): reparent(sc, null), and the handle is released now. */
        public Transaction remove(SurfaceControl sc) {
            reparent(sc, null);
            sc.release();
            return this;
        }
        public Transaction setBuffer(SurfaceControl sc, Object buffer) { return this; }
        /** AOSP (API 33) setBuffer(SurfaceControl, HardwareBuffer): the layer's
         *  next content. No compositor takes it (see the class note). */
        public Transaction setBuffer(SurfaceControl sc, android.hardware.HardwareBuffer buffer) {
            return this;
        }
        /** AOSP (API 33): as above, with the fence that says when the buffer is
         *  ready to be read. */
        public Transaction setBuffer(SurfaceControl sc, android.hardware.HardwareBuffer buffer,
                android.hardware.SyncFence fence) {
            return this;
        }
        /** AOSP (API 34): as above, with a callback for when the compositor no
         *  longer reads the buffer and the app may draw into it again. Nothing
         *  ever reads it here, so it is the app's again at once: the callback
         *  is called now, with an already signalled fence. Left uncalled, an
         *  app that recycles a fixed pool of buffers would run out of them. */
        public Transaction setBuffer(SurfaceControl sc, android.hardware.HardwareBuffer buffer,
                android.hardware.SyncFence fence,
                java.util.function.Consumer<android.hardware.SyncFence> releaseCallback) {
            if (releaseCallback != null) {
                releaseCallback.accept(android.hardware.SyncFence.createEmpty());
            }
            return this;
        }
        /** AOSP merge(Transaction): other's changes join this one's, after them; other is left
         *  empty. */
        public Transaction merge(Transaction other) {
            if (other == null || other == this) return this;
            java.util.ArrayList<Op> ops;
            java.util.ArrayList<Runnable> listeners;
            synchronized (other) {
                ops = other.mOps;
                other.mOps = null;
                listeners = other.mCommittedListeners;
                other.mCommittedListeners = null;
            }
            synchronized (this) {
                if (ops != null) {
                    if (mOps == null) mOps = new java.util.ArrayList<>(ops.size());
                    mOps.addAll(ops);
                }
                if (listeners != null) {
                    if (mCommittedListeners == null) mCommittedListeners = new java.util.ArrayList<>(listeners.size());
                    mCommittedListeners.addAll(listeners);
                }
            }
            return this;
        }

        /** AOSP sendSurfaceFlushJankData(SurfaceControl): asks SurfaceFlinger for the jank data it
         *  holds; there is no frame timeline here (JankData), so there is none to send. */
        public static void sendSurfaceFlushJankData(SurfaceControl sc) {}

        // ---- AOSP API-36 surface completion (android.view lane) -------------
        // Same contract as every setter above: chain and record, because macOS
        // composites the window itself and there is no SurfaceFlinger
        // transaction to submit. Each one names the AOSP member it mirrors.

        /** AOSP Transaction.setCrop(SurfaceControl, Rect): the modern name for
         *  setWindowCrop, which this class already has. */
        public Transaction setCrop(SurfaceControl sc, Rect crop) { return this; }
        /** AOSP Transaction.setDamageRegion(SurfaceControl, Region). */
        public Transaction setDamageRegion(SurfaceControl sc, Region region) { return this; }
        /** AOSP Transaction.setDataSpace(SurfaceControl, int) -- the
         *  android.hardware.DataSpace value of the layer's buffers. */
        public Transaction setDataSpace(SurfaceControl sc, int dataspace) { return this; }
        /** AOSP Transaction.setBufferTransform(SurfaceControl, int) -- one of
         *  the Surface.ROTATION_* / mirror transforms applied to the buffer. */
        public Transaction setBufferTransform(SurfaceControl sc, int transform) { return this; }
        /** AOSP Transaction.setContentPriority(SurfaceControl, int) (API 36). */
        public Transaction setContentPriority(SurfaceControl sc, int priority) { return this; }
        /** AOSP Transaction.setExtendedRangeBrightness(SurfaceControl, float, float). */
        public Transaction setExtendedRangeBrightness(SurfaceControl sc,
                float currentBufferRatio, float desiredRatio) { return this; }
        /** AOSP Transaction.setDesiredHdrHeadroom(SurfaceControl, float). */
        public Transaction setDesiredHdrHeadroom(SurfaceControl sc, float desiredRatio) {
            return this;
        }
        /** AOSP Transaction.setDesiredPresentTimeNanos(long) (API 36). */
        public Transaction setDesiredPresentTimeNanos(long desiredPresentTimeNanos) {
            return this;
        }
        /** AOSP Transaction.setFrameTimeline(long vsyncId) (API 33). */
        public Transaction setFrameTimeline(long vsyncId) { return this; }

        /** AOSP Transaction.setFrameRate(SurfaceControl, float, int) delegates to
         *  the 3-arg form with CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS, exactly as
         *  Surface.setFrameRate(float,int) does. */
        public Transaction setFrameRate(SurfaceControl sc, float frameRate, int compatibility) {
            return setFrameRate(sc, frameRate, compatibility,
                    Surface.CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
        }
        /** AOSP Transaction.setFrameRate(SurfaceControl, float, int, int). Frame
         *  rate is a HINT even on a device; a fixed-refresh-rate display accepts
         *  it and does nothing, which is what macOS's CAMetalLayer amounts to
         *  here (see Surface.setFrameRate's own comment). */
        public Transaction setFrameRate(SurfaceControl sc, float frameRate, int compatibility,
                int changeFrameRateStrategy) { return this; }
        /** AOSP Transaction.setFrameRate(SurfaceControl, Surface.FrameRateParams)
         *  (API 36), the range form. */
        public Transaction setFrameRate(SurfaceControl sc, Surface.FrameRateParams params) {
            return this;
        }
        /** AOSP Transaction.clearFrameRate(SurfaceControl): drop this layer's
         *  frame-rate vote entirely. */
        public Transaction clearFrameRate(SurfaceControl sc) { return this; }

        /** AOSP Transaction.addTransactionCommittedListener(Executor,
         *  TransactionCommittedListener). AOSP fires the listener once
         *  SurfaceFlinger has latched the transaction. apply() here is
         *  synchronous and complete on return (there is no compositor to wait
         *  for), so the listener is armed at apply() time and fires then --
         *  never dropped, which would leave a caller waiting forever. */
        public Transaction addTransactionCommittedListener(
                java.util.concurrent.Executor executor, TransactionCommittedListener listener) {
            if (executor != null && listener != null) {
                if (mCommittedListeners == null) {
                    mCommittedListeners = new java.util.ArrayList<Runnable>(1);
                }
                mCommittedListeners.add(() -> executor.execute(listener::onTransactionCommitted));
            }
            return this;
        }
        private java.util.ArrayList<Runnable> mCommittedListeners;
        /** @hide -- run by apply(); see addTransactionCommittedListener. */
        final void macapkFireCommitted() {
            if (mCommittedListeners == null) return;
            java.util.ArrayList<Runnable> pending = mCommittedListeners;
            mCommittedListeners = null;
            for (int i = 0; i < pending.size(); i++) pending.get(i).run();
        }

        /** AOSP Transaction.setTrustedPresentationCallback(SurfaceControl,
         *  TrustedPresentationThresholds, Executor, Consumer&lt;Boolean&gt;),
         *  deprecated at API 35. AOSP calls back with true once the layer has
         *  met the thresholds for the required time. Mac.apk's single window is
         *  presented directly by macOS with nothing composited over it, so a
         *  layer that is shown DOES meet the thresholds -- the callback is
         *  invoked with true, which is the answer a device gives for an
         *  unobscured, fully-opaque, on-screen layer. */
        public Transaction setTrustedPresentationCallback(SurfaceControl sc,
                TrustedPresentationThresholds thresholds,
                java.util.concurrent.Executor executor,
                java.util.function.Consumer<Boolean> listener) {
            if (executor != null && listener != null) {
                executor.execute(() -> listener.accept(Boolean.TRUE));
            }
            return this;
        }
        /** AOSP Transaction.clearTrustedPresentationCallback(SurfaceControl),
         *  deprecated at API 35. */
        public Transaction clearTrustedPresentationCallback(SurfaceControl sc) { return this; }

        @Override public void close() { clear(); }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(Parcel dest, int flags) {}

        public static final Parcelable.Creator<Transaction> CREATOR =
                new Parcelable.Creator<Transaction>() {
            @Override public Transaction createFromParcel(Parcel in) { return new Transaction(); }
            @Override public Transaction[] newArray(int size) { return new Transaction[size]; }
        };
    }
}
