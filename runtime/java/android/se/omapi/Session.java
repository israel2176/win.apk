// android.se.omapi.Session -- a connection to one secure element, from which
// channels to its applets are opened. Reader.openSession() never yields one
// here (see SEService), and the constructor is not public, as in AOSP. Mirrors
// frameworks/base/omapi/java/android/se/omapi/Session.java.
package android.se.omapi;

import java.io.IOException;

public final class Session {

    private final Reader mReader;
    private boolean mClosed;

    Session(Reader reader) {
        mReader = reader;
    }

    public Reader getReader() { return mReader; }

    /** The element's answer to reset, or null when it gave none. */
    public byte[] getATR() { return null; }

    public void close() { mClosed = true; }

    public boolean isClosed() { return mClosed; }

    public void closeChannels() {}

    public Channel openBasicChannel(byte[] aid, byte p2) throws IOException {
        throw new IOException("Secure Element is not present.");
    }

    public Channel openBasicChannel(byte[] aid) throws IOException {
        return openBasicChannel(aid, (byte) 0x00);
    }

    public Channel openLogicalChannel(byte[] aid, byte p2) throws IOException {
        throw new IOException("Secure Element is not present.");
    }

    public Channel openLogicalChannel(byte[] aid) throws IOException {
        return openLogicalChannel(aid, (byte) 0x00);
    }
}
