// INotificationManager's toast half, the part AOSP's android.widget.Toast calls (docs/research/
// aosp-views.md, "V1 as built"). On Android, NotificationManagerService queues an app's toasts one at
// a time and SystemUI's ToastUI shows a text toast with ToastPresenter (a TYPE_TOAST window through
// the window manager); a custom-view toast is shown by the app's own TN when the service tells it
// to. Here both run in the process, with AOSP's ToastPresenter and the same defaults: gravity
// R.integer.config_toastDefaultGravity, y offset R.dimen.toast_y_offset, 2000 ms for LENGTH_SHORT
// and 3500 ms for LENGTH_LONG (NotificationManagerService's SHORT_DELAY and LONG_DELAY).
// Notifications themselves stay Win.apk's (android.app.NotificationManager, Windows toasts).
package com.winapk.aospviews;

import android.app.ITransientNotification;
import android.app.ITransientNotificationCallback;
import android.content.Context;
import android.os.Binder;
import android.os.IBinder;
import android.view.View;
import android.widget.Toast;
import android.widget.ToastPresenter;

import java.util.ArrayDeque;

public final class ToastService {
    static final int SHORT_DELAY = 2000, LONG_DELAY = 3500;

    private static final class Record {
        final String pkg;
        final IBinder token;
        final CharSequence text;
        final ITransientNotification tn;
        final ITransientNotificationCallback callback;
        final int duration;
        ToastPresenter presenter;

        Record(String pkg, IBinder token, CharSequence text, ITransientNotification tn,
                ITransientNotificationCallback callback, int duration) {
            this.pkg = pkg;
            this.token = token;
            this.text = text;
            this.tn = tn;
            this.callback = callback;
            this.duration = duration;
        }
    }

    private final ArrayDeque<Record> mQueue = new ArrayDeque<>();
    private Record mShowing;
    private final Runnable mTimeout = this::hideShowing;

    public void enqueueTextToast(String pkg, IBinder token, CharSequence text, int duration, boolean isUiContext,
            int displayId, ITransientNotificationCallback callback) {
        enqueue(new Record(pkg, token, text, null, callback, duration));
    }

    public void enqueueToast(String pkg, IBinder token, ITransientNotification callback, int duration,
            boolean isUiContext, int displayId) {
        enqueue(new Record(pkg, token, null, callback, null, duration));
    }

    public void cancelToast(String pkg, IBinder token) {
        AospWindows.main().post(() -> {
            mQueue.removeIf(r -> r.token == token);
            if (mShowing != null && mShowing.token == token) hideShowing();
        });
    }

    public void finishToken(String pkg, IBinder token) {}

    private void enqueue(Record r) {
        AospWindows.main().post(() -> {
            mQueue.add(r);
            if (mShowing == null) showNext();
        });
    }

    private void showNext() {
        Record r = mQueue.poll();
        if (r == null) return;
        mShowing = r;
        try {
            if (r.tn != null) {
                r.tn.show(new Binder());
            } else {
                Context c = AospWindows.context();
                r.presenter = new ToastPresenter(c, null, AospWindows.sNotifications, r.pkg);
                View view = ToastPresenter.getTextToastView(c, r.text);
                r.presenter.show(view, r.token, new Binder(), r.duration,
                        c.getResources().getInteger(com.android.internal.R.integer.config_toastDefaultGravity), 0,
                        c.getResources().getDimensionPixelSize(com.android.internal.R.dimen.toast_y_offset), 0, 0,
                        r.callback);
            }
        } catch (Throwable t) {
            System.err.println(AospWindows.TAG + "toast not shown: " + t);
            mShowing = null;
            showNext();
            return;
        }
        AospWindows.main().postDelayed(mTimeout, r.duration == Toast.LENGTH_LONG ? LONG_DELAY : SHORT_DELAY);
    }

    private void hideShowing() {
        AospWindows.main().removeCallbacks(mTimeout);
        Record r = mShowing;
        mShowing = null;
        if (r != null) {
            try {
                if (r.tn != null) r.tn.hide();
                else if (r.presenter != null) r.presenter.hide(r.callback);
            } catch (Throwable t) {
                System.err.println(AospWindows.TAG + "toast not hidden: " + t);
            }
        }
        showNext();
    }
}
