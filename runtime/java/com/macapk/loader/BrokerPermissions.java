// com.macapk.loader.BrokerPermissions -- Win.apk: the permission checks the
// system broker makes when one app reaches another's component, as AOSP 14's
// system_server makes them. Before this the broker checked android:exported
// alone, so any app reached another's exported component however it was
// guarded.
//
//   checkComponent  ActivityManagerService.checkComponentPermission: the same
//                   app (uid) always; an unexported component no other app; no
//                   permission any app; else the caller must hold it.
//   holds           PermissionManagerService.checkPermission(perm, pkg): the
//                   package requests it (<uses-permission>) and it is granted:
//                     normal      at install, to every package that requests it;
//                     dangerous   by the user, in the signed app-ops store
//                                 (SpecialAccessStore.runtimeGrantedFor), or at
//                                 install to an app targeting API 22 or lower;
//                     signature   to a package signed as the permission's
//                                 declaring package is (ApkVerification, the
//                                 verified certificates and their rotation
//                                 lineage, CertCapabilities.PERMISSION), as
//                                 shouldGrantPermissionBySignature;
//                     |privileged, signatureOrSystem, |preinstalled
//                                 also to a privileged system app: here the
//                                 Google apps Win.apk ships as system apps,
//                                 only when Google's certificate verifiably
//                                 signed them (MacApkPackageManager.platformSystemApk);
//                     |pre23      also to an app targeting API 22 or lower.
//                   A permission no installed package and not the platform
//                   declares is granted to nobody. The platform's own
//                   (package "android") come from AOSP 14's framework-res-34.apk
//                   manifest, the table the framework's resources read
//                   (-Dwinapk.aosp.res.framework); no app carries the platform's
//                   key, so its signature permissions go to privileged apps only.
//   providers       ContentProviderHelper.checkContentProviderPermission (may the
//                   caller open the provider: its read or write permission, a
//                   path-permission's, or a URI grant under its authority), and
//                   ContentProvider.Transport's enforceReadPermission /
//                   enforceWritePermission per operation (read for query and a
//                   file opened without 'w', write for insert, update, delete
//                   and a file opened with 'w'), path-permissions first by path.
//   URI grants      UriGrantsManagerService: Intent.FLAG_GRANT_READ/WRITE_URI_PERMISSION
//                   (and FLAG_GRANT_PREFIX_URI_PERMISSION) on an intent the broker
//                   delivers to another app, and Context.grantUriPermission. The
//                   provider must allow it (android:grantUriPermissions or a
//                   matching <grant-uri-permission>), and the granter must hold
//                   access itself (its own provider, the provider's permissions, or
//                   a grant). Grants live as long as the broker (a device's last
//                   until it reboots unless persisted) and go with either package.
//
// The caller is always the package the broker has on record for the calling
// process (SystemServer.callerPackage: the pid the OS gives the pipe, mapped to
// the package the broker started it as or whose installed APK its command line
// names), never what a parcel says. null stands for the system (the broker
// itself), which holds every permission, as the system uid does on a device.
package com.macapk.loader;

import android.content.Intent;
import android.content.pm.PathPermission;
import android.content.pm.PermissionInfo;
import android.net.Uri;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

public final class BrokerPermissions {

    /** What the checks need to know of the installed packages. */
    public interface Packages {
        /** The package's manifest, or null when it is not installed. */
        Manifest manifest(String pkg);
        /** Its verified signers (never null; unverified when the signature does not verify). */
        ApkVerification.Result signers(String pkg);
        /** A privileged system app (a priv-app on a device). */
        boolean privileged(String pkg);
        /** The user's answer for a dangerous permission the package requests. */
        boolean runtimeGranted(String pkg, Manifest m, String perm);
        /** Its uid (packages sharing a uid are one app). */
        int uid(String pkg);
        /** Every installed package. */
        Iterable<String> all();
    }

    static final String PLATFORM = "android";

    private final Packages pkgs;
    final UriGrants grants = new UriGrants();

    public BrokerPermissions(Packages pkgs) { this.pkgs = pkgs; }

    // ---- permissions ---------------------------------------------------------------

    /** ActivityManagerService.checkComponentPermission(permission, pid, uid, owningUid, exported),
     *  the caller and the component's owner by package; a null caller is the system. */
    public boolean checkComponent(String permission, String caller, String owner, boolean exported) {
        if (caller == null || sameApp(caller, owner)) return true;
        if (!exported) return false;
        if (permission == null || permission.isEmpty()) return true;
        return holds(caller, permission);
    }

    /** A package's uid, the system's for null (the system). */
    public int uidOf(String pkg) { return pkg == null ? android.os.Process.SYSTEM_UID : pkgs.uid(pkg); }

    /** The same app: one package, or two sharing a uid. */
    boolean sameApp(String a, String b) {
        if (a == null || b == null) return a == b;
        if (a.equals(b)) return true;
        int ua = pkgs.uid(a), ub = pkgs.uid(b);
        return ua >= 0 && ua == ub;
    }

    /** PackageManager.checkPermission(permission, pkg) == PERMISSION_GRANTED; a null package is the system. */
    public boolean holds(String pkg, String permission) {
        if (pkg == null) return true;
        if (permission == null || permission.isEmpty()) return false;
        Manifest m = pkgs.manifest(pkg);
        if (m == null || !requests(m, permission)) return false;
        Object[] decl = declaration(permission);
        if (decl == null) return false;   // declared by nobody installed: granted to nobody
        String declarer = (String) decl[0];
        int level = (Integer) decl[1];
        int base = level & PermissionInfo.PROTECTION_MASK_BASE;
        boolean legacy = m.targetSdk > 0 && m.targetSdk < 23;
        switch (base) {
            case PermissionInfo.PROTECTION_NORMAL:
                return true;
            case PermissionInfo.PROTECTION_DANGEROUS:
                return legacy || pkgs.runtimeGranted(pkg, m, permission);
            case PermissionInfo.PROTECTION_SIGNATURE:
            case PermissionInfo.PROTECTION_SIGNATURE_OR_SYSTEM:
            case PermissionInfo.PROTECTION_INTERNAL:
                if (base != PermissionInfo.PROTECTION_INTERNAL && !PLATFORM.equals(declarer)
                        && signedAs(declarer, pkg)) {
                    return true;
                }
                if ((base == PermissionInfo.PROTECTION_SIGNATURE_OR_SYSTEM
                        || (level & PermissionInfo.PROTECTION_FLAG_PRIVILEGED) != 0
                        || (level & PermissionInfo.PROTECTION_FLAG_PREINSTALLED) != 0) && pkgs.privileged(pkg)) {
                    return true;
                }
                return (level & PermissionInfo.PROTECTION_FLAG_PRE23) != 0 && legacy;
            default:
                return false;
        }
    }

    /** shouldGrantPermissionBySignature: the package is signed as the permission's declarer is
     *  (its signer, or one in a lineage that gives the permission capability). */
    boolean signedAs(String declarer, String pkg) {
        if (declarer.equals(pkg)) return true;
        return ApkVerification.hasSignatureCapability(pkgs.signers(declarer), pkgs.signers(pkg),
                ApkVerification.CAP_PERMISSION);
    }

    static boolean requests(Manifest m, String permission) {
        for (Manifest.UsesPermissionEntry e : m.usesPermissions) {
            if (permission.equals(e.name)) return true;
        }
        return false;
    }

    /** {declaring package, protection level} of a permission, or null when nobody declares it. An
     *  app's own declaration of a platform permission does not replace the platform's (AOSP keeps
     *  the system's definition). */
    Object[] declaration(String permission) {
        Integer platform = platformLevels().get(permission);
        if (platform != null) return new Object[] { PLATFORM, platform };
        for (String p : pkgs.all()) {
            Manifest m = pkgs.manifest(p);
            if (m == null) continue;
            for (Manifest.PermissionEntry e : m.permissions) {
                if (permission.equals(e.name)) return new Object[] { p, e.protectionLevel };
            }
        }
        return null;
    }

    private static volatile Map<String, Integer> sPlatform;

    /** The platform's permissions and their protection levels, from framework-res-34.apk's
     *  manifest (read once; 911 permissions, about 150 ms). Empty, with a log line, when the file is
     *  not there: then no platform permission is granted to anyone. */
    static Map<String, Integer> platformLevels() {
        Map<String, Integer> m = sPlatform;
        if (m != null) return m;
        synchronized (BrokerPermissions.class) {
            if (sPlatform != null) return sPlatform;
            Map<String, Integer> out = new java.util.HashMap<>();
            String apk = System.getProperty("winapk.aosp.res.framework");
            try {
                if (apk == null || !new File(apk).isFile()) throw new java.io.FileNotFoundException(String.valueOf(apk));
                Manifest fw = Manifest.parse(new File(""), apk);
                for (Manifest.PermissionEntry e : fw.permissions) out.put(e.name, e.protectionLevel);
                log("platform permissions: " + out.size() + " from " + apk);
            } catch (Throwable t) {
                log("platform permissions unknown (" + t + "): none is granted to an app");
            }
            sPlatform = java.util.Collections.unmodifiableMap(out);
            return sPlatform;
        }
    }

    // ---- content providers ---------------------------------------------------------------

    /** ContentProviderHelper.checkContentProviderPermission: null when `caller` may open the
     *  provider, else AOSP's denial (the SecurityException's message). `who` names the caller in it,
     *  "(pid=<pid>, uid=<uid>)". */
    public String providerOpenDenial(String caller, String owner, Manifest.ProviderEntry e, String who) {
        if (caller == null || sameApp(caller, owner)) return null;
        if (checkComponent(e.readPermission, caller, owner, e.exported)) return null;
        if (checkComponent(e.writePermission, caller, owner, e.exported)) return null;
        if (e.pathPermissions != null) {
            for (PathPermission pp : e.pathPermissions) {
                String r = pp.getReadPermission(), w = pp.getWritePermission();
                if (r != null && checkComponent(r, caller, owner, e.exported)) return null;
                if (w != null && checkComponent(w, caller, owner, e.exported)) return null;
            }
        }
        if (grants.anyForAuthority(caller, e.authorities)) return null;
        if (!e.exported) {
            return "Permission Denial: opening provider " + e.name + " from " + who
                    + " that is not exported from uid " + pkgs.uid(owner);
        }
        return "Permission Denial: opening provider " + e.name + " from " + who + " requires "
                + e.readPermission + " or " + e.writePermission;
    }

    /** The provider-wide answer of one operation: granted to every uri of it. */
    public static final int OP_ALLOWED = 0;
    /** Granted for this uri only (a path-permission or a URI grant decided). */
    public static final int OP_ALLOWED_URI = 1;
    public static final int OP_DENIED = 2;

    /** ContentProvider.Transport.enforceReadPermission / enforceWritePermission
     *  (enforceReadPermissionInner): {OP_*, the failure's tail ("requires X, or
     *  grantUriPermission()") when denied}. */
    public Object[] providerOp(String caller, String owner, Manifest.ProviderEntry e, Uri uri, boolean write) {
        if (caller == null || sameApp(caller, owner)) return new Object[] { OP_ALLOWED, null };
        String missing = null;
        if (e.exported) {
            String componentPerm = write ? e.writePermission : e.readPermission;
            if (componentPerm != null) {
                if (holds(caller, componentPerm)) return new Object[] { OP_ALLOWED, null };
                missing = componentPerm;
            }
            // An unprotected provider is open unless a <path-permission> for this path is denied.
            boolean allowDefault = componentPerm == null;
            if (e.pathPermissions != null) {
                String path = uri != null && uri.getPath() != null ? uri.getPath() : "";
                for (PathPermission pp : e.pathPermissions) {
                    String pathPerm = write ? pp.getWritePermission() : pp.getReadPermission();
                    if (pathPerm != null && pp.match(path)) {
                        if (holds(caller, pathPerm)) return new Object[] { OP_ALLOWED_URI, null };
                        allowDefault = false;
                        missing = pathPerm;
                    }
                }
            }
            // With path-permissions the answer depends on the path: it holds for this uri only.
            if (allowDefault) return new Object[] { e.pathPermissions != null ? OP_ALLOWED_URI : OP_ALLOWED, null };
        }
        if (uri != null && grants.check(caller, uri, write ? Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                : Intent.FLAG_GRANT_READ_URI_PERMISSION)) {
            return new Object[] { OP_ALLOWED_URI, null };
        }
        String tail = e.exported ? " requires " + missing + ", or grantUriPermission()"
                : " requires the provider be exported, or grantUriPermission()";
        return new Object[] { OP_DENIED, tail };
    }

    /** UriGrantsManagerService.checkHoldingPermissions: whether `pkg` reaches `uri` with `mode`
     *  (FLAG_GRANT_READ/WRITE_URI_PERMISSION bits) by the provider's own rules, no grant counted. */
    boolean holdsByProvider(String pkg, String owner, Manifest.ProviderEntry e, Uri uri, int mode) {
        if (pkg == null || sameApp(pkg, owner)) return true;
        if (!e.exported) return false;
        boolean readMet = (mode & Intent.FLAG_GRANT_READ_URI_PERMISSION) == 0;
        boolean writeMet = (mode & Intent.FLAG_GRANT_WRITE_URI_PERMISSION) == 0;
        boolean allowDefaultRead = e.readPermission == null, allowDefaultWrite = e.writePermission == null;
        if (!readMet && e.readPermission != null && holds(pkg, e.readPermission)) readMet = true;
        if (!writeMet && e.writePermission != null && holds(pkg, e.writePermission)) writeMet = true;
        if (e.pathPermissions != null) {
            String path = uri.getPath() != null ? uri.getPath() : "";
            for (PathPermission pp : e.pathPermissions) {
                if (!pp.match(path)) continue;
                if (!readMet && pp.getReadPermission() != null) {
                    if (holds(pkg, pp.getReadPermission())) readMet = true; else allowDefaultRead = false;
                }
                if (!writeMet && pp.getWritePermission() != null) {
                    if (holds(pkg, pp.getWritePermission())) writeMet = true; else allowDefaultWrite = false;
                }
            }
        }
        if (allowDefaultRead) readMet = true;
        if (allowDefaultWrite) writeMet = true;
        return readMet && writeMet;
    }

    /** UriGrantsManagerService.checkUriPermission(uri, uid, mode): the provider's own rules, then
     *  the grants. */
    public boolean uriAccess(String pkg, String owner, Manifest.ProviderEntry e, Uri uri, int mode) {
        return holdsByProvider(pkg, owner, e, uri, mode) || grants.check(pkg, uri, mode);
    }

    // ---- URI grants ------------------------------------------------------------------------

    /** Where a content uri's provider is: {owning package, its entry}, or null. */
    public interface ProviderLookup { Object[] find(String authority); }

    /** UriGrantsManagerService.checkGrantUriPermissionFromIntent + grantUriPermissionUncheckedFromIntent:
     *  the uris an intent going from `granter` to `target` grants (its data and its ClipData's), each
     *  checked and recorded. Returns the refusals (empty when every grant went through), for the log. */
    public List<String> grantFromIntent(String granter, String target, Intent intent, ProviderLookup lookup) {
        List<String> refused = new ArrayList<>();
        if (intent == null || target == null) return refused;
        int flags = intent.getFlags();
        int mode = flags & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        if (mode == 0) return refused;
        boolean prefix = (flags & Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) != 0;
        List<Uri> uris = new ArrayList<>();
        if (intent.getData() != null) uris.add(intent.getData());
        android.content.ClipData clip = intent.getClipData();
        if (clip != null) {
            for (int i = 0; i < clip.getItemCount(); i++) {
                Uri u = clip.getItemAt(i).getUri();
                if (u != null) uris.add(u);
            }
        }
        for (Uri u : uris) {
            String why = grant(granter, target, u, mode, prefix, lookup);
            if (why != null) refused.add(why);
        }
        return refused;
    }

    /** One grant (checkGrantUriPermission, then the grant): null when made or not needed, else why
     *  it was refused (AOSP's SecurityException message). */
    public String grant(String granter, String target, Uri uri, int mode, boolean prefix, ProviderLookup lookup) {
        mode &= Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
        if (uri == null || mode == 0 || !"content".equals(uri.getScheme()) || uri.getAuthority() == null) return null;
        Object[] hit = lookup.find(uri.getAuthority());
        if (hit == null) return null;   // no such provider: nothing to grant
        String owner = (String) hit[0];
        Manifest.ProviderEntry e = (Manifest.ProviderEntry) hit[1];
        // The target reaches it already by the provider's permissions: no grant needed.
        if (holdsByProvider(target, owner, e, uri, mode)) return null;
        // android:grantUriPermissions, narrowed to the <grant-uri-permission> paths when there are any
        // (which the parser also takes as grantUriPermissions, as AOSP's does).
        boolean allowed = e.grantUriPermissions;
        if (e.uriPermissionPatterns != null && e.uriPermissionPatterns.length > 0) {
            allowed = false;
            String path = uri.getPath() != null ? uri.getPath() : "";
            for (android.os.PatternMatcher pm : e.uriPermissionPatterns) {
                if (pm != null && pm.match(path)) { allowed = true; break; }
            }
        }
        if (!allowed) {
            return "Provider " + owner + "/" + e.name + " does not allow granting of Uri permissions (uri "
                    + uri + ")";
        }
        if (!holdsByProvider(granter, owner, e, uri, mode) && !grants.check(granter, uri, mode)) {
            return "UID " + uidOf(granter)
                    + " does not have permission to " + uri
                    + "; you could obtain access using ACTION_OPEN_DOCUMENT or related APIs";
        }
        grants.add(target, owner, uri, mode, prefix);
        return null;
    }

    /** The grants a device keeps in UriGrantsManagerService (in memory: non-persistable grants). */
    static final class UriGrants {
        static final class Grant {
            final String target, owner, uri, authority;
            final boolean prefix;
            int mode;
            Grant(String target, String owner, Uri uri, boolean prefix, int mode) {
                this.target = target; this.owner = owner; this.uri = uri.toString();
                this.authority = uri.getAuthority(); this.prefix = prefix; this.mode = mode;
            }
        }
        private final List<Grant> list = new ArrayList<>();

        synchronized void add(String target, String owner, Uri uri, int mode, boolean prefix) {
            for (Grant g : list) {
                if (g.target.equals(target) && g.prefix == prefix && g.uri.equals(uri.toString())) {
                    g.mode |= mode;
                    return;
                }
            }
            list.add(new Grant(target, owner, uri, prefix, mode));
        }

        /** Whether `target` holds every bit of `mode` on `uri`. */
        synchronized boolean check(String target, Uri uri, int mode) {
            if (target == null || uri == null) return false;
            String s = uri.toString();
            int have = 0;
            for (Grant g : list) {
                if (!g.target.equals(target)) continue;
                if (g.prefix ? s.startsWith(g.uri) : s.equals(g.uri)) have |= g.mode;
            }
            return (have & mode) == mode;
        }

        /** UriGrantsManagerService.checkAuthorityGrants: any grant under one of the authorities
         *  (';'-separated, as the manifest lists them). */
        synchronized boolean anyForAuthority(String target, String authorities) {
            if (target == null || authorities == null) return false;
            for (String a : authorities.split(";")) {
                for (Grant g : list) if (g.target.equals(target) && a.trim().equals(g.authority)) return true;
            }
            return false;
        }

        /** Context.revokeUriPermission([target,] uri, mode) by the provider's owner: every grant of
         *  the uri (and the uris under it), to `target` or, when null, to anyone. */
        synchronized int revoke(String owner, String target, Uri uri, int mode) {
            String s = uri.toString();
            int n = 0;
            for (java.util.Iterator<Grant> it = list.iterator(); it.hasNext(); ) {
                Grant g = it.next();
                if (!g.owner.equals(owner) || !g.uri.startsWith(s)) continue;
                if (target != null && !g.target.equals(target)) continue;
                g.mode &= ~mode;
                if (g.mode == 0) { it.remove(); n++; }
            }
            return n;
        }

        /** A package went: its grants and the grants to its providers. */
        synchronized void forget(String pkg) {
            list.removeIf(g -> g.target.equals(pkg) || g.owner.equals(pkg));
        }
    }

    static void log(String m) { System.err.println("[system] " + m); }
}
