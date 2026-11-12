// android.net.wifi.WifiScanner -- the privileged scan service (a system API):
// one-off and background scans with the caller's own channels and timing.
// Mirrors packages/modules/Wifi
// framework/java/android/net/wifi/WifiScanner.java.
//
// The host scans when it chooses and hands over what it saw
// (WifiManager.getScanResults); an app cannot schedule the radio. So every
// request is answered through its listener with onFailure, AOSP's answer on a
// device whose scanner refuses the request, and never left without one.
package android.net.wifi;

import android.os.Parcel;
import android.os.Parcelable;
import android.os.WorkSource;

import java.util.ArrayList;
import java.util.List;

public class WifiScanner {

    public static final int WIFI_BAND_UNSPECIFIED = 0;
    public static final int WIFI_BAND_24_GHZ = 1;
    public static final int WIFI_BAND_5_GHZ = 2;
    public static final int WIFI_BAND_BOTH = 3;
    public static final int WIFI_BAND_5_GHZ_DFS_ONLY = 4;
    public static final int WIFI_BAND_5_GHZ_WITH_DFS = 6;
    public static final int WIFI_BAND_BOTH_WITH_DFS = 7;
    public static final int WIFI_BAND_6_GHZ = 8;
    public static final int WIFI_BAND_24_5_6_GHZ = 11;
    public static final int WIFI_BAND_24_5_WITH_DFS_6_GHZ = 15;
    public static final int WIFI_BAND_60_GHZ = 16;

    public static final int MIN_SCAN_PERIOD_MS = 1000;
    public static final int MAX_SCAN_PERIOD_MS = 1024000;

    public static final int REASON_SUCCEEDED = 0;
    public static final int REASON_UNSPECIFIED = -1;
    public static final int REASON_INVALID_LISTENER = -2;
    public static final int REASON_INVALID_REQUEST = -3;
    public static final int REASON_NOT_AUTHORIZED = -4;
    public static final int REASON_DUPLICATE_REQEUST = -5;

    public static final int REPORT_EVENT_AFTER_BUFFER_FULL = 0;
    public static final int REPORT_EVENT_AFTER_EACH_SCAN = (1 << 0);
    public static final int REPORT_EVENT_FULL_SCAN_RESULT = (1 << 1);
    public static final int REPORT_EVENT_NO_BATCH = (1 << 2);

    public static final int SCAN_TYPE_LOW_LATENCY = 0;
    public static final int SCAN_TYPE_LOW_POWER = 1;
    public static final int SCAN_TYPE_HIGH_ACCURACY = 2;

    private static final String UNSUPPORTED = "Wi-Fi scanning is not available to apps on this device";

    /** @hide in AOSP: Context.getSystemService(WIFI_SCANNING_SERVICE) makes it. */
    public WifiScanner() {}

    /** How a request ended. */
    public interface ActionListener {
        void onSuccess();
        void onFailure(int reason, String description);
    }

    /** The results of a scan the listener's owner started. */
    public interface ScanListener extends ActionListener {
        /** @deprecated never called since Android 12. */
        @Deprecated void onPeriodChanged(int periodInMs);
        void onResults(ScanData[] results);
        void onFullResult(ScanResult fullScanResult);
    }

    /** One channel to scan. */
    public static class ChannelSpec {
        /** Channel frequency in MHz. */
        public int frequency;
        /** @hide in AOSP. */
        public boolean passive;
        /** @hide in AOSP: how long to dwell on the channel, in ms. */
        public int dwellTimeMS;

        public ChannelSpec(int frequency) {
            this.frequency = frequency;
            passive = false;
            dwellTimeMS = 0;
        }
    }

    /** What to scan and how to report it. */
    public static class ScanSettings implements Parcelable {
        /** One of the WIFI_BAND_* values. */
        public int band;
        /** The channels to scan when band is WIFI_BAND_UNSPECIFIED. */
        public ChannelSpec[] channels;
        /** The period of a background scan, in ms. */
        public int periodInMs;
        /** A REPORT_EVENT_* bit mask. */
        public int reportEvents;
        public int numBssidsPerScan;
        public int maxScansToCache;
        public int maxPeriodInMs;
        public int stepCount;
        public boolean isPnoScan;
        /** One of the SCAN_TYPE_* values. */
        public int type = SCAN_TYPE_LOW_LATENCY;
        public boolean ignoreLocationSettings;
        public boolean hideFromAppOps;

        public ScanSettings() {}

        @Override public int describeContents() { return 0; }

        @Override public void writeToParcel(Parcel dest, int flags) {
            dest.writeInt(band);
            dest.writeInt(periodInMs);
            dest.writeInt(reportEvents);
            dest.writeInt(numBssidsPerScan);
            dest.writeInt(maxScansToCache);
            dest.writeInt(maxPeriodInMs);
            dest.writeInt(stepCount);
            dest.writeInt(isPnoScan ? 1 : 0);
            dest.writeInt(type);
            dest.writeInt(ignoreLocationSettings ? 1 : 0);
            dest.writeInt(hideFromAppOps ? 1 : 0);
            int n = (channels == null) ? 0 : channels.length;
            dest.writeInt(n);
            for (int i = 0; i < n; i++) {
                dest.writeInt(channels[i].frequency);
                dest.writeInt(channels[i].dwellTimeMS);
                dest.writeInt(channels[i].passive ? 1 : 0);
            }
        }

        public static final Parcelable.Creator<ScanSettings> CREATOR = new Parcelable.Creator<ScanSettings>() {
            @Override public ScanSettings createFromParcel(Parcel in) {
                ScanSettings settings = new ScanSettings();
                settings.band = in.readInt();
                settings.periodInMs = in.readInt();
                settings.reportEvents = in.readInt();
                settings.numBssidsPerScan = in.readInt();
                settings.maxScansToCache = in.readInt();
                settings.maxPeriodInMs = in.readInt();
                settings.stepCount = in.readInt();
                settings.isPnoScan = in.readInt() == 1;
                settings.type = in.readInt();
                settings.ignoreLocationSettings = in.readInt() == 1;
                settings.hideFromAppOps = in.readInt() == 1;
                int n = in.readInt();
                settings.channels = new ChannelSpec[n];
                for (int i = 0; i < n; i++) {
                    ChannelSpec spec = new ChannelSpec(in.readInt());
                    spec.dwellTimeMS = in.readInt();
                    spec.passive = in.readInt() == 1;
                    settings.channels[i] = spec;
                }
                return settings;
            }
            @Override public ScanSettings[] newArray(int size) { return new ScanSettings[size]; }
        };
    }

    /** The access points one scan saw. */
    public static class ScanData implements Parcelable {
        private final int mId;
        private final int mFlags;
        private final ScanResult[] mResults;

        /** @hide in AOSP: only the scanner makes one. */
        public ScanData(int id, int flags, ScanResult[] results) {
            mId = id;
            mFlags = flags;
            mResults = (results == null) ? new ScanResult[0] : results;
        }

        public int getId() { return mId; }

        public int getFlags() { return mFlags; }

        public ScanResult[] getResults() { return mResults; }

        @Override public int describeContents() { return 0; }

        @Override public void writeToParcel(Parcel dest, int flags) {
            dest.writeInt(mId);
            dest.writeInt(mFlags);
            dest.writeInt(mResults.length);
            for (ScanResult result : mResults) result.writeToParcel(dest, flags);
        }

        public static final Parcelable.Creator<ScanData> CREATOR = new Parcelable.Creator<ScanData>() {
            @Override public ScanData createFromParcel(Parcel in) {
                int id = in.readInt();
                int flags = in.readInt();
                ScanResult[] results = new ScanResult[in.readInt()];
                for (int i = 0; i < results.length; i++) results[i] = ScanResult.CREATOR.createFromParcel(in);
                return new ScanData(id, flags, results);
            }
            @Override public ScanData[] newArray(int size) { return new ScanData[size]; }
        };
    }

    /** The channels of a band. None: this scanner drives no radio. */
    public List<Integer> getAvailableChannels(int band) { return new ArrayList<Integer>(); }

    public void startBackgroundScan(ScanSettings settings, ScanListener listener) {
        startBackgroundScan(settings, listener, null);
    }

    public void startBackgroundScan(ScanSettings settings, ScanListener listener, WorkSource workSource) {
        refuse(listener);
    }

    public void stopBackgroundScan(ScanListener listener) {
        if (listener == null) throw new NullPointerException("listener cannot be null");
    }

    /** Asks for the results a background scan has cached; false: none will come. */
    public boolean getScanResults() { return false; }

    public void startScan(ScanSettings settings, ScanListener listener) {
        startScan(settings, listener, null);
    }

    public void startScan(ScanSettings settings, ScanListener listener, WorkSource workSource) {
        refuse(listener);
    }

    public void stopScan(ScanListener listener) {
        if (listener == null) throw new NullPointerException("listener cannot be null");
    }

    private static void refuse(ActionListener listener) {
        if (listener == null) throw new NullPointerException("listener cannot be null");
        listener.onFailure(REASON_UNSPECIFIED, UNSUPPORTED);
    }
}
