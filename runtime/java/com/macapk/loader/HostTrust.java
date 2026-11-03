// com.macapk.loader.HostTrust -- the JVM's default TLS trust store on Windows.
//
// Its own class, not a method of Boot, because two entry points need it and
// only one of them boots an app: Boot.run (an app's process) and
// com.macapk.gms.GmsCli.run (`winapk-runtime --gms`, what Settings > Google
// account runs). Calling into Boot from the second would run Boot's static
// initialisers for a process that has no app. Without it there, the sign-in's
// token exchange behind NetFree failed with "PKIX path building failed" while
// the same exchange from inside an app worked.
package com.macapk.loader;

public final class HostTrust {
    private static final String TAG = "Win.apk.HostTrust";

    private HostTrust() {}

    /**
     * On Windows, the trust anchors every TLS connection checks against are the
     * JDK's CAs AND the ones Windows itself trusts. On a device the system store
     * holds what the user's device trusts; on a PC that is the Windows root
     * store, which also holds the root of a filtering proxy (NetFree), a
     * company's TLS inspection or a locally installed CA. With the JDK's
     * cacerts alone every connection behind such a proxy failed with "PKIX
     * path building failed". The Windows store alone is not enough either: it
     * fetches public roots on demand, so one it never needed may be missing.
     *
     * The merged set is written to a PKCS12 file and made the JVM's default
     * trust store, before anything initialises TLS: apps (OkHttp, Volley,
     * Conscrypt-style code) and the GMS clients build their trust managers from
     * TrustManagerFactory.init((KeyStore) null), which reads exactly that
     * default. An explicit javax.net.ssl.trustStore is left alone, which also
     * makes a second call a no-op.
     */
    public static synchronized void install() {
        if (!System.getProperty("os.name", "").startsWith("Windows")) return;
        if (System.getProperty("javax.net.ssl.trustStore") != null) return;
        try {
            java.security.KeyStore merged = java.security.KeyStore.getInstance("PKCS12");
            merged.load(null, null);
            int n = 0;
            javax.net.ssl.TrustManagerFactory jdk = javax.net.ssl.TrustManagerFactory.getInstance(
                javax.net.ssl.TrustManagerFactory.getDefaultAlgorithm());
            jdk.init((java.security.KeyStore) null);
            for (javax.net.ssl.TrustManager tm : jdk.getTrustManagers()) {
                if (!(tm instanceof javax.net.ssl.X509TrustManager)) continue;
                for (java.security.cert.X509Certificate c : ((javax.net.ssl.X509TrustManager) tm).getAcceptedIssuers()) {
                    merged.setCertificateEntry("jdk-" + (n++), c);
                }
            }
            int fromWindows = 0;
            java.security.KeyStore win = java.security.KeyStore.getInstance("Windows-ROOT");
            win.load(null, null);
            for (java.util.Enumeration<String> e = win.aliases(); e.hasMoreElements();) {
                java.security.cert.Certificate c = win.getCertificate(e.nextElement());
                if (c == null || merged.getCertificateAlias(c) != null) continue;
                merged.setCertificateEntry("windows-" + (fromWindows++), c);
            }
            java.io.File file = java.io.File.createTempFile("winapk-trust-", ".p12");
            file.deleteOnExit();
            char[] password = "winapk".toCharArray();
            try (java.io.FileOutputStream out = new java.io.FileOutputStream(file)) {
                merged.store(out, password);
            }
            System.setProperty("javax.net.ssl.trustStore", file.getAbsolutePath());
            System.setProperty("javax.net.ssl.trustStoreType", "PKCS12");
            System.setProperty("javax.net.ssl.trustStorePassword", "winapk");
            log(false, "TLS trust anchors: " + n + " from the JDK + " + fromWindows
                + " more that Windows trusts");
        } catch (Throwable t) {
            log(true, "TLS trust anchors: kept the JDK's (" + t + ")");
        }
    }

    // NativeLog is a native: a bare JVM (debug/gms.sh) has none bound.
    private static void log(boolean warn, String m) {
        try {
            if (warn) NativeLog.w(TAG, m); else NativeLog.i(TAG, m);
        } catch (Throwable t) {
            System.err.println("[" + TAG + "] " + m);
        }
    }
}
