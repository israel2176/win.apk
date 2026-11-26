// ISliceManager in this process ("slice"), what AOSP's SliceManager asks system_server's
// SliceManagerService (frameworks/base/services/core/java/com/android/server/slice). A device keeps the
// slices pinned and the access granted to them; it keeps them here, for the one app in the process, as a
// phone keeps them where no slice host (no assistant, no launcher showing slices) is installed:
// hasSliceAccess is false for every package, a pin reaches the provider's onSlicePinned once, and a
// package other than the provider's own may read a slice only once granted. What the service would
// keep across a reboot (its backup payload) is the system's own, refused to an app as on a device.
package com.winapk.aospapp;

import android.app.ActivityThread;
import android.app.Application;
import android.app.slice.SliceProvider;
import android.app.slice.SliceSpec;
import android.content.ContentProvider;
import android.content.ContentProviderRegistry;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Handler;
import android.os.IBinder;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;

public final class SliceService {
    private final AospApp app;
    private final Handler handler;

    /** A pinned slice (PinnedSliceState): who pinned it, by token, and the specs they asked for. */
    private static final class Pinned {
        final String pkg;
        final Map<IBinder, String> pins = new HashMap<>();
        SliceSpec[] specs;
        Pinned(String pkg) { this.pkg = pkg; }
    }

    private final Map<Uri, Pinned> pinned = new LinkedHashMap<>();
    /** The packages granted access to a slice (SlicePermissionManager), by its uri. */
    private final Map<Uri, Set<String>> grants = new HashMap<>();

    SliceService(AospApp app, Handler handler) {
        this.app = app;
        this.handler = handler;
    }

    public void pinSlice(String pkg, Uri uri, SliceSpec[] specs, IBinder token) {
        verifyCaller(pkg);
        enforceAccess(pkg, uri);
        boolean first;
        synchronized (pinned) {
            Pinned p = pinned.get(uri);
            first = p == null;
            if (first) pinned.put(uri, p = new Pinned(providerPkg(uri)));
            p.pins.put(token, pkg);
            p.specs = p.specs == null ? specs : mergeSpecs(p.specs, specs);
        }
        if (first) notifyProvider(uri, true);
    }

    public void unpinSlice(String pkg, Uri uri, IBinder token) {
        verifyCaller(pkg);
        enforceAccess(pkg, uri);
        boolean last = false;
        synchronized (pinned) {
            Pinned p = pinned.get(uri);
            if (p == null) return;
            p.pins.remove(token);
            if (p.pins.isEmpty()) {
                pinned.remove(uri);
                last = true;
            }
        }
        if (last) notifyProvider(uri, false);
    }

    /** Full access to every slice is the assistant's and the default launcher's (hasFullSliceAccess):
     *  neither is installed here. */
    public boolean hasSliceAccess(String pkg) {
        verifyCaller(pkg);
        return false;
    }

    public SliceSpec[] getPinnedSpecs(Uri uri, String pkg) {
        verifyCaller(pkg);
        enforceAccess(pkg, uri);
        synchronized (pinned) {
            Pinned p = pinned.get(uri);
            if (p == null) throw new IllegalStateException(String.format("Slice %s not pinned", uri));
            return p.specs == null ? new SliceSpec[0] : p.specs.clone();
        }
    }

    public Uri[] getPinnedSlices(String pkg) {
        verifyCaller(pkg);
        List<Uri> out = new ArrayList<>();
        synchronized (pinned) {
            for (Map.Entry<Uri, Pinned> e : pinned.entrySet()) {
                if (Objects.equals(e.getValue().pkg, pkg)) out.add(e.getKey());
            }
        }
        return out.toArray(new Uri[0]);
    }

    public byte[] getBackupPayload(int user) {
        throw new SecurityException("Caller must be system");
    }

    public void applyRestore(byte[] payload, int user) {
        throw new SecurityException("Caller must be system");
    }

    public void grantSlicePermission(String pkg, String toPkg, Uri uri) {
        verifyCaller(pkg);
        enforceOwner(pkg, uri);
        synchronized (grants) {
            grants.computeIfAbsent(uri, u -> new HashSet<>()).add(toPkg);
        }
    }

    public void revokeSlicePermission(String pkg, String toPkg, Uri uri) {
        verifyCaller(pkg);
        enforceOwner(pkg, uri);
        synchronized (grants) {
            Set<String> s = grants.get(uri);
            if (s != null) s.remove(toPkg);
        }
    }

    /** SliceManagerService.checkSlicePermission: for each package of the uid, granted when it was
     *  given the slice, or holds one of the provider's auto-grant permissions (which grants it). */
    public int checkSlicePermission(Uri uri, String callingPkg, int pid, int uid, String[] autoGrant) {
        String[] pkgs = packagesForUid(uid);
        if (pkgs == null) return PackageManager.PERMISSION_DENIED;
        for (String pkg : pkgs) {
            synchronized (grants) {
                Set<String> s = grants.get(uri);
                if (s != null && s.contains(pkg)) return PackageManager.PERMISSION_GRANTED;
            }
            if (autoGrant != null && callingPkg != null) {
                enforceOwner(callingPkg, uri);
                Application a = ActivityThread.currentApplication();
                for (String perm : autoGrant) {
                    if (a != null && a.checkPermission(perm, pid, uid) == PackageManager.PERMISSION_GRANTED) {
                        synchronized (grants) {
                            grants.computeIfAbsent(uri, u -> new HashSet<>()).add(pkg);
                        }
                        return PackageManager.PERMISSION_GRANTED;
                    }
                }
            }
        }
        return PackageManager.PERMISSION_DENIED;
    }

    /** The permission screen's answer: MANAGE_SLICE_PERMISSIONS, a signature permission of the
     *  system's, which no app holds. */
    public void grantPermissionFromUser(Uri uri, String pkg, String callingPkg, boolean allSlices) {
        throw new SecurityException("Permission Denial: grantPermissionFromUser requires "
                + "android.permission.MANAGE_SLICE_PERMISSIONS");
    }

    // ---- what SliceManagerService checks ----

    private String[] packagesForUid(int uid) {
        return uid == android.os.Process.myUid() || uid == app.info.uid ? new String[] {app.info.packageName} : null;
    }

    /** verifyCaller: the calling uid owns pkg (AppOpsManager.checkPackage). */
    private void verifyCaller(String pkg) {
        if (pkg == null || !pkg.equals(app.info.packageName)) {
            throw new SecurityException("Specified package " + pkg + " under uid " + app.info.uid
                    + " but it is not");
        }
    }

    /** enforceAccess: the provider's own package, or one granted the slice. */
    private void enforceAccess(String pkg, Uri uri) {
        if (Objects.equals(providerPkg(uri), pkg)) return;
        synchronized (grants) {
            Set<String> s = grants.get(uri);
            if (s != null && s.contains(pkg)) return;
        }
        throw new SecurityException("Access to slice " + uri + " is required");
    }

    private void enforceOwner(String pkg, Uri uri) {
        if (pkg == null || !Objects.equals(providerPkg(uri), pkg)) throw new SecurityException("Caller must own " + uri);
    }

    /** The package whose provider answers the uri's authority: the app's, when one of its providers
     *  is published for it. */
    private String providerPkg(Uri uri) {
        String authority = uri == null ? null : uri.getAuthority();
        if (authority == null) return null;
        return ContentProviderRegistry.getLocal(authority) != null ? app.info.packageName : null;
    }

    /** PinnedSliceState's first pin and last unpin: the provider's onSlicePinned and onSliceUnpinned,
     *  which AOSP's SliceProvider runs from its "pin"/"unpin" call (the system's alone), on the
     *  service's thread. */
    private void notifyProvider(Uri uri, boolean pin) {
        ContentProvider p = ContentProviderRegistry.getLocal(uri.getAuthority());
        if (!(p instanceof SliceProvider)) return;
        SliceProvider sp = (SliceProvider) p;
        handler.post(() -> {
            try {
                if (pin) sp.onSlicePinned(uri);
                else sp.onSliceUnpinned(uri);
            } catch (RuntimeException e) {
                System.err.println("[aosp-app] slice " + uri + (pin ? " pin" : " unpin") + ": " + e);
            }
        });
    }

    /** PinnedSliceState.mergeSpecs: the specs both ask for, each at the lower revision. */
    private static SliceSpec[] mergeSpecs(SliceSpec[] have, SliceSpec[] asked) {
        if (asked == null) return have;
        List<SliceSpec> out = new ArrayList<>();
        for (SliceSpec s : have) {
            for (SliceSpec a : asked) {
                if (Objects.equals(s.getType(), a.getType())) {
                    out.add(s.getRevision() <= a.getRevision() ? s : a);
                    break;
                }
            }
        }
        return out.toArray(new SliceSpec[0]);
    }
}
