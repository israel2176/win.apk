// The system camera's capture screen: MediaStore.ACTION_IMAGE_CAPTURE (and
// ACTION_IMAGE_CAPTURE_SECURE) on a host that has one (Win.apk: a window on the
// PC's webcam, windows/host/native_camera.cpp).
//
// On a device these intents resolve to the camera app's capture activity
// (AOSP Camera2: com.android.camera2 / com.android.camera.CaptureActivity),
// which takes one picture and answers the caller:
//  - with EXTRA_OUTPUT (a content:// or file:// Uri the caller provides,
//    usually its own FileProvider's, with a write grant), the full-size JPEG is
//    written there and the result is RESULT_OK with an empty Intent;
//  - without it, RESULT_OK with a small Bitmap in the "data" extra;
//  - RESULT_CANCELED when the user backs out.
// Before this nothing resolved them: a "take a photo" button got
// ActivityNotFoundException, or saw resolveActivity() == null and stayed
// hidden. The camera screen is a full-screen activity on a device, so the
// caller is paused and stopped while it is up (hostCover(true)).
//
// The host draws the screen and answers with the JPEG's bytes, or null for a
// cancel, through macapkOnCaptured. The bytes are written from here, inside
// the app's own process, so the caller's own provider serves its EXTRA_OUTPUT.
//
// MACAPK_IMAGE_CAPTURE=0 leaves the intents unresolved, as before.
package com.macapk.loader;

import android.app.Activity;
import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.provider.MediaStore;

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

public final class CameraCaptureHandler {
    private static final String TAG = "Win.apk.Camera";

    static final boolean ON = !"0".equals(System.getenv("MACAPK_IMAGE_CAPTURE"));

    /** The camera app a device lists for the capture intents (AOSP Camera2). */
    static final String CAMERA_PACKAGE = "com.android.camera2";
    static final String CAMERA_ACTIVITY = "com.android.camera.CaptureActivity";

    /** The longest side of the "data" Bitmap without EXTRA_OUTPUT: a thumbnail,
     *  as the camera apps return (Camera2 scales the inline bitmap down). */
    private static final int INLINE_MAX = 640;

    private static final class Pending {
        final com.macapk.loader.app.HostResult token;
        final SpecialAccessHandler.Cover cover;
        final Uri output;
        Pending(com.macapk.loader.app.HostResult token, SpecialAccessHandler.Cover cover, Uri output) {
            this.token = token; this.cover = cover; this.output = output;
        }
    }
    private static final ConcurrentHashMap<Long, Pending> sPending = new ConcurrentHashMap<>();
    private static final AtomicLong sSeq = new AtomicLong(1);

    private CameraCaptureHandler() {}

    /** The PURE predicate: an image-capture intent this host can serve. */
    static boolean isCaptureIntent(Intent intent) {
        if (!ON || intent == null) return false;
        String a = intent.getAction();
        return (MediaStore.ACTION_IMAGE_CAPTURE.equals(a) || MediaStore.ACTION_IMAGE_CAPTURE_SECURE.equals(a))
            && available();
    }

    private static Boolean sAvailable;

    /** Whether the host has the screen (a native side without it: unresolved). */
    private static boolean available() {
        Boolean v = sAvailable;
        if (v == null) {
            boolean on;
            try { on = nativeAvailable(); } catch (Throwable t) { on = false; }
            v = Boolean.valueOf(on);
            sAvailable = v;
        }
        return v.booleanValue();
    }

    /** An explicit start of the camera component we list, pinned by the caller. */
    static boolean isCameraComponent(ComponentName cn) {
        return cn != null && CAMERA_PACKAGE.equals(cn.getPackageName());
    }

    /** The row queryIntentActivities lists for a capture intent. */
    static android.content.pm.ResolveInfo resolveInfo(Intent intent) {
        android.content.pm.ResolveInfo ri = new android.content.pm.ResolveInfo();
        android.content.IntentFilter f = new android.content.IntentFilter();
        f.addAction(intent != null && intent.getAction() != null
                ? intent.getAction() : MediaStore.ACTION_IMAGE_CAPTURE);
        f.addCategory(Intent.CATEGORY_DEFAULT);
        ri.filter = f;
        android.content.pm.ActivityInfo ai = new android.content.pm.ActivityInfo();
        ai.name = CAMERA_ACTIVITY;
        ai.packageName = CAMERA_PACKAGE;
        ai.exported = true;
        ai.enabled = true;
        android.content.pm.ApplicationInfo app = new android.content.pm.ApplicationInfo();
        app.packageName = CAMERA_PACKAGE;
        ai.applicationInfo = app;
        ri.activityInfo = ai;
        return ri;
    }

    /** Claim the caller's result synchronously, cover it as the full-screen
     *  camera activity would, and show the host's screen. */
    static boolean handle(Intent outer, Intent real, SpecialAccessHandler.Cover cover) {
        final com.macapk.loader.app.HostResult token = com.macapk.loader.app.HostResults.claim(outer);
        final SpecialAccessHandler.Cover c = SpecialAccessHandler.RESULT_ORDER_AOSP ? cover : null;
        final Uri output = outputOf(real);
        final long id = sSeq.getAndIncrement();
        sPending.put(id, new Pending(token, c, output));
        NativeLog.i(TAG, "capture: " + real.getAction() + (output != null ? " -> " + output : " (inline bitmap)")
                + " forResult=" + (token != null));
        if (c != null) c.cover();
        boolean shown = false;
        try {
            shown = nativeCapture(id);
        } catch (Throwable t) {
            NativeLog.w(TAG, "capture: no host camera screen (" + t + ") -- cancelled");
        }
        if (!shown) ActivityHost.macapkPostOnOwner(() -> finish(id, null));
        return true;
    }

    /** EXTRA_OUTPUT: a Uri extra, or (MediaStore's documented alternative) the
     *  first item of the intent's ClipData, which androidx's TakePicture fills. */
    private static Uri outputOf(Intent real) {
        Uri u = null;
        try { u = real.getParcelableExtra(MediaStore.EXTRA_OUTPUT, Uri.class); } catch (Throwable ignored) {}
        if (u == null) {
            try {
                android.content.ClipData clip = real.getClipData();
                if (clip != null && clip.getItemCount() > 0) u = clip.getItemAt(0).getUri();
            } catch (Throwable ignored) {}
        }
        return u;
    }

    /** JNI, from the host's thread when its screen closes: the JPEG, or null
     *  when the user cancelled. */
    public static void macapkOnCaptured(long id, byte[] jpeg) {
        ActivityHost.macapkPostOnOwner(() -> finish(id, jpeg));
    }

    private static void finish(long id, byte[] jpeg) {
        Pending p = sPending.remove(id);
        if (p == null) return;
        if (jpeg == null || jpeg.length == 0) {
            NativeLog.i(TAG, "capture: cancelled");
            deliver(p, Activity.RESULT_CANCELED, null);
            return;
        }
        Intent data = new Intent();
        if (p.output != null) {
            if (!write(p.output, jpeg)) {
                deliver(p, Activity.RESULT_CANCELED, null);
                return;
            }
            NativeLog.i(TAG, "capture: " + jpeg.length + " bytes -> " + p.output);
        } else {
            Bitmap thumb = thumbnail(jpeg);
            if (thumb == null) {
                NativeLog.w(TAG, "capture: the picture could not be decoded -- cancelled");
                deliver(p, Activity.RESULT_CANCELED, null);
                return;
            }
            data.putExtra("data", thumb);
            NativeLog.i(TAG, "capture: inline bitmap " + thumb.getWidth() + "x" + thumb.getHeight());
        }
        deliver(p, Activity.RESULT_OK, data);
    }

    /** The JPEG into the caller's Uri: its own provider through its
     *  ContentResolver, or a file path for a file:// Uri. */
    private static boolean write(Uri uri, byte[] jpeg) {
        try {
            if (ContentResolver.SCHEME_FILE.equals(uri.getScheme())) {
                File f = new File(uri.getPath());
                File dir = f.getParentFile();
                if (dir != null) dir.mkdirs();
                try (OutputStream out = new FileOutputStream(f)) { out.write(jpeg); }
                return true;
            }
            android.app.Application app = com.macapk.loader.app.Apps.current();
            if (app == null) throw new IllegalStateException("no application");
            try (OutputStream out = app.getContentResolver().openOutputStream(uri, "w")) {
                if (out == null) throw new java.io.IOException("no stream for " + uri);
                out.write(jpeg);
            }
            return true;
        } catch (Throwable t) {
            NativeLog.w(TAG, "capture: could not write " + uri + ": " + t + " -- cancelled");
            return false;
        }
    }

    private static Bitmap thumbnail(byte[] jpeg) {
        try {
            Bitmap b = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length);
            if (b == null) return null;
            int w = b.getWidth(), h = b.getHeight(), longest = Math.max(w, h);
            if (longest <= INLINE_MAX) return b;
            float s = (float) INLINE_MAX / longest;
            return Bitmap.createScaledBitmap(b, Math.max(1, Math.round(w * s)), Math.max(1, Math.round(h * s)), true);
        } catch (Throwable t) {
            NativeLog.w(TAG, "capture: thumbnail: " + t);
            return null;
        }
    }

    private static void deliver(Pending p, int resultCode, Intent data) {
        if (p.token == null) {
            if (p.cover != null) p.cover.uncover();
            return;
        }
        if (p.cover != null) {
            SpecialAccessHandler.finishWithResult(p.cover, p.token, resultCode, data);
        } else {
            p.token.deliver(resultCode, data);
        }
    }

    /** Whether the host has a capture screen. */
    private static native boolean nativeAvailable();

    /** Shows the host's capture screen; asynchronous: returns once it is
     *  scheduled, and the answer arrives through macapkOnCaptured. */
    private static native boolean nativeCapture(long id);
}
