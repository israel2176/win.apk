// com.macapk.gms.GmsTokenService -- the Google account's token service: the one process
// (`winapk-runtime --gms token-service`) that answers an app's token requests from the
// master token, so that no app's process reads it.
//
// On Android an app never holds the account's master token: AccountManager.getAuthToken
// reaches AccountManagerService, which asks the account's authenticator (Google Play
// services) for a token for the app, and GoogleAuthUtil / Play services' sign-in APIs mint
// tokens for the app the same way, from Play services' process. Win.apk's Google layer runs
// in each app's process (com.macapk.gms), so its token requests go here instead, over
// Binder (named pipes, windows/host/binder_win.cpp), and come back as scoped tokens.
//
// WHO IS ASKING. The transport gives the caller's pid from Windows, not from the message.
// An app's runtime was started as `winapk-runtime --apk <its installed APK>`: the package
// and the signing certificate are read from that file, as PackageManager has them on a
// device. A token minted for an app (Sign in with Google: an ID token or a server auth
// code, Google's `app` + `client_sig`) must name the caller's own package and certificate;
// a cached token keyed by an app (`app:<package>:...`) is the caller's own only. A runtime
// started without --apk is the account's own side (the system broker, `--gms`), trusted as
// GmsStore's owner is.
//
// LIFE. The first app that needs a token starts the service (GmsTokens) and waits for its
// name; a second one started at the same time finds the name taken and exits. It exits
// by itself once no request came for IDLE_MS. The system broker and the launcher's `--gms`
// verbs do not use it: they own the account and read it themselves.
//
// LIMITS (stated): every app runs with the Windows user's rights and its native code is
// not confined, so a process that sets out to can still read the user's DPAPI secrets or
// start a runtime naming another app's APK. What this gives is Android's shape: the master
// token is out of every app's Java code paths and files, and a well-behaved app's request
// carries its own identity.
package com.macapk.gms;

import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

public class GmsTokenService extends Binder {
    /** The name the service is published under (binder_win.cpp's published pipe). */
    public static final String NAME = "winapk.gms.tokens";
    static final String DESCRIPTOR = "com.winapk.gms.ITokenService";

    static final int TX_MIGRATE = IBinder.FIRST_CALL_TRANSACTION;
    static final int TX_PEEK = TX_MIGRATE + 1;
    static final int TX_PUT = TX_MIGRATE + 2;
    static final int TX_INVALIDATE = TX_MIGRATE + 3;
    static final int TX_INVALIDATE_VALUE = TX_MIGRATE + 4;
    static final int TX_SCOPED = TX_MIGRATE + 5;
    static final int TX_APP_TOKEN = TX_MIGRATE + 6;
    static final int TX_ACCOUNT_ID = TX_MIGRATE + 7;
    static final int TX_SIGN_IN = TX_MIGRATE + 8;

    /** How long the service waits for a request before it exits. */
    static final long IDLE_MS = 5 * 60_000L;

    private volatile long lastCallMs = System.currentTimeMillis();
    private final java.util.concurrent.atomic.AtomicInteger inFlight = new java.util.concurrent.atomic.AtomicInteger();

    public GmsTokenService() { super(DESCRIPTOR); }

    /** `--gms token-service`: serve until idle. 0 when it served (or another copy already
     *  does), 1 when it could not be published. */
    static int serve() {
        GmsStore.becomeOwner();
        GmsTokenService s = new GmsTokenService();
        // Callers own no Android uid here; the pid is what identifies them.
        android.os.MacApkBinderTransport.setUidResolver(pid -> 0);
        try {
            android.os.MacApkBinderTransport.publish(NAME, s);
        } catch (Throwable t) {
            log("not serving: " + t.getMessage() + " (another token service runs?)");
            return 0;
        }
        log("serving (pid " + ProcessHandle.current().pid() + ")");
        GmsStore.accountNames();   // an older install's plain tokens are sealed now, not at the first call
        while (true) {
            try { Thread.sleep(5_000); } catch (InterruptedException e) { return 0; }
            if (s.inFlight.get() == 0 && System.currentTimeMillis() - s.lastCallMs > IDLE_MS) {
                log("idle, exiting");
                return 0;
            }
        }
    }

    // ---- who is asking -------------------------------------------------------------

    /** {package, certificate SHA-1 (lower-case hex, or null when the APK's signature does
     *  not verify)} of the calling process, or null for the account's own side (a runtime
     *  started without --apk, or this process). Tests replace it. */
    static volatile java.util.function.IntFunction<String[]> sIdentity = GmsTokenService::identityOfPid;

    private static final Map<String, String[]> sApkIdentity = new ConcurrentHashMap<>();

    static String[] identityOfPid(int pid) {
        if (pid == ProcessHandle.current().pid()) return null;
        String apk = com.macapk.loader.WinProcesses.apkArgument(pid);
        if (apk == null) return null;
        java.io.File f = new java.io.File(apk);
        String key = f.getAbsolutePath() + "\n" + f.lastModified() + "\n" + f.length();
        String[] id = sApkIdentity.get(key);
        if (id != null) return id;
        String pkg;
        try {
            pkg = com.macapk.loader.Manifest.parse(new java.io.File(""), apk).packageName;
        } catch (Throwable t) {
            throw new SecurityException("pid " + pid + ": its app " + apk + " cannot be read: " + t);
        }
        if (pkg == null || pkg.isEmpty()) throw new SecurityException("pid " + pid + ": no package in " + apk);
        id = new String[] { pkg, certSha1(com.macapk.loader.ApkVerification.of(apk)) };
        sApkIdentity.put(key, id);
        return id;
    }

    /** PackageInfo.signatures[0] as AOSP reports it (PackageInfoUtils, GET_SIGNATURES): the
     *  oldest certificate of a rotation lineage, else the signer. Its SHA-1, as Google's
     *  client_sig wants it. */
    static String certSha1(com.macapk.loader.ApkVerification.Result r) {
        if (r == null || !r.verified || r.signers.isEmpty()) return null;
        byte[] der = r.past != null && !r.past.isEmpty() ? r.past.get(0) : r.signers.get(0);
        try {
            byte[] d = java.security.MessageDigest.getInstance("SHA-1").digest(der);
            StringBuilder sb = new StringBuilder(40);
            for (byte b : d) sb.append(String.format("%02x", b & 0xff));
            return sb.toString();
        } catch (Exception e) {
            return null;
        }
    }

    /** The caller's identity; null for the account's own side. */
    private static String[] caller() {
        return sIdentity.apply(Binder.getCallingPid());
    }

    /** A cached key of the form app:<package>:... belongs to that app alone. */
    private static void checkKey(String[] who, String key) {
        if (who == null) return;
        if (key == null || !key.startsWith("app:" + who[0] + ":")) {
            throw new SecurityException(who[0] + " may not use another app's cached token");
        }
    }

    private static void enforceSystemOrGms(String[] who, String op) {
        if (who != null && !"com.google.android.gms".equals(who[0])) {
            throw new SecurityException(who[0] + " is not permitted to call " + op);
        }
    }

    // ---- transactions --------------------------------------------------------------

    @Override
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
        if (code < TX_MIGRATE || code > TX_SIGN_IN) return super.onTransact(code, data, reply, flags);
        data.enforceInterface(DESCRIPTOR);
        inFlight.incrementAndGet();
        lastCallMs = System.currentTimeMillis();
        try {
            String[] who = caller();
            switch (code) {
                case TX_MIGRATE: {
                    GmsStore.accountNames();   // loading the file as the owner moves its secrets
                    reply.writeNoException();
                    return true;
                }
                case TX_PEEK: {
                    String account = data.readString(), key = data.readString();
                    checkKey(who, key);
                    String t = GmsStore.authToken(account, key);
                    reply.writeNoException();
                    reply.writeString(t);
                    return true;
                }
                case TX_PUT: {
                    String account = data.readString(), key = data.readString(), token = data.readString();
                    long expirySeconds = data.readLong();
                    checkKey(who, key);
                    // A token is cached for a signed-in account only: no app fills the
                    // store with rows for accounts that are not there.
                    if (GmsStore.hasAccount(account)) GmsStore.putAuthToken(account, key, token, expirySeconds);
                    reply.writeNoException();
                    return true;
                }
                case TX_INVALIDATE: {
                    String account = data.readString(), key = data.readString();
                    checkKey(who, key);
                    GmsStore.invalidateAuthToken(account, key);
                    reply.writeNoException();
                    return true;
                }
                case TX_INVALIDATE_VALUE: {
                    enforceSystemOrGms(who, "TX_INVALIDATE_VALUE");
                    GmsStore.invalidateAuthTokenValue(data.readString());
                    reply.writeNoException();
                    return true;
                }
                case TX_SCOPED: {
                    enforceSystemOrGms(who, "TX_SCOPED");
                    String account = data.readString(), scope = data.readString();
                    String t = GoogleAuthClient.fetchScopedToken(account, scope);
                    reply.writeNoException();
                    reply.writeString(t);
                    reply.writeString(t == null ? GoogleAuthClient.lastError() : null);
                    return true;
                }
                case TX_APP_TOKEN: {
                    String account = data.readString(), service = data.readString();
                    String appPkg = data.readString(), appSig = data.readString();
                    boolean email = data.readInt() != 0, profile = data.readInt() != 0, consented = data.readInt() != 0;
                    String tro = data.readString();
                    Map<String, String> extra = readMap(data);
                    int connectMs = data.readInt(), readMs = data.readInt();
                    if (who != null) {
                        // Google mints this token for the app it names: only the app itself
                        // may name it, with its own certificate (GMS asks PackageManager).
                        if (!who[0].equals(appPkg)) {
                            throw new SecurityException(who[0] + " asked for a token minted for " + appPkg);
                        }
                        if (who[1] == null || !who[1].equalsIgnoreCase(appSig)) {
                            throw new SecurityException(who[0] + " named a certificate that is not its own");
                        }
                    }
                    Map<String, String> r = GoogleAuthClient.fetchAppToken(account, service, appPkg, appSig,
                            email, profile, consented, tro, extra, connectMs, readMs);
                    reply.writeNoException();
                    writeMap(reply, r);
                    reply.writeString(r == null ? GoogleAuthClient.lastError() : null);
                    return true;
                }
                case TX_ACCOUNT_ID: {
                    String id = GoogleAuthClient.ensureAccountId(data.readString());
                    reply.writeNoException();
                    reply.writeString(id);
                    return true;
                }
                case TX_SIGN_IN: {
                    enforceSystemOrGms(who, "TX_SIGN_IN");
                    String email = GoogleAuthClient.exchangeOAuthToken(data.readString());
                    reply.writeNoException();
                    reply.writeString(email);
                    reply.writeString(email == null ? GoogleAuthClient.lastError() : null);
                    if (email != null) log("signed in " + GoogleAuthenticator.redact(email)
                            + " for " + (who != null ? who[0] : "the account's own side"));
                    return true;
                }
                default:
                    return false;
            }
        } catch (SecurityException e) {
            log("REFUSED: " + e.getMessage());
            reply.writeException(e);
            return true;
        } finally {
            lastCallMs = System.currentTimeMillis();
            inFlight.decrementAndGet();
        }
    }

    /** A map as (count, then key and value), -1 for null. */
    static void writeMap(Parcel p, Map<String, String> m) {
        if (m == null) { p.writeInt(-1); return; }
        p.writeInt(m.size());
        for (Map.Entry<String, String> e : m.entrySet()) { p.writeString(e.getKey()); p.writeString(e.getValue()); }
    }

    static Map<String, String> readMap(Parcel p) {
        int n = p.readInt();
        if (n < 0) return null;
        Map<String, String> m = new LinkedHashMap<>();
        for (int i = 0; i < n; i++) m.put(p.readString(), p.readString());
        return m;
    }

    static void log(String m) {
        try { com.macapk.loader.NativeLog.i("Win.apk.GMS", "tokens: " + m); }
        catch (Throwable t) { System.err.println("[tokens] " + m); }
    }
}
