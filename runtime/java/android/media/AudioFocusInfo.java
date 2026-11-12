// android.media.AudioFocusInfo -- who asked for audio focus and how: what an
// audio policy's focus listener is told. A value type; audio policies are not
// modelled here, so nothing delivers one, but it parcels as AOSP's does.
// Mirrors frameworks/base/media/java/android/media/AudioFocusInfo.java.
package android.media;

import android.os.Parcel;
import android.os.Parcelable;

import java.util.Objects;

public final class AudioFocusInfo implements Parcelable {

    private final AudioAttributes mAttributes;
    private final int mClientUid;
    private final String mClientId;
    private final String mPackageName;
    private final int mSdkTarget;
    private int mGainRequest;
    private int mLossReceived;
    private int mFlags;

    /** @hide in AOSP: only the audio service makes one. */
    public AudioFocusInfo(AudioAttributes aa, int clientUid, String clientId, String packageName,
            int gainRequest, int lossReceived, int flags, int sdk) {
        mAttributes = (aa == null) ? new AudioAttributes.Builder().build() : aa;
        mClientUid = clientUid;
        mClientId = (clientId == null) ? "" : clientId;
        mPackageName = (packageName == null) ? "" : packageName;
        mGainRequest = gainRequest;
        mLossReceived = lossReceived;
        mFlags = flags;
        mSdkTarget = sdk;
    }

    public AudioAttributes getAttributes() { return mAttributes; }

    public int getClientUid() { return mClientUid; }

    public String getClientId() { return mClientId; }

    public String getPackageName() { return mPackageName; }

    /** One of the AudioManager.AUDIOFOCUS_GAIN* values. */
    public int getGainRequest() { return mGainRequest; }

    /** The focus loss this client was last told of, 0 when none. */
    public int getLossReceived() { return mLossReceived; }

    public int getFlags() { return mFlags; }

    @Override public int hashCode() {
        return Objects.hash(mAttributes, mClientUid, mClientId, mPackageName, mGainRequest, mFlags);
    }

    @Override public boolean equals(Object obj) {
        if (this == obj) return true;
        if (!(obj instanceof AudioFocusInfo)) return false;
        AudioFocusInfo other = (AudioFocusInfo) obj;
        return mAttributes.equals(other.mAttributes) && mClientUid == other.mClientUid
                && mClientId.equals(other.mClientId) && mPackageName.equals(other.mPackageName)
                && mGainRequest == other.mGainRequest && mLossReceived == other.mLossReceived
                && mFlags == other.mFlags && mSdkTarget == other.mSdkTarget;
    }

    @Override public int describeContents() { return 0; }

    @Override public void writeToParcel(Parcel dest, int flags) {
        mAttributes.writeToParcel(dest, flags);
        dest.writeInt(mClientUid);
        dest.writeString(mClientId);
        dest.writeString(mPackageName);
        dest.writeInt(mGainRequest);
        dest.writeInt(mLossReceived);
        dest.writeInt(mFlags);
        dest.writeInt(mSdkTarget);
    }

    public static final Parcelable.Creator<AudioFocusInfo> CREATOR = new Parcelable.Creator<AudioFocusInfo>() {
        @Override public AudioFocusInfo createFromParcel(Parcel in) {
            AudioAttributes aa = AudioAttributes.CREATOR.createFromParcel(in);
            int clientUid = in.readInt();
            String clientId = in.readString();
            String packageName = in.readString();
            int gainRequest = in.readInt();
            int lossReceived = in.readInt();
            int flags = in.readInt();
            int sdk = in.readInt();
            return new AudioFocusInfo(aa, clientUid, clientId, packageName, gainRequest, lossReceived, flags, sdk);
        }
        @Override public AudioFocusInfo[] newArray(int size) { return new AudioFocusInfo[size]; }
    };
}
