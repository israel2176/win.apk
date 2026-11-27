// The task's activity records as system_server keeps them across the death of the app's process: on a
// device the ActivityRecords, with each one's intent, result target, saved instance state (the icicle,
// onSaveInstanceState's Bundle) and persistent state, live in system_server, so when the app's process
// is killed and the user returns, ActivityTaskManagerService relaunches the task's top with its saved
// Bundle (LaunchActivityItem's state) and the ones under it as they are revealed. Win.apk's system side
// lives in the app's own process (Tasks), so the records it would keep are written to a file the
// system owns, apps\<name>\task-state.parcel, beside the app's run files and not under the data the
// app sees, and read back by the next process of the app.
//
// Which records are written is ActivityRecord.handleAppDied's rule for a process that dies: a record
// is kept when it has saved state (mHaveState: never launched, or stopped with an icicle since it last
// resumed) and is not finishing; a resumed or paused activity has none and is removed. What ends the
// task drops the file: the user dismissing it (the window closed, as a task swiped from recents),
// Force stop (the launcher's Stop deletes it) and an update of the app (the file names the version and
// the APK's update time it was written for).
//
// The file is a Parcel, marshalled: a header, then each record's activity, intent, launch fields and
// its two state bundles, each bundle and intent marshalled on its own so one that cannot be (a binder
// or another live object in it, which only an in-process Parcel can carry) costs only that record.
package com.winapk.aospapp;

import android.content.Intent;
import android.os.Bundle;
import android.os.Parcel;
import android.os.PersistableBundle;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

final class SavedTask {
    private static final int MAGIC = 0x57545331;   // "WTS1"
    private static final int FORMAT = 1;

    /** One record as written (Tasks.Record's persistent part). */
    static final class Entry {
        String activity;
        Intent intent;
        String launchedFrom;
        /** Index of the record its result goes to, among the entries; -1 for none. */
        int resultTo = -1;
        String resultWho;
        int requestCode = -1;
        int requestedOrientation;
        Bundle icicle;
        PersistableBundle persistentState;
    }

    private final File file;
    private final long versionCode;
    private final long updateTime;
    private byte[] written;

    SavedTask(File file, long versionCode, long updateTime) {
        this.file = file;
        this.versionCode = versionCode;
        this.updateTime = updateTime;
    }

    File file() { return file; }

    // ---- Writing ----

    /** The records to keep, bottom first, as Tasks has them: written when they differ from what the file
     *  holds; no record deletes the file. */
    void write(List<Tasks.Record> keep) {
        if (keep.isEmpty()) {
            delete();
            return;
        }
        Parcel p = Parcel.obtain();
        try {
            p.writeInt(MAGIC);
            p.writeInt(FORMAT);
            p.writeLong(versionCode);
            p.writeLong(updateTime);
            p.writeInt(keep.size());
            for (Tasks.Record r : keep) {
                p.writeString(r.info.name);
                p.writeByteArray(r.marshalledIntent());
                p.writeString(r.launchedFromPackage);
                p.writeInt(r.resultTo != null ? keep.indexOf(r.resultTo) : -1);
                p.writeString(r.resultWho);
                p.writeInt(r.requestCode);
                p.writeInt(r.requestedOrientation);
                p.writeByteArray(r.marshalledIcicle);
                p.writeByteArray(r.marshalledPersistent);
            }
            byte[] bytes = p.marshall();
            if (written != null && Arrays.equals(written, bytes)) return;
            File dir = file.getParentFile();
            if (dir != null) dir.mkdirs();
            File part = new File(file.getPath() + ".part");
            Files.write(part.toPath(), bytes);
            Files.move(part.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING,
                    StandardCopyOption.ATOMIC_MOVE);
            written = bytes;
        } catch (IOException | RuntimeException e) {
            System.err.println("[aosp-app] the task's saved state was not written to " + file + ": " + e);
        } finally {
            p.recycle();
        }
    }

    void delete() {
        written = null;
        if (file.exists() && !file.delete()) {
            System.err.println("[aosp-app] the task's saved state could not be deleted: " + file);
        }
    }

    /** `b` marshalled on its own, or null when it cannot be kept outside this process (see above). */
    static byte[] marshall(Bundle b) {
        if (b == null) return null;
        Parcel p = Parcel.obtain();
        try {
            b.writeToParcel(p, 0);
            return liveObjects(p) ? null : p.marshall();
        } catch (RuntimeException e) {
            return null;
        } finally {
            p.recycle();
        }
    }

    static byte[] marshall(PersistableBundle b) {
        if (b == null) return null;
        Parcel p = Parcel.obtain();
        try {
            b.writeToParcel(p, 0);
            return liveObjects(p) ? null : p.marshall();
        } catch (RuntimeException e) {
            return null;
        } finally {
            p.recycle();
        }
    }

    /** The intent marshalled; one whose extras cannot be kept is kept as its filter (action, data, type,
     *  categories, component), as a launch still resolves from it. */
    static byte[] marshall(Intent intent) {
        for (Intent i : new Intent[] {intent, intent.cloneFilter()}) {
            Parcel p = Parcel.obtain();
            try {
                i.writeToParcel(p, 0);
                if (!liveObjects(p)) return p.marshall();
            } catch (RuntimeException e) {
                // the filter next
            } finally {
                p.recycle();
            }
        }
        return null;
    }

    /** Whether the Parcel holds objects only this process has (a binder, a descriptor, a Parcelable
     *  kept by reference): Win.apk's Parcel keeps them in a table beside its bytes, which marshall()
     *  leaves out (AOSP's marshall refuses a Parcel with binders). The table is read through the
     *  Parcel's transport accessor, package-private. */
    private static boolean liveObjects(Parcel p) {
        try {
            if (sObjects == null) {
                java.lang.reflect.Method m = Parcel.class.getDeclaredMethod("macapkTransportObjects");
                m.setAccessible(true);
                sObjects = m;
            }
            return ((Object[]) sObjects.invoke(p)).length > 0;
        } catch (ReflectiveOperationException | RuntimeException e) {
            return false;   // a Parcel without the table: its marshall() refuses what it cannot keep
        }
    }

    private static java.lang.reflect.Method sObjects;

    // ---- Reading ----

    /** The records the file holds, bottom first; empty when there is none, or it was written for another
     *  version of the app (then it is deleted), or it cannot be read. */
    List<Entry> read() {
        List<Entry> out = new ArrayList<>();
        if (!file.isFile()) return out;
        byte[] bytes;
        try {
            bytes = Files.readAllBytes(file.toPath());
        } catch (IOException e) {
            System.err.println("[aosp-app] the task's saved state is unreadable: " + e);
            return out;
        }
        Parcel p = Parcel.obtain();
        try {
            p.unmarshall(bytes, 0, bytes.length);
            p.setDataPosition(0);
            if (p.readInt() != MAGIC || p.readInt() != FORMAT) {
                System.err.println("[aosp-app] the task's saved state is of another format: dropped");
                delete();
                return out;
            }
            long code = p.readLong(), time = p.readLong();
            if (code != versionCode || time != updateTime) {
                // ActivityManagerService force-stops a package that is replaced: its activities go.
                System.err.println("[aosp-app] the task's saved state is of another version of the app ("
                        + code + "): dropped");
                delete();
                return out;
            }
            int n = p.readInt();
            for (int i = 0; i < n; i++) {
                Entry e = new Entry();
                e.activity = p.readString();
                e.intent = intent(p.createByteArray());
                e.launchedFrom = p.readString();
                e.resultTo = p.readInt();
                e.resultWho = p.readString();
                e.requestCode = p.readInt();
                e.requestedOrientation = p.readInt();
                e.icicle = bundle(p.createByteArray());
                e.persistentState = persistable(p.createByteArray());
                out.add(e);
            }
            written = bytes;
            return out;
        } catch (RuntimeException e) {
            System.err.println("[aosp-app] the task's saved state is damaged (" + e + "): dropped");
            delete();
            return new ArrayList<>();
        } finally {
            p.recycle();
        }
    }

    private static Parcel parcel(byte[] b) {
        Parcel p = Parcel.obtain();
        p.unmarshall(b, 0, b.length);
        p.setDataPosition(0);
        return p;
    }

    private static Intent intent(byte[] b) {
        if (b == null) return null;
        Parcel p = parcel(b);
        try {
            return Intent.CREATOR.createFromParcel(p);
        } finally {
            p.recycle();
        }
    }

    /** The Bundle still in its parcelled form: ActivityThread gives it the app's class loader before
     *  onCreate reads it (performLaunchActivity's r.state.setClassLoader). */
    private static Bundle bundle(byte[] b) {
        if (b == null) return null;
        Parcel p = parcel(b);
        try {
            return Bundle.CREATOR.createFromParcel(p);
        } finally {
            p.recycle();
        }
    }

    private static PersistableBundle persistable(byte[] b) {
        if (b == null) return null;
        Parcel p = parcel(b);
        try {
            return PersistableBundle.CREATOR.createFromParcel(p);
        } finally {
            p.recycle();
        }
    }
}
