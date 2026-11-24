// native_opensles.cpp -- libOpenSLES (OpenSL ES 1.0.1 with Android's
// extensions) for an app's native code, on audio_engine.h's mixer.
//
// Adapted from the other runtime (https://example.invalid/other-runtime),
// compat/media/ndk/opensles.cc, Copyright (C) 2026 its authors,
// SPDX-License-Identifier: Apache-2.0. Modified for Win.apk: the players write
// into audio_engine.h's streams and voices instead of the other runtime's tracks and
// player, the recorders read audio_capture.h's WASAPI capture, guest callbacks
// go through arm64::host_callable, and RECORD_AUDIO is the runtime's own
// permission model (SpecialAccessStore.holds, as AudioRecord asks it).
//
// What it gives apps is the part of Android's implementation
// (frameworks/wilhelm) that games and audio libraries use:
//
//   - the engine object and SLEngineItf (CreateOutputMix, CreateAudioPlayer,
//     CreateAudioRecorder, the supported-interface queries);
//   - the output mix object (realized; no environmental reverb);
//   - audio players from an Android simple buffer queue or a buffer queue of
//     PCM (SLDataFormat_PCM, SLAndroidDataFormat_PCM_EX: 8/16/24/32-bit,
//     float), an audio_engine.h stream fed from the queued buffers, the
//     queue's callback called as each buffer is taken (as wilhelm does from
//     AudioTrack's callback);
//   - audio players from an Android file descriptor (AAsset_openFileDescriptor's)
//     or a URI naming a file: the whole file decoded at Realize
//     (audio::decode: WAV, Ogg Vorbis, what Media Foundation reads) and played
//     as a clip voice, as SoundPool's are. http(s) URIs are not supported;
//   - SLPlayItf (states, position, duration, the head-at-end event), SLVolumeItf
//     (level in millibels, mute), SLSeekItf (position, loop),
//     SLAndroidConfigurationItf (the stream type, stored), SLPrefetchStatusItf
//     (always sufficient data: the file is in memory);
//   - audio recorders from the audio input device into an Android simple
//     buffer queue of PCM: the capture fills the queued buffers, the queue's
//     callback called as each is full; SLRecordItf (states, position). Like
//     AudioRecord they need RECORD_AUDIO where the runtime models it
//     (SL_RESULT_PERMISSION_DENIED at create, SL_RESULT_CONTENT_UNSUPPORTED at
//     a Realize after a revoke), and a capture device Windows lets them open.
//
// Every function is a System V entry the library calls through its vtables or
// its imports (SLAPIENTRY is empty on Android); an interface is a pointer to a
// slot whose first word is the vtable, so arm64 code calls the same host
// functions through the bridge (arm64_guest.h). Interface IDs are compared by
// GUID value, as wilhelm does. Callbacks run on each object's own feeder
// thread, like AAudio's (native_aaudio.cpp).
//
// The interface IDs: libOpenSLES exports each SL_IID_x as a variable of type
// SLInterfaceID, a pointer to the interface's GUID, and code that uses OpenSL
// ES reads them (into the arrays it passes to CreateAudioPlayer and
// GetInterface) as soon as it is loaded. The GUID's layout is the same on both
// sides: fixed-width fields, 16 bytes (SLInterfaceID_ in OpenSLES.h):
//
//   time_low 0 (4), time_mid 4 (2), time_hi_and_version 6 (2),
//   clock_seq 8 (2), node 10 (6)
//
// The values and the exported names are AOSP's (frameworks/wilhelm:
// src/OpenSLES_IID.cpp and src/sl_iid.cpp).

#include "native_opensles.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <jni.h>

#include "audio_capture.h"
#include "audio_engine.h"
#include "winapk/arm64_guest.h"
#include "winapk/guest_abi.h"
#include "winapk/mini_bionic.h"

namespace winapk::host {

namespace {

// ---- OpenSLES.h, OpenSLES_Android.h ---------------------------------------------------

using SLuint32 = uint32_t;
using SLint32 = int32_t;
using SLint16 = int16_t;
using SLresult = uint32_t;
using SLboolean = uint32_t;
using SLmillibel = int16_t;
using SLmillisecond = uint32_t;
using SLpermille = int16_t;

constexpr SLresult kSuccess = 0;
constexpr SLresult kPreconditionsViolated = 1;
constexpr SLresult kParameterInvalid = 2;
constexpr SLresult kBufferInsufficient = 7;
constexpr SLresult kContentUnsupported = 9;
constexpr SLresult kPermissionDenied = 11;
constexpr SLresult kFeatureUnsupported = 12;

constexpr SLuint32 kObjectUnrealized = 1, kObjectRealized = 2;
constexpr SLuint32 kObjectEventAsyncTermination = 4;
constexpr SLuint32 kPlayStopped = 1, kPlayPaused = 2, kPlayPlaying = 3;
constexpr SLuint32 kPlayEventHeadAtEnd = 0x1;
constexpr SLuint32 kRecordStopped = 1, kRecordPaused = 2, kRecordRecording = 3;
constexpr SLuint32 kLocatorUri = 1, kLocatorIoDevice = 3, kLocatorOutputMix = 4, kLocatorBufferQueue = 6,
                   kLocatorAndroidFd = 0x800007BC, kLocatorAndroidSimpleBufferQueue = 0x800007BD;
constexpr SLuint32 kIoDeviceAudioInput = 1, kDefaultDeviceIdAudioInput = 0xFFFFFFFF;
constexpr SLuint32 kFormatPcm = 2, kFormatPcmEx = 4;
constexpr SLuint32 kPcmRepresentationFloat = 3;
constexpr SLuint32 kByteOrderLittleEndian = 2;
constexpr SLuint32 kPrefetchStatusSufficientData = 3;
constexpr SLmillisecond kTimeUnknown = 0xFFFFFFFF;

struct SlGuid {
    uint32_t time_low;
    uint16_t time_mid;
    uint16_t time_hi_and_version;
    uint16_t clock_seq;
    uint8_t node[6];
};
static_assert(sizeof(SlGuid) == 16, "SLInterfaceID_");
using SLInterfaceID = const SlGuid*;

struct SlInterfaceId {
    const char* name;
    SlGuid guid;
};

const SlInterfaceId kInterfaceIds[] = {
    {"SL_IID_3DCOMMIT", {0x3564ad80, 0xdd0f, 0x11db, 0x9e19, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_3DDOPPLER", {0xb45c9a80, 0xddd2, 0x11db, 0xb028, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_3DGROUPING", {0xebe844e0, 0xddd2, 0x11db, 0xb510, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_3DLOCATION", {0x2b878020, 0xddd3, 0x11db, 0x8a01, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_3DMACROSCOPIC", {0x5089aec0, 0xddd3, 0x11db, 0x9ad3, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_3DSOURCE", {0x70bc7b00, 0xddd3, 0x11db, 0xa873, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_AUDIODECODERCAPABILITIES", {0x3fe5a3a0, 0xfcc6, 0x11db, 0x94ac, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_AUDIOENCODER", {0xd7d5af7a, 0x351c, 0x41a6, 0x94ec, {0x1a, 0xc9, 0x5c, 0x71, 0x82, 0x2c}}},
    {"SL_IID_AUDIOENCODERCAPABILITIES", {0x0f52a340, 0xfcd1, 0x11db, 0xa993, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_AUDIOIODEVICECAPABILITIES", {0xb2564dc0, 0xddd3, 0x11db, 0xbd62, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_BASSBOOST", {0x0634f220, 0xddd4, 0x11db, 0xa0fc, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_BUFFERQUEUE", {0x2bc99cc0, 0xddd4, 0x11db, 0x8d99, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_DEVICEVOLUME", {0xe1634760, 0xf3e2, 0x11db, 0x9ca9, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_DYNAMICINTERFACEMANAGEMENT", {0x63936540, 0xf775, 0x11db, 0x9cc4, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_DYNAMICSOURCE", {0xc55cc100, 0x038b, 0x11dc, 0xbb45, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_EFFECTSEND", {0x56e7d200, 0xddd4, 0x11db, 0xaefb, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ENGINE", {0x8d97c260, 0xddd4, 0x11db, 0x958f, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ENGINECAPABILITIES", {0x8320d0a0, 0xddd5, 0x11db, 0xa1b1, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ENVIRONMENTALREVERB", {0xc2e5d5f0, 0x94bd, 0x4763, 0x9cac, {0x4e, 0x23, 0x4d, 0x06, 0x83, 0x9e}}},
    {"SL_IID_EQUALIZER", {0x0bed4300, 0xddd6, 0x11db, 0x8f34, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_LED", {0x2cc1cd80, 0xddd6, 0x11db, 0x807e, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_METADATAEXTRACTION", {0xaa5b1f80, 0xddd6, 0x11db, 0xac8e, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_METADATATRAVERSAL", {0xc43662c0, 0xddd6, 0x11db, 0xa7ab, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_MIDIMESSAGE", {0xddf4a820, 0xddd6, 0x11db, 0xb174, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_MIDIMUTESOLO", {0x039eaf80, 0xddd7, 0x11db, 0x9a02, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_MIDITEMPO", {0x1f347400, 0xddd7, 0x11db, 0xa7ce, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_MIDITIME", {0x3da51de0, 0xddd7, 0x11db, 0xaf70, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_MUTESOLO", {0x5a28ebe0, 0xddd7, 0x11db, 0x8220, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_NULL", {0xec7178ec, 0xe5e1, 0x4432, 0xa3f4, {0x46, 0x57, 0xe6, 0x79, 0x52, 0x10}}},
    {"SL_IID_OBJECT", {0x79216360, 0xddd7, 0x11db, 0xac16, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_OUTPUTMIX", {0x97750f60, 0xddd7, 0x11db, 0x92b1, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_PITCH", {0xc7e8ee00, 0xddd7, 0x11db, 0xa42c, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_PLAY", {0xef0bd9c0, 0xddd7, 0x11db, 0xbf49, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_PLAYBACKRATE", {0x2e3b2a40, 0xddda, 0x11db, 0xa349, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_PREFETCHSTATUS", {0x2a41ee80, 0xddd8, 0x11db, 0xa41f, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_PRESETREVERB", {0x47382d60, 0xddd8, 0x11db, 0xbf3a, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_RATEPITCH", {0x61b62e60, 0xddda, 0x11db, 0x9eb8, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_RECORD", {0xc5657aa0, 0xdddb, 0x11db, 0x82f7, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_SEEK", {0xd43135a0, 0xdddc, 0x11db, 0xb458, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_THREADSYNC", {0xf6ac6b40, 0xdddc, 0x11db, 0xa62e, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_VIBRA", {0x169a8d60, 0xdddd, 0x11db, 0x923d, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_VIRTUALIZER", {0x37cc2c00, 0xdddd, 0x11db, 0x8577, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_VISUALIZATION", {0xe46b26a0, 0xdddd, 0x11db, 0x8afd, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_VOLUME", {0x09e8ede0, 0xddde, 0x11db, 0xb4f6, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_OUTPUTMIXEXT", {0xfe5cce00, 0x57bb, 0x11df, 0x951c, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDEFFECT", {0xae12da60, 0x99ac, 0x11df, 0xb456, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDEFFECTCAPABILITIES", {0x6a4f6d60, 0xb5e6, 0x11df, 0xbb3b, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDEFFECTSEND", {0x7be462c0, 0xbc43, 0x11df, 0x8670, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDCONFIGURATION", {0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", {0x198e4940, 0xc5d7, 0x11df, 0xa2a6, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDBUFFERQUEUESOURCE", {0x7fc1a460, 0xeec1, 0x11df, 0xa306, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDACOUSTICECHOCANCELLATION", {0x7b491460, 0x8d4d, 0x11e0, 0xbd61, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDAUTOMATICGAINCONTROL", {0x0a8abfe0, 0x654c, 0x11e0, 0xba26, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDNOISESUPPRESSION", {0x58b4b260, 0x8e06, 0x11e0, 0xaa8e, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
};
constexpr size_t kInterfaceIdCount = sizeof(kInterfaceIds) / sizeof(kInterfaceIds[0]);

SLInterfaceID iid(const char* name) {
    for (const auto& i : kInterfaceIds) {
        if (std::strcmp(i.name, name) == 0) return &i.guid;
    }
    return nullptr;
}

bool same_id(SLInterfaceID a, SLInterfaceID b) { return a && b && std::memcmp(a, b, sizeof(*a)) == 0; }

struct LocatorHeader {
    SLuint32 type;
};
struct LocatorUri {
    SLuint32 type;
    const char* uri;
};
struct LocatorAndroidFd {
    SLuint32 type;
    SLint32 fd;
    int64_t offset;
    int64_t length;  // SL_DATALOCATOR_ANDROIDFD_USE_FILE_SIZE (-1): to the end
};
struct LocatorQueue {
    SLuint32 type;
    SLuint32 num_buffers;
};
struct LocatorIoDevice {
    SLuint32 type, device_type, device_id;
    void* device;
};
struct FormatPcm {
    SLuint32 format_type, num_channels, samples_per_sec /* milliHz */, bits_per_sample, container_size,
        channel_mask, endianness;
};
struct FormatPcmEx {
    SLuint32 format_type, num_channels, sample_rate, bits_per_sample, container_size, channel_mask, endianness,
        representation;
};
struct DataSource {
    void* locator;
    void* format;
};
using DataSink = DataSource;

// ---- objects ----------------------------------------------------------------------------

struct Object;

// Every interface an object hands out: its vtable pointer first (what the app
// dereferences), then its object.
struct ItfSlot {
    const void* vtable;
    Object* object;
};
Object* object_of(const void* self) { return reinterpret_cast<const ItfSlot*>(self)->object; }

// The guest's callbacks: arm64 code gets a System V entry (host_callable).
using QueueCallback = void(WINAPK_GUEST_ABI*)(const void* itf, void* ctx);
using PlayCallback = void(WINAPK_GUEST_ABI*)(const void* itf, void* ctx, SLuint32 event);
using ObjectCallback = void(WINAPK_GUEST_ABI*)(const void* itf, const void* ctx, SLuint32 event, SLresult result,
                                               SLuint32 param, void* itf_out);
template <typename Fn>
Fn guest_fn(void* fn) {
    return reinterpret_cast<Fn>(fn ? arm64::host_callable(fn) : nullptr);
}

enum class Kind { kEngine, kOutputMix, kPlayer, kRecorder };
enum class Slot { kObject, kEngine, kPlay, kQueue, kVolume, kSeek, kConfig, kPrefetch, kRecord };
constexpr uint32_t kind_bit(Kind k) { return 1u << static_cast<unsigned>(k); }
constexpr uint32_t slot_bit(Slot s) { return 1u << static_cast<unsigned>(s); }
constexpr uint32_t kPlayers = kind_bit(Kind::kPlayer), kRecorders = kind_bit(Kind::kRecorder);
constexpr uint32_t kAllKinds = kind_bit(Kind::kEngine) | kind_bit(Kind::kOutputMix) | kPlayers | kRecorders;

struct Interface {
    const char* id;
    Slot slot;
    uint32_t kinds;
    bool implicit;    // exposed without being asked for
    bool prerealize;  // GetInterface answers it before Realize
};

const Interface kInterfaces[] = {
    {"SL_IID_OBJECT", Slot::kObject, kAllKinds, true, true},
    {"SL_IID_ENGINE", Slot::kEngine, kind_bit(Kind::kEngine), true, false},
    {"SL_IID_PLAY", Slot::kPlay, kPlayers, true, false},
    {"SL_IID_BUFFERQUEUE", Slot::kQueue, kPlayers | kRecorders, false, false},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", Slot::kQueue, kPlayers | kRecorders, false, false},
    {"SL_IID_VOLUME", Slot::kVolume, kPlayers, false, false},
    {"SL_IID_SEEK", Slot::kSeek, kPlayers, false, false},
    {"SL_IID_ANDROIDCONFIGURATION", Slot::kConfig, kPlayers | kRecorders, false, true},
    {"SL_IID_PREFETCHSTATUS", Slot::kPrefetch, kPlayers, false, false},
    {"SL_IID_RECORD", Slot::kRecord, kRecorders, true, false},
};

// The interface `id` names on an object of `kind`; a buffer-queue player has
// the queue and no seek or prefetch status, a file player the other way round.
const Interface* find_interface(Kind kind, bool queue, SLInterfaceID id) {
    for (const auto& i : kInterfaces) {
        if (!(i.kinds & kind_bit(kind)) || !same_id(id, iid(i.id))) continue;
        if (kind == Kind::kPlayer && ((i.slot == Slot::kQueue && !queue) ||
                                      ((i.slot == Slot::kSeek || i.slot == Slot::kPrefetch) && queue))) {
            return nullptr;
        }
        return &i;
    }
    return nullptr;
}

// The interfaces an object exposes: its implicit ones and those asked for.
// One asked for as required that it does not have fails the create.
SLresult check_interfaces(Kind kind, bool queue, SLuint32 count, const SLInterfaceID* ids, const SLboolean* required,
                          uint32_t* exposed) {
    uint32_t mask = 0;
    for (const auto& i : kInterfaces) {
        if ((i.kinds & kind_bit(kind)) && i.implicit) mask |= slot_bit(i.slot);
    }
    if (count > 0 && (!ids || !required)) return kParameterInvalid;
    bool unsupported = false;
    for (SLuint32 n = 0; n < count; ++n) {
        if (!ids[n]) return kParameterInvalid;
        if (const Interface* i = find_interface(kind, queue, ids[n])) {
            mask |= slot_bit(i->slot);
        } else if (required[n]) {
            unsupported = true;
        }
    }
    if (unsupported) return kFeatureUnsupported;
    *exposed = mask;
    return kSuccess;
}

// A PCM layout: bytes per sample and how audio_engine.h takes it.
enum class Pcm { kU8, kS16, kS24, kS32, kFloat };
size_t bytes_per_sample(Pcm f) {
    switch (f) {
        case Pcm::kU8: return 1;
        case Pcm::kS16: return 2;
        case Pcm::kS24: return 3;
        default: return 4;
    }
}

struct Object {
    Kind kind;
    ItfSlot object_itf, engine_itf, play_itf, queue_itf, volume_itf, seek_itf, config_itf, prefetch_itf, record_itf;
    SLuint32 state = kObjectUnrealized;
    uint32_t exposed = 0;
    ObjectCallback object_cb = nullptr;
    const void* object_ctx = nullptr;

    // Buffer queues (players and recorders).
    bool queue_player = false;
    Pcm pcm = Pcm::kS16;
    int channels = 2;
    uint32_t rate = 0;
    size_t frame_size = 4;
    SLuint32 queue_capacity = 0;
    struct Buffer {
        uint8_t* data;
        size_t size;
        size_t done;
    };
    std::deque<Buffer> queue;
    SLuint32 queue_index = 0;  // buffers taken (played or filled) so far
    QueueCallback queue_cb = nullptr;
    void* queue_ctx = nullptr;
    std::shared_ptr<audio::Stream> stream;

    // Players.
    SLuint32 play_state = kPlayStopped;
    PlayCallback play_cb = nullptr;
    void* play_ctx = nullptr;
    SLuint32 event_mask = 0;
    SLmillibel level = 0;
    bool mute = false;
    int stream_type = 3;  // SL_ANDROID_STREAM_MEDIA
    // File players: the file's bytes until Realize decodes them.
    std::vector<uint8_t> file;
    std::shared_ptr<const audio::Clip> clip;
    std::shared_ptr<audio::Voice> voice;
    bool loop = false;
    bool at_end = false;
    SLmillisecond start_ms = 0;  // where the next voice starts

    // Recorders.
    std::shared_ptr<audio::Capture> capture;
    SLuint32 record_state = kRecordStopped;
    bool capturing = false;
    uint64_t frames_recorded = 0;

    std::mutex mu;
    std::condition_variable cv;
    std::thread feeder;
    bool quit = false;
    bool delete_on_exit = false;  // destroyed from its own callback: the feeder frees it
};

float gain(const Object* o) { return o->mute ? 0.0f : static_cast<float>(std::pow(10.0, o->level / 2000.0)); }

void apply_volume(Object* o) {
    const float g = gain(o);
    if (o->stream) audio::stream_set_volume(o->stream.get(), g, g);
    if (o->voice) audio::voice_set_volume(o->voice.get(), g, g);
}

void release_resources(Object* o) {
    if (o->stream) audio::stream_release(o->stream);
    if (o->voice) audio::voice_stop(o->voice.get());
    if (o->capture) audio::capture_stop(o->capture.get());
}

// The end of a feeder loop: true when the object was destroyed from its own
// callback, and the feeder has freed it.
bool feeder_exits(Object* o, std::unique_lock<std::mutex>& lock) {
    if (!o->delete_on_exit) return false;
    lock.unlock();
    release_resources(o);
    delete o;
    return true;
}

// ---- PCM conversion ---------------------------------------------------------------------

int32_t s24(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) << 8 | static_cast<uint32_t>(p[1]) << 16 |
                                static_cast<uint32_t>(p[2]) << 24) >> 8;
}

// Writes what the stream takes now of `bytes` of the player's PCM; returns the
// bytes taken. 8-bit, 16-bit and float go in as they are; 24- and 32-bit
// integers as float.
size_t write_pcm(Object* o, const uint8_t* p, size_t bytes) {
    const size_t bps = bytes_per_sample(o->pcm);
    size_t samples = bytes / bps;
    switch (o->pcm) {
        case Pcm::kU8: return audio::stream_write(o->stream.get(), p, samples, 3, false);
        case Pcm::kS16: return audio::stream_write(o->stream.get(), p, samples, 2, false) * 2;
        case Pcm::kFloat: return audio::stream_write(o->stream.get(), p, samples, 4, false) * 4;
        default: break;
    }
    const size_t chunk = 4096 - 4096 % static_cast<size_t>(o->channels);
    samples = std::min(samples, chunk);
    float f[4096];
    for (size_t i = 0; i < samples; ++i) {
        const uint8_t* s = p + i * bps;
        if (o->pcm == Pcm::kS24) {
            f[i] = static_cast<float>(s24(s)) / 8388608.0f;
        } else {
            int32_t v;
            std::memcpy(&v, s, 4);
            f[i] = static_cast<float>(v) / 2147483648.0f;
        }
    }
    return audio::stream_write(o->stream.get(), f, samples, 4, false) * bps;
}

// Captured float samples into the recorder's PCM at `out`.
void store_pcm(Pcm pcm, const float* in, size_t samples, uint8_t* out) {
    for (size_t i = 0; i < samples; ++i) {
        const float v = std::clamp(in[i], -1.0f, 1.0f);
        switch (pcm) {
            case Pcm::kU8: out[i] = static_cast<uint8_t>(std::lround(v * 127.0f) + 128); break;
            case Pcm::kS16: {
                const int16_t s = static_cast<int16_t>(std::lround(v * 32767.0f));
                std::memcpy(out + i * 2, &s, 2);
                break;
            }
            case Pcm::kS24: {
                const int32_t s = static_cast<int32_t>(std::lround(v * 8388607.0f));
                out[i * 3] = static_cast<uint8_t>(s);
                out[i * 3 + 1] = static_cast<uint8_t>(s >> 8);
                out[i * 3 + 2] = static_cast<uint8_t>(s >> 16);
                break;
            }
            case Pcm::kS32: {
                const int32_t s = static_cast<int32_t>(std::llround(static_cast<double>(v) * 2147483647.0));
                std::memcpy(out + i * 4, &s, 4);
                break;
            }
            case Pcm::kFloat: std::memcpy(out + i * 4, &v, 4); break;
        }
    }
}

// The PCM format of a data source or sink; false if it is not one we play.
bool pcm_format(const void* format, Pcm* pcm, int* channels, uint32_t* rate) {
    const auto* f = static_cast<const FormatPcm*>(format);
    if (!f || (f->format_type != kFormatPcm && f->format_type != kFormatPcmEx)) return false;
    if (f->num_channels == 0 || f->samples_per_sec == 0) return false;
    if (f->container_size != 0 && f->container_size != f->bits_per_sample) return false;
    if (f->endianness != 0 && f->endianness != kByteOrderLittleEndian) return false;
    const bool is_float = f->format_type == kFormatPcmEx &&
                          static_cast<const FormatPcmEx*>(format)->representation == kPcmRepresentationFloat;
    switch (f->bits_per_sample) {
        case 8: *pcm = Pcm::kU8; break;
        case 16: *pcm = Pcm::kS16; break;
        case 24: *pcm = Pcm::kS24; break;
        case 32: *pcm = is_float ? Pcm::kFloat : Pcm::kS32; break;
        default: return false;
    }
    *channels = static_cast<int>(f->num_channels);
    // SLDataFormat_PCM's rate is in milliHz; SLAndroidDataFormat_PCM_EX's too.
    *rate = f->samples_per_sec / 1000;
    return *rate > 0;
}

// ---- the feeders ------------------------------------------------------------------------

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// A buffer-queue player's: copies queued buffers into the stream, calls the
// queue's callback as each is taken.
void feed_player(Object* o) {
    for (;;) {
        QueueCallback cb = nullptr;
        void* ctx = nullptr;
        bool full = false;
        {
            std::unique_lock<std::mutex> lock(o->mu);
            o->cv.wait_for(lock, std::chrono::milliseconds(2),
                           [o] { return o->quit || (o->play_state == kPlayPlaying && !o->queue.empty()); });
            if (o->quit) {
                feeder_exits(o, lock);
                return;
            }
            if (o->play_state != kPlayPlaying || o->queue.empty()) continue;
            Object::Buffer& b = o->queue.front();
            const size_t n = write_pcm(o, b.data + b.done, b.size - b.done);
            b.done += n;
            if (b.done >= b.size) {
                o->queue.pop_front();
                ++o->queue_index;
                cb = o->queue_cb;
                ctx = o->queue_ctx;
            } else if (n == 0) {
                full = true;
            }
        }
        if (cb) {
            cb(&o->queue_itf, ctx);
        } else if (full) {
            sleep_ms(2);  // the stream is full: the mixer drains it as it plays
        }
    }
}

// A recorder's: copies what was captured into the queued buffers, calls the
// queue's callback as each is full.
void feed_recorder(Object* o) {
    std::vector<float> f(4096);
    std::vector<uint8_t> pcm;
    for (;;) {
        QueueCallback cb = nullptr;
        void* ctx = nullptr;
        bool empty = false;
        {
            std::unique_lock<std::mutex> lock(o->mu);
            o->cv.wait_for(lock, std::chrono::milliseconds(10), [o] {
                return o->quit || (o->record_state == kRecordRecording && o->capturing && !o->queue.empty());
            });
            if (o->quit) {
                feeder_exits(o, lock);
                return;
            }
            if (o->record_state != kRecordRecording || !o->capturing || o->queue.empty()) continue;
            Object::Buffer& b = o->queue.front();
            const size_t bps = bytes_per_sample(o->pcm);
            const size_t chunk = f.size() - f.size() % static_cast<size_t>(o->channels);
            const size_t want = std::min((b.size - b.done) / bps, chunk);
            const size_t got = audio::capture_read(o->capture.get(), f.data(), want, false);
            store_pcm(o->pcm, f.data(), got, b.data + b.done);
            b.done += got * bps;
            o->frames_recorded += got / static_cast<size_t>(o->channels);
            if (b.done >= b.size) {
                o->queue.pop_front();
                ++o->queue_index;
                cb = o->queue_cb;
                ctx = o->queue_ctx;
            } else if (got == 0) {
                empty = true;
            }
        }
        if (cb) {
            cb(&o->queue_itf, ctx);
        } else if (empty) {
            sleep_ms(5);
        }
    }
}

// A file player's: notices the voice has played to its end. Wilhelm then
// pauses the player at the end and reports SL_PLAYEVENT_HEADATEND.
void watch_file_player(Object* o) {
    for (;;) {
        PlayCallback cb = nullptr;
        void* ctx = nullptr;
        {
            std::unique_lock<std::mutex> lock(o->mu);
            o->cv.wait_for(lock, std::chrono::milliseconds(10), [o] { return o->quit; });
            if (o->quit) {
                feeder_exits(o, lock);
                return;
            }
            if (o->play_state != kPlayPlaying || !o->voice || !audio::voice_finished(o->voice.get())) continue;
            o->voice.reset();
            o->at_end = true;
            o->play_state = kPlayPaused;
            if (o->event_mask & kPlayEventHeadAtEnd) {
                cb = o->play_cb;
                ctx = o->play_ctx;
            }
        }
        if (cb) cb(&o->play_itf, ctx, kPlayEventHeadAtEnd);
    }
}

// ---- RECORD_AUDIO -----------------------------------------------------------------------

// Whether the app may record, by the rule AudioRecord follows
// (AudioInputBridge.open): SpecialAccessStore.holds("android.permission.
// RECORD_AUDIO") is not FALSE. Asked of the runtime's JVM from the calling
// thread, attached if it is not a Java thread. Without a JVM (a test of this
// file alone) nothing is modelled, as when holds answers null.
bool record_audio_held() {
    using GetVms = jint(JNICALL*)(JavaVM**, jsize, jsize*);
    HMODULE jvm = GetModuleHandleW(L"jvm.dll");
    auto get_vms = jvm ? reinterpret_cast<GetVms>(GetProcAddress(jvm, "JNI_GetCreatedJavaVMs")) : nullptr;
    JavaVM* vm = nullptr;
    jsize count = 0;
    if (!get_vms || get_vms(&vm, 1, &count) != JNI_OK || count < 1) return true;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8) != JNI_OK) {
        JavaVMAttachArgs args{JNI_VERSION_1_8, const_cast<char*>("opensles"), nullptr};
        if (vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &args) != JNI_OK) return false;
    }
    if (env->PushLocalFrame(8) != JNI_OK) return false;
    bool held = false;
    jclass cls = env->FindClass("com/macapk/loader/SpecialAccessStore");
    jmethodID holds = cls ? env->GetStaticMethodID(cls, "holds", "(Ljava/lang/String;)Ljava/lang/Boolean;") : nullptr;
    jclass boolean_cls = env->FindClass("java/lang/Boolean");
    jmethodID value = boolean_cls ? env->GetMethodID(boolean_cls, "booleanValue", "()Z") : nullptr;
    if (holds && value) {
        jobject r = env->CallStaticObjectMethod(cls, holds, env->NewStringUTF("android.permission.RECORD_AUDIO"));
        held = !env->ExceptionCheck() && (!r || env->CallBooleanMethod(r, value));
    }
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        held = false;
    }
    env->PopLocalFrame(nullptr);
    return held;
}

// ---- SLObjectItf ------------------------------------------------------------------------

SLresult realize(Object* o) {
    if (o->state == kObjectRealized) return kPreconditionsViolated;
    if (o->kind == Kind::kPlayer && !o->queue_player) {
        o->clip = audio::decode(o->file.data(), o->file.size());
        std::vector<uint8_t>().swap(o->file);
        if (!o->clip || o->clip->rate <= 0) {
            std::fprintf(stderr, "[opensles] Realize: cannot decode the player's file -> SL_RESULT_CONTENT_UNSUPPORTED\n");
            return kContentUnsupported;
        }
    }
    if (o->kind == Kind::kRecorder) {
        // Realization can come long after creation: the permission may have
        // been revoked in between, before the device is opened.
        if (!record_audio_held()) {
            std::fprintf(stderr, "[opensles] recorder Realize: RECORD_AUDIO is not held\n");
            return kContentUnsupported;
        }
        // Opened once to prove the device opens (as AudioRecord's open is),
        // then stopped until SL_RECORDSTATE_RECORDING.
        o->capture = audio::capture_create(static_cast<int>(o->rate), o->channels, static_cast<int>(o->rate / 2));
        if (!audio::microphone_allowed() || !o->capture || !audio::capture_start(o->capture.get())) {
            std::fprintf(stderr, "[opensles] recorder Realize: no capture device (or Windows' microphone "
                                 "access is off) -> SL_RESULT_CONTENT_UNSUPPORTED\n");
            o->capture.reset();
            return kContentUnsupported;
        }
        audio::capture_stop(o->capture.get());
    }
    o->state = kObjectRealized;
    if (o->kind == Kind::kPlayer && o->queue_player) o->feeder = std::thread([o] { feed_player(o); });
    if (o->kind == Kind::kPlayer && !o->queue_player) o->feeder = std::thread([o] { watch_file_player(o); });
    if (o->kind == Kind::kRecorder) o->feeder = std::thread([o] { feed_recorder(o); });
    return kSuccess;
}

// Realize is synchronous here; an asynchronous one still reports its end
// through the object's callback, as wilhelm's does.
WINAPK_GUEST_ABI SLresult Obj_Realize(const void* self, SLboolean async) {
    Object* o = object_of(self);
    const SLresult r = realize(o);
    if (async && o->object_cb) {
        o->object_cb(self, o->object_ctx, kObjectEventAsyncTermination, r, o->state, nullptr);
    }
    return r;
}
WINAPK_GUEST_ABI SLresult Obj_Resume(const void*, SLboolean) { return kPreconditionsViolated; }
WINAPK_GUEST_ABI SLresult Obj_GetState(const void* self, SLuint32* state) {
    if (!state) return kParameterInvalid;
    *state = object_of(self)->state;
    return kSuccess;
}

WINAPK_GUEST_ABI SLresult Obj_GetInterface(const void* self, SLInterfaceID id, void* out) {
    Object* o = object_of(self);
    if (!out) return kParameterInvalid;
    *static_cast<const void**>(out) = nullptr;
    if (!id) return kParameterInvalid;
    const Interface* info = find_interface(o->kind, o->queue_player, id);
    if (!info) return kFeatureUnsupported;
    if (o->state != kObjectRealized && !info->prerealize) return kPreconditionsViolated;
    if (!(o->exposed & slot_bit(info->slot))) return kFeatureUnsupported;
    ItfSlot* slot = nullptr;
    switch (info->slot) {
        case Slot::kObject: slot = &o->object_itf; break;
        case Slot::kEngine: slot = &o->engine_itf; break;
        case Slot::kPlay: slot = &o->play_itf; break;
        case Slot::kQueue: slot = &o->queue_itf; break;
        case Slot::kVolume: slot = &o->volume_itf; break;
        case Slot::kSeek: slot = &o->seek_itf; break;
        case Slot::kConfig: slot = &o->config_itf; break;
        case Slot::kPrefetch: slot = &o->prefetch_itf; break;
        case Slot::kRecord: slot = &o->record_itf; break;
    }
    *static_cast<const void**>(out) = slot;
    return kSuccess;
}

WINAPK_GUEST_ABI SLresult Obj_RegisterCallback(const void* self, void* cb, const void* ctx) {
    Object* o = object_of(self);
    std::lock_guard<std::mutex> lock(o->mu);
    o->object_cb = guest_fn<ObjectCallback>(cb);
    o->object_ctx = ctx;
    return kSuccess;
}
WINAPK_GUEST_ABI void Obj_AbortAsyncOperation(const void*) {}

WINAPK_GUEST_ABI void Obj_Destroy(const void* self) {
    Object* o = object_of(self);
    {
        std::lock_guard<std::mutex> lock(o->mu);
        o->quit = true;
        // From inside one of its own callbacks: the feeder frees it on return.
        if (o->feeder.joinable() && o->feeder.get_id() == std::this_thread::get_id()) {
            o->delete_on_exit = true;
            o->feeder.detach();
            return;
        }
    }
    o->cv.notify_all();
    if (o->feeder.joinable()) o->feeder.join();
    release_resources(o);
    delete o;
}

WINAPK_GUEST_ABI SLresult Obj_SetPriority(const void*, SLint32, SLboolean) { return kSuccess; }
WINAPK_GUEST_ABI SLresult Obj_GetPriority(const void*, SLint32* priority, SLboolean* preemptable) {
    if (!priority || !preemptable) return kParameterInvalid;
    *priority = 0;  // SL_PRIORITY_NORMAL, wilhelm's default
    *preemptable = 0;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Obj_SetLossOfControlInterfaces(const void*, SLint32, const SLInterfaceID*, SLboolean) {
    return kSuccess;
}

const void* const kObjectVtable[] = {
    reinterpret_cast<const void*>(&Obj_Realize),
    reinterpret_cast<const void*>(&Obj_Resume),
    reinterpret_cast<const void*>(&Obj_GetState),
    reinterpret_cast<const void*>(&Obj_GetInterface),
    reinterpret_cast<const void*>(&Obj_RegisterCallback),
    reinterpret_cast<const void*>(&Obj_AbortAsyncOperation),
    reinterpret_cast<const void*>(&Obj_Destroy),
    reinterpret_cast<const void*>(&Obj_SetPriority),
    reinterpret_cast<const void*>(&Obj_GetPriority),
    reinterpret_cast<const void*>(&Obj_SetLossOfControlInterfaces),
};

// ---- SLPlayItf --------------------------------------------------------------------------

// A file player's voice, started (paused) at start_ms when there is none.
void ensure_voice(Object* o) {
    if (o->voice && !audio::voice_finished(o->voice.get())) return;
    const float g = gain(o);
    o->voice = audio::voice_start(o->clip, g, g, o->loop ? -1 : 0, 1.0f, true);
    if (o->start_ms) audio::voice_seek(o->voice.get(), o->start_ms / 1000.0);
    o->start_ms = 0;
}

SLmillisecond clip_duration(const Object* o) {
    if (!o->clip || o->clip->rate <= 0) return kTimeUnknown;
    return static_cast<SLmillisecond>(static_cast<uint64_t>(o->clip->frames()) * 1000 / o->clip->rate);
}

WINAPK_GUEST_ABI SLresult Play_SetPlayState(const void* self, SLuint32 state) {
    Object* o = object_of(self);
    if (state < kPlayStopped || state > kPlayPlaying) return kParameterInvalid;
    std::lock_guard<std::mutex> lock(o->mu);
    if (o->queue_player) {
        if (state == kPlayPlaying) {
            audio::stream_play(o->stream.get());
        } else if (state == kPlayPaused) {
            audio::stream_pause(o->stream.get());
        } else {
            // Stopped: the queue is cleared (wilhelm) and the track starts over.
            audio::stream_stop(o->stream.get());
            audio::stream_flush(o->stream.get());
            o->queue.clear();
        }
    } else if (o->clip) {
        if (state == kPlayPlaying) {
            if (o->at_end) o->start_ms = 0;
            o->at_end = false;
            ensure_voice(o);
            audio::voice_set_paused(o->voice.get(), false);
        } else if (state == kPlayPaused) {
            if (o->voice) audio::voice_set_paused(o->voice.get(), true);
        } else {
            if (o->voice) audio::voice_stop(o->voice.get());
            o->voice.reset();
            o->start_ms = 0;
            o->at_end = false;
        }
    }
    o->play_state = state;
    o->cv.notify_all();
    return kSuccess;
}

WINAPK_GUEST_ABI SLresult Play_GetPlayState(const void* self, SLuint32* state) {
    if (!state) return kParameterInvalid;
    Object* o = object_of(self);
    std::lock_guard<std::mutex> lock(o->mu);
    *state = o->play_state;
    return kSuccess;
}

WINAPK_GUEST_ABI SLresult Play_GetDuration(const void* self, SLmillisecond* msec) {
    if (!msec) return kParameterInvalid;
    *msec = clip_duration(object_of(self));
    return kSuccess;
}

WINAPK_GUEST_ABI SLresult Play_GetPosition(const void* self, SLmillisecond* msec) {
    if (!msec) return kParameterInvalid;
    Object* o = object_of(self);
    std::lock_guard<std::mutex> lock(o->mu);
    if (o->queue_player) {
        const int64_t frames = audio::stream_position(o->stream.get());
        *msec = o->rate ? static_cast<SLmillisecond>(static_cast<uint64_t>(std::max<int64_t>(frames, 0)) * 1000 / o->rate)
                        : 0;
    } else if (o->at_end) {
        *msec = clip_duration(o);
    } else if (o->voice && !audio::voice_finished(o->voice.get())) {
        *msec = static_cast<SLmillisecond>(audio::voice_position(o->voice.get()) * 1000.0);
    } else {
        *msec = o->start_ms;
    }
    return kSuccess;
}

WINAPK_GUEST_ABI SLresult Play_RegisterCallback(const void* self, void* cb, void* ctx) {
    Object* o = object_of(self);
    std::lock_guard<std::mutex> lock(o->mu);
    o->play_cb = guest_fn<PlayCallback>(cb);
    o->play_ctx = ctx;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Play_SetCallbackEventsMask(const void* self, SLuint32 mask) {
    Object* o = object_of(self);
    std::lock_guard<std::mutex> lock(o->mu);
    o->event_mask = mask;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Play_GetCallbackEventsMask(const void* self, SLuint32* mask) {
    if (!mask) return kParameterInvalid;
    *mask = object_of(self)->event_mask;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Play_SetMarkerPosition(const void*, SLmillisecond) { return kSuccess; }
WINAPK_GUEST_ABI SLresult Play_ClearMarkerPosition(const void*) { return kSuccess; }
WINAPK_GUEST_ABI SLresult Play_GetMarkerPosition(const void*, SLmillisecond* m) {
    if (m) *m = 0;
    return kPreconditionsViolated;  // no marker set
}
WINAPK_GUEST_ABI SLresult Play_SetPositionUpdatePeriod(const void*, SLmillisecond) { return kSuccess; }
WINAPK_GUEST_ABI SLresult Play_GetPositionUpdatePeriod(const void*, SLmillisecond* m) {
    if (!m) return kParameterInvalid;
    *m = 1000;
    return kSuccess;
}

const void* const kPlayVtable[] = {
    reinterpret_cast<const void*>(&Play_SetPlayState),
    reinterpret_cast<const void*>(&Play_GetPlayState),
    reinterpret_cast<const void*>(&Play_GetDuration),
    reinterpret_cast<const void*>(&Play_GetPosition),
    reinterpret_cast<const void*>(&Play_RegisterCallback),
    reinterpret_cast<const void*>(&Play_SetCallbackEventsMask),
    reinterpret_cast<const void*>(&Play_GetCallbackEventsMask),
    reinterpret_cast<const void*>(&Play_SetMarkerPosition),
    reinterpret_cast<const void*>(&Play_ClearMarkerPosition),
    reinterpret_cast<const void*>(&Play_GetMarkerPosition),
    reinterpret_cast<const void*>(&Play_SetPositionUpdatePeriod),
    reinterpret_cast<const void*>(&Play_GetPositionUpdatePeriod),
};

// ---- SLRecordItf ------------------------------------------------------------------------

WINAPK_GUEST_ABI SLresult Rec_SetRecordState(const void* self, SLuint32 state) {
    Object* o = object_of(self);
    if (state != kRecordStopped && state != kRecordPaused && state != kRecordRecording) return kParameterInvalid;
    std::lock_guard<std::mutex> lock(o->mu);
    if (!o->capture) return kPreconditionsViolated;
    if (state == kRecordRecording) {
        o->capturing = audio::capture_start(o->capture.get());
        if (!o->capturing) std::fprintf(stderr, "[opensles] SetRecordState(RECORDING): the capture did not start\n");
    } else {
        audio::capture_stop(o->capture.get());
        o->capturing = false;
        if (state == kRecordStopped) o->frames_recorded = 0;  // wilhelm's AudioRecord is stopped
    }
    o->record_state = state;
    o->cv.notify_all();
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Rec_GetRecordState(const void* self, SLuint32* state) {
    if (!state) return kParameterInvalid;
    Object* o = object_of(self);
    std::lock_guard<std::mutex> lock(o->mu);
    *state = o->record_state;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Rec_SetDurationLimit(const void*, SLmillisecond) { return kSuccess; }
WINAPK_GUEST_ABI SLresult Rec_GetPosition(const void* self, SLmillisecond* msec) {
    if (!msec) return kParameterInvalid;
    Object* o = object_of(self);
    std::lock_guard<std::mutex> lock(o->mu);
    *msec = o->rate ? static_cast<SLmillisecond>(o->frames_recorded * 1000 / o->rate) : 0;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Rec_RegisterCallback(const void*, void*, void*) { return kSuccess; }
WINAPK_GUEST_ABI SLresult Rec_SetCallbackEventsMask(const void* self, SLuint32 mask) {
    object_of(self)->event_mask = mask;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Rec_GetCallbackEventsMask(const void* self, SLuint32* mask) {
    if (!mask) return kParameterInvalid;
    *mask = object_of(self)->event_mask;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Rec_SetMarkerPosition(const void*, SLmillisecond) { return kSuccess; }
WINAPK_GUEST_ABI SLresult Rec_ClearMarkerPosition(const void*) { return kSuccess; }
WINAPK_GUEST_ABI SLresult Rec_GetMarkerPosition(const void*, SLmillisecond* m) {
    if (m) *m = 0;
    return kPreconditionsViolated;
}
WINAPK_GUEST_ABI SLresult Rec_SetPositionUpdatePeriod(const void*, SLmillisecond) { return kSuccess; }
WINAPK_GUEST_ABI SLresult Rec_GetPositionUpdatePeriod(const void*, SLmillisecond* m) {
    if (!m) return kParameterInvalid;
    *m = 1000;
    return kSuccess;
}

const void* const kRecordVtable[] = {
    reinterpret_cast<const void*>(&Rec_SetRecordState),
    reinterpret_cast<const void*>(&Rec_GetRecordState),
    reinterpret_cast<const void*>(&Rec_SetDurationLimit),
    reinterpret_cast<const void*>(&Rec_GetPosition),
    reinterpret_cast<const void*>(&Rec_RegisterCallback),
    reinterpret_cast<const void*>(&Rec_SetCallbackEventsMask),
    reinterpret_cast<const void*>(&Rec_GetCallbackEventsMask),
    reinterpret_cast<const void*>(&Rec_SetMarkerPosition),
    reinterpret_cast<const void*>(&Rec_ClearMarkerPosition),
    reinterpret_cast<const void*>(&Rec_GetMarkerPosition),
    reinterpret_cast<const void*>(&Rec_SetPositionUpdatePeriod),
    reinterpret_cast<const void*>(&Rec_GetPositionUpdatePeriod),
};

// ---- SLBufferQueueItf / SLAndroidSimpleBufferQueueItf (one vtable) ----------------------

WINAPK_GUEST_ABI SLresult Queue_Enqueue(const void* self, const void* buffer, SLuint32 size) {
    Object* o = object_of(self);
    if (!buffer || size == 0) return kParameterInvalid;
    std::lock_guard<std::mutex> lock(o->mu);
    if (o->queue.size() >= o->queue_capacity) return kBufferInsufficient;
    // Whole frames only: the stream and the capture move frames.
    o->queue.push_back({static_cast<uint8_t*>(const_cast<void*>(buffer)), size / o->frame_size * o->frame_size, 0});
    o->cv.notify_all();
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Queue_Clear(const void* self) {
    Object* o = object_of(self);
    std::lock_guard<std::mutex> lock(o->mu);
    o->queue.clear();
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Queue_GetState(const void* self, SLuint32* state /* {count, index} */) {
    Object* o = object_of(self);
    if (!state) return kParameterInvalid;
    std::lock_guard<std::mutex> lock(o->mu);
    state[0] = static_cast<SLuint32>(o->queue.size());
    state[1] = o->queue_index;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Queue_RegisterCallback(const void* self, void* cb, void* ctx) {
    Object* o = object_of(self);
    std::lock_guard<std::mutex> lock(o->mu);
    o->queue_cb = guest_fn<QueueCallback>(cb);
    o->queue_ctx = ctx;
    return kSuccess;
}

const void* const kQueueVtable[] = {
    reinterpret_cast<const void*>(&Queue_Enqueue),
    reinterpret_cast<const void*>(&Queue_Clear),
    reinterpret_cast<const void*>(&Queue_GetState),
    reinterpret_cast<const void*>(&Queue_RegisterCallback),
};

// ---- SLVolumeItf ------------------------------------------------------------------------

// SLmillibel and SLpermille are 16-bit: AAPCS64 leaves the upper bits of a
// narrow argument unspecified, so the value is taken from the low 16.
WINAPK_GUEST_ABI SLresult Vol_SetVolumeLevel(const void* self, SLint32 raw) {
    Object* o = object_of(self);
    std::lock_guard<std::mutex> lock(o->mu);
    o->level = std::min<SLmillibel>(static_cast<SLmillibel>(raw), 0);
    apply_volume(o);
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Vol_GetVolumeLevel(const void* self, SLmillibel* level) {
    if (!level) return kParameterInvalid;
    *level = object_of(self)->level;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Vol_GetMaxVolumeLevel(const void*, SLmillibel* level) {
    if (!level) return kParameterInvalid;
    *level = 0;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Vol_SetMute(const void* self, SLboolean mute) {
    Object* o = object_of(self);
    std::lock_guard<std::mutex> lock(o->mu);
    o->mute = mute != 0;
    apply_volume(o);
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Vol_GetMute(const void* self, SLboolean* mute) {
    if (!mute) return kParameterInvalid;
    *mute = object_of(self)->mute ? 1 : 0;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Vol_EnableStereoPosition(const void*, SLboolean) { return kSuccess; }
WINAPK_GUEST_ABI SLresult Vol_IsEnabledStereoPosition(const void*, SLboolean* enabled) {
    if (!enabled) return kParameterInvalid;
    *enabled = 0;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Vol_SetStereoPosition(const void*, SLint32) { return kSuccess; }
WINAPK_GUEST_ABI SLresult Vol_GetStereoPosition(const void*, SLpermille* position) {
    if (!position) return kParameterInvalid;
    *position = 0;
    return kSuccess;
}

const void* const kVolumeVtable[] = {
    reinterpret_cast<const void*>(&Vol_SetVolumeLevel),
    reinterpret_cast<const void*>(&Vol_GetVolumeLevel),
    reinterpret_cast<const void*>(&Vol_GetMaxVolumeLevel),
    reinterpret_cast<const void*>(&Vol_SetMute),
    reinterpret_cast<const void*>(&Vol_GetMute),
    reinterpret_cast<const void*>(&Vol_EnableStereoPosition),
    reinterpret_cast<const void*>(&Vol_IsEnabledStereoPosition),
    reinterpret_cast<const void*>(&Vol_SetStereoPosition),
    reinterpret_cast<const void*>(&Vol_GetStereoPosition),
};

// ---- SLSeekItf (file players) -----------------------------------------------------------

WINAPK_GUEST_ABI SLresult Seek_SetPosition(const void* self, SLmillisecond pos, SLuint32) {
    Object* o = object_of(self);
    std::lock_guard<std::mutex> lock(o->mu);
    if (!o->clip) return kPreconditionsViolated;
    o->at_end = false;
    pos = std::min(pos, clip_duration(o));
    if (o->voice && !audio::voice_finished(o->voice.get())) {
        audio::voice_seek(o->voice.get(), pos / 1000.0);
    } else {
        o->start_ms = pos;
    }
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Seek_SetLoop(const void* self, SLboolean loop, SLmillisecond, SLmillisecond) {
    Object* o = object_of(self);
    std::lock_guard<std::mutex> lock(o->mu);
    o->loop = loop != 0;
    if (o->voice) audio::voice_set_loop(o->voice.get(), o->loop ? -1 : 0);
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Seek_GetLoop(const void* self, SLboolean* loop, SLmillisecond* start, SLmillisecond* end) {
    Object* o = object_of(self);
    if (!loop || !start || !end) return kParameterInvalid;
    *loop = o->loop ? 1 : 0;
    *start = 0;
    *end = kTimeUnknown;  // SL_TIME_UNKNOWN: the end of the content
    return kSuccess;
}

const void* const kSeekVtable[] = {
    reinterpret_cast<const void*>(&Seek_SetPosition),
    reinterpret_cast<const void*>(&Seek_SetLoop),
    reinterpret_cast<const void*>(&Seek_GetLoop),
};

// ---- SLAndroidConfigurationItf ----------------------------------------------------------

WINAPK_GUEST_ABI SLresult Config_SetConfiguration(const void* self, const char* key, const void* value, SLuint32 size) {
    Object* o = object_of(self);
    if (!key || !value) return kParameterInvalid;
    // "androidPlaybackStreamType": SL_ANDROID_STREAM_* (AudioSystem's stream
    // types). Stored: every stream plays through the one mixer.
    if (std::strcmp(key, "androidPlaybackStreamType") == 0 && size >= sizeof(SLint32)) {
        o->stream_type = *static_cast<const SLint32*>(value);
    }
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Config_GetConfiguration(const void* self, const char* key, SLuint32* size, void* value) {
    Object* o = object_of(self);
    if (!key || !size) return kParameterInvalid;
    if (std::strcmp(key, "androidPlaybackStreamType") != 0) return kParameterInvalid;
    if (value && *size >= sizeof(SLint32)) *static_cast<SLint32*>(value) = o->stream_type;
    *size = sizeof(SLint32);
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Config_AcquireJavaProxy(const void*, SLuint32, void**) { return kFeatureUnsupported; }
WINAPK_GUEST_ABI SLresult Config_ReleaseJavaProxy(const void*, SLuint32) { return kFeatureUnsupported; }

const void* const kConfigVtable[] = {
    reinterpret_cast<const void*>(&Config_SetConfiguration),
    reinterpret_cast<const void*>(&Config_GetConfiguration),
    reinterpret_cast<const void*>(&Config_AcquireJavaProxy),
    reinterpret_cast<const void*>(&Config_ReleaseJavaProxy),
};

// ---- SLPrefetchStatusItf (file players: the whole file is in memory) --------------------

WINAPK_GUEST_ABI SLresult Prefetch_GetPrefetchStatus(const void*, SLuint32* status) {
    if (!status) return kParameterInvalid;
    *status = kPrefetchStatusSufficientData;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Prefetch_GetFillLevel(const void*, SLpermille* level) {
    if (!level) return kParameterInvalid;
    *level = 1000;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Prefetch_RegisterCallback(const void*, void*, void*) { return kSuccess; }
WINAPK_GUEST_ABI SLresult Prefetch_SetCallbackEventsMask(const void*, SLuint32) { return kSuccess; }
WINAPK_GUEST_ABI SLresult Prefetch_GetCallbackEventsMask(const void*, SLuint32* mask) {
    if (!mask) return kParameterInvalid;
    *mask = 0;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Prefetch_SetFillUpdatePeriod(const void*, SLint32) { return kSuccess; }
WINAPK_GUEST_ABI SLresult Prefetch_GetFillUpdatePeriod(const void*, SLpermille* period) {
    if (!period) return kParameterInvalid;
    *period = 100;
    return kSuccess;
}

const void* const kPrefetchVtable[] = {
    reinterpret_cast<const void*>(&Prefetch_GetPrefetchStatus),
    reinterpret_cast<const void*>(&Prefetch_GetFillLevel),
    reinterpret_cast<const void*>(&Prefetch_RegisterCallback),
    reinterpret_cast<const void*>(&Prefetch_SetCallbackEventsMask),
    reinterpret_cast<const void*>(&Prefetch_GetCallbackEventsMask),
    reinterpret_cast<const void*>(&Prefetch_SetFillUpdatePeriod),
    reinterpret_cast<const void*>(&Prefetch_GetFillUpdatePeriod),
};

// ---- SLEngineItf ------------------------------------------------------------------------

Object* new_object(Kind kind, uint32_t exposed);

Object* new_object_with(Kind kind, uint32_t exposed, const void* const* engine_vtable) {
    auto* o = new Object();
    o->kind = kind;
    o->exposed = exposed;
    o->object_itf = {kObjectVtable, o};
    o->engine_itf = {engine_vtable, o};
    o->play_itf = {kPlayVtable, o};
    o->queue_itf = {kQueueVtable, o};
    o->volume_itf = {kVolumeVtable, o};
    o->seek_itf = {kSeekVtable, o};
    o->config_itf = {kConfigVtable, o};
    o->prefetch_itf = {kPrefetchVtable, o};
    o->record_itf = {kRecordVtable, o};
    return o;
}

WINAPK_GUEST_ABI SLresult Eng_Unsupported() { return kFeatureUnsupported; }

WINAPK_GUEST_ABI SLresult Eng_CreateOutputMix(const void*, const void** out, SLuint32 count, const SLInterfaceID* ids,
                                              const SLboolean* required) {
    if (!out) return kParameterInvalid;
    *out = nullptr;
    uint32_t exposed = 0;
    const SLresult r = check_interfaces(Kind::kOutputMix, false, count, ids, required, &exposed);
    if (r != kSuccess) return r;
    *out = &new_object(Kind::kOutputMix, exposed)->object_itf;
    return kSuccess;
}

// The bytes of `length` bytes of guest descriptor `fd` from `offset` (to the
// end when length is negative), through the shim's pread64, which knows the
// guest's descriptors.
bool read_guest_fd(int fd, int64_t offset, int64_t length, std::vector<uint8_t>* out) {
    using Pread = int64_t(WINAPK_GUEST_ABI*)(int, void*, size_t, int64_t);
    auto pread = reinterpret_cast<Pread>(mini_bionic_resolve("pread64"));
    if (!pread || fd < 0 || offset < 0) return false;
    constexpr int64_t kMax = int64_t{512} << 20;  // a file player's whole file is decoded into memory
    const int64_t want = length < 0 ? kMax : std::min(length, kMax);
    out->clear();
    uint8_t chunk[64 * 1024];
    while (static_cast<int64_t>(out->size()) < want) {
        const size_t n = static_cast<size_t>(std::min<int64_t>(sizeof(chunk), want - static_cast<int64_t>(out->size())));
        const int64_t got = pread(fd, chunk, n, offset + static_cast<int64_t>(out->size()));
        if (got < 0) return false;
        if (got == 0) break;
        out->insert(out->end(), chunk, chunk + got);
    }
    return !out->empty();
}

// A URI's file: a path or a file: URI, by its Android name (paths.cpp).
bool read_uri(const char* uri, std::vector<uint8_t>* out) {
    if (!uri) return false;
    std::string path = uri;
    if (path.rfind("file://", 0) == 0) path = path.substr(7);
    if (path.empty() || path[0] != '/') {
        std::fprintf(stderr, "[opensles] URI player: \"%s\" is not a file (http(s) is not supported)\n", uri);
        return false;
    }
    const std::wstring host = android_host_path(path.c_str());
    HANDLE h = CreateFileW(host.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (int64_t{512} << 20);
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD done = 0;
        size_t at = 0;
        while (ok && at < out->size()) {
            ok = ReadFile(h, out->data() + at, static_cast<DWORD>(std::min<size_t>(out->size() - at, 1u << 30)), &done,
                          nullptr) &&
                 done > 0;
            at += done;
        }
    }
    CloseHandle(h);
    return ok;
}

WINAPK_GUEST_ABI SLresult Eng_CreateAudioPlayer(const void*, const void** out, const DataSource* src,
                                                const DataSink* sink, SLuint32 count, const SLInterfaceID* ids,
                                                const SLboolean* required) {
    if (!out) return kParameterInvalid;
    *out = nullptr;
    if (!src || !src->locator || !sink || !sink->locator) return kParameterInvalid;
    if (static_cast<const LocatorHeader*>(sink->locator)->type != kLocatorOutputMix) return kContentUnsupported;
    const SLuint32 type = static_cast<const LocatorHeader*>(src->locator)->type;
    const bool queue = type == kLocatorBufferQueue || type == kLocatorAndroidSimpleBufferQueue;
    uint32_t exposed = 0;
    const SLresult r = check_interfaces(Kind::kPlayer, queue, count, ids, required, &exposed);
    if (r != kSuccess) return r;
    std::unique_ptr<Object> o(new_object(Kind::kPlayer, exposed));
    if (queue) {
        if (!pcm_format(src->format, &o->pcm, &o->channels, &o->rate)) {
            std::fprintf(stderr, "[opensles] CreateAudioPlayer: a PCM format it does not play -> "
                                 "SL_RESULT_CONTENT_UNSUPPORTED\n");
            return kContentUnsupported;
        }
        o->queue_player = true;
        o->queue_capacity = std::max<SLuint32>(static_cast<const LocatorQueue*>(src->locator)->num_buffers, 1);
        o->frame_size = bytes_per_sample(o->pcm) * static_cast<size_t>(o->channels);
        // 100 ms in the stream: the app's queue holds the rest.
        const int encoding = o->pcm == Pcm::kU8 ? 3 : o->pcm == Pcm::kS16 ? 2 : 4;
        o->stream = audio::stream_create(static_cast<int>(o->rate), o->channels, encoding,
                                         static_cast<int>(std::max<uint32_t>(o->rate / 10, 512)));
        if (!o->stream) return kContentUnsupported;
        std::fprintf(stderr, "[opensles] CreateAudioPlayer: buffer queue of %u, %u Hz, %d ch, %zu-byte samples\n",
                     o->queue_capacity, o->rate, o->channels, bytes_per_sample(o->pcm));
    } else if (type == kLocatorAndroidFd) {
        const auto* l = static_cast<const LocatorAndroidFd*>(src->locator);
        if (!read_guest_fd(l->fd, l->offset, l->length, &o->file)) {
            std::fprintf(stderr, "[opensles] CreateAudioPlayer: cannot read fd %d -> SL_RESULT_CONTENT_UNSUPPORTED\n",
                         l->fd);
            return kContentUnsupported;
        }
        std::fprintf(stderr, "[opensles] CreateAudioPlayer: fd %d, %zu bytes\n", l->fd, o->file.size());
    } else if (type == kLocatorUri) {
        const char* uri = static_cast<const LocatorUri*>(src->locator)->uri;
        if (!read_uri(uri, &o->file)) return kContentUnsupported;
        std::fprintf(stderr, "[opensles] CreateAudioPlayer: %s, %zu bytes\n", uri, o->file.size());
    } else {
        std::fprintf(stderr, "[opensles] CreateAudioPlayer: data locator 0x%x -> SL_RESULT_CONTENT_UNSUPPORTED\n", type);
        return kContentUnsupported;
    }
    *out = &o.release()->object_itf;
    return kSuccess;
}

// The audio input device into an Android simple buffer queue of PCM.
WINAPK_GUEST_ABI SLresult Eng_CreateAudioRecorder(const void*, const void** out, const DataSource* src,
                                                  const DataSink* sink, SLuint32 count, const SLInterfaceID* ids,
                                                  const SLboolean* required) {
    if (!out) return kParameterInvalid;
    *out = nullptr;
    if (!src || !src->locator || !sink || !sink->locator || !sink->format) return kParameterInvalid;
    uint32_t exposed = 0;
    const SLresult r = check_interfaces(Kind::kRecorder, true, count, ids, required, &exposed);
    if (r != kSuccess) return r;
    const auto* io = static_cast<const LocatorIoDevice*>(src->locator);
    if (io->type != kLocatorIoDevice || io->device_type != kIoDeviceAudioInput ||
        io->device_id != kDefaultDeviceIdAudioInput || io->device) {
        return kParameterInvalid;
    }
    const SLuint32 sink_type = static_cast<const LocatorHeader*>(sink->locator)->type;
    if (sink_type != kLocatorAndroidSimpleBufferQueue && sink_type != kLocatorBufferQueue) return kParameterInvalid;
    const SLuint32 buffers = static_cast<const LocatorQueue*>(sink->locator)->num_buffers;
    if (buffers < 1 || buffers > 255) return kParameterInvalid;
    // Android checks the descriptor at create; only the device waits for Realize.
    const auto* f = static_cast<const FormatPcm*>(sink->format);
    if (f->format_type != kFormatPcm && f->format_type != kFormatPcmEx) return kParameterInvalid;
    if (f->num_channels == 0 || f->samples_per_sec == 0) return kParameterInvalid;
    if (f->num_channels > 8 || f->samples_per_sec < 8000000 || f->samples_per_sec > 192000000) {
        return kContentUnsupported;
    }
    std::unique_ptr<Object> o(new_object(Kind::kRecorder, exposed));
    if (!pcm_format(sink->format, &o->pcm, &o->channels, &o->rate)) return kContentUnsupported;
    if (!record_audio_held()) {
        std::fprintf(stderr, "[opensles] CreateAudioRecorder: the app does not hold android.permission.RECORD_AUDIO "
                             "-> SL_RESULT_PERMISSION_DENIED\n");
        return kPermissionDenied;
    }
    o->queue_capacity = buffers;
    o->frame_size = bytes_per_sample(o->pcm) * static_cast<size_t>(o->channels);
    std::fprintf(stderr, "[opensles] CreateAudioRecorder: buffer queue of %u, %u Hz, %d ch, %zu-byte samples\n",
                 buffers, o->rate, o->channels, bytes_per_sample(o->pcm));
    *out = &o.release()->object_itf;
    return kSuccess;
}

bool object_kind(SLuint32 id, Kind* kind) {
    switch (id) {
        case 0x1001: *kind = Kind::kEngine; return true;     // SL_OBJECTID_ENGINE
        case 0x1004: *kind = Kind::kPlayer; return true;     // SL_OBJECTID_AUDIOPLAYER
        case 0x1005: *kind = Kind::kRecorder; return true;   // SL_OBJECTID_AUDIORECORDER
        case 0x1009: *kind = Kind::kOutputMix; return true;  // SL_OBJECTID_OUTPUTMIX
        default: return false;
    }
}

SLresult query_num_interfaces(Kind kind, SLuint32* n) {
    if (!n) return kParameterInvalid;
    *n = 0;
    for (const auto& i : kInterfaces) {
        if (i.kinds & kind_bit(kind)) ++*n;
    }
    return kSuccess;
}

SLresult query_interface(Kind kind, SLuint32 index, SLInterfaceID* id) {
    if (!id) return kParameterInvalid;
    *id = nullptr;
    for (const auto& i : kInterfaces) {
        if (!(i.kinds & kind_bit(kind))) continue;
        if (index-- == 0) {
            *id = iid(i.id);
            return kSuccess;
        }
    }
    return kParameterInvalid;
}

WINAPK_GUEST_ABI SLresult Eng_QueryNumSupportedInterfaces(const void*, SLuint32 object_id, SLuint32* n) {
    Kind kind;
    if (!n) return kParameterInvalid;
    return object_kind(object_id, &kind) ? query_num_interfaces(kind, n) : kFeatureUnsupported;
}
WINAPK_GUEST_ABI SLresult Eng_QuerySupportedInterfaces(const void*, SLuint32 object_id, SLuint32 index,
                                                       SLInterfaceID* id) {
    Kind kind;
    if (!id) return kParameterInvalid;
    return object_kind(object_id, &kind) ? query_interface(kind, index, id) : kFeatureUnsupported;
}
WINAPK_GUEST_ABI SLresult Eng_QueryNumSupportedExtensions(const void*, SLuint32* n) {
    if (!n) return kParameterInvalid;
    *n = 0;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult Eng_QuerySupportedExtension(const void*, SLuint32, char*, SLint16*) {
    return kParameterInvalid;  // there are none
}
WINAPK_GUEST_ABI SLresult Eng_IsExtensionSupported(const void*, const char*, SLboolean* supported) {
    if (!supported) return kParameterInvalid;
    *supported = 0;
    return kSuccess;
}

const void* const kEngineVtable[] = {
    reinterpret_cast<const void*>(&Eng_Unsupported),  // CreateLEDDevice
    reinterpret_cast<const void*>(&Eng_Unsupported),  // CreateVibraDevice
    reinterpret_cast<const void*>(&Eng_CreateAudioPlayer),
    reinterpret_cast<const void*>(&Eng_CreateAudioRecorder),
    reinterpret_cast<const void*>(&Eng_Unsupported),  // CreateMidiPlayer
    reinterpret_cast<const void*>(&Eng_Unsupported),  // CreateListener
    reinterpret_cast<const void*>(&Eng_Unsupported),  // Create3DGroup
    reinterpret_cast<const void*>(&Eng_CreateOutputMix),
    reinterpret_cast<const void*>(&Eng_Unsupported),  // CreateMetadataExtractor
    reinterpret_cast<const void*>(&Eng_Unsupported),  // CreateExtensionObject
    reinterpret_cast<const void*>(&Eng_QueryNumSupportedInterfaces),
    reinterpret_cast<const void*>(&Eng_QuerySupportedInterfaces),
    reinterpret_cast<const void*>(&Eng_QueryNumSupportedExtensions),
    reinterpret_cast<const void*>(&Eng_QuerySupportedExtension),
    reinterpret_cast<const void*>(&Eng_IsExtensionSupported),
};

Object* new_object(Kind kind, uint32_t exposed) { return new_object_with(kind, exposed, kEngineVtable); }

// ---- the library's exports --------------------------------------------------------------

WINAPK_GUEST_ABI SLresult slCreateEngine(const void** out, SLuint32 option_count, const void* options, SLuint32 count,
                                         const SLInterfaceID* ids, const SLboolean* required) {
    if (!out) return kParameterInvalid;
    *out = nullptr;
    if (option_count > 0 && !options) return kParameterInvalid;
    struct EngineOption {
        SLuint32 feature, data;
    };
    const auto* opt = static_cast<const EngineOption*>(options);
    for (SLuint32 i = 0; i < option_count; ++i) {
        // SL_ENGINEOPTION_THREADSAFE (always so here), SL_ENGINEOPTION_LOSSOFCONTROL (not supported).
        if (opt[i].feature != 1 && opt[i].feature != 2) return kParameterInvalid;
        if (opt[i].feature == 2 && opt[i].data != 0) return kFeatureUnsupported;
    }
    uint32_t exposed = 0;
    const SLresult r = check_interfaces(Kind::kEngine, false, count, ids, required, &exposed);
    if (r != kSuccess) return r;
    *out = &new_object(Kind::kEngine, exposed)->object_itf;
    return kSuccess;
}
WINAPK_GUEST_ABI SLresult slQueryNumSupportedEngineInterfaces(SLuint32* n) {
    return query_num_interfaces(Kind::kEngine, n);
}
WINAPK_GUEST_ABI SLresult slQuerySupportedEngineInterfaces(SLuint32 index, SLInterfaceID* id) {
    return query_interface(Kind::kEngine, index, id);
}

// The exported variables: SL_IID_x is id_variables()[i], which points at
// kInterfaceIds[i].guid.
const SlGuid* const* id_variables() {
    static const SlGuid* const* vars = [] {
        static const SlGuid* v[kInterfaceIdCount];
        for (size_t i = 0; i < kInterfaceIdCount; ++i) v[i] = &kInterfaceIds[i].guid;
        return v;
    }();
    return vars;
}

}  // namespace

void* native_opensles_resolve(const char* name) {
    if (std::strcmp(name, "slCreateEngine") == 0) return reinterpret_cast<void*>(&slCreateEngine);
    if (std::strcmp(name, "slQueryNumSupportedEngineInterfaces") == 0) {
        return reinterpret_cast<void*>(&slQueryNumSupportedEngineInterfaces);
    }
    if (std::strcmp(name, "slQuerySupportedEngineInterfaces") == 0) {
        return reinterpret_cast<void*>(&slQuerySupportedEngineInterfaces);
    }
    if (std::strncmp(name, "SL_IID_", 7) != 0) return nullptr;
    for (size_t i = 0; i < kInterfaceIdCount; ++i) {
        if (std::strcmp(name, kInterfaceIds[i].name) == 0) {
            return const_cast<void*>(static_cast<const void*>(&id_variables()[i]));
        }
    }
    return nullptr;
}

}  // namespace winapk::host
