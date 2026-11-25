// com.macapk.gms.GoogleAuthClient -- the token exchange with Google's /auth.
//
// TWO CALLS, and they are different questions:
//   1. exchangeOAuthToken(): the `oauth_token` a completed sign-in leaves behind
//      -> a MASTER TOKEN (the long-lived `Token=` line). Once per sign-in.
//   2. fetchScopedToken(): master token + a scope -> a short-lived bearer token
//      (`Auth=`). Once per scope, cached until it expires.
//
// ---- THE CLIENT IDENTITY ----------------------------------------------------
// Google's /auth wants to know which client is asking: an `app` package name and
// a `client_sig` (the SHA-1 of that package's signing certificate).
//
// Sign-in (Call 1) and every scope but one are asked as
// `app=com.android.vending` with `client_sig` = the SHA-1 of Win.apk's own
// platform certificate (com.macapk.gms.PlatformSignature): this device hosts a
// com.android.vending, and that package is signed by Win.apk.
//
// The googleplay scope, which licence checks need, Google issues only to the
// Play Store's registered client, so that one call names the Play Store's
// certificate SHA-1 instead (PLAY_STORE_CLIENT_SIG and applyPlayStoreIdentity
// below, which say what that does and does not do). The hash is public and
// sent as plain text; no key is involved.
//
// PROTOCOL REFERENCE (read-only, never vendored): microG GmsCore @157c9d86,
//   play-services-base/core/src/main/java/org/microg/gms/auth/AuthRequest.java
//   play-services-base/core/src/main/java/org/microg/gms/common/HttpFormClient.java
//   play-services-core/src/main/java/org/microg/gms/auth/AuthManager.java
// Full tabulation: docs/gms-checkin-auth-wire_2026-09-05.md
package com.macapk.gms;

import java.io.IOException;
import java.util.LinkedHashMap;
import java.util.Map;

public final class GoogleAuthClient {

    private static final String URL = "https://android.googleapis.com/auth";

    /** The package identity we present. See the header: the honest default is
     *  the package we genuinely host. */
    private static final String DEFAULT_APP = "com.android.vending";

    private GoogleAuthClient() {}

    // ===================================================================
    // Call 1: oauth_token -> master token
    // ===================================================================

    /**
     * Complete a sign-in. The caller supplies the `oauth_token` value harvested
     * from the EmbeddedSetup WebView's cookies.
     *
     * @return the account email on success, null on failure (the reason is
     *         logged and returned by {@link #lastError()}).
     */
    public static String exchangeOAuthToken(String oauthToken) {
        if (oauthToken == null || oauthToken.isEmpty()) {
            sLastError.set("no oauth_token supplied");
            return null;
        }
        // An app's process: the master token Google answers with is the token
        // service's to keep (GmsStore's header); only the e-mail comes back.
        if (!GmsStore.owner()) return GmsTokens.signIn(oauthToken);
        Map<String, String> form = baseParams();
        form.put("service", "ac2dm");
        form.put("Token", oauthToken);
        form.put("get_accountid", "1");
        // Tells the server that `Token` is an OAuth ACCESS token rather than a
        // master token -- this is what distinguishes call 1 from call 2.
        form.put("ACCESS_TOKEN", "1");
        form.put("add_account", "1");
        // ⚠ THE LITERAL STRING "null", AND THIS IS NOT AN ATTESTATION SPOOF.
        // Google's /auth expects a `droid_guard_results` field on the ac2dm
        // exchange; OMITTING it entirely is a malformed request and Google
        // answers HTTP 400 "missing droidguard" (measured 2026-09-05 on a real
        // sign-in). The field's HONEST value for a device that ran no DroidGuard
        // is the four-character string "null" -- which is exactly what we send,
        // and exactly what microG sends (LoginActivity.java:356). We do NOT run
        // DroidGuard, we do NOT download or execute its VM bytecode, and we do
        // NOT forge a result: "null" says "I have no DroidGuard attestation",
        // which is the truth. Win.apk does not defeat or spoof attestation;
        // declaring that there is no result is not that. If Google
        // ever rejects "null" and demands a real result, THAT is the honest
        // stopping point (it would mean the flow now requires attestation we
        // will not fabricate), not a prompt to start running DroidGuard.
        form.put("droidguard_results", "null");   // @RequestContent name, AuthRequest.java:71

        Map<String, String> resp = post(form);
        if (resp == null) return null;

        String email  = resp.get("Email");
        String master = resp.get("Token");
        if (email == null || master == null) {
            sLastError.set("sign-in response carried no "
                    + (email == null ? "Email" : "Token") + " line"
                    + " (keys: " + resp.keySet() + ")");
            return null;
        }
        GmsStore.putAccount(email, master);
        if (resp.get("services")  != null) GmsStore.putAccountData(email, "services",  resp.get("services"));
        if (resp.get("accountId") != null) GmsStore.putAccountData(email, "accountId", resp.get("accountId"));
        if (resp.get("firstName") != null) GmsStore.putAccountData(email, "firstName", resp.get("firstName"));
        if (resp.get("lastName")  != null) GmsStore.putAccountData(email, "lastName",  resp.get("lastName"));
        log("signed in " + GoogleAuthenticator.redact(email)
            + " (services=" + resp.get("services") + ")");
        return email;
    }

    /**
     * Store an account from a master token the caller already holds, after
     * Google accepts it: the same ac2dm call as above with the master token as
     * `Token` (no ACCESS_TOKEN), which answers an Auth for it and, with
     * get_accountid, the account's id (gaia id). Returns the e-mail, or null
     * with lastError() set.
     */
    public static String signInWithMasterToken(String email, String master) {
        if (email == null || master == null || email.isEmpty() || master.isEmpty()) {
            sLastError.set("no e-mail or master token supplied");
            return null;
        }
        Map<String, String> form = baseParams();
        form.put("Email", email);
        form.put("service", "ac2dm");
        form.put("source", "android");
        form.put("Token", master);
        form.put("get_accountid", "1");
        Map<String, String> resp = post(form);
        if (resp == null) return null;
        if (resp.get("Auth") == null) {
            sLastError.set("Google did not accept the master token (keys: " + resp.keySet() + ")");
            return null;
        }
        GmsStore.putAccount(email, master);
        if (resp.get("services")  != null) GmsStore.putAccountData(email, "services",  resp.get("services"));
        if (resp.get("accountId") != null) GmsStore.putAccountData(email, "accountId", resp.get("accountId"));
        if (resp.get("firstName") != null) GmsStore.putAccountData(email, "firstName", resp.get("firstName"));
        if (resp.get("lastName")  != null) GmsStore.putAccountData(email, "lastName",  resp.get("lastName"));
        log("signed in " + GoogleAuthenticator.redact(email) + " from a master token (accountId "
            + (resp.get("accountId") != null ? "present" : "absent") + ")");
        if (resp.get("accountId") == null) ensureAccountId(email);
        return email;
    }

    /**
     * The account's id (its gaia id), fetched once when the sign-in did not
     * bring it: Google answers ac2dm with a master token without accountId.
     * Play services cannot do without it: it keeps it as the account's
     * "GoogleUserId" user data (written there at a device's sign-in), and its
     * getToken answers every app "missing gaiaId ... internal error", so the
     * Play Store got no token at all. Asked as Play services asks for its own
     * userinfo token (app and caller com.google.android.gms with Google's
     * certificate), with get_accountid; when that answer has no accountId
     * either, the token's userinfo ("sub" is the gaia id). Stored as account
     * data, where the broker's account service reads it. Returns it, or null.
     */
    public static String ensureAccountId(String email) {
        if (email == null) return null;
        String have = GmsStore.accountData(email, "accountId");
        if (have != null && !have.isEmpty()) return have;
        if (!GmsStore.owner()) return GmsTokens.accountId(email);
        String master = GmsStore.masterToken(email);
        if (master == null) return null;
        Map<String, String> form = baseParams();
        form.put("app", GMS_PACKAGE);
        form.put("client_sig", PLAY_STORE_CLIENT_SIG);
        form.put("callerPkg", GMS_PACKAGE);
        form.put("callerSig", PLAY_STORE_CLIENT_SIG);
        form.put("Email", email);
        form.put("service", "oauth2:https://www.googleapis.com/auth/userinfo.email");
        form.put("source", "android");
        form.put("Token", master);
        form.put("get_accountid", "1");
        Map<String, String> resp = post(form);
        if (resp == null) {
            log("account id: no userinfo token (" + sLastError.get() + ")");
            return null;
        }
        String id = resp.get("accountId");
        if ((id == null || id.isEmpty()) && resp.get("Auth") != null) {
            try {
                Map<String, String> h = new LinkedHashMap<>();
                h.put("Authorization", "Bearer " + resp.get("Auth"));
                GmsHttp.Response r = GmsHttp.get("https://www.googleapis.com/oauth2/v3/userinfo", h);
                if (r.ok()) {
                    java.util.regex.Matcher m = java.util.regex.Pattern.compile("\"sub\"\\s*:\\s*\"(\\d+)\"")
                            .matcher(r.text());
                    if (m.find()) id = m.group(1);
                } else {
                    log("account id: userinfo answered " + r);
                }
            } catch (IOException e) {
                log("account id: userinfo failed: " + e);
            }
        }
        if (id == null || id.isEmpty()) {
            log("account id: Google did not tell it");
            return null;
        }
        GmsStore.putAccountData(email, "accountId", id);
        log("account id of " + GoogleAuthenticator.redact(email) + " fetched");
        return id;
    }

    // ===================================================================
    // Call 2: master token -> scoped token
    // ===================================================================

    /**
     * A bearer token for one scope, fetched from Google and cached.
     *
     * @return null when no token could be obtained -- never a placeholder. A
     *         caller that receives null must surface an error; treating an
     *         absent token as an empty one produces a 401 from Google that reads
     *         downstream like a substantive answer.
     */
    public static String fetchScopedToken(String account, String scope) {
        if (account == null || scope == null) return null;
        // An app's process asks the token service, as AccountManager asks the
        // authenticator through AccountManagerService.
        if (!GmsStore.owner()) return GmsTokens.scoped(account, scope);
        String master = GmsStore.masterToken(account);
        if (master == null) {
            sLastError.set("no master token for " + GoogleAuthenticator.redact(account)
                       + " -- sign in first");
            return null;
        }
        Map<String, String> form = baseParams();
        applyPlayStoreIdentity(form, scope);
        form.put("Email", account);
        form.put("service", scope);
        form.put("source", "android");
        form.put("Token", master);
        // microG AuthManager:352-353 sets this pair on the AccountManager path,
        // which is the path a licence check takes.
        form.put("is_called_from_account_manager", "1");
        form.put("_opt_is_called_from_account_manager", "1");

        Map<String, String> resp = post(form);
        if (resp == null) return null;

        String auth = resp.get("Auth");
        if (auth == null) {
            // A response with no Auth but a Permission / consent payload means
            // Google wants the USER to approve this scope in a browser. That is a
            // real, actionable state -- not a transport failure -- so it is
            // reported as itself rather than folded into "network error".
            if (resp.containsKey("Permission") || resp.containsKey("ConsentDataBase64")) {
                sLastError.set("Google requires user consent for scope " + scope
                        + " (Permission=" + resp.get("Permission") + ")");
            } else {
                sLastError.set("no Auth line for scope " + scope
                        + " (keys: " + resp.keySet() + ")");
            }
            return null;
        }

        // `Expiry` is an ABSOLUTE epoch-seconds value, not a duration (microG
        // AuthManager:194 compares it against currentTimeMillis()/1000). Reading
        // it as a duration would cache every token for ~55,000 years.
        long expirySeconds = 0L;
        String exp = resp.get("Expiry");
        if (exp != null) {
            try {
                long absolute = Long.parseLong(exp.trim());
                long delta = absolute - System.currentTimeMillis() / 1000L;
                if (delta > 0) expirySeconds = delta;
            } catch (NumberFormatException ignored) {}
        }
        GmsStore.putAuthToken(account, scope, auth, expirySeconds);
        log("got a token for " + scope + " on " + GoogleAuthenticator.redact(account)
            + " (expires in " + expirySeconds + "s)");
        return auth;
    }

    // ===================================================================
    // Call 3: master token -> a token minted FOR a hosted app (Google sign-in)
    // ===================================================================

    /**
     * A token for {@code service} minted on behalf of the HOSTED app -- the call
     * GMS makes when an app asks "sign this user in to me": the ID token
     * ({@code audience:server:client_id:<serverClientId>}) and the server auth
     * code ({@code oauth2:server:client_id:<id>:api_scope:<scopes>}).
     *
     * ⛔ THE IDENTITY IS THE CALLING APP'S OWN. {@code app}/{@code client_sig} are
     * the hosted package and the SHA-1 of ITS OWN signing certificate (read from
     * its APK by MacApkPackageManager), and so are {@code callerPkg}/{@code
     * callerSig}: in-process, the app IS the caller. That is microG's
     * {@code AuthRequest.callerIsApp()} shape (AuthManager.requestAuth(legacy =
     * false)). microG's sign-in path instead sends {@code callerIsGms()}, i.e.
     * Google's own GMS certificate hash; we never send that, and never another
     * app's identity. Google checks app+client_sig against the OAuth client the
     * app's developer registered for serverClientId, which is exactly the check a
     * phone's GMS passes on the app's behalf.
     *
     * {@code has_permission=1} is sent ONLY when {@code consented} is true, i.e.
     * after the user clicked an account in our chooser -- the consent step GMS's
     * own chooser represents (microG AuthManager: hasPermission(isPermitted())).
     * Without it Google answers with what it would need (issueAdvice / Permission)
     * and stores nothing, which is what {@code debug/gms.sh idtoken} probes.
     *
     * @param tokenRequestOptions base64 RequestOptions proto (a nonce), or null.
     * @return Google's parsed response (Auth, Expiry, grantedScopes, issueAdvice,
     *         ...), or null on a transport / refusal failure ({@link #lastError}).
     *         Never cached: an ID token is per app and per nonce, and a shared
     *         scope-keyed cache would hand one app another app's token.
     */
    public static Map<String, String> fetchAppToken(String account, String service,
            String appPkg, String appSigSha1, boolean includeEmail, boolean includeProfile,
            boolean consented, String tokenRequestOptions) {
        return fetchAppToken(account, service, appPkg, appSigSha1, includeEmail, includeProfile,
                consented, tokenRequestOptions, null);
    }

    /** As above, plus the per-request /auth fields a token KIND needs on top of the
     *  identity ({@code oauth2_prompt}, {@code it_caveat_types},
     *  {@code include_granted_scopes} for a server auth code -- see
     *  GoogleSignInBroker.authCodeFields). Null or empty adds nothing. */
    public static Map<String, String> fetchAppToken(String account, String service,
            String appPkg, String appSigSha1, boolean includeEmail, boolean includeProfile,
            boolean consented, String tokenRequestOptions, Map<String, String> extra) {
        return fetchAppToken(account, service, appPkg, appSigSha1, includeEmail, includeProfile,
                consented, tokenRequestOptions, extra, 0, 0);
    }

    /** As above with an explicit HTTP time budget ({@code connectMs}/{@code readMs}; 0 =
     *  GmsHttp's default 15 s + 30 s). For a caller that must FAIL FAST: GoogleSignInBroker's
     *  "has Google already authorized this app" question is asked while a login sheet waits
     *  on the answer, and a stalled server must not hold it for 45 s per account. */
    public static Map<String, String> fetchAppToken(String account, String service,
            String appPkg, String appSigSha1, boolean includeEmail, boolean includeProfile,
            boolean consented, String tokenRequestOptions, Map<String, String> extra,
            int connectMs, int readMs) {
        if (account == null || service == null || appPkg == null || appSigSha1 == null) {
            sLastError.set("fetchAppToken: missing account/service/app identity");
            return null;
        }
        // An app's process asks the token service, which checks that the identity is
        // this app's own (GMS reads the caller's from PackageManager on a device).
        if (!GmsStore.owner()) {
            return GmsTokens.appToken(account, service, appPkg, appSigSha1, includeEmail, includeProfile,
                    consented, tokenRequestOptions, extra, connectMs, readMs);
        }
        String master = GmsStore.masterToken(account);
        if (master == null) {
            sLastError.set("no master token for " + GoogleAuthenticator.redact(account)
                       + " -- sign in first");
            return null;
        }
        return post(appTokenForm(account, master, service, appPkg, appSigSha1,
                includeEmail, includeProfile, consented, tokenRequestOptions, extra), connectMs, readMs);
    }

    /** The form {@link #fetchAppToken} posts. Package-private and side-effect
     *  free so the offline gate can assert the identity fields without a
     *  network round trip (debug/gsignin_gate.sh). */
    static Map<String, String> appTokenForm(String account, String master, String service,
            String appPkg, String appSigSha1, boolean includeEmail, boolean includeProfile,
            boolean consented, String tokenRequestOptions) {
        return appTokenForm(account, master, service, appPkg, appSigSha1, includeEmail,
                includeProfile, consented, tokenRequestOptions, null);
    }

    static Map<String, String> appTokenForm(String account, String master, String service,
            String appPkg, String appSigSha1, boolean includeEmail, boolean includeProfile,
            boolean consented, String tokenRequestOptions, Map<String, String> extra) {
        Map<String, String> form = baseParams();
        form.put("app", appPkg);
        form.put("client_sig", appSigSha1);
        form.put("callerPkg", appPkg);
        form.put("callerSig", appSigSha1);
        form.put("Email", account);
        form.put("service", service);
        form.put("source", "android");
        form.put("Token", master);
        form.put("oauth2_include_email", includeEmail ? "1" : "0");
        form.put("oauth2_include_profile", includeProfile ? "1" : "0");
        if (tokenRequestOptions != null) form.put("token_request_options", tokenRequestOptions);
        if (consented) form.put("has_permission", "1");
        if (extra != null) {
            // Only request-kind fields; the identity above is never overridable.
            for (Map.Entry<String, String> e : extra.entrySet()) {
                String k = e.getKey();
                if (k == null || e.getValue() == null || form.containsKey(k)) continue;
                form.put(k, e.getValue());
            }
        }
        return form;
    }

    /** RequestOptions{remote=7:1, nonceWrapper=8:{nonce=1}, version=10:6}, base64.
     *  Field numbers: microG play-services-core-proto auth.proto RequestOptions /
     *  NonceWrapper; values: microG extensions.kt performSignIn (remote = 1,
     *  version = 6). This is how a nonce reaches the ID token's `nonce` claim. */
    static String nonceRequestOptions(String nonce) {
        if (nonce == null || nonce.isEmpty()) return null;
        byte[] b = Protobuf.writer()
                .int32(7, 1)
                .message(8, Protobuf.writer().string(1, nonce))
                .int32(10, 6)
                .toByteArray();
        return java.util.Base64.getEncoder().encodeToString(b);
    }

    // ===================================================================
    // Shared request shape
    // ===================================================================

    private static Map<String, String> baseParams() {
        String app = DEFAULT_APP;
        String sig = platformSigSha1();
        long androidId = GmsStore.androidId();

        Map<String, String> form = new LinkedHashMap<>();
        form.put("app", app);
        form.put("client_sig", sig);
        form.put("callerPkg", app);
        form.put("callerSig", sig);
        // Hex, UNSIGNED. Long.toHexString is already unsigned; a signed
        // BigInteger rendering would differ for any id with bit 63 set, and the
        // two spellings disagree exactly when it matters.
        form.put("androidId", androidId == 0 ? "0" : Long.toHexString(androidId));
        form.put("sdk_version", Integer.toString(sdkInt()));
        String country = countryCode();
        form.put("device_country", country);
        form.put("operatorCountry", country);
        form.put("lang", java.util.Locale.getDefault().toString());
        form.put("google_play_services_version",
                 Integer.toString(com.macapk.loader.PlayServices.VERSION_CODE));
        return form;
    }

    // ======================================================================
    // The Play Store's client identity for the licensing scope
    // ======================================================================
    //
    // Google's /auth will not mint a googleplay-scoped token for any other
    // client: it answers HTTP 400 UNREGISTERED_ON_API_CONSOLE, because that
    // scope reaches the Play Store's backend and is issued only to the Play
    // Store's registered client, identified by the package name and the SHA-1
    // of the Play Store's signing certificate below. So the scoped-token call
    // (Call 3, the one a licence check makes) names that client:
    // app=com.android.vending, client_sig/callerSig = that SHA-1,
    // callerPkg=com.google.android.gms. The same is done by other open-source
    // Google services layers (microG) and store clients (Aurora Store).
    //
    // client_sig is the certificate's public SHA-1 as plain text in a form
    // field, not a signature: no key is used or needed, and nothing is signed
    // in Google's name. What the call gets is a token for the user's own
    // account, with which Google answers licence checks for apps that account
    // owns.
    //
    // What it does not do: it makes no licence (the response Google returns is
    // Google-signed and the app's own verifier still checks it); it is no
    // device-integrity claim (Play Integrity, SafetyNet and DroidGuard are not
    // touched). The package signature reported to apps is a separate matter:
    // com.macapk.gms.GoogleGmsCertificate.
    static final String PLAY_STORE_CLIENT_SIG = "38918a453d07199354f8b19af05ec6562ced5788";
    private static final String GMS_PACKAGE = "com.google.android.gms";
    private static final String VENDING_PACKAGE = "com.android.vending";


    /**
     * Overwrite the four identity fields with the Play Store's, for the
     * googleplay licensing scope only.
     *
     * Scope-gated so the identity is not lent to every token a guest app
     * requests: any app can call AccountManager.getAuthToken for any scope, and
     * presenting the Play Store's client there would give that app what the
     * Play Store is authorized for. Any other scope is asked under Win.apk's
     * own client identity and refused by Google if that scope is gated. Applied
     * only to the scoped-token call; Call 1 (sign-in) succeeds under Win.apk's
     * own identity.
     */
    private static void applyPlayStoreIdentity(Map<String, String> form, String scope) {
        if (!GoogleAuthenticator.SCOPE_GOOGLEPLAY.equals(scope)) return;
        form.put("app", VENDING_PACKAGE);
        form.put("client_sig", PLAY_STORE_CLIENT_SIG);
        form.put("callerPkg", GMS_PACKAGE);
        form.put("callerSig", PLAY_STORE_CLIENT_SIG);
    }

    /** @return the parsed response on HTTP 200, or null (with {@link #lastError}
     *          set) on any failure. */
    private static Map<String, String> post(Map<String, String> form) {
        return post(form, 0, 0);
    }

    private static Map<String, String> post(Map<String, String> form, int connectMs, int readMs) {
        Map<String, String> headers = new LinkedHashMap<>();
        headers.put("User-Agent", "GoogleAuth/1.4 (" + android.os.Build.DEVICE
                + " " + android.os.Build.ID + "); gzip");
        headers.put("app", form.get("app"));
        headers.put("device", form.get("androidId"));
        try {
            GmsHttp.Response r = postFormAndroidStyle(URL, form, headers, connectMs, readMs);
            GmsHttp.log("auth", URL, r);
            String body = r.text();
            if (!r.ok()) {
                // Google reports auth failures as a NON-200 whose body is the
                // reason (`Error=BadAuthentication`, `Error=NeedsBrowser`, ...).
                // Keeping that text is the whole difference between a diagnosable
                // failure and "HTTP 403".
                sLastError.set("HTTP " + r.status + " " + firstLine(body));
                // In the log too: a caller that drops lastError() (the broker's
                // account service answering an app) otherwise leaves only the
                // status. The reason line carries no credential.
                String why = sLastError.get();
                log("refused " + form.get("service") + ": " + (why.length() > 200 ? why.substring(0, 200) + "..." : why));
                return null;
            }
            Map<String, String> parsed = parse(body);
            // A 200 that still carries Error= is not documented behaviour, but
            // treating it as success would hand the caller a bundle with no
            // token and no explanation.
            if (parsed.containsKey("Error")) {
                sLastError.set("Error=" + parsed.get("Error")
                        + (parsed.containsKey("ErrorDetail")
                           ? " (" + parsed.get("ErrorDetail") + ")" : ""));
                return null;
            }
            sLastError.set(null);
            return parsed;
        } catch (IOException e) {
            sLastError.set("network: " + e);
            return null;
        }
    }

    /**
     * Android's Uri.encode, not java.net.URLEncoder.
     *
     * They differ in ways that matter here: URLEncoder emits '+' for a space
     * where Uri.encode emits %20, and URLEncoder percent-encodes '!' '~' '\'' '('
     * ')' '*' which Uri.encode leaves alone. Google's /auth is parsing what
     * Android clients send, so we send what Android sends. The allowed set is
     * Uri.encode's documented default: alphanumerics plus  _ - ! . ~ ' ( ) *
     */
    private static GmsHttp.Response postFormAndroidStyle(String url, Map<String, String> form,
                                                         Map<String, String> headers,
                                                         int connectMs, int readMs)
            throws IOException {
        StringBuilder sb = new StringBuilder();
        for (Map.Entry<String, String> e : form.entrySet()) {
            if (e.getValue() == null) continue;
            if (sb.length() > 0) sb.append('&');
            sb.append(uriEncode(e.getKey())).append('=').append(uriEncode(e.getValue()));
        }
        Map<String, String> h = new LinkedHashMap<>(headers);
        h.put("Content-Type", "application/x-www-form-urlencoded");
        byte[] body = sb.toString().getBytes("UTF-8");
        if (connectMs > 0 && readMs > 0) {
            return GmsHttp.postBytes(url, body, "application/x-www-form-urlencoded", h, false,
                    connectMs, readMs);
        }
        return GmsHttp.postBytes(url, body, "application/x-www-form-urlencoded", h, false);
    }

    static String uriEncode(String s) {
        if (s == null) return "";
        StringBuilder out = new StringBuilder(s.length() + 16);
        byte[] utf8 = s.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        for (byte b : utf8) {
            int c = b & 0xFF;
            boolean safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                        || (c >= '0' && c <= '9')
                        || c == '_' || c == '-' || c == '!' || c == '.'
                        || c == '~' || c == '\'' || c == '(' || c == ')' || c == '*';
            if (safe) out.append((char) c);
            else      out.append('%').append(String.format("%02X", c));
        }
        return out.toString();
    }

    /**
     * The line-based `key=value` response format.
     *
     * ⚠ SPLIT ON THE FIRST '=' ONLY. Token values are base64 and routinely
     * contain '=' padding; a split-on-every-'=' truncates every token in the
     * response and the failure looks like a server problem.
     *
     * A line with no '=' is SKIPPED rather than thrown on. microG's own parser
     * indexes [1] outside its try block and aborts the whole parse on such a line
     * (HttpFormClient.java:139-140); inheriting that would mean one unexpected
     * line loses a valid token that is sitting right there in the body.
     */
    static Map<String, String> parse(String body) {
        Map<String, String> out = new LinkedHashMap<>();
        if (body == null) return out;
        for (String line : body.split("\n")) {
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            out.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
        }
        return out;
    }

    // ===================================================================

    // PER THREAD (review lanes21): a failing call sets this and its caller reads it
    // right after on the SAME thread (post -> absorb / PlayLicenseClient / GmsCli).
    // A process-wide field let a concurrent /auth call on another thread (checkin,
    // FCM) overwrite it between the two, so a network failure (status 7) could be
    // reported as 12500 or the reverse. Every in-tree reader runs on the thread that
    // made the call (exchangeOAuthToken is synchronous on NativeSignIn's caller).
    private static final ThreadLocal<String> sLastError = new ThreadLocal<String>();

    /** Why the last call ON THIS THREAD failed, for a CLI or a launcher sheet to
     *  show. Cleared on success. */
    public static String lastError() { return sLastError.get(); }

    /** GmsTokens: the token service's reason, as this thread's lastError(). */
    static void setLastError(String why) { sLastError.set(why); }

    /** SHA-1 of our platform certificate, lower-case hex -- the format
     *  client_sig wants (microG PackageUtils.java:151-179). */
    static String platformSigSha1() {
        try {
            byte[] der = PlatformSignature.certificateDer();
            byte[] sha1 = java.security.MessageDigest.getInstance("SHA-1").digest(der);
            StringBuilder sb = new StringBuilder(40);
            for (byte b : sha1) sb.append(String.format("%02x", b));
            return sb.toString();
        } catch (Throwable t) {
            return "";
        }
    }

    private static int sdkInt() {
        try { return android.os.Build.VERSION.SDK_INT; } catch (Throwable t) { return 30; }
    }

    private static String countryCode() {
        try {
            String c = java.util.Locale.getDefault().getCountry();
            if (c != null && !c.isEmpty()) return c.toLowerCase(java.util.Locale.ROOT);
        } catch (Throwable ignored) {}
        return "us";
    }

    private static String env(String name, String def) {
        String v = System.getenv(name);
        return (v == null || v.isEmpty()) ? def : v;
    }

    private static String firstLine(String s) {
        if (s == null || s.isEmpty()) return "(empty body)";
        int nl = s.indexOf('\n');
        String first = (nl < 0 ? s : s.substring(0, nl)).trim();
        return first.isEmpty() ? "(blank first line)" : first;
    }

    static void log(String m) {
        try { com.macapk.loader.NativeLog.i("Mac.apk.GMS", "auth: " + m); }
        catch (Throwable t) { System.err.println("[auth] " + m); }
    }
}
