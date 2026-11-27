package com.macapk.loader;

/**
 * Whether the PC gives an app Vulkan, for the features a device announces it with
 * (PackageManager.hasSystemFeature and getSystemAvailableFeatures): the version an instance made
 * on the PC's Vulkan finds, at most 1.3, in AOSP's encoding (major << 22 | minor << 12; 0 = none).
 * The native side (windows/host/native_vulkan.cpp, over host/vk/guest_vk.cpp) is the libvulkan an
 * app's native code gets, so the two answers agree: the PC's driver, else the SwiftShader driver
 * shipped beside the runtime.
 */
public final class HostVulkan {
    private HostVulkan() {}

    private static volatile int sVersion = -1;

    private static native int nativeApiVersion();

    /** The version, 0 when there is no Vulkan (or no host native: a plain JVM). Asked once. */
    public static int apiVersion() {
        int v = sVersion;
        if (v < 0) {
            try {
                v = nativeApiVersion();
            } catch (Throwable t) {
                v = 0;
            }
            sVersion = v;
        }
        return v;
    }
}
