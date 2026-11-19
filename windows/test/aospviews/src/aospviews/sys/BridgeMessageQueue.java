package aospviews.sys;

import android.os.MessageQueue;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

/** android.os.MessageQueue's natives (libutils' Looper: an epoll fd and an eventfd) as a monitor
 *  per queue: pollOnce waits until woken or timed out, wake wakes it. No file-descriptor events.
 *  Win.apk's own MessageQueue (NativeLooper) is the real counterpart. */
public final class BridgeMessageQueue {
    private BridgeMessageQueue() {}

    private static final class Q {
        boolean woken, polling;
    }
    private static final Map<Long, Q> QUEUES = new ConcurrentHashMap<>();
    private static final AtomicLong IDS = new AtomicLong();

    public static long nativeInit() {
        long id = IDS.incrementAndGet();
        QUEUES.put(id, new Q());
        return id;
    }
    public static void nativeDestroy(long p) { QUEUES.remove(p); }

    public static void nativePollOnce(MessageQueue mq, long p, int timeoutMillis) {
        Q q = QUEUES.get(p);
        synchronized (q) {
            try {
                q.polling = true;
                long end = timeoutMillis < 0 ? Long.MAX_VALUE : System.currentTimeMillis() + timeoutMillis;
                while (!q.woken) {
                    long left = end - System.currentTimeMillis();
                    if (left <= 0) break;
                    q.wait(timeoutMillis < 0 ? 0 : left);
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            } finally {
                q.woken = false;
                q.polling = false;
            }
        }
    }

    public static void nativeWake(long p) {
        Q q = QUEUES.get(p);
        if (q == null) return;
        synchronized (q) {
            q.woken = true;
            q.notifyAll();
        }
    }

    public static boolean nativeIsPolling(long p) {
        Q q = QUEUES.get(p);
        return q != null && q.polling;
    }

    public static void nativeSetFileDescriptorEvents(long p, int fd, int events) {}
}
