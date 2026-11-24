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
        Intent copy = new Intent(intent);
        copy.setComponent(new android.content.ComponentName(info.packageName, info.name));
        app.tasks.handler.post(() -> {
            Tasks.Record from = resultTo != null ? app.tasks.find(resultTo) : null;
            app.tasks.start(info, copy, callingPackage, from, resultWho, from != null ? requestCode : -1);
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

    public IActivityClientController getActivityClientController() { return app.clientController; }
}
