// IActivityManager in this process ("activity"): what AOSP's ActivityThread, ContextImpl and
// Instrumentation ask ActivityManagerService. attachApplication is the process start as AMS runs it
// (ActivityManagerService.attachApplicationLocked): the application is bound (bindApplication, with
// the host's ApplicationInfo and configuration), then the activity task manager starts the launch
// activity in it. Methods with no answer here return their type's zero
// (com.winapk.aospapp.gen.IActivityManagerImpl logs each once). Services are ActiveServices'.
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

    public void attachApplication(IApplicationThread thread, long startSeq) {
        try {
            thread.bindApplication(app.info.processName, app.info, null, null,
                    ProviderInfoList.fromList(Collections.emptyList()), null /* instrumentation */, null, null,
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

    public void unbindFinished(android.os.IBinder token, android.content.Intent service, boolean doRebind) {}

    public void updateServiceGroup(android.app.IServiceConnection connection, int group, int importance) {}
}
