// android.net.wifi.WifiInfo -- snapshot of the active Wi-Fi connection.
// WifiManager fills it from the host's radio (com.macapk.loader.HostRadios)
// when the host reports one; otherwise it is the steady stub it always was.
// The MAC stays AOSP's redacted constant: apps have not been given the
// device's own MAC since Android 6.
package android.net.wifi;

public class WifiInfo implements android.net.TransportInfo {
    /** AOSP WifiInfo sentinel constants. */
    public static final int LINK_SPEED_UNKNOWN = -1;
    public static final int INVALID_RSSI = -127;
    /** AOSP returns this for getSSID() when the SSID is unavailable. */
    public static final String UNKNOWN_SSID = "<unknown ssid>";

    private final String mSsid;      // raw, unquoted; null = unknown
    private final String mBssid;
    private final int mRssi, mFrequency, mLinkSpeed, mIpAddress;
    private final SupplicantState mState;

    /** The stub: a healthy connection with no identity, for a host that does
     *  not report its radio. */
    public WifiInfo() {
        this(null, "", -60, 2412, 100, 0, SupplicantState.COMPLETED);
    }

    WifiInfo(String ssid, String bssid, int rssi, int frequency, int linkSpeed, int ipAddress,
            SupplicantState state) {
        mSsid = ssid;
        mBssid = bssid;
        mRssi = rssi;
        mFrequency = frequency;
        mLinkSpeed = linkSpeed;
        mIpAddress = ipAddress;
        mState = state;
    }

    public String getMacAddress()  { return "02:00:00:00:00:00"; }
    public String getBSSID()       { return mBssid; }
    /** AOSP wraps a UTF-8 SSID in double quotes, and answers UNKNOWN_SSID when
     *  it is withheld (no location permission) or there is no connection. */
    public String getSSID()        { return mSsid == null ? UNKNOWN_SSID : "\"" + mSsid + "\""; }
    public int    getRssi()        { return mRssi; }
    public int    getLinkSpeed()   { return mLinkSpeed; }     // Mbps
    public int    getTxLinkSpeedMbps() { return mLinkSpeed; }
    public int    getRxLinkSpeedMbps() { return mLinkSpeed; }
    /** AOSP getFrequency(): the channel frequency in MHz. */
    public int    getFrequency()   { return mFrequency; }
    public int    getNetworkId()   { return -1; }
    /** AOSP getIpAddress() (deprecated): the IPv4 address as a little-endian
     *  int, 0 if unknown. */
    public int    getIpAddress()   { return mIpAddress; }
    public boolean getHiddenSSID() { return false; }
    // Passpoint, the Wi-Fi generation and the link's ceilings: not in the host's report. AOSP's own
    // answers for "not a Passpoint network" and "unknown".
    public String getPasspointFqdn() { return null; }
    public String getPasspointProviderFriendlyName() { return null; }
    public String getPasspointUniqueId() { return null; }
    public int getWifiStandard() { return ScanResult.WIFI_STANDARD_UNKNOWN; }
    public int getMaxSupportedTxLinkSpeedMbps() { return LINK_SPEED_UNKNOWN; }
    public int getMaxSupportedRxLinkSpeedMbps() { return LINK_SPEED_UNKNOWN; }
    public int getSubscriptionId() { return -1; }
    public boolean isRestricted() { return false; }
    /** AOSP SECURITY_TYPE_* (API 31), what getCurrentSecurityType() answers. */
    public static final int SECURITY_TYPE_UNKNOWN = -1;
    public static final int SECURITY_TYPE_OPEN = 0;
    public static final int SECURITY_TYPE_WEP = 1;
    public static final int SECURITY_TYPE_PSK = 2;
    public static final int SECURITY_TYPE_EAP = 3;
    public static final int SECURITY_TYPE_SAE = 4;
    public static final int SECURITY_TYPE_EAP_WPA3_ENTERPRISE_192_BIT = 5;
    public static final int SECURITY_TYPE_OWE = 6;
    public static final int SECURITY_TYPE_WAPI_PSK = 7;
    public static final int SECURITY_TYPE_WAPI_CERT = 8;
    public static final int SECURITY_TYPE_EAP_WPA3_ENTERPRISE = 9;
    public static final int SECURITY_TYPE_OSEN = 10;
    public static final int SECURITY_TYPE_PASSPOINT_R1_R2 = 11;
    public static final int SECURITY_TYPE_PASSPOINT_R3 = 12;
    public static final int SECURITY_TYPE_DPP = 13;
    /** AOSP getCurrentSecurityType(): the security of the connection, or
     *  SECURITY_TYPE_UNKNOWN. The host's radio report (HostRadios) carries no
     *  security type. */
    public int getCurrentSecurityType() { return SECURITY_TYPE_UNKNOWN; }
    /** AOSP getSupplicantState(): COMPLETED while associated. */
    public SupplicantState getSupplicantState() { return mState; }
}
