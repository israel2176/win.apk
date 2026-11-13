// android.net.wifi.WifiManager -- Wi-Fi service. The state, the connection and
// the scan results come from the host's radio when the host reports one
// (com.macapk.loader.HostRadios); controls that would change the host's Wi-Fi
// (enable, connect, add networks) answer "not possible".
package android.net.wifi;

import android.net.DhcpInfo;
import android.os.WorkSource;
import java.util.Collections;
import java.util.List;

public class WifiManager {
    public static final int WIFI_STATE_DISABLING = 0;
    public static final int WIFI_STATE_DISABLED  = 1;
    public static final int WIFI_STATE_ENABLING  = 2;
    public static final int WIFI_STATE_ENABLED   = 3;
    public static final int WIFI_STATE_UNKNOWN   = 4;

    // Action constants -- F-Droid's WifiStateChangeService and similar
    // listeners register IntentFilters with these.
    public static final String WIFI_STATE_CHANGED_ACTION = "android.net.wifi.WIFI_STATE_CHANGED";
    public static final String NETWORK_STATE_CHANGED_ACTION = "android.net.wifi.STATE_CHANGE";
    public static final String SCAN_RESULTS_AVAILABLE_ACTION = "android.net.wifi.SCAN_RESULTS";
    public static final String NETWORK_IDS_CHANGED_ACTION = "android.net.wifi.NETWORK_IDS_CHANGED";
    public static final String SUPPLICANT_STATE_CHANGED_ACTION = "android.net.wifi.supplicant.STATE_CHANGE";
    public static final String SUPPLICANT_CONNECTION_CHANGE_ACTION = "android.net.wifi.supplicant.CONNECTION_CHANGE";
    public static final String CONFIGURED_NETWORKS_CHANGED_ACTION = "android.net.wifi.CONFIGURED_NETWORKS_CHANGE";
    public static final String RSSI_CHANGED_ACTION = "android.net.wifi.RSSI_CHANGED";

    public static final String EXTRA_WIFI_STATE = "wifi_state";
    public static final String EXTRA_PREVIOUS_WIFI_STATE = "previous_wifi_state";
    public static final String EXTRA_NEW_STATE = "newState";
    public static final String EXTRA_NETWORK_INFO = "networkInfo";
    public static final String EXTRA_BSSID = "bssid";
    public static final String EXTRA_WIFI_INFO = "wifiInfo";
    public static final String EXTRA_NEW_RSSI = "newRssi";
    public static final String EXTRA_SUPPLICANT_CONNECTED = "connected";

    public static final int WIFI_MODE_FULL = 1;
    public static final int WIFI_MODE_SCAN_ONLY = 2;
    public static final int WIFI_MODE_FULL_HIGH_PERF = 3;
    public static final int WIFI_MODE_FULL_LOW_LATENCY = 4;

    private final WifiInfo info = new WifiInfo();
    private final DhcpInfo dhcp = new DhcpInfo();

    public WifiManager() { startEvents(); }

    // ---- the host's events (HostRadios.wifiEvent) -> the broadcasts ----------
    //
    // One thread for the process, started with the first WifiManager, turns
    // the host's Wi-Fi notifications into WIFI_STATE_CHANGED_ACTION,
    // NETWORK_STATE_CHANGED_ACTION (with EXTRA_NETWORK_INFO), RSSI_CHANGED_ACTION
    // and SCAN_RESULTS_AVAILABLE_ACTION, as WifiStateMachine sends them. A host
    // that reports no events (wifiEvent() < 0) gets no thread and the old
    // answers (startScan's timer).

    private static final Object sEventsLock = new Object();
    private static boolean sEventsStarted;
    private static boolean sEventsReported;  // the host answers wifiEvent

    private static void startEvents() {
        synchronized (sEventsLock) {
            if (sEventsStarted) return;
            sEventsStarted = true;
            int first = com.macapk.loader.HostRadios.wifiEvent(0);
            if (first < 0) return;
            sEventsReported = true;
        }
        Thread t = new Thread(WifiManager::pumpEvents, "wifi-events");
        t.setDaemon(true);
        t.start();
    }

    private static boolean eventsReported() {
        synchronized (sEventsLock) { return sEventsReported; }
    }

    private static void pumpEvents() {
        int lastState = -1;
        long lastRssiAt = 0;
        for (;;) {
            int e = com.macapk.loader.HostRadios.wifiEvent(10000);
            if (e < 0) return;
            if (e == 0) continue;
            android.content.Context ctx = android.app.ActivityThread.currentApplication();
            if (ctx == null) continue;
            try {
                WifiManager wm = (WifiManager) ctx.getSystemService(android.content.Context.WIFI_SERVICE);
                if (wm == null) wm = new WifiManager();
                switch (e) {
                case com.macapk.loader.HostRadios.WIFI_EVENT_STATE: {
                    int state = wm.getWifiState();
                    android.content.Intent i = new android.content.Intent(WIFI_STATE_CHANGED_ACTION);
                    i.putExtra(EXTRA_WIFI_STATE, state);
                    i.putExtra(EXTRA_PREVIOUS_WIFI_STATE, lastState < 0 ? WIFI_STATE_UNKNOWN : lastState);
                    lastState = state;
                    ctx.sendBroadcast(i);
                    break;
                }
                case com.macapk.loader.HostRadios.WIFI_EVENT_CONNECTED:
                case com.macapk.loader.HostRadios.WIFI_EVENT_DISCONNECTED: {
                    boolean up = e == com.macapk.loader.HostRadios.WIFI_EVENT_CONNECTED;
                    android.content.Intent i = new android.content.Intent(NETWORK_STATE_CHANGED_ACTION);
                    i.putExtra(EXTRA_NETWORK_INFO, new android.net.NetworkInfo(
                            android.net.ConnectivityManager.TYPE_WIFI,
                            up ? android.net.NetworkInfo.State.CONNECTED : android.net.NetworkInfo.State.DISCONNECTED));
                    if (up) i.putExtra(EXTRA_BSSID, wm.getConnectionInfo().getBSSID());
                    ctx.sendBroadcast(i);
                    android.content.Intent s = new android.content.Intent(SUPPLICANT_CONNECTION_CHANGE_ACTION);
                    s.putExtra(EXTRA_SUPPLICANT_CONNECTED, up);
                    ctx.sendBroadcast(s);
                    break;
                }
                case com.macapk.loader.HostRadios.WIFI_EVENT_SIGNAL: {
                    // WifiStateMachine polls the RSSI every three seconds
                    // and reports changes; the host reports every change.
                    long now = android.os.SystemClock.elapsedRealtime();
                    if (now - lastRssiAt < 3000) break;
                    lastRssiAt = now;
                    android.content.Intent i = new android.content.Intent(RSSI_CHANGED_ACTION);
                    i.putExtra(EXTRA_NEW_RSSI, wm.getConnectionInfo().getRssi());
                    ctx.sendBroadcast(i);
                    break;
                }
                case com.macapk.loader.HostRadios.WIFI_EVENT_SCAN: {
                    android.content.Intent i = new android.content.Intent(SCAN_RESULTS_AVAILABLE_ACTION);
                    i.putExtra(EXTRA_RESULTS_UPDATED, true);
                    ctx.sendBroadcast(i);
                    break;
                }
                default:
                    break;
                }
            } catch (Throwable t) {
                android.util.Log.w("WifiManager", "wifi event " + e + " not delivered", t);
            }
        }
    }

    // ---- the host's radio (com.macapk.loader.HostRadios) -------------------
    //
    // A host that reports its Wi-Fi (Win.apk) answers with the real state,
    // connection and scan results; one that does not (HostRadios.wifiState()
    // < 0) keeps the stub answers: "on", a nameless connection, no networks.

    public boolean isWifiEnabled() {
        int s = com.macapk.loader.HostRadios.wifiState();
        return s < 0 || s == WIFI_STATE_ENABLED;
    }

    public int getWifiState() {
        int s = com.macapk.loader.HostRadios.wifiState();
        return s < 0 ? WIFI_STATE_ENABLED : s;
    }

    public boolean setWifiEnabled(boolean on) { return false; }

    public WifiInfo getConnectionInfo() {
        String[] c = com.macapk.loader.HostRadios.wifiConnection();
        if (c == null) {
            if (com.macapk.loader.HostRadios.wifiState() < 0) return info;
            return new WifiInfo(null, "02:00:00:00:00:00", WifiInfo.INVALID_RSSI, -1,
                    WifiInfo.LINK_SPEED_UNKNOWN, 0, SupplicantState.DISCONNECTED);
        }
        String ssid = c[0].isEmpty() ? null : c[0];
        String bssid = c[1].isEmpty() ? "02:00:00:00:00:00" : c[1];
        return new WifiInfo(ssid, bssid, parse(c[2], WifiInfo.INVALID_RSSI), parse(c[3], -1),
                parse(c[4], WifiInfo.LINK_SPEED_UNKNOWN), hostIpv4(), SupplicantState.COMPLETED);
    }

    private static int parse(String s, int fallback) {
        try { return s.isEmpty() ? fallback : Integer.parseInt(s); }
        catch (NumberFormatException e) { return fallback; }
    }

    /** The host's IPv4 address on its default route, as AOSP's little-endian
     *  int. Connecting a UDP socket sends nothing; it only picks the route. */
    private static int hostIpv4() {
        try (java.net.DatagramSocket s = new java.net.DatagramSocket()) {
            s.connect(java.net.InetAddress.getByName("192.0.2.1"), 9);
            byte[] a = s.getLocalAddress().getAddress();
            if (a.length != 4) return 0;
            return (a[0] & 0xff) | (a[1] & 0xff) << 8 | (a[2] & 0xff) << 16 | (a[3] & 0xff) << 24;
        } catch (Throwable t) {
            return 0;
        }
    }

    /** AOSP: the connection's DHCP-supplied configuration. The host's
     *  interface configuration when it reports one (HostRadios.wifiDhcp:
     *  Windows does); all zero otherwise (the Mac, or not connected). */
    public DhcpInfo getDhcpInfo() {
        int[] d = com.macapk.loader.HostRadios.wifiDhcp();
        if (d == null || d.length < 7) return dhcp;
        DhcpInfo out = new DhcpInfo();
        out.ipAddress = d[0];
        out.gateway = d[1];
        out.netmask = d[2];
        out.dns1 = d[3];
        out.dns2 = d[4];
        out.serverAddress = d[5];
        out.leaseDuration = d[6];
        return out;
    }

    public List<ScanResult> getScanResults() {
        String[] r = com.macapk.loader.HostRadios.wifiScanResults();
        if (r == null) return Collections.emptyList();
        List<ScanResult> out = new java.util.ArrayList<>();
        long now = android.os.SystemClock.elapsedRealtime() * 1000L;
        for (int i = 0; i + 4 < r.length; i += 5) {
            ScanResult s = new ScanResult();
            s.SSID = r[i];
            s.BSSID = r[i + 1];
            s.capabilities = r[i + 2];
            s.level = parse(r[i + 3], -100);
            s.frequency = parse(r[i + 4], 0);
            s.timestamp = now;
            out.add(s);
        }
        return out;
    }

    /** AOSP: the results arrive later, announced by SCAN_RESULTS_AVAILABLE_ACTION
     *  (EXTRA_RESULTS_UPDATED): when the host says the scan finished
     *  (pumpEvents), or, on a host that reports no events, after the four
     *  seconds Windows takes. */
    public boolean startScan() {
        if (!com.macapk.loader.HostRadios.wifiStartScan()) return false;
        if (eventsReported()) return true;
        Thread t = new Thread(() -> {
            try {
                Thread.sleep(4000L);
                android.content.Context ctx = android.app.ActivityThread.currentApplication();
                if (ctx == null) return;
                android.content.Intent i = new android.content.Intent(SCAN_RESULTS_AVAILABLE_ACTION);
                i.putExtra(EXTRA_RESULTS_UPDATED, true);
                ctx.sendBroadcast(i);
            } catch (Throwable ignored) {
            }
        }, "wifi-scan-results");
        t.setDaemon(true);
        t.start();
        return true;
    }

    public static final String EXTRA_RESULTS_UPDATED = "resultsUpdated";
    public boolean  reconnect()               { return false; }
    public boolean  reassociate()             { return false; }
    public boolean  disconnect()              { return false; }
    public boolean  pingSupplicant()          { return false; }
    public boolean  saveConfiguration()       { return false; }
    public List<?>  getConfiguredNetworks()   { return Collections.emptyList(); }
    // AOSP: addNetwork(WifiConfiguration)/updateNetwork(WifiConfiguration) --
    // verified against android-emulator/android-sdk/platforms/android-30/android.jar
    // (`javap -p android.net.wifi.WifiManager`): both take the concrete
    // WifiConfiguration type, not Object. A guest javac-compiles a call site as
    // invokevirtual .../WifiManager.addNetwork(Landroid/net/wifi/WifiConfiguration;)I;
    // our previous `(Ljava/lang/Object;)I` descriptor does not match that call site
    // at all, so it threw NoSuchMethodError before this method could ever run (same
    // descriptor-mismatch bug class as the v1.0.406 NetworkRequest/ConnectivityManager
    // fixes). AOSP returns -1 ("network could not be added") on failure, matching the
    // "no supplicant here" answer this class already gives every other Wi-Fi control.
    public int      addNetwork(WifiConfiguration config) { return -1; }
    public int      updateNetwork(WifiConfiguration config) { return -1; }
    public boolean  removeNetwork(int id)     { return false; }
    public boolean  enableNetwork(int id, boolean disableOthers) { return false; }
    public boolean  disableNetwork(int id)    { return false; }
    public boolean  is5GHzBandSupported()     { return true; }
    public boolean  isP2pSupported()          { return false; }
    public boolean  isScanAlwaysAvailable()   { return false; }

    public WifiLock createWifiLock(int lockType, String tag) { return new WifiLock(); }
    public WifiLock createWifiLock(String tag)               { return new WifiLock(); }

    public MulticastLock createMulticastLock(String tag) { return new MulticastLock(); }

    public static class WifiLock {
        public void acquire() {}
        public void release() {}
        public boolean isHeld() { return false; }
        public void setReferenceCounted(boolean on) {}
        // AOSP: setWorkSource(android.os.WorkSource) -- verified against
        // android-emulator/android-sdk/platforms/android-30/android.jar
        // (`javap -p android.net.wifi.WifiManager$WifiLock`). Same
        // descriptor-mismatch bug class as addNetwork/updateNetwork above: a
        // guest's invokevirtual site carries the WorkSource descriptor and
        // would NoSuchMethodError against the old Object-typed overload.
        public void setWorkSource(WorkSource ws) {}
    }

    public static class MulticastLock {
        public void acquire() {}
        public void release() {}
        public boolean isHeld() { return false; }
        public void setReferenceCounted(boolean on) {}
    }

    /** AOSP calculateSignalLevel(int) (API 30): the level on the device's own
     *  scale, 0..getMaxSignalLevel(), which is AOSP's default of 4. */
    public int calculateSignalLevel(int rssi) { return calculateSignalLevel(rssi, 5); }

    /** AOSP isWifiApEnabled(), a system API: whether this device is running a
     *  hotspot. An app cannot start one here, and does not see the host's. */
    public boolean isWifiApEnabled() { return false; }

    /** AOSP isWpa3SaeSupported(): the host's radio report does not say, and the
     *  networks an app could ask to join with it are refused anyway
     *  (addNetwork), so the claim an app can rely on is "no". */
    public boolean isWpa3SaeSupported() { return false; }

    /** AOSP isStaConcurrencyForLocalOnlyConnectionsSupported() (API 31): a
     *  second station connection beside the main one. One connection, the
     *  host's. */
    public boolean isStaConcurrencyForLocalOnlyConnectionsSupported() { return false; }

    // What the radio can do, asked by connectivity and analytics libraries before anything else.
    // The host's radio report (native_radios.cpp) says none of it, and an app can make no use of
    // any of it here (one connection, the host's): "no", as for the two above. They were missing,
    // a NoSuchMethodError in the nine of the popular apps that ask.
    public boolean isTdlsSupported() { return false; }
    public boolean isPreferredNetworkOffloadSupported() { return false; }
    public boolean isEnhancedPowerReportingSupported() { return false; }
    public boolean isDeviceToApRttSupported() { return false; }
    public boolean is24GHzBandSupported() { return true; }
    public boolean is6GHzBandSupported() { return false; }
    public boolean is60GHzBandSupported() { return false; }
    public boolean isWpa3SuiteBSupported() { return false; }
    public boolean isEnhancedOpenSupported() { return false; }
    public boolean isEasyConnectSupported() { return false; }
    public boolean isWapiSupported() { return false; }
    public boolean isPasspointTermsAndConditionsSupported() { return false; }
    public boolean isWifiDisplayR2Supported() { return false; }
    public boolean isDecoratedIdentitySupported() { return false; }
    public boolean isWpaPersonalSupported() { return true; }
    public boolean isWepSupported() { return false; }
    public boolean isTlsV13Supported() { return false; }
    public boolean isTlsMinimumVersionSupported() { return false; }
    public boolean isTidToLinkMappingNegotiationSupported() { return false; }
    public boolean isDualBandSimultaneousSupported() { return false; }
    public boolean isD2dSupportedWhenInfraStaDisabled() { return false; }
    public boolean isAggressiveRoamingModeSupported() { return false; }
    public boolean isMakeBeforeBreakWifiSwitchingSupported() { return false; }
    public boolean isStaApConcurrencySupported() { return false; }
    public boolean isStaBridgedApConcurrencySupported() { return false; }
    public boolean isBridgedApConcurrencySupported() { return false; }
    public boolean isStaConcurrencyForMultiInternetSupported() { return false; }
    public boolean isScanThrottleEnabled() { return true; }
    public boolean isAutoWakeupEnabled() { return false; }
    /** AOSP getMaxNumberOfChannelsPerNetworkSpecifierRequest() (API 34): 0 where the feature is absent. */
    public int getMaxNumberOfChannelsPerNetworkSpecifierRequest() { return 0; }

    public static int calculateSignalLevel(int rssi, int numLevels) {
        if (rssi <= -100) return 0;
        if (rssi >= -55) return numLevels - 1;
        float inputRange = (-55) - (-100);
        float outputRange = numLevels - 1;
        return (int) ((rssi - (-100)) * outputRange / inputRange);
    }
}

