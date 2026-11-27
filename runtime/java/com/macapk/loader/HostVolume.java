// HostVolume -- Windows' output volume, which AudioManager's media stream is (android.media.AudioManager).
//
// A phone's media volume is the device's: the same number in every app, the one the volume keys move,
// what the user hears. On Windows that is the default output device's master volume and mute
// (IAudioEndpointVolume, windows/host/native_volume.cpp), which the taskbar's volume and the keyboard's
// keys move. AudioManager reads and sets STREAM_MUSIC (and the streams AOSP aliases to it) here.
package com.macapk.loader;

public final class HostVolume {
    private HostVolume() {}

    /** The default output device's master volume, 0..1; -1 when there is no output device (or no host:
     *  a plain JVM, where the native is not bound). */
    public static float masterVolume() {
        try {
            return nativeMasterVolume();
        } catch (UnsatisfiedLinkError e) {
            return -1f;
        }
    }

    /** Sets it; false when there is no output device. */
    public static boolean setMasterVolume(float scalar) {
        try {
            return nativeSetMasterVolume(Math.max(0f, Math.min(1f, scalar)));
        } catch (UnsatisfiedLinkError e) {
            return false;
        }
    }

    /** 1 muted, 0 not, -1 no output device. */
    public static int muted() {
        try {
            return nativeMuted();
        } catch (UnsatisfiedLinkError e) {
            return -1;
        }
    }

    public static boolean setMuted(boolean muted) {
        try {
            return nativeSetMuted(muted);
        } catch (UnsatisfiedLinkError e) {
            return false;
        }
    }

    /** Whether any program plays sound on the default output device now (an active audio session):
     *  AudioService.isMusicActive's answer for the whole device. */
    public static boolean anyActive() {
        try {
            return nativeAnyActive();
        } catch (UnsatisfiedLinkError e) {
            return false;
        }
    }

    private static native float nativeMasterVolume();
    private static native boolean nativeSetMasterVolume(float scalar);
    private static native int nativeMuted();
    private static native boolean nativeSetMuted(boolean muted);
    private static native boolean nativeAnyActive();
}
