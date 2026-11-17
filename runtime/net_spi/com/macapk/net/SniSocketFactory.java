// SniSocketFactory -- the SSLSocketFactory under an app's https HttpURLConnection:
// the app's (or the JVM default) factory, whose sockets are told the host's
// name (SNI) before the JDK connects them.
//
// WHY: the JDK's HttpsClient names the host on a socket only when the socket is
// its own (SSLSocketImpl.setHost), or, for a socket from another provider, when
// the connection's HostnameVerifier is the JDK's default (it then sets
// SSLParameters' server names along with endpoint identification). Boot makes
// OkHostnameVerifier the default, as on a device, so a socket from another
// provider got no name at all. Play services installs its own Conscrypt as the
// default SSLSocketFactory (ProviderInstaller), and its check-in to
// android.googleapis.com went out with no SNI: Google's front end answered with
// its "invalid2.invalid" certificate, PKIX refused it, the check-in failed, and
// the Play Store stayed at android-id 0 ("Server error", DF-DFERH-01). A
// device's okhttp passes the host on every socket; this does the same.
// The hostname check itself is unchanged: the JDK still asks the connection's
// HostnameVerifier after the handshake.

package com.macapk.net;

import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Method;
import java.net.InetAddress;
import java.net.Socket;
import java.util.List;
import javax.net.ssl.SNIHostName;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

final class SniSocketFactory extends SSLSocketFactory {
    final SSLSocketFactory d;
    private final String host;

    private SniSocketFactory(SSLSocketFactory d, String host) { this.d = d; this.host = host; }

    /** `sf` (unwrapped first) naming `host` on its sockets; `sf` itself when there is no name to give. */
    static SSLSocketFactory forHost(SSLSocketFactory sf, String host) {
        sf = unwrap(sf);
        if (sf == null || !isHostName(host)) return sf;
        return new SniSocketFactory(sf, host);
    }

    /** The factory the app set (or the default), as the app should see it. */
    static SSLSocketFactory unwrap(SSLSocketFactory sf) {
        return sf instanceof SniSocketFactory s ? s.d : sf;
    }

    /** RFC 6066: SNI carries a DNS name, never an IP literal. */
    private static boolean isHostName(String h) {
        if (h == null || h.isEmpty() || h.indexOf(':') >= 0 || h.startsWith("[")) return false;
        if (h.chars().allMatch(c -> c == '.' || (c >= '0' && c <= '9'))) return false;
        return true;
    }

    private Socket name(Socket s) {
        if (!(s instanceof SSLSocket x)) return s;
        // Conscrypt's own setter first (it also keeps the name for session reuse);
        // SSLParameters' server names for every other provider, and for Conscrypt
        // too, whose setSSLParameters takes the SNI name as its hostname.
        try {
            Method m = x.getClass().getMethod("setHostname", String.class);
            m.setAccessible(true);
            m.invoke(x, host);
        } catch (ReflectiveOperationException | RuntimeException noSuchSetter) {
            // Not a Conscrypt socket: the parameters below are its only way.
        }
        try {
            SSLParameters p = x.getSSLParameters();
            List<?> names = p.getServerNames();
            if (names == null || names.isEmpty()) {
                p.setServerNames(List.of(new SNIHostName(host)));
                x.setSSLParameters(p);
            }
        } catch (RuntimeException notAName) {
            // A name SNIHostName refuses goes without, as the JDK's own socket would.
        }
        return s;
    }

    @Override public String[] getDefaultCipherSuites() { return d.getDefaultCipherSuites(); }
    @Override public String[] getSupportedCipherSuites() { return d.getSupportedCipherSuites(); }
    @Override public Socket createSocket() throws IOException { return name(d.createSocket()); }
    @Override public Socket createSocket(Socket s, String h, int port, boolean autoClose) throws IOException {
        return name(d.createSocket(s, h, port, autoClose));
    }
    @Override public Socket createSocket(Socket s, InputStream consumed, boolean autoClose) throws IOException {
        return name(d.createSocket(s, consumed, autoClose));
    }
    @Override public Socket createSocket(String h, int port) throws IOException {
        return name(d.createSocket(h, port));
    }
    @Override public Socket createSocket(String h, int port, InetAddress la, int lp) throws IOException {
        return name(d.createSocket(h, port, la, lp));
    }
    @Override public Socket createSocket(InetAddress a, int port) throws IOException {
        return name(d.createSocket(a, port));
    }
    @Override public Socket createSocket(InetAddress a, int port, InetAddress la, int lp) throws IOException {
        return name(d.createSocket(a, port, la, lp));
    }
}
