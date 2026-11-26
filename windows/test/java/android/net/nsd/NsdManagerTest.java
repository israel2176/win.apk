// NsdManagerTest -- NsdManager over a stand-in for Windows' DNS-SD (NsdManager.Backend), on a plain JVM:
// a discovery reports what appears and, after it misses its browses, what is gone; a resolve gives host,
// port and TXT record; a registration takes the name the responder gave; a listener in use is refused, as
// AOSP refuses it; without DNS-SD every request fails with FAILURE_INTERNAL_ERROR.
package android.net.nsd;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public class NsdManagerTest {
    static int failures;
    static final List<String> events = Collections.synchronizedList(new ArrayList<>());

    static void check(String what, Object got, Object want) {
        boolean ok = want.equals(got);
        System.out.println((ok ? "  ok   " : "  FAIL ") + what + ": " + got + (ok ? "" : " (want " + want + ")"));
        if (!ok) failures++;
    }

    /** The network: what each browse answers, in turn (the last repeats). */
    static final List<String[]> browses = new ArrayList<>();
    static int browseCount;
    static boolean available = true;

    public static void main(String[] args) throws Exception {
        NsdManager.sBrowseMs = 1;
        NsdManager.sPauseMs = 20;
        NsdManager.sMissesToLose = 2;
        NsdManager.sBackend = new NsdManager.Backend() {
            @Override public boolean available() { return available; }
            @Override public synchronized String[] browse(String query, int ms) {
                events.add("browse " + query);
                return browses.get(Math.min(browseCount++, browses.size() - 1));
            }
            @Override public String[] resolve(String instance, int ms) {
                return instance.equals("My TV._googlecast._tcp.local")
                        ? new String[] {"tv.local", "8009", "192.168.1.20", "", "fn", "Living room"} : null;
            }
            @Override public long register(String instance, int port, String[] keys, String[] values, int ms,
                    String[] registered) {
                registered[0] = "Game (2)._game._udp.local";
                return 7;
            }
            @Override public void unregister(long handle) { events.add("unregister " + handle); }
        };
        NsdManager m = new NsdManager();

        // Discovery: TV appears, then a speaker; TV misses two browses and is lost.
        browses.add(new String[] {"My TV._googlecast._tcp.local"});
        browses.add(new String[] {"My TV._googlecast._tcp.local", "Speaker._googlecast._tcp.local"});
        browses.add(new String[] {"Speaker._googlecast._tcp.local"});
        browses.add(new String[] {"Speaker._googlecast._tcp.local"});
        CountDownLatch lost = new CountDownLatch(1);
        NsdManager.DiscoveryListener d = new NsdManager.DiscoveryListener() {
            @Override public void onStartDiscoveryFailed(String t, int e) { events.add("start failed"); }
            @Override public void onStopDiscoveryFailed(String t, int e) {}
            @Override public void onDiscoveryStarted(String t) { events.add("started " + t); }
            @Override public void onDiscoveryStopped(String t) { events.add("stopped " + t); }
            @Override public void onServiceFound(NsdServiceInfo i) {
                events.add("found " + i.getServiceName() + " " + i.getServiceType());
            }
            @Override public void onServiceLost(NsdServiceInfo i) {
                events.add("lost " + i.getServiceName());
                lost.countDown();
            }
        };
        m.discoverServices("_googlecast._tcp", NsdManager.PROTOCOL_DNS_SD, d);
        boolean threw = false;
        try {
            m.discoverServices("_googlecast._tcp", NsdManager.PROTOCOL_DNS_SD, d);
        } catch (IllegalArgumentException e) {
            threw = true;
        }
        check("a listener in use is refused", threw, true);
        check("TV lost", lost.await(5, TimeUnit.SECONDS), true);
        m.stopServiceDiscovery(d);
        Thread.sleep(200);
        List<String> got = new ArrayList<>();
        synchronized (events) {
            for (String e : events) if (!e.startsWith("browse")) got.add(e);
        }
        check("discovery", got, List.of("started _googlecast._tcp", "found My TV _googlecast._tcp.",
                "found Speaker _googlecast._tcp.", "lost My TV", "stopped _googlecast._tcp"));
        String firstBrowse = null;
        synchronized (events) {
            for (String e : events) if (e.startsWith("browse")) { firstBrowse = e; break; }
        }
        check("browsed the .local type", firstBrowse, "browse _googlecast._tcp.local");

        // Resolve.
        CountDownLatch resolved = new CountDownLatch(1);
        NsdServiceInfo tv = new NsdServiceInfo();
        tv.setServiceName("My TV");
        tv.setServiceType("_googlecast._tcp.");
        final NsdServiceInfo[] out = new NsdServiceInfo[1];
        m.resolveService(tv, new NsdManager.ResolveListener() {
            @Override public void onResolveFailed(NsdServiceInfo i, int e) { resolved.countDown(); }
            @Override public void onServiceResolved(NsdServiceInfo i) { out[0] = i; resolved.countDown(); }
        });
        resolved.await(5, TimeUnit.SECONDS);
        check("resolved host", out[0] != null ? out[0].getHost().getHostAddress() : null, "192.168.1.20");
        check("resolved port", out[0] != null ? out[0].getPort() : null, 8009);
        check("resolved TXT", out[0] != null ? new String(out[0].getAttributes().get("fn")) : null, "Living room");

        // Register, renamed by the responder; then unregister.
        CountDownLatch registered = new CountDownLatch(2);
        NsdServiceInfo game = new NsdServiceInfo();
        game.setServiceName("Game");
        game.setServiceType("_game._udp");
        game.setPort(4000);
        final String[] name = new String[1];
        NsdManager.RegistrationListener r = new NsdManager.RegistrationListener() {
            @Override public void onRegistrationFailed(NsdServiceInfo i, int e) { registered.countDown(); }
            @Override public void onUnregistrationFailed(NsdServiceInfo i, int e) {}
            @Override public void onServiceRegistered(NsdServiceInfo i) { name[0] = i.getServiceName(); registered.countDown(); }
            @Override public void onServiceUnregistered(NsdServiceInfo i) { registered.countDown(); }
        };
        m.registerService(game, NsdManager.PROTOCOL_DNS_SD, r);
        Thread.sleep(300);
        m.unregisterService(r);
        registered.await(5, TimeUnit.SECONDS);
        check("registered as the responder named it", name[0], "Game (2)");
        check("unregistered", events.contains("unregister 7"), true);

        // No DNS-SD: discovery fails as AOSP's does for a service that cannot.
        available = false;
        CountDownLatch failed = new CountDownLatch(1);
        m.discoverServices("_http._tcp", NsdManager.PROTOCOL_DNS_SD, new NsdManager.DiscoveryListener() {
            @Override public void onStartDiscoveryFailed(String t, int e) { failed.countDown(); }
            @Override public void onStopDiscoveryFailed(String t, int e) {}
            @Override public void onDiscoveryStarted(String t) {}
            @Override public void onDiscoveryStopped(String t) {}
            @Override public void onServiceFound(NsdServiceInfo i) {}
            @Override public void onServiceLost(NsdServiceInfo i) {}
        });
        check("no DNS-SD: start failed", failed.await(5, TimeUnit.SECONDS), true);

        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
