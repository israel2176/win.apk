// AudioManagerVolumeTest -- AudioManager's stream volumes as AOSP's AudioService keeps them (ranges,
// defaults, aliasing, mute, adjust), on a plain JVM: no host, so HostVolume has no output device and
// every stream, the media stream too, keeps its index in the process. With winapk-runtime the media
// stream reads and sets Windows' volume instead (windows/host/native_volume.cpp).
package android.media;

public class AudioManagerVolumeTest {
    static int failures;

    static void check(String what, Object got, Object want) {
        boolean ok = want.equals(got);
        System.out.println((ok ? "  ok   " : "  FAIL ") + what + ": " + got + (ok ? "" : " (want " + want + ")"));
        if (!ok) failures++;
    }

    public static void main(String[] args) {
        AudioManager a = new AudioManager(), b = new AudioManager();
        // AOSP's ranges and defaults.
        check("music max", a.getStreamMaxVolume(AudioManager.STREAM_MUSIC), 15);
        check("voice call min", a.getStreamMinVolume(AudioManager.STREAM_VOICE_CALL), 1);
        check("alarm default", a.getStreamVolume(AudioManager.STREAM_ALARM), 6);
        // One value for the device: another AudioManager reads what this one set.
        a.setStreamVolume(AudioManager.STREAM_MUSIC, 11, 0);
        check("music shared", b.getStreamVolume(AudioManager.STREAM_MUSIC), 11);
        // Clamped to the stream's range.
        a.setStreamVolume(AudioManager.STREAM_ALARM, 99, 0);
        check("alarm clamped", a.getStreamVolume(AudioManager.STREAM_ALARM), 7);
        // Aliases of a device without telephony: notification follows ring, accessibility music.
        a.setStreamVolume(AudioManager.STREAM_RING, 3, 0);
        check("notification follows ring", a.getStreamVolume(AudioManager.STREAM_NOTIFICATION), 3);
        check("accessibility follows music", a.getStreamVolume(AudioManager.STREAM_ACCESSIBILITY), 11);
        // Adjust and mute.
        a.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_LOWER, 0);
        check("music lowered", a.getStreamVolume(AudioManager.STREAM_MUSIC), 10);
        a.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_MUTE, 0);
        check("music muted", a.isStreamMute(AudioManager.STREAM_MUSIC), true);
        check("a muted stream reads 0", a.getStreamVolume(AudioManager.STREAM_MUSIC), 0);
        a.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_TOGGLE_MUTE, 0);
        check("music back", a.getStreamVolume(AudioManager.STREAM_MUSIC), 10);
        a.adjustVolume(AudioManager.ADJUST_RAISE, 0);
        check("adjustVolume moves music", a.getStreamVolume(AudioManager.STREAM_MUSIC), 11);
        boolean threw = false;
        try {
            a.getStreamVolume(42);
        } catch (IllegalArgumentException e) {
            threw = true;
        }
        check("a bad stream throws", threw, true);
        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
