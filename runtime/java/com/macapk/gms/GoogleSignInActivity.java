// com.macapk.gms.GoogleSignInActivity -- the account chooser com.google.android.gms
// shows when an app asks to sign a user in ("Choose an account / to continue to
// <App>"), hosted in the calling app's process because Mac.apk's GMS stand-in
// has no process of its own.
//
// Two entry points, one screen (see GoogleSignInBroker's header):
//  - action com.google.android.gms.auth.GOOGLE_SIGN_IN, started for result 40962
//    by the app's own SignInHubActivity, request in the `config` extra;
//  - EXTRA_PENDING, started by the framework CredentialManager for a
//    GetGoogleIdOption / GetSignInWithGoogleOption.
//
// The click on an account IS the consent step: only then is the token minted
// with has_permission=1, for the calling app's own identity. Cancel (the button
// or Back) answers like GMS: errorCode 13 for the hub (it reports 12501
// SIGN_IN_CANCELLED), TYPE_USER_CANCELED for Credential Manager. With no account
// the screen offers only "Add an account", which opens the same Google sign-in
// window as Mac.apk's Accounts panel (NativeSignIn) -- agents never type into it.
// Win.apk: with no account on the PC at all, that window opens at once -- the
// account is Win.apk's, shared by every app (Settings > Google account), not
// this app's -- and the account added is the one signed in with.
package com.macapk.gms;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.List;

public class GoogleSignInActivity extends Activity {

    private GoogleSignInBroker.Request mRequest;
    private GoogleSignInBroker.Pending mPending;
    private android.os.Parcelable mConfig;     // legacy: the app's SignInConfiguration
    private boolean mFinished;
    private boolean mIdentity;                 // One Tap / getSignInIntent start (IdentitySignInService)
    private volatile boolean mBusy;
    private TextView mStatus;
    private String mAppLabel;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Intent in = getIntent();
        try {
            long id = in != null ? in.getLongExtra(GoogleSignInBroker.EXTRA_PENDING, 0L) : 0L;
            if (in != null && in.getBooleanExtra(IdentitySignInService.EXTRA_IDENTITY, false)) {
                mIdentity = true;
                mRequest = IdentitySignInService.requestFrom(in);
            } else if (id != 0L) {
                mPending = GoogleSignInBroker.pending(id);
                mRequest = mPending != null ? mPending.request : null;
                if (mPending != null) {
                    // The app cancelled its CancellationSignal while this chooser is up:
                    // close it silently (the request is withdrawn, nothing is delivered).
                    mPending.setCancelHook(new Runnable() {
                        @Override public void run() {
                            runOnUiThread(new Runnable() {
                                @Override public void run() {
                                    if (mFinished) return;
                                    mFinished = true;
                                    finish();
                                }
                            });
                        }
                    });
                    // Cancelled between the lookup and the hook: nothing to show.
                    if (mPending.done.get()) { mFinished = true; finish(); return; }
                }
            } else if (in != null && GoogleSignInBroker.ACTION_GOOGLE_SIGN_IN.equals(in.getAction())) {
                Bundle ex = in.getExtras();
                if (ex != null) ex.setClassLoader(getClassLoader());
                mConfig = ex != null ? ex.getParcelable(GoogleSignInBroker.EXTRA_CONFIG) : null;
                if (mConfig != null) mRequest = GoogleSignInBroker.parseSignInConfiguration(mConfig);
                if (mRequest != null && mRequest.callerPkg == null) mRequest.callerPkg = getPackageName();
            }
        } catch (Throwable t) {
            GoogleSignInBroker.log("chooser: request parse failed: " + t);
            mRequest = null;
        }
        if (mRequest == null) {
            GoogleSignInBroker.log("chooser: no request -- answering INTERNAL_ERROR");
            if (mIdentity) finishWith(null, GoogleSignInBroker.STATUS_INTERNAL_ERROR, "no request");
            else finishLegacyError(GoogleSignInBroker.STATUS_INTERNAL_ERROR);
            return;
        }
        // A request names the app it signs in to; it must be THIS process's app
        // (microG AuthSignInActivity: "package name mismatch" -> DEVELOPER_ERROR).
        if (!getPackageName().equals(mRequest.callerPkg)) {
            GoogleSignInBroker.log("chooser: request for " + mRequest.callerPkg + " in " + getPackageName());
            finishWith(null, GoogleSignInBroker.STATUS_DEVELOPER_ERROR, "package name mismatch");
            return;
        }
        try {
            CharSequence l = getPackageManager().getApplicationLabel(getApplicationInfo());
            mAppLabel = l != null ? l.toString() : getPackageName();
        } catch (Throwable t) {
            mAppLabel = getPackageName();
        }
        List<String> offered = GoogleSignInBroker.eligibleAccounts(mRequest);
        GoogleSignInBroker.log("chooser: " + mRequest + " accounts=" + offered.size()
                + (mIdentity ? " (identity)" : ""));
        if (mIdentity && mRequest.kind == GoogleSignInBroker.Kind.GOOGLE_ID && mRequest.filterByAuthorized
                && mRequest.autoSelect && offered.size() == 1
                && GoogleSignInBroker.autoSignInAllowed(offered.get(0), mRequest.callerPkg)
                && GoogleSignInBroker.usedOnThisDevice(offered.get(0), mRequest.callerPkg)) {
            // One Tap auto-select for a returning user: sign in without a sheet.
            GoogleSignInBroker.log("chooser: auto-select (returning user, no UI)");
            onAccountPicked(offered.get(0));
            return;
        }
        buildUi();
        if (offered.isEmpty() && !mRequest.filterByAuthorized && noAccountAtAll() && NativeSignIn.available()) {
            GoogleSignInBroker.log("chooser: no account on this PC -- adding Win.apk's account");
            addAccount(true);
        }
    }

    /** No Google account is signed in to Win.apk (as opposed to none that this request may use). */
    private static boolean noAccountAtAll() {
        for (String a : GmsStore.accountNames()) {
            if (a != null && !a.startsWith("_")) return false;
        }
        return true;
    }

    // ------------------------------------------------------------------ UI

    private int dp(float v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private TextView text(String s, float sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        return t;
    }

    private void buildUi() {
        final int ink = 0xFF202124, sub = 0xFF5F6368, blue = 0xFF1A73E8, line = 0xFFDADCE0;
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(0xFFF1F3F4);
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackgroundColor(0xFFFFFFFF);
        card.setPadding(dp(24), dp(24), dp(24), dp(16));
        LinearLayout.LayoutParams cardLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        cardLp.setMargins(dp(16), dp(32), dp(16), dp(32));
        LinearLayout outer = new LinearLayout(this);
        outer.setOrientation(LinearLayout.VERTICAL);
        outer.addView(card, cardLp);
        scroll.addView(outer, new ScrollView.LayoutParams(
                ScrollView.LayoutParams.MATCH_PARENT, ScrollView.LayoutParams.WRAP_CONTENT));

        card.addView(text(tr("Sign in with Google", "כניסה עם חשבון Google"), 14, sub, false));
        List<String> accounts = GoogleSignInBroker.eligibleAccounts(mRequest);
        TextView title = text(accounts.isEmpty() ? tr("Add a Google account to Win.apk", "הוספת חשבון Google ל-Win.apk")
                : tr("Choose an account", "בחירת חשבון"), 24, ink,
                false);
        title.setPadding(0, dp(16), 0, dp(4));
        card.addView(title);
        card.addView(text(tr("to continue to ", "כדי להמשיך אל ") + mAppLabel, 16, sub, false));
        if (accounts.isEmpty()) {
            TextView shared = text(tr("Every app on this PC uses the account added to Win.apk.",
                    "כל האפליקציות במחשב הזה משתמשות בחשבון שנוסף ל-Win.apk."), 14, sub, false);
            shared.setPadding(0, dp(8), 0, 0);
            card.addView(shared);
        }
        View rule = new View(this);
        rule.setBackgroundColor(line);
        LinearLayout.LayoutParams ruleLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, Math.max(1, dp(1)));
        ruleLp.setMargins(0, dp(20), 0, 0);
        card.addView(rule, ruleLp);

        for (final String a : accounts) {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.VERTICAL);
            row.setPadding(dp(4), dp(14), dp(4), dp(14));
            String name = GoogleSignInBroker.displayName(a);
            if (name != null) row.addView(text(name, 16, ink, true));
            row.addView(text(a, 14, sub, false));
            row.setClickable(true);
            row.setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) { onAccountPicked(a); }
            });
            card.addView(row);
            View sep = new View(this);
            sep.setBackgroundColor(line);
            card.addView(sep, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, Math.max(1, dp(1))));
        }

        TextView add = text(accounts.isEmpty() ? tr("Add an account", "הוספת חשבון")
                : tr("Use another account", "שימוש בחשבון אחר"), 16, blue, false);
        add.setPadding(dp(4), dp(14), dp(4), dp(14));
        add.setClickable(true);
        add.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { onAddAccount(); }
        });
        card.addView(add);

        TextView consent = text(tr("To continue, Google will share your name and email address with ",
                "כדי להמשיך, Google תשתף את השם וכתובת האימייל שלך עם ") + mAppLabel + ".", 13, sub, false);
        consent.setPadding(0, dp(16), 0, dp(8));
        card.addView(consent);

        mStatus = text("", 13, 0xFFD93025, false);
        card.addView(mStatus);

        TextView cancel = text(tr("Cancel", "ביטול"), 15, blue, true);
        cancel.setGravity(Gravity.END);
        cancel.setPadding(dp(4), dp(16), dp(4), dp(4));
        cancel.setClickable(true);
        cancel.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { onCancel(); }
        });
        card.addView(cancel, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        if (hebrew()) scroll.setLayoutDirection(View.LAYOUT_DIRECTION_RTL);
        setContentView(scroll);
    }

    /** Win.apk's language for this window: its own setting (winapk.ui.lang, from
     *  the runtime), else the locale the app runs in. */
    private static boolean hebrew() {
        String ui = System.getProperty("winapk.ui.lang");
        if (ui != null && !ui.isEmpty()) return "he".equals(ui);
        String lang = java.util.Locale.getDefault().getLanguage();
        return "he".equals(lang) || "iw".equals(lang);
    }

    private static String tr(String english, String hebrew) { return hebrew() ? hebrew : english; }

    private String unregisteredMessage() {
        final boolean netFree = GoogleSignInBroker.appSignedByNetFree(this, mRequest.callerPkg);
        if (hebrew()) {
            return netFree
                    ? "גוגל לא מזהה את העותק הזה של " + mAppLabel + ": הוא נחתם מחדש על ידי נטפרי, ולכן"
                      + " ההתחברות עם גוגל לא תעבוד בו (גם בטלפון). צריך עותק שחתום על ידי מפתח האפליקציה."
                    : "גוגל לא מזהה את העותק הזה של " + mAppLabel + ": הוא לא חתום בחתימה של מפתח"
                      + " האפליקציה. צריך עותק מקורי של האפליקציה.";
        }
        return netFree
                ? "Google does not recognize this copy of " + mAppLabel + ": NetFree re-signed it, so"
                  + " Google sign-in cannot work in it (on a phone either). It needs a copy signed by the"
                  + " app's developer."
                : "Google does not recognize this copy of " + mAppLabel + ": it is not signed by the"
                  + " app's developer. It needs the app's original copy.";
    }

    private void setStatus(final String s) {
        runOnUiThread(new Runnable() {
            @Override public void run() { if (mStatus != null) mStatus.setText(s); }
        });
    }

    // ------------------------------------------------------------- actions

    private void onAccountPicked(final String account) {
        if (mBusy || mFinished) return;
        mBusy = true;
        GoogleSignInBroker.log("chooser: user picked " + GoogleAuthenticator.redact(account));
        setStatus(tr("Signing in…", "מתבצעת כניסה…"));
        final String sig = GoogleSignInBroker.appCertSha1(this, mRequest.callerPkg);
        Thread t = new Thread(new Runnable() {
            @Override public void run() {
                final GoogleSignInBroker.Result res =
                        GoogleSignInBroker.signIn(mRequest, account, sig, true);
                GoogleSignInBroker.log("chooser: sign-in " + (res.ok() ? "OK" : "FAILED status="
                        + res.status + " (" + res.error + ")") + " idToken=" + (res.idToken != null)
                        + " authCode=" + (res.serverAuthCode != null));
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        mBusy = false;
                        if (res.ok()) {
                            finishWith(res, GoogleSignInBroker.STATUS_SUCCESS, null);
                        } else if (res.error != null && res.error.contains("UNREGISTERED_ON_API_CONSOLE")) {
                            // Google knows this app by its package AND its developer's
                            // signing certificate. A copy signed by someone else (a
                            // filter that re-signs downloads, such as NetFree's patcher)
                            // is refused whatever the account, here as on a phone: say
                            // so and stay, rather than close on the app's generic error.
                            setStatus(unregisteredMessage());
                        } else {
                            finishWith(null, res.status, res.error);
                        }
                    }
                });
            }
        }, "gms-signin");
        t.setDaemon(true);
        t.start();
    }

    private void onAddAccount() {
        addAccount(false);
    }

    /** Opens Win.apk's Google sign-in window. `direct`: the screen opened straight into it
     *  (no account at all), so the account added signs in to the app and a cancel cancels. */
    private void addAccount(final boolean direct) {
        if (mBusy || mFinished) return;
        if (!NativeSignIn.available()) {
            setStatus(tr("Adding an account needs Win.apk's Google sign-in window. "
                    + "Add one in Win.apk > Settings > Google account, then try again.",
                    "כדי להוסיף חשבון צריך את חלון הכניסה של Google ב-Win.apk. אפשר להוסיף חשבון"
                    + " ב-Win.apk > הגדרות > חשבון Google ולנסות שוב."));
            return;
        }
        if (NativeSignIn.active()) return;
        mBusy = true;
        setStatus(tr("Finish signing in in the Google window…", "יש להשלים את הכניסה בחלון של Google…"));
        Thread t = new Thread(new Runnable() {
            @Override public void run() {
                final String added = NativeSignIn.signInInteractive();
                GoogleSignInBroker.log("chooser: add account -> " + (added != null ? "added" : "cancelled"));
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        mBusy = false;
                        if (mFinished) return;
                        if (direct && added == null) {
                            onCancel();
                            return;
                        }
                        if (direct && GoogleSignInBroker.eligibleAccounts(mRequest).contains(added)) {
                            onAccountPicked(added);
                            return;
                        }
                        buildUi();
                        if (added == null) setStatus(tr("No account was added.", "לא נוסף חשבון."));
                    }
                });
            }
        }, "gms-signin-add");
        t.setDaemon(true);
        t.start();
    }

    private void onCancel() {
        if (mFinished) return;
        GoogleSignInBroker.log("chooser: cancelled by the user");
        finishWith(null, GoogleSignInBroker.STATUS_HUB_CANCELLED, "cancelled");
    }

    @Override
    public void onBackPressed() {
        onCancel();
    }

    @Override
    protected void onDestroy() {
        // Leaving without an answer must not strand a Credential Manager caller.
        if (!mFinished && mPending != null) {
            GoogleSignInBroker.fail(mPending,
                    android.credentials.GetCredentialException.TYPE_USER_CANCELED, "chooser closed");
        }
        super.onDestroy();
    }

    // -------------------------------------------------------------- results

    private void finishWith(GoogleSignInBroker.Result res, int status, String error) {
        if (mFinished) return;
        mFinished = true;
        if (mIdentity) {
            // SignInClient.getSignInCredentialFromIntent reads "status" and, on
            // success, "sign_in_credential"; a user cancel is status 16 (CANCELED).
            boolean ok = res != null && res.ok();
            int code = status == GoogleSignInBroker.STATUS_HUB_CANCELLED ? IdentitySignInService.STATUS_CANCELED
                    : status == GoogleSignInBroker.STATUS_SIGN_IN_FAILED ? IdentitySignInService.STATUS_INTERNAL_ERROR
                    : status;
            Intent data = IdentitySignInService.resultIntent(res, code, error);
            setResult(ok ? RESULT_OK : RESULT_CANCELED, data);
            GoogleSignInBroker.log("chooser: identity result " + (ok ? "OK sign_in_credential" : "status=" + code));
            finish();
            return;
        }
        if (mPending != null) {
            if (res != null) GoogleSignInBroker.complete(mPending, res);
            else GoogleSignInBroker.fail(mPending,
                    status == GoogleSignInBroker.STATUS_HUB_CANCELLED
                            ? android.credentials.GetCredentialException.TYPE_USER_CANCELED
                            : android.credentials.GetCredentialException.TYPE_UNKNOWN,
                    error != null ? error : "sign-in failed");
            finish();
            return;
        }
        if (res != null) {
            try {
                ClassLoader cl = mConfig != null ? mConfig.getClass().getClassLoader() : getClassLoader();
                android.os.Parcelable acct = GoogleSignInBroker.buildAppSignInAccount(cl, mRequest, res);
                Intent data = new Intent();
                data.putExtra(GoogleSignInBroker.EXTRA_SIGN_IN_ACCOUNT, acct);
                setResult(RESULT_OK, data);
                GoogleSignInBroker.log("chooser: RESULT_OK signInAccount -> " + acct.getClass().getName());
                finish();
                return;
            } catch (Throwable t) {
                GoogleSignInBroker.log("chooser: could not build the app's SignInAccount: " + t);
                status = GoogleSignInBroker.STATUS_INTERNAL_ERROR;
            }
        }
        mFinished = false;
        finishLegacyError(status);
    }

    private void finishLegacyError(int status) {
        if (mFinished) return;
        mFinished = true;
        Intent data = new Intent();
        data.putExtra(GoogleSignInBroker.EXTRA_ERROR_CODE, status);
        setResult(RESULT_CANCELED, data);
        finish();
    }
}
