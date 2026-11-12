// sun.misc.Cleaner -- libcore's phantom-reference cleaner, which the JDK moved
// to jdk.internal.ref in 9 and so no longer has under this name. Android kept
// it: java.nio's direct buffers hang one off each buffer, and apps reach it by
// name (DirectByteBuffer.cleaner().clean(), Netty, LWJGL-style buffer pools).
//
// A port of AOSP libcore ojluni/src/main/java/sun/misc/Cleaner.java: the same
// doubly linked list that keeps cleaners reachable until their referent is
// not, the same public surface (create, clean). On ART the reference handler
// calls clean() on a Cleaner it dequeues; HotSpot's does not know this class,
// so a daemon thread of our own drains the queue.
//
// Lives beside Unsafe.java for the same reason: javac refuses `package
// sun.misc` outside `--patch-module jdk.unsupported`.
//
// NO em dashes in this file (project convention).
package sun.misc;

import java.lang.ref.PhantomReference;
import java.lang.ref.Reference;
import java.lang.ref.ReferenceQueue;

public class Cleaner extends PhantomReference<Object> {

    private static final ReferenceQueue<Object> sQueue = new ReferenceQueue<>();

    // The list is what keeps a Cleaner alive once its creator drops it: a
    // phantom reference that is itself unreachable is never enqueued.
    private static Cleaner first = null;

    private static boolean sThreadStarted;

    private Cleaner next = null, prev = null;

    private final Runnable thunk;

    private static synchronized Cleaner add(Cleaner cl) {
        if (first != null) {
            cl.next = first;
            first.prev = cl;
        }
        first = cl;
        if (!sThreadStarted) {
            sThreadStarted = true;
            Thread t = new Thread(Cleaner::drain, "sun.misc.Cleaner");
            t.setDaemon(true);
            t.start();
        }
        return cl;
    }

    private static synchronized boolean remove(Cleaner cl) {
        // If already removed, do nothing
        if (cl.next == cl) {
            return false;
        }
        // Update list
        if (first == cl) {
            if (cl.next != null) {
                first = cl.next;
            } else {
                first = cl.prev;
            }
        }
        if (cl.next != null) {
            cl.next.prev = cl.prev;
        }
        if (cl.prev != null) {
            cl.prev.next = cl.next;
        }
        // Indicate removal by pointing the cleaner to itself
        cl.next = cl;
        cl.prev = cl;
        return true;
    }

    private static void drain() {
        for (;;) {
            try {
                Reference<?> r = sQueue.remove();
                if (r instanceof Cleaner) {
                    ((Cleaner) r).clean();
                }
            } catch (InterruptedException e) {
                // Nothing interrupts this thread on purpose; keep draining.
            } catch (Throwable t) {
                // AOSP ends the process when a thunk throws. One app's failed
                // cleanup must not stop every later one, so it is only reported.
                System.err.println("sun.misc.Cleaner: cleanup failed: " + t);
            }
        }
    }

    private Cleaner(Object referent, Runnable thunk) {
        super(referent, sQueue);
        this.thunk = thunk;
    }

    /**
     * Creates a new cleaner.
     *
     * @param ob the referent object to be cleaned
     * @param thunk the cleanup code to be run when the cleaner is invoked; it
     *        runs on the cleaner thread, or on the thread that calls clean()
     * @return the new cleaner, or null when thunk is null
     */
    public static Cleaner create(Object ob, Runnable thunk) {
        if (thunk == null) {
            return null;
        }
        return add(new Cleaner(ob, thunk));
    }

    /** Runs this cleaner, if it has not been run before. */
    public void clean() {
        if (!remove(this)) {
            return;
        }
        thunk.run();
    }
}
