// The one pending HostResult of a start being handed to the host: the task manager offers it right
// before a host handler runs (on the same thread), and the handler claims it for the intent it was
// given; a start that was not for a result offers none, and the handler then claims null (a device's
// answer to a caller that did not ask).
package com.macapk.loader.app;

import android.content.Intent;

public final class HostResults {
    private HostResults() {}

    private static final ThreadLocal<Object[]> sPending = new ThreadLocal<>();

    /** The result `launch`'s handler may claim, or nothing (null). */
    public static void offer(Intent launch, HostResult result) {
        if (result == null) sPending.remove();
        else sPending.set(new Object[] {launch, result});
    }

    /** The result offered for `launch` (the intent the handler was given), at most once. */
    public static HostResult claim(Intent launch) {
        Object[] p = sPending.get();
        if (p == null || p[0] != launch) return null;
        sPending.remove();
        return (HostResult) p[1];
    }
}
