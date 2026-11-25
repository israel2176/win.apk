// GmsTokensTest -- the Google account's master token held by its owner alone (GmsStore, the
// token service GmsTokenService and its client GmsTokens), on a plain JVM.
//
//   MACAPK_GMS_DIR=<empty scratch folder> java -cp <framework + loader jars + this> \
//       com.macapk.gms.GmsTokensTest <scratch folder> <repository root>
//
// DPAPI (windows/host/data_protection.h, winapk-data-protection-test) is replaced by a stand-in
// that also hides the text, so the files can be checked for the secrets in the clear. Both sides
// of the token service run in this process: the service is called as a local binder, and owns the
// account only for the time of each call, as its own process does. No network: every token asked
// of Google is for an account with no master token, which fails before any request.
//
// Checks:
//   migration    an older install's gms_state.properties with the master token and a token in
//                the clear: an app's process lists the account but cannot read the token; the
//                owner's first read moves both into tokens.dat, sealed, and the public file keeps
//                the account (signedIn) without them.
//   owner        masterToken, putAccount and removeAccount in the owner; refused in an app.
//   service      an app's peek / put / invalidate / scoped / app token through the service, the
//                reasons coming back as lastError(); another app's cached ID token and a token
//                minted for another package are refused.
//   broker       AccountStore.sealedAt moves the broker's plain accounts table into its sealed file.
//   sources      no app-side code reads the master token or the token files (a scan of the tree).
package com.macapk.gms;

import android.os.Parcel;
import android.os.RemoteException;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Properties;

public final class GmsTokensTest {
    static int failures;

    static void check(String what, boolean ok, String detail) {
        System.out.println("  [" + (ok ? "PASS" : "FAIL") + "] " + what + (detail.isEmpty() ? "" : ": " + detail));
        if (!ok) failures++;
    }

    static void check(String what, boolean ok) { check(what, ok, ""); }

    /** DPAPI's stand-in: a marker, then the bytes changed, so a file never shows the text. */
    static final class TestCodec implements com.macapk.loader.DataProtection.Codec {
        static final byte[] MAGIC = "SEALED1".getBytes(StandardCharsets.US_ASCII);
        @Override public byte[] seal(byte[] p) {
            byte[] out = new byte[MAGIC.length + p.length];
            System.arraycopy(MAGIC, 0, out, 0, MAGIC.length);
            for (int i = 0; i < p.length; i++) out[MAGIC.length + i] = (byte) (p[i] ^ 0x5a);
            return out;
        }
        @Override public byte[] unseal(byte[] s) throws IOException {
            if (s.length < MAGIC.length) throw new IOException("not sealed");
            for (int i = 0; i < MAGIC.length; i++) if (s[i] != MAGIC[i]) throw new IOException("not sealed");
            byte[] out = new byte[s.length - MAGIC.length];
            for (int i = 0; i < out.length; i++) out[i] = (byte) (s[MAGIC.length + i] ^ 0x5a);
            return out;
        }
    }

    /** The token service in this process: each call runs as the owner, as in its own process. */
    static final class LocalService extends GmsTokenService {
        @Override public boolean transact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            boolean was = GmsStore.owner();
            GmsStore.ownerForTest(true);
            try {
                return super.transact(code, data, reply, flags);
            } finally {
                GmsStore.ownerForTest(was);
            }
        }
    }

    static String text(File f) throws IOException {
        return f.isFile() ? new String(Files.readAllBytes(f.toPath()), StandardCharsets.ISO_8859_1) : "";
    }

    static boolean refused(Runnable r) {
        try {
            r.run();
            return false;
        } catch (SecurityException e) {
            return true;
        }
    }

    static final String A = "first@example.com", B = "second@example.com", NOBODY = "nobody@example.com";
    static final String MASTER_A = "aas_et/TEST-MASTER-FIRST", TOKEN_X = "ya29.TEST-SCOPED-X";
    static final String SCOPE_X = "oauth2:https://www.googleapis.com/auth/test.x";
    static final String APP = "com.example.signin", OTHER_APP = "com.example.other";
    static final String APP_SIG = "0123456789abcdef0123456789abcdef01234567";

    public static void main(String[] args) throws Exception {
        File scratch = new File(args[0]);
        File repo = new File(args[1]);
        File gms = GmsStore.dir();
        check("MACAPK_GMS_DIR is the scratch folder's gms", gms.getCanonicalPath().startsWith(scratch.getCanonicalPath()),
                gms.getPath());
        com.macapk.loader.DataProtection.setCodecForTest(new TestCodec());

        migration(gms);
        owner(gms);
        service();
        broker(scratch);
        sources(repo);

        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }

    static void migration(File gms) throws Exception {
        System.out.println("migration of an older install's plain tokens");
        gms.mkdirs();
        File pub = new File(gms, "gms_state.properties"), sec = new File(gms, "tokens.dat");
        Properties old = new Properties();
        old.setProperty("account." + A + ".masterToken", MASTER_A);
        old.setProperty("account." + A + ".data.accountId", "4242");
        old.setProperty("token." + A + "." + SCOPE_X + ".value", TOKEN_X);
        old.setProperty("token." + A + "." + SCOPE_X + ".expiry", Long.toString(System.currentTimeMillis() + 3_600_000L));
        old.setProperty("checkin.androidId", "1234");
        try (java.io.OutputStream o = Files.newOutputStream(pub.toPath())) { old.store(o, null); }

        // An app's process with no token service to be had.
        GmsStore.ownerForTest(false);
        GmsTokens.sServiceForTest = null;
        GmsStore.reload();
        check("an app lists the account of an older install", GmsStore.accountNames().equals(List.of(A)),
                GmsStore.accountNames().toString());
        check("an app may read the account's data", "4242".equals(GmsStore.accountData(A, "accountId")));
        check("an app may not read the master token", refused(() -> GmsStore.masterToken(A)));
        check("an app's token request without the service is no token", GmsStore.authToken(A, SCOPE_X) == null);
        check("and says why", String.valueOf(GoogleAuthClient.lastError()).contains("token service"),
                String.valueOf(GoogleAuthClient.lastError()));
        check("an app does not move the tokens itself", !sec.exists() && text(pub).contains("TEST-MASTER-FIRST"));

        // The service is there: the app's first read has the owner move the secrets first.
        GmsTokens.sServiceForTest = new LocalService();
        GmsStore.reload();
        List<String> names = GmsStore.accountNames();
        check("after the owner's move the account is still listed", names.equals(List.of(A)), names.toString());
        check("tokens.dat was written", sec.isFile());
        String p = text(pub), s = text(sec);
        check("gms_state.properties no longer holds the master token", !p.contains("TEST-MASTER-FIRST"));
        check("gms_state.properties no longer holds the scoped token", !p.contains("TEST-SCOPED-X"));
        check("gms_state.properties says the account is signed in", p.contains(".signedIn"));
        check("it keeps the rest", p.contains("androidId") && p.contains("accountId"));
        check("tokens.dat is sealed (no secret in the clear)", !s.contains("TEST-MASTER-FIRST") && !s.contains("TEST-SCOPED"));
        check("no leftover .tmp file", !new File(gms, "gms_state.properties.tmp").exists()
                && !new File(gms, "tokens.dat.tmp").exists());
        check("the app's cached token comes from the service", TOKEN_X.equals(GmsStore.authToken(A, SCOPE_X)));
        check("the app still may not read the master token", refused(() -> GmsStore.masterToken(A)));
    }

    static void owner(File gms) throws Exception {
        System.out.println("the owner");
        GmsStore.ownerForTest(true);
        GmsStore.reload();
        check("the owner reads the moved master token", MASTER_A.equals(GmsStore.masterToken(A)));
        GmsStore.putAccount(B, "aas_et/TEST-MASTER-SECOND");
        check("an account the owner adds is listed", GmsStore.accountNames().equals(List.of(A, B)),
                GmsStore.accountNames().toString());
        check("its master token is sealed, not in the public file",
                !text(new File(gms, "gms_state.properties")).contains("TEST-MASTER-SECOND")
                        && !text(new File(gms, "tokens.dat")).contains("TEST-MASTER-SECOND"));
        check("accounts.present is there", new File(gms, "accounts.present").exists());
        GmsStore.reload();
        check("it reads back", "aas_et/TEST-MASTER-SECOND".equals(GmsStore.masterToken(B)));
        GmsStore.removeAccount(B);
        GmsStore.reload();
        check("a removed account is gone", GmsStore.accountNames().equals(List.of(A)));
        check("with its master token", GmsStore.masterToken(B) == null);

        GmsStore.ownerForTest(false);
        check("an app may not add an account", refused(() -> GmsStore.putAccount(B, "x")));
        check("an app may not remove one", refused(() -> GmsStore.removeAccount(A)));
        check("an app may not list the tokens", refused(() -> GmsStore.scopesFor(A)));
        check("an app may not read a token raw", refused(() -> GmsStore.authTokenRaw(A, SCOPE_X)));
    }

    static void service() throws Exception {
        System.out.println("an app's requests through the token service");
        GmsStore.ownerForTest(false);
        GmsTokens.sServiceForTest = new LocalService();
        GmsTokenService.sIdentity = pid -> new String[] { APP, APP_SIG };

        GmsStore.putAuthToken(A, "oauth2:test.put", "ya29.TEST-PUT", 3600);
        check("a token an app caches is there for it", "ya29.TEST-PUT".equals(GmsStore.authToken(A, "oauth2:test.put")));
        GmsStore.invalidateAuthTokenValue("ya29.TEST-PUT");
        check("invalidated by value, it is gone", GmsStore.authToken(A, "oauth2:test.put") == null);
        GmsStore.putAuthToken(A, "oauth2:test.put", "ya29.TEST-PUT2", 0);
        GmsStore.invalidateAuthToken(A, "oauth2:test.put");
        check("invalidated by scope, it is gone", GmsStore.authToken(A, "oauth2:test.put") == null);
        GmsStore.putAuthToken(NOBODY, "oauth2:test.put", "ya29.TEST-NOBODY", 0);
        check("no token is cached for an account that is not signed in",
                GmsStore.authToken(NOBODY, "oauth2:test.put") == null);

        String own = "app:" + APP + ":" + APP_SIG + ":ep:audience:server:client_id:1";
        GmsStore.putAuthToken(A, own, "eyTEST.OWN", 3600);
        check("an app's own cached ID token is its", "eyTEST.OWN".equals(GmsStore.authToken(A, own)));
        GmsTokenService.sIdentity = pid -> new String[] { OTHER_APP, APP_SIG };
        check("another app may not read it", GmsStore.authToken(A, own) == null);
        check("and is told it was refused", String.valueOf(GoogleAuthClient.lastError()).contains("refused"),
                String.valueOf(GoogleAuthClient.lastError()));
        GmsTokenService.sIdentity = pid -> new String[] { APP, APP_SIG };

        // Tokens Google would mint: for an account with no master token, so no request is made.
        check("a scoped token for an account that is not signed in is none",
                GoogleAuthClient.fetchScopedToken(NOBODY, SCOPE_X) == null);
        check("with the service's reason", String.valueOf(GoogleAuthClient.lastError()).contains("no master token"),
                String.valueOf(GoogleAuthClient.lastError()));
        Map<String, String> r = GoogleAuthClient.fetchAppToken(NOBODY, "audience:server:client_id:1", APP, APP_SIG,
                true, true, true, null, Collections.singletonMap("oauth2_prompt", "auto"), 0, 0);
        check("an app's own token request reaches the token exchange", r == null
                && String.valueOf(GoogleAuthClient.lastError()).contains("no master token"),
                String.valueOf(GoogleAuthClient.lastError()));
        r = GoogleAuthClient.fetchAppToken(NOBODY, "audience:server:client_id:1", OTHER_APP, APP_SIG,
                true, true, true, null);
        check("a token minted for another package is refused", r == null
                && String.valueOf(GoogleAuthClient.lastError()).contains("refused"),
                String.valueOf(GoogleAuthClient.lastError()));
        r = GoogleAuthClient.fetchAppToken(NOBODY, "audience:server:client_id:1", APP,
                "ffffffffffffffffffffffffffffffffffffffff", true, true, true, null);
        check("a certificate that is not the app's own is refused", r == null
                && String.valueOf(GoogleAuthClient.lastError()).contains("refused"),
                String.valueOf(GoogleAuthClient.lastError()));
        check("an account id is asked of the service", GoogleAuthClient.ensureAccountId(NOBODY) == null);
        check("the account's own data answers without it", "4242".equals(GoogleAuthClient.ensureAccountId(A)));

        // The account's own side (the broker) names any app: it is trusted as the owner is.
        GmsTokenService.sIdentity = pid -> null;
        r = GoogleAuthClient.fetchAppToken(NOBODY, "audience:server:client_id:1", OTHER_APP, APP_SIG,
                true, true, true, null);
        check("the account's own side may ask for any app", r == null
                && String.valueOf(GoogleAuthClient.lastError()).contains("no master token"),
                String.valueOf(GoogleAuthClient.lastError()));

        // The parcel's map both ways.
        Parcel p = Parcel.obtain();
        Map<String, String> m = new java.util.LinkedHashMap<>();
        m.put("Auth", "x=y");
        m.put("Expiry", "1");
        GmsTokenService.writeMap(p, m);
        GmsTokenService.writeMap(p, null);
        p.setDataPosition(0);
        check("a reply's map reads back", m.equals(GmsTokenService.readMap(p)) && GmsTokenService.readMap(p) == null);
        p.recycle();
    }

    static void broker(File scratch) throws Exception {
        System.out.println("the broker's accounts table");
        File dir = new File(scratch, "system_ce");
        dir.mkdirs();
        File plain = new File(dir, "accounts_ce.properties");
        Properties old = new Properties();
        old.setProperty("n", "1");
        old.setProperty("a.0.name", A);
        old.setProperty("a.0.type", "com.google");
        old.setProperty("a.0.pw", MASTER_A);
        try (java.io.OutputStream o = Files.newOutputStream(plain.toPath())) { old.store(o, null); }
        com.macapk.accounts.AccountStore store = com.macapk.accounts.AccountStore.sealedAt(plain);
        check("the plain table's row reads", MASTER_A.equals(store.password(A, "com.google")));
        File sealed = new File(dir, "accounts_ce.dat");
        check("it is moved into accounts_ce.dat", sealed.isFile() && !plain.exists());
        check("which is sealed", !text(sealed).contains("TEST-MASTER-FIRST"));
        store.add(B, "com.google", "aas_et/TEST-MASTER-SECOND", null);
        check("a new row is sealed too", !text(sealed).contains("TEST-MASTER-SECOND"));
    }

    /** No app-side code reads the master token or the token files: the reads are in the owner's
     *  files only (and GoogleAuthClient's are behind GmsStore.owner()). */
    static void sources(File repo) throws Exception {
        System.out.println("the sources");
        List<String> allowed = List.of(
                "runtime/java/com/macapk/gms/GmsStore.java",
                "runtime/java/com/macapk/gms/GoogleAuthClient.java",
                "runtime/java/com/macapk/gms/GmsCli.java",
                "runtime/java/com/macapk/gms/GmsTokenService.java",
                "runtime/java/com/macapk/loader/AccountsService.java");
        String[] reads = { "GmsStore.masterToken(", "GmsStore.scopesFor(", "GmsStore.authTokenRaw(",
                "GmsStore.putAccount(", "GmsStore.becomeOwner(", "\"tokens.dat\"", "\"gms_state.properties\"" };
        List<String> found = new ArrayList<>();
        for (String root : new String[] { "runtime/java", "windows/java" }) {
            File r = new File(repo, root);
            check(root + " is there", r.isDirectory());
            try (java.util.stream.Stream<java.nio.file.Path> walk = Files.walk(r.toPath())) {
                for (java.nio.file.Path f : (Iterable<java.nio.file.Path>) walk::iterator) {
                    if (!f.toString().endsWith(".java")) continue;
                    String rel = repo.toPath().relativize(f).toString().replace('\\', '/');
                    if (allowed.contains(rel)) continue;
                    String src = new String(Files.readAllBytes(f), StandardCharsets.UTF_8);
                    for (String s : reads) if (src.contains(s)) found.add(rel + ": " + s);
                }
            }
        }
        // SystemServer only declares the broker the owner.
        String sys = new String(Files.readAllBytes(new File(repo, "runtime/java/com/macapk/loader/SystemServer.java")
                .toPath()), StandardCharsets.UTF_8);
        found.removeIf(f -> f.equals("runtime/java/com/macapk/loader/SystemServer.java: GmsStore.becomeOwner("));
        check("the broker is the account's owner", sys.contains("GmsStore.becomeOwner()"));
        check("no other code reads the master token or the token files", found.isEmpty(), found.toString());
        String auth = new String(Files.readAllBytes(new File(repo, "runtime/java/com/macapk/gms/GoogleAuthClient.java")
                .toPath()), StandardCharsets.UTF_8);
        int guarded = 0;
        for (String m : new String[] { "GmsTokens.signIn(", "GmsTokens.accountId(", "GmsTokens.scoped(",
                "GmsTokens.appToken(" }) {
            if (auth.contains("if (!GmsStore.owner()) return " + m) || auth.contains(m)) guarded++;
        }
        check("GoogleAuthClient sends an app's four master-token calls to the service", guarded == 4);
    }
}
