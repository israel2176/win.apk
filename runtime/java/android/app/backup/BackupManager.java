// android.app.backup.BackupManager -- "tell the system my data
// changed" service. Cocos2d-x calls dataChanged() after writing user
// prefs. There's no backup transport in Mac.apk; calls are no-ops.
//
// Win.apk: every method answers as AOSP 14's BackupManager does when it finds no
// backup service (checkServiceBinder leaves sService null): no transport, backup
// off, nothing to restore. Google Play services calls the @SystemApi members
// (getCurrentTransport in its d2d MigrateCleaner); without them its process died
// on NoSuchMethodError at every start of that operation, and the device checkin it
// was running never finished (issue #208).
package android.app.backup;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;

public class BackupManager {
    // AOSP's values (frameworks/base/core/java/android/app/backup/BackupManager.java).
    public static final int SUCCESS = 0;
    public static final int ERROR_BACKUP_NOT_ALLOWED = -2001;
    public static final int ERROR_PACKAGE_NOT_FOUND = -2002;
    public static final int ERROR_BACKUP_CANCELLED = -2003;
    public static final int ERROR_TRANSPORT_ABORTED = -1000;
    public static final int ERROR_TRANSPORT_PACKAGE_REJECTED = -1002;
    public static final int ERROR_TRANSPORT_QUOTA_EXCEEDED = -1005;
    public static final int ERROR_AGENT_FAILURE = -1003;
    public static final int ERROR_TRANSPORT_UNAVAILABLE = -1;
    public static final int ERROR_TRANSPORT_INVALID = -2;
    public static final int FLAG_NON_INCREMENTAL_BACKUP = 1;
    public static final String EXTRA_BACKUP_SERVICES_AVAILABLE = "backup_services_available";
    public static final String PACKAGE_MANAGER_SENTINEL = "@pm@";

    public BackupManager(Context context) {}
    public void dataChanged() {}
    public static void dataChanged(String packageName) {}
    public static void dataChangedForUser(int userId, String packageName) {}
    /** AOSP: -1 when there is no backup service. */
    public int requestRestore(RestoreObserver observer) { return -1; }

    /** AOSP getUserForAncestralSerialNumber(long ancestralSerialNumber) (API 29):
     *  "Returns a UserHandle for the user that has ancestral serial number
     *  ancestralSerialNumber", used during a device-to-device restore to map a
     *  profile on the OLD device to one on the new. AOSP returns null when no
     *  user matches -- and Mac.apk has no restore session, no ancestral device
     *  and exactly one user, so nothing can match. */
    public android.os.UserHandle getUserForAncestralSerialNumber(long ancestralSerialNumber) {
        return null;
    }

    // ---- @SystemApi, as AOSP answers with no backup service ----

    public void setBackupEnabled(boolean isEnabled) {}
    public void setFrameworkSchedulingEnabled(boolean isEnabled) {}
    public boolean isBackupEnabled() { return false; }
    public boolean isBackupServiceActive(android.os.UserHandle user) { return false; }
    public void setAutoRestore(boolean isEnabled) {}
    public String getCurrentTransport() { return null; }
    public ComponentName getCurrentTransportComponent() { return null; }
    public String[] listAllTransports() { return null; }
    public void updateTransportAttributes(ComponentName transportComponent, String name,
            Intent configurationIntent, String currentDestinationString, Intent dataManagementIntent,
            String dataManagementLabel) {}
    public void updateTransportAttributes(ComponentName transportComponent, String name,
            Intent configurationIntent, String currentDestinationString, Intent dataManagementIntent,
            CharSequence dataManagementLabel) {}
    public String selectBackupTransport(String transport) { return null; }
    public void backupNow() {}
    public long getAvailableRestoreToken(String packageName) { return 0; }
    public boolean isAppEligibleForBackup(String packageName) { return false; }
    public void cancelBackups() {}
    public void setAncestralSerialNumber(long ancestralSerialNumber) {}
    public Intent getConfigurationIntent(String transportName) { return null; }
    public String getDestinationString(String transportName) { return null; }
    public Intent getDataManagementIntent(String transportName) { return null; }
    public String getDataManagementLabel(String transportName) { return null; }
    public CharSequence getDataManagementIntentLabel(String transportName) { return null; }
    public void excludeKeysFromRestore(String packageName, java.util.List<String> keys) {}
}
