// com.macapk.loader.DataProtection -- secrets at rest sealed for the Windows user
// (windows/host/data_protection.h: DPAPI, current user, Win.apk's entropy), as the
// launcher seals the app-ops signing key.
//
// Used for the files that hold the Google account's master token and its tokens:
// com.macapk.gms.GmsStore's gms\tokens.dat and the system broker's accounts table
// (com.macapk.accounts.AccountStore.sealedAt). Only the processes that own the account
// open them (GmsStore.owner()); an app's process never does.
//
// What sealing does not do: stop code running as the same Windows user. Every app runs
// with the user's rights and its native code is not confined, so in principle it can open
// what this seals. Sealing keeps the token out of plain files, backups and a copy of the
// data folder taken to another account or PC; keeping it from the apps is the work of who
// reads it (docs/ARCHITECTURE.md, "Google services").
package com.macapk.loader;

import java.io.IOException;

public final class DataProtection {
    private DataProtection() {}

    /** How the bytes are sealed: DPAPI on Windows; a test's own stand-in on a plain JVM. */
    public interface Codec {
        byte[] seal(byte[] plain) throws IOException;
        byte[] unseal(byte[] sealed) throws IOException;
    }

    private static volatile Codec sCodec;

    /** A plain JVM's test has no DPAPI: it seals with this instead. Null goes back to DPAPI. */
    public static void setCodecForTest(Codec c) { sCodec = c; }

    /** `plain` sealed for this Windows user. Throws when it cannot be: the caller then
     *  writes nothing (a secret is never written in the clear instead). */
    public static byte[] seal(byte[] plain) throws IOException {
        Codec c = sCodec;
        if (c != null) return c.seal(plain);
        byte[] out;
        try {
            out = nativeSeal(plain);
        } catch (UnsatisfiedLinkError e) {
            throw new IOException("no data protection in this process (" + e.getMessage() + ")");
        }
        if (out == null) throw new IOException("Windows refused to protect the data");
        return out;
    }

    /** A blob {@link #seal} made, opened. Throws when it is not one, was changed, or
     *  belongs to another Windows user or PC. */
    public static byte[] unseal(byte[] sealed) throws IOException {
        Codec c = sCodec;
        if (c != null) return c.unseal(sealed);
        byte[] out;
        try {
            out = nativeUnseal(sealed);
        } catch (UnsatisfiedLinkError e) {
            throw new IOException("no data protection in this process (" + e.getMessage() + ")");
        }
        if (out == null) throw new IOException("the data could not be opened (changed, or another user's)");
        return out;
    }

    private static native byte[] nativeSeal(byte[] plain);
    private static native byte[] nativeUnseal(byte[] sealed);
}
