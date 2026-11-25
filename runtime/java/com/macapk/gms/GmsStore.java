// com.macapk.gms.GmsStore -- the persisted state behind Mac.apk's GMS layer:
// the signed-in Google account(s), the master token, cached scoped auth tokens,
// and the checkin identity (android_id / security_token).
//
// WHAT LIVES HERE AND WHY IT IS ONE CLASS. Every GMS vertical (licensing, FCM,
// any getAuthToken consumer) needs the same three things -- an account, a token
// for a scope, and the device's checkin id. Splitting them across three stores
// would mean three file formats, three lock disciplines and three chances to
// write a token to a world-readable file. So: one store, one file, one lock.
//
// ---- WHERE THE SECRETS GO, AND WHO READS THEM (Win.apk) --------------------
// Two files under <data>\gms\ (MACAPK_GMS_DIR):
//   gms_state.properties  what any process may know: which accounts are signed in
//                         (account.<email>.signedIn), their non-secret data, the
//                         checkin identity, the FCM registration record.
//   tokens.dat            the secrets: each account's MASTER TOKEN and the cached
//                         scoped tokens, as a Properties text sealed with DPAPI for
//                         the Windows user (com.macapk.loader.DataProtection).
//
// ONLY THE ACCOUNT'S OWNER OPENS tokens.dat. On Android the master token is held by
// the account authenticator (Google Play services) alone; an app asks
// AccountManager.getAuthToken and gets a scoped token, never the master token. Here
// the owner side is: the launcher's `--gms` verbs (Settings > Google account and its
// sign-in window), the token service (`--gms token-service`, GmsTokenService) and the
// system broker (its account service gives the master token to Google's own Play
// services as the account's password, as AccountManagerService does). Such a process
// calls becomeOwner() first. Every other process -- every app's -- gets its tokens
// from the token service over Binder (GmsTokens), and masterToken() refuses it.
//
// MIGRATION. An older Win.apk kept the master token and the tokens in plain text in
// gms_state.properties. The first owner process that reads it moves them into
// tokens.dat (sealed), then rewrites gms_state.properties without them. An app's
// process that finds no tokens.dat beside an existing gms_state.properties asks the
// token service to do that before it reads the file.
//
// WHAT THIS DOES NOT DO: every app runs with the Windows user's rights and its native
// code is not confined, so code that sets out to can still read the user's DPAPI
// secrets. This keeps the token out of every app's Java code paths and out of plain
// files; isolating apps from each other needs a sandbox (future work).
//
// FORMAT: java.util.Properties. Flat, JDK-native, and its store()/load() pair
// escapes '=' ':' and whitespace in BOTH keys and values -- which matters,
// because a scope key is literally "oauth2:https://www.googleapis.com/auth/..."
// and a token is arbitrary base64url with '=' padding. Hand-rolling a format
// here would be re-solving an escaping problem the JDK already solved.
package com.macapk.gms;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.attribute.PosixFilePermission;
import java.util.ArrayList;
import java.util.EnumSet;
import java.util.List;
import java.util.Properties;
import java.util.Set;

public final class GmsStore {

    private static final String K_ANDROID_ID     = "checkin.androidId";
    private static final String K_SECURITY_TOKEN = "checkin.securityToken";
    private static final String K_CHECKIN_MS     = "checkin.lastMs";
    private static final String K_CHECKIN_VER    = "checkin.deviceDataVersionInfo";
    /** `gcm.registered.<pkg>` -> first-registration timestamp. See hasEverRegistered. */
    private static final String K_REG_PREFIX     = "gcm.registered.";
    private static final String P_ACCOUNT        = "account.";       // account.<email>.masterToken
    private static final String P_TOKEN          = "token.";         // token.<email>.<scope>.value / .expiry

    private static final String S_SIGNED_IN      = ".signedIn";      // account.<email>.signedIn (public)
    private static final String S_MASTER         = ".masterToken";   // account.<email>.masterToken (sealed)

    private static final Object LOCK = new Object();
    private static Properties sProps;      // null until first load
    private static Properties sSecrets;    // owner only; null until first load

    private GmsStore() {}

    // ===================================================================
    // Who may read the secrets
    // ===================================================================

    private static volatile boolean sOwner;

    /** This process holds the account (see the header): the launcher's `--gms` verbs,
     *  the token service and the system broker. Called once, before any GmsStore use. */
    public static void becomeOwner() { sOwner = true; }

    /** Whether this process reads the secrets itself; otherwise it asks the token service. */
    public static boolean owner() { return sOwner; }

    /** GmsTokensTest plays both sides in one process. */
    static void ownerForTest(boolean on) { sOwner = on; }

    private static void requireOwner(String what) {
        if (!sOwner) {
            throw new SecurityException(what + ": only the account's owner holds the master token"
                    + " (an app asks the token service for a scoped token)");
        }
    }

    // ===================================================================
    // Location
    // ===================================================================

    /** The GMS state directory. Sibling of installed_apps.json on purpose: one
     *  place for "things the Mac.apk installation knows", so a user clearing
     *  Mac.apk's data clears the Google session too. */
    public static File dir() {
        String override = System.getenv("MACAPK_GMS_DIR");
        if (override != null && !override.isEmpty()) return new File(override);
        return new File(System.getProperty("user.home"),
                "Library/Application Support/Mac.apk/gms");
    }

    private static File file() { return new File(dir(), "gms_state.properties"); }

    /** The sealed secrets (owner only). */
    static File secretsFile() { return new File(dir(), "tokens.dat"); }

    // ===================================================================
    // Load / save
    // ===================================================================

    /** The file's (mtime, size) when sProps was read or last written by us. */
    private static long sStampTime = -1, sStampSize = -1;
    /** Whether sProps came from a complete read of an existing file. */
    private static boolean sReadClean;

    private static Properties props() {
        synchronized (LOCK) {
            File f = file();
            // Several processes share this file (every app, the system broker,
            // `--gms` from the launcher's Settings). A copy read once and kept
            // for the life of the process misses a sign-in or sign-out made
            // elsewhere, and its next save() writes the stale copy back over
            // it. So a cached copy is good only while the file is unchanged.
            if (sProps != null && f.lastModified() == sStampTime && f.length() == sStampSize) return sProps;
            // An app's process does not read a file that may still hold an older
            // Win.apk's plain master token: the owner moves it out first.
            if (!sOwner && f.isFile() && !secretsFile().exists()) GmsTokens.migrate();
            Properties p = new Properties();
            boolean clean = false;
            if (f.isFile()) {
                try (InputStream in = Files.newInputStream(f.toPath())) {
                    p.load(in);
                    clean = true;
                } catch (Throwable t) {
                    // A corrupt store must not take the process down, and it must
                    // not silently look like "signed out" either -- that would send
                    // a caller to fabricate a fresh sign-in over a file that still
                    // holds a good token. Loud, then empty.
                    warn("could not read " + f + ": " + t);
                }
            }
            sProps = p;
            sReadClean = clean;
            sStampTime = f.lastModified();
            sStampSize = f.length();
            if (sOwner && clean && (hasSecretKeys(p) || !secretsFile().exists())) migrate(p);
            return p;
        }
    }

    /** Whether `p` (gms_state.properties) still holds what belongs in tokens.dat. */
    private static boolean hasSecretKeys(Properties p) {
        for (String key : p.stringPropertyNames()) if (isSecretKey(key)) return true;
        return false;
    }

    private static boolean isSecretKey(String key) {
        return (key.startsWith(P_ACCOUNT) && key.endsWith(S_MASTER)) || key.startsWith(P_TOKEN);
    }

    /** An older Win.apk's plain secrets -> tokens.dat, sealed; then the public file
     *  without them. tokens.dat is written first: a crash between the two leaves the
     *  secrets in both, and the next owner run finishes the move. Writes tokens.dat
     *  even with nothing to move, so an app's process knows there is nothing left to
     *  move. The caller holds LOCK. */
    private static void migrate(Properties pub) {
        Properties sec = secrets();
        if (sec == null) return;   // unreadable: leave the plain copy rather than lose it
        int moved = 0;
        for (String key : new ArrayList<>(pub.stringPropertyNames())) {
            if (!isSecretKey(key)) continue;
            String v = pub.getProperty(key);
            if (sec.getProperty(key) == null) sec.setProperty(key, v);
            if (key.startsWith(P_ACCOUNT) && key.endsWith(S_MASTER)) {
                pub.setProperty(key.substring(0, key.length() - S_MASTER.length()) + S_SIGNED_IN, "1");
            }
            pub.remove(key);
            moved++;
        }
        if (!saveSecrets()) return;
        if (moved > 0) {
            save();
            try { Files.deleteIfExists(new File(file().getPath() + ".tmp").toPath()); } catch (Throwable ignored) {}
            warn("moved " + moved + " secret entr" + (moved == 1 ? "y" : "ies") + " from " + file().getName()
                    + " into " + secretsFile().getName() + " (sealed)");
        }
    }

    // ===================================================================
    // The sealed secrets (owner only)
    // ===================================================================

    private static long sSecStampTime = -1, sSecStampSize = -1;
    /** Whether tokens.dat exists but could not be opened: then it is not written over. */
    private static boolean sSecUnreadable;

    /** The secrets, reloaded when another owner process changed the file; null when
     *  the file exists and cannot be opened (another Windows user's, or changed). */
    private static Properties secrets() {
        synchronized (LOCK) {
            File f = secretsFile();
            if (sSecrets != null && f.lastModified() == sSecStampTime && f.length() == sSecStampSize) return sSecrets;
            Properties p = new Properties();
            sSecUnreadable = false;
            if (f.isFile()) {
                try {
                    byte[] plain = com.macapk.loader.DataProtection.unseal(Files.readAllBytes(f.toPath()));
                    p.load(new java.io.ByteArrayInputStream(plain));
                    java.util.Arrays.fill(plain, (byte) 0);
                } catch (Throwable t) {
                    // Not signed out: the account stays listed, its token requests fail
                    // with this reason, and the file is left for a later look.
                    warn("could not open " + f + ": " + t);
                    sSecUnreadable = true;
                    sSecrets = null;
                    return null;
                }
            }
            sSecrets = p;
            sSecStampTime = f.lastModified();
            sSecStampSize = f.length();
            return p;
        }
    }

    /** Seal and write the secrets, beside then over the file (a reader sees the old
     *  file or the new one). False when nothing was written: no data protection, or
     *  the file that is there could not be opened. */
    private static boolean saveSecrets() {
        synchronized (LOCK) {
            if (sSecrets == null || sSecUnreadable) return false;
            File f = secretsFile();
            try {
                java.io.ByteArrayOutputStream text = new java.io.ByteArrayOutputStream();
                sSecrets.store(text, null);
                byte[] plain = text.toByteArray();
                byte[] sealed = com.macapk.loader.DataProtection.seal(plain);
                java.util.Arrays.fill(plain, (byte) 0);
                File parent = f.getParentFile();
                if (parent != null && !parent.isDirectory()) Files.createDirectories(parent.toPath());
                File tmp = new File(f.getPath() + ".tmp");
                Files.write(tmp.toPath(), sealed);
                try {
                    Files.move(tmp.toPath(), f.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING,
                            java.nio.file.StandardCopyOption.ATOMIC_MOVE);
                } catch (IOException e) {
                    Files.write(f.toPath(), sealed);
                    Files.deleteIfExists(tmp.toPath());
                }
                sSecStampTime = f.lastModified();
                sSecStampSize = f.length();
                return true;
            } catch (Throwable t) {
                warn("could not write " + f + ": " + t);
                return false;
            }
        }
    }

    private static String secret(String key) {
        requireOwner("reading a " + (key.startsWith(P_TOKEN) ? "token" : "master token"));
        Properties p = secrets();
        return p == null ? null : p.getProperty(key);
    }

    /** True when the accounts this process sees come from a complete read of
     *  the store's file (not a missing or unreadable one): what a caller needs
     *  before it treats an account's absence as a sign-out. */
    public static boolean readCleanly() {
        synchronized (LOCK) {
            props();
            return sReadClean;
        }
    }

    private static void save() {
        synchronized (LOCK) {
            if (sProps == null) return;
            File f = file();
            try {
                File parent = f.getParentFile();
                if (parent != null && !parent.isDirectory()) {
                    Files.createDirectories(parent.toPath());
                    chmod700(parent);
                }
                // Create the file EMPTY and lock it down BEFORE writing content.
                // Writing first and chmod-ing after leaves a window where a token
                // sits at the process umask, which on a default macOS account is
                // world-readable.
                // Written beside the file, then moved over it: another process
                // reading while we write must see the old file or the new one,
                // never a truncated one (an account missing from a half-written
                // file would read as a sign-out).
                File tmp = new File(f.getPath() + ".tmp");
                if (!tmp.exists()) {
                    Files.createFile(tmp.toPath());
                }
                chmod600(tmp);
                try (OutputStream out = Files.newOutputStream(tmp.toPath())) {
                    sProps.store(out, "Win.apk Google account state; the tokens are in tokens.dat, sealed");
                }
                chmod600(tmp);   // store() does not change perms; re-assert after truncate
                try {
                    Files.move(tmp.toPath(), f.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING,
                            java.nio.file.StandardCopyOption.ATOMIC_MOVE);
                } catch (IOException e) {
                    // No atomic replace here (a reader holding the file without
                    // delete sharing): write in place, as before.
                    try (OutputStream out = Files.newOutputStream(f.toPath())) {
                        sProps.store(out, "Win.apk Google account state; the tokens are in tokens.dat, sealed");
                    }
                    Files.deleteIfExists(tmp.toPath());
                    chmod600(f);
                }
                sStampTime = f.lastModified();
                sStampSize = f.length();
                sReadClean = true;
            } catch (Throwable t) {
                warn("could not write " + f + ": " + t);
            }
            syncAccountSentinel();
        }
    }

    /** The name of a zero-byte marker that exists iff at least one account is
     *  signed in. */
    private static final String SENTINEL = "accounts.present";

    /**
     * Keep the {@code accounts.present} sentinel in step with {@link #hasAccount}.
     *
     * WHY A SEPARATE EMPTY FILE EXISTS. The launcher wants to show a "Sign in
     * with Google" nudge until an account is added, and hide it once one is. The
     * only truthful source for "is an account signed in" is this store -- but
     * the launcher must NOT read the credentials file (it holds live tokens),
     * and asking the runtime over `--gms accounts` on every launcher start pays a
     * whole JVM boot just to decorate a glyph. So the store drops a marker the
     * launcher can `stat` in microseconds: its mere EXISTENCE is the answer, it
     * carries no content, and it lives in the same 0700 directory, so it reveals
     * nothing a reader of that directory could not already learn.
     *
     * ⚠ IT MUST NOT BE A FILE-EXISTENCE HEURISTIC ON gms_state.properties. That
     * file is created by CHECKIN, which happens with zero accounts, so "the state
     * file exists" is true for a device that has never signed in -- exactly the
     * case the nudge is for. The sentinel is gated on hasAccount(), not on the
     * store file existing, so a checkin-only device correctly still shows it.
     *
     * Called from {@link #save()}, so it is reconciled after EVERY mutation and
     * cannot drift: a sign-in creates it, a sign-out that empties the store
     * removes it, and a checkin (no account) leaves it absent.
     */
    static void syncAccountSentinel() {
        File marker = new File(dir(), SENTINEL);
        try {
            boolean want = hasAccount();
            boolean have = marker.exists();
            if (want && !have) {
                File parent = marker.getParentFile();
                if (parent != null && !parent.isDirectory()) {
                    Files.createDirectories(parent.toPath());
                    chmod700(parent);
                }
                Files.createFile(marker.toPath());
            } else if (!want && have) {
                Files.deleteIfExists(marker.toPath());
            }
        } catch (Throwable ignored) {
            // A missing nudge is a cosmetic loss, never a functional one -- the
            // account still works, and the panel's own probe reports the truth.
        }
    }

    /** The absolute path of the sentinel, for the launcher to check. */
    public static File accountSentinelFile() { return new File(dir(), SENTINEL); }

    private static void chmod600(File f) {
        try {
            Set<PosixFilePermission> perms =
                EnumSet.of(PosixFilePermission.OWNER_READ, PosixFilePermission.OWNER_WRITE);
            Files.setPosixFilePermissions(f.toPath(), perms);
        } catch (Throwable ignored) {}
    }

    private static void chmod700(File d) {
        try {
            Set<PosixFilePermission> perms = EnumSet.of(PosixFilePermission.OWNER_READ,
                    PosixFilePermission.OWNER_WRITE, PosixFilePermission.OWNER_EXECUTE);
            Files.setPosixFilePermissions(d.toPath(), perms);
        } catch (Throwable ignored) {}
    }

    // ===================================================================
    // Checkin identity
    // ===================================================================

    /** The device's checkin android_id, or 0 if this device has never checked
     *  in. 0 is a meaningful answer, not a failure: it is what a device reports
     *  before registration, and callers that need one must trigger a checkin
     *  rather than invent a value. */
    public static long androidId() {
        return parseLong(props().getProperty(K_ANDROID_ID), 0L);
    }

    public static long securityToken() {
        return parseLong(props().getProperty(K_SECURITY_TOKEN), 0L);
    }

    public static long lastCheckinMs() {
        return parseLong(props().getProperty(K_CHECKIN_MS), 0L);
    }

    public static String deviceDataVersionInfo() {
        return props().getProperty(K_CHECKIN_VER);
    }

    public static void setCheckin(long androidId, long securityToken, String versionInfo) {
        Properties p = props();
        synchronized (LOCK) {
            p.setProperty(K_ANDROID_ID, Long.toUnsignedString(androidId));
            p.setProperty(K_SECURITY_TOKEN, Long.toUnsignedString(securityToken));
            p.setProperty(K_CHECKIN_MS, Long.toString(System.currentTimeMillis()));
            if (versionInfo != null) p.setProperty(K_CHECKIN_VER, versionInfo);
        }
        save();
    }

    // ===================================================================
    // FCM registration record
    // ===================================================================

    /** "Has this package EVER obtained an FCM token from us?"
     *
     *  This exists so the MCS receive channel can start LAZILY without breaking
     *  correctness. An app registers once and then caches its token in its own
     *  storage; on every later launch it does NOT register again, but it still
     *  expects push to arrive. So "start the socket when someone registers" is
     *  only correct if we also remember who registered on a PREVIOUS run --
     *  otherwise a cached-token app silently never receives anything, which is
     *  the sort of bug that looks like a flaky network rather than a logic error.
     *
     *  Deliberately a package-keyed flag, not a stored token: we do not need the
     *  token (the app has it), and storing one would be a second copy to keep
     *  coherent with Google. */
    public static boolean hasEverRegistered(String pkg) {
        if (pkg == null || pkg.isEmpty()) return false;
        Properties p = props();
        synchronized (LOCK) {
            return p.getProperty(K_REG_PREFIX + pkg) != null;
        }
    }

    public static void markRegistered(String pkg) {
        if (pkg == null || pkg.isEmpty()) return;
        Properties p = props();
        synchronized (LOCK) {
            if (p.getProperty(K_REG_PREFIX + pkg) != null) return;   // no needless rewrite
            p.setProperty(K_REG_PREFIX + pkg, Long.toString(System.currentTimeMillis()));
        }
        save();
    }

    // ===================================================================
    // Accounts
    // ===================================================================

    /** Every signed-in Google account name (the email), in insertion-stable
     *  order. Empty when nobody has signed in -- which is exactly what a fresh
     *  device reports, so callers already handle it. */
    public static List<String> accountNames() {
        List<String> out = new ArrayList<>();
        Properties p = props();
        for (String key : p.stringPropertyNames()) {
            // .masterToken: a file an owner has not migrated yet (the name only; the
            // value is never read here).
            String suffix = key.endsWith(S_SIGNED_IN) ? S_SIGNED_IN : key.endsWith(S_MASTER) ? S_MASTER : null;
            if (suffix != null && key.startsWith(P_ACCOUNT)) {
                String name = key.substring(P_ACCOUNT.length(), key.length() - suffix.length());
                if (!name.isEmpty() && !out.contains(name)) out.add(name);
            }
        }
        java.util.Collections.sort(out);
        return out;
    }

    /** Whether `account` is signed in. Any process may ask; the master token is not read. */
    public static boolean hasAccount(String account) {
        return account != null && accountNames().contains(account);
    }

    public static boolean hasAccount() { return !accountNames().isEmpty(); }

    /** The single account most callers want. AOSP's AccountManager has no
     *  concept of a "primary" account, so this is only a convenience for our own
     *  CLI and for services that must pick one; it deliberately returns the
     *  FIRST in sorted order rather than guessing at a preference. */
    public static String primaryAccount() {
        List<String> a = accountNames();
        return a.isEmpty() ? null : a.get(0);
    }

    /** The account's master token. Owner processes only (see the header): anywhere
     *  else this throws SecurityException. */
    public static String masterToken(String account) {
        if (account == null) return null;
        return secret(P_ACCOUNT + account + S_MASTER);
    }

    /** Store a signed-in account: its master token sealed, then the public marker
     *  (a process that sees the marker finds the token). Owner processes only. */
    public static void putAccount(String account, String masterToken) {
        if (account == null || masterToken == null) return;
        requireOwner("storing an account");
        synchronized (LOCK) {
            Properties sec = secrets();
            if (sec == null) throw new IllegalStateException("the account's token store cannot be opened");
            sec.setProperty(P_ACCOUNT + account + S_MASTER, masterToken);
            if (!saveSecrets()) throw new IllegalStateException("the account's token could not be stored");
            props().setProperty(P_ACCOUNT + account + S_SIGNED_IN, "1");
        }
        save();
    }

    /** A non-secret per-account attribute (the `services=` list Google returns
     *  at sign-in, the display name, the account id, ...): in gms_state.properties,
     *  which any process may read, because none of it is a credential. */
    public static String accountData(String account, String key) {
        if (account == null || key == null) return null;
        return props().getProperty(P_ACCOUNT + account + ".data." + key);
    }

    public static void putAccountData(String account, String key, String value) {
        if (account == null || key == null) return;
        synchronized (LOCK) {
            if (value == null) props().remove(P_ACCOUNT + account + ".data." + key);
            else               props().setProperty(P_ACCOUNT + account + ".data." + key, value);
        }
        save();
    }

    public static void removeAccount(String account) {
        if (account == null) return;
        requireOwner("removing an account");
        synchronized (LOCK) {
            Properties p = props();
            for (String key : new ArrayList<>(p.stringPropertyNames())) {
                if (key.startsWith(P_ACCOUNT + account + ".") || key.startsWith(P_TOKEN + account + ".")) p.remove(key);
            }
            Properties sec = secrets();
            if (sec != null) {
                for (String key : new ArrayList<>(sec.stringPropertyNames())) {
                    if (key.startsWith(P_ACCOUNT + account + ".") || key.startsWith(P_TOKEN + account + ".")) {
                        sec.remove(key);
                    }
                }
                saveSecrets();
            }
        }
        save();
        AccountPhoto.delete(account);
    }

    // ===================================================================
    // Scoped auth tokens
    // ===================================================================

    /** A cached scoped token, or null if absent OR expired.
     *
     *  Expiry is enforced HERE rather than at the call site because every caller
     *  would otherwise have to remember to, and the failure mode of forgetting
     *  is the worst kind: a stale token produces an HTTP 401 from Google that
     *  reads to the caller like "not licensed" rather than "re-authenticate".
     *
     *  The 5-minute skew is the usual clock-drift guard: a token that expires in
     *  three minutes is treated as already gone, because the request it would be
     *  used for may not land inside that window.
     *
     *  In an app's process the answer comes from the token service, as an app's
     *  peekAuthToken comes from AccountManagerService's token table. */
    public static String authToken(String account, String scope) {
        if (account == null || scope == null) return null;
        if (!sOwner) return GmsTokens.peek(account, scope);
        String base = P_TOKEN + account + "." + scope;
        String value = secret(base + ".value");
        if (value == null || value.isEmpty()) return null;
        long expiry = parseLong(secret(base + ".expiry"), 0L);
        if (expiry > 0 && System.currentTimeMillis() > expiry - 300_000L) {
            return null;    // expired or about to be
        }
        return value;
    }

    /** @param expirySeconds seconds-from-now, as Google's /auth reports it; <= 0
     *                       means the server did not say, and we cache without an
     *                       expiry rather than inventing one. */
    public static void putAuthToken(String account, String scope, String token, long expirySeconds) {
        if (account == null || scope == null || token == null) return;
        if (!sOwner) { GmsTokens.put(account, scope, token, expirySeconds); return; }
        String base = P_TOKEN + account + "." + scope;
        synchronized (LOCK) {
            Properties sec = secrets();
            if (sec == null) return;
            sec.setProperty(base + ".value", token);
            if (expirySeconds > 0) {
                sec.setProperty(base + ".expiry", Long.toString(System.currentTimeMillis() + expirySeconds * 1000L));
            } else {
                sec.remove(base + ".expiry");
            }
            saveSecrets();
        }
    }

    /** Every scope we hold a token for on this account. Owner only. */
    public static java.util.List<String> scopesFor(String account) {
        java.util.List<String> out = new ArrayList<>();
        if (account == null) return out;
        requireOwner("listing tokens");
        Properties sec = secrets();
        if (sec == null) return out;
        String prefix = P_TOKEN + account + ".";
        for (String key : sec.stringPropertyNames()) {
            if (key.startsWith(prefix) && key.endsWith(".value")) {
                out.add(key.substring(prefix.length(), key.length() - ".value".length()));
            }
        }
        return out;
    }

    /** The cached token WITHOUT the expiry check. Owner only: for identifying a token
     *  by value during invalidation -- an expired token still has to be found and
     *  removed, and {@link #authToken} would hide it. */
    public static String authTokenRaw(String account, String scope) {
        if (account == null || scope == null) return null;
        return secret(P_TOKEN + account + "." + scope + ".value");
    }

    /** AOSP AccountManager.invalidateAuthToken semantics: drop the cached copy so
     *  the next request re-fetches. Does NOT tell Google anything -- neither does
     *  a device. */
    public static void invalidateAuthToken(String account, String scope) {
        if (account == null || scope == null) return;
        if (!sOwner) { GmsTokens.invalidate(account, scope); return; }
        String base = P_TOKEN + account + "." + scope;
        synchronized (LOCK) {
            Properties sec = secrets();
            if (sec == null) return;
            sec.remove(base + ".value");
            sec.remove(base + ".expiry");
            saveSecrets();
        }
    }

    /** AOSP's invalidateAuthToken(type, VALUE): drop every cached token of every
     *  account whose value is `token` (AccountManagerService searches its token
     *  table the same way). */
    public static void invalidateAuthTokenValue(String token) {
        if (token == null) return;
        if (!sOwner) { GmsTokens.invalidateValue(token); return; }
        synchronized (LOCK) {
            for (String name : accountNames()) {
                for (String scope : scopesFor(name)) {
                    if (token.equals(authTokenRaw(name, scope))) invalidateAuthToken(name, scope);
                }
            }
        }
    }

    /** Drop every cached scoped token for an account, keeping the master token.
     *  Used when Google rejects a token for a reason that invalidates the whole
     *  cache (a password change, a revoked app grant). Owner only. */
    public static void invalidateAllTokens(String account) {
        if (account == null) return;
        requireOwner("dropping tokens");
        synchronized (LOCK) {
            Properties sec = secrets();
            if (sec == null) return;
            for (String key : new ArrayList<>(sec.stringPropertyNames())) {
                if (key.startsWith(P_TOKEN + account + ".")) sec.remove(key);
            }
            saveSecrets();
        }
    }

    // ===================================================================

    private static long parseLong(String s, long def) {
        if (s == null || s.isEmpty()) return def;
        try { return Long.parseUnsignedLong(s.trim()); } catch (NumberFormatException e) { return def; }
    }

    /** Forget everything cached in memory so the next read re-reads the file.
     *  The CLI needs this because it mutates the store in one JVM and a second
     *  process must see it; a long-lived app process never calls it. */
    public static void reload() {
        synchronized (LOCK) {
            sProps = null; sStampTime = -1; sStampSize = -1;
            sSecrets = null; sSecStampTime = -1; sSecStampSize = -1;
        }
    }

    private static void warn(String msg) {
        try { com.macapk.loader.NativeLog.w("Mac.apk.GMS", "GmsStore: " + msg); }
        catch (Throwable t) { System.err.println("[GmsStore] " + msg); }
    }
}
