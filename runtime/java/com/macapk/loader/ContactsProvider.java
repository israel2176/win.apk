// ContactsProvider -- the device's contacts provider (authority com.android.contacts,
// android.provider.ContactsContract), as AOSP's packages/providers/ContactsProvider
// (ContactsProvider2 over ContactsDatabaseHelper) serves it.
//
// Where it lives. A device runs ContactsProvider2 once, in the
// com.android.providers.contacts process, over one database every app reads through
// Binder. Win.apk hosts the provider in every app's process (Boot.installSystemProviders,
// beside MediaStore's) over ONE database file in the Android file system,
// <android root>/data/data/com.android.providers.contacts/databases/contacts2.db, the
// path AOSP's own package keeps it under. SQLite's file locks make the processes take
// turns as connections in one process would; a write transaction takes the write lock
// first (beginWrite), so two processes never both read then both wait to write. A
// change is announced with ContentResolver.notifyChange on ContactsContract.AUTHORITY_URI,
// as ContactsProvider2 does: this process's observers at once, other processes' through
// the system broker when it runs (Use Google services), as for every authority.
//
// Who may use it. The ProviderInfo is AOSP's manifest entry: package
// com.android.providers.contacts, exported, readPermission READ_CONTACTS,
// writePermission WRITE_CONTACTS, grantUriPermissions. The calling app is another
// package, so ContentResolver's transport checks (ContentProvider.macapkEnforce*) ask the
// app's own runtime permission state (Context.checkPermission, the launcher's grants)
// before any call reaches this class, and a caller without the permission gets AOSP's
// "Permission Denial" SecurityException.
//
// The schema. AOSP's tables by their exported column names: raw_contacts (one per
// account's copy of a person), data (one row per phone, email, name... with mimetype and
// data1..data15), contacts (the aggregate the user sees), groups, agg_exceptions,
// deleted_contacts and phone_lookup (each phone number's normalized form and its
// caller-id min match, for PhoneLookup). Queries run on views that join them under the
// exact column names ContactsContract documents (view_data, view_contacts,
// view_raw_contacts, view_raw_entities, view_contact_entities), so an app's own
// selection and sort order are SQLite's to evaluate, as on a device. As ContactsProvider2's
// strict SQLiteQueryBuilder, a selection is compiled on its own and runs wrapped twice,
// so it cannot reach past the provider's own clause (andSelection), and a projection
// names the view's columns only (checkedProjection, its projection maps' check).
// AOSP keeps mimetypes in a side table; here the data row holds the mimetype string, which
// the views expose under the same name.
//
// What it computes, as ContactsProvider2 does: a StructuredName row's display name from
// its parts and its parts from a display name (NameSplitter, simplified to "first word
// given, last word family"); each raw contact's display name from its best source
// (DisplayNameSources: structured name > phonetic name > nickname > organization > phone >
// email); a phone row's NORMALIZED_NUMBER (data4) when the number is in international
// form; each contact's name raw contact, photo, starred, has_phone_number, lookup key and
// last-updated time; dirty and version on every change not made by a sync adapter
// (CALLER_IS_SYNCADAPTER); a raw contact of the local account deleted at once, an
// account's marked deleted for its sync adapter, as ContactsProvider2.deleteRawContact
// does.
//
// Aggregation. Each raw contact is its own contact unless AggregationExceptions joins two
// (TYPE_KEEP_TOGETHER); TYPE_KEEP_SEPARATE and TYPE_AUTOMATIC split them again. AOSP's
// ContactAggregator also joins raw contacts of different accounts whose names, numbers or
// addresses match; Win.apk does not, so two accounts' copies of one person stay two
// contacts.
//
// Not served (an IllegalArgumentException "Unknown URI", as ContactsProvider2 answers a
// URI it does not match): vCards, the user's profile (its queries answer no rows), search
// suggestions, status updates, display photos (the thumbnail in the Photo row's data15 is
// served as contacts/<id>/photo; writing a raw contact's display_photo is refused).
package com.macapk.loader;

import android.content.ContentProvider;
import android.content.ContentProviderOperation;
import android.content.ContentProviderResult;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.content.OperationApplicationException;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteException;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.ContactsContract;
import android.telephony.PhoneNumberUtils;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public final class ContactsProvider extends ContentProvider {
    private static final String TAG = "ContactsProvider";

    /** The package AOSP's contacts and call log providers belong to. */
    public static final String PACKAGE = "com.android.providers.contacts";

    static final String MIME_NAME = "vnd.android.cursor.item/name";
    static final String MIME_PHONE = "vnd.android.cursor.item/phone_v2";
    static final String MIME_EMAIL = "vnd.android.cursor.item/email_v2";
    static final String MIME_POSTAL = "vnd.android.cursor.item/postal-address_v2";
    static final String MIME_NICKNAME = "vnd.android.cursor.item/nickname";
    static final String MIME_ORGANIZATION = "vnd.android.cursor.item/organization";
    static final String MIME_PHOTO = "vnd.android.cursor.item/photo";
    static final String MIME_SIP = "vnd.android.cursor.item/sip_address";
    static final String MIME_GROUP_MEMBERSHIP = "vnd.android.cursor.item/group_membership";

    // ContactsContract.DisplayNameSources.
    static final int SOURCE_UNDEFINED = 0, SOURCE_EMAIL = 10, SOURCE_PHONE = 20, SOURCE_ORGANIZATION = 30,
                     SOURCE_NICKNAME = 35, SOURCE_PHONETIC = 37, SOURCE_STRUCTURED_NAME = 40;

    // ContactsContract.AggregationExceptions.TYPE_*.
    static final int AGG_AUTOMATIC = 0, AGG_KEEP_TOGETHER = 1, AGG_KEEP_SEPARATE = 2;

    private static final int SCHEMA_VERSION = 1;

    private final Object mLock = new Object();
    private SQLiteDatabase mDb;
    /** Depth of this provider's open write transaction (applyBatch nests the calls in it). */
    private int mWriteDepth;
    /** A change made in the open write transaction, and whether all of it was a sync adapter's. */
    private boolean mChanged;
    private boolean mChangedByApp;

    public ContactsProvider() {}

    // ---- installation ---------------------------------------------------------------

    /** The ProviderInfo of AOSP's ContactsProvider2 manifest entry. */
    static ProviderInfo providerInfo() {
        ProviderInfo info = new ProviderInfo();
        info.name = ContactsProvider.class.getName();
        info.authority = ContactsContract.AUTHORITY;
        info.packageName = PACKAGE;
        info.processName = PACKAGE;
        info.exported = true;
        info.enabled = true;
        info.grantUriPermissions = true;
        info.readPermission = "android.permission.READ_CONTACTS";
        info.writePermission = "android.permission.WRITE_CONTACTS";
        return info;
    }

    /** Attaches and registers the contacts and call log providers in this process, unless
     *  the app serves the authority itself (its own provider is already installed and
     *  keeps it). Called by Boot with the Application, after the app's providers. */
    public static void install(Context app) {
        if (android.content.ContentProviderRegistry.getLocal(ContactsContract.AUTHORITY) == null) {
            ContactsProvider p = new ContactsProvider();
            p.attachInfo(app, providerInfo());   // runs onCreate
            if (p.mDb != null) {
                android.content.ContentProviderRegistry.register(ContactsContract.AUTHORITY, p);
                NativeLog.i(TAG, "system provider com.android.contacts attached");
            }
        }
        if (android.content.ContentProviderRegistry.getLocal(android.provider.CallLog.AUTHORITY) == null) {
            CallLogProvider c = new CallLogProvider();
            c.attachInfo(app, CallLogProvider.providerInfo());
            if (c.isOpen()) {
                android.content.ContentProviderRegistry.register(android.provider.CallLog.AUTHORITY, c);
                NativeLog.i(TAG, "system provider call_log attached");
            }
        }
    }

    /** <android root>/data/data/com.android.providers.contacts/databases/<name>: the
     *  provider package's own database folder, shared by every app's process. */
    static File databaseFile(String name) {
        String root = System.getProperty("macapk.android.root");
        if (root == null || root.isEmpty()) return null;
        return new File(new File(new File(new File(root, "data"), "data"), PACKAGE), "databases/" + name);
    }

    /** Opens a provider database, made if missing; null (logged) when it cannot be. */
    static SQLiteDatabase openDatabase(String name) {
        File f = databaseFile(name);
        if (f == null) {
            NativeLog.w(TAG, name + ": no Android file system root (macapk.android.root)");
            return null;
        }
        File dir = f.getParentFile();
        if (dir != null) dir.mkdirs();
        try {
            return SQLiteDatabase.openOrCreateDatabase(f.getAbsolutePath(), null);
        } catch (RuntimeException e) {
            NativeLog.w(TAG, name + ": cannot open " + f + ": " + e);
            return null;
        }
    }

    @Override
    public boolean onCreate() {
        mDb = openDatabase("contacts2.db");
        if (mDb == null) return false;
        try {
            createSchema(mDb);
        } catch (RuntimeException e) {
            NativeLog.w(TAG, "contacts2.db: schema failed: " + e);
            mDb.close();
            mDb = null;
            return false;
        }
        return true;
    }

    private static void createSchema(SQLiteDatabase db) {
        // Every app's process opens this database at start: read the version first, so the
        // exclusive transaction (which another starting process would wait on) is taken only
        // when the tables are missing or older.
        if (db.getVersion() >= SCHEMA_VERSION) return;
        db.beginTransaction();
        try {
            if (db.getVersion() < SCHEMA_VERSION) {
                for (String sql : SCHEMA) db.execSQL(sql);
                db.setVersion(SCHEMA_VERSION);
            }
            db.setTransactionSuccessful();
        } finally {
            db.endTransaction();
        }
    }

    // Data columns of a row, as the views export them (raw_contact_id apart: each view
    // names it once).
    private static final String DATA_COLS =
            "d.mimetype AS mimetype, d.is_primary AS is_primary, d.is_super_primary AS is_super_primary,"
            + " d.is_read_only AS is_read_only, d.data_version AS data_version,"
            + " d.data1 AS data1, d.data2 AS data2, d.data3 AS data3, d.data4 AS data4, d.data5 AS data5,"
            + " d.data6 AS data6, d.data7 AS data7, d.data8 AS data8, d.data9 AS data9, d.data10 AS data10,"
            + " d.data11 AS data11, d.data12 AS data12, d.data13 AS data13, d.data14 AS data14,"
            + " d.data15 AS data15, d.data_sync1 AS data_sync1, d.data_sync2 AS data_sync2,"
            + " d.data_sync3 AS data_sync3, d.data_sync4 AS data_sync4,"
            + " d.preferred_phone_account_component_name AS preferred_phone_account_component_name,"
            + " d.preferred_phone_account_id AS preferred_phone_account_id,"
            + " d.carrier_presence AS carrier_presence, NULL AS res_package,"
            + " CASE WHEN d.mimetype = '" + MIME_GROUP_MEMBERSHIP + "'"
            + " THEN (SELECT g.sourceid FROM groups g WHERE g._id = d.data1) END AS group_sourceid,"
            + " 0 AS times_used, NULL AS last_time_used";

    // A raw contact's account and sync columns (r).
    private static final String RAW_COLS =
            "r.account_name AS account_name, r.account_type AS account_type, r.data_set AS data_set,"
            + " CASE WHEN r.data_set IS NULL THEN r.account_type ELSE r.account_type || '/' || r.data_set END"
            + " AS account_type_and_data_set, r.sourceid AS sourceid, r.backup_id AS backup_id,"
            + " r.version AS version, r.dirty AS dirty, r.deleted AS deleted, r.metadata_dirty AS metadata_dirty,"
            + " r.aggregation_mode AS aggregation_mode, r.sync1 AS sync1, r.sync2 AS sync2,"
            + " r.sync3 AS sync3, r.sync4 AS sync4";

    // A contact's columns (c, with n its name raw contact).
    private static final String CONTACT_COLS =
            "c.lookup AS lookup, n.display_name AS display_name, n.display_name_alt AS display_name_alt,"
            + " n.display_name_source AS display_name_source, n.phonetic_name AS phonetic_name,"
            + " n.phonetic_name_style AS phonetic_name_style, n.sort_key AS sort_key,"
            + " n.sort_key_alt AS sort_key_alt, c.starred AS starred, c.pinned AS pinned,"
            + " c.has_phone_number AS has_phone_number, c.photo_id AS photo_id,"
            + " c.photo_file_id AS photo_file_id,"
            + " CASE WHEN c.photo_id IS NULL THEN NULL"
            + " ELSE 'content://com.android.contacts/contacts/' || c._id || '/photo' END AS photo_uri,"
            + " CASE WHEN c.photo_id IS NULL THEN NULL"
            + " ELSE 'content://com.android.contacts/contacts/' || c._id || '/photo' END AS photo_thumb_uri,"
            + " c.custom_ringtone AS custom_ringtone, c.send_to_voicemail AS send_to_voicemail,"
            + " 1 AS in_visible_group, 1 AS in_default_directory, 0 AS is_user_profile,"
            + " c.times_contacted AS times_contacted, c.last_time_contacted AS last_time_contacted,"
            + " c.contact_last_updated_timestamp AS contact_last_updated_timestamp,"
            + " c.name_raw_contact_id AS name_raw_contact_id,"
            + " NULL AS contact_presence, NULL AS contact_chat_capability, NULL AS contact_status,"
            + " NULL AS contact_status_ts, NULL AS contact_status_res_package,"
            + " NULL AS contact_status_label, NULL AS contact_status_icon";

    private static final String[] SCHEMA = {
        "CREATE TABLE IF NOT EXISTS write_lock (n INTEGER NOT NULL)",
        "INSERT INTO write_lock (n) SELECT 0 WHERE NOT EXISTS (SELECT 1 FROM write_lock)",
        "CREATE TABLE IF NOT EXISTS contacts (_id INTEGER PRIMARY KEY AUTOINCREMENT,"
            + " name_raw_contact_id INTEGER, photo_id INTEGER, photo_file_id INTEGER,"
            + " custom_ringtone TEXT, send_to_voicemail INTEGER NOT NULL DEFAULT 0,"
            + " starred INTEGER NOT NULL DEFAULT 0, pinned INTEGER NOT NULL DEFAULT 0,"
            + " has_phone_number INTEGER NOT NULL DEFAULT 0, lookup TEXT,"
            + " times_contacted INTEGER NOT NULL DEFAULT 0, last_time_contacted INTEGER,"
            + " contact_last_updated_timestamp INTEGER)",
        "CREATE TABLE IF NOT EXISTS raw_contacts (_id INTEGER PRIMARY KEY AUTOINCREMENT,"
            + " contact_id INTEGER, account_name TEXT, account_type TEXT, data_set TEXT,"
            + " sourceid TEXT, backup_id TEXT, version INTEGER NOT NULL DEFAULT 1,"
            + " dirty INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0,"
            + " metadata_dirty INTEGER NOT NULL DEFAULT 0, aggregation_mode INTEGER NOT NULL DEFAULT 0,"
            + " custom_ringtone TEXT, send_to_voicemail INTEGER NOT NULL DEFAULT 0,"
            + " starred INTEGER NOT NULL DEFAULT 0, pinned INTEGER NOT NULL DEFAULT 0,"
            + " times_contacted INTEGER NOT NULL DEFAULT 0, last_time_contacted INTEGER,"
            + " display_name TEXT, display_name_alt TEXT, display_name_source INTEGER NOT NULL DEFAULT 0,"
            + " phonetic_name TEXT, phonetic_name_style INTEGER NOT NULL DEFAULT 0,"
            + " sort_key TEXT, sort_key_alt TEXT, sync1 TEXT, sync2 TEXT, sync3 TEXT, sync4 TEXT)",
        "CREATE INDEX IF NOT EXISTS raw_contacts_contact_id ON raw_contacts (contact_id)",
        "CREATE TABLE IF NOT EXISTS data (_id INTEGER PRIMARY KEY AUTOINCREMENT,"
            + " raw_contact_id INTEGER NOT NULL, mimetype TEXT NOT NULL,"
            + " is_primary INTEGER NOT NULL DEFAULT 0, is_super_primary INTEGER NOT NULL DEFAULT 0,"
            + " is_read_only INTEGER NOT NULL DEFAULT 0, data_version INTEGER NOT NULL DEFAULT 0,"
            + " data1 TEXT, data2 TEXT, data3 TEXT, data4 TEXT, data5 TEXT, data6 TEXT, data7 TEXT,"
            + " data8 TEXT, data9 TEXT, data10 TEXT, data11 TEXT, data12 TEXT, data13 TEXT, data14 TEXT,"
            + " data15 TEXT, data_sync1 TEXT, data_sync2 TEXT, data_sync3 TEXT, data_sync4 TEXT,"
            + " preferred_phone_account_component_name TEXT, preferred_phone_account_id TEXT,"
            + " carrier_presence INTEGER NOT NULL DEFAULT 0)",
        "CREATE INDEX IF NOT EXISTS data_raw_contact_id ON data (raw_contact_id)",
        "CREATE TABLE IF NOT EXISTS phone_lookup (data_id INTEGER PRIMARY KEY,"
            + " raw_contact_id INTEGER NOT NULL, normalized_number TEXT, min_match TEXT)",
        "CREATE INDEX IF NOT EXISTS phone_lookup_min_match ON phone_lookup (min_match)",
        "CREATE TABLE IF NOT EXISTS groups (_id INTEGER PRIMARY KEY AUTOINCREMENT,"
            + " account_name TEXT, account_type TEXT, data_set TEXT, sourceid TEXT,"
            + " version INTEGER NOT NULL DEFAULT 1, dirty INTEGER NOT NULL DEFAULT 0, title TEXT,"
            + " title_res INTEGER, notes TEXT, system_id TEXT, deleted INTEGER NOT NULL DEFAULT 0,"
            + " group_visible INTEGER NOT NULL DEFAULT 0, should_sync INTEGER NOT NULL DEFAULT 1,"
            + " auto_add INTEGER NOT NULL DEFAULT 0, favorites INTEGER NOT NULL DEFAULT 0,"
            + " group_is_read_only INTEGER NOT NULL DEFAULT 0, res_package TEXT,"
            + " sync1 TEXT, sync2 TEXT, sync3 TEXT, sync4 TEXT)",
        "CREATE TABLE IF NOT EXISTS agg_exceptions (_id INTEGER PRIMARY KEY AUTOINCREMENT,"
            + " type INTEGER NOT NULL, raw_contact_id1 INTEGER NOT NULL, raw_contact_id2 INTEGER NOT NULL,"
            + " UNIQUE (raw_contact_id1, raw_contact_id2))",
        "CREATE TABLE IF NOT EXISTS deleted_contacts (contact_id INTEGER PRIMARY KEY,"
            + " contact_deleted_timestamp INTEGER NOT NULL DEFAULT 0)",
        "CREATE VIEW IF NOT EXISTS view_contacts AS SELECT c._id AS _id, " + CONTACT_COLS
            + " FROM contacts c LEFT JOIN raw_contacts n ON n._id = c.name_raw_contact_id",
        "CREATE VIEW IF NOT EXISTS view_raw_contacts AS SELECT r._id AS _id, r.contact_id AS contact_id, "
            + RAW_COLS + ", r.display_name AS display_name, r.display_name_alt AS display_name_alt,"
            + " r.display_name_source AS display_name_source, r.phonetic_name AS phonetic_name,"
            + " r.phonetic_name_style AS phonetic_name_style, r.sort_key AS sort_key,"
            + " r.sort_key_alt AS sort_key_alt, r.starred AS starred, r.pinned AS pinned,"
            + " r.custom_ringtone AS custom_ringtone, r.send_to_voicemail AS send_to_voicemail,"
            + " r.times_contacted AS times_contacted, r.last_time_contacted AS last_time_contacted,"
            + " 0 AS raw_contact_is_user_profile FROM raw_contacts r",
        "CREATE VIEW IF NOT EXISTS view_data AS SELECT d._id AS _id, d.raw_contact_id AS raw_contact_id, "
            + DATA_COLS + ", " + RAW_COLS + ", c._id AS contact_id, " + CONTACT_COLS
            + " FROM data d JOIN raw_contacts r ON r._id = d.raw_contact_id"
            + " JOIN contacts c ON c._id = r.contact_id"
            + " LEFT JOIN raw_contacts n ON n._id = c.name_raw_contact_id WHERE r.deleted = 0",
        "CREATE VIEW IF NOT EXISTS view_raw_entities AS SELECT r._id AS _id, r.contact_id AS contact_id, "
            + RAW_COLS + ", r.starred AS starred, r.custom_ringtone AS custom_ringtone,"
            + " r.send_to_voicemail AS send_to_voicemail, r.display_name AS display_name,"
            + " d._id AS data_id, " + DATA_COLS
            + " FROM raw_contacts r LEFT JOIN data d ON d.raw_contact_id = r._id",
        "CREATE VIEW IF NOT EXISTS view_contact_entities AS SELECT c._id AS _id, c._id AS contact_id,"
            + " r._id AS raw_contact_id, " + RAW_COLS + ", " + CONTACT_COLS + ", d._id AS data_id, "
            + DATA_COLS
            + " FROM contacts c JOIN raw_contacts r ON r.contact_id = c._id"
            + " LEFT JOIN raw_contacts n ON n._id = c.name_raw_contact_id"
            + " LEFT JOIN data d ON d.raw_contact_id = r._id WHERE r.deleted = 0",
        "CREATE VIEW IF NOT EXISTS view_groups AS SELECT g._id AS _id, g.account_name AS account_name,"
            + " g.account_type AS account_type, g.data_set AS data_set,"
            + " CASE WHEN g.data_set IS NULL THEN g.account_type ELSE g.account_type || '/' || g.data_set END"
            + " AS account_type_and_data_set, g.sourceid AS sourceid, g.version AS version,"
            + " g.dirty AS dirty, g.title AS title, g.title_res AS title_res, g.notes AS notes,"
            + " g.system_id AS system_id, g.deleted AS deleted, g.group_visible AS group_visible,"
            + " g.should_sync AS should_sync, g.auto_add AS auto_add, g.favorites AS favorites,"
            + " g.group_is_read_only AS group_is_read_only, g.res_package AS res_package,"
            + " g.sync1 AS sync1, g.sync2 AS sync2, g.sync3 AS sync3, g.sync4 AS sync4,"
            + " (SELECT COUNT(DISTINCT r.contact_id) FROM data d JOIN raw_contacts r ON r._id = d.raw_contact_id"
            + " WHERE d.mimetype = '" + MIME_GROUP_MEMBERSHIP + "' AND d.data1 = g._id AND r.deleted = 0)"
            + " AS summ_count,"
            + " (SELECT COUNT(DISTINCT r.contact_id) FROM data d JOIN raw_contacts r ON r._id = d.raw_contact_id"
            + " JOIN contacts c ON c._id = r.contact_id WHERE d.mimetype = '" + MIME_GROUP_MEMBERSHIP + "'"
            + " AND d.data1 = g._id AND r.deleted = 0 AND c.has_phone_number = 1) AS summ_phones"
            + " FROM groups g",
        // PhoneLookup's columns: the contact's, and the matched number's.
        "CREATE VIEW IF NOT EXISTS view_phone_lookup AS SELECT contact_id AS _id, contact_id, lookup,"
            + " display_name, display_name_alt, display_name_source, phonetic_name, sort_key, starred,"
            + " pinned, has_phone_number, photo_id, photo_file_id, photo_uri, photo_thumb_uri,"
            + " custom_ringtone, send_to_voicemail, in_visible_group, in_default_directory,"
            + " times_contacted, last_time_contacted, contact_last_updated_timestamp,"
            + " contact_presence, contact_status, data1 AS number, data2 AS type, data3 AS label,"
            + " data4 AS normalized_number, _id AS data_id, mimetype FROM view_data",
    };

    // ---- the write transaction -------------------------------------------------------

    private void beginWrite() {
        if (mWriteDepth++ > 0) return;
        mChanged = false;
        mChangedByApp = false;
        mDb.beginTransaction();
        // The write lock before any read: SQLite's busy wait cannot settle two processes that
        // each read and then both ask to write, so take the writer's place first.
        mDb.execSQL("UPDATE write_lock SET n = n + 1");
    }

    private void markChanged(boolean syncAdapter) {
        mChanged = true;
        if (!syncAdapter) mChangedByApp = true;
    }

    /** Ends the write; commits when `ok` at the outermost level, then tells the observers. */
    private void endWrite(boolean ok) {
        if (--mWriteDepth > 0) {
            if (!ok) mWriteFailed = true;
            return;
        }
        boolean notify = false;
        boolean byApp = mChangedByApp;
        try {
            if (ok && !mWriteFailed) mDb.setTransactionSuccessful();
            notify = ok && !mWriteFailed && mChanged;
        } finally {
            mWriteFailed = false;
            mDb.endTransaction();
        }
        if (notify) notifyContacts(byApp);
    }
    private boolean mWriteFailed;

    private void notifyContacts(boolean byApp) {
        Context c = getContext();
        if (c == null) return;
        // ContactsProvider2.notifyChange(syncToNetwork): a sync adapter's own change is
        // not sent back to the network.
        c.getContentResolver().notifyChange(ContactsContract.AUTHORITY_URI, null, byApp);
    }

    // ---- query ---------------------------------------------------------------------

    /** One query route: a view, a where clause with its arguments, a default sort. */
    private static final class Q {
        String view;
        final StringBuilder where = new StringBuilder();
        final List<String> args = new ArrayList<>();
        String sort;
        void and(String clause, String... a) {
            if (where.length() > 0) where.append(" AND ");
            where.append('(').append(clause).append(')');
            args.addAll(Arrays.asList(a));
        }
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs,
                        String sortOrder) {
        synchronized (mLock) {
            if (mDb == null) return null;
            List<String> s = uri.getPathSegments();
            Cursor special = querySpecial(uri, s, projection);
            if (special != null) return special;
            Q q = route(uri, s);
            if (q == null) throw new IllegalArgumentException("Unknown URI " + uri);
            long directory = longParam(uri, "directory", 0);
            if (directory != 0 && directory != 1) q.and("0");   // no other directory
            String[] columns = checkedProjection(mDb, q.view, projection);
            String where = andSelection(mDb, q.view, q.where.toString(), selection);
            List<String> args = new ArrayList<>(q.args);
            if (selectionArgs != null && selection != null && !selection.isEmpty()) {
                args.addAll(Arrays.asList(selectionArgs));
            }
            String limit = uri.getQueryParameter(ContactsContract.LIMIT_PARAM_KEY);
            if (limit != null && !limit.matches("\\d+")) limit = null;
            String order = sortOrder != null && !sortOrder.isEmpty() ? sortOrder : q.sort;
            Cursor c = mDb.query(q.view, columns, where.isEmpty() ? null : where,
                    args.toArray(new String[0]), null, null, order, limit);
            return copy(getContext(), c, ContactsContract.AUTHORITY_URI);
        }
    }

    // ---- what an app's selection and projection may say ----------------------------------

    /** `where`, the provider's own clause, ANDed with an app's `selection` as AOSP's
     *  SQLiteQueryBuilder does in strict mode (setStrict(true), which ContactsProvider2 and
     *  CallLogProvider set): the selection is compiled on its own first, wrapped once, and
     *  runs wrapped twice, so a selection such as "1) OR (1" cannot close the provider's
     *  parenthesis and escape its clause (voicemail, deleted rows, one contact's URI).
     *  A selection that is not SQL fails with the SQLiteException validateSql throws on a
     *  device; one that compiles wrapped once but not twice is one that would escape the
     *  single wrap (a comment or a statement end eating the closing parenthesis), and is
     *  refused. Empty when neither is given. */
    static String andSelection(SQLiteDatabase db, String table, String where, String selection) {
        if (where == null) where = "";
        if (selection == null || selection.isEmpty()) return where;
        db.validateSql("SELECT 1 FROM " + table + " WHERE (" + selection + ")", null);
        try {
            db.validateSql("SELECT 1 FROM " + table + " WHERE ((" + selection + "))", null);
        } catch (SQLiteException e) {
            throw new IllegalArgumentException("Invalid selection: " + selection);
        }
        String wrapped = "((" + selection + "))";
        return where.isEmpty() ? wrapped : where + " AND " + wrapped;
    }

    /** The columns of each table and view the providers query, read from the schema once. */
    private static final Map<String, Set<String>> sColumns = new ConcurrentHashMap<>();

    /** AOSP SQLiteQueryBuilder's aggregate form of a mapped column ("COUNT(_id)"). */
    private static final Pattern AGGREGATE =
            Pattern.compile("(?i)(AVG|COUNT|MAX|MIN|SUM|TOTAL|GROUP_CONCAT)\\((.+)\\)");

    /** An app's projection checked as ContactsProvider2's and CallLogProvider's projection
     *  maps check it (SQLiteQueryBuilder.setStrictColumns): each entry is a column of the
     *  table or view, or an aggregate of one; anything else, an expression or a subquery
     *  included, is refused with AOSP's "Invalid column". */
    static String[] checkedProjection(SQLiteDatabase db, String table, String[] projection) {
        if (projection == null) return null;
        Set<String> known = sColumns.computeIfAbsent(table, t -> {
            try (Cursor c = db.rawQuery("SELECT * FROM " + t + " LIMIT 0", null)) {
                return new HashSet<>(Arrays.asList(c.getColumnNames()));
            }
        });
        for (String column : projection) {
            if (column != null && known.contains(column)) continue;
            Matcher m = column == null ? null : AGGREGATE.matcher(column);
            if (m != null && m.matches() && known.contains(m.group(2))) continue;
            throw new IllegalArgumentException("Invalid column " + column);
        }
        return projection;
    }

    /** The rows of `c` copied out and `c` closed, as the CursorWindow a provider's cursor
     *  crosses Binder in: the statement does not stay open in the database (another
     *  process's writer would wait on it). Changes under `notify` refresh it. */
    static Cursor copy(Context ctx, Cursor c, Uri notify) {
        if (c == null) return null;
        try {
            String[] cols = c.getColumnNames();
            MatrixCursor out = new MatrixCursor(cols, Math.max(c.getCount(), 1));
            while (c.moveToNext()) {
                Object[] row = new Object[cols.length];
                for (int i = 0; i < cols.length; i++) {
                    switch (c.getType(i)) {
                        case Cursor.FIELD_TYPE_INTEGER: row[i] = c.getLong(i); break;
                        case Cursor.FIELD_TYPE_FLOAT: row[i] = c.getDouble(i); break;
                        case Cursor.FIELD_TYPE_STRING: row[i] = c.getString(i); break;
                        case Cursor.FIELD_TYPE_BLOB: row[i] = c.getBlob(i); break;
                        default: row[i] = null; break;
                    }
                }
                out.addRow(row);
            }
            if (ctx != null && notify != null) out.setNotificationUri(ctx.getContentResolver(), notify);
            return out;
        } finally {
            c.close();
        }
    }

    /** The URIs whose rows are not a view's: directories, provider status, phone lookup. */
    private Cursor querySpecial(Uri uri, List<String> s, String[] projection) {
        if (s.isEmpty()) return null;
        switch (s.get(0)) {
            case "directories": {
                // The two directories ContactsProvider2's DirectoryProvider always has.
                String[] cols = {"_id", "packageName", "typeResourceId", "displayName", "authority",
                        "accountType", "accountName", "exportSupport", "shortcutSupport", "photoSupport"};
                MatrixCursor m = new MatrixCursor(cols);
                for (long id : new long[]{0, 1}) {
                    if (s.size() > 1 && !String.valueOf(id).equals(s.get(1))) continue;
                    m.addRow(new Object[]{id, PACKAGE, 0, null, ContactsContract.AUTHORITY, null, null,
                            0, 2, 3});
                }
                return project(m, projection);
            }
            case "provider_status": {
                long n = longQuery("SELECT COUNT(*) FROM contacts", null);
                MatrixCursor m = new MatrixCursor(new String[]{"status", "database_creation_timestamp"});
                m.addRow(new Object[]{n == 0 ? 2 : 0, 0L});
                return project(m, projection);
            }
            default:
                return null;
        }
    }

    private static Cursor project(MatrixCursor m, String[] projection) {
        if (projection == null) return m;
        String[] all = m.getColumnNames();
        MatrixCursor out = new MatrixCursor(projection);
        while (m.moveToNext()) {
            Object[] row = new Object[projection.length];
            for (int i = 0; i < projection.length; i++) {
                int at = Arrays.asList(all).indexOf(projection[i]);
                if (at < 0) throw new IllegalArgumentException("Invalid column " + projection[i]);
                switch (m.getType(at)) {
                    case Cursor.FIELD_TYPE_INTEGER: row[i] = m.getLong(at); break;
                    case Cursor.FIELD_TYPE_STRING: row[i] = m.getString(at); break;
                    default: row[i] = null; break;
                }
            }
            out.addRow(row);
        }
        m.close();
        return out;
    }

    /** ContactsProvider2's URI matcher, for queries. Null for a URI it does not serve. */
    private Q route(Uri uri, List<String> s) {
        final int n = s.size();
        if (n == 0) return null;
        Q q = new Q();
        String a = s.get(0);
        switch (a) {
            case "contacts": {
                q.view = "view_contacts";
                if (n == 1) return q;
                String b = s.get(1);
                if (isId(b)) {
                    if (n == 2) { q.and("_id = ?", b); return q; }
                    return contactSub(q, b, s, 2);
                }
                if (b.equals("lookup") && n >= 3) {
                    String id = n >= 4 && isId(s.get(3)) ? s.get(3) : null;
                    long contact = lookupContactId(s.get(2), id);
                    String cid = String.valueOf(contact);
                    int next = id != null ? 4 : 3;
                    if (n == next) { q.and("_id = ?", cid); return q; }
                    return contactSub(q, cid, s, next);
                }
                if (b.equals("filter")) {
                    q.sort = "sort_key";
                    String f = n >= 3 ? s.get(2) : "";
                    q.and(contactFilter(f, q));
                    return q;
                }
                if (b.equals("strequent")) {
                    // Since Android 10 the provider keeps no usage counts: "strequent" is the
                    // starred contacts and "frequent" is empty.
                    q.and("starred = 1");
                    if (n >= 4 && s.get(2).equals("filter")) q.and(contactFilter(s.get(3), q));
                    q.sort = "sort_key";
                    return q;
                }
                if (b.equals("frequent")) { q.and("0"); return q; }
                return null;
            }
            case "raw_contacts": {
                q.view = "view_raw_contacts";
                accountParams(uri, q);
                if (n == 1) return q;
                if (!isId(s.get(1))) return null;
                if (n == 2) { q.and("_id = ?", s.get(1)); return q; }
                if (n == 3 && s.get(2).equals("data")) {
                    q.view = "view_data";
                    q.and("raw_contact_id = ?", s.get(1));
                    return q;
                }
                if (n == 3 && s.get(2).equals("entity")) {
                    q.view = "view_raw_entities";
                    q.and("_id = ?", s.get(1));
                    return q;
                }
                return null;
            }
            case "raw_contact_entities": {
                q.view = "view_raw_entities";
                accountParams(uri, q);
                if (!isSyncAdapter(uri)) q.and("deleted = 0");
                return n == 1 ? q : null;
            }
            case "data": {
                q.view = "view_data";
                if (n == 1) return q;
                String b = s.get(1);
                if (isId(b) && n == 2) { q.and("_id = ?", b); return q; }
                return dataKind(q, s);
            }
            case "phone_lookup": {
                if (n < 2) return null;
                q.view = "view_phone_lookup";
                String number = s.get(1);
                if (uri.getBooleanQueryParameter(ContactsContract.PhoneLookup.QUERY_PARAMETER_SIP_ADDRESS, false)) {
                    q.and("mimetype = ? AND number = ?", MIME_SIP, number);
                } else {
                    q.and("mimetype = ?", MIME_PHONE);
                    q.and("data_id IN (" + joinIds(phoneMatches(number)) + ")");
                }
                return q;
            }
            case "groups": {
                q.view = "view_groups";
                accountParams(uri, q);
                if (n == 1) return q;
                if (n == 2 && isId(s.get(1))) { q.and("_id = ?", s.get(1)); return q; }
                return null;
            }
            case "groups_summary":
                q.view = "view_groups";
                accountParams(uri, q);
                return n == 1 ? q : null;
            case "aggregation_exceptions":
                q.view = "agg_exceptions";
                return n == 1 ? q : null;
            case "deleted_contacts":
                q.view = "deleted_contacts";
                if (n == 1) return q;
                if (n == 2 && isId(s.get(1))) { q.and("contact_id = ?", s.get(1)); return q; }
                return null;
            case "profile": {
                // The device owner's profile: none here, so its rows are none, in the
                // columns of the matching table.
                q.view = n >= 2 && s.get(1).equals("raw_contacts") ? "view_raw_contacts"
                        : n >= 2 && s.get(1).equals("data") ? "view_data"
                        : n >= 2 && s.get(1).equals("raw_contact_entities") ? "view_raw_entities"
                        : "view_contacts";
                q.and("0");
                return q;
            }
            default:
                return null;
        }
    }

    /** contacts/<id>/data, /entities, /photo. */
    private static Q contactSub(Q q, String contactId, List<String> s, int at) {
        if (s.size() != at + 1) return null;
        switch (s.get(at)) {
            case "data":
                q.view = "view_data";
                q.and("contact_id = ?", contactId);
                return q;
            case "entities":
                q.view = "view_contact_entities";
                q.and("contact_id = ?", contactId);
                return q;
            case "photo":
                q.view = "view_data";
                q.and("_id = (SELECT photo_id FROM contacts WHERE _id = ?)", contactId);
                return q;
            default:
                return null;
        }
    }

    /** data/phones, data/emails, data/postals, data/callables, data/contactables, with an
     *  id, a filter or (emails) a lookup. */
    private Q dataKind(Q q, List<String> s) {
        final int n = s.size();
        String kind = s.get(1);
        String[] mimes;
        switch (kind) {
            case "phones": mimes = new String[]{MIME_PHONE}; break;
            case "emails": mimes = new String[]{MIME_EMAIL}; break;
            case "postals": mimes = new String[]{MIME_POSTAL}; break;
            case "callables": mimes = new String[]{MIME_PHONE, MIME_SIP}; break;
            case "contactables": mimes = new String[]{MIME_PHONE, MIME_EMAIL}; break;
            default: return null;
        }
        StringBuilder in = new StringBuilder();
        for (String m : mimes) in.append(in.length() == 0 ? "" : ", ").append('\'').append(m).append('\'');
        q.and("mimetype IN (" + in + ")");
        if (n == 2) return q;
        String c = s.get(2);
        if (n == 3 && isId(c)) { q.and("_id = ?", c); return q; }
        if (c.equals("filter")) {
            String f = n >= 4 ? s.get(3) : "";
            q.sort = "sort_key";
            if (f.trim().isEmpty()) { q.and("0"); return q; }
            String like = likePrefix(f.trim());
            StringBuilder w = new StringBuilder(nameMatch(q, f));
            String digits = digitsOf(f);
            if (kind.equals("emails") || kind.equals("contactables")) {
                w.append(" OR (mimetype = '").append(MIME_EMAIL).append("' AND data1 LIKE ? ESCAPE '\\')");
                q.args.add(like);
            }
            if (!digits.isEmpty() && !kind.equals("emails") && !kind.equals("postals")) {
                w.append(" OR _id IN (SELECT data_id FROM phone_lookup"
                        + " WHERE REPLACE(normalized_number, '+', '') LIKE ?)");
                q.args.add(digits + "%");
            }
            q.and(w.toString());
            return q;
        }
        if (kind.equals("emails") && c.equals("lookup") && n == 4) {
            List<String> addresses = new ArrayList<>();
            for (String part : s.get(3).split(",")) if (!part.trim().isEmpty()) addresses.add(part.trim());
            if (addresses.isEmpty()) { q.and("0"); return q; }
            StringBuilder w = new StringBuilder();
            for (String addr : addresses) {
                w.append(w.length() == 0 ? "" : " OR ").append("data1 = ? COLLATE NOCASE");
                q.args.add(addr);
            }
            q.and(w.toString());
            return q;
        }
        return null;
    }

    /** Contacts' filter: a name word starting with the text, or an email address or a phone
     *  number starting with it. An empty filter matches nothing. */
    private String contactFilter(String filter, Q q) {
        if (filter.trim().isEmpty()) return "0";
        StringBuilder w = new StringBuilder(nameMatch(q, filter));
        w.append(" OR _id IN (SELECT r.contact_id FROM data d JOIN raw_contacts r ON r._id = d.raw_contact_id"
                + " WHERE r.deleted = 0 AND d.mimetype IN ('" + MIME_EMAIL + "', '" + MIME_NICKNAME + "')"
                + " AND d.data1 LIKE ? ESCAPE '\\')");
        q.args.add(likePrefix(filter.trim()));
        String digits = digitsOf(filter);
        if (!digits.isEmpty()) {
            w.append(" OR _id IN (SELECT r.contact_id FROM phone_lookup p JOIN raw_contacts r"
                    + " ON r._id = p.raw_contact_id WHERE r.deleted = 0"
                    + " AND REPLACE(p.normalized_number, '+', '') LIKE ?)");
            q.args.add(digits + "%");
        }
        return w.toString();
    }

    /** A display name (either order) with a word starting with the filter's text. */
    private static String nameMatch(Q q, String filter) {
        String f = filter.trim();
        String prefix = likePrefix(f);
        String word = "% " + likeEscape(f) + "%";
        q.args.add(prefix);
        q.args.add(word);
        q.args.add(prefix);
        return "display_name LIKE ? ESCAPE '\\' OR display_name LIKE ? ESCAPE '\\'"
                + " OR display_name_alt LIKE ? ESCAPE '\\'";
    }

    private static String likeEscape(String s) {
        return s.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_");
    }

    private static String likePrefix(String s) { return likeEscape(s) + "%"; }

    private static String digitsOf(String s) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            if (ch >= '0' && ch <= '9') b.append(ch);
        }
        return b.toString();
    }

    private static boolean isId(String s) { return s != null && !s.isEmpty() && s.matches("\\d+"); }

    private static long longParam(Uri uri, String key, long dflt) {
        String v = uri.getQueryParameter(key);
        if (v == null) return dflt;
        try { return Long.parseLong(v); } catch (NumberFormatException e) { return dflt; }
    }

    static boolean isSyncAdapter(Uri uri) {
        return uri.getBooleanQueryParameter(ContactsContract.CALLER_IS_SYNCADAPTER, false);
    }

    /** RawContacts' and Groups' account_name / account_type / data_set query parameters. */
    private static void accountParams(Uri uri, Q q) {
        String name = uri.getQueryParameter("account_name");
        String type = uri.getQueryParameter("account_type");
        if (name == null && type == null) return;
        if (name == null || type == null) {
            throw new IllegalArgumentException("Must specify both or neither of account_name and account_type");
        }
        q.and("account_name = ? AND account_type = ?", name, type);
        String set = uri.getQueryParameter("data_set");
        if (set != null) q.and("data_set = ?", set);
    }

    private static String joinIds(Iterable<Long> ids) {
        StringBuilder b = new StringBuilder();
        for (long id : ids) b.append(b.length() == 0 ? "" : ",").append(id);
        return b.length() == 0 ? "-1" : b.toString();
    }

    /** The phone rows whose number matches `number` as caller id matches it: the same last
     *  seven digits (min match), then PhoneNumberUtils.compare, as PhoneLookup does. */
    private List<Long> phoneMatches(String number) {
        List<Long> out = new ArrayList<>();
        String min = PhoneNumberUtils.toCallerIDMinMatch(number);
        if (min == null || min.isEmpty()) return out;
        String normalized = PhoneNumberUtils.normalizeNumber(number);
        try (Cursor c = mDb.rawQuery("SELECT p.data_id, p.normalized_number FROM phone_lookup p"
                + " JOIN raw_contacts r ON r._id = p.raw_contact_id WHERE r.deleted = 0 AND p.min_match = ?",
                new String[]{min})) {
            while (c.moveToNext()) {
                if (PhoneNumberUtils.compare(normalized, c.getString(1))) out.add(c.getLong(0));
            }
        }
        return out;
    }

    /** The contact a lookup key names: the contact with that key; else the one with the id
     *  given beside it; else the contact of a raw contact one of its segments names. -1 for none. */
    private long lookupContactId(String key, String idOrNull) {
        long id = longQuery("SELECT _id FROM contacts WHERE lookup = ?", new String[]{key});
        if (id > 0) return id;
        if (idOrNull != null) {
            id = longQuery("SELECT _id FROM contacts WHERE _id = ?", new String[]{idOrNull});
            if (id > 0) return id;
        }
        for (String seg : key.split("\\.")) {
            int r = seg.indexOf('r');
            if (r < 0) continue;
            String raw = seg.substring(r + 1);
            int dash = raw.indexOf('-');
            if (dash >= 0) raw = raw.substring(0, dash);
            if (!isId(raw)) continue;
            id = longQuery("SELECT contact_id FROM raw_contacts WHERE _id = ? AND deleted = 0", new String[]{raw});
            if (id > 0) return id;
        }
        return -1;
    }

    private long longQuery(String sql, String[] args) {
        try (Cursor c = mDb.rawQuery(sql, args)) {
            return c.moveToFirst() && !c.isNull(0) ? c.getLong(0) : -1;
        }
    }

    private List<Long> idsQuery(String sql, String[] args) {
        List<Long> out = new ArrayList<>();
        try (Cursor c = mDb.rawQuery(sql, args)) {
            while (c.moveToNext()) if (!c.isNull(0)) out.add(c.getLong(0));
        }
        return out;
    }

    // ---- getType ---------------------------------------------------------------------

    @Override
    public String getType(Uri uri) {
        List<String> s = uri.getPathSegments();
        final int n = s.size();
        if (n == 0) return null;
        switch (s.get(0)) {
            case "contacts":
                if (n == 1 || s.get(1).equals("filter") || s.get(1).equals("strequent")) {
                    return "vnd.android.cursor.dir/contact";
                }
                if (s.get(n - 1).equals("photo")) return "image/jpeg";
                if (s.get(n - 1).equals("data") || s.get(n - 1).equals("entities")) return null;
                return "vnd.android.cursor.item/contact";
            case "raw_contacts":
                if (n == 1) return "vnd.android.cursor.dir/raw_contact";
                if (n == 2) return "vnd.android.cursor.item/raw_contact";
                return null;
            case "raw_contact_entities":
                return "vnd.android.cursor.dir/raw_contact_entity";
            case "data": {
                if (n == 1) return "vnd.android.cursor.dir/data";
                if (n == 2 && isId(s.get(1))) {
                    synchronized (mLock) {
                        if (mDb == null) return null;
                        try (Cursor c = mDb.rawQuery("SELECT mimetype FROM data WHERE _id = ?",
                                new String[]{s.get(1)})) {
                            return c.moveToFirst() ? c.getString(0) : null;
                        }
                    }
                }
                String item = n == 3 && isId(s.get(2)) ? "item" : "dir";
                switch (s.get(1)) {
                    case "phones": return "vnd.android.cursor." + item + "/phone_v2";
                    case "emails": return "vnd.android.cursor." + item + "/email_v2";
                    case "postals": return "vnd.android.cursor." + item + "/postal-address_v2";
                    default: return null;
                }
            }
            case "phone_lookup": return "vnd.android.cursor.dir/phone_lookup";
            case "groups": return n == 1 ? "vnd.android.cursor.dir/group" : "vnd.android.cursor.item/group";
            case "aggregation_exceptions": return "vnd.android.cursor.dir/aggregation_exception";
            case "provider_status": return "vnd.android.cursor.dir/provider_status";
            case "directories":
                return n == 1 ? "vnd.android.cursor.dir/contact_directories"
                              : "vnd.android.cursor.item/contact_directory";
            default: return null;
        }
    }

    // ---- insert ----------------------------------------------------------------------

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        synchronized (mLock) {
            if (mDb == null) return null;
            List<String> s = uri.getPathSegments();
            final boolean sync = isSyncAdapter(uri);
            ContentValues v = values != null ? new ContentValues(values) : new ContentValues();
            boolean ok = false;
            beginWrite();
            try {
                Uri out;
                if (s.size() == 1 && s.get(0).equals("raw_contacts")) {
                    out = ContentUris.withAppendedId(ContactsContract.RawContacts.CONTENT_URI,
                            insertRawContact(uri, v, sync));
                } else if (s.size() == 1 && s.get(0).equals("data")) {
                    out = ContentUris.withAppendedId(ContactsContract.Data.CONTENT_URI, insertData(v, sync));
                } else if (s.size() == 3 && s.get(0).equals("raw_contacts") && isId(s.get(1))
                        && s.get(2).equals("data")) {
                    v.put("raw_contact_id", Long.parseLong(s.get(1)));
                    out = ContentUris.withAppendedId(ContactsContract.Data.CONTENT_URI, insertData(v, sync));
                } else if (s.size() == 1 && s.get(0).equals("groups")) {
                    out = ContentUris.withAppendedId(ContactsContract.Groups.CONTENT_URI, insertGroup(uri, v, sync));
                } else if (s.size() >= 1 && s.get(0).equals("contacts")) {
                    throw new UnsupportedOperationException("Aggregate contacts are created automatically");
                } else {
                    throw new UnsupportedOperationException("Unknown uri: " + uri);
                }
                markChanged(sync);
                ok = true;
                return out;
            } finally {
                endWrite(ok);
            }
        }
    }

    private static final Set<String> RAW_WRITABLE = new LinkedHashSet<>(Arrays.asList(
            "account_name", "account_type", "data_set", "sourceid", "backup_id", "version", "dirty",
            "deleted", "metadata_dirty", "aggregation_mode", "custom_ringtone", "send_to_voicemail",
            "starred", "pinned", "times_contacted", "last_time_contacted", "sync1", "sync2", "sync3",
            "sync4"));

    private long insertRawContact(Uri uri, ContentValues v, boolean sync) {
        String pName = uri.getQueryParameter("account_name");
        String pType = uri.getQueryParameter("account_type");
        if (pName != null) v.put("account_name", pName);
        if (pType != null) v.put("account_type", pType);
        String set = uri.getQueryParameter("data_set");
        if (set != null) v.put("data_set", set);
        String name = v.getAsString("account_name");
        String type = v.getAsString("account_type");
        if ((name == null || name.isEmpty()) != (type == null || type.isEmpty())) {
            throw new IllegalArgumentException("Must specify both or neither of ACCOUNT_NAME and ACCOUNT_TYPE");
        }
        ContentValues row = only(v, RAW_WRITABLE);
        row.remove("deleted");
        if (!sync) row.put("dirty", 1);
        long raw = mDb.insertOrThrow("raw_contacts", null, row);
        ContentValues c = new ContentValues();
        c.put("name_raw_contact_id", raw);
        long contact = mDb.insertOrThrow("contacts", null, c);
        ContentValues link = new ContentValues();
        link.put("contact_id", contact);
        mDb.update("raw_contacts", link, "_id = ?", new String[]{String.valueOf(raw)});
        updateRawDisplayName(raw);
        updateContact(contact);
        return raw;
    }

    private static final Set<String> DATA_WRITABLE = new LinkedHashSet<>(Arrays.asList(
            "raw_contact_id", "mimetype", "is_primary", "is_super_primary", "is_read_only", "data_version",
            "data1", "data2", "data3", "data4", "data5", "data6", "data7", "data8", "data9", "data10",
            "data11", "data12", "data13", "data14", "data15", "data_sync1", "data_sync2", "data_sync3",
            "data_sync4", "preferred_phone_account_component_name", "preferred_phone_account_id",
            "carrier_presence"));

    private long insertData(ContentValues v, boolean sync) {
        Long raw = v.getAsLong("raw_contact_id");
        if (raw == null) throw new IllegalArgumentException("raw_contact_id is required");
        String mime = v.getAsString("mimetype");
        if (mime == null || mime.isEmpty()) throw new IllegalArgumentException("mimetype is required");
        if (longQuery("SELECT _id FROM raw_contacts WHERE _id = ?", new String[]{String.valueOf(raw)}) < 0) {
            throw new IllegalArgumentException("Raw contact " + raw + " does not exist");
        }
        ContentValues row = only(v, DATA_WRITABLE);
        fixUpData(mime, row);
        if (asBool(row.get("is_super_primary"))) row.put("is_primary", 1);
        long id = mDb.insertOrThrow("data", null, row);
        afterDataChange(id, raw, mime, row, sync);
        return id;
    }

    /** What a data row of `mime` gets filled in, as ContactsProvider2's DataRowHandlers do. */
    private static void fixUpData(String mime, ContentValues row) {
        if (MIME_NAME.equals(mime)) {
            fixUpStructuredName(row);
        } else if (MIME_PHONE.equals(mime) && row.containsKey("data1") && !row.containsKey("data4")) {
            String e164 = PhoneNumberUtils.formatNumberToE164(row.getAsString("data1"),
                    Locale.getDefault().getCountry());
            if (e164 != null) row.put("data4", e164);
        }
    }

    /** StructuredName: the display name joined from its parts, or the parts split from a
     *  display name given alone (NameSplitter, simplified). */
    static void fixUpStructuredName(ContentValues row) {
        String display = row.getAsString("data1");
        String given = row.getAsString("data2"), family = row.getAsString("data3");
        String prefix = row.getAsString("data4"), middle = row.getAsString("data5");
        String suffix = row.getAsString("data6");
        boolean parts = !empty(given) || !empty(family) || !empty(prefix) || !empty(middle) || !empty(suffix);
        if (empty(display) && parts) {
            String joined = join(" ", prefix, given, middle, family);
            if (!empty(suffix)) joined = joined.isEmpty() ? suffix : joined + ", " + suffix;
            row.put("data1", joined);
        } else if (!empty(display) && !parts) {
            String[] words = display.trim().split("\\s+");
            if (words.length == 1) {
                row.put("data2", words[0]);
            } else {
                row.put("data2", words[0]);
                row.put("data3", words[words.length - 1]);
                if (words.length > 2) {
                    row.put("data5", String.join(" ", Arrays.copyOfRange(words, 1, words.length - 1)));
                }
            }
        }
    }

    private static String join(String sep, String... parts) {
        StringBuilder b = new StringBuilder();
        for (String p : parts) {
            if (empty(p)) continue;
            if (b.length() > 0) b.append(sep);
            b.append(p.trim());
        }
        return b.toString();
    }

    private static boolean empty(String s) { return s == null || s.trim().isEmpty(); }

    private static boolean asBool(Object o) {
        if (o == null) return false;
        if (o instanceof Boolean) return (Boolean) o;
        if (o instanceof Number) return ((Number) o).intValue() != 0;
        String s = o.toString();
        return s.equals("1") || s.equalsIgnoreCase("true");
    }

    private static ContentValues only(ContentValues v, Set<String> allowed) {
        ContentValues out = new ContentValues();
        for (String k : v.keySet()) {
            if (!allowed.contains(k)) throw new IllegalArgumentException("Invalid column " + k);
            Object o = v.get(k);
            if (o == null) out.putNull(k);
            else if (o instanceof byte[]) out.put(k, (byte[]) o);
            else if (o instanceof Boolean) out.put(k, ((Boolean) o) ? 1 : 0);
            else if (o instanceof Long) out.put(k, (Long) o);
            else if (o instanceof Integer) out.put(k, (Integer) o);
            else if (o instanceof Double) out.put(k, (Double) o);
            else if (o instanceof Float) out.put(k, (Float) o);
            else out.put(k, o.toString());
        }
        return out;
    }

    /** After a data row was written: the phone lookup row, the primaries, the raw contact's
     *  name and its contact's aggregate, dirty and version. */
    private void afterDataChange(long dataId, long raw, String mime, ContentValues row, boolean sync) {
        String id = String.valueOf(dataId);
        if (MIME_PHONE.equals(mime)) {
            mDb.delete("phone_lookup", "data_id = ?", new String[]{id});
            String number = null;
            try (Cursor c = mDb.rawQuery("SELECT data1 FROM data WHERE _id = ?", new String[]{id})) {
                if (c.moveToFirst()) number = c.getString(0);
            }
            if (!empty(number)) {
                ContentValues p = new ContentValues();
                p.put("data_id", dataId);
                p.put("raw_contact_id", raw);
                p.put("normalized_number", PhoneNumberUtils.normalizeNumber(number));
                p.put("min_match", PhoneNumberUtils.toCallerIDMinMatch(number));
                mDb.insertOrThrow("phone_lookup", null, p);
            }
        }
        if (row != null && asBool(row.get("is_super_primary"))) {
            // One super primary of a kind per contact, one primary per raw contact.
            mDb.execSQL("UPDATE data SET is_super_primary = 0 WHERE mimetype = ? AND _id != ? AND raw_contact_id IN"
                    + " (SELECT _id FROM raw_contacts WHERE contact_id ="
                    + " (SELECT contact_id FROM raw_contacts WHERE _id = ?))",
                    new Object[]{mime, dataId, raw});
        }
        if (row != null && asBool(row.get("is_primary"))) {
            mDb.execSQL("UPDATE data SET is_primary = 0 WHERE mimetype = ? AND _id != ? AND raw_contact_id = ?",
                    new Object[]{mime, dataId, raw});
        }
        touchRaw(raw, sync);
        updateRawDisplayName(raw);
        updateContact(contactOf(raw));
    }

    /** A change not a sync adapter's: the raw contact is dirty and one version newer. */
    private void touchRaw(long raw, boolean sync) {
        if (sync) {
            mDb.execSQL("UPDATE raw_contacts SET version = version + 1 WHERE _id = ?", new Object[]{raw});
        } else {
            mDb.execSQL("UPDATE raw_contacts SET version = version + 1, dirty = 1 WHERE _id = ?",
                    new Object[]{raw});
        }
    }

    private long contactOf(long raw) {
        return longQuery("SELECT contact_id FROM raw_contacts WHERE _id = ?", new String[]{String.valueOf(raw)});
    }

    /** The raw contact's display name from its best source (DisplayNameSources). */
    private void updateRawDisplayName(long raw) {
        int bestSource = SOURCE_UNDEFINED;
        String display = null, alt = null, phonetic = null;
        try (Cursor c = mDb.rawQuery("SELECT mimetype, data1, data2, data3, data4, data5, data7, data8, data9"
                + " FROM data WHERE raw_contact_id = ? ORDER BY is_super_primary DESC, is_primary DESC, _id",
                new String[]{String.valueOf(raw)})) {
            while (c.moveToNext()) {
                String mime = c.getString(0);
                String d1 = c.isNull(1) ? null : c.getString(1);
                int source = SOURCE_UNDEFINED;
                String name = null, nameAlt = null;
                if (MIME_NAME.equals(mime)) {
                    String given = c.getString(2), family = c.getString(3), middle = c.getString(5);
                    String ph = join(" ", c.getString(6), c.getString(7), c.getString(8));
                    if (!empty(d1)) {
                        source = SOURCE_STRUCTURED_NAME;
                        name = d1;
                        nameAlt = empty(family) ? d1
                                : family.trim() + (empty(given) && empty(middle) ? ""
                                        : ", " + join(" ", given, middle));
                    } else if (!ph.isEmpty()) {
                        source = SOURCE_PHONETIC;
                        name = ph;
                    }
                    if (!ph.isEmpty() && phonetic == null) phonetic = ph;
                } else if (MIME_NICKNAME.equals(mime) && !empty(d1)) {
                    source = SOURCE_NICKNAME; name = d1;
                } else if (MIME_ORGANIZATION.equals(mime)) {
                    String org = !empty(d1) ? d1 : c.getString(4);
                    if (!empty(org)) { source = SOURCE_ORGANIZATION; name = org; }
                } else if (MIME_PHONE.equals(mime) && !empty(d1)) {
                    source = SOURCE_PHONE; name = d1;
                } else if (MIME_EMAIL.equals(mime) && !empty(d1)) {
                    source = SOURCE_EMAIL; name = d1;
                }
                if (source > bestSource) {
                    bestSource = source;
                    display = name;
                    alt = nameAlt != null ? nameAlt : name;
                }
            }
        }
        ContentValues v = new ContentValues();
        v.put("display_name", display);
        v.put("display_name_alt", alt);
        v.put("display_name_source", bestSource);
        v.put("phonetic_name", phonetic);
        v.put("sort_key", display);
        v.put("sort_key_alt", alt);
        mDb.update("raw_contacts", v, "_id = ?", new String[]{String.valueOf(raw)});
    }

    /** The aggregate's columns from its raw contacts; a contact left with none is deleted
     *  (and listed in deleted_contacts). */
    private void updateContact(long contact) {
        if (contact <= 0) return;
        String id = String.valueOf(contact);
        List<Long> raws = idsQuery("SELECT _id FROM raw_contacts WHERE contact_id = ? AND deleted = 0 ORDER BY _id",
                new String[]{id});
        long now = System.currentTimeMillis();
        if (raws.isEmpty()) {
            mDb.delete("contacts", "_id = ?", new String[]{id});
            mDb.execSQL("UPDATE raw_contacts SET contact_id = NULL WHERE contact_id = ?", new Object[]{contact});
            mDb.execSQL("INSERT OR REPLACE INTO deleted_contacts (contact_id, contact_deleted_timestamp)"
                    + " VALUES (?, ?)", new Object[]{contact, now});
            return;
        }
        long nameRaw = longQuery("SELECT _id FROM raw_contacts WHERE contact_id = ? AND deleted = 0"
                + " ORDER BY display_name_source DESC, _id LIMIT 1", new String[]{id});
        long photo = longQuery("SELECT d._id FROM data d JOIN raw_contacts r ON r._id = d.raw_contact_id"
                + " WHERE r.contact_id = ? AND r.deleted = 0 AND d.mimetype = ? AND d.data15 IS NOT NULL"
                + " ORDER BY d.is_super_primary DESC, (r._id = ?) DESC, d.is_primary DESC, d._id LIMIT 1",
                new String[]{id, MIME_PHOTO, String.valueOf(nameRaw)});
        StringBuilder lookup = new StringBuilder();
        for (long r : raws) lookup.append(lookup.length() == 0 ? "" : ".").append("0r").append(r);
        ContentValues v = new ContentValues();
        v.put("name_raw_contact_id", nameRaw);
        if (photo > 0) v.put("photo_id", photo); else v.putNull("photo_id");
        v.put("lookup", lookup.toString());
        v.put("contact_last_updated_timestamp", now);
        mDb.update("contacts", v, "_id = ?", new String[]{id});
        mDb.execSQL("UPDATE contacts SET"
                + " starred = (SELECT IFNULL(MAX(starred), 0) FROM raw_contacts WHERE contact_id = ?1 AND deleted = 0),"
                + " pinned = (SELECT IFNULL(MAX(pinned), 0) FROM raw_contacts WHERE contact_id = ?1 AND deleted = 0),"
                // Sent to voicemail only when every raw contact is.
                + " send_to_voicemail = (SELECT IFNULL(MIN(send_to_voicemail), 0) FROM raw_contacts"
                + " WHERE contact_id = ?1 AND deleted = 0),"
                + " custom_ringtone = (SELECT custom_ringtone FROM raw_contacts WHERE contact_id = ?1"
                + " AND deleted = 0 AND custom_ringtone IS NOT NULL ORDER BY _id LIMIT 1),"
                + " times_contacted = (SELECT IFNULL(MAX(times_contacted), 0) FROM raw_contacts"
                + " WHERE contact_id = ?1 AND deleted = 0),"
                + " last_time_contacted = (SELECT MAX(last_time_contacted) FROM raw_contacts"
                + " WHERE contact_id = ?1 AND deleted = 0),"
                + " has_phone_number = EXISTS (SELECT 1 FROM data d JOIN raw_contacts r ON r._id = d.raw_contact_id"
                + " WHERE r.contact_id = ?1 AND r.deleted = 0 AND d.mimetype = '" + MIME_PHONE + "'"
                + " AND d.data1 IS NOT NULL AND d.data1 != '')"
                + " WHERE _id = ?1", new Object[]{contact});
    }

    private long insertGroup(Uri uri, ContentValues v, boolean sync) {
        String pName = uri.getQueryParameter("account_name");
        String pType = uri.getQueryParameter("account_type");
        if (pName != null) v.put("account_name", pName);
        if (pType != null) v.put("account_type", pType);
        ContentValues row = only(v, GROUP_WRITABLE);
        row.remove("deleted");
        if (!sync) row.put("dirty", 1);
        return mDb.insertOrThrow("groups", null, row);
    }

    private static final Set<String> GROUP_WRITABLE = new LinkedHashSet<>(Arrays.asList(
            "account_name", "account_type", "data_set", "sourceid", "version", "dirty", "title", "title_res",
            "notes", "system_id", "deleted", "group_visible", "should_sync", "auto_add", "favorites",
            "group_is_read_only", "res_package", "sync1", "sync2", "sync3", "sync4"));

    // ---- update ----------------------------------------------------------------------

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        synchronized (mLock) {
            if (mDb == null) return 0;
            List<String> s = uri.getPathSegments();
            final boolean sync = isSyncAdapter(uri);
            ContentValues v = values != null ? new ContentValues(values) : new ContentValues();
            boolean ok = false;
            beginWrite();
            try {
                int n;
                String first = s.isEmpty() ? "" : s.get(0);
                if (first.equals("aggregation_exceptions") && s.size() == 1) {
                    n = updateAggregationException(v);
                } else if (first.equals("contacts")) {
                    n = updateContacts(uri, s, v, selection, selectionArgs, sync);
                } else if (first.equals("raw_contacts") && s.size() <= 2) {
                    n = updateRawContacts(rawIds(uri, s, selection, selectionArgs), v, sync);
                } else if (first.equals("data") || (first.equals("raw_contacts") && s.size() == 3)) {
                    n = updateData(dataIds(uri, s, selection, selectionArgs), v, sync);
                } else if (first.equals("groups")) {
                    n = updateGroups(uri, s, v, selection, selectionArgs, sync);
                } else {
                    throw new UnsupportedOperationException("Unknown uri: " + uri);
                }
                if (n > 0) markChanged(sync);
                ok = true;
                return n;
            } finally {
                endWrite(ok);
            }
        }
    }

    /** The _ids a URI and selection pick out of `view`, the one its writes may name (the query's
     *  routing; a URI routed elsewhere, such as contacts/<id>/data, is not written through). */
    private List<Long> idsOf(Uri uri, List<String> s, String view, String selection, String[] selectionArgs) {
        Q q = route(uri, s);
        if (q == null || !q.view.equals(view)) {
            throw new UnsupportedOperationException("Unknown uri: " + uri);
        }
        String where = andSelection(mDb, q.view, q.where.toString(), selection);
        List<String> args = new ArrayList<>(q.args);
        if (selectionArgs != null && selection != null && !selection.isEmpty()) {
            args.addAll(Arrays.asList(selectionArgs));
        }
        return idsQuery("SELECT DISTINCT _id FROM " + q.view
                + (where.isEmpty() ? "" : " WHERE " + where), args.toArray(new String[0]));
    }

    private List<Long> rawIds(Uri uri, List<String> s, String selection, String[] args) {
        return idsOf(uri, s, "view_raw_contacts", selection, args);
    }

    private List<Long> dataIds(Uri uri, List<String> s, String selection, String[] args) {
        return idsOf(uri, s, "view_data", selection, args);
    }

    private static final Set<String> CONTACT_OPTIONS = new LinkedHashSet<>(Arrays.asList(
            "starred", "pinned", "custom_ringtone", "send_to_voicemail", "times_contacted",
            "last_time_contacted"));

    /** Contacts' writable options (ContactOptionsColumns), written to every raw contact. */
    private int updateContacts(Uri uri, List<String> s, ContentValues v, String selection, String[] args,
                               boolean sync) {
        ContentValues opts = new ContentValues();
        for (String k : v.keySet()) {
            if (CONTACT_OPTIONS.contains(k)) {
                Object o = v.get(k);
                if (o == null) opts.putNull(k);
                else if (o instanceof Boolean) opts.put(k, ((Boolean) o) ? 1 : 0);
                else opts.put(k, o.toString());
            }
        }
        List<Long> contacts = idsOf(uri, s, "view_contacts", selection, args);
        for (long contact : contacts) {
            if (opts.size() == 0) continue;
            List<Long> raws = idsQuery("SELECT _id FROM raw_contacts WHERE contact_id = ? AND deleted = 0",
                    new String[]{String.valueOf(contact)});
            for (long raw : raws) {
                mDb.update("raw_contacts", opts, "_id = ?", new String[]{String.valueOf(raw)});
                touchRaw(raw, sync);
            }
            updateContact(contact);
        }
        return contacts.size();
    }

    private int updateRawContacts(List<Long> raws, ContentValues v, boolean sync) {
        ContentValues row = only(v, RAW_WRITABLE);
        boolean syncOnly = true;
        for (String k : row.keySet()) {
            if (!k.equals("dirty") && !k.startsWith("sync") && !k.equals("sourceid") && !k.equals("version")) {
                syncOnly = false;
            }
        }
        Set<Long> contacts = new LinkedHashSet<>();
        for (long raw : raws) {
            String id = String.valueOf(raw);
            long before = contactOf(raw);
            if (row.size() > 0) mDb.update("raw_contacts", row, "_id = ?", new String[]{id});
            if (!syncOnly && !row.containsKey("dirty")) touchRaw(raw, sync);
            Integer deleted = row.getAsInteger("deleted");
            if (deleted != null && deleted == 0 && before <= 0) {
                // Undeleted by its sync adapter: a contact of its own again.
                ContentValues c = new ContentValues();
                c.put("name_raw_contact_id", raw);
                long contact = mDb.insertOrThrow("contacts", null, c);
                mDb.execSQL("UPDATE raw_contacts SET contact_id = ? WHERE _id = ?", new Object[]{contact, raw});
                before = contact;
            }
            updateRawDisplayName(raw);
            contacts.add(before);
        }
        for (long c : contacts) updateContact(c);
        return raws.size();
    }

    private int updateData(List<Long> ids, ContentValues v, boolean sync) {
        ContentValues row = only(v, DATA_WRITABLE);
        row.remove("raw_contact_id");
        for (long id : ids) {
            String sid = String.valueOf(id);
            String mime = null;
            long raw = -1;
            try (Cursor c = mDb.rawQuery("SELECT mimetype, raw_contact_id FROM data WHERE _id = ?", new String[]{sid})) {
                if (c.moveToFirst()) { mime = c.getString(0); raw = c.getLong(1); }
            }
            if (mime == null) continue;
            ContentValues r = new ContentValues(row);
            if (MIME_NAME.equals(mime) && (r.containsKey("data1") || r.containsKey("data2") || r.containsKey("data3"))) {
                // The name's other half follows the half that changed.
                if (!r.containsKey("data1")) r.putNull("data1");
                fixUpStructuredName(r);
            } else if (MIME_PHONE.equals(mime) && r.containsKey("data1") && !r.containsKey("data4")) {
                r.putNull("data4");
                fixUpData(mime, r);
            }
            if (r.size() > 0) mDb.update("data", r, "_id = ?", new String[]{sid});
            afterDataChange(id, raw, mime, r, sync);
        }
        return ids.size();
    }

    private int updateGroups(Uri uri, List<String> s, ContentValues v, String selection, String[] args,
                             boolean sync) {
        ContentValues row = only(v, GROUP_WRITABLE);
        if (!sync && !row.containsKey("dirty")) row.put("dirty", 1);
        List<Long> ids = idsOf(uri, s, "view_groups", selection, args);
        for (long id : ids) {
            mDb.update("groups", row, "_id = ?", new String[]{String.valueOf(id)});
            mDb.execSQL("UPDATE groups SET version = version + 1 WHERE _id = ?", new Object[]{id});
        }
        return ids.size();
    }

    /** AggregationExceptions: an update with TYPE, RAW_CONTACT_ID1 and RAW_CONTACT_ID2
     *  records the user's choice and joins or splits the two raw contacts' contacts. */
    private int updateAggregationException(ContentValues v) {
        Integer type = v.getAsInteger("type");
        Long r1 = v.getAsLong("raw_contact_id1"), r2 = v.getAsLong("raw_contact_id2");
        if (type == null || r1 == null || r2 == null) {
            throw new IllegalArgumentException("type, raw_contact_id1 and raw_contact_id2 are required");
        }
        long a = Math.min(r1, r2), b = Math.max(r1, r2);
        if (a == b) return 0;
        if (type == AGG_AUTOMATIC) {
            mDb.execSQL("DELETE FROM agg_exceptions WHERE raw_contact_id1 = ? AND raw_contact_id2 = ?",
                    new Object[]{a, b});
        } else {
            mDb.execSQL("INSERT OR REPLACE INTO agg_exceptions (type, raw_contact_id1, raw_contact_id2)"
                    + " VALUES (?, ?, ?)", new Object[]{type, a, b});
        }
        long ca = contactOf(a), cb = contactOf(b);
        if (type == AGG_KEEP_TOGETHER) {
            if (ca > 0 && cb > 0 && ca != cb) {
                mDb.execSQL("UPDATE raw_contacts SET contact_id = ? WHERE contact_id = ?", new Object[]{ca, cb});
                updateContact(cb);   // left with none: deleted
                updateContact(ca);
            }
        } else if (ca > 0 && ca == cb && !keptTogether(a, b)) {
            // Split: b goes to a contact of its own.
            ContentValues c = new ContentValues();
            c.put("name_raw_contact_id", b);
            long contact = mDb.insertOrThrow("contacts", null, c);
            mDb.execSQL("UPDATE raw_contacts SET contact_id = ? WHERE _id = ?", new Object[]{contact, b});
            updateContact(contact);
            updateContact(ca);
        }
        return 1;
    }

    private boolean keptTogether(long a, long b) {
        return longQuery("SELECT _id FROM agg_exceptions WHERE type = ? AND raw_contact_id1 = ? AND raw_contact_id2 = ?",
                new String[]{String.valueOf(AGG_KEEP_TOGETHER), String.valueOf(a), String.valueOf(b)}) > 0;
    }

    // ---- delete ----------------------------------------------------------------------

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        synchronized (mLock) {
            if (mDb == null) return 0;
            List<String> s = uri.getPathSegments();
            final boolean sync = isSyncAdapter(uri);
            boolean ok = false;
            beginWrite();
            try {
                int n = 0;
                String first = s.isEmpty() ? "" : s.get(0);
                if (first.equals("contacts")) {
                    for (long contact : idsOf(uri, s, "view_contacts", selection, selectionArgs)) {
                        for (long raw : idsQuery("SELECT _id FROM raw_contacts WHERE contact_id = ? AND deleted = 0",
                                new String[]{String.valueOf(contact)})) {
                            deleteRawContact(raw, sync);
                        }
                        n++;
                    }
                } else if (first.equals("raw_contacts") && s.size() <= 2) {
                    for (long raw : rawIds(uri, s, selection, selectionArgs)) {
                        deleteRawContact(raw, sync);
                        n++;
                    }
                } else if (first.equals("data") || (first.equals("raw_contacts") && s.size() == 3)) {
                    for (long id : dataIds(uri, s, selection, selectionArgs)) {
                        long raw = longQuery("SELECT raw_contact_id FROM data WHERE _id = ?",
                                new String[]{String.valueOf(id)});
                        mDb.delete("data", "_id = ?", new String[]{String.valueOf(id)});
                        mDb.delete("phone_lookup", "data_id = ?", new String[]{String.valueOf(id)});
                        if (raw > 0) {
                            touchRaw(raw, sync);
                            updateRawDisplayName(raw);
                            updateContact(contactOf(raw));
                        }
                        n++;
                    }
                } else if (first.equals("groups")) {
                    for (long id : idsOf(uri, s, "view_groups", selection, selectionArgs)) {
                        String sid = String.valueOf(id);
                        boolean local = longQuery("SELECT _id FROM groups WHERE _id = ? AND account_type IS NULL",
                                new String[]{sid}) > 0;
                        if (sync || local) {
                            mDb.delete("groups", "_id = ?", new String[]{sid});
                            mDb.delete("data", "mimetype = ? AND data1 = ?", new String[]{MIME_GROUP_MEMBERSHIP, sid});
                        } else {
                            mDb.execSQL("UPDATE groups SET deleted = 1, dirty = 1 WHERE _id = ?", new Object[]{id});
                        }
                        n++;
                    }
                } else {
                    throw new UnsupportedOperationException("Unknown uri: " + uri);
                }
                if (n > 0) markChanged(sync);
                ok = true;
                return n;
            } finally {
                endWrite(ok);
            }
        }
    }

    /** ContactsProvider2.deleteRawContact: a sync adapter's delete, or a raw contact of the
     *  local account (no account type), removes the row and its data; any other is marked
     *  deleted and dirty for its account's sync adapter and leaves its contact. */
    private void deleteRawContact(long raw, boolean sync) {
        String id = String.valueOf(raw);
        long contact = contactOf(raw);
        boolean local = longQuery("SELECT _id FROM raw_contacts WHERE _id = ? AND account_type IS NULL",
                new String[]{id}) > 0;
        if (sync || local) {
            mDb.delete("phone_lookup", "raw_contact_id = ?", new String[]{id});
            mDb.delete("data", "raw_contact_id = ?", new String[]{id});
            mDb.delete("agg_exceptions", "raw_contact_id1 = ? OR raw_contact_id2 = ?", new String[]{id, id});
            mDb.delete("raw_contacts", "_id = ?", new String[]{id});
        } else {
            mDb.execSQL("UPDATE raw_contacts SET deleted = 1, dirty = 1, version = version + 1, contact_id = NULL"
                    + " WHERE _id = ?", new Object[]{raw});
        }
        updateContact(contact);
    }

    // ---- batches ---------------------------------------------------------------------

    /** All of a batch in one transaction, one notification at its end, as AOSP's
     *  AbstractContactsProvider applies it. */
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

    // ---- photos ----------------------------------------------------------------------

    /** contacts/<id>/photo (and by lookup key): the Photo row's thumbnail, served as a file
     *  of the provider package's cache folder named by the row and its bytes' checksum, so a
     *  reader holding an older one open is never written under. A display photo file is not
     *  kept, so display_photo is not found; nothing here is written to. */
    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (mode != null && mode.indexOf('w') >= 0) {
            throw new FileNotFoundException("Writing is not supported: " + uri);
        }
        List<String> s = uri.getPathSegments();
        if (s.isEmpty() || !s.get(s.size() - 1).equals("photo") || !s.get(0).equals("contacts")) {
            throw new FileNotFoundException("No file at " + uri);
        }
        byte[] photo = null;
        long dataId = -1;
        synchronized (mLock) {
            if (mDb == null) throw new FileNotFoundException("No contacts database");
            Q q = route(uri, s);
            if (q == null) throw new FileNotFoundException("No file at " + uri);
            try (Cursor c = mDb.rawQuery("SELECT _id, data15 FROM " + q.view + " WHERE " + q.where,
                    q.args.toArray(new String[0]))) {
                if (c.moveToFirst() && !c.isNull(1)) {
                    dataId = c.getLong(0);
                    photo = c.getBlob(1);
                }
            }
        }
        if (photo == null) throw new FileNotFoundException("No photo at " + uri);
        File db = databaseFile("contacts2.db");
        if (db == null) throw new FileNotFoundException("No Android file system root");
        File dir = new File(db.getParentFile().getParentFile(), "cache/photos");
        java.util.zip.CRC32 crc = new java.util.zip.CRC32();
        crc.update(photo);
        String prefix = "photo-" + dataId + "-";
        File f = new File(dir, prefix + Long.toHexString(crc.getValue()) + "-" + photo.length + ".jpg");
        if (!f.isFile()) {
            dir.mkdirs();
            File tmp = new File(dir, f.getName() + "." + android.os.Process.myPid() + ".tmp");
            try (FileOutputStream os = new FileOutputStream(tmp)) {
                os.write(photo);
            } catch (IOException e) {
                tmp.delete();
                throw new FileNotFoundException("Cannot write " + tmp + ": " + e.getMessage());
            }
            if (!tmp.renameTo(f) && !f.isFile()) {
                tmp.delete();
                throw new FileNotFoundException("Cannot write " + f);
            }
            tmp.delete();
            // The row's earlier pictures; one a reader still holds open stays until next time.
            File[] old = dir.listFiles((d, name) -> name.startsWith(prefix) && !name.equals(f.getName()));
            if (old != null) for (File o : old) o.delete();
        }
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
    }
}
