// com.macapk.loader.app.IntentSenders -- the door from the app layer's activity manager (windows/java/aosp_app,
// compiled against AOSP's classes) into Win.apk's PendingIntent, which is the system's record of a sender
// here: what system_server's PendingIntentRecord answers on a device (startActivityIntentSender,
// getIntentSenderWithFeature). AOSP's PendingIntent has none of these members, so the app layer reaches
// them through this class, as it reaches the rest of Win.apk through com.macapk.*.
package com.macapk.loader.app;

import android.app.PendingIntent;
import android.content.Context;
import android.content.IIntentSender;
import android.content.Intent;

import java.util.function.BiConsumer;

public final class IntentSenders {
    private IntentSenders() {}

    /** The PendingIntent an IIntentSender of this process stands for, else null. */
    public static PendingIntent of(IIntentSender target) { return PendingIntent.macapkOf(target); }

    /** The sender's IIntentSender (what IntentSender.getTarget gives). */
    public static IIntentSender target(PendingIntent pi) { return pi.macapkTarget(); }

    public static boolean canceled(PendingIntent pi) { return pi.macapkCanceled(); }

    /** The activity a one-activity sender starts, fill-in and flags applied; null for any other kind. */
    public static Intent activityIntent(PendingIntent pi, Intent fillIn, int flagsMask, int flagsValues) {
        return pi.macapkActivityIntent(fillIn, flagsMask, flagsValues);
    }

    /** Sends any other kind; true when it was cancelled. */
    public static boolean sendCanceled(PendingIntent pi, int code, Intent fillIn) {
        return pi.macapkSend(code, fillIn) < 0;
    }

    /** Activity.createPendingResult's sender: sent, it gives `sink` the code and the intent. */
    public static PendingIntent activityResult(Context creator, int requestCode, Intent data, int flags,
            BiConsumer<Integer, Intent> sink) {
        return PendingIntent.macapkActivityResult(creator, requestCode, data, flags, sink);
    }

    /** MediaStore's create*Request consent screen (no system UI for it here). */
    public static boolean isMediaConsentRequest(Intent intent) {
        return android.provider.MediaStore.macapkIsConsentRequest(intent);
    }
}
