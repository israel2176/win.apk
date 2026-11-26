// PbeProvidersTest -- the password-based (PBE) crypto an app gets by name, against Android 14's:
// the names Android answers from its BouncyCastle provider resolve to "BC" here too
// (android.security.MacApkProviderPreference, which the host sets straight after JNI_CreateJavaVM),
// with the bytes AOSP 14's BC gives, and everything else stays where it was (AES, GCM, SHA, HMAC,
// PBKDF2 and TLS on the JDK's providers).
//
//   javac -d out windows/test/java/PbeProvidersTest.java
//   MACAPK_LOG_LEVEL=7 winapk-runtime --data <scratch dir> --java-test out PbeProvidersTest
//
// In winapk-runtime (--java-test), since what is checked is that the host set the preference before
// the JVM's first provider lookup; the BC provider is added here as Boot adds it in an app's run.
//
// The expected ciphertexts and MAC were made with AOSP 14's BouncyCastle (android-all
// 14-robolectric-10818077, com.android.internal.org.bouncycastle, BC 1.68 as AOSP trims and patches
// it) as the only provider of these names, as on a device, which has no SunJCE.
//
// Exits 0 when every check passes.

import java.security.AlgorithmParameters;
import java.security.MessageDigest;
import java.security.Provider;
import java.security.Security;
import java.util.Arrays;
import java.util.HexFormat;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.Mac;
import javax.crypto.SecretKey;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.PBEKeySpec;
import javax.crypto.spec.PBEParameterSpec;
import javax.crypto.spec.SecretKeySpec;
import javax.net.ssl.SSLContext;

public final class PbeProvidersTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "ok   " : "FAIL ") + what);
        if (!ok) failures++;
    }

    private interface Step { Object run() throws Exception; }

    /** What a step answers, or the exception's class and message. */
    private static String attempt(Step s) {
        try {
            Object o = s.run();
            return o instanceof byte[] ? HexFormat.of().formatHex((byte[]) o) : String.valueOf(o);
        } catch (Exception e) {
            return e.getClass().getSimpleName() + ": " + e.getMessage();
        }
    }

    private static final byte[] PLAIN = "The quick brown fox".getBytes();
    private static final String NON_ASCII = "pässwörd";
    private static final int ITERATIONS = 77;

    private static PBEParameterSpec spec(int saltLength) {
        return new PBEParameterSpec(Arrays.copyOf("0123456789abcdef".getBytes(), saltLength), ITERATIONS);
    }

    // {algorithm, password, salt length, AOSP 14's BC ciphertext of PLAIN}
    private static final String[][] ANDROID = {
        {"PBEWithMD5AndDES", "password", "4", "178596b73d2e304a397695f30352278192a741ffbd3ce0b9"},
        {"PBEWithMD5AndDES", "password", "8", "da77110ec33a75ef752c9937a85c6996de91589f788a00e8"},
        {"PBEWithMD5AndDES", "password", "16", "816d2f12e284b03fb6e4af787b0bea540160c17fba2f8908"},
        {"PBEWithMD5AndDES", NON_ASCII, "4", "c43354c9024dcf03f70dbb28970d8e4c4f4b052d40b56e4b"},
        {"PBEWithMD5AndDES", NON_ASCII, "8", "b5532cb426a6a2987f6f557f638995ca22b11797af1dd162"},
        {"PBEWithMD5AndDES", NON_ASCII, "16", "b109be75ecd6dc435d66f6d5e4b6830a55abf48009816f52"},
        // Salts under 8 bytes are left out: they fall through to BC with SunJCE's key encoding
        // (MacApkProviderPreference, "SCOPE").
        {"PBEWithSHA1AndDESede", "password", "8", "2e14d106e11abe2abab79b336c0c940d734a4c7d8d24e4f2"},
        {"PBEWithSHA1AndDESede", "password", "16", "09fc95b48921a7cf8bfc220b24f439021737c4a046bb64b1"},
        {"PBEWithSHA1AndDESede", NON_ASCII, "4", "60388a55f60d0dd3755139e926eb36de6d4c8f697b1069d1"},
        {"PBEWithSHA1AndDESede", NON_ASCII, "8", "cc6d3587d3502113691da00f61de66d953f26cf4b58da440"},
        {"PBEWithSHA1AndDESede", NON_ASCII, "16", "acd74bdae789ef2e1187bc756e6f8f3daf8adf09b07f0d7a"},
    };

    public static void main(String[] args) throws Exception {
        String preferred = Security.getProperty("jdk.security.provider.preferred");
        check(preferred != null && preferred.contains("Cipher.PBEWithMD5AndDES:BC"),
              "the host set jdk.security.provider.preferred before Java ran: " + preferred);

        // As Boot does in an app's run: BC last, so it only fills the gaps but for the preferred names.
        if (Security.getProvider("BC") == null) {
            Security.addProvider((Provider) Class.forName("org.bouncycastle.jce.provider.BouncyCastleProvider")
                .getDeclaredConstructor().newInstance());
        }
        check(Security.getProviders()[Security.getProviders().length - 1].getName().equals("BC"),
              "BC is the last provider (the list is unchanged)");

        // ---- Which provider answers each name ----
        String[][] resolves = {
            // Android's BC, here too
            {"Cipher", "PBEWithMD5AndDES", "BC"},
            {"Cipher", "PBEWITHMD5ANDDES", "BC"},
            {"SecretKeyFactory", "PBEWithMD5AndDES", "BC"},
            {"Mac", "PBEWithHmacSHA1", "BC"},
            {"Mac", "PBEWithHmacSHA224", "BC"},
            {"Mac", "PBEWithHmacSHA256", "BC"},
            {"Mac", "PBEWithHmacSHA384", "BC"},
            {"Mac", "PBEWithHmacSHA512", "BC"},
            // Names only BC has: BC already, as before
            {"Cipher", "PBEWITHSHAAND256BITAES-CBC-BC", "BC"},
            {"SecretKeyFactory", "PBEWITHSHAAND256BITAES-CBC-BC", "BC"},
            // Left on SunJCE (MacApkProviderPreference, "SCOPE")
            {"Cipher", "PBEWithSHA1AndDESede", "SunJCE"},
            {"SecretKeyFactory", "PBEWithSHA1AndDESede", "SunJCE"},
            {"AlgorithmParameters", "PBEWithMD5AndDES", "SunJCE"},
            // The JDK's, as before
            {"Cipher", "AES/GCM/NoPadding", "SunJCE"},
            {"Cipher", "AES/CBC/PKCS5Padding", "SunJCE"},
            {"Cipher", "AES", "SunJCE"},
            {"Cipher", "ChaCha20-Poly1305", "SunJCE"},
            {"KeyGenerator", "AES", "SunJCE"},
            {"Mac", "HmacSHA256", "SunJCE"},
            {"Mac", "HmacSHA1", "SunJCE"},
            {"SecretKeyFactory", "PBKDF2WithHmacSHA1", "SunJCE"},
            {"SecretKeyFactory", "PBKDF2WithHmacSHA256", "SunJCE"},
            {"MessageDigest", "SHA-256", "SUN"},
            {"MessageDigest", "SHA-1", "SUN"},
            {"MessageDigest", "MD5", "SUN"},
            {"SSLContext", "TLS", "SunJSSE"},
        };
        for (String[] r : resolves) {
            String got = attempt(() -> switch (r[0]) {
                case "Cipher" -> Cipher.getInstance(r[1]).getProvider().getName();
                case "SecretKeyFactory" -> SecretKeyFactory.getInstance(r[1]).getProvider().getName();
                case "Mac" -> Mac.getInstance(r[1]).getProvider().getName();
                case "AlgorithmParameters" -> AlgorithmParameters.getInstance(r[1]).getProvider().getName();
                case "KeyGenerator" -> KeyGenerator.getInstance(r[1]).getProvider().getName();
                case "MessageDigest" -> MessageDigest.getInstance(r[1]).getProvider().getName();
                case "SSLContext" -> SSLContext.getInstance(r[1]).getProvider().getName();
                default -> "?";
            });
            check(got.equals(r[2]), r[0] + "." + r[1] + " from " + r[2] + " (got " + got + ")");
        }

        // ---- The bytes AOSP 14's BC gives, and back ----
        for (String[] v : ANDROID) {
            String alg = v[0];
            char[] password = v[1].toCharArray();
            int salt = Integer.parseInt(v[2]);
            String what = alg + (v[1].equals(NON_ASCII) ? " non-ASCII password" : "") + " salt " + salt;
            String got = attempt(() -> {
                SecretKey key = SecretKeyFactory.getInstance(alg).generateSecret(new PBEKeySpec(password));
                Cipher c = Cipher.getInstance(alg);
                c.init(Cipher.ENCRYPT_MODE, key, spec(salt));
                byte[] ct = c.doFinal(PLAIN);
                Cipher d = Cipher.getInstance(alg);
                d.init(Cipher.DECRYPT_MODE, key, spec(salt));
                if (!Arrays.equals(d.doFinal(ct), PLAIN)) throw new IllegalStateException("decrypts to other bytes");
                return ct;
            });
            check(got.equals(v[3]), what + ": Android's ciphertext, decrypts with the spec (got " + got + ")");
        }

        // PBEWithMD5AndDES: getParameters() is null after an init with a password-only key, as on a
        // device (no AlgorithmParameters for 1.2.840.113549.1.5.3 there), so apps pass the spec; the
        // JDK's AlgorithmParameters for the name still decrypt.
        for (int salt : new int[] {4, 8, 16}) {
            String got = attempt(() -> {
                SecretKey key = SecretKeyFactory.getInstance("PBEWithMD5AndDES")
                    .generateSecret(new PBEKeySpec("password".toCharArray()));
                Cipher c = Cipher.getInstance("PBEWithMD5AndDES");
                c.init(Cipher.ENCRYPT_MODE, key, spec(salt));
                byte[] ct = c.doFinal(PLAIN);
                AlgorithmParameters made = AlgorithmParameters.getInstance("PBEWithMD5AndDES");
                made.init(spec(salt));
                Cipher d = Cipher.getInstance("PBEWithMD5AndDES");
                d.init(Cipher.DECRYPT_MODE, key, made);
                return (c.getParameters() == null) + " " + Arrays.equals(d.doFinal(ct), PLAIN);
            });
            check(got.equals("true true"), "PBEWithMD5AndDES salt " + salt
                  + ": getParameters() null as on Android, AlgorithmParameters decrypt (got " + got + ")");
        }

        // A key whose PBEKeySpec carries the salt, with no parameters at init: Android derives from
        // the key's salt (SunJCE drew a random one instead), and getParameters() gives it back.
        for (int salt : new int[] {4, 8}) {
            String got = attempt(() -> {
                PBEParameterSpec ps = spec(salt);
                SecretKey key = SecretKeyFactory.getInstance("PBEWithMD5AndDES")
                    .generateSecret(new PBEKeySpec("password".toCharArray(), ps.getSalt(), ITERATIONS));
                Cipher c = Cipher.getInstance("PBEWithMD5AndDES");
                c.init(Cipher.ENCRYPT_MODE, key);
                byte[] ct = c.doFinal(PLAIN);
                Cipher d = Cipher.getInstance("PBEWithMD5AndDES");
                d.init(Cipher.DECRYPT_MODE, key, c.getParameters());
                if (!Arrays.equals(d.doFinal(ct), PLAIN)) throw new IllegalStateException("decrypts to other bytes");
                return ct;
            });
            String expected = salt == 4 ? ANDROID[0][3] : ANDROID[1][3];
            check(got.equals(expected), "PBEWithMD5AndDES key with salt " + salt
                  + ", no init parameters: Android's ciphertext, getParameters() decrypts (got " + got + ")");
        }

        // What worked before still works: a SunJCE key, or the raw password as a SecretKeySpec, with an
        // 8-byte salt (BC refuses both keys, "Algorithm requires a PBE key", and the JDK's delayed provider
        // selection falls through to SunJCE, whose bytes are the same).
        String sunKey = attempt(() -> {
            SecretKey key = SecretKeyFactory.getInstance("PBEWithMD5AndDES", "SunJCE")
                .generateSecret(new PBEKeySpec("password".toCharArray()));
            Cipher c = Cipher.getInstance("PBEWithMD5AndDES");
            c.init(Cipher.ENCRYPT_MODE, key, spec(8));
            return c.doFinal(PLAIN);
        });
        check(sunKey.equals(ANDROID[1][3]), "PBEWithMD5AndDES with a SunJCE key, salt 8 (got " + sunKey + ")");
        String rawKey = attempt(() -> {
            Cipher c = Cipher.getInstance("PBEWithMD5AndDES");
            c.init(Cipher.ENCRYPT_MODE, new SecretKeySpec("password".getBytes(), "PBEWithMD5AndDES"), spec(8));
            return c.doFinal(PLAIN);
        });
        check(rawKey.equals(ANDROID[1][3]), "PBEWithMD5AndDES with a SecretKeySpec key, salt 8 (got " + rawKey + ")");

        // PBEWithSHA1AndDESede (on SunJCE): getParameters() round trip with an 8+ byte salt, as before.
        for (int salt : new int[] {8, 16}) {
            String got = attempt(() -> {
                SecretKey key = SecretKeyFactory.getInstance("PBEWithSHA1AndDESede")
                    .generateSecret(new PBEKeySpec("password".toCharArray()));
                Cipher c = Cipher.getInstance("PBEWithSHA1AndDESede");
                c.init(Cipher.ENCRYPT_MODE, key, spec(salt));
                byte[] ct = c.doFinal(PLAIN);
                Cipher d = Cipher.getInstance("PBEWithSHA1AndDESede");
                d.init(Cipher.DECRYPT_MODE, key, c.getParameters());
                return Arrays.equals(d.doFinal(ct), PLAIN);
            });
            check(got.equals("true"), "PBEWithSHA1AndDESede salt " + salt + ": getParameters() decrypts (got " + got + ")");
        }
        String shortDesede = attempt(() -> {
            SecretKey key = SecretKeyFactory.getInstance("PBEWithSHA1AndDESede")
                .generateSecret(new PBEKeySpec("password".toCharArray()));
            Cipher c = Cipher.getInstance("PBEWithSHA1AndDESede");
            c.init(Cipher.ENCRYPT_MODE, key, spec(4));
            byte[] ct = c.doFinal(PLAIN);
            Cipher d = Cipher.getInstance("PBEWithSHA1AndDESede");
            d.init(Cipher.DECRYPT_MODE, key, spec(4));
            return Arrays.equals(d.doFinal(ct), PLAIN);
        });
        check(shortDesede.equals("true"), "PBEWithSHA1AndDESede salt 4 decrypts with the spec (got " + shortDesede + ")");

        // PBEWithHmacSHA1: the key from the SecretKeyFactory of the name, the PKCS#12 key AOSP's BC derives.
        String mac = attempt(() -> {
            SecretKey key = SecretKeyFactory.getInstance("PBEWithHmacSHA1")
                .generateSecret(new PBEKeySpec("password".toCharArray()));
            Mac m = Mac.getInstance("PBEWithHmacSHA1");
            m.init(key, new PBEParameterSpec("saltsalt".getBytes(), 100));
            return m.doFinal("x".getBytes());
        });
        check(mac.equals("adf2eea7acc7acc2321a78de722f4ca026899eea"), "PBEWithHmacSHA1: Android's MAC (got " + mac + ")");

        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
