// android.se.omapi.Channel -- an ISO 7816-4 channel to an applet on a secure
// element. A Session never opens one here (see SEService), and the constructor
// is not public, as in AOSP. Mirrors
// frameworks/base/omapi/java/android/se/omapi/Channel.java.
package android.se.omapi;

import java.io.IOException;

public final class Channel implements java.nio.channels.Channel {

    private final Session mSession;
    private boolean mClosed;

    Channel(Session session) {
        mSession = session;
    }

    @Override public void close() { mClosed = true; }

    @Override public boolean isOpen() { return !mClosed; }

    public boolean isBasicChannel() { return false; }

    /** @throws IOException always: there is no secure element to talk to */
    public byte[] transmit(byte[] command) throws IOException {
        throw new IOException("Secure Element is not present.");
    }

    public Session getSession() { return mSession; }

    /** The applet's answer to SELECT, or null when it gave none. */
    public byte[] getSelectResponse() { return null; }

    public boolean selectNext() throws IOException {
        throw new IOException("Secure Element is not present.");
    }
}
