// The result of a start the host answers itself (a picker, the camera, an install, a settings page,
// the permission prompt: what another app or the system would answer on a device), for the activity
// that started it for a result. The task manager makes one for each start it hands to the host
// (com.macapk.loader.app.HostResults) and delivers it as ActivityThread delivers any result: to the
// activity's onActivityResult, or its fragment's, through a client transaction.
package com.macapk.loader.app;

import android.content.Intent;

public interface HostResult {
    /** The request code the activity started with. */
    int requestCode();

    /** The result, delivered when the activity is resumed (as a device does it: after onStart, before
     *  onResume, the activity paused for it first when it is resumed). */
    void deliver(int resultCode, Intent data);

    /** The result, kept for the activity's next resume (a host Cover's uncoverWithResult runs it
     *  right before bringing the activity back). */
    void deliverNow(int resultCode, Intent data);
}
