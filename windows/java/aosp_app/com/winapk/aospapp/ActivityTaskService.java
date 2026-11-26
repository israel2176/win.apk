// IActivityTaskManager in this process ("activity_task"): what AOSP's Instrumentation.execStartActivity,
// ActivityTaskManager and ActivityClient ask ActivityTaskManagerService. A start is resolved by the
// host's package manager (AospApp.Resolver), answered at once as system_server answers a binder call,
// and carried out on the system thread by Tasks. Methods with no answer here return their type's zero
// (com.winapk.aospapp.gen.IActivityTaskManagerImpl logs each once).
package com.winapk.aospapp;

import android.app.ActivityManager;
import android.app.IActivityClientController;
import android.app.IApplicationThread;
import android.app.ProfilerInfo;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.os.IBinder;

public final class ActivityTaskService {
    private final AospApp app;

    /** ActivityManager.START_FORWARD_AND_REQUEST_CONFLICT (hidden): Instrumentation
     *  .checkStartActivityResult throws AndroidRuntimeException for it. */
    private static final int START_FORWARD_AND_REQUEST_CONFLICT = -93;

    ActivityTaskService(AospApp app) { this.app = app; }

    /** ActivityTaskManagerService.startActivity: START_INTENT_NOT_RESOLVED (-92) when nothing
     *  matches, which Instrumentation.checkStartActivityResult turns into ActivityNotFoundException,
     *  as on Android; otherwise START_SUCCESS, the start itself done on the system thread. */
    public int startActivity(IApplicationThread caller, String callingPackage, String callingFeatureId,
            Intent intent, String resolvedType, IBinder resultTo, String resultWho, int requestCode,
            int flags, ProfilerInfo profilerInfo, Bundle options) {
        if (android.content.pm.PackageManager.ACTION_REQUEST_PERMISSIONS.equals(intent.getAction())) {
            return requestPermissions(intent, resultTo, resultWho, requestCode);
        }
        ActivityInfo info = app.resolver.resolveActivity(intent, resolvedType);
        // Another package's activity runs in that package's process on a device: here the host
        // takes it (another installed app, the system's settings pages, the browser...).
        if (info == null || !app.info.packageName.equals(info.packageName)) {
            return startOnHost(intent, resultTo, resultWho, requestCode);
        }
        if ((intent.getFlags() & Intent.FLAG_ACTIVITY_FORWARD_RESULT) != 0 && resultTo != null && requestCode >= 0) {
            // ActivityStarter.executeRequest: a result both forwarded and asked for.
            return START_FORWARD_AND_REQUEST_CONFLICT;
        }
        Intent copy = new Intent(intent);
        copy.setComponent(new android.content.ComponentName(info.packageName, info.name));
        app.tasks.handler.post(() -> {
            Tasks.Record source = resultTo != null ? app.tasks.find(resultTo) : null;
            app.tasks.startFrom(info, copy, callingPackage, source, resultWho, requestCode);
        });
        return ActivityManager.START_SUCCESS;
    }

    /** A start no activity of the app answers: the host's hand-off (AppHost.Start.hostStart), as
     *  another app or the system would take it on a device, run here on the caller's thread with
     *  the requester's HostResult on offer; START_INTENT_NOT_RESOLVED when the host has nothing. */
    private int startOnHost(Intent intent, IBinder resultTo, String resultWho, int requestCode) {
        if (app.hostStart == null) return ActivityManager.START_INTENT_NOT_RESOLVED;
        com.macapk.loader.app.HostResults.offer(intent, resultTo != null && requestCode >= 0
                ? new AospHostResult(app.tasks, resultTo, resultWho, requestCode) : null);
        try {
            boolean taken = app.hostStart.test(intent);
            System.err.println("[aosp-app] a start no activity of the app answers, " + (taken ? "taken" : "refused")
                    + " by the host: " + intent);
            return taken ? ActivityManager.START_SUCCESS : ActivityManager.START_INTENT_NOT_RESOLVED;
        } finally {
            com.macapk.loader.app.HostResults.offer(null, null);
        }
    }

    /** PackageManager.ACTION_REQUEST_PERMISSIONS (Activity.requestPermissions): the permission
     *  controller's prompt on a device, the host's here (AppHost.Start.hostPermissions); its answer
     *  goes back as GrantPermissionsActivity's result: RESULT_OK with the names and the grants. */
    private int requestPermissions(Intent intent, IBinder resultTo, String resultWho, int requestCode) {
        String[] names = intent.getStringArrayExtra(android.content.pm.PackageManager.EXTRA_REQUEST_PERMISSIONS_NAMES);
        if (names == null) names = new String[0];
        AospHostResult result = resultTo != null && requestCode >= 0
                ? new AospHostResult(app.tasks, resultTo, resultWho, requestCode) : null;
        if (app.hostPermissions == null) return ActivityManager.START_INTENT_NOT_RESOLVED;
        app.hostPermissions.accept(names, (perms, grants) -> {
            if (result == null) return;
            Intent data = new Intent(android.content.pm.PackageManager.ACTION_REQUEST_PERMISSIONS)
                    .putExtra(android.content.pm.PackageManager.EXTRA_REQUEST_PERMISSIONS_NAMES, perms)
                    .putExtra(android.content.pm.PackageManager.EXTRA_REQUEST_PERMISSIONS_RESULTS, grants);
            result.deliver(android.app.Activity.RESULT_OK, data);
        });
        return ActivityManager.START_SUCCESS;
    }

    /** ActivityTaskManagerService.startActivities (TaskStackBuilder, Context.startActivities): each
     *  intent resolved first (one that resolves to nothing fails them all), then the whole stack
     *  started in order, the last on top: only it is launched, the ones under it are made when the
     *  user comes back to them, as ActivityStarter does with each record but the top. */
    public int startActivities(IApplicationThread caller, String callingPackage, String callingFeatureId,
            Intent[] intents, String[] resolvedTypes, IBinder resultTo, Bundle options, int userId) {
        if (intents == null || intents.length == 0) return ActivityManager.START_SUCCESS;
        ActivityInfo[] infos = new ActivityInfo[intents.length];
        Intent[] copies = new Intent[intents.length];
        for (int i = 0; i < intents.length; i++) {
            infos[i] = app.resolver.resolveActivity(intents[i],
                    resolvedTypes != null && i < resolvedTypes.length ? resolvedTypes[i] : null);
            if (infos[i] == null) return ActivityManager.START_INTENT_NOT_RESOLVED;
            copies[i] = new Intent(intents[i]);
            copies[i].setComponent(new android.content.ComponentName(infos[i].packageName, infos[i].name));
        }
        app.tasks.handler.post(() -> app.tasks.startAll(infos, copies, callingPackage));
        return ActivityManager.START_SUCCESS;
    }

    /** ActivityTaskManagerService.startActivityIntentSender (Activity.startIntentSenderForResult,
     *  ContextImpl.startIntentSender): the PendingIntent behind the sender is sent as
     *  PendingIntentRecord.sendInner sends it. One activity is started for the caller's result, with
     *  the fill-in merged and the caller's flags applied; any other kind runs as its send() does.
     *  START_CANCELED for a cancelled sender (AOSP's Activity throws SendIntentException for it).
     *  MediaStore's create*Request consent screens, which the system shows on a device and Win.apk
     *  has none of, are the user declining: RESULT_CANCELED to the caller. */
    public int startActivityIntentSender(IApplicationThread caller, android.content.IIntentSender target,
            IBinder allowlistToken, Intent fillInIntent, String resolvedType, IBinder resultTo, String resultWho,
            int requestCode, int flagsMask, int flagsValues, Bundle options) {
        android.app.PendingIntent pi = com.macapk.loader.app.IntentSenders.of(target);
        if (pi == null) {
            if (target == null) throw new IllegalArgumentException("Bad PendingIntent object");
            // Another process's sender: its own process sends it (no result comes back).
            try {
                target.send(0, fillInIntent, resolvedType, allowlistToken, null, null, options);
            } catch (android.os.RemoteException e) {
                return START_CANCELED;
            }
            return ActivityManager.START_SUCCESS;
        }
        if (com.macapk.loader.app.IntentSenders.canceled(pi)) return START_CANCELED;
        Intent intent = com.macapk.loader.app.IntentSenders.activityIntent(pi, fillInIntent, flagsMask, flagsValues);
        if (intent == null) {
            return com.macapk.loader.app.IntentSenders.sendCanceled(pi, 0, fillInIntent)
                    ? START_CANCELED : ActivityManager.START_SUCCESS;
        }
        if (com.macapk.loader.app.IntentSenders.isMediaConsentRequest(intent)) {
            if (resultTo != null && requestCode >= 0) {
                new AospHostResult(app.tasks, resultTo, resultWho, requestCode)
                        .deliver(android.app.Activity.RESULT_CANCELED, null);
            }
            return ActivityManager.START_SUCCESS;
        }
        return startActivity(caller, pi.getCreatorPackage(), null, intent, resolvedType, resultTo, resultWho,
                requestCode, 0, null, options);
    }

    /** ActivityManager.START_CANCELED (hidden). */
    private static final int START_CANCELED = -96;

    public IActivityClientController getActivityClientController() { return app.clientController; }
}
