// ViewHost -- the one door from the rest of the runtime into Win.apk's own
// android.view / android.widget / com.android.internal.policy plumbing
// (docs/research/aosp-views.md, step 0; windows/README.md, "Changes to the
// upstream tree").
//
// Win.apk's view classes declare macapk* methods and fields, and members and
// classes AOSP's do not have (ViewRootImpl.getInstance, MacApkDragSession, a
// no-argument Display...): attaching a window's root, the frame and its damage,
// touch mode and hover, drag, the IME's active connection, the system bars,
// SurfaceView's GL and video plumbing. Code outside those packages (the loader,
// android.app, android.opengl, android.media...) calls them only through this
// interface, so that running AOSP's own views later needs an implementation of
// it, not patched copies of AOSP classes. Today the only implementation is
// WinApkViewHost, which calls the member each method stands for, one for one,
// with the same receiver and arguments: going through here changes nothing.
//
// Inside those packages the classes keep calling each other directly. A Win.apk
// subclass outside them that overrides a hook (GLSurfaceView's macapkDrawFrame,
// Dialog's macapkOwnInflater) still overrides it, and its super. calls stay.
package com.macapk.loader.view;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.SurfaceTexture;
import android.view.Display;
import android.view.InputDevice;
import android.view.InputQueue;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewRootImpl;
import android.view.ViewTreeObserver;
import android.view.Window;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.CaptioningManager;
import android.view.inputmethod.InputConnection;
import android.view.textclassifier.TextClassificationManager;
import android.view.textservice.TextServicesManager;
import android.widget.TextView;

import com.android.internal.policy.DecorView;
import com.macapk.loader.MetalSurfaceCallbacks;

import java.util.List;

public interface ViewHost {

    /** The view host of this JVM: chosen once, for the whole run. */
    static ViewHost get() { return WinApkViewHost.INSTANCE; }

    // ---- A window's root: attach, detach, remove ----

    /** Runs the attach pass over the tree (View.macapkDispatchAttachedToWindow). */
    void dispatchAttachedToWindow(View root);

    /** Runs the detach pass over the tree (View.macapkDispatchDetachedFromWindow). */
    void dispatchDetachedFromWindow(View root);

    /** True once the root's window was removed (View.macapkIsWindowRemoved). */
    boolean isWindowRemoved(View root);

    /** A new window for this root (View.macapkClearWindowRemoved). */
    void clearWindowRemoved(View root);

    /** Marks the root's window removed and detaches it (View.macapkRemoveWindow). */
    void removeWindow(View root);

    /** Drops the views' static references to a finished context (View.macapkForgetContext). */
    void forgetViewContext(Context context);

    /** Drops the inflaters' static references to a finished context (LayoutInflater.macapkForgetContext). */
    void forgetInflaterContext(Context context);

    /** Records a window root (WindowManagerGlobal.getInstance().macapkWindowAdded). */
    void windowAdded(View root, WindowManager.LayoutParams params);

    /** Forgets a window root (WindowManagerGlobal.getInstance().macapkWindowRemoved). */
    void windowRemoved(View root);

    /** Records an activity's window (WindowManagerGlobal.getInstance().macapkActivityWindowAdded). */
    void activityWindowAdded(Activity activity);

    /** Forgets an activity's window (WindowManagerGlobal.getInstance().macapkActivityWindowRemoved). */
    void activityWindowRemoved(Activity activity);

    /** Hands a taken input queue to its callback once the window is up (Window.macapkOnWindowAddedForInput). */
    void onWindowAddedForInput(Window window);

    /** A window's DecorView (Win.apk's constructor, over any Window). */
    DecorView newDecorView(Context context, int featureId, Window window, WindowManager.LayoutParams params);

    // ---- The frame: traversals, damage, the tree's state ----

    /** The process's one view root (ViewRootImpl.getInstance). */
    ViewRootImpl viewRoot();

    /** Schedules a traversal (ViewRootImpl.schedule). */
    void scheduleTraversal();

    /** Schedules a layout traversal (ViewRootImpl.scheduleLayout). */
    void scheduleLayout();

    /** Takes the traversal-scheduled flag (ViewRootImpl.consumeTraversalScheduled). */
    boolean consumeTraversalScheduled(ViewRootImpl root);

    /** Takes the layout-requested flag (ViewRootImpl.consumeLayoutRequested). */
    boolean consumeLayoutRequested(ViewRootImpl root);

    /** Whether partial-frame damage is on for this environment value (ViewRootImpl.macapkDamageGate). */
    boolean damageGate(String env);

    /** The damage since the last call, DAMAGE_* of ViewCanvasDriver (ViewRootImpl.macapkConsumeDamage). */
    int consumeDamage(ViewRootImpl root, int[] out4);

    /** Brackets the root's draw: start (ViewRootImpl.macapkBeginDraw). */
    void beginDraw(View root, Canvas canvas, boolean partial, int kind, int[] rect);

    /** Brackets the root's draw: end (ViewRootImpl.macapkEndDraw). */
    void endDraw();

    /** Keeps the damage this frame's traversal raised for the next frame (ViewRootImpl.macapkEndFrameDamage). */
    boolean endFrameDamage(ViewRootImpl root, int[] drawn);

    /** Asks for another frame with the same damage (ViewRootImpl.macapkScheduleSameDamage). */
    void scheduleSameDamage(ViewRootImpl root);

    /** Runs the main thread's Choreographer frame (Choreographer.macapkDriveMainFrame). */
    void driveMainFrame();

    /** The view's tree observer, without creating one (View.macapkPeekTreeObserver). */
    ViewTreeObserver peekTreeObserver(View view);

    /** True when the observer has layout or draw listeners (ViewTreeObserver.macapkHasLayoutOrDrawListeners). */
    boolean hasLayoutOrDrawListeners(ViewTreeObserver observer);

    /** Takes the root's pending insets dispatch (View.macapkConsumeInsetsDispatchPending). */
    boolean consumeInsetsDispatchPending(View root);

    /** The view's background colour (View.getBackgroundColor). */
    int backgroundColor(View view);

    /** The layout_gravity read from the XML (ViewGroup.LayoutParams.macapkGravity). */
    int layoutGravity(ViewGroup.LayoutParams params);

    /** A gravity attribute's text ("bottom|end") as its flags; NO_GRAVITY when none (Gravity.parse). */
    int parseGravity(String text);

    /** The view's native handle, 0 when none (View.getNativeHandle). */
    long nativeHandle(View view);

    // ---- Input: keys, touch mode, hover, listeners ----

    /** Whether hover events go to views (View.HOVER_VIEWS). */
    boolean hoverViews();

    /** Sets a key event's source (KeyEvent.macapkSetSource). */
    void setKeySource(KeyEvent event, int source);

    /** Sets a key event's character (KeyEvent.macapkSetUnicodeChar). */
    void setKeyUnicodeChar(KeyEvent event, int unicodeChar);

    /** The window's touch mode (ViewRootImpl.macapkInTouchMode). */
    boolean inTouchMode();

    /** Enters or leaves touch mode over a root; true when it changed (ViewRootImpl.macapkEnsureTouchMode). */
    boolean ensureTouchMode(View root, boolean inTouchMode);

    /** The view's touch listener (View.getOnTouchListener). */
    View.OnTouchListener onTouchListener(View view);

    /** True when the view has a touch listener (View.macapkHasOnTouchListener). */
    boolean hasOnTouchListener(View view);

    /** True when the view has a drag listener (View.macapkHasOnDragListener). */
    boolean hasOnDragListener(View view);

    /** The input queue a NativeActivity took (Window.macapkTakenInputQueue). */
    InputQueue takenInputQueue(Window window);

    /** An input device (Win.apk's constructor). */
    InputDevice newInputDevice(int id, int generation, int controllerNumber, String name, int vendorId,
            int productId, String descriptor, boolean isExternal, int sources, int keyboardType,
            KeyCharacterMap keyCharacterMap, boolean hasVibrator, boolean hasMicrophone,
            boolean hasButtonUnderPad);

    // ---- Drag and drop (MacApkDragSession) ----

    /** True while a drag is under way (MacApkDragSession.isActive). */
    boolean dragActive();

    /** Ends the drag with no drop (MacApkDragSession.abort). */
    void dragAbort();

    /** Takes the source's pending cancel (MacApkDragSession.takeSourceCancel). */
    boolean dragTakeSourceCancel();

    /** The root the drag runs in (MacApkDragSession.activeRoot). */
    View dragActiveRoot();

    /** A pointer event of the drag (MacApkDragSession.onPointer). */
    void dragPointer(int action, float x, float y);

    /** Draws the drag shadow over the frame (MacApkDragSession.drawShadow). */
    void drawDragShadow(Canvas canvas, View root);

    // ---- The IME's target ----

    /** The text view the IME serves, or null (InputMethodManager.macapkActiveInput). */
    TextView imeActiveInput();

    /** The active input connection, or null (InputMethodManager.macapkActiveConnection). */
    InputConnection imeActiveConnection();

    /** The text view the soft keyboard was asked for (InputMethodManager.macapkShowTarget). */
    TextView imeShowTarget();

    /** A trace line about the focused view (InputMethodManager.macapkTraceFocused). */
    String imeTraceFocused();

    /** The editor action Enter runs (InputMethodManager.macapkImeActionForEnter). */
    int imeActionForEnter();

    /** True when the IME's target is a password field (InputMethodManager.macapkPasswordTargetActive). */
    boolean imePasswordTargetActive();

    /** A typed key handled by the text view itself; true when consumed (TextView.macapkHandleSoftKey). */
    boolean handleSoftKey(TextView view, int unicodeChar, int keyCode);

    /** True when the text view hides its text (TextView.macapkHasPasswordTransformationMethod). */
    boolean hasPasswordTransformationMethod(TextView view);

    // ---- The display and the system bars ----

    /** A Display (Win.apk's constructor). */
    Display newDisplay();

    /** The screen's width in pixels (Display.macapkRealWidthPx). */
    int realWidthPx();

    /** The screen's height in pixels (Display.macapkRealHeightPx). */
    int realHeightPx();

    /** The navigation bar's width when it is on a side (Display.macapkNavDecorWidthPx). */
    int navDecorWidthPx();

    /** The navigation bar's height when it is at the bottom (Display.macapkNavDecorHeightPx). */
    int navDecorHeightPx();

    /** The navigation bar's thickness (Display.macapkNavDecorPx). */
    int navDecorPx();

    /** The status bar's height (Display.macapkStatusDecorPx). */
    int statusDecorPx();

    /** True when the app asked for full screen (Window.macapkFullscreen). */
    boolean fullscreen();

    /** True when the app hid these bar types (Window.macapkBarHidden). */
    boolean barHidden(int type);

    /** True while the app's content fits inside the bars (Window.macapkDecorFitsSystemWindows). */
    boolean decorFitsSystemWindows();

    /** The app's bar-appearance bit, or its unset value (Window.macapkBarAppearance). */
    int barAppearance(int bit);

    /** The status bar colour the app set, or barColorUnset() (Window.macapkForcedStatusBarColor). */
    int forcedStatusBarColor();

    /** The navigation bar colour the app set, or barColorUnset() (Window.macapkForcedNavigationBarColor). */
    int forcedNavigationBarColor();

    /** The value of a bar colour the app did not set (Window.MACAPK_BAR_COLOR_UNSET). */
    int barColorUnset();

    // ---- The view tree's owners (lifecycle, saved state), Compose ----

    /** The owner tag families, {class, field...} (View.MACAPK_VIEW_TREE_OWNERS). */
    String[][] viewTreeOwners();

    /** Registers an owner tag key (View.macapkRegisterOwnerKey). */
    void registerOwnerKey(int key);

    /** The values of the view's keyed tags (View.macapkKeyedTagValues). */
    List<Object> keyedTagValues(View view);

    /** Retries Compose attaches that found no owners (View.macapkRetryComposeAttaches). */
    void retryComposeAttaches();

    // ---- System services Win.apk makes in process ----

    /** Win.apk's constructor. */
    AccessibilityManager newAccessibilityManager();

    /** Win.apk's constructor. */
    CaptioningManager newCaptioningManager();

    /** Win.apk's constructor. */
    TextServicesManager newTextServicesManager(Context context);

    /** Win.apk's constructor. */
    TextClassificationManager newTextClassificationManager();

    // ---- Chrome Win.apk draws: dialogs, NativeActivity ----

    /** A font family by name (TextView.setMacapkFontFamily). */
    void setFontFamily(TextView view, String family);

    /** A typeface style (TextView.setMacapkTextStyle). */
    void setTextStyle(TextView view, int style);

    /** The surface callback a NativeActivity took (Window.macapkTakenSurface). */
    SurfaceHolder.Callback2 takenSurface(Window window);

    // ---- Surface and SurfaceView: GL, video, encoders ----

    /** Receives a Surface's frames: what an encoder's input surface draws. */
    interface FrameSink {
        void onFrame(int[] argb, int width, int height, long timestampNs);
    }

    /** The surface's SurfaceTexture, or null (Surface.macapkGetSurfaceTexture). */
    SurfaceTexture surfaceTexture(Surface surface);

    /** The SurfaceView the surface belongs to, or null (Surface.macapkGetOwnerSurfaceView). */
    SurfaceView ownerSurfaceView(Surface surface);

    /** Notes that native code took the surface as a window (Surface.macapkNoteNativeWindowConsumer). */
    void noteNativeWindowConsumer(Surface surface);

    /** Sends the surface's frames to a sink, null to stop (Surface.macapkSetFrameSink). */
    void setFrameSink(Surface surface, FrameSink sink, int width, int height);

    /** The window's GL overlay driver (SurfaceView.macapkSetOverlayDriver). */
    void setOverlayDriver(MetalSurfaceCallbacks driver, int widthPx, int heightPx);

    /** The video layer's rectangle, packed (SurfaceView.macapkActiveVideoRectPacked). */
    long activeVideoRectPacked();

    /** A decoder's output surface (SurfaceView.macapkRegisterVideoSink). */
    void registerVideoSink(Surface surface);

    /** (SurfaceView.macapkUnregisterVideoSink) */
    void unregisterVideoSink(Surface surface);

    /** Shows a video frame on the surface's SurfaceView; true when shown (SurfaceView.macapkPresentVideoFrame). */
    boolean presentVideoFrame(Surface surface, int[] argb, int width, int height);

    /** True for a SurfaceView's GL thread (SurfaceView.macapkIsSurfaceViewGlThread). */
    boolean isSurfaceViewGlThread(Thread thread);

    /** (SurfaceView.macapkMarkGlThreadSurvivedRefusal) */
    void markGlThreadSurvivedRefusal(Thread thread, String call);

    /** (SurfaceView.macapkGlWaitFrame) */
    boolean glWaitFrame();

    /** (SurfaceView.macapkHolderSizeIsBufferSize) */
    boolean holderSizeIsBufferSize();

    /** (SurfaceView.macapkIsZOrderOnTop) */
    boolean isZOrderOnTop(SurfaceView view);

    /** True when the view's renderer runs on the OS main thread (SurfaceView.macapkRendersOnOsMain, virtual). */
    boolean rendersOnOsMain(SurfaceView view);

    /** (SurfaceView.macapkForkGlQueueServiced) */
    boolean forkGlQueueServiced(SurfaceView view);

    /** (SurfaceView.macapkRendererMayStart) */
    boolean rendererMayStart(SurfaceView view, int displayW, int displayH);

    /** (SurfaceView.macapkRendererSurfaceSize) */
    int[] rendererSurfaceSize(SurfaceView view, int displayW, int displayH);

    /** (SurfaceView.macapkWarnGlSizeSplit) */
    void warnGlSizeSplit(SurfaceView view, int toldW, int toldH, int displayW, int displayH);

    /** (SurfaceView.macapkDriveOverlayForCurrentSurface) */
    int[] driveOverlayForCurrentSurface(SurfaceView view);

    /** (SurfaceView.macapkConvergeSurfaceLifecycle) */
    void convergeSurfaceLifecycle(SurfaceView view, int widthPx, int heightPx);

    /** (SurfaceView.macapkDetectDrawBehindTop) */
    void detectDrawBehindTop(SurfaceView view, Object renderer);

    /** (SurfaceView.macapkNoteNativeDisplaySize) */
    void noteNativeDisplaySize(SurfaceView view, int w, int h);

    /** (SurfaceView.macapkOwnerStoppedUnderTop) */
    boolean ownerStoppedUnderTop(SurfaceView view);
}
