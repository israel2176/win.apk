// android.net.nsd.NsdManager
//
// AOSP's network service-discovery (DNS-SD/mDNS) service. Apps fetch it via
// getSystemService(NSD_SERVICE) to register/discover network services on the LAN.
// On Windows' own mDNS responder (com.macapk.loader.HostNsd); see below. Mirrors
// frameworks/base/core/java/android/net/nsd/NsdManager.java surface used by apps.
//
// Type fix (F-NET audit pass): every listener callback below, and
// resolveService's own parameter, were typed `Object serviceInfo` instead of
// `NsdServiceInfo`. AOSP
// (android.googlesource.com/platform/packages/modules/Connectivity, main branch,
// framework-t/src/android/net/nsd/NsdManager.java -- NsdManager.java moved out of
// frameworks/base into this mainline module, which is why the type is not visible
// to debug/framework_lint.py's android-30 android.jar):
//   interface RegistrationListener {
//       void onRegistrationFailed(NsdServiceInfo serviceInfo, int errorCode);
//       void onUnregistrationFailed(NsdServiceInfo serviceInfo, int errorCode);
//       void onServiceRegistered(NsdServiceInfo serviceInfo);
//       void onServiceUnregistered(NsdServiceInfo serviceInfo);
//   }
//   interface DiscoveryListener {
//       ...
//       void onServiceFound(NsdServiceInfo serviceInfo);
//       void onServiceLost(NsdServiceInfo serviceInfo);
//   }
//   interface ResolveListener {
//       void onResolveFailed(NsdServiceInfo serviceInfo, int errorCode);
//       void onServiceResolved(NsdServiceInfo serviceInfo);
//   }
//   public void resolveService(NsdServiceInfo serviceInfo, ResolveListener listener)
// A guest app compiled against the real android.jar emits these exact descriptors.
// Our `Object`-typed interface methods do NOT override AOSP's -- an app class that
// correctly implements onServiceFound(NsdServiceInfo) leaves our differently-shaped
// abstract method unimplemented, so the FIRST call into it throws AbstractMethodError
// (same bug class as the v1.0.406 ConnectivityManager.NetworkCallback fix, Object ->
// LinkProperties). Worse, `resolveService(Object, ResolveListener)` is a method-
// descriptor mismatch on the ENTRY POINT itself: a guest calling
// resolveService(myServiceInfo, myListener) resolves to
// `(Landroid/net/nsd/NsdServiceInfo;Landroid/net/nsd/NsdManager$ResolveListener;)V`,
// which our old `(Ljava/lang/Object;...)V` signature does not provide at all --
// NoSuchMethodError before any callback ever fires.
package android.net.nsd;

import android.content.Context;

public final class NsdManager {

    public static final int PROTOCOL_DNS_SD = 0x0001;
    public static final int FAILURE_INTERNAL_ERROR        = 0;
    public static final int FAILURE_ALREADY_ACTIVE        = 3;
    public static final int FAILURE_MAX_LIMIT             = 4;

    /** AOSP NsdManager.RegistrationListener. */
    public interface RegistrationListener {
        void onRegistrationFailed(NsdServiceInfo serviceInfo, int errorCode);
        void onUnregistrationFailed(NsdServiceInfo serviceInfo, int errorCode);
        void onServiceRegistered(NsdServiceInfo serviceInfo);
        void onServiceUnregistered(NsdServiceInfo serviceInfo);
    }
    /** AOSP NsdManager.DiscoveryListener. */
    public interface DiscoveryListener {
        void onStartDiscoveryFailed(String serviceType, int errorCode);
        void onStopDiscoveryFailed(String serviceType, int errorCode);
        void onDiscoveryStarted(String serviceType);
        void onDiscoveryStopped(String serviceType);
        void onServiceFound(NsdServiceInfo serviceInfo);
        void onServiceLost(NsdServiceInfo serviceInfo);
    }
    /** AOSP NsdManager.ResolveListener. */
    public interface ResolveListener {
        void onResolveFailed(NsdServiceInfo serviceInfo, int errorCode);
        void onServiceResolved(NsdServiceInfo serviceInfo);
    }

    /** AOSP NsdManager.ServiceInfoCallback (API 34): a resolved service's updates. Play services
     *  implements it; R8 puts a stand-in for it into the app, which loads when no platform class does
     *  and fails at its first use. */
    public interface ServiceInfoCallback {
        void onServiceInfoCallbackRegistrationFailed(int errorCode);
        void onServiceUpdated(NsdServiceInfo serviceInfo);
        void onServiceLost();
        void onServiceInfoCallbackUnregistered();
    }

    public NsdManager() {}
    public NsdManager(Context context) {}

    // Behind this class is Windows' own mDNS responder (com.macapk.loader.HostNsd,
    // windows/host/native_nsd.cpp): discovery browses the type again every few seconds and reports
    // what appeared and what stopped answering, a resolve asks for one instance, a registration is
    // Windows' (at this computer's .local name). Every listener callback comes from this manager's own
    // threads after the call returned, as AOSP's NsdManager delivers them from its handler thread
    // (an app whose callback touches what it sets up right after the call must find it there). Where
    // Windows has no DNS-SD (before Windows 10 1809) each request fails with FAILURE_INTERNAL_ERROR,
    // AOSP's answer for a service that could not do it.

    /** What does the work: Windows' DNS-SD, or a test's stand-in (NsdManagerTest). */
    interface Backend {
        boolean available();
        String[] browse(String query, int ms);
        String[] resolve(String instance, int ms);
        long register(String instance, int port, String[] keys, String[] values, int ms, String[] registered);
        void unregister(long handle);
    }

    static Backend sBackend = new Backend() {
        @Override public boolean available() { return com.macapk.loader.HostNsd.available(); }
        @Override public String[] browse(String query, int ms) { return com.macapk.loader.HostNsd.browse(query, ms); }
        @Override public String[] resolve(String instance, int ms) {
            return com.macapk.loader.HostNsd.resolve(instance, ms);
        }
        @Override public long register(String instance, int port, String[] keys, String[] values, int ms,
                String[] registered) {
            return com.macapk.loader.HostNsd.register(instance, port, keys, values, ms, registered);
        }
        @Override public void unregister(long handle) { com.macapk.loader.HostNsd.unregister(handle); }
    };

    /** How long one browse listens, the pause between browses, and how many browses in a row a found
     *  service may miss before it is lost (a responder can miss one window). */
    static int sBrowseMs = 3000, sPauseMs = 2000, sMissesToLose = 2;
    private static final int RESOLVE_MS = 5000, REGISTER_MS = 5000;

    private static android.os.Handler sHandler;

    private static synchronized android.os.Handler handler() {
        if (sHandler == null) {
            android.os.HandlerThread t = new android.os.HandlerThread("NsdManager");
            t.setDaemon(true);  // never holds the process up at exit
            t.start();
            sHandler = new android.os.Handler(t.getLooper());
        }
        return sHandler;
    }

    /** Work that blocks (a browse, a resolve, a registration) runs here, its callbacks then posted to the
     *  manager's thread. */
    private static final java.util.concurrent.ExecutorService sWork =
            java.util.concurrent.Executors.newCachedThreadPool(r -> {
                Thread t = new Thread(r, "NsdManager-work");
                t.setDaemon(true);
                return t;
            });

    private static void checkProtocol(int protocolType) {
        // AOSP: checkProtocol.
        if (protocolType != PROTOCOL_DNS_SD) throw new IllegalArgumentException("Unsupported protocol");
    }

    private static void checkListener(Object listener) {
        // AOSP: checkListener.
        if (listener == null) throw new IllegalArgumentException("listener cannot be null");
    }

    /** "_http._tcp", "_http._tcp." or "_http._tcp.local." -> "_http._tcp". */
    static String bareType(String type) {
        String t = type == null ? "" : type.trim();
        while (t.endsWith(".")) t = t.substring(0, t.length() - 1);
        if (t.endsWith(".local")) t = t.substring(0, t.length() - ".local".length());
        while (t.endsWith(".")) t = t.substring(0, t.length() - 1);
        return t;
    }

    /** What AOSP's NsdService hands an app for a found service's type: the bare type and a final dot. */
    static String foundType(String bare) { return bare + "."; }

    /** The instance's name from its full name ("My TV._http._tcp.local"), else null when it is not of
     *  the type. Instance names may hold dots, so the type is taken off the end. */
    static String instanceName(String full, String bare) {
        String f = full;
        while (f.endsWith(".")) f = f.substring(0, f.length() - 1);
        String suffix = "." + bare + ".local";
        if (f.endsWith(suffix)) return f.substring(0, f.length() - suffix.length());
        suffix = "." + bare;
        if (f.endsWith(suffix)) return f.substring(0, f.length() - suffix.length());
        return null;
    }

    /** A registered service. */
    private static final class Registered {
        final long handle;
        final NsdServiceInfo info;

        Registered(long handle, NsdServiceInfo info) {
            this.handle = handle;
            this.info = info;
        }
    }

    /** A running discovery. */
    private static final class Discovery {
        final String type, bare;
        final DiscoveryListener listener;
        volatile boolean stopped;
        final java.util.Map<String, Integer> misses = new java.util.HashMap<>();

        Discovery(String type, DiscoveryListener listener) {
            this.type = type;
            this.bare = bareType(type);
            this.listener = listener;
        }
    }

    private static final java.util.Map<Object, Object> sActive =
            java.util.Collections.synchronizedMap(new java.util.IdentityHashMap<>());

    /** AOSP: a listener already in use throws (NsdManager.putListener). */
    private static void claim(Object listener, Object what) {
        synchronized (sActive) {
            if (sActive.containsKey(listener)) throw new IllegalArgumentException("listener already in use");
            sActive.put(listener, what);
        }
    }

    public void registerService(final NsdServiceInfo serviceInfo, int protocolType,
            final RegistrationListener listener) {
        checkListener(listener);
        checkProtocol(protocolType);
        if (serviceInfo == null || android.text.TextUtils.isEmpty(serviceInfo.getServiceName())
                || android.text.TextUtils.isEmpty(serviceInfo.getServiceType())) {
            throw new IllegalArgumentException("Service name or type cannot be empty");
        }
        if (serviceInfo.getPort() <= 0) throw new IllegalArgumentException("Invalid port number");
        claim(listener, serviceInfo);
        sWork.execute(() -> {
            java.util.Map<String, byte[]> attrs = serviceInfo.getAttributes();
            String[] keys = new String[attrs.size()], values = new String[attrs.size()];
            int i = 0;
            for (java.util.Map.Entry<String, byte[]> e : attrs.entrySet()) {
                keys[i] = e.getKey();
                values[i++] = e.getValue() != null ? new String(e.getValue(), java.nio.charset.StandardCharsets.UTF_8) : "";
            }
            String bare = bareType(serviceInfo.getServiceType());
            String[] registered = new String[1];
            long handle = sBackend.available() ? sBackend.register(serviceInfo.getServiceName() + "." + bare + ".local",
                    serviceInfo.getPort(), keys, values, REGISTER_MS, registered) : 0;
            handler().post(() -> {
                if (handle == 0) {
                    sActive.remove(listener);
                    listener.onRegistrationFailed(serviceInfo, FAILURE_INTERNAL_ERROR);
                    return;
                }
                sActive.put(listener, new Registered(handle, serviceInfo));
                // AOSP hands back the name the responder took (renamed on a conflict).
                if (registered[0] != null) {
                    String name = instanceName(registered[0], bare);
                    if (name != null) serviceInfo.setServiceName(name);
                }
                listener.onServiceRegistered(serviceInfo);
            });
        });
    }

    /** AOSP throws for a listener it does not know; an app that unregisters in its teardown whatever
     *  happened before must not die of that here, so an unknown one is ignored. */
    public void unregisterService(RegistrationListener listener) {
        checkListener(listener);
        Object what = sActive.remove(listener);
        if (!(what instanceof Registered)) return;
        Registered r = (Registered) what;
        sWork.execute(() -> {
            sBackend.unregister(r.handle);
            handler().post(() -> listener.onServiceUnregistered(r.info));
        });
    }

    public void discoverServices(final String serviceType, int protocolType, final DiscoveryListener listener) {
        checkListener(listener);
        checkProtocol(protocolType);
        if (android.text.TextUtils.isEmpty(serviceType)) throw new IllegalArgumentException("Service type cannot be empty");
        final Discovery d = new Discovery(serviceType, listener);
        claim(listener, d);
        if (!sBackend.available()) {
            handler().post(() -> {
                sActive.remove(listener);
                listener.onStartDiscoveryFailed(serviceType, FAILURE_INTERNAL_ERROR);
            });
            return;
        }
        handler().post(() -> listener.onDiscoveryStarted(serviceType));
        sWork.execute(() -> {
            while (!d.stopped) {
                String[] names = sBackend.browse(d.bare + ".local", sBrowseMs);
                if (d.stopped) break;
                java.util.Set<String> seen = new java.util.LinkedHashSet<>();
                if (names != null) {
                    for (String full : names) {
                        String name = instanceName(full, d.bare);
                        if (name != null) seen.add(name);
                    }
                }
                java.util.List<String> found = new java.util.ArrayList<>(), lost = new java.util.ArrayList<>();
                for (String name : seen) {
                    if (d.misses.put(name, 0) == null) found.add(name);
                }
                for (java.util.Iterator<java.util.Map.Entry<String, Integer>> it = d.misses.entrySet().iterator();
                        it.hasNext();) {
                    java.util.Map.Entry<String, Integer> e = it.next();
                    if (seen.contains(e.getKey())) continue;
                    e.setValue(e.getValue() + 1);
                    if (e.getValue() >= sMissesToLose) {
                        lost.add(e.getKey());
                        it.remove();
                    }
                }
                handler().post(() -> {
                    if (d.stopped) return;
                    for (String name : found) listener.onServiceFound(info(name, d.bare));
                    for (String name : lost) listener.onServiceLost(info(name, d.bare));
                });
                try {
                    Thread.sleep(sPauseMs);
                } catch (InterruptedException e) {
                    break;
                }
            }
        });
    }

    private static NsdServiceInfo info(String name, String bare) {
        NsdServiceInfo i = new NsdServiceInfo();
        i.setServiceName(name);
        i.setServiceType(foundType(bare));
        return i;
    }

    /** As unregisterService, an unknown listener is ignored. */
    public void stopServiceDiscovery(DiscoveryListener listener) {
        checkListener(listener);
        Object what = sActive.remove(listener);
        if (!(what instanceof Discovery)) return;
        Discovery d = (Discovery) what;
        d.stopped = true;
        handler().post(() -> listener.onDiscoveryStopped(d.type));
    }

    /** AOSP registerServiceInfoCallback (API 34): the service resolved, its updates delivered on the
     *  caller's executor; here the one resolve's answer (no further updates are watched). */
    public void registerServiceInfoCallback(NsdServiceInfo serviceInfo,
            java.util.concurrent.Executor executor, final ServiceInfoCallback listener) {
        if (executor == null) throw new NullPointerException("executor");
        checkListener(listener);
        claim(listener, serviceInfo);
        sWork.execute(() -> {
            NsdServiceInfo got = resolveNow(serviceInfo);
            handler().post(() -> executor.execute(() -> {
                if (got == null) {
                    sActive.remove(listener);
                    listener.onServiceInfoCallbackRegistrationFailed(FAILURE_INTERNAL_ERROR);
                } else {
                    listener.onServiceUpdated(got);
                }
            }));
        });
    }

    public void unregisterServiceInfoCallback(ServiceInfoCallback listener) {
        checkListener(listener);
        if (sActive.remove(listener) == null) return;
        handler().post(listener::onServiceInfoCallbackUnregistered);
    }

    public void resolveService(final NsdServiceInfo serviceInfo, final ResolveListener listener) {
        checkListener(listener);
        if (serviceInfo == null || android.text.TextUtils.isEmpty(serviceInfo.getServiceName())
                || android.text.TextUtils.isEmpty(serviceInfo.getServiceType())) {
            throw new IllegalArgumentException("Service name or type cannot be empty");
        }
        claim(listener, serviceInfo);
        sWork.execute(() -> {
            NsdServiceInfo got = resolveNow(serviceInfo);
            handler().post(() -> {
                sActive.remove(listener);
                if (got == null) listener.onResolveFailed(serviceInfo, FAILURE_INTERNAL_ERROR);
                else listener.onServiceResolved(got);
            });
        });
    }

    /** One resolve: the service's host address (IPv4 first, as AOSP's legacy getHost), port and TXT
     *  record, or null. */
    private static NsdServiceInfo resolveNow(NsdServiceInfo asked) {
        if (!sBackend.available()) return null;
        String bare = bareType(asked.getServiceType());
        String[] r = sBackend.resolve(asked.getServiceName() + "." + bare + ".local", RESOLVE_MS);
        if (r == null || r.length < 4) return null;
        NsdServiceInfo out = new NsdServiceInfo();
        out.setServiceName(asked.getServiceName());
        out.setServiceType(foundType(bare));
        try {
            out.setPort(Integer.parseInt(r[1]));
        } catch (NumberFormatException e) {
            return null;
        }
        String address = !r[2].isEmpty() ? r[2] : r[3];
        try {
            out.setHost(!address.isEmpty() ? java.net.InetAddress.getByName(address)
                    : java.net.InetAddress.getByName(r[0]));
        } catch (java.net.UnknownHostException e) {
            return null;
        }
        for (int i = 4; i + 1 < r.length; i += 2) out.setAttribute(r[i], r[i + 1]);
        return out;
    }
}
