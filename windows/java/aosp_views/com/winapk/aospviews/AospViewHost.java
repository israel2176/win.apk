// The second ViewHost (runtime/java/com/macapk/loader/view/ViewHost.java): the runtime's door into
// android.view when the views are AOSP 14's own (WINAPK_AOSP_VIEWS=1; docs/research/aosp-views.md,
// step V1). An Activity's window is AOSP's PhoneWindow, its decor AOSP's DecorView, and it is added
// to AOSP's WindowManagerGlobal on resume, as ActivityThread.handleResumeActivity does: AOSP's
// ViewRootImpl then measures, lays out and draws it on Choreographer's frames, over the in-process
// window manager of AospWindows.
//
// What Win.apk's own window root did through this interface (attaching a root by hand, its frame
// and damage, touch mode, drag, the IME's active connection, the material chrome) has no meaning
// here: ViewRootImpl does it, and those methods answer as when nothing is attached (logged once).
// What stays Win.apk's with AOSP's views (Surface, SurfaceView, KeyEvent, InputDevice) goes to
// Win.apk's own host, ViewHost.winApk().
//
// Not yet (V2): input from the host into ViewRootImpl's input stages, the IME, accessibility, the
// status and navigation bars as insets, dialogs made by Win.apk's android.app.Dialog.
package com.winapk.aospviews;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.SurfaceTexture;
import android.os.Binder;
import android.view.Display;
import android.view.InputDevice;
import android.view.InputQueue;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewRootImpl;
import android.view.ViewTreeObserver;
import android.view.Window;
import android.view.WindowManager;
import android.view.WindowManagerGlobal;
import android.view.WindowManagerImpl;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.CaptioningManager;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.view.textclassifier.TextClassificationManager;
import android.view.textservice.TextServicesManager;
import android.widget.TextView;

import com.android.internal.policy.DecorView;
import com.android.internal.policy.PhoneLayoutInflater;
import com.android.internal.policy.PhoneWindow;
import com.macapk.loader.MetalSurfaceCallbacks;
import com.macapk.loader.view.ViewHost;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.WeakHashMap;
import java.util.concurrent.ConcurrentHashMap;

public final class AospViewHost implements ViewHost {

    public static final AospViewHost INSTANCE = new AospViewHost();

    private final ViewHost mWinApk = ViewHost.winApk();
    private final Set<String> mLogged = ConcurrentHashMap.newKeySet();
    /** Each context's own PhoneLayoutInflater, as SystemServiceRegistry caches one per ContextImpl. */
    private final Map<Context, LayoutInflater> mInflaters = Collections.synchronizedMap(new WeakHashMap<>());
    /** The activities whose decor is in WindowManagerGlobal. */
    private final Map<Activity, View> mAdded = Collections.synchronizedMap(new WeakHashMap<>());
    private volatile Context mApp;

    private AospViewHost() {
        System.err.println("[win.apk] AOSP views: AOSP 14's android.view and android.widget, PhoneWindow and"
                + " ViewRootImpl over the in-process window manager");
        AospWindows.ensureServices();
        // Every window draws in software into its Surface (HWUI's own pipeline is step V3): an app
        // window gets FLAG_HARDWARE_ACCELERATED from WindowManagerGlobal.addView when the
        // application is hardware accelerated, and ViewRootImpl then makes a ThreadedRenderer
        // unless the renderer is off for the process, as AOSP turns it off for a low-end
        // persistent process (ThreadedRenderer.initForSystemProcess).
        android.view.ThreadedRenderer.sRendererEnabled = false;
    }

    private void moot(String what) {
        if (mLogged.add(what)) System.err.println(AospWindows.TAG + what + ": Win.apk's window root only; not used with AOSP's views");
    }

    private void note(Context c) {
        if (c == null) return;
        AospWindows.noteContext(c);
        if (mApp == null) {
            Context app = c.getApplicationContext();
            mApp = app != null ? app : c;
        }
    }

    // ---- AOSP's own window root ----

    @Override public boolean aospWindows() { return true; }

    /** Activity.attach: `mWindow = new PhoneWindow(this, window, activityConfigCallback)`, its
     *  callback, its inflater's private factory, and its window manager (a local one over the
     *  context's WindowManagerImpl, with the window as parent). Not hardware accelerated: the
     *  window draws in software into its Surface (V3 is HWUI). */
    @Override public Window newActivityWindow(Activity activity) {
        note(activity);
        PhoneWindow w = new PhoneWindow(activity, null, null);
        w.setCallback(activity);
        w.getLayoutInflater().setPrivateFactory(activity);
        WindowManager wm = (WindowManager) activity.getSystemService(Context.WINDOW_SERVICE);
        String name = activity.getComponentName() != null ? activity.getComponentName().flattenToString()
                : activity.getClass().getName();
        w.setWindowManager(wm, new Binder(), name, false);
        return w;
    }

    @Override public Object systemService(Context context, String name) {
        note(context);
        switch (name) {
            case Context.WINDOW_SERVICE:
                return new WindowManagerImpl(context);
            case Context.LAYOUT_INFLATER_SERVICE:
                synchronized (mInflaters) {
                    LayoutInflater i = mInflaters.get(context);
                    if (i == null) {
                        i = new PhoneLayoutInflater(context);
                        mInflaters.put(context, i);
                    }
                    return i;
                }
            case Context.INPUT_METHOD_SERVICE:
                return InputMethodManager.forContext(context);
            case Context.ACCESSIBILITY_SERVICE:
                return AccessibilityManager.getInstance(context);
            case Context.CAPTIONING_SERVICE:
                return new CaptioningManager(context);
            case Context.TEXT_CLASSIFICATION_SERVICE:
                return new TextClassificationManager(context);
            case Context.TEXT_SERVICES_MANAGER_SERVICE:
                return newTextServicesManager(context);
            default:
                return null;
        }
    }

    // ---- Input (step V2): the host's events into the windows' own input path ----

    @Override public com.macapk.loader.TouchDispatcher newTouchDispatcher() { return new AospInput(); }

    @Override public boolean deliverInput(android.view.InputEvent event) {
        if (event instanceof android.view.KeyEvent k) {
            AospWindows.main().post(() -> InputRouter.key(k));
        } else if (event instanceof android.view.MotionEvent m) {
            AospWindows.main().post(() -> InputRouter.motion(m));
        } else {
            return false;
        }
        return true;
    }

    @Override public boolean injectInput(String verb, String[] words, String line) {
        return InjectInput.run(verb, words, line);
    }

    // ---- A window's root ----

    @Override public void dispatchAttachedToWindow(View root) { moot("dispatchAttachedToWindow"); }
    @Override public void dispatchDetachedFromWindow(View root) { moot("dispatchDetachedFromWindow"); }
    @Override public boolean isWindowRemoved(View root) { return false; }
    @Override public void clearWindowRemoved(View root) {}
    @Override public void removeWindow(View root) { moot("removeWindow"); }
    @Override public void forgetViewContext(Context context) {}
    @Override public void forgetInflaterContext(Context context) { mInflaters.remove(context); }
    @Override public void windowAdded(View root, WindowManager.LayoutParams params) { moot("windowAdded"); }
    @Override public void windowRemoved(View root) { moot("windowRemoved"); }

    /** ActivityThread.handleResumeActivity: the decor goes to the window manager once, as
     *  TYPE_BASE_APPLICATION, on the host window. */
    @Override public void activityWindowAdded(Activity activity) {
        if (activity == null || activity.isFinishing() || mAdded.containsKey(activity)) return;
        Window w = activity.getWindow();
        if (w == null) return;
        View decor = w.getDecorView();
        WindowManager.LayoutParams l = w.getAttributes();
        l.type = WindowManager.LayoutParams.TYPE_BASE_APPLICATION;
        AospWindows.ensureHostSurface(String.valueOf(activity.getTitle()));
        activity.getWindowManager().addView(decor, l);
        mAdded.put(activity, decor);
    }

    /** handleDestroyActivity: wm.removeViewImmediate(decor). */
    @Override public void activityWindowRemoved(Activity activity) {
        View decor = activity != null ? mAdded.remove(activity) : null;
        if (decor == null) return;
        try {
            activity.getWindowManager().removeViewImmediate(decor);
        } catch (RuntimeException e) {
            System.err.println(AospWindows.TAG + "removing " + activity.getClass().getName() + "'s window: " + e);
        }
    }

    @Override public void onWindowAddedForInput(Window window) {}

    @Override public DecorView newDecorView(Context context, int featureId, Window window,
            WindowManager.LayoutParams params) {
        throw new IllegalStateException("Win.apk's own window decor asked for with AOSP's views");
    }

    // ---- The frame: ViewRootImpl's own ----

    @Override public ViewRootImpl viewRoot() { return null; }

    private static List<View> roots() {
        try {
            return new ArrayList<>(WindowManagerGlobal.getInstance().getWindowViews());
        } catch (RuntimeException e) {
            return Collections.emptyList();
        }
    }

    @Override public void scheduleTraversal() { for (View v : roots()) v.invalidate(); }
    @Override public void scheduleLayout() { for (View v : roots()) v.requestLayout(); }
    @Override public boolean consumeTraversalScheduled(ViewRootImpl root) { return false; }
    @Override public boolean consumeLayoutRequested(ViewRootImpl root) { return false; }
    @Override public boolean damageGate(String env) { return false; }
    @Override public int consumeDamage(ViewRootImpl root, int[] out4) { return 0; }
    @Override public void beginDraw(View root, Canvas canvas, boolean partial, int kind, int[] rect) {}
    @Override public void endDraw() {}
    @Override public boolean endFrameDamage(ViewRootImpl root, int[] drawn) { return false; }
    @Override public void scheduleSameDamage(ViewRootImpl root) {}
    @Override public void driveMainFrame() {}
    @Override public ViewTreeObserver peekTreeObserver(View view) { return null; }
    @Override public boolean hasLayoutOrDrawListeners(ViewTreeObserver observer) { return false; }
    @Override public boolean consumeInsetsDispatchPending(View root) { return false; }

    // ---- What a view holds that AOSP keeps private ----

    @Override public int backgroundColor(View view) {
        android.graphics.drawable.Drawable bg = view.getBackground();
        return bg instanceof android.graphics.drawable.ColorDrawable cd ? cd.getColor() : 0;
    }

    @Override public int layoutGravity(ViewGroup.LayoutParams params) {
        if (params instanceof android.widget.FrameLayout.LayoutParams f) return f.gravity;
        if (params instanceof android.widget.LinearLayout.LayoutParams l) return l.gravity;
        return -1;
    }

    /** Gravity's names (the attribute's flag names), `|`-separated, as AAPT2 compiles them. */
    @Override public int parseGravity(String text) {
        if (text == null) return 0;
        String t = text.trim();
        if (t.startsWith("0x")) return (int) Long.parseLong(t.substring(2), 16);
        if (!t.isEmpty() && Character.isDigit(t.charAt(0))) return Integer.parseInt(t);
        int g = 0;
        for (String part : t.split("\\|")) {
            switch (part.trim()) {
                case "top": g |= android.view.Gravity.TOP; break;
                case "bottom": g |= android.view.Gravity.BOTTOM; break;
                case "left": g |= android.view.Gravity.LEFT; break;
                case "right": g |= android.view.Gravity.RIGHT; break;
                case "center_vertical": g |= android.view.Gravity.CENTER_VERTICAL; break;
                case "fill_vertical": g |= android.view.Gravity.FILL_VERTICAL; break;
                case "center_horizontal": g |= android.view.Gravity.CENTER_HORIZONTAL; break;
                case "fill_horizontal": g |= android.view.Gravity.FILL_HORIZONTAL; break;
                case "center": g |= android.view.Gravity.CENTER; break;
                case "fill": g |= android.view.Gravity.FILL; break;
                case "clip_vertical": g |= android.view.Gravity.CLIP_VERTICAL; break;
                case "clip_horizontal": g |= android.view.Gravity.CLIP_HORIZONTAL; break;
                case "start": g |= android.view.Gravity.START; break;
                case "end": g |= android.view.Gravity.END; break;
                default: break;
            }
        }
        return g;
    }

    /** A Win.apk SurfaceView's (or GL view's) host handle; AOSP's View has none. */
    @Override public long nativeHandle(View view) {
        try {
            Object r = view.getClass().getMethod("getNativeHandle").invoke(view);
            return r instanceof Long l ? l : 0L;
        } catch (ReflectiveOperationException | RuntimeException e) {
            return 0L;
        }
    }

    @Override public boolean hoverViews() { return false; }
    @Override public void setKeySource(KeyEvent event, int source) { mWinApk.setKeySource(event, source); }
    @Override public void setKeyUnicodeChar(KeyEvent event, int unicodeChar) {
        mWinApk.setKeyUnicodeChar(event, unicodeChar);
    }
    @Override public boolean inTouchMode() { return true; }
    @Override public boolean ensureTouchMode(View root, boolean inTouchMode) {
        ViewRootImpl r = root.getViewRootImpl();
        if (r == null) return false;
        try {
            // Package-private on AOSP (View.requestFocusFromTouch's way in).
            java.lang.reflect.Method m = ViewRootImpl.class.getDeclaredMethod("ensureTouchMode", boolean.class);
            m.setAccessible(true);
            return (Boolean) m.invoke(r, inTouchMode);
        } catch (ReflectiveOperationException e) {
            return false;
        }
    }

    private static Object listenerInfoField(View view, String field) {
        try {
            java.lang.reflect.Field li = View.class.getDeclaredField("mListenerInfo");
            li.setAccessible(true);
            Object info = li.get(view);
            if (info == null) return null;
            java.lang.reflect.Field f = info.getClass().getDeclaredField(field);
            f.setAccessible(true);
            return f.get(info);
        } catch (ReflectiveOperationException | RuntimeException e) {
            return null;
        }
    }

    @Override public View.OnTouchListener onTouchListener(View view) {
        return (View.OnTouchListener) listenerInfoField(view, "mOnTouchListener");
    }
    @Override public boolean hasOnTouchListener(View view) { return onTouchListener(view) != null; }
    @Override public boolean hasOnDragListener(View view) { return listenerInfoField(view, "mOnDragListener") != null; }
    @Override public InputQueue takenInputQueue(Window window) { return null; }

    @Override public InputDevice newInputDevice(int id, int generation, int controllerNumber, String name,
            int vendorId, int productId, String descriptor, boolean isExternal, int sources, int keyboardType,
            KeyCharacterMap keyCharacterMap, boolean hasVibrator, boolean hasMicrophone,
            boolean hasButtonUnderPad) {
        return mWinApk.newInputDevice(id, generation, controllerNumber, name, vendorId, productId, descriptor,
                isExternal, sources, keyboardType, keyCharacterMap, hasVibrator, hasMicrophone, hasButtonUnderPad);
    }

    // ---- Drag: AOSP's own DragEvent dispatch, through the window session (V2) ----

    @Override public boolean dragActive() { return false; }
    @Override public void dragAbort() {}
    @Override public boolean dragTakeSourceCancel() { return false; }
    @Override public View dragActiveRoot() { return null; }
    @Override public void dragPointer(int action, float x, float y) {}
    @Override public void drawDragShadow(Canvas canvas, View root) {}

    // ---- The IME: AOSP's InputMethodManager; the host's IME bridge is V2 ----

    @Override public TextView imeActiveInput() { return null; }
    @Override public InputConnection imeActiveConnection() { return ImeService.connection(); }
    @Override public TextView imeShowTarget() { return null; }
    @Override public String imeTraceFocused() { return ""; }
    @Override public int imeActionForEnter() { return 0; }
    @Override public boolean imePasswordTargetActive() { return false; }
    @Override public boolean handleSoftKey(TextView view, int unicodeChar, int keyCode) { return false; }
    @Override public boolean hasPasswordTransformationMethod(TextView view) {
        return view.getTransformationMethod() instanceof android.text.method.PasswordTransformationMethod;
    }

    // ---- The display: one, the host window's box; its system bars are Bars' ----

    @Override public Display newDisplay() { return AospWindows.defaultDisplay(); }
    @Override public int realWidthPx() { return com.macapk.loader.DisplayConfig.realWidthPx(); }
    @Override public int realHeightPx() { return com.macapk.loader.DisplayConfig.realHeightPx(); }
    // The decor the configuration subtracts (DisplayConfig's ladder): the bars' sizes when there are
    // bars, whatever a window asked of them, as DisplayPolicy's decor insets are.
    @Override public int navDecorWidthPx() { Bars.update(); return Bars.navOnBottom() ? 0 : Bars.navThickness(); }
    @Override public int navDecorHeightPx() { Bars.update(); return Bars.navOnBottom() ? Bars.navThickness() : 0; }
    @Override public int navDecorPx() { Bars.update(); return Bars.navThickness(); }
    @Override public int statusDecorPx() { Bars.update(); return Bars.statusHeight(); }
    // Hidden bars and FLAG_FULLSCREEN are the controlling window's requested visibility (Bars).
    @Override public boolean fullscreen() { return Bars.hidden(android.view.WindowInsets.Type.statusBars()); }
    @Override public boolean barHidden(int type) { return Bars.hidden(type); }
    @Override public boolean decorFitsSystemWindows() { return true; }
    @Override public int barAppearance(int bit) { return (Bars.appearance() & bit) != 0 ? 1 : 0; }
    @Override public int forcedStatusBarColor() { return barColorUnset(); }
    @Override public int forcedNavigationBarColor() { return barColorUnset(); }
    @Override public int barColorUnset() { return mWinApk.barColorUnset(); }

    // ---- View tree owners: androidx tags AOSP's decor itself ----

    @Override public String[][] viewTreeOwners() { return new String[0][]; }
    @Override public void registerOwnerKey(int key) {}

    @Override public List<Object> keyedTagValues(View view) {
        List<Object> out = new ArrayList<>();
        try {
            java.lang.reflect.Field f = View.class.getDeclaredField("mKeyedTags");
            f.setAccessible(true);
            android.util.SparseArray<?> tags = (android.util.SparseArray<?>) f.get(view);
            if (tags != null) for (int i = 0; i < tags.size(); i++) out.add(tags.valueAt(i));
        } catch (ReflectiveOperationException | RuntimeException e) {
            // none
        }
        return out;
    }

    @Override public void retryComposeAttaches() {}

    // ---- Managers ----

    private Context app() {
        Context c = mApp;
        if (c == null) throw new IllegalStateException("AOSP views: a manager asked for before any context");
        return c;
    }

    @Override public AccessibilityManager newAccessibilityManager() { return AccessibilityManager.getInstance(app()); }
    @Override public CaptioningManager newCaptioningManager() { return new CaptioningManager(app()); }

    @Override public TextServicesManager newTextServicesManager(Context context) {
        try {
            return TextServicesManager.createInstance(context);
        } catch (Exception e) {
            return null;
        }
    }

    @Override public TextClassificationManager newTextClassificationManager() {
        return new TextClassificationManager(app());
    }

    @Override public void setFontFamily(TextView view, String family) {
        android.graphics.Typeface tf = view.getTypeface();
        view.setTypeface(android.graphics.Typeface.create(family, tf != null ? tf.getStyle() : 0));
    }

    @Override public void setTextStyle(TextView view, int style) { view.setTypeface(view.getTypeface(), style); }

    // ---- Surfaces: Win.apk's classes ----

    @Override public SurfaceHolder.Callback2 takenSurface(Window window) { return null; }
    @Override public SurfaceTexture surfaceTexture(Surface surface) { return mWinApk.surfaceTexture(surface); }
    @Override public SurfaceView ownerSurfaceView(Surface surface) { return mWinApk.ownerSurfaceView(surface); }
    @Override public void noteNativeWindowConsumer(Surface surface) { mWinApk.noteNativeWindowConsumer(surface); }
    @Override public void setFrameSink(Surface surface, FrameSink sink, int width, int height) {
        mWinApk.setFrameSink(surface, sink, width, height);
    }
    @Override public void setOverlayDriver(MetalSurfaceCallbacks driver, int widthPx, int heightPx) {
        mWinApk.setOverlayDriver(driver, widthPx, heightPx);
    }
    @Override public long activeVideoRectPacked() { return mWinApk.activeVideoRectPacked(); }
    @Override public void registerVideoSink(Surface surface) { mWinApk.registerVideoSink(surface); }
    @Override public void unregisterVideoSink(Surface surface) { mWinApk.unregisterVideoSink(surface); }
    @Override public boolean presentVideoFrame(Surface surface, int[] argb, int width, int height) {
        return mWinApk.presentVideoFrame(surface, argb, width, height);
    }
    @Override public boolean isSurfaceViewGlThread(Thread thread) { return mWinApk.isSurfaceViewGlThread(thread); }
    @Override public void markGlThreadSurvivedRefusal(Thread thread, String call) {
        mWinApk.markGlThreadSurvivedRefusal(thread, call);
    }
    @Override public boolean glWaitFrame() { return mWinApk.glWaitFrame(); }
    @Override public boolean holderSizeIsBufferSize() { return mWinApk.holderSizeIsBufferSize(); }
    @Override public boolean isZOrderOnTop(SurfaceView view) { return mWinApk.isZOrderOnTop(view); }
    @Override public boolean rendersOnOsMain(SurfaceView view) { return mWinApk.rendersOnOsMain(view); }
    @Override public boolean forkGlQueueServiced(SurfaceView view) { return mWinApk.forkGlQueueServiced(view); }
    @Override public boolean rendererMayStart(SurfaceView view, int displayW, int displayH) {
        return mWinApk.rendererMayStart(view, displayW, displayH);
    }
    @Override public int[] rendererSurfaceSize(SurfaceView view, int displayW, int displayH) {
        return mWinApk.rendererSurfaceSize(view, displayW, displayH);
    }
    @Override public void warnGlSizeSplit(SurfaceView view, int toldW, int toldH, int displayW, int displayH) {
        mWinApk.warnGlSizeSplit(view, toldW, toldH, displayW, displayH);
    }
    @Override public int[] driveOverlayForCurrentSurface(SurfaceView view) {
        return mWinApk.driveOverlayForCurrentSurface(view);
    }
    @Override public void convergeSurfaceLifecycle(SurfaceView view, int widthPx, int heightPx) {
        mWinApk.convergeSurfaceLifecycle(view, widthPx, heightPx);
    }
    @Override public void detectDrawBehindTop(SurfaceView view, Object renderer) {
        mWinApk.detectDrawBehindTop(view, renderer);
    }
    @Override public void noteNativeDisplaySize(SurfaceView view, int w, int h) {
        mWinApk.noteNativeDisplaySize(view, w, h);
    }
    @Override public boolean ownerStoppedUnderTop(SurfaceView view) { return mWinApk.ownerStoppedUnderTop(view); }
}
