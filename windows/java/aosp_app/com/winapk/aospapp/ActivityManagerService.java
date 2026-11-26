// IActivityManager in this process ("activity"): what AOSP's ActivityThread, ContextImpl and
// Instrumentation ask ActivityManagerService. attachApplication is the process start as AMS runs it
// (ActivityManagerService.attachApplicationLocked): the application is bound (bindApplication, with
// the host's ApplicationInfo and configuration), then the activity task manager starts the launch
// activity in it. Methods with no answer here return their type's zero
// (com.winapk.aospapp.gen.IActivityManagerImpl logs each once). Services are ActiveServices',
// broadcasts ActiveBroadcasts'.
package com.winapk.aospapp;

import android.app.IApplicationThread;
import android.content.pm.ProviderInfoList;
import android.content.res.CompatibilityInfo;
import android.os.Bundle;
import android.os.RemoteException;
import android.os.SystemClock;

import java.util.Collections;

public final class ActivityManagerService {
    private final AospApp app;

    ActivityManagerService(AospApp app) { this.app = app; }

    /** ActivityManager.INTENT_SENDER_* (hidden). */
    private static final int INTENT_SENDER_BROADCAST = 1, INTENT_SENDER_ACTIVITY = 2,
            INTENT_SENDER_ACTIVITY_RESULT = 3, INTENT_SENDER_SERVICE = 4, INTENT_SENDER_FOREGROUND_SERVICE = 5;

    /** ActivityManagerService.getIntentSenderWithFeature: the PendingIntent's sender a call to
     *  AOSP's own classes asks for. Activity.createPendingResult's INTENT_SENDER_ACTIVITY_RESULT is
     *  a PendingIntent that, sent, gives its code and intent to the activity's onActivityResult as
     *  ActivityTaskManagerService.sendActivityResult does; the other kinds are PendingIntent's own
     *  (Win.apk's PendingIntent makes them itself, without this call). */
    public android.content.IIntentSender getIntentSenderWithFeature(int type, String packageName, String featureId,
            android.os.IBinder token, String resultWho, int requestCode, android.content.Intent[] intents,
            String[] resolvedTypes, int flags, Bundle options, int userId) {
        android.content.Context c = com.macapk.loader.app.Apps.current();
        android.content.Intent first = intents != null && intents.length > 0 ? intents[intents.length - 1] : null;
        android.app.PendingIntent pi;
        switch (type) {
            case INTENT_SENDER_ACTIVITY_RESULT: {
                if (token == null) return null;
                AospHostResult result = new AospHostResult(app.tasks, token, resultWho, requestCode);
                pi = com.macapk.loader.app.IntentSenders.activityResult(c, requestCode, first, flags,
                        (code, data) -> result.deliver(code, data));
                break;
            }
            case INTENT_SENDER_ACTIVITY:
                pi = intents != null && intents.length > 1
                        ? android.app.PendingIntent.getActivities(c, requestCode, intents, flags, options)
                        : android.app.PendingIntent.getActivity(c, requestCode, first, flags, options);
                break;
            case INTENT_SENDER_BROADCAST:
                pi = android.app.PendingIntent.getBroadcast(c, requestCode, first, flags);
                break;
            case INTENT_SENDER_SERVICE:
                pi = android.app.PendingIntent.getService(c, requestCode, first, flags);
                break;
            case INTENT_SENDER_FOREGROUND_SERVICE:
                pi = android.app.PendingIntent.getForegroundService(c, requestCode, first, flags);
                break;
            default:
                return null;
        }
        return pi != null ? com.macapk.loader.app.IntentSenders.target(pi) : null;
    }

    /** The same without a feature id (IActivityManager.getIntentSender). */
    public android.content.IIntentSender getIntentSender(int type, String packageName, android.os.IBinder token,
            String resultWho, int requestCode, android.content.Intent[] intents, String[] resolvedTypes, int flags,
            Bundle options, int userId) {
        return getIntentSenderWithFeature(type, packageName, null, token, resultWho, requestCode, intents,
                resolvedTypes, flags, options, userId);
    }

    public void attachApplication(IApplicationThread thread, long startSeq) {
        // The process's own name, as ActivityManagerService binds a ProcessRecord's (app.processName), not
        // the application's default: a process a component's android:process started is named so
        // (com.android.vending:background). Application.getProcessName() answers it, and the Play Store
        // picks its process's dependency graph by it: its :background services died on a ClassCastException
        // with the main process's graph (issue #208's logs).
        String process = com.macapk.loader.Boot.processName();
        if (process == null || process.isEmpty()) process = app.info.processName;
        try {
            thread.bindApplication(process, app.info, null, null,
                    ProviderInfoList.fromList(app.providers), null /* instrumentation */, null, null,
                    null, null, 0 /* debugMode */, false, false, false /* restrictedBackupMode */, false,
                    app.config, CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO, null /* services */,
                    new Bundle() /* coreSettings */, "unknown", null, null, new long[0] /* disabledCompatChanges */, null,
                    SystemClock.elapsedRealtime(), SystemClock.uptimeMillis());
        } catch (RemoteException e) {
            throw new IllegalStateException(e);
        }
        app.attached(thread);
    }

    public void finishAttachApplication(long startSeq) {}

    // ---- Providers ----

    /** ActivityThread.installProvider published the app's providers (installContentProviders): the
     *  process resolves their authorities to them (ContentProviderRegistry, which ContentResolver
     *  reads). */
    public void publishContentProviders(IApplicationThread caller, java.util.List<android.app.ContentProviderHolder> providers) {
        if (providers == null) return;
        for (android.app.ContentProviderHolder h : providers) {
            if (h == null || h.info == null || h.provider == null) continue;
            android.content.ContentProvider p = android.content.ContentProvider.coerceToLocalContentProvider(h.provider);
            if (p == null) continue;
            android.content.ContentProviderRegistry.register(h.info.authority, p);
            synchronized (published) {
                for (String a : h.info.authority.split(";")) published.put(a.trim(), h.info);
            }
        }
    }

    private final java.util.Map<String, android.content.pm.ProviderInfo> published = new java.util.HashMap<>();

    /** A provider by authority (ContextImpl's ApplicationContentResolver, ActivityThread.acquireProvider):
     *  this process's, with its local provider so the client calls it directly. */
    public android.app.ContentProviderHolder getContentProvider(IApplicationThread caller, String callingPackage,
            String name, int userId, boolean stable) {
        android.content.ContentProvider p = android.content.ContentProviderRegistry.get(name);
        if (p == null) return null;
        android.content.pm.ProviderInfo info;
        synchronized (published) {
            info = published.get(name);
        }
        if (info == null) {
            // One the host installed in the process (the system's media, documents, settings...).
            info = new android.content.pm.ProviderInfo();
            info.authority = name;
            info.name = p.getClass().getName();
            info.packageName = app.info.packageName;
            info.applicationInfo = app.info;
        }
        android.app.ContentProviderHolder h = new android.app.ContentProviderHolder(info);
        h.provider = p.getIContentProvider();
        h.noReleaseNeeded = true;
        return h;
    }

    public boolean refContentProvider(android.os.IBinder connection, int stableDelta, int unstableDelta) {
        return true;
    }

    public void removeContentProvider(android.os.IBinder connection, boolean stable) {}

    // ---- URI grants (UriGrantsManagerService, the system broker's) ----

    /** Context.grantUriPermission: to another app, recorded by the system broker, which checks the
     *  provider allows it and this app may grant it (a SecurityException otherwise); to this app
     *  itself, nothing to grant. */
    public void grantUriPermission(IApplicationThread caller, String targetPkg, android.net.Uri uri, int mode,
            int userId) {
        if (targetPkg == null || targetPkg.equals(app.info.packageName)) return;
        com.macapk.loader.SystemClient.grantUri(targetPkg, uri, mode);
    }

    /** Context.revokeUriPermission([targetPackage,] uri, mode). */
    public void revokeUriPermission(IApplicationThread caller, String targetPkg, android.net.Uri uri, int mode,
            int userId) {
        com.macapk.loader.SystemClient.revokeUri(targetPkg, uri, mode);
    }

    /** Context.checkUriPermission(uri, pid, uid, mode): the system broker's answer for an installed
     *  app's provider (its permissions and the grants, UriGrantsManagerService.checkUriPermission).
     *  A uri no installed app's provider serves (this process's own document providers) is this
     *  app's own to reach: granted to this app, denied to others. */
    public int checkUriPermission(android.net.Uri uri, int pid, int uid, int mode, int userId,
            android.os.IBinder callerToken) {
        if (pid == android.os.Process.myPid()) return android.content.pm.PackageManager.PERMISSION_GRANTED;
        int answer;
        try {
            answer = com.macapk.loader.SystemClient.checkUri(pid, uri, mode);
        } catch (LinkageError e) {
            answer = -1;   // a JVM without the loader (the plain-JVM test)
        }
        if (answer == 1 || (answer != 0 && uid == android.os.Process.myUid())) {
            return android.content.pm.PackageManager.PERMISSION_GRANTED;
        }
        return android.content.pm.PackageManager.PERMISSION_DENIED;
    }

    // ---- Broadcasts (ActiveBroadcasts) ----

    public android.content.Intent registerReceiverWithFeature(IApplicationThread caller, String callerPackage,
            String callingFeatureId, String receiverId, android.content.IIntentReceiver receiver,
            android.content.IntentFilter filter, String requiredPermission, int userId, int flags) {
        return app.broadcasts.register(receiver, filter, flags, requiredPermission);
    }

    public android.content.Intent registerReceiver(IApplicationThread caller, String callerPackage,
            android.content.IIntentReceiver receiver, android.content.IntentFilter filter, String requiredPermission,
            int userId, int flags) {
        return app.broadcasts.register(receiver, filter, flags, requiredPermission);
    }

    public void unregisterReceiver(android.content.IIntentReceiver receiver) {
        app.broadcasts.unregister(receiver);
    }

    public int broadcastIntentWithFeature(IApplicationThread caller, String callingFeatureId,
            android.content.Intent intent, String resolvedType, android.content.IIntentReceiver resultTo,
            int resultCode, String resultData, Bundle resultExtras, String[] requiredPermissions,
            String[] excludedPermissions, String[] excludedPackages, int appOp, Bundle options, boolean serialized,
            boolean sticky, int userId) {
        return app.broadcasts.broadcast(intent, resultTo, resultCode, resultData, resultExtras, serialized, sticky,
                requiredPermissions);
    }

    public int broadcastIntent(IApplicationThread caller, android.content.Intent intent, String resolvedType,
            android.content.IIntentReceiver resultTo, int resultCode, String resultData, Bundle resultExtras,
            String[] requiredPermissions, int appOp, Bundle options, boolean serialized, boolean sticky, int userId) {
        return app.broadcasts.broadcast(intent, resultTo, resultCode, resultData, resultExtras, serialized, sticky,
                requiredPermissions);
    }

    public void unbroadcastIntent(IApplicationThread caller, android.content.Intent intent, int userId) {
        app.broadcasts.removeSticky(intent);
    }

    public void finishReceiver(android.os.IBinder who, int resultCode, String resultData, Bundle resultExtras,
            boolean resultAbort, int flags) {
        app.broadcasts.finishReceiver(who, resultCode, resultData, resultExtras, resultAbort);
    }

    // ---- Services (ActiveServices) ----

    public android.content.ComponentName startService(IApplicationThread caller, android.content.Intent service,
            String resolvedType, boolean requireForeground, String callingFeatureId, String callingPackage,
            int userId) {
        return app.services.startService(service);
    }

    public int stopService(IApplicationThread caller, android.content.Intent service, String resolvedType,
            int userId) {
        return app.services.stopService(service);
    }

    public boolean stopServiceToken(android.content.ComponentName className, android.os.IBinder token,
            int startId) {
        return app.services.stopServiceToken(className, token, startId);
    }

    public int bindService(IApplicationThread caller, android.os.IBinder token, android.content.Intent service,
            String resolvedType, android.app.IServiceConnection connection, long flags, String callingPackage,
            int userId) {
        return app.services.bindService(service, connection, flags);
    }

    public int bindServiceInstance(IApplicationThread caller, android.os.IBinder token,
            android.content.Intent service, String resolvedType, android.app.IServiceConnection connection,
            long flags, String instanceName, String callingPackage, int userId) {
        return app.services.bindService(service, connection, flags);
    }

    public boolean unbindService(android.app.IServiceConnection connection) {
        return app.services.unbindService(connection);
    }

    public void publishService(android.os.IBinder token, android.content.Intent intent, android.os.IBinder service) {
        app.services.publishService(token, intent, service);
    }

    public android.os.IBinder peekService(android.content.Intent service, String resolvedType,
            String callingPackage) {
        return app.services.peekService(service);
    }

    public void setServiceForeground(android.content.ComponentName className, android.os.IBinder token, int id,
            android.app.Notification notification, int flags, int foregroundServiceType) {
        app.services.setServiceForeground(token, id, foregroundServiceType);
    }

    public int getForegroundServiceType(android.content.ComponentName className, android.os.IBinder token) {
        return app.services.getForegroundServiceType(token);
    }

    /** What the client says it finished (onCreate, onStartCommand, onBind...): nothing waits here. */
    public void serviceDoneExecuting(android.os.IBinder token, int type, int startId, int res) {}

    public void unbindFinished(android.os.IBinder token, android.content.Intent service, boolean doRebind) {
        app.services.unbindFinished(token, service, doRebind);
    }

    public void updateServiceGroup(android.app.IServiceConnection connection, int group, int importance) {}
}
