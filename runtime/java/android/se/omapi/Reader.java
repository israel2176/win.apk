// android.se.omapi.Reader -- one secure element reader (a SIM slot, the
// embedded SE). SEService never connects here, so none is ever handed out and
// the constructor is not public, as in AOSP; the class is the element type of
// SEService.getReaders(). Mirrors
// frameworks/base/omapi/java/android/se/omapi/Reader.java.
package android.se.omapi;

import java.io.IOException;

public final class Reader {

    private final String mName;
    private final SEService mService;

    Reader(SEService service, String name) {
        mService = service;
        mName = name;
    }

    public String getName() { return mName; }

    /** @throws IOException always: there is no secure element to talk to */
    public Session openSession() throws IOException {
        throw new IOException("Secure Element is not present.");
    }

    public boolean isSecureElementPresent() { return false; }

    public SEService getSEService() { return mService; }

    public void closeSessions() {}
}
