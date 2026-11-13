// android.net.NetworkInfo -- the "current network" snapshot returned
// by ConnectivityManager, and the EXTRA_NETWORK_INFO of WifiManager's
// NETWORK_STATE_CHANGED_ACTION, which is why it is Parcelable as on AOSP.
package android.net;

public class NetworkInfo implements android.os.Parcelable {
    // AOSP's six, in AOSP's order: SUSPENDED was missing, a NoSuchFieldError in the switch tables of
    // the apps that name it (Telegram among them), and ordinal() differed from a device's.
    public enum State { CONNECTING, CONNECTED, SUSPENDED, DISCONNECTING, DISCONNECTED, UNKNOWN }

    private final int type;
    private final State state;

    public NetworkInfo() { this(ConnectivityManager.TYPE_WIFI, State.CONNECTED); }
    public NetworkInfo(int type, State state) {
        this.type = type;
        this.state = state;
    }

    public int    getType()        { return type; }
    // getSubtype(): for a mobile network this is the TelephonyManager
    // NETWORK_TYPE_* value; for non-mobile (our claimed WIFI) AOSP returns 0
    // (NETWORK_TYPE_UNKNOWN). Firebase's datatransport CctTransportBackend
    // (getNetSubtypeValue) calls this on every telemetry upload -- a missing
    // method threw NoSuchMethodError on a background thread and Crashlytics
    // (ReportUncaughtExceptionsAsFatal) halted the whole process.
    public int    getSubtype()     { return 0; }
    public String getTypeName()    { return type == ConnectivityManager.TYPE_WIFI ? "WIFI" : "MOBILE"; }
    public String getSubtypeName() { return ""; }
    // Pre-API-29 metadata accessors several analytics SDKs read; AOSP returns
    // null/false for an always-up non-failover network.
    public String getReason()     { return null; }
    public String getExtraInfo()  { return null; }
    public boolean isFailover()   { return false; }
    public State  getState()       { return state; }
    public boolean isConnected()       { return state == State.CONNECTED; }
    public boolean isConnectedOrConnecting() { return state == State.CONNECTED || state == State.CONNECTING; }
    public boolean isAvailable()       { return true; }
    public boolean isRoaming()         { return false; }
    public DetailedState getDetailedState() {
        switch (state) {
        case CONNECTING: return DetailedState.CONNECTING;
        case DISCONNECTING: return DetailedState.DISCONNECTING;
        case DISCONNECTED: return DetailedState.DISCONNECTED;
        case SUSPENDED: return DetailedState.SUSPENDED;
        case UNKNOWN: return DetailedState.IDLE;
        default: return DetailedState.CONNECTED;
        }
    }

    public enum DetailedState {
        IDLE, SCANNING, CONNECTING, AUTHENTICATING, OBTAINING_IPADDR,
        CONNECTED, SUSPENDED, DISCONNECTING, DISCONNECTED, FAILED, BLOCKED, VERIFYING_POOR_LINK,
        CAPTIVE_PORTAL_CHECK
    }

    @Override public String toString() {
        return "[type: " + getTypeName() + ", state: " + state + ", detailed: " + getDetailedState() + "]";
    }

    // PARCELABLE on AOSP: the type and the state round-trip.
    @Override public int describeContents() { return 0; }

    @Override public void writeToParcel(android.os.Parcel dest, int flags) {
        dest.writeInt(type);
        dest.writeString(state.name());
    }

    public static final Creator<NetworkInfo> CREATOR = new Creator<NetworkInfo>() {
        @Override public NetworkInfo createFromParcel(android.os.Parcel in) {
            int type = in.readInt();
            State state;
            try {
                state = State.valueOf(in.readString());
            } catch (IllegalArgumentException | NullPointerException e) {
                state = State.UNKNOWN;
            }
            return new NetworkInfo(type, state);
        }

        @Override public NetworkInfo[] newArray(int size) { return new NetworkInfo[size]; }
    };
}
