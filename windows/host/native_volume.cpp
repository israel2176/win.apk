// native_volume.cpp -- com.macapk.loader.HostVolume: the default output device's master volume and mute,
// and whether anything plays on it, for AudioManager's media stream (a phone's media volume is the
// device's: the one Windows' taskbar volume and the keyboard's volume keys move).
//
// Each call opens the default render endpoint afresh: the default device changes when headphones are
// plugged in or the user picks another in Windows' settings, and a phone's volume follows its current
// output too. COM is entered on the calling thread (any Java thread), multithreaded, as the other
// audio natives do.

#include <windows.h>

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include "host_natives.h"

namespace winapk::host {

namespace {

template <typename T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() { return p; }
};

/** The default output device (multimedia role, as an app's sound goes to), or false. */
bool default_device(Com<IMMDevice>& device) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Com<IMMDeviceEnumerator> enumerator;
    return SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator))) &&
           SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device));
}

bool endpoint_volume(Com<IAudioEndpointVolume>& volume) {
    Com<IMMDevice> device;
    return default_device(device) &&
           SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(&volume)));
}

jfloat JNICALL Vol_master(JNIEnv*, jclass) {
    Com<IAudioEndpointVolume> volume;
    float level = 0;
    if (!endpoint_volume(volume) || FAILED(volume->GetMasterVolumeLevelScalar(&level))) return -1.f;
    return level;
}

jboolean JNICALL Vol_setMaster(JNIEnv*, jclass, jfloat scalar) {
    Com<IAudioEndpointVolume> volume;
    return endpoint_volume(volume) && SUCCEEDED(volume->SetMasterVolumeLevelScalar(scalar, nullptr));
}

jint JNICALL Vol_muted(JNIEnv*, jclass) {
    Com<IAudioEndpointVolume> volume;
    BOOL mute = FALSE;
    if (!endpoint_volume(volume) || FAILED(volume->GetMute(&mute))) return -1;
    return mute ? 1 : 0;
}

jboolean JNICALL Vol_setMuted(JNIEnv*, jclass, jboolean muted) {
    Com<IAudioEndpointVolume> volume;
    return endpoint_volume(volume) && SUCCEEDED(volume->SetMute(muted ? TRUE : FALSE, nullptr));
}

/** Any audio session on the default output that is active (playing), this process's included. */
jboolean JNICALL Vol_anyActive(JNIEnv*, jclass) {
    Com<IMMDevice> device;
    Com<IAudioSessionManager2> manager;
    Com<IAudioSessionEnumerator> sessions;
    if (!default_device(device) ||
        FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&manager))) ||
        FAILED(manager->GetSessionEnumerator(&sessions))) {
        return JNI_FALSE;
    }
    int count = 0;
    if (FAILED(sessions->GetCount(&count))) return JNI_FALSE;
    for (int i = 0; i < count; ++i) {
        Com<IAudioSessionControl> session;
        AudioSessionState state = AudioSessionStateInactive;
        if (SUCCEEDED(sessions->GetSession(i, &session)) && SUCCEEDED(session->GetState(&state)) &&
            state == AudioSessionStateActive) {
            return JNI_TRUE;
        }
    }
    return JNI_FALSE;
}

constexpr const char* kCls = "com.macapk.loader.HostVolume";

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE(kCls, "nativeMasterVolume", "()F", Vol_master),
    WINAPK_HOST_NATIVE(kCls, "nativeSetMasterVolume", "(F)Z", Vol_setMaster),
    WINAPK_HOST_NATIVE(kCls, "nativeMuted", "()I", Vol_muted),
    WINAPK_HOST_NATIVE(kCls, "nativeSetMuted", "(Z)Z", Vol_setMuted),
    WINAPK_HOST_NATIVE(kCls, "nativeAnyActive", "()Z", Vol_anyActive),
};

}  // namespace

HostMethods native_volume_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
