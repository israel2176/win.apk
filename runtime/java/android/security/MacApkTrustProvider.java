// android.security.MacApkTrustProvider -- the platform's default
// TrustManagerFactory ("PKIX", alias "X509"), shaped the way Android's is.
//
// WHY: on a device the default TrustManagerFactory comes from Conscrypt
// (TrustManagerImpl), and Conscrypt's engines call
// checkServerTrusted(chain, authType, engine) with authType "GENERIC" for a
// TLS 1.3 handshake (TLS 1.3 has no key-exchange-specific auth type). An app
// that bundles its own Conscrypt (Google Play services does: its
// com.google.android.gms.org.conscrypt) takes the PLATFORM default trust
// manager and passes it "GENERIC". Ours was the JDK's X509TrustManagerImpl,
// whose EndEntityChecker names the same case "UNKNOWN" and rejects anything
// else ("Unknown authType: GENERIC"), so every TLS 1.3 connection such an app
// made failed its handshake -- measured: Play services' checkin to
// android.googleapis.com.
//
// HOW: the JDK's PKIX factory does the work (same trust store, same path
// validation, same hostname/endpoint checks); each X509ExtendedTrustManager
// it returns is wrapped so the one Android-only spelling, "GENERIC", reaches
// the JDK as its own name for the same thing, "UNKNOWN". Every other argument
// passes through untouched, so nothing is trusted that was not before.
package android.security;

import java.net.Socket;
import java.security.InvalidAlgorithmParameterException;
import java.security.KeyStore;
import java.security.KeyStoreException;
import java.security.Provider;
import java.security.Security;
import java.security.cert.CertificateException;
import java.security.cert.X509Certificate;
import javax.net.ssl.ManagerFactoryParameters;
import javax.net.ssl.SSLEngine;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.TrustManagerFactorySpi;
import javax.net.ssl.X509ExtendedTrustManager;

public final class MacApkTrustProvider extends Provider {
    private static final String NAME = "MacApkTrust";

    private MacApkTrustProvider() {
        super(NAME, "1.0", "Android-shaped default TrustManagerFactory over the JDK's PKIX");
        put("TrustManagerFactory.PKIX", Factory.class.getName());
        put("Alg.Alias.TrustManagerFactory.X509", "PKIX");
        put("Alg.Alias.TrustManagerFactory.X.509", "PKIX");
        // Conscrypt's KeyManagerFactory: "PKIX", alias "X509". The JDK names its
        // own "SunX509" and "NewSunX509" (alias PKIX) only, so an app that loads
        // its client certificate with getInstance("X509") (Cal) got
        // NoSuchAlgorithmException and talked to its servers without it.
        put("KeyManagerFactory.PKIX", KmFactory.class.getName());
        put("Alg.Alias.KeyManagerFactory.X509", "PKIX");
    }

    /** The JDK's NewSunX509 (X509KeyManagerImpl: several keys, aliases chosen per
     *  handshake), as Conscrypt's KeyManagerFactoryImpl is on Android. */
    public static final class KmFactory extends javax.net.ssl.KeyManagerFactorySpi {
        private final javax.net.ssl.KeyManagerFactory jdk;
        public KmFactory() throws java.security.GeneralSecurityException {
            jdk = javax.net.ssl.KeyManagerFactory.getInstance("NewSunX509", "SunJSSE");
        }
        @Override protected void engineInit(KeyStore ks, char[] password)
                throws KeyStoreException, java.security.NoSuchAlgorithmException,
                       java.security.UnrecoverableKeyException {
            jdk.init(ks, password);
        }
        @Override protected void engineInit(ManagerFactoryParameters spec)
                throws InvalidAlgorithmParameterException { jdk.init(spec); }
        @Override protected javax.net.ssl.KeyManager[] engineGetKeyManagers() { return jdk.getKeyManagers(); }
    }

    /** First in the provider list, so an unqualified TrustManagerFactory
     *  lookup (the default) resolves here. Idempotent. */
    public static void install() {
        if (Security.getProvider(NAME) == null) Security.insertProviderAt(new MacApkTrustProvider(), 1);
    }

    public static final class Factory extends TrustManagerFactorySpi {
        private final TrustManagerFactory jdk;
        public Factory() throws java.security.GeneralSecurityException {
            jdk = TrustManagerFactory.getInstance("PKIX", "SunJSSE");
        }
        @Override protected void engineInit(KeyStore ks) throws KeyStoreException { jdk.init(ks); }
        @Override protected void engineInit(ManagerFactoryParameters spec)
                throws InvalidAlgorithmParameterException { jdk.init(spec); }
        @Override protected TrustManager[] engineGetTrustManagers() {
            TrustManager[] tms = jdk.getTrustManagers();
            for (int i = 0; i < tms.length; i++) {
                if (tms[i] instanceof X509ExtendedTrustManager x) tms[i] = new Tm(x);
            }
            return tms;
        }
    }

    private static String jdkAuthType(String authType) {
        return "GENERIC".equals(authType) ? "UNKNOWN" : authType;
    }

    static final class Tm extends X509ExtendedTrustManager {
        private final X509ExtendedTrustManager d;
        Tm(X509ExtendedTrustManager d) { this.d = d; }
        @Override public void checkClientTrusted(X509Certificate[] c, String a) throws CertificateException {
            d.checkClientTrusted(c, jdkAuthType(a));
        }
        @Override public void checkServerTrusted(X509Certificate[] c, String a) throws CertificateException {
            try { d.checkServerTrusted(c, jdkAuthType(a)); }
            catch (CertificateException e) { explain(c, null, null, e); throw e; }
        }
        @Override public void checkClientTrusted(X509Certificate[] c, String a, Socket s) throws CertificateException {
            d.checkClientTrusted(c, jdkAuthType(a), s);
        }
        @Override public void checkServerTrusted(X509Certificate[] c, String a, Socket s) throws CertificateException {
            try { d.checkServerTrusted(c, jdkAuthType(a), s); }
            catch (CertificateException e) {
                explain(c, s, s instanceof javax.net.ssl.SSLSocket ss ? ss.getHandshakeSession() : null, e);
                throw e;
            }
        }
        @Override public void checkClientTrusted(X509Certificate[] c, String a, SSLEngine e) throws CertificateException {
            d.checkClientTrusted(c, jdkAuthType(a), e);
        }
        @Override public void checkServerTrusted(X509Certificate[] c, String a, SSLEngine e) throws CertificateException {
            try { d.checkServerTrusted(c, jdkAuthType(a), e); }
            catch (CertificateException x) { explain(c, e, e == null ? null : e.getHandshakeSession(), x); throw x; }
        }
        @Override public X509Certificate[] getAcceptedIssuers() { return d.getAcceptedIssuers(); }

        /** A server's chain this trust manager refused, once per host and reason: what
         *  was sent (subject <- issuer), how many anchors this manager holds, the name
         *  asked for (SNI) and whose TLS stack asked. "PKIX path building failed" alone
         *  cannot tell a missing root (a filtering proxy's, a store that was not the
         *  host's) from a server's wrong certificate (no SNI) or a constraint. */
        private void explain(X509Certificate[] chain, Object via, javax.net.ssl.SSLSession session,
                             CertificateException e) {
            try {
                String host = session == null ? null : session.getPeerHost();
                StringBuilder sni = new StringBuilder();
                if (session instanceof javax.net.ssl.ExtendedSSLSession x) {
                    try {
                        for (javax.net.ssl.SNIServerName n : x.getRequestedServerNames()) {
                            if (n instanceof javax.net.ssl.SNIHostName h) sni.append(h.getAsciiName());
                        }
                    } catch (Throwable ignored) { }
                }
                String key = host + "|" + e.getMessage();
                if (!sExplained.add(key) || sExplained.size() > 64) return;
                StringBuilder sent = new StringBuilder();
                for (int i = 0; chain != null && i < chain.length; i++) {
                    if (i > 0) sent.append(" ; ");
                    sent.append(chain[i].getSubjectX500Principal().getName()).append(" <- ")
                        .append(chain[i].getIssuerX500Principal().getName());
                }
                int anchors = -1;
                try { anchors = d.getAcceptedIssuers().length; } catch (Throwable ignored) { }
                com.macapk.loader.NativeLog.w("MacApkTrust", "refused " + (host == null ? "(unknown host)" : host)
                    + " (SNI " + (sni.length() == 0 ? "none" : sni) + ", via "
                    + (via == null ? "no socket or engine" : via.getClass().getName()) + ", " + anchors
                    + " anchors): " + e.getMessage() + "; chain sent: " + sent);
            } catch (Throwable ignored) { }
        }
    }

    private static final java.util.Set<String> sExplained =
        java.util.concurrent.ConcurrentHashMap.newKeySet();
}
