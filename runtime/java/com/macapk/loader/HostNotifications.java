// com.macapk.loader.HostNotifications -- NotificationManager's host side: the
// notifications an app posts, shown by the host as its own (on Windows, toasts
// in the Action Center with the app's name and icon:
// windows/host/native_notifications.cpp). A host without the natives (the
// Mac) shows nothing, as before: every call is a no-op there.
//
// NotificationManager has already applied what the system service would: the
// POST_NOTIFICATIONS permission and the channel's importance. This class turns
// a Notification into what a host notification can show -- the title, the
// text as Android lays it out (the big text or the inbox lines when the
// expanded form has them, else the content text; the sub text as the smaller
// line), the large icon, a progress bar -- and keeps the posted ones by key, so
// a click can send the notification's contentIntent, as tapping it on a
// device does. Its actions are the host notification's buttons, at most three
// as in the shade, an action with a free-form RemoteInput with a text field:
// a click sends the action's intent, the typed text added as RemoteInput's
// results, as the shade's inline reply does.
package com.macapk.loader;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.RemoteInput;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class HostNotifications {
    private static final String TAG = "HostNotifications";

    private HostNotifications() {}

    // The host. `key` names one notification (NotificationManager's tag and
    // id); posting a key again replaces what it showed. `popup` is whether the
    // notification interrupts (a banner with a sound) or only joins the list.
    private static native boolean nativeInit();
    private static native boolean nativePost(String key, String title, String text, String subText,
            byte[] largeIconPng, int progressMax, int progress, boolean indeterminate, boolean popup,
            String[] actionTitles, String[] replyLabels);
    private static native void nativeCancel(String key);
    private static native void nativeCancelAll();

    private static int sAvailable = -1;  // -1 not asked yet, 0 no host notifications, 1 yes
    private static final Map<String, Notification> sPosted = new HashMap<>();

    private static synchronized boolean available() {
        if (sAvailable < 0) {
            boolean ok;
            try {
                ok = nativeInit();
            } catch (Throwable t) {  // no natives at all (the Mac)
                ok = false;
            }
            sAvailable = ok ? 1 : 0;
        }
        return sAvailable == 1;
    }

    private static String key(String tag, int id) { return (tag == null ? "" : tag) + "|" + id; }

    private static boolean empty(CharSequence s) { return s == null || s.length() == 0; }

    private static CharSequence lines(CharSequence[] lines) {
        if (lines == null || lines.length == 0) return null;
        StringBuilder sb = new StringBuilder();
        for (CharSequence l : lines) {
            if (empty(l)) continue;
            if (sb.length() > 0) sb.append('\n');
            sb.append(l);
        }
        return sb.length() > 0 ? sb : null;
    }

    private static byte[] largeIconPng(Notification n) {
        android.graphics.Bitmap b = n.largeIcon;
        if (b == null && n.largeIconObj != null) b = n.largeIconObj.getBitmap();  // null unless a bitmap icon
        if (b == null) return null;
        try {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            return b.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, out) ? out.toByteArray() : null;
        } catch (Throwable t) {
            return null;
        }
    }

    /** NotificationManager.notify, once the permission and the channel let it
     *  through; `importance` is the channel's (NotificationManager.IMPORTANCE_*,
     *  the priority's equivalent for a notification without a channel). */
    public static void post(String tag, int id, Notification n, int importance) {
        if (n == null || !available()) return;
        Bundle ex = n.getExtras();
        CharSequence title = ex.getCharSequence(Notification.EXTRA_TITLE_BIG);
        if (empty(title)) title = ex.getCharSequence(Notification.EXTRA_TITLE);
        CharSequence text = ex.getCharSequence(Notification.EXTRA_BIG_TEXT);
        if (empty(text)) text = lines(ex.getCharSequenceArray(Notification.EXTRA_TEXT_LINES));
        if (empty(text)) text = ex.getCharSequence(Notification.EXTRA_TEXT);
        if (empty(title) && empty(text)) text = n.tickerText;
        if (empty(title) && empty(text)) {
            NativeLog.w(TAG, "notify " + key(tag, id) + ": no title and no text, not shown");
            return;
        }
        CharSequence sub = ex.getCharSequence(Notification.EXTRA_SUB_TEXT);
        if (empty(sub)) sub = ex.getCharSequence(Notification.EXTRA_SUMMARY_TEXT);
        final int max = ex.getInt(Notification.EXTRA_PROGRESS_MAX, 0);
        final int progress = ex.getInt(Notification.EXTRA_PROGRESS, 0);
        final boolean indeterminate = ex.getBoolean(Notification.EXTRA_PROGRESS_INDETERMINATE, false);
        final String key = key(tag, id);
        final boolean update;
        synchronized (sPosted) {
            update = sPosted.put(key, n) != null;
        }
        // DEFAULT and above interrupt, as NotificationManagerService lets them;
        // a re-post with FLAG_ONLY_ALERT_ONCE (a progress bar moving) does not.
        boolean popup = importance >= NotificationManager.IMPORTANCE_DEFAULT
                && !(update && (n.flags & Notification.FLAG_ONLY_ALERT_ONCE) != 0);
        List<Notification.Action> shown = shownActions(n);
        String[] titles = new String[shown.size()];
        String[] replies = new String[shown.size()];
        for (int i = 0; i < titles.length; i++) {
            Notification.Action a = shown.get(i);
            titles[i] = a.title.toString();
            RemoteInput input = freeFormInput(a);
            if (input != null) replies[i] = input.getLabel() == null ? "" : input.getLabel().toString();
        }
        try {
            nativePost(key, title == null ? null : title.toString(), text == null ? null : text.toString(),
                    sub == null ? null : sub.toString(), largeIconPng(n), max, progress, indeterminate, popup,
                    titles, replies);
        } catch (Throwable t) {
            NativeLog.w(TAG, "notify " + key + " failed: " + t);
        }
    }

    /** The actions the shade shows as buttons: the notification's own (not the
     *  contextual ones a system service suggests, which AOSP lays out apart),
     *  those with a title and an intent, at most three. */
    static List<Notification.Action> shownActions(Notification n) {
        List<Notification.Action> out = new ArrayList<>();
        if (n.actions == null) return out;
        for (Notification.Action a : n.actions) {
            if (a == null || a.isContextual() || empty(a.title) || a.actionIntent == null) continue;
            out.add(a);
            if (out.size() == 3) break;
        }
        return out;
    }

    /** The action's first RemoteInput that takes typed text: the field the
     *  shade opens when the action is tapped. */
    static RemoteInput freeFormInput(Notification.Action a) {
        RemoteInput[] inputs = a.getRemoteInputs();
        if (inputs == null) return null;
        for (RemoteInput r : inputs) {
            if (r != null && r.getAllowFreeFormInput()) return r;
        }
        return null;
    }

    /** The fill-in intent an action is sent with: the reply under the
     *  RemoteInput's result key, as RemoteInput.addResultsToIntent puts it,
     *  so the app's RemoteInput.getResultsFromIntent finds it. Null when
     *  there is nothing to add. */
    static Intent replyIntent(Notification.Action a, String reply) {
        RemoteInput input = freeFormInput(a);
        if (input == null || reply == null) return null;
        Intent fill = new Intent();
        Bundle results = new Bundle();
        results.putCharSequence(input.getResultKey(), reply);
        RemoteInput.addResultsToIntent(a.getRemoteInputs(), fill, results);
        return fill;
    }

    public static void cancel(String tag, int id) {
        final String key = key(tag, id);
        synchronized (sPosted) {
            sPosted.remove(key);
        }
        // Also for a key this process never posted: the host keeps a previous
        // run's notifications, as a device keeps them when the process ends.
        if (available()) nativeCancel(key);
    }

    public static void cancelAll() {
        synchronized (sPosted) {
            sPosted.clear();
        }
        if (available()) nativeCancelAll();
    }

    /** The host: the user clicked the notification `key` (on a host thread).
     *  What a tap does on a device: the contentIntent is sent and an
     *  auto-cancel notification goes away. The host has already brought the
     *  app's window to the front. */
    public static void onActivated(final String key) {
        final Notification n;
        synchronized (sPosted) {
            n = sPosted.get(key);
        }
        if (n == null) return;  // a previous run's, or already cancelled
        Runnable tap = () -> {
            if ((n.flags & Notification.FLAG_AUTO_CANCEL) != 0) {
                synchronized (sPosted) {
                    if (sPosted.get(key) == n) sPosted.remove(key);
                }
            }
            PendingIntent pi = n.contentIntent;
            if (pi == null) return;
            try {
                pi.send();
            } catch (Throwable t) {
                NativeLog.w(TAG, "contentIntent of " + key + ": " + t);
            }
        };
        Looper main = Looper.getMainLooper();
        if (main != null) new Handler(main).post(tap);
        else tap.run();
    }

    /** The host: the user clicked action `index` of the notification `key`
     *  (its index among shownActions), `reply` the text typed into its field
     *  (null without one). What tapping an action does on a device: its intent
     *  is sent, with the reply; the notification stays, for the app to update
     *  or cancel. */
    public static void onAction(final String key, final int index, final String reply) {
        final Notification n;
        synchronized (sPosted) {
            n = sPosted.get(key);
        }
        if (n == null) return;
        List<Notification.Action> shown = shownActions(n);
        if (index < 0 || index >= shown.size()) return;
        final Notification.Action a = shown.get(index);
        Runnable tap = () -> {
            try {
                Intent fill = replyIntent(a, reply);
                if (fill != null) {
                    a.actionIntent.send(com.macapk.loader.app.Apps.current(), 0, fill);
                } else {
                    a.actionIntent.send();
                }
            } catch (Throwable t) {
                NativeLog.w(TAG, "action " + index + " of " + key + ": " + t);
            }
        };
        Looper main = Looper.getMainLooper();
        if (main != null) new Handler(main).post(tap);
        else tap.run();
    }
}
