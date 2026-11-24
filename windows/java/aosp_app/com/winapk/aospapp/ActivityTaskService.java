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
        ActivityInfo info = app.resolver.resolveActivity(intent, resolvedType);
        if (info == null) return ActivityManager.START_INTENT_NOT_RESOLVED;
        Intent copy = new Intent(intent);
        copy.setComponent(new android.content.ComponentName(info.packageName, info.name));
        app.tasks.handler.post(() -> {
            Tasks.Record from = resultTo != null ? app.tasks.find(resultTo) : null;
            app.tasks.start(info, copy, callingPackage, from, resultWho, from != null ? requestCode : -1);
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
