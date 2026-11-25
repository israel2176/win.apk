// com.macapk.gms.GmsTokens -- an app's side of the Google account's token service
// (GmsTokenService): what AccountManager's binder to AccountManagerService, and Play
// services' token calls, are on a device. GmsStore and GoogleAuthClient send here every
// request that needs the master token or the token cache when this process is not the
// account's owner (GmsStore.owner()), which is every app's process.
//
// The first request starts `winapk-runtime --gms token-service` when nothing answers
// the name (as WindowsBroker starts the system broker) and waits for it; a service that
// exited while idle is started again on the next request. Its log is logs\gms-tokens.log.
// Where there is no service to be had (no Windows host), a request fails as a token
// Google refused would: null, with the reason in GoogleAuthClient.lastError().
package com.macapk.gms;

import android.os.IBinder;
import android.os.Parcel;

import java.io.File;
import java.util.List;
import java.util.Map;

public final class GmsTokens {
    private GmsTokens() {}

    /** How long a request waits for a service it started: a JVM boot. */
    static final long START_WAIT_MS = 30_000L;

    private static volatile IBinder sService;
    /** A test's service in this process; null in the runtime. */
    static volatile IBinder sServiceForTest;

    private static IBinder lookUp() {
        IBinder t = sServiceForTest;
        if (t != null) return t;
        try { return android.os.MacApkBinderTransport.lookUp(GmsTokenService.NAME); }
        catch (Throwable e) { return null; }
    }

    /** The service, started when nothing answers its name. Null when there is none. */
    static synchronized IBinder service() {
        IBinder s = sService;
        if (s != null && s.isBinderAlive() && sServiceForTest == null) return s;
        s = lookUp();
        if (s == null && com.macapk.loader.WinProcesses.available()) {
            start();
            long deadline = System.currentTimeMillis() + START_WAIT_MS;
            while (s == null && System.currentTimeMillis() < deadline) {
                try { Thread.sleep(200); } catch (InterruptedException e) { break; }
                s = lookUp();
            }
        }
        sService = s;
        return s;
    }

    private static void start() {
        try {
            List<String> cmd = com.macapk.loader.WindowsBroker.runtimeCommand();
            cmd.add("--gms");
            cmd.add("token-service");
            ProcessBuilder pb = new ProcessBuilder(cmd);
            // The service gets no app's identity or window from this process.
            for (String k : new String[] { "MACAPK_SERVICE_HOST", "MACAPK_PROCESS_NAME", "MACAPK_SECONDARY_PROCESS",
                    "MACAPK_APK_PATH", "WINAPK_READY_EVENT", "WINAPK_START_WINDOW", "WINAPK_EMBED_PARENT",
                    "WINAPK_HEADLESS_EXIT_AFTER_FRAMES", "WINAPK_HEADLESS_FRAME" }) {
                pb.environment().remove(k);
            }
            pb.redirectErrorStream(true);
            String data = System.getProperty("winapk.data");
            if (data != null && !data.isEmpty()) {
                File logs = new File(data, "logs");
                logs.mkdirs();
                pb.redirectOutput(ProcessBuilder.Redirect.appendTo(new File(logs, "gms-tokens.log")));
            } else {
                pb.redirectOutput(ProcessBuilder.Redirect.DISCARD);
            }
            Process p = pb.start();
            GmsTokenService.log("started the token service (pid " + p.pid() + ")");
        } catch (Throwable t) {
            GmsTokenService.log("could not start the token service: " + t);
        }
    }

    interface Writer { void write(Parcel p); }
    interface Reader<T> { T read(Parcel p); }

    /** One call; a service that died meanwhile (it exits when idle) is started again
     *  and asked once more. A refusal (SecurityException) is the answer, not retried. */
    private static <T> T call(int code, Writer args, Reader<T> read, T none) {
        for (int attempt = 0; attempt < 2; attempt++) {
            IBinder s = service();
            if (s == null) {
                GoogleAuthClient.setLastError("the Google account's token service is not running");
                return none;
            }
            Parcel d = Parcel.obtain(), r = Parcel.obtain();
            try {
                d.writeInterfaceToken(GmsTokenService.DESCRIPTOR);
                if (args != null) args.write(d);
                try {
                    s.transact(code, d, r, 0);
                } catch (android.os.RemoteException | RuntimeException e) {
                    // The service is gone (it exits when idle): start it and ask again.
                    synchronized (GmsTokens.class) { sService = null; }
                    GoogleAuthClient.setLastError("the Google account's token service failed: " + e);
                    continue;
                }
                // What the service threw is its answer, not asked again (a sign-in's
                // oauth_token works once).
                r.readException();
                return read == null ? none : read.read(r);
            } catch (SecurityException e) {
                GmsTokenService.log("refused: " + e.getMessage());
                GoogleAuthClient.setLastError("refused by the token service: " + e.getMessage());
                return none;
            } catch (RuntimeException e) {
                GoogleAuthClient.setLastError("the Google account's token service failed: " + e);
                return none;
            } finally {
                d.recycle();
                r.recycle();
            }
        }
        return none;
    }

    /** Have the owner move an older install's plain secrets out of gms_state.properties. */
    static void migrate() {
        call(GmsTokenService.TX_MIGRATE, null, null, null);
    }

    static String peek(String account, String key) {
        return call(GmsTokenService.TX_PEEK, p -> { p.writeString(account); p.writeString(key); },
                Parcel::readString, null);
    }

    static void put(String account, String key, String token, long expirySeconds) {
        call(GmsTokenService.TX_PUT, p -> {
            p.writeString(account); p.writeString(key); p.writeString(token); p.writeLong(expirySeconds);
        }, null, null);
    }

    static void invalidate(String account, String key) {
        call(GmsTokenService.TX_INVALIDATE, p -> { p.writeString(account); p.writeString(key); }, null, null);
    }

    static void invalidateValue(String token) {
        call(GmsTokenService.TX_INVALIDATE_VALUE, p -> p.writeString(token), null, null);
    }

    /** GoogleAuthClient.fetchScopedToken, in the service. */
    static String scoped(String account, String scope) {
        return call(GmsTokenService.TX_SCOPED, p -> { p.writeString(account); p.writeString(scope); },
                GmsTokens::tokenOrError, null);
    }

    /** GoogleAuthClient.fetchAppToken, in the service, for this app (the service checks
     *  that appPkg and appSig are this process's own). */
    static Map<String, String> appToken(String account, String service, String appPkg, String appSig,
            boolean includeEmail, boolean includeProfile, boolean consented, String tro,
            Map<String, String> extra, int connectMs, int readMs) {
        return call(GmsTokenService.TX_APP_TOKEN, p -> {
            p.writeString(account); p.writeString(service); p.writeString(appPkg); p.writeString(appSig);
            p.writeInt(includeEmail ? 1 : 0); p.writeInt(includeProfile ? 1 : 0); p.writeInt(consented ? 1 : 0);
            p.writeString(tro);
            GmsTokenService.writeMap(p, extra);
            p.writeInt(connectMs); p.writeInt(readMs);
        }, r -> {
            Map<String, String> m = GmsTokenService.readMap(r);
            String err = r.readString();
            if (m == null) GoogleAuthClient.setLastError(err);
            return m;
        }, null);
    }

    /** GoogleAuthClient.ensureAccountId, in the service. */
    static String accountId(String account) {
        return call(GmsTokenService.TX_ACCOUNT_ID, p -> p.writeString(account), Parcel::readString, null);
    }

    /** GoogleAuthClient.exchangeOAuthToken, in the service: the sign-in window's answer
     *  becomes the account's master token there, and only the e-mail comes back. */
    static String signIn(String oauthToken) {
        return call(GmsTokenService.TX_SIGN_IN, p -> p.writeString(oauthToken), GmsTokens::tokenOrError, null);
    }

    /** (value, error) -> the value, with the error as lastError() when there is none. */
    private static String tokenOrError(Parcel r) {
        String v = r.readString(), err = r.readString();
        if (v == null) GoogleAuthClient.setLastError(err);
        return v;
    }
}
