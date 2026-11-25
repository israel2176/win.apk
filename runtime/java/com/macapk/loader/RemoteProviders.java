// com.macapk.loader.RemoteProviders -- a ContentProvider in ANOTHER app's
// process (cross-process Binder, phase 4).
//
// On a device ContentResolver acquires an IContentProvider binder from the
// provider's process (ActivityManagerService.getContentProvider) and every
// query / insert / update / delete / getType / call / openFile is a transaction
// on it. Here: the system broker (SystemServer.GET_PROVIDER) finds the package
// that declares the authority, starts it if needed, and asks that app's thread
// for the provider's binder (Transport, below); the caller wraps it in a Proxy
// -- a ContentProvider whose methods transact -- which ContentProviderRegistry
// hands to ContentResolver like any provider.
//
// THE WIRE (descriptor android.content.IContentProvider; our own codes, both
// ends are Mac.apk). A Uri crosses as its string. A Cursor crosses as typed
// rows (column names, then each cell's Cursor.getType and value) and is rebuilt
// as a MatrixCursor -- the data AOSP's CursorWindow carries, without the shared
// window; large results ride the transport's out-of-line path. A file crosses
// as a real file descriptor (openFile -> ParcelFileDescriptor -> Mach fileport).
//
// ContentObserver / notifyChange cross processes through the broker by
// authority (SystemClient.observeRemote / notifyRemote).
//
// PERMISSIONS. Opening the provider is the broker's ContentProviderHelper check
// (GET_PROVIDER); every operation is then checked here as AOSP's ContentProvider.Transport
// checks it (enforceReadPermission for query and a file opened for reading,
// enforceWritePermission for insert, bulkInsert, update, delete and a file opened with 'w';
// getType and call are the provider's own business), the decision the system broker's
// (CHECK_PROVIDER: the caller by the pid the OS gives this pipe, its permissions, the
// provider's path-permissions and the URI grants). A denial is AOSP's SecurityException;
// a provider-wide allow is remembered for that process.
//
// LIMITS (stated): a Cursor is a snapshot, not a live window.
package com.macapk.loader;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;

import java.util.Map;
import java.util.WeakHashMap;

public final class RemoteProviders {
    private RemoteProviders() {}

    static final String DESCRIPTOR = "android.content.IContentProvider";
    static final int TX_QUERY = IBinder.FIRST_CALL_TRANSACTION;
    static final int TX_INSERT = TX_QUERY + 1;
    static final int TX_UPDATE = TX_QUERY + 2;
    static final int TX_DELETE = TX_QUERY + 3;
    static final int TX_GET_TYPE = TX_QUERY + 4;
    static final int TX_CALL = TX_QUERY + 5;
    static final int TX_OPEN_FILE = TX_QUERY + 6;
    static final int TX_BULK_INSERT = TX_QUERY + 7;

    private static final Map<ContentProvider, Transport> sTransports = new WeakHashMap<>();

    /** The binder that serves `p` to other processes (one per provider). */
    static synchronized IBinder transportFor(ContentProvider p) {
        return sTransports.computeIfAbsent(p, Transport::new);
    }

    // ---- cursors -----------------------------------------------------------------

    static void writeCursor(Parcel out, Cursor c) {
        if (c == null) { out.writeInt(-1); return; }
        try {
            String[] cols = c.getColumnNames();
            out.writeInt(cols.length);
            out.writeStringArray(cols);
            out.writeInt(c.getCount());
            c.moveToPosition(-1);
            while (c.moveToNext()) {
                for (int i = 0; i < cols.length; i++) {
                    int t = c.getType(i);
                    out.writeInt(t);
                    switch (t) {
                        case Cursor.FIELD_TYPE_INTEGER: out.writeLong(c.getLong(i)); break;
                        case Cursor.FIELD_TYPE_FLOAT:   out.writeDouble(c.getDouble(i)); break;
                        case Cursor.FIELD_TYPE_STRING:  out.writeString(c.getString(i)); break;
                        case Cursor.FIELD_TYPE_BLOB:    out.writeByteArray(c.getBlob(i)); break;
                        default: break;   // FIELD_TYPE_NULL
                    }
                }
            }
        } finally {
            c.close();
        }
    }

    static Cursor readCursor(Parcel in) {
        int ncol = in.readInt();
        if (ncol < 0) return null;
        String[] cols = in.createStringArray();
        int rows = in.readInt();
        MatrixCursor m = new MatrixCursor(cols, Math.max(rows, 1));
        for (int r = 0; r < rows; r++) {
            Object[] row = new Object[ncol];
            for (int i = 0; i < ncol; i++) {
                switch (in.readInt()) {
                    case Cursor.FIELD_TYPE_INTEGER: row[i] = in.readLong(); break;
                    case Cursor.FIELD_TYPE_FLOAT:   row[i] = in.readDouble(); break;
                    case Cursor.FIELD_TYPE_STRING:  row[i] = in.readString(); break;
                    case Cursor.FIELD_TYPE_BLOB:    row[i] = in.createByteArray(); break;
                    default:                        row[i] = null;
                }
            }
            m.addRow(row);
        }
        return m;
    }

    private static void writeUri(Parcel p, Uri u) { p.writeString(u != null ? u.toString() : null); }
    private static Uri readUri(Parcel p) { String s = p.readString(); return s != null ? Uri.parse(s) : null; }

    // ---- the serving side ----------------------------------------------------------

    /** A local provider, served to other processes. */
    static final class Transport extends Binder {
        private final ContentProvider p;
        /** Calling processes allowed provider-wide: "pid:start time:r|w". */
        private final java.util.Set<String> allowed = java.util.concurrent.ConcurrentHashMap.newKeySet();
        Transport(ContentProvider p) { super(DESCRIPTOR); this.p = p; }

        /** ContentProvider.Transport.enforceReadPermission / enforceWritePermission for the calling
         *  process, the broker deciding; throws AOSP's SecurityException. */
        private void enforce(Uri uri, boolean write) {
            int pid = Binder.getCallingPid();
            if (pid == (int) ProcessHandle.current().pid()) return;
            long started;
            try { started = WinProcesses.startTime(pid); } catch (Throwable t) { started = 0; }
            String key = pid + ":" + started + ":" + (write ? "w" : "r");
            if (started != 0 && allowed.contains(key)) return;
            Object[] a = SystemClient.checkProvider(pid, uri, write);
            int answer = (Integer) a[0];
            if (answer == BrokerPermissions.OP_ALLOWED) {
                if (started != 0) allowed.add(key);
                return;
            }
            if (answer == BrokerPermissions.OP_ALLOWED_URI) return;
            throw new SecurityException("Permission Denial: " + (write ? "writing " : "reading ")
                    + p.getClass().getName() + " uri " + uri + " from pid=" + pid + ", uid="
                    + Binder.getCallingUid() + a[1]);
        }

        @Override protected boolean onTransact(int code, Parcel d, Parcel r, int flags)
                throws RemoteException {
            if (code < TX_QUERY || code > TX_BULK_INSERT) return super.onTransact(code, d, r, flags);
            d.enforceInterface(DESCRIPTOR);
            Uri uri = readUri(d);
            try {
                switch (code) {
                    case TX_QUERY: enforce(uri, false); break;
                    case TX_INSERT: case TX_BULK_INSERT: case TX_UPDATE: case TX_DELETE: enforce(uri, true); break;
                    default: break;   // getType, call: unchecked, as AOSP's Transport; openFile below
                }
            } catch (SecurityException e) {
                r.writeException(e);
                return true;
            }
            switch (code) {
                case TX_QUERY: {
                    String[] proj = d.createStringArray();
                    String sel = d.readString();
                    String[] args = d.createStringArray();
                    String sort = d.readString();
                    Cursor c = p.query(uri, proj, sel, args, sort);
                    r.writeNoException();
                    writeCursor(r, c);
                    return true;
                }
                case TX_INSERT: {
                    ContentValues v = d.readTypedObject(ContentValues.CREATOR);
                    Uri out = p.insert(uri, v);
                    r.writeNoException(); writeUri(r, out);
                    return true;
                }
                case TX_BULK_INSERT: {
                    ContentValues[] v = d.createTypedArray(ContentValues.CREATOR);
                    int n = p.bulkInsert(uri, v);
                    r.writeNoException(); r.writeInt(n);
                    return true;
                }
                case TX_UPDATE: {
                    ContentValues v = d.readTypedObject(ContentValues.CREATOR);
                    String sel = d.readString();
                    String[] args = d.createStringArray();
                    int n = p.update(uri, v, sel, args);
                    r.writeNoException(); r.writeInt(n);
                    return true;
                }
                case TX_DELETE: {
                    String sel = d.readString();
                    String[] args = d.createStringArray();
                    int n = p.delete(uri, sel, args);
                    r.writeNoException(); r.writeInt(n);
                    return true;
                }
                case TX_GET_TYPE: {
                    String t = p.getType(uri);
                    r.writeNoException(); r.writeString(t);
                    return true;
                }
                case TX_CALL: {
                    String authority = d.readString();
                    String method = d.readString();
                    String arg = d.readString();
                    Bundle extras = d.readBundle();
                    Bundle out = authority != null ? p.call(authority, method, arg, extras)
                                                   : p.call(method, arg, extras);
                    r.writeNoException(); r.writeBundle(out);
                    return true;
                }
                case TX_OPEN_FILE: {
                    String mode = d.readString();
                    try {
                        // ContentProvider.Transport.enforceFilePermission.
                        enforce(uri, mode != null && mode.indexOf('w') != -1);
                    } catch (SecurityException e) {
                        r.writeException(e);
                        return true;
                    }
                    ParcelFileDescriptor pfd;
                    try {
                        pfd = p.openFile(uri, mode);
                    } catch (java.io.FileNotFoundException e) {
                        // AOSP surfaces it to the caller as FileNotFoundException.
                        r.writeNoException(); r.writeInt(0); r.writeString(e.getMessage());
                        return true;
                    }
                    r.writeNoException();
                    r.writeInt(pfd != null ? 1 : 0);
                    if (pfd != null) r.writeFileDescriptor(pfd.getFileDescriptor());
                    else r.writeString("no file");
                    return true;
                }
            }
            return false;
        }
    }

    // ---- the calling side -----------------------------------------------------------

    /** A provider in another process: every call is a transaction. */
    static final class Proxy extends ContentProvider {
        private final IBinder remote;
        Proxy(IBinder remote, String authority) {
            this.remote = remote;
            android.content.pm.ProviderInfo info = new android.content.pm.ProviderInfo();
            info.authority = authority;
            info.exported = true;
            try { attachInfo(null, info); } catch (Throwable ignored) {}
        }
        @Override public boolean onCreate() { return true; }

        private Parcel call(int code, Uri uri, java.util.function.Consumer<Parcel> args) {
            Parcel d = Parcel.obtain(), r = Parcel.obtain();
            d.writeInterfaceToken(DESCRIPTOR);
            writeUri(d, uri);
            if (args != null) args.accept(d);
            try {
                remote.transact(code, d, r, 0);
            } catch (RemoteException e) {
                d.recycle(); r.recycle();
                // AOSP ContentResolver: a dead provider process reads as "no
                // result" (null cursor / 0 rows) after logging.
                NativeLog.w("RemoteProvider", "provider process gone: " + e);
                return null;
            }
            d.recycle();
            r.readException();
            return r;
        }

        @Override public Cursor query(Uri uri, String[] projection, String selection,
                                      String[] selectionArgs, String sortOrder) {
            Parcel r = call(TX_QUERY, uri, d -> {
                d.writeStringArray(projection); d.writeString(selection);
                d.writeStringArray(selectionArgs); d.writeString(sortOrder);
            });
            if (r == null) return null;
            try { return readCursor(r); } finally { r.recycle(); }
        }
        @Override public Uri insert(Uri uri, ContentValues values) {
            Parcel r = call(TX_INSERT, uri, d -> d.writeTypedObject(values, 0));
            if (r == null) return null;
            try { return readUri(r); } finally { r.recycle(); }
        }
        @Override public int bulkInsert(Uri uri, ContentValues[] values) {
            Parcel r = call(TX_BULK_INSERT, uri, d -> d.writeTypedArray(values, 0));
            if (r == null) return 0;
            try { return r.readInt(); } finally { r.recycle(); }
        }
        @Override public int update(Uri uri, ContentValues values, String selection, String[] args) {
            Parcel r = call(TX_UPDATE, uri, d -> {
                d.writeTypedObject(values, 0); d.writeString(selection); d.writeStringArray(args);
            });
            if (r == null) return 0;
            try { return r.readInt(); } finally { r.recycle(); }
        }
        @Override public int delete(Uri uri, String selection, String[] args) {
            Parcel r = call(TX_DELETE, uri, d -> { d.writeString(selection); d.writeStringArray(args); });
            if (r == null) return 0;
            try { return r.readInt(); } finally { r.recycle(); }
        }
        @Override public String getType(Uri uri) {
            Parcel r = call(TX_GET_TYPE, uri, null);
            if (r == null) return null;
            try { return r.readString(); } finally { r.recycle(); }
        }
        @Override public Bundle call(String method, String arg, Bundle extras) {
            return callInner(null, method, arg, extras);
        }
        @Override public Bundle call(String authority, String method, String arg, Bundle extras) {
            return callInner(authority, method, arg, extras);
        }
        private Bundle callInner(String authority, String method, String arg, Bundle extras) {
            Parcel r = call(TX_CALL, null, d -> {
                d.writeString(authority); d.writeString(method); d.writeString(arg); d.writeBundle(extras);
            });
            if (r == null) return null;
            try { return r.readBundle(); } finally { r.recycle(); }
        }
        @Override public ParcelFileDescriptor openFile(Uri uri, String mode)
                throws java.io.FileNotFoundException {
            Parcel r = call(TX_OPEN_FILE, uri, d -> d.writeString(mode));
            if (r == null) throw new java.io.FileNotFoundException("provider process gone: " + uri);
            try {
                if (r.readInt() == 0) throw new java.io.FileNotFoundException(r.readString());
                return r.readFileDescriptor();
            } finally { r.recycle(); }
        }
    }
}
