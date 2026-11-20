// The request side of the HttpURLConnection an app gets (AndroidHttpHandlerProvider's
// HucConnection / HucsConnection) against Android's, which is OkHttp's
// HttpURLConnectionImpl: getRequestProperties() is the fields the app set, in a
// case-insensitive map with no null key, and throws IllegalStateException once the
// request went out (the JDK's own flag drops after an error answer and disconnect(),
// and its map then comes back with the request line as a key whose value is null:
// Dynatrace's NPE in bit);
// getRequestProperty() is the last value set, Authorization included; setting a
// field after the request went out throws. A loopback server, no network.
//
// Run: javac -d out runtime/net_spi/com/macapk/net/*.java windows/test/java/com/macapk/net/HttpRequestPropertiesTest.java
//      java --add-exports java.base/sun.net.www.protocol.http=ALL-UNNAMED \
//           --add-exports java.base/sun.net.www.protocol.https=ALL-UNNAMED -cp out com.macapk.net.HttpRequestPropertiesTest

package com.macapk.net;

import com.sun.net.httpserver.HttpServer;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.URL;
import java.net.URLStreamHandler;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;

@SuppressWarnings("deprecation")   // new URL(URL, String, URLStreamHandler), as the provider uses
public final class HttpRequestPropertiesTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "[PASS] " : "[FAIL] ") + what);
        if (!ok) failures++;
    }

    private static boolean throwsIse(Runnable r) {
        try {
            r.run();
            return false;
        } catch (IllegalStateException e) {
            return true;
        }
    }

    public static void main(String[] args) throws Exception {
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        server.createContext("/", ex -> {
            byte[] body = "hello".getBytes(StandardCharsets.UTF_8);
            ex.sendResponseHeaders(ex.getRequestURI().getPath().equals("/error") ? 500 : 200, body.length);
            try (OutputStream os = ex.getResponseBody()) { os.write(body); }
        });
        server.start();
        try {
            AndroidHttpHandlerProvider provider = new AndroidHttpHandlerProvider();
            URLStreamHandler http = provider.createURLStreamHandler("http");
            URLStreamHandler https = provider.createURLStreamHandler("https");
            check(http != null && https != null, "the provider hands out both handlers");
            int port = server.getAddress().getPort();

            HttpURLConnection c = (HttpURLConnection) new URL(null, "http://127.0.0.1:" + port + "/", http).openConnection();
            check(c instanceof HucConnection, "http is the wrapper");
            c.setRequestProperty("X-Test", "a");
            c.addRequestProperty("x-test", "b");
            c.setRequestProperty("Authorization", "Bearer t");
            c.setRequestProperty("X-Null", null);
            Map<String, List<String>> before = c.getRequestProperties();
            check(!before.containsKey(null), "no null key before the request");
            check(List.of("a", "b").equals(before.get("X-TEST")), "values in order, any case: " + before);
            check(before.containsKey("authorization"), "Authorization listed (the JDK hides it)");
            check(!before.containsKey("X-Null"), "a null value is ignored, as OkHttp does");
            check(throwsUoe(before), "the map is unmodifiable");
            check("b".equals(c.getRequestProperty("X-TEST")), "getRequestProperty: the last value");
            check("Bearer t".equals(c.getRequestProperty("authorization")), "getRequestProperty: Authorization");

            check(c.getResponseCode() == 200, "the request answers 200");
            try (InputStream in = c.getInputStream()) { in.readAllBytes(); }
            check(throwsIse(c::getRequestProperties), "getRequestProperties throws once the request went out");
            check(throwsIse(() -> c.setRequestProperty("X-Late", "1")), "setRequestProperty throws after");
            check(throwsIse(() -> c.addRequestProperty("X-Late", "1")), "addRequestProperty throws after");
            check("b".equals(c.getRequestProperty("x-test")), "getRequestProperty still answers after");
            check(c.getRequestProperty("User-Agent") == null, "headers the JDK added are not the app's");
            c.disconnect();
            check(throwsIse(c::getRequestProperties), "getRequestProperties throws after disconnect()");

            // An error answer, then disconnect(): the JDK's connection is "not
            // connected" again and its map holds {"GET /error HTTP/1.1"=[null], Accept,
            // User-Agent, Host, ...}. OkHttp's disconnect() only cancels: the exception.
            HttpURLConnection e = (HttpURLConnection) new URL(null, "http://127.0.0.1:" + port + "/error", http).openConnection();
            e.setRequestProperty("X-Test", "a");
            check(e.getResponseCode() == 500, "the error request answers 500");
            try {
                e.getInputStream();
            } catch (java.io.IOException expected) {
                // "Server returned HTTP response code: 500"
            }
            e.disconnect();
            check(throwsIse(e::getRequestProperties), "getRequestProperties throws after an error and disconnect()");

            HttpURLConnection s = (HttpURLConnection) new URL(null, "https://127.0.0.1:" + port + "/", https).openConnection();
            check(s instanceof HucsConnection, "https is the wrapper");
            s.setRequestProperty("Accept", "x/y");
            Map<String, List<String>> sb = s.getRequestProperties();
            check(!sb.containsKey(null) && List.of("x/y").equals(sb.get("accept")), "https: the app's fields: " + sb);
            try {
                s.connect();   // a TLS handshake with a plain HTTP server: fails
            } catch (java.io.IOException expected) {
                // OkHttp's connected is set before the attempt, so it stays set
            }
            check(throwsIse(s::getRequestProperties), "https: throws after a connect, even a failed one");
        } finally {
            server.stop(0);
        }
        if (failures != 0) {
            System.out.println(failures + " FAILED");
            System.exit(1);
        }
        System.out.println("ALL PASSED");
    }

    private static boolean throwsUoe(Map<String, List<String>> m) {
        try {
            m.put("Y", List.of("1"));
            return false;
        } catch (UnsupportedOperationException e) {
            return true;
        }
    }
}
