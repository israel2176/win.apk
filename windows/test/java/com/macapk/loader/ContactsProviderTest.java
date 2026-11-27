// ContactsProviderTest -- the contacts (com.android.contacts) and call log (call_log)
// providers as an app reaches them through its ContentResolver: denied without
// READ_/WRITE_CONTACTS and READ_/WRITE_CALL_LOG, as AOSP's transport denies them; a
// contact written as a contacts editor writes one (applyBatch with back references) and
// read back as apps read it (Contacts, Phone, Email lookup, PhoneLookup, filters, lookup
// keys, entities); options, joining two raw contacts, deletion; the call log's insert,
// query, number filter, voicemail rule and delete; observers told of each change; a
// second process's provider over the same database sees the same rows.
//
// Needs SQLite's natives: CI runs it in winapk-runtime --java-test.
//   ContactsProviderTest <android root dir>
package com.macapk.loader;

import android.content.ContentProviderOperation;
import android.content.ContentProviderResult;
import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Entity;
import android.content.EntityIterator;
import android.content.pm.PackageManager;
import android.database.ContentObserver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.CallLog;
import android.provider.ContactsContract;
import android.provider.ContactsContract.CommonDataKinds.Email;
import android.provider.ContactsContract.CommonDataKinds.Phone;
import android.provider.ContactsContract.CommonDataKinds.StructuredName;
import android.provider.ContactsContract.Contacts;
import android.provider.ContactsContract.Data;
import android.provider.ContactsContract.RawContacts;

import java.io.File;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.atomic.AtomicInteger;

public final class ContactsProviderTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "PASS " : "FAIL ") + what);
        if (!ok) failures++;
    }

    /** The app: a package of its own whose runtime permissions the test grants. */
    static final class App extends ContextWrapper {
        final Set<String> granted = new HashSet<>();
        private final ContentResolver resolver = new ContentResolver(this);
        App() { super(null); }
        @Override public String getPackageName() { return "com.example.contactstest"; }
        @Override public int checkPermission(String permission, int pid, int uid) {
            return granted.contains(permission) ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
        }
        @Override public ContentResolver getContentResolver() { return resolver; }
        @Override public Context getApplicationContext() { return this; }
    }

    static final class Counter extends ContentObserver {
        final AtomicInteger n = new AtomicInteger();
        Counter() { super(null); }
        @Override public void onChange(boolean selfChange, Uri uri) { n.incrementAndGet(); }
    }

    private static boolean denied(Runnable r) {
        try {
            r.run();
            return false;
        } catch (SecurityException e) {
            return e.getMessage() != null && e.getMessage().contains("Permission Denial");
        }
    }

    private static int count(Cursor c) {
        if (c == null) return -1;
        try { return c.getCount(); } finally { c.close(); }
    }

    private static String first(Cursor c, String column) {
        if (c == null) return null;
        try {
            return c.moveToFirst() ? c.getString(c.getColumnIndexOrThrow(column)) : null;
        } finally {
            c.close();
        }
    }

    public static void main(String[] args) throws Exception {
        File root = new File(args.length > 0 ? args[0] : System.getProperty("java.io.tmpdir") + "/contacts-test");
        deleteTree(root);
        root.mkdirs();
        System.setProperty("macapk.android.root", root.getAbsolutePath());

        App app = new App();
        ContactsProvider.install(app);
        ContentResolver cr = app.getContentResolver();
        check(new File(root, "data/data/com.android.providers.contacts/databases/contacts2.db").isFile()
              && new File(root, "data/data/com.android.providers.contacts/databases/calllog.db").isFile(),
              "databases under the Android file system's provider package");

        // ---- permission denial ------------------------------------------------------
        check(denied(() -> cr.query(Contacts.CONTENT_URI, null, null, null, null)),
              "query without READ_CONTACTS: Permission Denial");
        check(denied(() -> cr.insert(RawContacts.CONTENT_URI, new ContentValues())),
              "insert without WRITE_CONTACTS: Permission Denial");
        check(denied(() -> cr.query(CallLog.Calls.CONTENT_URI, null, null, null, null)),
              "call log query without READ_CALL_LOG: Permission Denial");
        app.granted.add("android.permission.READ_CONTACTS");
        check(count(cr.query(Contacts.CONTENT_URI, null, null, null, null)) == 0, "READ_CONTACTS: no contacts yet");
        check(denied(() -> cr.insert(RawContacts.CONTENT_URI, new ContentValues())),
              "READ_CONTACTS alone does not write");
        app.granted.add("android.permission.WRITE_CONTACTS");

        // ---- a contact as an editor writes one -----------------------------------------
        Counter contactsObserver = new Counter();
        cr.registerContentObserver(Contacts.CONTENT_URI, true, contactsObserver);
        ArrayList<ContentProviderOperation> ops = new ArrayList<>();
        ops.add(ContentProviderOperation.newInsert(RawContacts.CONTENT_URI)
                .withValue(RawContacts.ACCOUNT_TYPE, null).withValue(RawContacts.ACCOUNT_NAME, null).build());
        ops.add(ContentProviderOperation.newInsert(Data.CONTENT_URI)
                .withValueBackReference(Data.RAW_CONTACT_ID, 0)
                .withValue(Data.MIMETYPE, StructuredName.CONTENT_ITEM_TYPE)
                .withValue(StructuredName.GIVEN_NAME, "John").withValue(StructuredName.FAMILY_NAME, "Smith").build());
        ops.add(ContentProviderOperation.newInsert(Data.CONTENT_URI)
                .withValueBackReference(Data.RAW_CONTACT_ID, 0)
                .withValue(Data.MIMETYPE, Phone.CONTENT_ITEM_TYPE)
                .withValue(Phone.NUMBER, "+972 54-123-4567").withValue(Phone.TYPE, Phone.TYPE_MOBILE).build());
        ops.add(ContentProviderOperation.newInsert(Data.CONTENT_URI)
                .withValueBackReference(Data.RAW_CONTACT_ID, 0)
                .withValue(Data.MIMETYPE, Email.CONTENT_ITEM_TYPE)
                .withValue(Email.ADDRESS, "john@example.com").withValue(Email.TYPE, Email.TYPE_WORK).build());
        ContentProviderResult[] res = cr.applyBatch(ContactsContract.AUTHORITY, ops);
        long raw = ContentUris.parseId(res[0].uri);
        check(res.length == 4 && raw > 0, "applyBatch: raw contact and three data rows inserted");
        check(contactsObserver.n.get() == 1, "one notification for the batch: " + contactsObserver.n.get());

        long contactId;
        try (Cursor c = cr.query(Contacts.CONTENT_URI,
                new String[]{Contacts._ID, Contacts.DISPLAY_NAME, Contacts.HAS_PHONE_NUMBER, Contacts.LOOKUP_KEY,
                        Contacts.DISPLAY_NAME_ALTERNATIVE}, null, null, null)) {
            check(c.getCount() == 1 && c.moveToFirst(), "one contact");
            contactId = c.getLong(0);
            check("John Smith".equals(c.getString(1)), "display name joined from its parts: " + c.getString(1));
            check(c.getInt(2) == 1, "has_phone_number");
            check("Smith, John".equals(c.getString(4)), "alternative name: " + c.getString(4));
            Uri lookup = Contacts.getLookupUri(contactId, c.getString(3));
            check(ContentUris.withAppendedId(Contacts.CONTENT_URI, contactId)
                    .equals(Contacts.lookupContact(cr, lookup)), "lookup URI resolves to the contact");
        }
        try (Cursor c = cr.query(Phone.CONTENT_URI, new String[]{Phone.NUMBER, Phone.DISPLAY_NAME, Phone.CONTACT_ID,
                Phone.NORMALIZED_NUMBER, Phone.TYPE}, null, null, null)) {
            check(c.getCount() == 1 && c.moveToFirst() && "+972 54-123-4567".equals(c.getString(0))
                  && "John Smith".equals(c.getString(1)) && c.getLong(2) == contactId
                  && c.getInt(4) == Phone.TYPE_MOBILE, "Phone.CONTENT_URI joins the contact");
            check("+972541234567".equals(c.getString(3)), "normalized number: " + c.getString(3));
        }
        check(count(cr.query(Email.CONTENT_URI, null, Email.ADDRESS + "=?", new String[]{"john@example.com"}, null)) == 1,
              "Email.CONTENT_URI with a selection");
        check("John Smith".equals(first(cr.query(Uri.withAppendedPath(Email.CONTENT_LOOKUP_URI, Uri.encode("JOHN@example.com")),
                null, null, null, null), Email.DISPLAY_NAME)), "Email lookup, case aside");
        check("John Smith".equals(first(cr.query(Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI,
                Uri.encode("054-1234567")), new String[]{ContactsContract.PhoneLookup.DISPLAY_NAME}, null, null, null),
                ContactsContract.PhoneLookup.DISPLAY_NAME)), "PhoneLookup matches the national form");
        check(count(cr.query(Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, "0549999999"),
                null, null, null, null)) == 0, "PhoneLookup: another number matches nothing");
        check(count(cr.query(Uri.withAppendedPath(Contacts.CONTENT_FILTER_URI, "smi"), null, null, null, null)) == 1,
              "Contacts filter on a name word");
        check(count(cr.query(Uri.withAppendedPath(Phone.CONTENT_FILTER_URI, "0541"), null, null, null, null)) == 0
              && count(cr.query(Uri.withAppendedPath(Phone.CONTENT_FILTER_URI, "97254"), null, null, null, null)) == 1,
              "Phone filter on the number's digits");
        try (Cursor c = cr.query(Uri.withAppendedPath(ContentUris.withAppendedId(Contacts.CONTENT_URI, contactId),
                Contacts.Data.CONTENT_DIRECTORY), new String[]{Data.MIMETYPE}, null, null, null)) {
            check(c.getCount() == 3, "contacts/<id>/data: three rows");
        }
        EntityIterator it = RawContacts.newEntityIterator(cr.query(ContactsContract.RawContactsEntity.CONTENT_URI,
                null, RawContacts._ID + "=?", new String[]{String.valueOf(raw)}, null));
        try {
            Entity e = it.hasNext() ? it.next() : null;
            check(e != null && e.getSubValues().size() == 3 && !it.hasNext(),
                  "RawContacts entity iterator: one raw contact, three data rows");
        } finally {
            it.close();
        }

        // ---- options, a second raw contact joined, deletion -------------------------------
        ContentValues star = new ContentValues();
        star.put(Contacts.STARRED, 1);
        check(cr.update(ContentUris.withAppendedId(Contacts.CONTENT_URI, contactId), star, null, null) == 1
              && "1".equals(first(cr.query(ContentUris.withAppendedId(Contacts.CONTENT_URI, contactId),
                      new String[]{Contacts.STARRED}, null, null, null), Contacts.STARRED)), "starred");
        check(count(cr.query(ContactsContract.Contacts.CONTENT_STREQUENT_URI, null, null, null, null)) == 1,
              "strequent lists the starred contact");
        ContentValues photo = new ContentValues();
        photo.put(Data.RAW_CONTACT_ID, raw);
        photo.put(Data.MIMETYPE, ContactsContract.CommonDataKinds.Photo.CONTENT_ITEM_TYPE);
        photo.put(ContactsContract.CommonDataKinds.Photo.PHOTO, new byte[]{1, 2, 3});
        cr.insert(Data.CONTENT_URI, photo);
        Uri contactUri = ContentUris.withAppendedId(Contacts.CONTENT_URI, contactId);
        check(first(cr.query(contactUri, new String[]{Contacts.PHOTO_URI}, null, null, null), Contacts.PHOTO_URI) != null,
              "photo_uri once the contact has a photo");
        try (java.io.InputStream in = Contacts.openContactPhotoInputStream(cr, contactUri);
             java.io.InputStream file = cr.openInputStream(Uri.withAppendedPath(contactUri, Contacts.Photo.CONTENT_DIRECTORY))) {
            check(in != null && in.readAllBytes().length == 3, "openContactPhotoInputStream");
            check(file != null && file.readAllBytes().length == 3, "contacts/<id>/photo opened as a file");
        }
        ContentValues acct = new ContentValues();
        acct.put(RawContacts.ACCOUNT_TYPE, "com.example.account");
        acct.put(RawContacts.ACCOUNT_NAME, "me@example.com");
        long raw2 = ContentUris.parseId(cr.insert(RawContacts.CONTENT_URI, acct));
        ContentValues nick = new ContentValues();
        nick.put(Data.RAW_CONTACT_ID, raw2);
        nick.put(Data.MIMETYPE, ContactsContract.CommonDataKinds.Nickname.CONTENT_ITEM_TYPE);
        nick.put(ContactsContract.CommonDataKinds.Nickname.NAME, "Johnny");
        cr.insert(Data.CONTENT_URI, nick);
        check(count(cr.query(Contacts.CONTENT_URI, null, null, null, null)) == 2, "a raw contact is its own contact");
        ContentValues join = new ContentValues();
        join.put(ContactsContract.AggregationExceptions.TYPE, ContactsContract.AggregationExceptions.TYPE_KEEP_TOGETHER);
        join.put(ContactsContract.AggregationExceptions.RAW_CONTACT_ID1, raw);
        join.put(ContactsContract.AggregationExceptions.RAW_CONTACT_ID2, raw2);
        cr.update(ContactsContract.AggregationExceptions.CONTENT_URI, join, null, null);
        try (Cursor c = cr.query(Contacts.CONTENT_URI, new String[]{Contacts._ID, Contacts.DISPLAY_NAME}, null, null, null)) {
            check(c.getCount() == 1 && c.moveToFirst() && "John Smith".equals(c.getString(1)),
                  "KEEP_TOGETHER joins them, named by the structured name");
            contactId = c.getLong(0);
        }
        cr.delete(ContentUris.withAppendedId(RawContacts.CONTENT_URI, raw2), null, null);
        check("1".equals(first(cr.query(ContentUris.withAppendedId(RawContacts.CONTENT_URI, raw2),
                new String[]{RawContacts.DELETED}, null, null, null), RawContacts.DELETED)),
              "an account's raw contact is marked deleted for its sync adapter");
        cr.delete(ContentUris.withAppendedId(RawContacts.CONTENT_URI, raw2)
                .buildUpon().appendQueryParameter(ContactsContract.CALLER_IS_SYNCADAPTER, "true").build(), null, null);
        check(count(cr.query(RawContacts.CONTENT_URI, null, null, null, null)) == 1, "the sync adapter's delete removes it");

        // ---- another process's provider over the same database ------------------------------
        App other = new App();
        other.granted.add("android.permission.READ_CONTACTS");
        ContactsProvider second = new ContactsProvider();
        second.attachInfo(other, ContactsProvider.providerInfo());
        try (Cursor c = second.query(Phone.CONTENT_URI, new String[]{Phone.NUMBER}, null, null, null)) {
            check(c != null && c.getCount() == 1, "a second process's provider sees the same rows");
        }

        int before = contactsObserver.n.get();
        check(cr.delete(ContentUris.withAppendedId(Contacts.CONTENT_URI, contactId), null, null) == 1
              && count(cr.query(Contacts.CONTENT_URI, null, null, null, null)) == 0
              && count(cr.query(Data.CONTENT_URI, null, null, null, null)) == 0, "deleting the contact");
        check(contactsObserver.n.get() > before, "observer told of the delete");
        check(count(cr.query(ContactsContract.DeletedContacts.CONTENT_URI, null, "contact_id=?",
                new String[]{String.valueOf(contactId)}, null)) == 1, "deleted_contacts lists it");
        boolean unknown = false;
        try { cr.insert(Contacts.CONTENT_URI, new ContentValues()); } catch (UnsupportedOperationException e) { unknown = true; }
        check(unknown, "contacts are not inserted directly");

        // ---- call log ----------------------------------------------------------------------
        check(denied(() -> cr.insert(CallLog.Calls.CONTENT_URI, new ContentValues())),
              "call log insert without WRITE_CALL_LOG: Permission Denial");
        app.granted.add("android.permission.READ_CALL_LOG");
        app.granted.add("android.permission.WRITE_CALL_LOG");
        Counter callsObserver = new Counter();
        cr.registerContentObserver(CallLog.Calls.CONTENT_URI, true, callsObserver);
        long now = System.currentTimeMillis();
        ContentValues call = new ContentValues();
        call.put(CallLog.Calls.NUMBER, "+972541234567");
        call.put(CallLog.Calls.TYPE, CallLog.Calls.INCOMING_TYPE);
        call.put(CallLog.Calls.DATE, now);
        call.put(CallLog.Calls.DURATION, 42);
        Uri callUri = cr.insert(CallLog.Calls.CONTENT_URI, call);
        check(callUri != null && callsObserver.n.get() == 1, "a call inserted, observer told");
        call.put(CallLog.Calls.NUMBER, "035551234");
        call.put(CallLog.Calls.TYPE, CallLog.Calls.OUTGOING_TYPE);
        call.put(CallLog.Calls.DATE, now + 1000);
        cr.insert(CallLog.Calls.CONTENT_URI, call);
        check("035551234".equals(CallLog.Calls.getLastOutgoingCall(app)), "getLastOutgoingCall");
        check(count(cr.query(Uri.withAppendedPath(CallLog.Calls.CONTENT_FILTER_URI, Uri.encode("054-123-4567")),
                null, null, null, null)) == 1, "calls/filter matches the number");
        check(count(cr.query(CallLog.Calls.CONTENT_URI.buildUpon().appendQueryParameter("limit", "1").build(),
                null, null, null, CallLog.Calls.DEFAULT_SORT_ORDER)) == 1, "limit parameter");
        boolean refused = false;
        ContentValues vm = new ContentValues();
        vm.put(CallLog.Calls.TYPE, CallLog.Calls.VOICEMAIL_TYPE);
        try { cr.insert(CallLog.Calls.CONTENT_URI, vm); } catch (IllegalArgumentException e) { refused = true; }
        check(refused, "a voicemail is not inserted without allow_voicemails");
        refused = false;
        ContentValues bad = new ContentValues();
        bad.put("no_such_column", 1);
        try { cr.insert(CallLog.Calls.CONTENT_URI, bad); } catch (IllegalArgumentException e) { refused = true; }
        check(refused, "an unknown column is refused");
        try (Cursor c = cr.query(callUri, new String[]{CallLog.Calls.DURATION, CallLog.Calls.LAST_MODIFIED}, null, null, null)) {
            check(c.moveToFirst() && c.getLong(0) == 42 && c.getLong(1) > 0, "calls/<id> with last_modified set");
        }
        check(cr.delete(CallLog.Calls.CONTENT_URI, CallLog.Calls.TYPE + "=?",
                new String[]{String.valueOf(CallLog.Calls.OUTGOING_TYPE)}) == 1
              && count(cr.query(CallLog.Calls.CONTENT_URI, null, null, null, null)) == 1, "delete by selection");

        if (failures == 0) System.out.println("ALL PASSED");
        else System.out.println(failures + " FAILED");
        System.out.flush();
        System.exit(failures == 0 ? 0 : 1);
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        f.delete();
    }
}
