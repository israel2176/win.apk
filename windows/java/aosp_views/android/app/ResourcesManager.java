// The part of AOSP's android.app.ResourcesManager that AOSP's views call (WINAPK_AOSP_VIEWS=1):
// ViewRootImpl.updateInternalDisplay after every configuration change of a window,
// ConfigurationHelper and WindowMetricsController. Win.apk's resources are made by its own
// ResBackend, not by a ResourcesManager, and AOSP's whole class (its ResourcesImpl caches and
// per-activity override configurations) has nothing to manage here; these three members have the
// bodies AOSP gives them for a process with no activity override configurations.
package android.app;

import android.content.res.Configuration;
import android.content.res.Resources;
import android.hardware.display.DisplayManagerGlobal;
import android.os.IBinder;
import android.view.Display;

public final class ResourcesManager {
    private static final ResourcesManager sInstance = new ResourcesManager();

    private ResourcesManager() {}

    public static ResourcesManager getInstance() { return sInstance; }

    /** AOSP: the display with the resources' display adjustments. */
    public Display getAdjustedDisplay(int displayId, Resources resources) {
        DisplayManagerGlobal dm = DisplayManagerGlobal.getInstance();
        if (dm == null) return null;
        return dm.getCompatibleDisplay(displayId, resources);
    }

    /** AOSP: with no ActivityResources registered for the token, the override configuration is
     *  the same only when there is none. */
    public boolean isSameResourcesOverrideConfig(IBinder activityToken, Configuration overrideConfig) {
        return overrideConfig == null;
    }
}
