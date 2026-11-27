// CallLogProvider -- the device's call log (authority call_log, android.provider.CallLog),
// as AOSP's packages/providers/ContactsProvider CallLogProvider serves it.
//
// Like ContactsProvider (whose header says where it lives and why): hosted in every app's
// process over one database, <android root>/data/data/com.android.providers.contacts/
// databases/calllog.db, AOSP's own file for it; its ProviderInfo is AOSP's manifest entry
// (package com.android.providers.contacts, exported, readPermission READ_CALL_LOG,
// writePermission WRITE_CALL_LOG), so ContentResolver's transport asks the app's runtime
// permissions first. Win.apk has no telephony: nothing adds a call of its own, so the log
// holds what apps with WRITE_CALL_LOG put there.
//
// What CallLogProvider does and this does too: the calls table under CallLog.Calls' column
// names; calls, calls/<id> and calls/filter/<number> (PhoneNumberUtils.compare, as the
// PHONE_NUMBERS_EQUAL function AOSP's query uses); the limit and offset query parameters;
// voicemail rows (TYPE 4) left out of every query unless allow_voicemails=true, which needs
// READ_VOICEMAIL as AOSP's VoicemailPermissions do, and refused on insert without it, an
// app's selection unable to reach them (ContactsProvider.andSelection: strict, as AOSP's
// query builder); an unknown column refused ("Column 'x' is invalid" in values, "Invalid
// column" in a projection); LAST_MODIFIED set on every write;
// deleting by calls only (AOSP answers "Cannot delete that URL" to calls/<id>); every
// change announced on CallLog.Calls.CONTENT_URI.
package com.macapk.loader;

import android.content.ContentProvider;
import android.content.ContentProviderOperation;
import android.content.ContentProviderResult;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.content.OperationApplicationException;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.net.Uri;
import android.provider.CallLog;
import android.telephony.PhoneNumberUtils;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

public final class CallLogProvider extends ContentProvider {
    private static final String TAG = "CallLogProvider";
    private static final int SCHEMA_VERSION = 1;
    private static final int VOICEMAIL_TYPE = 4;   // CallLog.Calls.VOICEMAIL_TYPE
    private static final String ALLOW_VOICEMAILS = "allow_voicemails";
    private static final String READ_VOICEMAIL = "com.android.voicemail.permission.READ_VOICEMAIL";
    private static final String ADD_VOICEMAIL = "com.android.voicemail.permission.ADD_VOICEMAIL";

    /** CallLog.Calls' columns (AOSP 14's calls table). */
    static final Set<String> COLUMNS = new LinkedHashSet<>(Arrays.asList(
            "_id", "number", "presentation", "post_dial_digits", "via_number", "date", "duration",
            "data_usage", "type", "features", "subscription_component_name", "subscription_id",
            "phone_account_address", "phone_account_hidden", "subscription_component_name_migrated",
            "name", "numbertype", "numberlabel", "countryiso", "voicemail_uri", "is_read",
            "geocoded_location", "lookup_uri", "matched_number", "normalized_number", "photo_id",
            "photo_uri", "formatted_number", "transcription", "transcription_state", "new",
            "last_modified", "add_for_all_users", "block_reason", "call_screening_component_name",
            "call_screening_app_name", "priority", "subject", "location", "composer_photo_uri",
            "missed_reason", "is_call_log_phone_account_migration_pending", "is_business_call",
            "asserted_display_name"));

    private static final String[] SCHEMA = {
        "CREATE TABLE IF NOT EXISTS write_lock (n INTEGER NOT NULL)",
        "INSERT INTO write_lock (n) SELECT 0 WHERE NOT EXISTS (SELECT 1 FROM write_lock)",
        "CREATE TABLE IF NOT EXISTS calls (_id INTEGER PRIMARY KEY AUTOINCREMENT, number TEXT,"
            + " presentation INTEGER NOT NULL DEFAULT 1, post_dial_digits TEXT NOT NULL DEFAULT '',"
            + " via_number TEXT NOT NULL DEFAULT '', date INTEGER, duration INTEGER, data_usage INTEGER,"
            + " type INTEGER, features INTEGER NOT NULL DEFAULT 0, subscription_component_name TEXT,"
            + " subscription_id TEXT, phone_account_address TEXT,"
            + " phone_account_hidden INTEGER NOT NULL DEFAULT 0,"
            + " subscription_component_name_migrated INTEGER NOT NULL DEFAULT 0, name TEXT,"
            + " numbertype INTEGER, numberlabel TEXT, countryiso TEXT, voicemail_uri TEXT,"
            + " is_read INTEGER, geocoded_location TEXT, lookup_uri TEXT, matched_number TEXT,"
            + " normalized_number TEXT, photo_id INTEGER NOT NULL DEFAULT 0, photo_uri TEXT,"
            + " formatted_number TEXT, transcription TEXT, transcription_state INTEGER NOT NULL DEFAULT 0,"
            + " new INTEGER, last_modified INTEGER DEFAULT 0, add_for_all_users INTEGER NOT NULL DEFAULT 1,"
            + " block_reason INTEGER NOT NULL DEFAULT 0, call_screening_component_name TEXT,"
            + " call_screening_app_name TEXT, priority INTEGER NOT NULL DEFAULT 0, subject TEXT,"
            + " location TEXT, composer_photo_uri TEXT, missed_reason INTEGER NOT NULL DEFAULT 0,"
            + " is_call_log_phone_account_migration_pending INTEGER NOT NULL DEFAULT 0,"
            + " is_business_call INTEGER NOT NULL DEFAULT 0, asserted_display_name TEXT)",
        "CREATE INDEX IF NOT EXISTS calls_date ON calls (date)",
    };

    private final Object mLock = new Object();
    private SQLiteDatabase mDb;
    private int mWriteDepth;
    private boolean mChanged, mWriteFailed;

    public CallLogProvider() {}

    /** The ProviderInfo of AOSP's CallLogProvider manifest entry. */
    static ProviderInfo providerInfo() {
        ProviderInfo info = new ProviderInfo();
        info.name = CallLogProvider.class.getName();
        info.authority = CallLog.AUTHORITY;
        info.packageName = ContactsProvider.PACKAGE;
        info.processName = ContactsProvider.PACKAGE;
        info.exported = true;
        info.enabled = true;
        info.grantUriPermissions = true;
        info.readPermission = "android.permission.READ_CALL_LOG";
        info.writePermission = "android.permission.WRITE_CALL_LOG";
        return info;
    }

    boolean isOpen() { return mDb != null; }

    @Override
    public boolean onCreate() {
        mDb = ContactsProvider.openDatabase("calllog.db");
        if (mDb == null) return false;
        // As ContactsProvider's schema: no exclusive transaction at every start once current.
        if (mDb.getVersion() >= SCHEMA_VERSION) return true;
        try {
            mDb.beginTransaction();
            try {
                if (mDb.getVersion() < SCHEMA_VERSION) {
                    for (String sql : SCHEMA) mDb.execSQL(sql);
                    mDb.setVersion(SCHEMA_VERSION);
                }
                mDb.setTransactionSuccessful();
            } finally {
                mDb.endTransaction();
            }
        } catch (RuntimeException e) {
            NativeLog.w(TAG, "calllog.db: schema failed: " + e);
            mDb.close();
            mDb = null;
            return false;
        }
        return true;
    }

    // ---- write transaction (as ContactsProvider's) ------------------------------------

    private void beginWrite() {
        if (mWriteDepth++ > 0) return;
        mChanged = false;
        mDb.beginTransaction();
        mDb.execSQL("UPDATE write_lock SET n = n + 1");
    }

    private void endWrite(boolean ok) {
        if (--mWriteDepth > 0) {
            if (!ok) mWriteFailed = true;
            return;
        }
        boolean notify = false;
        try {
            if (ok && !mWriteFailed) mDb.setTransactionSuccessful();
            notify = ok && !mWriteFailed && mChanged;
        } finally {
            mWriteFailed = false;
            mDb.endTransaction();
        }
        Context c = getContext();
        if (notify && c != null) c.getContentResolver().notifyChange(CallLog.Calls.CONTENT_URI, null, false);
    }

    // ---- routing -------------------------------------------------------------------

    /** 1 calls, 2 calls/<id>, 3 calls/filter/<number>; 0 for any other URI. */
    private static int match(Uri uri) {
        List<String> s = uri.getPathSegments();
        if (s.isEmpty() || !s.get(0).equals("calls")) return 0;
        if (s.size() == 1) return 1;
        if (s.size() == 2 && s.get(1).matches("\\d+")) return 2;
        if (s.size() == 3 && s.get(1).equals("filter")) return 3;
        return 0;
    }

    private boolean callerHolds(String permission) {
        Context c = getContext();
        return c != null && c.checkPermission(permission, android.os.Process.myPid(), android.os.Process.myUid())
                == PackageManager.PERMISSION_GRANTED;
    }

    /** VoicemailPermissions: allow_voicemails=true is the caller asking for voicemail rows. */
    private boolean voicemailRequest(Uri uri, String permission) {
        if (!uri.getBooleanQueryParameter(ALLOW_VOICEMAILS, false)) return false;
        if (!callerHolds(permission) && !callerHolds(READ_VOICEMAIL)) {
            throw new SecurityException("Caller must have " + permission + " to access voicemails");
        }
        return true;
    }

    private String where(Uri uri, int m, String selection, List<String> args, String voicemailPermission) {
        StringBuilder w = new StringBuilder();
        if (!voicemailRequest(uri, voicemailPermission)) w.append("(type != ").append(VOICEMAIL_TYPE).append(')');
        if (m == 2) {
            w.append(w.length() == 0 ? "" : " AND ").append("(_id = ?)");
            args.add(uri.getLastPathSegment());
        } else if (m == 3) {
            w.append(w.length() == 0 ? "" : " AND ").append("(_id IN (")
                    .append(numberMatches(uri.getLastPathSegment())).append("))");
        }
        String all = ContactsProvider.andSelection(mDb, "calls", w.toString(), selection);
        return all.isEmpty() ? null : all;
    }

    /** The calls whose number is the same as `number` (PHONE_NUMBERS_EQUAL). */
    private String numberMatches(String number) {
        StringBuilder ids = new StringBuilder();
        try (Cursor c = mDb.rawQuery("SELECT _id, number FROM calls", null)) {
            while (c.moveToNext()) {
                if (!c.isNull(1) && PhoneNumberUtils.compare(number, c.getString(1))) {
                    ids.append(ids.length() == 0 ? "" : ",").append(c.getLong(0));
                }
            }
        }
        return ids.length() == 0 ? "-1" : ids.toString();
    }

    // ---- ContentProvider ---------------------------------------------------------------

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs,
                        String sortOrder) {
        synchronized (mLock) {
            if (mDb == null) return null;
            int m = match(uri);
            if (m == 0) throw new IllegalArgumentException("Unknown URL " + uri);
            List<String> args = new ArrayList<>();
            String where = where(uri, m, selection, args, READ_VOICEMAIL);
            if (selectionArgs != null) args.addAll(Arrays.asList(selectionArgs));
            String limit = uri.getQueryParameter(CallLog.Calls.LIMIT_PARAM_KEY);
            String offset = uri.getQueryParameter(CallLog.Calls.OFFSET_PARAM_KEY);
            if (limit != null && !limit.matches("\\d+")) limit = null;
            if (limit != null && offset != null && offset.matches("\\d+")) limit = offset + "," + limit;
            String[] columns = ContactsProvider.checkedProjection(mDb, "calls", projection);
            Cursor c = mDb.query("calls", columns, where, args.toArray(new String[0]), null, null, sortOrder, limit);
            return ContactsProvider.copy(getContext(), c, CallLog.CONTENT_URI);
        }
    }

    @Override
    public String getType(Uri uri) {
        switch (match(uri)) {
            case 1: case 3: return CallLog.Calls.CONTENT_TYPE;
            case 2: return CallLog.Calls.CONTENT_ITEM_TYPE;
            default: return null;
        }
    }

    private static ContentValues checked(ContentValues values) {
        ContentValues v = new ContentValues();
        if (values == null) return v;
        for (String k : values.keySet()) {
            if (!COLUMNS.contains(k)) throw new IllegalArgumentException("Column '" + k + "' is invalid.");
            Object o = values.get(k);
            if (o == null) v.putNull(k);
            else if (o instanceof Boolean) v.put(k, ((Boolean) o) ? 1 : 0);
            else if (o instanceof Long) v.put(k, (Long) o);
            else if (o instanceof Integer) v.put(k, (Integer) o);
            else if (o instanceof Double) v.put(k, (Double) o);
            else if (o instanceof byte[]) v.put(k, (byte[]) o);
            else v.put(k, o.toString());
        }
        return v;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        synchronized (mLock) {
            if (mDb == null) return null;
            if (match(uri) != 1) throw new IllegalArgumentException("Unknown URL " + uri);
            ContentValues v = checked(values);
            Integer type = v.getAsInteger("type");
            if (type != null && type == VOICEMAIL_TYPE && !voicemailRequest(uri, ADD_VOICEMAIL)) {
                throw new IllegalArgumentException("Cannot insert a voicemail without " + ALLOW_VOICEMAILS + "=true");
            }
            v.remove("_id");
            v.put("last_modified", System.currentTimeMillis());
            boolean ok = false;
            beginWrite();
            try {
                long id = mDb.insertOrThrow("calls", null, v);
                mChanged = true;
                ok = true;
                return ContentUris.withAppendedId(CallLog.Calls.CONTENT_URI, id);
            } finally {
                endWrite(ok);
            }
        }
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        synchronized (mLock) {
            if (mDb == null) return 0;
            int m = match(uri);
            if (m != 1 && m != 2) throw new UnsupportedOperationException("Cannot update URL: " + uri);
            ContentValues v = checked(values);
            v.remove("_id");
            v.put("last_modified", System.currentTimeMillis());
            List<String> args = new ArrayList<>();
            String where = where(uri, m, selection, args, ADD_VOICEMAIL);
            if (selectionArgs != null) args.addAll(Arrays.asList(selectionArgs));
            boolean ok = false;
            beginWrite();
            try {
                int n = mDb.update("calls", v, where, args.toArray(new String[0]));
                if (n > 0) mChanged = true;
                ok = true;
                return n;
            } finally {
                endWrite(ok);
            }
        }
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        synchronized (mLock) {
            if (mDb == null) return 0;
            if (match(uri) != 1) throw new UnsupportedOperationException("Cannot delete that URL: " + uri);
            List<String> args = new ArrayList<>();
            String where = where(uri, 1, selection, args, ADD_VOICEMAIL);
            if (selectionArgs != null) args.addAll(Arrays.asList(selectionArgs));
            boolean ok = false;
            beginWrite();
            try {
                int n = mDb.delete("calls", where, args.toArray(new String[0]));
                if (n > 0) mChanged = true;
                ok = true;
                return n;
            } finally {
                endWrite(ok);
            }
        }
    }

    @Override
    public ContentProviderResult[] applyBatch(ArrayList<ContentProviderOperation> operations)
            throws OperationApplicationException {
        synchronized (mLock) {
            if (mDb == null) return super.applyBatch(operations);
            boolean ok = false;
            beginWrite();
            try {
                ContentProviderResult[] r = super.applyBatch(operations);
                ok = true;
                return r;
            } finally {
                endWrite(ok);
            }
        }
    }

    @Override
    public int bulkInsert(Uri uri, ContentValues[] values) {
        synchronized (mLock) {
            if (mDb == null) return 0;
            boolean ok = false;
            beginWrite();
            try {
                int n = super.bulkInsert(uri, values);
                ok = true;
                return n;
            } finally {
                endWrite(ok);
            }
        }
    }
}
