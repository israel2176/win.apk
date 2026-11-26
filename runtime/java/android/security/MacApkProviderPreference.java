// android.security.MacApkProviderPreference -- make the password-based (PBE)
// crypto services Android answers from its "BC" provider resolve to BC here too.
//
// WHY (not an AOSP class, hence the MacApk prefix -- this is Mac.apk wiring):
//   On a device the provider list is AndroidNSSP, AndroidOpenSSL (Conscrypt),
//   CertPathProvider, AndroidKeyStoreBCWorkaround, BC, HarmonyJSSE,
//   AndroidKeyStore: there is no SunJCE, and Conscrypt implements no PBE
//   Cipher, SecretKeyFactory or Mac (its SecretKeyFactory table is DESEDE
//   only, its AlgorithmParameters AES/ChaCha20/DESEDE/EC/GCM/OAEP/PSS), so an
//   unqualified Cipher/SecretKeyFactory/Mac.getInstance("PBEWith...") is
//   answered by BC (external/bouncycastle, android14-release: DES.java
//   registers Cipher and SecretKeyFactory PBEWITHMD5ANDDES, SHA1/224/256/384/
//   512.java the Mac PBEWITHHMACSHA*). Here the JDK's SunJCE sits ahead of BC
//   (Boot adds BC at the END, so it only fills gaps) and answers those names
//   with JDK semantics instead:
//     - PBES1 (PBEWithMD5AndDES): SunJCE throws "Salt must be 8 bytes long"
//       for any other salt; BC, i.e. Android, takes any length. Waze 4.100
//       (AuthNativeManager.encryptPassword) and Pocket Casts 8.22 (the 16-char
//       ANDROID_ID as salt) die on it. The bytes are the same for an 8-byte
//       salt, so nothing an app stored before changes.
//     - The key classes do not mix: a SunJCE PBEKey handed to BC's cipher is
//       "Algorithm requires a PBE key", so the JDK's delayed provider selection
//       does not fall through to BC on its own.
//     - PBEWithHmacSHA* Macs give DIFFERENT bytes (SunJCE derives the key with
//       PBKDF2, BC with the PKCS#12 generator), so a MAC computed here would
//       not verify against one computed on a phone.
//   getParameters() after a PBEWithMD5AndDES init is null, as on a device:
//   AOSP's BC asks every provider for the key's OID (its Android-changed
//   DefaultJcaJceHelper) and Android has no AlgorithmParameters for
//   1.2.840.113549.1.5.3 (AOSP removed BC's PBEPBKDF1; libcore's StandardNames
//   unprovides it), so an app passes the PBEParameterSpec to decrypt.
//
// HOW: the JDK's own per-algorithm preference, the "jdk.security.provider.
//   preferred" security property (JDK-7092821). It is read ONCE, when the
//   process-wide ProviderList is first built (a list made later by
//   Security.addProvider copies it), so install() must run before anything
//   touches a security provider: the Windows host calls it straight after
//   JNI_CreateJavaVM (windows/host/runtime_main.cpp, create_vm), before any of
//   the framework's Java runs, in every JVM it starts. A preferred provider
//   that is not registered is skipped, so the entries are inert until Boot
//   adds BC. The provider LIST itself is unchanged, and AES, GCM, SHA, HMAC,
//   PBKDF2 and TLS stay on the JDK's providers.
//
// SCOPE: the PBE names that SunJCE, the bundled bcprov and Android's BC all
//   offer, but for PBEWithSHA1AndDESede (and SecretKeyFactory "PBE", its
//   alias on Android): bcprov 1.79's PKCS#12 ciphers return null from
//   getParameters() (BaseBlockCipher leaves pbeAlgorithm unset on that path;
//   fixed in bcprov 1.83, which sets "PKCS12PBE"), where SunJCE and a device
//   return parameters, so preferring it would break an encrypt-then-decrypt
//   with getParameters() that works today. Its difference stays: a salt
//   under 8 bytes falls through to BC with SunJCE's key encoding and gives
//   bytes a device would not. PBKDF2WithHmacSHA* are deliberately NOT here:
//   SunJCE and BC give identical bytes for them and SunJCE's is ~5x faster on
//   HotSpot's SHA intrinsics. windows/test/java/PbeProvidersTest.java checks
//   the resolution and the bytes against AOSP 14's BC.
package android.security;

import java.security.Security;

public final class MacApkProviderPreference {
    static final String PROPERTY = "jdk.security.provider.preferred";
    static final String BC = "BC";

    /** Type.Algorithm entries Android resolves to BC and SunJCE would answer. */
    static final String[] BC_PREFERRED = {
        "Cipher.PBEWithMD5AndDES",
        "SecretKeyFactory.PBEWithMD5AndDES",
        "Mac.PBEWithHmacSHA1",
        "Mac.PBEWithHmacSHA224",
        "Mac.PBEWithHmacSHA256",
        "Mac.PBEWithHmacSHA384",
        "Mac.PBEWithHmacSHA512",
    };

    private MacApkProviderPreference() {}

    /** Append the BC entries to jdk.security.provider.preferred, keeping any
     *  entries already configured ahead of ours. Idempotent. */
    public static void install() {
        String cur = Security.getProperty(PROPERTY);
        StringBuilder sb = new StringBuilder();
        if (cur != null && !cur.trim().isEmpty()) sb.append(cur.trim());
        for (String e : BC_PREFERRED) {
            String entry = e + ":" + BC;
            if (cur != null && cur.contains(entry)) continue;
            if (sb.length() > 0) sb.append(',');
            sb.append(entry);
        }
        Security.setProperty(PROPERTY, sb.toString());
    }
}
