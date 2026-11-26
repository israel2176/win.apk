package android.window;
import android.content.Context;
import android.view.View;
import android.widget.FrameLayout;
/** The view of the system splash screen handed to the app so it can coordinate a
 *  custom exit animation. Mac.apk shows no separate splash window, so this is an
 *  empty container; remove() detaches it. API 31.
 *  Source: frameworks/base/core/java/android/window/SplashScreenView.java */
public final class SplashScreenView extends FrameLayout {
    private View mIconView;
    public SplashScreenView(Context context) { super(context); }
    public View getIconView() { return mIconView; }
    public java.time.Instant getIconAnimationStart() { return null; }
    public java.time.Duration getIconAnimationDuration() { return null; }
    public void remove() {
        android.view.ViewParent p = getParent();
        if (p instanceof android.view.ViewGroup) ((android.view.ViewGroup) p).removeView(this);
    }

    /** AOSP 14 SplashScreenView.SplashScreenViewParcelable: the system's splash screen view sent to the
     *  app's process (IActivityTaskManager, ClientTransactionHandler and ActivityThread name it, so
     *  reflection on them builds it). A minimal faithful stub: Win.apk's system shows no splash screen
     *  window, so none is ever made or sent; it carries the icon size and background colour. */
    public static class SplashScreenViewParcelable implements android.os.Parcelable {
        private int mIconSize;
        private int mBackgroundColor;
        private android.os.RemoteCallback mClientCallback;

        public SplashScreenViewParcelable(SplashScreenView view) {
            android.view.View icon = view.getIconView();
            mIconSize = icon != null ? icon.getWidth() : 0;
            android.graphics.drawable.Drawable bg = view.getBackground();
            mBackgroundColor = bg instanceof android.graphics.drawable.ColorDrawable
                    ? ((android.graphics.drawable.ColorDrawable) bg).getColor() : 0;
        }

        private SplashScreenViewParcelable(android.os.Parcel source) {
            mIconSize = source.readInt();
            mBackgroundColor = source.readInt();
        }

        @Override public int describeContents() { return 0; }

        @Override public void writeToParcel(android.os.Parcel dest, int flags) {
            dest.writeInt(mIconSize);
            dest.writeInt(mBackgroundColor);
        }

        public static final android.os.Parcelable.Creator<SplashScreenViewParcelable> CREATOR =
                new android.os.Parcelable.Creator<SplashScreenViewParcelable>() {
                    @Override public SplashScreenViewParcelable createFromParcel(android.os.Parcel source) {
                        return new SplashScreenViewParcelable(source);
                    }
                    @Override public SplashScreenViewParcelable[] newArray(int size) {
                        return new SplashScreenViewParcelable[size];
                    }
                };

        /** AOSP: recycles the bitmaps it copied; none here. */
        public void clearIfNeeded() { }

        int getIconSize() { return mIconSize; }

        int getBackgroundColor() { return mBackgroundColor; }

        public void setClientCallback(android.os.RemoteCallback clientCallback) { mClientCallback = clientCallback; }
    }
}
