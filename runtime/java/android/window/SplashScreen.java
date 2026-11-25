package android.window;
/** Controls the system splash screen for an activity. Mac.apk shows no separate
 *  splash window, so exit-animation listeners never fire and the theme override
 *  is inert. Source: frameworks/base/core/java/android/window/SplashScreen.java */
public interface SplashScreen {
    interface OnExitAnimationListener {
        void onSplashScreenExit(android.window.SplashScreenView view);
    }
    default void setOnExitAnimationListener(OnExitAnimationListener listener) { }
    default void clearOnExitAnimationListener() { }
    default void setSplashScreenTheme(int themeId) { }

    /** AOSP's implementation, which Activity.getSplashScreen() makes for each activity
     *  (AOSP 14 Activity.getOrCreateSplashScreen: `new SplashScreen.SplashScreenImpl(this)`).
     *  On a device its listener is handed the system's splash view once the app's first
     *  frame is up; the launcher's starting window here is not one the app can take
     *  over, so the listener is kept and never called, as on a device when the system
     *  shows no splash (a start that is not from the launcher). */
    class SplashScreenImpl implements SplashScreen {
        private OnExitAnimationListener mExitAnimationListener;

        public SplashScreenImpl(android.content.Context context) { }

        @Override public void setOnExitAnimationListener(OnExitAnimationListener listener) {
            synchronized (this) { mExitAnimationListener = listener; }
        }

        @Override public void clearOnExitAnimationListener() {
            synchronized (this) { mExitAnimationListener = null; }
        }
    }
}
