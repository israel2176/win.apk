// IActivityManager in this process ("activity"): what AOSP's ActivityThread, ContextImpl and
// Instrumentation ask ActivityManagerService. attachApplication is the process start as AMS runs it
// (ActivityManagerService.attachApplicationLocked): the application is bound (bindApplication, with
// the host's ApplicationInfo and configuration), then the activity task manager starts the launch
// activity in it. Methods with no answer here return their type's zero
// (com.winapk.aospapp.gen.IActivityManagerImpl logs each once); services, receivers and providers
// through ActivityThread are step A3.
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
}
