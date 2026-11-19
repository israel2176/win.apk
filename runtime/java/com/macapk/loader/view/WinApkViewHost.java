// WinApkViewHost -- ViewHost on Win.apk's own android.view / android.widget:
// each method calls the member it stands for, on the same receiver with the same
// arguments, and nothing else.
package com.macapk.loader.view;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.SurfaceTexture;
import android.view.Choreographer;
import android.view.Display;
import android.view.InputDevice;
import android.view.InputQueue;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.MacApkDragSession;
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
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.CaptioningManager;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.view.textclassifier.TextClassificationManager;
import android.view.textservice.TextServicesManager;
import android.widget.TextView;

import com.android.internal.policy.DecorView;
import com.macapk.loader.MetalSurfaceCallbacks;

import java.util.List;

final class WinApkViewHost implements ViewHost {

    static final WinApkViewHost INSTANCE = new WinApkViewHost();

    private WinApkViewHost() {}

    // ---- A window's root ----

    @Override public void dispatchAttachedToWindow(View root) { root.macapkDispatchAttachedToWindow(); }

    @Override public void dispatchDetachedFromWindow(View root) { root.macapkDispatchDetachedFromWindow(); }

    @Override public boolean isWindowRemoved(View root) { return root.macapkIsWindowRemoved(); }

    @Override public void clearWindowRemoved(View root) { root.macapkClearWindowRemoved(); }

    @Override public void removeWindow(View root) { root.macapkRemoveWindow(); }

    @Override public void forgetViewContext(Context context) { View.macapkForgetContext(context); }

    @Override public void forgetInflaterContext(Context context) { LayoutInflater.macapkForgetContext(context); }

    @Override public void windowAdded(View root, WindowManager.LayoutParams params) {
        WindowManagerGlobal.getInstance().macapkWindowAdded(root, params);
    }

    @Override public void windowRemoved(View root) { WindowManagerGlobal.getInstance().macapkWindowRemoved(root); }

    @Override public void activityWindowAdded(Activity activity) {
        WindowManagerGlobal.getInstance().macapkActivityWindowAdded(activity);
    }

    @Override public void activityWindowRemoved(Activity activity) {
        WindowManagerGlobal.getInstance().macapkActivityWindowRemoved(activity);
    }

    @Override public void onWindowAddedForInput(Window window) { window.macapkOnWindowAddedForInput(); }

    @Override public DecorView newDecorView(Context context, int featureId, Window window,
            WindowManager.LayoutParams params) {
        return new DecorView(context, featureId, window, params);
    }

    // ---- The frame ----

    @Override public ViewRootImpl viewRoot() { return ViewRootImpl.getInstance(); }

    @Override public void scheduleTraversal() { ViewRootImpl.schedule(); }

    @Override public void scheduleLayout() { ViewRootImpl.scheduleLayout(); }

    @Override public boolean consumeTraversalScheduled(ViewRootImpl root) { return root.consumeTraversalScheduled(); }

    @Override public boolean consumeLayoutRequested(ViewRootImpl root) { return root.consumeLayoutRequested(); }

    @Override public boolean damageGate(String env) { return ViewRootImpl.macapkDamageGate(env); }

    @Override public int consumeDamage(ViewRootImpl root, int[] out4) { return root.macapkConsumeDamage(out4); }

    @Override public void beginDraw(View root, Canvas canvas, boolean partial, int kind, int[] rect) {
        ViewRootImpl.macapkBeginDraw(root, canvas, partial, kind, rect);
    }

    @Override public void endDraw() { ViewRootImpl.macapkEndDraw(); }

    @Override public boolean endFrameDamage(ViewRootImpl root, int[] drawn) { return root.macapkEndFrameDamage(drawn); }

    @Override public void scheduleSameDamage(ViewRootImpl root) { root.macapkScheduleSameDamage(); }

    @Override public void driveMainFrame() { Choreographer.macapkDriveMainFrame(); }

    @Override public ViewTreeObserver peekTreeObserver(View view) { return view.macapkPeekTreeObserver(); }

    @Override public boolean hasLayoutOrDrawListeners(ViewTreeObserver observer) {
        return observer.macapkHasLayoutOrDrawListeners();
    }

    @Override public boolean consumeInsetsDispatchPending(View root) {
        return root.macapkConsumeInsetsDispatchPending();
    }

    @Override public int backgroundColor(View view) { return view.getBackgroundColor(); }

    @Override public int layoutGravity(ViewGroup.LayoutParams params) { return params.macapkGravity; }

    @Override public long nativeHandle(View view) { return view.getNativeHandle(); }

    // ---- Input ----

    @Override public boolean hoverViews() { return View.HOVER_VIEWS; }

    @Override public void setKeySource(KeyEvent event, int source) { event.macapkSetSource(source); }

    @Override public void setKeyUnicodeChar(KeyEvent event, int unicodeChar) {
        event.macapkSetUnicodeChar(unicodeChar);
    }

    @Override public boolean inTouchMode() { return ViewRootImpl.macapkInTouchMode(); }

    @Override public boolean ensureTouchMode(View root, boolean inTouchMode) {
        return ViewRootImpl.macapkEnsureTouchMode(root, inTouchMode);
    }

    @Override public View.OnTouchListener onTouchListener(View view) { return view.getOnTouchListener(); }

    @Override public boolean hasOnTouchListener(View view) { return view.macapkHasOnTouchListener(); }

    @Override public boolean hasOnDragListener(View view) { return view.macapkHasOnDragListener(); }

    @Override public InputQueue takenInputQueue(Window window) { return window.macapkTakenInputQueue(); }

    @Override public InputDevice newInputDevice(int id, int generation, int controllerNumber, String name,
            int vendorId, int productId, String descriptor, boolean isExternal, int sources, int keyboardType,
            KeyCharacterMap keyCharacterMap, boolean hasVibrator, boolean hasMicrophone,
            boolean hasButtonUnderPad) {
        return new InputDevice(id, generation, controllerNumber, name, vendorId, productId, descriptor,
                isExternal, sources, keyboardType, keyCharacterMap, hasVibrator, hasMicrophone,
                hasButtonUnderPad);
    }

    // ---- Drag and drop ----

    @Override public boolean dragActive() { return MacApkDragSession.isActive(); }

    @Override public void dragAbort() { MacApkDragSession.abort(); }

    @Override public boolean dragTakeSourceCancel() { return MacApkDragSession.takeSourceCancel(); }

    @Override public View dragActiveRoot() { return MacApkDragSession.activeRoot(); }

    @Override public void dragPointer(int action, float x, float y) { MacApkDragSession.onPointer(action, x, y); }

    @Override public void drawDragShadow(Canvas canvas, View root) { MacApkDragSession.drawShadow(canvas, root); }

    // ---- The IME's target ----

    @Override public TextView imeActiveInput() { return InputMethodManager.macapkActiveInput(); }

    @Override public InputConnection imeActiveConnection() { return InputMethodManager.macapkActiveConnection(); }

    @Override public TextView imeShowTarget() { return InputMethodManager.macapkShowTarget(); }

    @Override public String imeTraceFocused() { return InputMethodManager.macapkTraceFocused(); }

    @Override public int imeActionForEnter() { return InputMethodManager.macapkImeActionForEnter(); }

    @Override public boolean imePasswordTargetActive() { return InputMethodManager.macapkPasswordTargetActive(); }

    @Override public boolean handleSoftKey(TextView view, int unicodeChar, int keyCode) {
        return view.macapkHandleSoftKey(unicodeChar, keyCode);
    }

    @Override public boolean hasPasswordTransformationMethod(TextView view) {
        return view.macapkHasPasswordTransformationMethod();
    }

    // ---- The display and the system bars ----

    @Override public Display newDisplay() { return new Display(); }

    @Override public int realWidthPx() { return Display.macapkRealWidthPx(); }

    @Override public int realHeightPx() { return Display.macapkRealHeightPx(); }

    @Override public int navDecorWidthPx() { return Display.macapkNavDecorWidthPx(); }

    @Override public int navDecorHeightPx() { return Display.macapkNavDecorHeightPx(); }

    @Override public int navDecorPx() { return Display.macapkNavDecorPx(); }

    @Override public int statusDecorPx() { return Display.macapkStatusDecorPx(); }

    @Override public boolean fullscreen() { return Window.macapkFullscreen(); }

    @Override public boolean barHidden(int type) { return Window.macapkBarHidden(type); }

    @Override public boolean decorFitsSystemWindows() { return Window.macapkDecorFitsSystemWindows(); }

    @Override public int barAppearance(int bit) { return Window.macapkBarAppearance(bit); }

    @Override public int forcedStatusBarColor() { return Window.macapkForcedStatusBarColor(); }

    @Override public int forcedNavigationBarColor() { return Window.macapkForcedNavigationBarColor(); }

    @Override public int barColorUnset() { return Window.MACAPK_BAR_COLOR_UNSET; }

    // ---- The view tree's owners, Compose ----

    @Override public String[][] viewTreeOwners() { return View.MACAPK_VIEW_TREE_OWNERS; }

    @Override public void registerOwnerKey(int key) { View.macapkRegisterOwnerKey(key); }

    @Override public List<Object> keyedTagValues(View view) { return view.macapkKeyedTagValues(); }

    @Override public void retryComposeAttaches() { View.macapkRetryComposeAttaches(); }

    // ---- System services ----

    @Override public AccessibilityManager newAccessibilityManager() { return new AccessibilityManager(); }

    @Override public CaptioningManager newCaptioningManager() { return new CaptioningManager(); }

    @Override public TextServicesManager newTextServicesManager(Context context) {
        return new TextServicesManager(context);
    }

    @Override public TextClassificationManager newTextClassificationManager() {
        return new TextClassificationManager();
    }

    // ---- Chrome ----

    @Override public void setFontFamily(TextView view, String family) { view.setMacapkFontFamily(family); }

    @Override public void setTextStyle(TextView view, int style) { view.setMacapkTextStyle(style); }

    @Override public SurfaceHolder.Callback2 takenSurface(Window window) { return window.macapkTakenSurface(); }

    // ---- Surface and SurfaceView ----

    @Override public SurfaceTexture surfaceTexture(Surface surface) { return surface.macapkGetSurfaceTexture(); }

    @Override public SurfaceView ownerSurfaceView(Surface surface) { return surface.macapkGetOwnerSurfaceView(); }

    @Override public void noteNativeWindowConsumer(Surface surface) { surface.macapkNoteNativeWindowConsumer(); }

    @Override public void setFrameSink(Surface surface, FrameSink sink, int width, int height) {
        // Surface keeps its own sink type; only its null-ness and onFrame are used.
        surface.macapkSetFrameSink(sink == null ? null : sink::onFrame, width, height);
    }

    @Override public void setOverlayDriver(MetalSurfaceCallbacks driver, int widthPx, int heightPx) {
        SurfaceView.macapkSetOverlayDriver(driver, widthPx, heightPx);
    }

    @Override public long activeVideoRectPacked() { return SurfaceView.macapkActiveVideoRectPacked(); }

    @Override public void registerVideoSink(Surface surface) { SurfaceView.macapkRegisterVideoSink(surface); }

    @Override public void unregisterVideoSink(Surface surface) { SurfaceView.macapkUnregisterVideoSink(surface); }

    @Override public boolean presentVideoFrame(Surface surface, int[] argb, int width, int height) {
        return SurfaceView.macapkPresentVideoFrame(surface, argb, width, height);
    }

    @Override public boolean isSurfaceViewGlThread(Thread thread) {
        return SurfaceView.macapkIsSurfaceViewGlThread(thread);
    }

    @Override public void markGlThreadSurvivedRefusal(Thread thread, String call) {
        SurfaceView.macapkMarkGlThreadSurvivedRefusal(thread, call);
    }

    @Override public boolean glWaitFrame() { return SurfaceView.macapkGlWaitFrame(); }

    @Override public boolean holderSizeIsBufferSize() { return SurfaceView.macapkHolderSizeIsBufferSize(); }

    @Override public boolean isZOrderOnTop(SurfaceView view) { return view.macapkIsZOrderOnTop(); }

    @Override public boolean rendersOnOsMain(SurfaceView view) { return view.macapkRendersOnOsMain(); }

    @Override public boolean forkGlQueueServiced(SurfaceView view) { return view.macapkForkGlQueueServiced(); }

    @Override public boolean rendererMayStart(SurfaceView view, int displayW, int displayH) {
        return view.macapkRendererMayStart(displayW, displayH);
    }

    @Override public int[] rendererSurfaceSize(SurfaceView view, int displayW, int displayH) {
        return view.macapkRendererSurfaceSize(displayW, displayH);
    }

    @Override public void warnGlSizeSplit(SurfaceView view, int toldW, int toldH, int displayW, int displayH) {
        view.macapkWarnGlSizeSplit(toldW, toldH, displayW, displayH);
    }

    @Override public int[] driveOverlayForCurrentSurface(SurfaceView view) {
        return view.macapkDriveOverlayForCurrentSurface();
    }

    @Override public void convergeSurfaceLifecycle(SurfaceView view, int widthPx, int heightPx) {
        view.macapkConvergeSurfaceLifecycle(widthPx, heightPx);
    }

    @Override public void detectDrawBehindTop(SurfaceView view, Object renderer) {
        view.macapkDetectDrawBehindTop(renderer);
    }

    @Override public void noteNativeDisplaySize(SurfaceView view, int w, int h) {
        view.macapkNoteNativeDisplaySize(w, h);
    }

    @Override public boolean ownerStoppedUnderTop(SurfaceView view) { return view.macapkOwnerStoppedUnderTop(); }
}
