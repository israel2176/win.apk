// One-off (to be deleted): what HostTrust.install costs, and what a cheaper store would.
import java.security.KeyStore;
import java.security.cert.Certificate;
import java.security.cert.X509Certificate;
import java.util.*;
import javax.net.ssl.*;

public class HostTrustBench {
    static List<Certificate> collect() throws Exception {
        List<Certificate> all = new ArrayList<>();
        TrustManagerFactory jdk = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        jdk.init((KeyStore) null);
        for (TrustManager tm : jdk.getTrustManagers())
            if (tm instanceof X509TrustManager x) all.addAll(Arrays.asList(x.getAcceptedIssuers()));
        KeyStore win = KeyStore.getInstance("Windows-ROOT");
        win.load(null, null);
        for (Enumeration<String> e = win.aliases(); e.hasMoreElements();) {
            Certificate c = win.getCertificate(e.nextElement());
            if (c != null) all.add(c);
        }
        return all;
    }
    static long store(String type, List<Certificate> certs) throws Exception {
        long t = System.nanoTime();
        KeyStore ks = KeyStore.getInstance(type);
        ks.load(null, null);
        int i = 0;
        for (Certificate c : certs) if (ks.getCertificateAlias(c) == null) ks.setCertificateEntry("c" + (i++), c);
        java.io.File f = java.io.File.createTempFile("bench", "." + type);
        try (var out = new java.io.FileOutputStream(f)) { ks.store(out, "winapk".toCharArray()); }
        long build = System.nanoTime() - t;
        t = System.nanoTime();
        KeyStore back = KeyStore.getInstance(type);
        try (var in = new java.io.FileInputStream(f)) { back.load(in, "winapk".toCharArray()); }
        System.out.printf("  %s: build+store %d ms, load %d ms, %d entries, %d KB%n", type, build / 1000000,
                (System.nanoTime() - t) / 1000000, back.size(), f.length() / 1024);
        return build;
    }
    public static void main(String[] a) throws Exception {
        long t0 = System.nanoTime();
        List<Certificate> certs = collect();
        System.out.printf("collect (JDK cacerts + Windows-ROOT): %d ms, %d certs%n", (System.nanoTime() - t0) / 1000000, certs.size());
        for (int round = 0; round < 2; round++) {
            System.out.println("round " + round);
            store("PKCS12", certs);
            store("JKS", certs);
            System.setProperty("keystore.pkcs12.certProtectionAlgorithm", "NONE");
            System.setProperty("keystore.pkcs12.macAlgorithm", "NONE");
            System.out.print(" (pkcs12 unprotected)");
            store("PKCS12", certs);
            System.clearProperty("keystore.pkcs12.certProtectionAlgorithm");
            System.clearProperty("keystore.pkcs12.macAlgorithm");
        }
    }
}
