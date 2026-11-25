// native_opensles.cpp -- libOpenSLES for an app's native code: OpenSL ES 1.0.1
// with Android's extensions, as Android 14 implements them (AOSP
// frameworks/wilhelm), on the host's audio.
//
// The objects:
//
//   - The engine is shared, as on Android: a second slCreateEngine returns the
//     same object with one more reference, and only the last Destroy ends it.
//     It owns the callback thread (below).
//   - The output mix is the default device; it has no effects.
//   - Audio players: a PCM buffer queue (SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE
//     or SL_DATALOCATOR_BUFFERQUEUE) or a file (SL_DATALOCATOR_ANDROIDFD, a URI:
//     file://, an absolute path, http:// or https://) played to the output mix;
//     or a file decoded into the app's buffer queue as 16-bit PCM (Android's
//     decode-to-PCM, with SLMetadataExtractionItf for its format).
//   - Audio recorders: the default microphone into a buffer queue.
//
// Playback is pulled by audio_engine.h's mixer, with no thread per player:
// each player keeps a short ring of stereo frames at the output's rate, which
// the mixer drains every cycle. The callback thread -- one per engine, like
// the AudioTrack callback thread a player has on Android -- keeps each ring
// filled a little over two device periods ahead: it takes the app's buffers
// (or the decoded file), converts any of the PCM formats Android accepts
// (8-bit unsigned, 16/24/32-bit integer, float; 1 to 8 channels folded to
// stereo) and resamples band-limited to the output's rate (audio_pcm.h). A
// buffer is handed back -- the app's callback called -- once it has been
// taken whole into the ring, as Android's is once AudioTrack has copied it.
//
// Callbacks (buffer queue, play events, prefetch, object and dynamic
// interface events, recorder buffers) all run on the callback thread, one at
// a time in the order they happened, and never with a lock of ours held, so
// a callback may call back into any interface, enqueue, or Destroy its own
// object. Destroy from another thread waits for a callback of that object in
// progress to return, as Android's CallbackProtector does; nothing is called
// for an object once Destroy has returned.
//
// File players decode the whole file (audio_engine.h's decode(): WAV, Ogg
// Vorbis, Ogg Opus, and MP3/AAC/... through Media Foundation) when they are
// first set to PAUSED or PLAYING, which is when Android prepares them; the
// prefetch status interface reports the outcome as Android does (a failure is
// a FILLLEVELCHANGE|STATUSCHANGE callback with level 0 and UNDERFLOW). An fd
// is read when the player is created (Android dups it). http(s) URIs are
// downloaded then, whole, up to 128 MB: a stream without an end does not play.
//
// A recorder is realized only when the app holds RECORD_AUDIO (asked as
// AudioRecord asks it) and a microphone can be opened: otherwise Realize fails
// with SL_RESULT_CONTENT_UNSUPPORTED, which is what Android's AudioRecord
// initCheck failure gives.
//
// Not implemented, and reported as such (GetInterface or a required interface
// answers SL_RESULT_FEATURE_UNSUPPORTED): effects (reverb, equalizer, bass
// boost, virtualizer, effect sends), 3D, MIDI, LED and vibra devices, the
// engine's I/O device and codec capability interfaces, SL_IID_ANDROIDEFFECT*,
// the recorder's echo cancellation, gain control and noise suppression, the
// MPEG-2 TS and AAC ADTS Android buffer queue sources, and the Java routing
// proxy. SL_PLAYEVENT_HEADMOVING is accepted and never sent, as on Android.

#include "native_opensles.h"

#include "audio_capture.h"
#include "audio_engine.h"
#include "audio_pcm.h"
#include "winapk/arm64_guest.h"
#include "winapk/guest_abi.h"
#include "winapk/mini_bionic.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <winhttp.h>

namespace winapk::host {

namespace {

// ---- the ABI (OpenSLES.h, OpenSLES_Android.h, OpenSLES_AndroidConfiguration.h) ------------

using SLresult = uint32_t;
constexpr SLresult kSuccess = 0, kPreconditionsViolated = 1, kParameterInvalid = 2,
                   kBufferInsufficient = 7, kContentUnsupported = 9, kFeatureUnsupported = 12;

constexpr uint32_t kObjEngine = 0x1001, kObjAudioPlayer = 0x1004, kObjAudioRecorder = 0x1005,
                   kObjOutputMix = 0x1009;
constexpr uint32_t kUnrealized = 1, kRealized = 2;
constexpr uint32_t kObjectEventAsyncTermination = 2, kDynItfEventAsyncTermination = 2;

constexpr uint32_t kLocUri = 1, kLocAddress = 2, kLocIoDevice = 3, kLocOutputMix = 4, kLocBufferQueue = 6,
                   kLocMidiBufferQueue = 7, kLocAndroidFd = 0x800007BC, kLocAndroidSimpleBq = 0x800007BD,
                   kLocAndroidBq = 0x800007BE;
constexpr uint32_t kFmtMime = 1, kFmtPcm = 2, kFmtPcmEx = 4;
constexpr uint32_t kRepSigned = 1, kRepUnsigned = 2, kRepFloat = 3;
constexpr uint32_t kBigEndian = 1, kLittleEndian = 2;
constexpr uint32_t kIoDeviceAudioInput = 1, kDefaultAudioInput = 0xFFFFFFFF, kDefaultAudioOutput = 0xFFFFFFFE;

constexpr uint32_t kStopped = 1, kPaused = 2, kPlaying = 3;  // SL_PLAYSTATE_* and SL_RECORDSTATE_*
constexpr uint32_t kHeadAtEnd = 1, kHeadAtMarker = 2, kHeadAtNewPos = 4, kHeadMoving = 8, kHeadStalled = 0x10;
constexpr uint32_t kRecHeadAtLimit = 1, kRecBufferFull = 0x20;
constexpr uint32_t kTimeUnknown = 0xFFFFFFFF;
constexpr uint32_t kPrefetchStatusChange = 1, kPrefetchFillLevelChange = 2;
constexpr uint32_t kPrefetchUnderflow = 1, kPrefetchSufficient = 2;
constexpr uint32_t kSeekFast = 1, kSeekAccurate = 2;
constexpr uint32_t kRateSilent = 0x100, kRateStaggered = 0x200, kRateNoPitchCor = 0x400, kRatePitchCor = 0x800;
constexpr int16_t kMinRate = 500, kMaxRate = 2000;  // permille, AudioTrack's and MediaPlayer's range on Android
constexpr uint32_t kEngineOptionThreadSafe = 1, kEngineOptionLossOfControl = 2;
constexpr uint32_t kCharsetBinary = 1, kCharsetUtf8 = 0x1D;

// SLAndroidConfigurationItf's keys and values.
constexpr const char* kKeyStreamType = "androidPlaybackStreamType";
constexpr const char* kKeyPerformanceMode = "androidPerformanceMode";
constexpr const char* kKeyRecordingPreset = "androidRecordingPreset";
constexpr int32_t kStreamMedia = 3;
constexpr uint32_t kPerfLatency = 1;
constexpr uint32_t kPresetGeneric = 1;

constexpr const char* kPcmKeys[] = {
    "AndroidPcmFormatNumChannels",   "AndroidPcmFormatSampleRate",  "AndroidPcmFormatBitsPerSample",
    "AndroidPcmFormatContainerSize", "AndroidPcmFormatChannelMask", "AndroidPcmFormatEndianness",
};

struct Guid {
    uint32_t time_low;
    uint16_t time_mid, time_hi;
    uint16_t clock_seq;
    uint8_t node[6];
};

struct Itf;
struct Object;

// What a guest holds: SLxxxItf is a pointer to a pointer to a vtable.
struct Itf {
    const void* const* vtbl;
    Object* obj;
};

struct DataSource {
    const void* locator;
    const void* format;
};
struct LocBufferQueue {
    uint32_t type, buffers;
};
struct LocUri {
    uint32_t type;
    const char* uri;
};
struct LocFd {
    uint32_t type;
    int32_t fd;
    int64_t offset, length;
};
struct LocOutputMix {
    uint32_t type;
    Itf* mix;
};
struct LocIoDevice {
    uint32_t type, device_type, device_id;
    Itf* device;
};
struct FmtPcm {
    uint32_t type, channels, milli_hz, bits, container, mask, endianness;
};
struct FmtPcmEx {
    FmtPcm pcm;
    uint32_t representation;
};
struct FmtMime {
    uint32_t type;
    const char* mime;
    uint32_t container;
};
struct EngineOption {
    uint32_t feature, data;
};
struct MetadataInfo {
    uint32_t size, encoding;
    char lang_country[16];
    uint8_t data[1];
};

#define GUEST WINAPK_GUEST_ABI
using ObjectCallback = void(GUEST*)(Itf*, const void* ctx, uint32_t event, SLresult result, uint32_t param, void* itf);
using DynCallback = void(GUEST*)(Itf*, void* ctx, uint32_t event, SLresult result, const Guid* iid);
using PlayCallback = void(GUEST*)(Itf*, void* ctx, uint32_t event);  // also the recorder's and prefetch's
using QueueCallback = void(GUEST*)(Itf*, void* ctx);

template <typename F>
F guest_fn(void* fn) {
    return reinterpret_cast<F>(fn ? arm64::host_callable(fn) : nullptr);
}

void log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[opensles] %s\n", line);
}

// ---- interface IDs -----------------------------------------------------------------------
// The values of AOSP's OpenSLES_IID.cpp (the Khronos 1.0.1 IDs and Android's).

struct IidVar {
    const char* name;
    Guid guid;
};

const IidVar kIids[] = {
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
    {"SL_IID_ENVIRONMENTALREVERB", {0xc2e5d5f0, 0x94bd, 0x4763, 0x9cac, {0x4e, 0x23, 0x4d, 0x6, 0x83, 0x9e}}},
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
    {"SL_IID_ANDROIDEFFECT", {0xae12da60, 0x99ac, 0x11df, 0xb456, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDEFFECTCAPABILITIES", {0x6a4f6d60, 0xb5e6, 0x11df, 0xbb3b, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDEFFECTSEND", {0x7be462c0, 0xbc43, 0x11df, 0x8670, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDCONFIGURATION", {0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", {0x198e4940, 0xc5d7, 0x11df, 0xa2a6, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDBUFFERQUEUESOURCE", {0x7fc1a460, 0xeec1, 0x11df, 0xa306, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDACOUSTICECHOCANCELLATION",
     {0x7b491460, 0x8d4d, 0x11e0, 0xbd61, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDAUTOMATICGAINCONTROL", {0x0a8abfe0, 0x654c, 0x11e0, 0xba26, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {"SL_IID_ANDROIDNOISESUPPRESSION", {0x58b4b260, 0x8e06, 0x11e0, 0xaa8e, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
};
constexpr size_t kIidCount = sizeof(kIids) / sizeof(kIids[0]);

// The SL_IID_x variables: each holds a pointer to its GUID.
const Guid* const* iid_vars() {
    static const Guid* vars[kIidCount] = {};
    static const bool filled = [] {
        for (size_t i = 0; i < kIidCount; ++i) vars[i] = &kIids[i].guid;
        return true;
    }();
    (void)filled;
    return vars;
}

const Guid* iid(const char* name) {
    for (const auto& v : kIids) {
        if (std::strcmp(v.name, name) == 0) return &v.guid;
    }
    return nullptr;
}

bool same(const void* a, const Guid* b) { return a && b && std::memcmp(a, b, sizeof(Guid)) == 0; }

// The interfaces this library implements; others are known IDs it answers
// "unsupported" for.
enum Kind {
    kObject,
    kDim,
    kEngineItf,
    kEngineCaps,
    kThreadSync,
    kOutputMixItf,
    kPlay,
    kQueue,
    kVolume,
    kMuteSolo,
    kSeek,
    kPrefetch,
    kRate,
    kConfig,
    kMetadata,
    kRecord,
    kKindCount
};
constexpr uint32_t bit(Kind k) { return 1u << k; }

// The kind an interface ID names on an object of class `cls`; -1 unknown, -2
// a known ID this library does not implement.
int kind_of(uint32_t cls, const void* id) {
    struct Map {
        const char* name;
        int kind;
    };
    static const Map kMap[] = {
        {"SL_IID_OBJECT", kObject},
        {"SL_IID_DYNAMICINTERFACEMANAGEMENT", kDim},
        {"SL_IID_ENGINE", kEngineItf},
        {"SL_IID_ENGINECAPABILITIES", kEngineCaps},
        {"SL_IID_THREADSYNC", kThreadSync},
        {"SL_IID_OUTPUTMIX", kOutputMixItf},
        {"SL_IID_PLAY", kPlay},
        {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", kQueue},
        {"SL_IID_VOLUME", kVolume},
        {"SL_IID_MUTESOLO", kMuteSolo},
        {"SL_IID_SEEK", kSeek},
        {"SL_IID_PREFETCHSTATUS", kPrefetch},
        {"SL_IID_PLAYBACKRATE", kRate},
        {"SL_IID_ANDROIDCONFIGURATION", kConfig},
        {"SL_IID_METADATAEXTRACTION", kMetadata},
        {"SL_IID_RECORD", kRecord},
    };
    for (const auto& m : kMap) {
        if (same(id, iid(m.name))) return m.kind;
    }
    // SL_IID_BUFFERQUEUE is the simple queue's alias on a player only.
    if (same(id, iid("SL_IID_BUFFERQUEUE"))) return cls == kObjAudioPlayer ? kQueue : -2;
    for (const auto& v : kIids) {
        if (same(id, &v.guid)) return -2;
    }
    return -1;
}

// Which interfaces each class has: implicit ones always, explicit ones when
// asked for at creation, dynamic ones also through SLDynamicInterfaceManagementItf;
// "early" ones are reachable before Realize. A player narrows these further by
// what it plays (Player::allowed).
struct ClassInfo {
    uint32_t id;
    uint32_t implicit, explicit_, dynamic, early;
};
constexpr ClassInfo kEngineClass{kObjEngine, bit(kObject) | bit(kDim) | bit(kEngineItf) | bit(kEngineCaps) |
                                                 bit(kThreadSync),
                                 0, 0, bit(kObject)};
constexpr ClassInfo kMixClass{kObjOutputMix, bit(kObject) | bit(kDim) | bit(kOutputMixItf), 0, 0, bit(kObject)};
constexpr ClassInfo kPlayerClass{kObjAudioPlayer, bit(kObject) | bit(kDim) | bit(kPlay),
                                 bit(kQueue) | bit(kVolume) | bit(kMuteSolo) | bit(kSeek) | bit(kPrefetch) |
                                     bit(kConfig),
                                 bit(kRate) | bit(kMetadata), bit(kObject) | bit(kConfig)};
constexpr ClassInfo kRecorderClass{kObjAudioRecorder, bit(kObject) | bit(kDim) | bit(kRecord),
                                   bit(kQueue) | bit(kConfig), 0, bit(kObject) | bit(kConfig)};

const ClassInfo* class_of(uint32_t id) {
    switch (id) {
    case kObjEngine: return &kEngineClass;
    case kObjOutputMix: return &kMixClass;
    case kObjAudioPlayer: return &kPlayerClass;
    case kObjAudioRecorder: return &kRecorderClass;
    default: return nullptr;
    }
}

// The IDs of a class's interfaces, in a stable order (QuerySupportedInterfaces).
std::vector<const Guid*> class_iids(const ClassInfo& c) {
    static const char* const kNames[kKindCount] = {
        "SL_IID_OBJECT",          "SL_IID_DYNAMICINTERFACEMANAGEMENT",
        "SL_IID_ENGINE",          "SL_IID_ENGINECAPABILITIES",
        "SL_IID_THREADSYNC",      "SL_IID_OUTPUTMIX",
        "SL_IID_PLAY",            "SL_IID_ANDROIDSIMPLEBUFFERQUEUE",
        "SL_IID_VOLUME",          "SL_IID_MUTESOLO",
        "SL_IID_SEEK",            "SL_IID_PREFETCHSTATUS",
        "SL_IID_PLAYBACKRATE",    "SL_IID_ANDROIDCONFIGURATION",
        "SL_IID_METADATAEXTRACTION", "SL_IID_RECORD",
    };
    std::vector<const Guid*> out;
    const uint32_t all = c.implicit | c.explicit_ | c.dynamic;
    for (int k = 0; k < kKindCount; ++k) {
        if (all & bit(static_cast<Kind>(k))) out.push_back(iid(kNames[k]));
    }
    if (c.id == kObjAudioPlayer) out.push_back(iid("SL_IID_BUFFERQUEUE"));
    return out;
}

// ---- the callback thread -----------------------------------------------------------------

struct Media;

// One per engine. Holds no app-visible state: players and recorders hand it
// work (service) and callbacks to run (post).
struct Core {
    std::mutex m;
    std::condition_variable cv, idle;
    bool quit = false, kick = false;
    std::deque<std::pair<std::shared_ptr<Object>, std::function<void()>>> events;
    std::vector<std::shared_ptr<Media>> media;
    const Object* running = nullptr;  // whose callback runs now
    std::thread::id tid;

    void post(std::shared_ptr<Object> who, std::function<void()> fn) {
        std::lock_guard<std::mutex> hold(m);
        events.emplace_back(std::move(who), std::move(fn));
        kick = true;
        cv.notify_one();
    }
    void wake() {
        std::lock_guard<std::mutex> hold(m);
        kick = true;
        cv.notify_one();
    }
    void add(std::shared_ptr<Media> x) {
        std::lock_guard<std::mutex> hold(m);
        media.push_back(std::move(x));
        kick = true;
        cv.notify_one();
    }
    void remove(const Media* x);
    // Returns once no callback of `obj` runs (at once on this thread itself).
    void wait_idle(const Object* obj) {
        std::unique_lock<std::mutex> hold(m);
        if (std::this_thread::get_id() == tid) return;
        idle.wait(hold, [&] { return running != obj; });
    }
};

void run_core(std::shared_ptr<Core> core);

// ---- objects -----------------------------------------------------------------------------

struct Engine;

struct Object : std::enable_shared_from_this<Object> {
    explicit Object(const ClassInfo& c) : cls(c) {
        for (auto& i : itfs) i.obj = this;
    }
    virtual ~Object() = default;

    const ClassInfo& cls;
    std::shared_ptr<Engine> engine;  // null for the engine itself
    std::shared_ptr<Core> core;
    std::mutex lock;
    uint32_t state = kUnrealized;
    uint32_t exposed = 0;
    std::atomic<bool> dead{false};
    ObjectCallback object_cb = nullptr;
    const void* object_ctx = nullptr;
    DynCallback dyn_cb = nullptr;
    void* dyn_ctx = nullptr;
    int32_t priority = INT32_MAX;
    uint32_t preemptable = 0;
    Itf itfs[kKindCount] = {};

    Itf* itf(Kind k) { return &itfs[k]; }
    // Interfaces this object can have beyond its class's rule (a player's
    // source and sink narrow them).
    virtual uint32_t allowed() const { return cls.implicit | cls.explicit_ | cls.dynamic; }
    virtual SLresult realize() { return kSuccess; }
    // Destroy's work: detach from the mixer and the callback thread.
    virtual void teardown() {}
};

Object* obj_of(const void* self) { return static_cast<const Itf*>(self)->obj; }
template <typename T>
T* as(const void* self) {
    return static_cast<T*>(obj_of(self));
}

// The live objects, by address: an app's pointer is good until Destroy.
std::mutex g_objects_lock;
std::unordered_map<const Object*, std::shared_ptr<Object>> g_objects;

void publish(std::shared_ptr<Object> o) {
    std::lock_guard<std::mutex> hold(g_objects_lock);
    g_objects[o.get()] = std::move(o);
}
std::shared_ptr<Object> unpublish(const Object* o) {
    std::lock_guard<std::mutex> hold(g_objects_lock);
    auto it = g_objects.find(o);
    if (it == g_objects.end()) return nullptr;
    auto keep = std::move(it->second);
    g_objects.erase(it);
    return keep;
}

// Whose answer is "which interfaces to expose": AOSP's checkInterfaces.
SLresult choose_interfaces(const Object& o, uint32_t n, const Guid* const* ids, const uint32_t* required,
                           uint32_t* exposed) {
    *exposed = o.cls.implicit;
    if (n > 0 && !ids) return kParameterInvalid;
    for (uint32_t i = 0; i < n; ++i) {
        if (!ids[i]) return kParameterInvalid;
        const bool must = required && required[i];
        const int k = kind_of(o.cls.id, ids[i]);
        const uint32_t b = k >= 0 ? bit(static_cast<Kind>(k)) : 0;
        if (b && (o.allowed() & b) && ((o.cls.implicit | o.cls.explicit_ | o.cls.dynamic) & b)) {
            *exposed |= b;
        } else if (must) {
            return kFeatureUnsupported;
        }
    }
    return kSuccess;
}

// ---- engine ------------------------------------------------------------------------------

struct Engine : Object {
    Engine() : Object(kEngineClass) {}
    int refs = 1;
    bool thread_safe = true;
    std::mutex sync_lock;  // SLThreadSyncItf
    std::condition_variable sync_cv;
    bool in_section = false;
    std::thread::id section_owner;

    SLresult realize() override {
        core = std::make_shared<Core>();
        std::thread t(run_core, core);
        {
            std::lock_guard<std::mutex> hold(core->m);
            core->tid = t.get_id();
        }
        t.detach();
        return kSuccess;
    }
    void teardown() override {
        if (!core) return;
        std::lock_guard<std::mutex> hold(core->m);
        core->quit = true;
        core->cv.notify_one();
    }
};

std::mutex g_engine_lock;
std::shared_ptr<Engine> g_engine;  // Android's one engine per process

// ---- output mix --------------------------------------------------------------------------

struct OutputMix : Object {
    OutputMix() : Object(kMixClass) {}
    void* device_cb = nullptr;
    void* device_ctx = nullptr;
};

// ---- players and recorders ---------------------------------------------------------------

// Something the callback thread services: a player or a recorder.
struct Media : Object {
    using Object::Object;
    // Does what is due now (fills rings and buffers, posts callbacks); true
    // when it made progress and may make more at once.
    virtual bool service() = 0;
    // True while it needs servicing without anything waking the thread (a
    // recorder reading the microphone).
    virtual bool polled() const { return false; }

    // The app's buffer queue (SLAndroidSimpleBufferQueueItf / SLBufferQueueItf).
    struct Buf {
        uint8_t* data;
        uint32_t size;
    };
    uint32_t queue_slots = 0;
    std::deque<Buf> queue;
    uint32_t consumed = 0;  // bytes of the front buffer taken (or filled)
    uint32_t queue_index = 0;
    QueueCallback queue_cb = nullptr;
    void* queue_ctx = nullptr;

    // Play or record events and their timing (SLPlayItf / SLRecordItf).
    uint32_t transport = kStopped;
    PlayCallback event_cb = nullptr;
    void* event_ctx = nullptr;
    uint32_t event_mask = 0;
    uint32_t marker = kTimeUnknown;
    uint32_t period = 1000;
    int64_t last_ms = 0;
    int64_t next_period_ms = 1000;

    // SLAndroidConfigurationItf.
    int32_t stream_type = kStreamMedia;
    uint32_t performance = kPerfLatency;
    uint32_t preset = kPresetGeneric;

    void post_event(uint32_t event) {
        if (!(event_mask & event) || !event_cb) return;
        auto cb = event_cb;
        void* ctx = event_ctx;
        Itf* caller = itf(cls.id == kObjAudioRecorder ? kRecord : kPlay);
        core->post(shared_from_this(), [cb, ctx, caller, event] { cb(caller, ctx, event); });
    }
    void post_queue_done() {
        if (!queue_cb) return;
        auto cb = queue_cb;
        void* ctx = queue_ctx;
        Itf* caller = itf(kQueue);
        core->post(shared_from_this(), [cb, ctx, caller] { cb(caller, ctx); });
    }
    // The marker and the periodic position events, as the head moves from
    // last_ms to now_ms (forward; a jump back restarts the periods).
    void timing(int64_t now_ms) {
        if (now_ms < last_ms) {
            last_ms = now_ms;
            next_period_ms = (now_ms / period + 1) * period;
            return;
        }
        if (marker != kTimeUnknown && last_ms < marker && marker <= now_ms) post_event(kHeadAtMarker);
        if (now_ms >= next_period_ms) {
            post_event(kHeadAtNewPos);
            next_period_ms = (now_ms / period + 1) * period;
        }
        last_ms = now_ms;
    }
    void reset_timing(int64_t now_ms) {
        last_ms = now_ms;
        next_period_ms = (now_ms / period + 1) * period;
    }
};

void Core::remove(const Media* x) {
    std::lock_guard<std::mutex> hold(m);
    media.erase(std::remove_if(media.begin(), media.end(),
                               [x](const std::shared_ptr<Media>& p) { return p.get() == x; }),
                media.end());
    kick = true;
    cv.notify_one();
}

// The PCM a buffer queue carries.
struct Pcm {
    audio::pcm::Sample sample = audio::pcm::Sample::kS16;
    int channels = 2;
    uint32_t mask = 3;
    int rate = 44100;
    size_t frame_bytes() const { return audio::pcm::sample_bytes(sample) * static_cast<size_t>(channels); }
};

// Checks an app's PCM format as Android's checkDataFormat does, then
// Android's player/recorder limits; fills `out`.
SLresult parse_pcm(const void* format, bool output, Pcm* out, const char** why) {
    const uint32_t type = *static_cast<const uint32_t*>(format);
    if (type != kFmtPcm && type != kFmtPcmEx) {
        *why = "the buffer queue's format is not PCM";
        return type == kFmtMime ? kContentUnsupported : kParameterInvalid;
    }
    const auto& f = *static_cast<const FmtPcm*>(format);
    uint32_t rep = 0;
    if (type == kFmtPcmEx) {
        rep = static_cast<const FmtPcmEx*>(format)->representation;
        if (rep != kRepSigned && rep != kRepUnsigned && rep != kRepFloat) {
            return *why = "representation", kParameterInvalid;
        }
    }
    if (f.channels == 0) return *why = "no channels", kParameterInvalid;
    if (f.channels > 8) return *why = "more than 8 channels", kContentUnsupported;
    if (f.milli_hz == 0) return *why = "no sample rate", kParameterInvalid;
    if (f.milli_hz < 8000000 || f.milli_hz > 192000000) {
        return *why = "sample rate out of 8-192 kHz", kContentUnsupported;
    }
    switch (f.container) {
    case 8:
        if (rep && rep != kRepUnsigned) return *why = "8-bit is unsigned", kParameterInvalid;
        break;
    case 16:
    case 24:
        if (rep && rep != kRepSigned) return *why = "16/24-bit is signed", kParameterInvalid;
        break;
    case 32:
        if (rep && rep != kRepSigned && rep != kRepFloat) return *why = "32-bit is signed or float", kParameterInvalid;
        break;
    default: return *why = "container size", kParameterInvalid;
    }
    if (f.bits == 0 || f.container < f.bits) return *why = "bits per sample", kParameterInvalid;
    uint32_t mask = f.mask;
    if (mask == 0) {
        mask = output ? audio::pcm::default_out_mask(static_cast<int>(f.channels))
                      : (f.channels == 1 ? audio::pcm::kFrontLeft
                                         : f.channels == 2 ? (audio::pcm::kFrontLeft | audio::pcm::kFrontRight) : 0);
        if (mask == 0) return *why = "no channel mask and no default", kParameterInvalid;
    }
    if (audio::pcm::popcount(mask & ~audio::pcm::kNonPositional) != static_cast<int>(f.channels)) {
        return *why = "channel mask does not match the channel count", kParameterInvalid;
    }
    if (f.endianness != kBigEndian && f.endianness != kLittleEndian) return *why = "byte order", kParameterInvalid;
    // Android's own limits.
    if (f.endianness != kLittleEndian) return *why = "big-endian PCM", kContentUnsupported;
    if (f.container != f.bits) return *why = "container wider than the sample", kContentUnsupported;
    if (output && !audio::pcm::player_mask_ok(mask, static_cast<int>(f.channels))) {
        return *why = "channel mask Android's downmixer refuses", kContentUnsupported;
    }
    if (!output && f.channels > 2) return *why = "more than 2 recorded channels", kContentUnsupported;
    out->channels = static_cast<int>(f.channels);
    out->mask = mask;
    out->rate = static_cast<int>(f.milli_hz / 1000);
    out->sample = f.container == 8    ? audio::pcm::Sample::kU8
                  : f.container == 16 ? audio::pcm::Sample::kS16
                  : f.container == 24 ? audio::pcm::Sample::kS24
                  : rep == kRepFloat  ? audio::pcm::Sample::kF32
                                      : audio::pcm::Sample::kS32;
    return kSuccess;
}

// ---- reading files -------------------------------------------------------------------------

std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

bool starts_with_ci(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return s.size() >= n && _strnicmp(s.c_str(), prefix, n) == 0;
}

bool read_host_file(const std::wstring& path, std::vector<uint8_t>* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1LL << 31);
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out->empty() || (ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) &&
                              got == out->size());
    }
    CloseHandle(h);
    return ok;
}

// A guest descriptor's bytes, through the shim's pread64 (guest descriptors
// are the shim's, not the CRT's). length < 0: to the end.
bool read_guest_fd(int fd, int64_t offset, int64_t length, std::vector<uint8_t>* out) {
    using Pread = int64_t(GUEST*)(int, void*, size_t, int64_t);
    auto pread = reinterpret_cast<Pread>(mini_bionic_resolve("pread64"));
    if (!pread || fd < 0 || offset < 0) return false;
    out->clear();
    constexpr size_t kChunk = 1 << 16;
    const int64_t limit = length < 0 ? (1LL << 31) : length;
    while (static_cast<int64_t>(out->size()) < limit) {
        const size_t want = static_cast<size_t>(std::min<int64_t>(kChunk, limit - static_cast<int64_t>(out->size())));
        const size_t at = out->size();
        out->resize(at + want);
        const int64_t got = pread(fd, out->data() + at, want, offset + static_cast<int64_t>(at));
        if (got <= 0) {
            out->resize(at);
            return got == 0 && !out->empty();
        }
        out->resize(at + static_cast<size_t>(got));
    }
    return true;
}

// The whole body of an http(s) URL, WinHTTP with the system's proxy.
bool http_get(const std::string& url, std::vector<uint8_t>* out, const std::atomic<bool>& cancel) {
    constexpr size_t kMax = 128u << 20;
    const std::wstring wurl(url.begin(), url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    wchar_t extra[2048] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return false;
    const std::wstring object = std::wstring(path) + extra;
    HINTERNET session = WinHttpOpen(L"Win.apk OpenSL ES", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        session = WinHttpOpen(L"Win.apk OpenSL ES", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) return false;
    bool ok = false;
    HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request =
        connect ? WinHttpOpenRequest(connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                : nullptr;
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        ok = status == 200;
        while (ok && !cancel.load()) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail)) {
                ok = false;
                break;
            }
            if (avail == 0) break;
            if (out->size() + avail > kMax) {
                ok = false;
                break;
            }
            const size_t at = out->size();
            out->resize(at + avail);
            DWORD got = 0;
            if (!WinHttpReadData(request, out->data() + at, avail, &got)) {
                ok = false;
                break;
            }
            out->resize(at + got);
        }
        ok = ok && !cancel.load() && !out->empty();
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

// ---- the ring the mixer drains -------------------------------------------------------------

// Stereo frames at the output's rate, written by the callback thread and read
// by the mixer, both under Player::ring_lock.
class Ring {
public:
    explicit Ring(size_t frames = 0) : buf_(frames * 2) {}
    size_t size() const { return count_; }
    size_t room() const { return buf_.size() / 2 - count_; }
    void clear() { head_ = count_ = 0; }
    void write(const float* in, size_t frames) {
        const size_t cap = buf_.size() / 2;
        for (size_t f = 0; f < frames; ++f) {
            const size_t at = (head_ + count_) % cap;
            buf_[at * 2] = in[f * 2];
            buf_[at * 2 + 1] = in[f * 2 + 1];
            ++count_;
        }
    }
    // Adds up to `frames` frames, scaled, into `out`; returns how many.
    size_t mix_into(float* out, size_t frames, float gl, float gr) {
        const size_t cap = buf_.size() / 2;
        const size_t n = std::min(frames, count_);
        for (size_t f = 0; f < n; ++f) {
            const size_t at = (head_ + f) % cap;
            out[f * 2] += buf_[at * 2] * gl;
            out[f * 2 + 1] += buf_[at * 2 + 1] * gr;
        }
        head_ = (head_ + n) % cap;
        count_ -= n;
        return n;
    }

private:
    std::vector<float> buf_;
    size_t head_ = 0, count_ = 0;
};

// ---- audio players ---------------------------------------------------------------------------

struct Player;

// The player's place in the mixer.
class PlayerSource final : public audio::Source {
public:
    explicit PlayerSource(Player* p) : player_(p) {}
    bool mix(float* out, size_t frames, int out_rate) override;

private:
    Player* player_;  // outlives the source: teardown removes it from the mixer first
};

struct Player : Media {
    enum Type { kQueuePlayer, kFilePlayer, kDecoder };
    enum Prep { kUnprepared, kPreparing, kReady, kFailed };

    explicit Player(Type t) : Media(kPlayerClass), type(t) {}
    const Type type;

    // What it plays.
    Pcm pcm;                       // a buffer queue's format
    audio::pcm::Downmix downmix;   // and how it folds to stereo
    std::string uri;               // a URI player's
    std::vector<uint8_t> encoded;  // an fd player's file, read at Realize
    std::atomic<bool> cancel{false};
    Prep prep = kUnprepared;
    std::shared_ptr<const audio::Clip> clip;
    uint32_t pending_seek_ms = kTimeUnknown;

    // SLVolumeItf, SLMuteSoloItf.
    int16_t level = 0;
    bool muted = false, pan_on = false;
    int16_t pan = 0;
    uint8_t mute_mask = 0, solo_mask = 0;
    // SLSeekItf, SLPlaybackRateItf.
    bool looping = false;
    int16_t rate = 1000;
    uint32_t rate_props = kRateNoPitchCor;
    // SLPrefetchStatusItf.
    uint32_t prefetch_status = kPrefetchUnderflow;
    int16_t fill_level = 0;
    PlayCallback prefetch_cb = nullptr;
    void* prefetch_ctx = nullptr;
    uint32_t prefetch_mask = 0;
    int16_t fill_period = 100;
    // SLMetadataExtractionItf's key filter (kept; there are six keys).
    bool filtered = false;

    // Rendering: the input timeline is the source's frames (the queue's since
    // the last stop, or the file's), and origin is the frame the resampler's
    // position 0 stands for.
    audio::pcm::Resampler resampler;
    int64_t origin = 0;
    int64_t feed = 0;          // a file's next frame to resample (counting loops)
    bool flushed_end = false;  // the file's end went into the resampler
    bool starved = false;      // a queue ran dry and HEADATEND went out
    int64_t wraps = 0;         // loops fed since the last restart
    bool at_end = false;       // a file played to its end (HEADATEND sent)
    std::vector<float> scratch;
    std::vector<float> rendered;  // the resampler's output on its way into the ring
    std::shared_ptr<PlayerSource> source;

    // What the mixer reads, under ring_lock.
    std::mutex ring_lock;
    Ring ring;
    bool audible = false;  // PLAYING with something to play
    bool detached = false;
    float gain_l = 1, gain_r = 1;
    bool stalled = false;  // the ring ran dry while playing
    bool full_cycle = false;

    int in_rate() const { return type == kQueuePlayer ? pcm.rate : (clip ? clip->rate : 0); }
    int channel_count() const { return type == kQueuePlayer ? pcm.channels : (clip ? 2 : 0); }

    uint32_t allowed() const override {
        uint32_t a = bit(kObject) | bit(kDim) | bit(kPlay) | bit(kPrefetch) | bit(kConfig);
        if (type == kQueuePlayer) {
            a |= bit(kQueue) | bit(kVolume) | bit(kRate) | (pcm.channels > 1 ? bit(kMuteSolo) : 0);
        }
        if (type == kFilePlayer) a |= bit(kSeek) | bit(kVolume) | bit(kMuteSolo) | bit(kRate);
        if (type == kDecoder) a |= bit(kQueue) | bit(kSeek) | bit(kMetadata);
        return a;
    }

    // AOSP's android_player_volumeUpdate: level in millibels, mute, channel
    // mute/solo (stereo and wider only), and the stereo position -- a pan law
    // for mono, a balance for stereo.
    void update_gains() {
        // Millibels to amplitude, as sles_to_android_amplification.
        const float amp = static_cast<float>(std::pow(10.0, level / 2000.0));
        float l = amp, r = amp;
        const int ch = channel_count();
        if (pan_on) {
            if (ch == 1) {
                const double theta = (1000 + pan) * (3.14159265358979323846 / 4) / 1000.0;
                l *= static_cast<float>(std::cos(theta));
                r *= static_cast<float>(std::sin(theta));
            } else if (pan > 0) {
                l *= (1000 - pan) / 1000.0f;
            } else {
                r *= (1000 + pan) / 1000.0f;
            }
        }
        bool hear_l = true, hear_r = true;
        if (ch >= 2) {
            hear_l = !(mute_mask & 1) && ((solo_mask & 1) || !(solo_mask & 2));
            hear_r = !(mute_mask & 2) && ((solo_mask & 2) || !(solo_mask & 1));
        }
        if (muted || !hear_l) l = 0;
        if (muted || !hear_r) r = 0;
        std::lock_guard<std::mutex> hold(ring_lock);
        gain_l = l;
        gain_r = r;
    }

    double ratio() const {
        const int out = audio::output_rate();
        return out > 0 && in_rate() > 0 ? static_cast<double>(in_rate()) * rate / 1000.0 / out : 1.0;
    }

    // The source frame being heard now: what the resampler took, less what
    // waits in the ring.
    int64_t heard_frame() {
        size_t queued;
        {
            std::lock_guard<std::mutex> hold(ring_lock);
            queued = ring.size();
        }
        const double at = resampler.position() - queued * resampler.ratio();
        return origin + std::max<int64_t>(0, static_cast<int64_t>(at));
    }

    uint32_t position_ms() {
        if (type == kDecoder) return in_rate() ? static_cast<uint32_t>(feed * 1000 / in_rate()) : 0;
        if (in_rate() <= 0) return 0;
        int64_t f = heard_frame();
        if (clip && clip->frames()) {
            // The timeline runs on through loops; the file's frame is its remainder.
            const int64_t n = static_cast<int64_t>(clip->frames());
            f = at_end ? n : wraps ? f % n : std::min(f, n);
        }
        return static_cast<uint32_t>(f * 1000 / in_rate());
    }

    // Starts again at source frame `at`: nothing queued for the mixer, the
    // resampler empty.
    void restart_at(int64_t at) {
        {
            std::lock_guard<std::mutex> hold(ring_lock);
            ring.clear();
            stalled = false;
            full_cycle = false;
        }
        resampler.reset();
        origin = feed = at;
        flushed_end = false;
        wraps = 0;
        at_end = false;
        reset_timing(in_rate() ? at * 1000 / in_rate() : 0);
    }

    void set_audible() {
        const bool on = transport == kPlaying && (type != kFilePlayer || prep == kReady);
        std::lock_guard<std::mutex> hold(ring_lock);
        audible = on;
    }

    void post_prefetch(uint32_t events) {
        if (!(exposed & bit(kPrefetch)) || !prefetch_cb) return;
        auto cb = prefetch_cb;
        void* ctx = prefetch_ctx;
        Itf* caller = itf(kPrefetch);
        core->post(shared_from_this(), [cb, ctx, caller, events] { cb(caller, ctx, events); });
    }

    SLresult realize() override;
    void teardown() override;
    void prepare();
    void prepared(std::shared_ptr<const audio::Clip> c);
    bool service() override;
    bool service_output();
    bool service_decoder();
};

bool PlayerSource::mix(float* out, size_t frames, int) {
    Player* p = player_;
    bool wake = false;
    {
        std::lock_guard<std::mutex> hold(p->ring_lock);
        if (p->detached) return false;
        if (!p->audible) return true;
        const size_t n = p->ring.mix_into(out, frames, p->gain_l, p->gain_r);
        if (n < frames && p->full_cycle && !p->stalled) p->stalled = true;
        p->full_cycle = n == frames;
        wake = true;
    }
    if (wake) p->core->wake();
    return true;
}

SLresult Player::realize() {
    core = engine->core;
    if (type == kQueuePlayer) {
        downmix = audio::pcm::downmix_for(pcm.channels, pcm.mask);
        prep = kReady;
        prefetch_status = kPrefetchSufficient;
        fill_level = 1000;
    }
    if (type != kDecoder) {
        // Ring room for the widest lead: a tenth of a second of the output.
        ring = Ring(static_cast<size_t>(std::max(audio::output_rate(), 8000) / 5));
        source = std::make_shared<PlayerSource>(this);
        audio::source_add(source);
    }
    update_gains();
    core->add(std::static_pointer_cast<Media>(shared_from_this()));
    return kSuccess;
}

void Player::teardown() {
    cancel = true;
    if (core) core->remove(this);
    if (source) {
        {
            std::lock_guard<std::mutex> hold(ring_lock);
            detached = true;
            audible = false;
        }
        audio::source_remove(source.get());
    }
}

// Android prepares a file player when it first leaves STOPPED: the file is
// decoded on a thread of its own, and the outcome goes to the prefetch
// interface and the callback thread.
void Player::prepare() {
    if (prep != kUnprepared) return;
    prep = kPreparing;
    auto self = std::static_pointer_cast<Player>(shared_from_this());
    std::thread([self] {
        std::vector<uint8_t> bytes;
        bool read = true;
        if (!self->uri.empty()) {
            if (starts_with_ci(self->uri, "http://") || starts_with_ci(self->uri, "https://")) {
                read = http_get(self->uri, &bytes, self->cancel);
            } else {
                std::string path = starts_with_ci(self->uri, "file://") ? url_decode(self->uri.substr(7)) : self->uri;
                read = read_host_file(android_host_path(path.c_str()), &bytes);
            }
        } else {
            bytes = std::move(self->encoded);
        }
        std::shared_ptr<const audio::Clip> clip;
        if (read && !self->cancel) clip = audio::decode(bytes.data(), bytes.size());
        if (!clip) log("prepare failed: %s", read ? "the file does not decode" : "the file cannot be read");
        self->prepared(std::move(clip));
    }).detach();
}

void Player::prepared(std::shared_ptr<const audio::Clip> c) {
    std::lock_guard<std::mutex> hold(lock);
    if (dead) return;
    if (!c || c->frames() == 0) {
        prep = kFailed;
        prefetch_status = kPrefetchUnderflow;
        fill_level = 0;
        // Android's prefetch error: one callback with both events, level 0
        // and UNDERFLOW, only when both are in the mask.
        if ((prefetch_mask & (kPrefetchStatusChange | kPrefetchFillLevelChange)) ==
            (kPrefetchStatusChange | kPrefetchFillLevelChange)) {
            post_prefetch(kPrefetchStatusChange | kPrefetchFillLevelChange);
        }
        return;
    }
    clip = std::move(c);
    prep = kReady;
    if (pending_seek_ms != kTimeUnknown) {
        restart_at(std::min<int64_t>(static_cast<int64_t>(pending_seek_ms) * clip->rate / 1000,
                                     static_cast<int64_t>(clip->frames())));
        pending_seek_ms = kTimeUnknown;
    } else {
        restart_at(0);
    }
    fill_level = 1000;
    prefetch_status = kPrefetchSufficient;
    // Separate callbacks, the level first, as Android's player sends them.
    if (prefetch_mask & kPrefetchFillLevelChange) post_prefetch(kPrefetchFillLevelChange);
    if (prefetch_mask & kPrefetchStatusChange) post_prefetch(kPrefetchStatusChange);
    update_gains();
    set_audible();
    log("prepared: %u frames at %d Hz (%u ms)", static_cast<unsigned>(clip->frames()), clip->rate,
        static_cast<unsigned>(clip->frames() * 1000 / clip->rate));
    core->wake();
}

bool Player::service() {
    std::lock_guard<std::mutex> hold(lock);
    if (dead || state != kRealized) return false;
    return type == kDecoder ? service_decoder() : service_output();
}

// Keeps the ring a little over two device periods ahead of the mixer.
bool Player::service_output() {
    if (transport != kPlaying || prep != kReady) return false;
    const int out_rate = audio::output_rate();
    resampler.set_ratio(ratio());
    const size_t lead =
        std::clamp<size_t>(audio::output_period_frames() * 2 + static_cast<size_t>(out_rate / 200),
                           static_cast<size_t>(out_rate / 100), static_cast<size_t>(out_rate / 10));
    bool progress = false;
    bool ran_dry = false;
    std::vector<float>& out = rendered;
    for (int guard = 0; guard < 256; ++guard) {
        size_t have, room;
        {
            std::lock_guard<std::mutex> hold(ring_lock);
            have = ring.size();
            room = ring.room();
        }
        if (have >= lead || room == 0) break;
        out.resize(std::min(lead - have, room) * 2);
        const size_t n = resampler.pull(out.data(), out.size() / 2);
        if (n > 0) {
            std::lock_guard<std::mutex> hold(ring_lock);
            ring.write(out.data(), n);
            continue;
        }
        // The resampler wants more of the source.
        if (type == kQueuePlayer) {
            if (queue.empty()) {
                ran_dry = true;
                break;
            }
            Buf& front = queue.front();
            const size_t fb = pcm.frame_bytes();
            const size_t frames = std::min<size_t>((front.size - consumed) / fb, 1024);
            scratch.clear();
            if (frames > 0) {
                audio::pcm::to_stereo(pcm.sample, downmix, front.data + consumed, frames, &scratch);
                resampler.push(scratch.data(), frames);
                consumed += static_cast<uint32_t>(frames * fb);
            }
            if (front.size - consumed < fb) {  // taken whole (a partial last frame is dropped)
                queue.pop_front();
                consumed = 0;
                ++queue_index;
                post_queue_done();
            }
            progress = true;
        } else {
            const int64_t n_frames = static_cast<int64_t>(clip->frames());
            if (feed >= n_frames && looping) {
                feed = 0;  // the timeline goes on: position_ms takes it modulo the file
                ++wraps;
            }
            if (feed < n_frames) {
                const size_t take = static_cast<size_t>(std::min<int64_t>(n_frames - feed, 1024));
                resampler.push(&clip->samples[static_cast<size_t>(feed) * 2], take);
                feed += static_cast<int64_t>(take);
            } else if (!flushed_end) {
                resampler.push_silence(static_cast<size_t>(resampler.half_width()) + 1);
                flushed_end = true;
            } else {
                ran_dry = true;
                break;
            }
            progress = true;
        }
    }

    bool stalled_now;
    size_t queued;
    {
        std::lock_guard<std::mutex> hold(ring_lock);
        stalled_now = stalled;
        stalled = false;
        queued = ring.size();
    }
    // An underrun with something still to play (not a queue run dry, not a
    // file's end): HEADSTALLED, as Android's AudioTrack underrun event.
    if (stalled_now && (type == kQueuePlayer ? !queue.empty() : !flushed_end)) post_event(kHeadStalled);
    if (type == kQueuePlayer) {
        // Starved: Android sends HEADATEND and keeps PLAYING; the prefetch
        // status drops to UNDERFLOW until the next Enqueue.
        if (ran_dry && queued == 0 && !starved && queue.empty()) {
            starved = true;
            post_event(kHeadAtEnd);
            if (exposed & bit(kPrefetch)) {
                prefetch_status = kPrefetchUnderflow;
                fill_level = 0;
                if (prefetch_mask & kPrefetchStatusChange) post_prefetch(kPrefetchStatusChange);
                if (prefetch_mask & kPrefetchFillLevelChange) post_prefetch(kPrefetchFillLevelChange);
            }
        }
    } else if (ran_dry && queued == 0 && flushed_end) {
        // The file's end has been heard: HEADATEND, and Android pauses it.
        transport = kPaused;
        at_end = true;
        set_audible();
        timing(static_cast<int64_t>(clip->frames() * 1000 / clip->rate));
        post_event(kHeadAtEnd);
        return progress;
    }
    timing(position_ms());
    return progress;
}

// Decode to PCM: fills each buffer the app enqueues with the next 16-bit
// stereo frames of the file, as fast as the app hands buffers over.
bool Player::service_decoder() {
    if (transport != kPlaying || prep != kReady) return false;
    bool progress = false;
    const int64_t n = static_cast<int64_t>(clip->frames());
    for (int i = 0; i < 8 && !queue.empty(); ++i) {
        if (feed >= n) {
            if (looping) {
                feed = 0;
            } else {
                transport = kPaused;
                post_event(kHeadAtEnd);
                return progress;
            }
        }
        Buf& front = queue.front();
        const size_t frames = front.size / 4;
        const size_t take = static_cast<size_t>(std::min<int64_t>(n - feed, static_cast<int64_t>(frames)));
        for (size_t f = 0; f < take * 2; ++f) {
            audio::pcm::write_sample(audio::pcm::Sample::kS16, clip->samples[static_cast<size_t>(feed) * 2 + f],
                                     front.data + f * 2);
        }
        std::memset(front.data + take * 4, 0, front.size - take * 4);
        feed += static_cast<int64_t>(take);
        queue.pop_front();
        ++queue_index;
        post_queue_done();
        progress = true;
    }
    timing(position_ms());
    return progress;
}

// ---- audio recorders ---------------------------------------------------------------------

struct Recorder : Media {
    Recorder() : Media(kRecorderClass) {}
    Pcm pcm;
    std::shared_ptr<audio::Capture> capture;
    bool capturing = false;
    int64_t recorded = 0;  // frames written into the app's buffers since STOPPED
    bool overrun = false;
    std::vector<float> scratch;
    std::atomic<bool> recording{false};

    uint32_t allowed() const override { return bit(kObject) | bit(kDim) | bit(kRecord) | bit(kQueue) | bit(kConfig); }
    bool polled() const override { return recording.load(); }

    SLresult realize() override {
        core = engine->core;
        if (!audio::record_audio_permitted()) {
            log("CreateAudioRecorder: Realize refused: the app does not hold android.permission.RECORD_AUDIO");
            return kContentUnsupported;
        }
        if (!audio::microphone_allowed()) {
            log("CreateAudioRecorder: Realize refused: Windows' privacy settings deny the microphone");
            return kContentUnsupported;
        }
        capture = audio::capture_create(pcm.rate, pcm.channels, pcm.rate / 10);
        if (!capture || !audio::capture_start(capture.get())) {
            log("CreateAudioRecorder: Realize refused: no microphone could be opened");
            capture.reset();
            return kContentUnsupported;
        }
        audio::capture_stop(capture.get());
        core->add(std::static_pointer_cast<Media>(shared_from_this()));
        return kSuccess;
    }
    void teardown() override {
        if (core) core->remove(this);
        if (capture && capturing) audio::capture_stop(capture.get());
        capturing = false;
        recording = false;
    }
    uint32_t position_ms() const { return static_cast<uint32_t>(recorded * 1000 / pcm.rate); }

    bool service() override {
        std::lock_guard<std::mutex> hold(lock);
        if (dead || state != kRealized || transport != kPlaying || !capture) return false;
        bool progress = false;
        const size_t fb = pcm.frame_bytes();
        const size_t sb = audio::pcm::sample_bytes(pcm.sample);
        while (!queue.empty()) {
            Buf& front = queue.front();
            const size_t want = (front.size - consumed) / fb;
            scratch.resize(want * pcm.channels);
            const size_t got = audio::capture_read(capture.get(), scratch.data(), scratch.size(), false) / pcm.channels;
            for (size_t s = 0; s < got * pcm.channels; ++s) {
                audio::pcm::write_sample(pcm.sample, scratch[s], front.data + consumed + s * sb);
            }
            consumed += static_cast<uint32_t>(got * fb);
            recorded += static_cast<int64_t>(got);
            if (front.size - consumed < fb) {
                queue.pop_front();
                consumed = 0;
                ++queue_index;
                post_queue_done();
                progress = true;
                overrun = false;
                continue;
            }
            break;
        }
        // No buffer to record into: Android's AudioRecord overruns, and the
        // recorder sends HEADSTALLED.
        if (queue.empty() && !overrun) {
            overrun = true;
            post_event(kHeadStalled);
        }
        timing(position_ms());
        return progress;
    }
};

// ---- the callback thread's loop ----------------------------------------------------------

void run_core(std::shared_ptr<Core> core) {
    for (;;) {
        std::vector<std::shared_ptr<Media>> media;
        {
            std::unique_lock<std::mutex> hold(core->m);
            bool poll = false;
            for (const auto& x : core->media) poll = poll || x->polled();
            core->cv.wait_for(hold, std::chrono::milliseconds(poll ? 5 : 250),
                              [&] { return core->kick || core->quit; });
            // An engine destroyed before its players keeps serving them.
            if (core->quit && core->events.empty() && core->media.empty()) break;
            core->kick = false;
        }
        for (int round = 0; round < 64; ++round) {
            {
                std::lock_guard<std::mutex> hold(core->m);
                media = core->media;
            }
            bool more = false;
            for (const auto& x : media) {
                if (!x->dead) more = x->service() || more;
            }
            media.clear();
            // The callbacks, one at a time, in order, with nothing locked.
            for (;;) {
                std::pair<std::shared_ptr<Object>, std::function<void()>> ev;
                {
                    std::lock_guard<std::mutex> hold(core->m);
                    if (core->events.empty()) break;
                    ev = std::move(core->events.front());
                    core->events.pop_front();
                    core->running = ev.first.get();
                }
                if (!ev.first->dead) ev.second();
                {
                    std::lock_guard<std::mutex> hold(core->m);
                    core->running = nullptr;
                }
                core->idle.notify_all();
                more = true;
            }
            if (!more) break;
            if (round == 63) core->wake();  // a busy queue: come back without waiting
        }
    }
}

// ---- SLObjectItf ---------------------------------------------------------------------------

void post_object_event(Object* o, SLresult result) {
    if (!o->object_cb || !o->core) return;
    auto cb = o->object_cb;
    const void* ctx = o->object_ctx;
    Itf* caller = o->itf(kObject);
    const uint32_t state = o->state;
    o->core->post(o->shared_from_this(),
                  [cb, ctx, caller, result, state] {
                      cb(caller, ctx, kObjectEventAsyncTermination, result, state, nullptr);
                  });
}

GUEST SLresult Object_Realize(Itf* self, uint32_t async) {
    Object* o = self->obj;
    SLresult r;
    {
        std::lock_guard<std::mutex> hold(o->lock);
        if (o->state == kRealized) {
            // Android's shared engine: realizing it again is not an error.
            return o->cls.id == kObjEngine ? kSuccess : kPreconditionsViolated;
        }
        r = o->realize();
        if (r == kSuccess) o->state = kRealized;
    }
    if (async) {
        // Realize's work is quick here; the callback still comes on the
        // callback thread, after Realize returned.
        post_object_event(o, r);
        return kSuccess;
    }
    return r;
}

GUEST SLresult Object_Resume(Itf*, uint32_t) { return kPreconditionsViolated; }  // never suspended

GUEST SLresult Object_GetState(Itf* self, uint32_t* state) {
    if (!state) return kParameterInvalid;
    std::lock_guard<std::mutex> hold(self->obj->lock);
    *state = self->obj->state;
    return kSuccess;
}

GUEST SLresult Object_GetInterface(Itf* self, const Guid* id, Itf** out) {
    if (!out) return kParameterInvalid;
    *out = nullptr;
    if (!id) return kParameterInvalid;
    Object* o = self->obj;
    const int k = kind_of(o->cls.id, id);
    if (k < 0) return kFeatureUnsupported;
    std::lock_guard<std::mutex> hold(o->lock);
    const uint32_t b = bit(static_cast<Kind>(k));
    if (!(o->exposed & b)) return kFeatureUnsupported;
    if (o->state != kRealized && !(o->cls.early & b)) return kPreconditionsViolated;
    *out = o->itf(static_cast<Kind>(k));
    return kSuccess;
}

GUEST SLresult Object_RegisterCallback(Itf* self, void* cb, void* ctx) {
    Object* o = self->obj;
    std::lock_guard<std::mutex> hold(o->lock);
    o->object_cb = guest_fn<ObjectCallback>(cb);
    o->object_ctx = ctx;
    return kSuccess;
}

GUEST void Object_AbortAsyncOperation(Itf*) {}  // Realize finishes before it returns

void destroy_object(Object* o) {
    if (o->cls.id == kObjEngine) {
        std::lock_guard<std::mutex> hold(g_engine_lock);
        auto* e = static_cast<Engine*>(o);
        if (--e->refs > 0) return;
        if (g_engine.get() == e) g_engine.reset();
    }
    std::shared_ptr<Object> keep = unpublish(o);
    if (!keep) return;
    {
        std::lock_guard<std::mutex> hold(o->lock);
        o->dead = true;
    }
    o->teardown();
    // A callback of this object running on the callback thread finishes
    // first, unless that callback is what destroys it.
    if (o->core && o->cls.id != kObjEngine) o->core->wait_idle(o);
}

GUEST void Object_Destroy(Itf* self) { destroy_object(self->obj); }

GUEST SLresult Object_SetPriority(Itf* self, int32_t priority, uint32_t preemptable) {
    std::lock_guard<std::mutex> hold(self->obj->lock);
    self->obj->priority = priority;
    self->obj->preemptable = preemptable;
    return kSuccess;
}

GUEST SLresult Object_GetPriority(Itf* self, int32_t* priority, uint32_t* preemptable) {
    if (!priority || !preemptable) return kParameterInvalid;
    std::lock_guard<std::mutex> hold(self->obj->lock);
    *priority = self->obj->priority;
    *preemptable = self->obj->preemptable;
    return kSuccess;
}

GUEST SLresult Object_SetLossOfControlInterfaces(Itf*, int16_t, const Guid**, uint32_t) { return kSuccess; }

const void* const kObjectVtbl[] = {
    reinterpret_cast<const void*>(&Object_Realize),
    reinterpret_cast<const void*>(&Object_Resume),
    reinterpret_cast<const void*>(&Object_GetState),
    reinterpret_cast<const void*>(&Object_GetInterface),
    reinterpret_cast<const void*>(&Object_RegisterCallback),
    reinterpret_cast<const void*>(&Object_AbortAsyncOperation),
    reinterpret_cast<const void*>(&Object_Destroy),
    reinterpret_cast<const void*>(&Object_SetPriority),
    reinterpret_cast<const void*>(&Object_GetPriority),
    reinterpret_cast<const void*>(&Object_SetLossOfControlInterfaces),
};

// ---- SLDynamicInterfaceManagementItf -------------------------------------------------------

void post_dyn_event(Object* o, SLresult result, const Guid* id) {
    if (!o->dyn_cb || !o->core) return;
    auto cb = o->dyn_cb;
    void* ctx = o->dyn_ctx;
    Itf* caller = o->itf(kDim);
    o->core->post(o->shared_from_this(),
                  [cb, ctx, caller, result, id] { cb(caller, ctx, kDynItfEventAsyncTermination, result, id); });
}

GUEST SLresult Dim_AddInterface(Itf* self, const Guid* id, uint32_t async) {
    if (!id) return kParameterInvalid;
    Object* o = self->obj;
    const int k = kind_of(o->cls.id, id);
    SLresult r;
    {
        std::lock_guard<std::mutex> hold(o->lock);
        const uint32_t b = k >= 0 ? bit(static_cast<Kind>(k)) : 0;
        if (!b || !(o->cls.dynamic & b) || !(o->allowed() & b)) {
            r = kFeatureUnsupported;
        } else if (o->exposed & b) {
            r = kPreconditionsViolated;
        } else {
            o->exposed |= b;
            r = kSuccess;
        }
    }
    if (async && r == kSuccess) {
        post_dyn_event(o, r, id);
        return kSuccess;
    }
    return r;
}

GUEST SLresult Dim_RemoveInterface(Itf* self, const Guid* id) {
    if (!id) return kParameterInvalid;
    Object* o = self->obj;
    const int k = kind_of(o->cls.id, id);
    std::lock_guard<std::mutex> hold(o->lock);
    const uint32_t b = k >= 0 ? bit(static_cast<Kind>(k)) : 0;
    if (!b || !(o->cls.dynamic & b)) return kFeatureUnsupported;
    if (!(o->exposed & b)) return kPreconditionsViolated;
    o->exposed &= ~b;
    return kSuccess;
}

GUEST SLresult Dim_ResumeInterface(Itf*, const Guid* id, uint32_t) {
    return id ? kPreconditionsViolated : kParameterInvalid;  // nothing is ever suspended
}

GUEST SLresult Dim_RegisterCallback(Itf* self, void* cb, void* ctx) {
    Object* o = self->obj;
    std::lock_guard<std::mutex> hold(o->lock);
    o->dyn_cb = guest_fn<DynCallback>(cb);
    o->dyn_ctx = ctx;
    return kSuccess;
}

const void* const kDimVtbl[] = {
    reinterpret_cast<const void*>(&Dim_AddInterface),
    reinterpret_cast<const void*>(&Dim_RemoveInterface),
    reinterpret_cast<const void*>(&Dim_ResumeInterface),
    reinterpret_cast<const void*>(&Dim_RegisterCallback),
};

// ---- SLEngineItf -------------------------------------------------------------------------

void init_vtables(Object* o);

// Creation's common tail: interfaces, the engine reference, publication.
template <typename T>
SLresult finish_create(Itf* engine_itf, std::shared_ptr<T> o, uint32_t n, const Guid* const* ids,
                       const uint32_t* required, Itf** out) {
    uint32_t exposed = 0;
    const SLresult r = choose_interfaces(*o, n, ids, required, &exposed);
    if (r != kSuccess) return r;
    o->exposed = exposed;
    o->engine = std::static_pointer_cast<Engine>(engine_itf->obj->shared_from_this());
    o->core = o->engine->core;
    init_vtables(o.get());
    *out = o->itf(kObject);
    publish(std::move(o));
    return kSuccess;
}

GUEST SLresult Engine_CreateLEDDevice(Itf*, Itf** out, uint32_t, uint32_t, const Guid* const*, const uint32_t*) {
    if (out) *out = nullptr;
    return out ? kFeatureUnsupported : kParameterInvalid;
}

GUEST SLresult Engine_CreateAudioPlayer(Itf* self, Itf** out, const DataSource* src, const DataSource* sink,
                                        uint32_t n, const Guid* const* ids, const uint32_t* required) {
    if (!out) return kParameterInvalid;
    *out = nullptr;
    const char* why = "";
    auto refuse = [&](SLresult r) {
        log("CreateAudioPlayer refused: %s (SL_RESULT %u)", why, r);
        return r;
    };
    if (!src || !src->locator) return why = "no data source", refuse(kParameterInvalid);
    if (!sink || !sink->locator) return why = "no data sink", refuse(kParameterInvalid);
    const uint32_t src_type = *static_cast<const uint32_t*>(src->locator);
    const uint32_t sink_type = *static_cast<const uint32_t*>(sink->locator);
    const bool src_queue = src_type == kLocBufferQueue || src_type == kLocAndroidSimpleBq;
    const bool sink_queue = sink_type == kLocBufferQueue || sink_type == kLocAndroidSimpleBq;
    switch (src_type) {
    case kLocUri:
    case kLocAndroidFd:
    case kLocBufferQueue:
    case kLocAndroidSimpleBq: break;
    case kLocAddress:
    case kLocIoDevice:
    case kLocOutputMix:
    case kLocMidiBufferQueue:
    case kLocAndroidBq: return why = "a data source this player cannot play", refuse(kContentUnsupported);
    default: return why = "an unknown data locator", refuse(kParameterInvalid);
    }
    if (sink_type == kLocOutputMix) {
        const auto* mix = static_cast<const LocOutputMix*>(sink->locator);
        if (!mix->mix || mix->mix->obj->cls.id != kObjOutputMix) {
            return why = "no output mix", refuse(kParameterInvalid);
        }
    } else if (!sink_queue) {
        return why = "a data sink other than an output mix or a buffer queue", refuse(kParameterInvalid);
    }

    std::shared_ptr<Player> p;
    if (src_queue) {
        if (sink_type != kLocOutputMix) {
            return why = "a buffer queue played to a buffer queue", refuse(kParameterInvalid);
        }
        const auto* bq = static_cast<const LocBufferQueue*>(src->locator);
        if (bq->buffers < 1 || bq->buffers > 255) return why = "1-255 buffers", refuse(kParameterInvalid);
        if (!src->format) return why = "no PCM format", refuse(kParameterInvalid);
        p = std::make_shared<Player>(Player::kQueuePlayer);
        const SLresult r = parse_pcm(src->format, true, &p->pcm, &why);
        if (r != kSuccess) return refuse(r);
        p->queue_slots = bq->buffers;
    } else {
        if (src->format && *static_cast<const uint32_t*>(src->format) != kFmtMime) {
            return why = "a URI or fd source's format is not MIME", refuse(kContentUnsupported);
        }
        if (sink_queue) {
            const auto* bq = static_cast<const LocBufferQueue*>(sink->locator);
            if (bq->buffers < 1 || bq->buffers > 255) return why = "1-255 buffers", refuse(kParameterInvalid);
            if (!sink->format || *static_cast<const uint32_t*>(sink->format) != kFmtPcm) {
                return why = "decoding to anything but SL_DATAFORMAT_PCM", refuse(kParameterInvalid);
            }
            p = std::make_shared<Player>(Player::kDecoder);
            p->queue_slots = bq->buffers;
        } else {
            p = std::make_shared<Player>(Player::kFilePlayer);
        }
        if (src_type == kLocUri) {
            const auto* u = static_cast<const LocUri*>(src->locator);
            if (!u->uri) return why = "no URI", refuse(kParameterInvalid);
            p->uri = u->uri;
            const bool known = starts_with_ci(p->uri, "file://") || starts_with_ci(p->uri, "http://") ||
                               starts_with_ci(p->uri, "https://") || (!p->uri.empty() && p->uri[0] == '/');
            if (!known) return why = "a URI scheme other than file, http or https", refuse(kContentUnsupported);
        } else {
            const auto* fd = static_cast<const LocFd*>(src->locator);
            // The bytes are taken now: Android dups the descriptor at Realize, and
            // an app may close it once the player exists.
            if (!read_guest_fd(fd->fd, fd->offset, fd->length, &p->encoded)) {
                return why = "the descriptor cannot be read", refuse(kParameterInvalid);
            }
        }
    }
    // A queue cannot be required of a file player, nor seeking of a queue:
    // choose_interfaces goes by what this player allows.
    const SLresult r = finish_create(self, p, n, ids, required, out);
    if (r != kSuccess) return why = "a required interface this player does not have", refuse(r);
    if (p->type == Player::kQueuePlayer) {
        log("CreateAudioPlayer: buffer queue of %u, %d Hz, %d channel(s), %u-byte samples", p->queue_slots,
            p->pcm.rate, p->pcm.channels, static_cast<unsigned>(audio::pcm::sample_bytes(p->pcm.sample)));
    } else {
        log("CreateAudioPlayer: %s %s%s", p->uri.empty() ? "fd" : "URI", p->uri.empty() ? "" : p->uri.c_str(),
            p->type == Player::kDecoder ? ", decoded to a buffer queue" : "");
    }
    return kSuccess;
}

GUEST SLresult Engine_CreateAudioRecorder(Itf* self, Itf** out, const DataSource* src, const DataSource* sink,
                                          uint32_t n, const Guid* const* ids, const uint32_t* required) {
    if (!out) return kParameterInvalid;
    *out = nullptr;
    const char* why = "";
    auto refuse = [&](SLresult r) {
        log("CreateAudioRecorder refused: %s (SL_RESULT %u)", why, r);
        return r;
    };
    if (!src || !src->locator || !sink || !sink->locator) return why = "no source or sink", refuse(kParameterInvalid);
    const auto* io = static_cast<const LocIoDevice*>(src->locator);
    if (io->type != kLocIoDevice) return why = "the source is not an I/O device", refuse(kParameterInvalid);
    if (io->device_type != kIoDeviceAudioInput) {
        return why = "the device is not an audio input", refuse(kParameterInvalid);
    }
    if (io->device_id != kDefaultAudioInput) return why = "not the default input", refuse(kParameterInvalid);
    const auto* bq = static_cast<const LocBufferQueue*>(sink->locator);
    if (bq->type != kLocAndroidSimpleBq) {
        return why = "the sink is not an Android simple buffer queue", refuse(kParameterInvalid);
    }
    if (bq->buffers < 1 || bq->buffers > 255) return why = "1-255 buffers", refuse(kParameterInvalid);
    if (!sink->format) return why = "no PCM format", refuse(kParameterInvalid);
    auto rec = std::make_shared<Recorder>();
    SLresult r = parse_pcm(sink->format, false, &rec->pcm, &why);
    if (r != kSuccess) return refuse(r == kContentUnsupported && *static_cast<const uint32_t*>(sink->format) == kFmtMime
                                         ? kParameterInvalid
                                         : r);
    rec->queue_slots = bq->buffers;
    r = finish_create(self, rec, n, ids, required, out);
    if (r != kSuccess) return why = "interfaces", refuse(r);
    log("CreateAudioRecorder: %d Hz, %d channel(s)", rec->pcm.rate, rec->pcm.channels);
    return kSuccess;
}

// The other Create calls take more arguments; System V lets a callee ignore them.
GUEST SLresult Engine_CreateUnsupported(Itf*, Itf** out) {
    if (out) *out = nullptr;
    return out ? kFeatureUnsupported : kParameterInvalid;
}

GUEST SLresult Engine_CreateOutputMix(Itf* self, Itf** out, uint32_t n, const Guid* const* ids,
                                      const uint32_t* required) {
    if (!out) return kParameterInvalid;
    *out = nullptr;
    auto mix = std::make_shared<OutputMix>();
    const SLresult r = finish_create(self, mix, n, ids, required, out);
    if (r != kSuccess) log("CreateOutputMix refused: a required interface (reverb, equalizer...) is not available");
    return r;
}

GUEST SLresult Engine_QueryNumSupportedInterfaces(Itf*, uint32_t object_id, uint32_t* count) {
    if (!count) return kParameterInvalid;
    const ClassInfo* c = class_of(object_id);
    if (!c) {
        *count = 0;
        return kFeatureUnsupported;
    }
    *count = static_cast<uint32_t>(class_iids(*c).size());
    return kSuccess;
}

GUEST SLresult Engine_QuerySupportedInterfaces(Itf*, uint32_t object_id, uint32_t index, const Guid** out) {
    if (!out) return kParameterInvalid;
    *out = nullptr;
    const ClassInfo* c = class_of(object_id);
    if (!c) return kFeatureUnsupported;
    const auto ids = class_iids(*c);
    if (index >= ids.size()) return kParameterInvalid;
    *out = ids[index];
    return kSuccess;
}

// Android names one extension: its SDK level.
std::string extension_name() {
    using Get = int(GUEST*)(const char*, char*);
    auto get = reinterpret_cast<Get>(mini_bionic_resolve("__system_property_get"));
    char sdk[92] = {};
    if (!get || get("ro.build.version.sdk", sdk) <= 0) std::strcpy(sdk, "29");
    return std::string("ANDROID_SDK_LEVEL_") + sdk;
}

GUEST SLresult Engine_QueryNumSupportedExtensions(Itf*, uint32_t* count) {
    if (!count) return kParameterInvalid;
    *count = 1;
    return kSuccess;
}

GUEST SLresult Engine_QuerySupportedExtension(Itf*, uint32_t index, char* name, int16_t* length) {
    if (!length) return kParameterInvalid;
    if (index >= 1) {
        *length = 0;
        return kParameterInvalid;
    }
    const std::string ext = extension_name();
    const int16_t actual = static_cast<int16_t>(ext.size() + 1);
    SLresult r = kSuccess;
    if (name) {
        if (*length <= 0) {
            r = kBufferInsufficient;
        } else if (actual > *length) {
            std::memcpy(name, ext.data(), static_cast<size_t>(*length - 1));
            name[*length - 1] = '\0';
            r = kBufferInsufficient;
        } else {
            std::memcpy(name, ext.c_str(), ext.size() + 1);
        }
    }
    *length = actual;
    return r;
}

GUEST SLresult Engine_IsExtensionSupported(Itf*, const char* name, uint32_t* supported) {
    if (!supported) return kParameterInvalid;
    *supported = 0;
    if (!name) return kParameterInvalid;
    *supported = extension_name() == name;
    return kSuccess;
}

const void* const kEngineVtbl[] = {
    reinterpret_cast<const void*>(&Engine_CreateLEDDevice),
    reinterpret_cast<const void*>(&Engine_CreateLEDDevice),  // CreateVibraDevice: no vibra device either
    reinterpret_cast<const void*>(&Engine_CreateAudioPlayer),
    reinterpret_cast<const void*>(&Engine_CreateAudioRecorder),
    reinterpret_cast<const void*>(&Engine_CreateUnsupported),  // CreateMidiPlayer
    reinterpret_cast<const void*>(&Engine_CreateUnsupported),  // CreateListener
    reinterpret_cast<const void*>(&Engine_CreateUnsupported),  // Create3DGroup
    reinterpret_cast<const void*>(&Engine_CreateOutputMix),
    reinterpret_cast<const void*>(&Engine_CreateUnsupported),  // CreateMetadataExtractor
    reinterpret_cast<const void*>(&Engine_CreateUnsupported),  // CreateExtensionObject
    reinterpret_cast<const void*>(&Engine_QueryNumSupportedInterfaces),
    reinterpret_cast<const void*>(&Engine_QuerySupportedInterfaces),
    reinterpret_cast<const void*>(&Engine_QueryNumSupportedExtensions),
    reinterpret_cast<const void*>(&Engine_QuerySupportedExtension),
    reinterpret_cast<const void*>(&Engine_IsExtensionSupported),
};

// ---- SLEngineCapabilitiesItf ---------------------------------------------------------------

GUEST SLresult Caps_QuerySupportedProfiles(Itf*, uint16_t* profiles) {
    if (!profiles) return kParameterInvalid;
    *profiles = 0;  // none of Phone, Music, Game is implemented whole (Android claims none either)
    return kSuccess;
}

GUEST SLresult Caps_QueryAvailableVoices(Itf*, uint16_t type, int16_t* max, uint32_t* absolute, int16_t* free) {
    if (type != 1) {  // SL_VOICETYPE_2D_AUDIO; there is no MIDI or 3D
        if (type == 2 || type == 4 || type == 8) {
            if (max) *max = 0;
            if (absolute) *absolute = 1;
            if (free) *free = 0;
            return kSuccess;
        }
        return kParameterInvalid;
    }
    size_t players = 0;
    {
        std::lock_guard<std::mutex> hold(g_objects_lock);
        for (const auto& o : g_objects) players += o.second->cls.id == kObjAudioPlayer;
    }
    constexpr int16_t kVoices = 64;  // no hard limit: a mixer source each
    if (max) *max = kVoices;
    if (absolute) *absolute = 0;
    if (free) *free = static_cast<int16_t>(std::max<int>(0, kVoices - static_cast<int>(players)));
    return kSuccess;
}

GUEST SLresult Caps_QueryNumberOfMIDISynthesizers(Itf*, int16_t* n) {
    if (!n) return kParameterInvalid;
    *n = 0;
    return kSuccess;
}

GUEST SLresult Caps_QueryAPIVersion(Itf*, int16_t* major, int16_t* minor, int16_t* step) {
    if (!major || !minor || !step) return kParameterInvalid;
    *major = 1;
    *minor = 0;
    *step = 1;
    return kSuccess;
}

// No LED or vibra devices: asked for the count, 0; asked for one, none is valid.
GUEST SLresult Caps_QueryDevices(Itf*, uint32_t* index, uint32_t* id, void* descriptor) {
    if (!index || id || descriptor) return kParameterInvalid;
    *index = 0;
    return kSuccess;
}

GUEST SLresult Caps_IsThreadSafe(Itf* self, uint32_t* safe) {
    if (!safe) return kParameterInvalid;
    *safe = static_cast<Engine*>(self->obj)->thread_safe;
    return kSuccess;
}

const void* const kCapsVtbl[] = {
    reinterpret_cast<const void*>(&Caps_QuerySupportedProfiles),
    reinterpret_cast<const void*>(&Caps_QueryAvailableVoices),
    reinterpret_cast<const void*>(&Caps_QueryNumberOfMIDISynthesizers),
    reinterpret_cast<const void*>(&Caps_QueryAPIVersion),
    reinterpret_cast<const void*>(&Caps_QueryDevices),
    reinterpret_cast<const void*>(&Caps_QueryDevices),
    reinterpret_cast<const void*>(&Caps_IsThreadSafe),
};

// ---- SLThreadSyncItf -------------------------------------------------------------------------

GUEST SLresult Sync_Enter(Itf* self) {
    auto* e = static_cast<Engine*>(self->obj);
    std::unique_lock<std::mutex> hold(e->sync_lock);
    if (e->in_section && e->section_owner == std::this_thread::get_id()) return kPreconditionsViolated;
    e->sync_cv.wait(hold, [&] { return !e->in_section; });
    e->in_section = true;
    e->section_owner = std::this_thread::get_id();
    return kSuccess;
}

GUEST SLresult Sync_Exit(Itf* self) {
    auto* e = static_cast<Engine*>(self->obj);
    std::lock_guard<std::mutex> hold(e->sync_lock);
    if (!e->in_section || e->section_owner != std::this_thread::get_id()) return kPreconditionsViolated;
    e->in_section = false;
    e->sync_cv.notify_one();
    return kSuccess;
}

const void* const kSyncVtbl[] = {
    reinterpret_cast<const void*>(&Sync_Enter),
    reinterpret_cast<const void*>(&Sync_Exit),
};

// ---- SLOutputMixItf ------------------------------------------------------------------------

GUEST SLresult Mix_GetDestinationOutputDeviceIDs(Itf*, int32_t* count, uint32_t* ids) {
    if (!count) return kParameterInvalid;
    SLresult r = kSuccess;
    if (ids) {
        if (*count < 1) {
            r = kBufferInsufficient;
        } else {
            ids[0] = kDefaultAudioOutput;
        }
    }
    *count = 1;
    return r;
}

GUEST SLresult Mix_RegisterDeviceChangeCallback(Itf* self, void* cb, void* ctx) {
    auto* m = static_cast<OutputMix*>(self->obj);
    std::lock_guard<std::mutex> hold(m->lock);
    m->device_cb = cb;
    m->device_ctx = ctx;
    return kSuccess;
}

GUEST SLresult Mix_ReRoute(Itf*, int32_t n, const uint32_t* ids) {
    if (n != 1 || !ids) return kParameterInvalid;
    return ids[0] == kDefaultAudioOutput ? kSuccess : kParameterInvalid;
}

const void* const kMixVtbl[] = {
    reinterpret_cast<const void*>(&Mix_GetDestinationOutputDeviceIDs),
    reinterpret_cast<const void*>(&Mix_RegisterDeviceChangeCallback),
    reinterpret_cast<const void*>(&Mix_ReRoute),
};

// ---- SLPlayItf ---------------------------------------------------------------------------

GUEST SLresult Play_SetPlayState(Itf* self, uint32_t s) {
    if (s != kStopped && s != kPaused && s != kPlaying) return kParameterInvalid;
    auto* p = as<Player>(self);
    {
        std::lock_guard<std::mutex> hold(p->lock);
        const uint32_t was = p->transport;
        if (s == was) return kSuccess;
        p->transport = s;
        if (p->type != Player::kQueuePlayer && s != kStopped) p->prepare();
        if (s == kStopped) {
            // Android's AudioTrack stop: the position goes back to 0 and what
            // was taken but not heard is dropped; the queue keeps its buffers.
            // A file goes back to its start.
            p->restart_at(0);
            p->starved = false;
        } else if (s == kPlaying && p->type == Player::kFilePlayer && p->at_end) {
            p->restart_at(0);  // played to its end: start over, as MediaPlayer does
        } else if (s == kPlaying && was == kStopped) {
            p->reset_timing(p->position_ms());
        }
        if (p->type == Player::kDecoder && s == kPlaying && p->clip &&
            p->feed >= static_cast<int64_t>(p->clip->frames())) {
            p->feed = 0;  // decoded to its end: from the start again
        }
        p->set_audible();
    }
    if (p->core) p->core->wake();
    return kSuccess;
}

GUEST SLresult Play_GetPlayState(Itf* self, uint32_t* s) {
    if (!s) return kParameterInvalid;
    auto* p = as<Media>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    *s = p->transport;
    return kSuccess;
}

GUEST SLresult Play_GetDuration(Itf* self, uint32_t* ms) {
    if (!ms) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    *ms = p->clip && p->clip->rate > 0 ? static_cast<uint32_t>(p->clip->frames() * 1000 / p->clip->rate) : kTimeUnknown;
    return kSuccess;
}

GUEST SLresult Play_GetPosition(Itf* self, uint32_t* ms) {
    if (!ms) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    *ms = p->position_ms();
    return kSuccess;
}

GUEST SLresult Events_RegisterCallback(Itf* self, void* cb, void* ctx) {
    auto* m = as<Media>(self);
    std::lock_guard<std::mutex> hold(m->lock);
    m->event_cb = guest_fn<PlayCallback>(cb);
    m->event_ctx = ctx;
    return kSuccess;
}

GUEST SLresult Play_SetCallbackEventsMask(Itf* self, uint32_t mask) {
    if (mask & ~(kHeadAtEnd | kHeadAtMarker | kHeadAtNewPos | kHeadMoving | kHeadStalled)) return kParameterInvalid;
    auto* m = as<Media>(self);
    std::lock_guard<std::mutex> hold(m->lock);
    m->event_mask = mask;
    return kSuccess;
}

GUEST SLresult Events_GetCallbackEventsMask(Itf* self, uint32_t* mask) {
    if (!mask) return kParameterInvalid;
    auto* m = as<Media>(self);
    std::lock_guard<std::mutex> hold(m->lock);
    *mask = m->event_mask;
    return kSuccess;
}

GUEST SLresult Events_SetMarkerPosition(Itf* self, uint32_t ms) {
    if (ms == kTimeUnknown) return kParameterInvalid;
    auto* m = as<Media>(self);
    std::lock_guard<std::mutex> hold(m->lock);
    m->marker = ms;
    return kSuccess;
}

GUEST SLresult Events_ClearMarkerPosition(Itf* self) {
    auto* m = as<Media>(self);
    std::lock_guard<std::mutex> hold(m->lock);
    m->marker = kTimeUnknown;
    return kSuccess;
}

GUEST SLresult Events_GetMarkerPosition(Itf* self, uint32_t* ms) {
    if (!ms) return kParameterInvalid;
    auto* m = as<Media>(self);
    std::lock_guard<std::mutex> hold(m->lock);
    *ms = m->marker;
    return m->marker == kTimeUnknown ? kPreconditionsViolated : kSuccess;
}

GUEST SLresult Events_SetPositionUpdatePeriod(Itf* self, uint32_t ms) {
    if (ms == 0) return kParameterInvalid;
    auto* m = as<Media>(self);
    std::lock_guard<std::mutex> hold(m->lock);
    m->period = ms;
    m->next_period_ms = (m->last_ms / ms + 1) * ms;
    return kSuccess;
}

GUEST SLresult Events_GetPositionUpdatePeriod(Itf* self, uint32_t* ms) {
    if (!ms) return kParameterInvalid;
    auto* m = as<Media>(self);
    std::lock_guard<std::mutex> hold(m->lock);
    *ms = m->period;
    return kSuccess;
}

const void* const kPlayVtbl[] = {
    reinterpret_cast<const void*>(&Play_SetPlayState),
    reinterpret_cast<const void*>(&Play_GetPlayState),
    reinterpret_cast<const void*>(&Play_GetDuration),
    reinterpret_cast<const void*>(&Play_GetPosition),
    reinterpret_cast<const void*>(&Events_RegisterCallback),
    reinterpret_cast<const void*>(&Play_SetCallbackEventsMask),
    reinterpret_cast<const void*>(&Events_GetCallbackEventsMask),
    reinterpret_cast<const void*>(&Events_SetMarkerPosition),
    reinterpret_cast<const void*>(&Events_ClearMarkerPosition),
    reinterpret_cast<const void*>(&Events_GetMarkerPosition),
    reinterpret_cast<const void*>(&Events_SetPositionUpdatePeriod),
    reinterpret_cast<const void*>(&Events_GetPositionUpdatePeriod),
};

// ---- SLAndroidSimpleBufferQueueItf / SLBufferQueueItf -----------------------------------------

GUEST SLresult Queue_Enqueue(Itf* self, const void* buffer, uint32_t size) {
    if (!buffer || size == 0) return kParameterInvalid;
    auto* m = as<Media>(self);
    {
        std::lock_guard<std::mutex> hold(m->lock);
        if (m->queue.size() >= m->queue_slots) return kBufferInsufficient;
        m->queue.push_back({static_cast<uint8_t*>(const_cast<void*>(buffer)), size});
        if (m->cls.id == kObjAudioPlayer) {
            auto* p = static_cast<Player*>(m);
            if (p->starved) {
                // Refilled after running dry: Android's prefetch status goes
                // back to SUFFICIENTDATA.
                p->starved = false;
                if ((p->exposed & bit(kPrefetch)) && p->prefetch_status == kPrefetchUnderflow) {
                    p->prefetch_status = kPrefetchSufficient;
                    p->fill_level = 1000;
                    if (p->prefetch_mask & kPrefetchStatusChange) p->post_prefetch(kPrefetchStatusChange);
                    if (p->prefetch_mask & kPrefetchFillLevelChange) p->post_prefetch(kPrefetchFillLevelChange);
                }
            }
        }
    }
    if (m->core) m->core->wake();
    return kSuccess;
}

GUEST SLresult Queue_Clear(Itf* self) {
    auto* m = as<Media>(self);
    std::lock_guard<std::mutex> hold(m->lock);
    m->queue.clear();
    m->consumed = 0;
    m->queue_index = 0;
    if (m->cls.id == kObjAudioPlayer) {
        auto* p = static_cast<Player*>(m);
        // AudioTrack's flush: what the mixer has not played yet goes too,
        // and so does every pointer into the app's buffers.
        if (p->type == Player::kQueuePlayer) p->restart_at(p->heard_frame());
    }
    return kSuccess;
}

GUEST SLresult Queue_GetState(Itf* self, uint32_t* out) {
    if (!out) return kParameterInvalid;
    auto* m = as<Media>(self);
    std::lock_guard<std::mutex> hold(m->lock);
    out[0] = static_cast<uint32_t>(m->queue.size());
    out[1] = m->queue_index;
    return kSuccess;
}

GUEST SLresult Queue_RegisterCallback(Itf* self, void* cb, void* ctx) {
    auto* m = as<Media>(self);
    std::lock_guard<std::mutex> hold(m->lock);
    if (m->transport != kStopped) return kPreconditionsViolated;  // Android's precondition
    m->queue_cb = guest_fn<QueueCallback>(cb);
    m->queue_ctx = ctx;
    return kSuccess;
}

const void* const kQueueVtbl[] = {
    reinterpret_cast<const void*>(&Queue_Enqueue),
    reinterpret_cast<const void*>(&Queue_Clear),
    reinterpret_cast<const void*>(&Queue_GetState),
    reinterpret_cast<const void*>(&Queue_RegisterCallback),
};

// ---- SLVolumeItf -------------------------------------------------------------------------

template <typename F>
SLresult with_player(Itf* self, F f) {
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    const SLresult r = f(p);
    if (r == kSuccess) p->update_gains();
    return r;
}

GUEST SLresult Volume_SetVolumeLevel(Itf* self, int16_t level) {
    if (level > 0) return kParameterInvalid;
    return with_player(self, [&](Player* p) { return p->level = level, kSuccess; });
}
GUEST SLresult Volume_GetVolumeLevel(Itf* self, int16_t* level) {
    if (!level) return kParameterInvalid;
    return with_player(self, [&](Player* p) { return *level = p->level, kSuccess; });
}
GUEST SLresult Volume_GetMaxVolumeLevel(Itf*, int16_t* level) {
    if (!level) return kParameterInvalid;
    *level = 0;
    return kSuccess;
}
GUEST SLresult Volume_SetMute(Itf* self, uint32_t mute) {
    return with_player(self, [&](Player* p) { return p->muted = mute != 0, kSuccess; });
}
GUEST SLresult Volume_GetMute(Itf* self, uint32_t* mute) {
    if (!mute) return kParameterInvalid;
    return with_player(self, [&](Player* p) { return *mute = p->muted, kSuccess; });
}
GUEST SLresult Volume_EnableStereoPosition(Itf* self, uint32_t on) {
    return with_player(self, [&](Player* p) { return p->pan_on = on != 0, kSuccess; });
}
GUEST SLresult Volume_IsEnabledStereoPosition(Itf* self, uint32_t* on) {
    if (!on) return kParameterInvalid;
    return with_player(self, [&](Player* p) { return *on = p->pan_on, kSuccess; });
}
GUEST SLresult Volume_SetStereoPosition(Itf* self, int16_t pos) {
    if (pos < -1000 || pos > 1000) return kParameterInvalid;
    return with_player(self, [&](Player* p) { return p->pan = pos, kSuccess; });
}
GUEST SLresult Volume_GetStereoPosition(Itf* self, int16_t* pos) {
    if (!pos) return kParameterInvalid;
    return with_player(self, [&](Player* p) { return *pos = p->pan, kSuccess; });
}

const void* const kVolumeVtbl[] = {
    reinterpret_cast<const void*>(&Volume_SetVolumeLevel),
    reinterpret_cast<const void*>(&Volume_GetVolumeLevel),
    reinterpret_cast<const void*>(&Volume_GetMaxVolumeLevel),
    reinterpret_cast<const void*>(&Volume_SetMute),
    reinterpret_cast<const void*>(&Volume_GetMute),
    reinterpret_cast<const void*>(&Volume_EnableStereoPosition),
    reinterpret_cast<const void*>(&Volume_IsEnabledStereoPosition),
    reinterpret_cast<const void*>(&Volume_SetStereoPosition),
    reinterpret_cast<const void*>(&Volume_GetStereoPosition),
};

// ---- SLMuteSoloItf -------------------------------------------------------------------------

SLresult channel_ok(Player* p, uint8_t chan) {
    const int n = p->channel_count();
    if (n == 0) return kPreconditionsViolated;  // not known before the file is prepared
    return chan < n ? kSuccess : kParameterInvalid;
}

GUEST SLresult Mute_SetChannelMute(Itf* self, uint8_t chan, uint32_t mute) {
    return with_player(self, [&](Player* p) {
        const SLresult r = channel_ok(p, chan);
        if (r == kSuccess && chan < 8) {
            p->mute_mask = mute ? (p->mute_mask | (1 << chan)) : (p->mute_mask & ~(1 << chan));
        }
        return r;
    });
}
GUEST SLresult Mute_GetChannelMute(Itf* self, uint8_t chan, uint32_t* mute) {
    if (!mute) return kParameterInvalid;
    return with_player(self, [&](Player* p) {
        const SLresult r = channel_ok(p, chan);
        if (r == kSuccess) *mute = (p->mute_mask >> chan) & 1;
        return r;
    });
}
GUEST SLresult Mute_SetChannelSolo(Itf* self, uint8_t chan, uint32_t solo) {
    return with_player(self, [&](Player* p) {
        const SLresult r = channel_ok(p, chan);
        if (r == kSuccess && chan < 8) {
            p->solo_mask = solo ? (p->solo_mask | (1 << chan)) : (p->solo_mask & ~(1 << chan));
        }
        return r;
    });
}
GUEST SLresult Mute_GetChannelSolo(Itf* self, uint8_t chan, uint32_t* solo) {
    if (!solo) return kParameterInvalid;
    return with_player(self, [&](Player* p) {
        const SLresult r = channel_ok(p, chan);
        if (r == kSuccess) *solo = (p->solo_mask >> chan) & 1;
        return r;
    });
}
GUEST SLresult Mute_GetNumChannels(Itf* self, uint8_t* n) {
    if (!n) return kParameterInvalid;
    return with_player(self, [&](Player* p) {
        if (p->channel_count() == 0) return kPreconditionsViolated;
        *n = static_cast<uint8_t>(p->channel_count());
        return kSuccess;
    });
}

const void* const kMuteSoloVtbl[] = {
    reinterpret_cast<const void*>(&Mute_SetChannelMute), reinterpret_cast<const void*>(&Mute_GetChannelMute),
    reinterpret_cast<const void*>(&Mute_SetChannelSolo), reinterpret_cast<const void*>(&Mute_GetChannelSolo),
    reinterpret_cast<const void*>(&Mute_GetNumChannels),
};

// ---- SLSeekItf ---------------------------------------------------------------------------

GUEST SLresult Seek_SetPosition(Itf* self, uint32_t ms, uint32_t mode) {
    if (mode != kSeekFast && mode != kSeekAccurate) return kParameterInvalid;
    auto* p = as<Player>(self);
    {
        std::lock_guard<std::mutex> hold(p->lock);
        if (!p->clip) {
            p->pending_seek_ms = ms;  // applied once the file is prepared
        } else {
            const int64_t at = std::min<int64_t>(static_cast<int64_t>(ms) * p->clip->rate / 1000,
                                                 static_cast<int64_t>(p->clip->frames()));
            if (p->type == Player::kDecoder) {
                p->feed = at;
            } else {
                p->restart_at(at);
            }
        }
    }
    if (p->core) p->core->wake();
    return kSuccess;
}

// Android loops the whole file only: any other segment is unsupported.
GUEST SLresult Seek_SetLoop(Itf* self, uint32_t enable, uint32_t start, uint32_t end) {
    if (!(start < end)) return kParameterInvalid;
    if (start != 0 && end != kTimeUnknown) return kFeatureUnsupported;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    p->looping = enable != 0;
    return kSuccess;
}

GUEST SLresult Seek_GetLoop(Itf* self, uint32_t* enabled, uint32_t* start, uint32_t* end) {
    if (!enabled || !start || !end) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    *enabled = p->looping;
    *start = 0;
    *end = kTimeUnknown;
    return kSuccess;
}

const void* const kSeekVtbl[] = {
    reinterpret_cast<const void*>(&Seek_SetPosition),
    reinterpret_cast<const void*>(&Seek_SetLoop),
    reinterpret_cast<const void*>(&Seek_GetLoop),
};

// ---- SLPrefetchStatusItf -----------------------------------------------------------------

GUEST SLresult Prefetch_GetPrefetchStatus(Itf* self, uint32_t* status) {
    if (!status) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    *status = p->prefetch_status;
    return kSuccess;
}
GUEST SLresult Prefetch_GetFillLevel(Itf* self, int16_t* level) {
    if (!level) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    *level = p->fill_level;
    return kSuccess;
}
GUEST SLresult Prefetch_RegisterCallback(Itf* self, void* cb, void* ctx) {
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    p->prefetch_cb = guest_fn<PlayCallback>(cb);
    p->prefetch_ctx = ctx;
    return kSuccess;
}
GUEST SLresult Prefetch_SetCallbackEventsMask(Itf* self, uint32_t mask) {
    if (mask & ~(kPrefetchStatusChange | kPrefetchFillLevelChange)) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    p->prefetch_mask = mask;
    return kSuccess;
}
GUEST SLresult Prefetch_GetCallbackEventsMask(Itf* self, uint32_t* mask) {
    if (!mask) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    *mask = p->prefetch_mask;
    return kSuccess;
}
GUEST SLresult Prefetch_SetFillUpdatePeriod(Itf* self, int16_t period) {
    if (period <= 0 || period > 1000) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    p->fill_period = period;
    return kSuccess;
}
GUEST SLresult Prefetch_GetFillUpdatePeriod(Itf* self, int16_t* period) {
    if (!period) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    *period = p->fill_period;
    return kSuccess;
}

const void* const kPrefetchVtbl[] = {
    reinterpret_cast<const void*>(&Prefetch_GetPrefetchStatus),
    reinterpret_cast<const void*>(&Prefetch_GetFillLevel),
    reinterpret_cast<const void*>(&Prefetch_RegisterCallback),
    reinterpret_cast<const void*>(&Prefetch_SetCallbackEventsMask),
    reinterpret_cast<const void*>(&Prefetch_GetCallbackEventsMask),
    reinterpret_cast<const void*>(&Prefetch_SetFillUpdatePeriod),
    reinterpret_cast<const void*>(&Prefetch_GetFillUpdatePeriod),
};

// ---- SLPlaybackRateItf ---------------------------------------------------------------------
// Rate changes pitch with it (SL_RATEPROP_NOPITCHCORAUDIO), 0.5x to 2x, as
// Android's AudioTrack-backed players.

GUEST SLresult Rate_SetRate(Itf* self, int16_t rate) {
    if (rate < kMinRate || rate > kMaxRate) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    p->rate = rate;
    return kSuccess;
}
GUEST SLresult Rate_GetRate(Itf* self, int16_t* rate) {
    if (!rate) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    *rate = p->rate;
    return kSuccess;
}
GUEST SLresult Rate_SetPropertyConstraints(Itf* self, uint32_t props) {
    if (props & ~(kRateSilent | kRateStaggered | kRateNoPitchCor | kRatePitchCor)) return kParameterInvalid;
    if (!(props & kRateNoPitchCor)) return kFeatureUnsupported;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    p->rate_props = props;
    return kSuccess;
}
GUEST SLresult Rate_GetProperties(Itf* self, uint32_t* props) {
    if (!props) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    *props = p->rate_props;
    return kSuccess;
}
GUEST SLresult Rate_GetCapabilitiesOfRate(Itf*, int16_t rate, uint32_t* caps) {
    if (!caps) return kParameterInvalid;
    if (rate < kMinRate || rate > kMaxRate) {
        *caps = 0;
        return kParameterInvalid;
    }
    *caps = kRateNoPitchCor;
    return kSuccess;
}
GUEST SLresult Rate_GetRateRange(Itf*, uint8_t index, int16_t* min, int16_t* max, int16_t* step, uint32_t* caps) {
    if (!min || !max || !step || !caps || index > 0) return kParameterInvalid;
    *min = kMinRate;
    *max = kMaxRate;
    *step = 0;  // continuous
    *caps = kRateNoPitchCor;
    return kSuccess;
}

const void* const kRateVtbl[] = {
    reinterpret_cast<const void*>(&Rate_SetRate),
    reinterpret_cast<const void*>(&Rate_GetRate),
    reinterpret_cast<const void*>(&Rate_SetPropertyConstraints),
    reinterpret_cast<const void*>(&Rate_GetProperties),
    reinterpret_cast<const void*>(&Rate_GetCapabilitiesOfRate),
    reinterpret_cast<const void*>(&Rate_GetRateRange),
};

// ---- SLAndroidConfigurationItf -----------------------------------------------------------
// Set before Realize, as on Android. The stream type and the recording preset
// are kept and answered; the host has one output and one input path. The
// performance mode is answered as granted: every player is pulled by the
// mixer each device period, the shared path Android's LATENCY mode gets.

GUEST SLresult Config_SetConfiguration(Itf* self, const char* key, const void* value, uint32_t size) {
    if (!key || !value || size == 0) return kParameterInvalid;
    auto* m = as<Media>(self);
    std::lock_guard<std::mutex> hold(m->lock);
    if (size < 4) return kBufferInsufficient;
    const uint32_t v = *static_cast<const uint32_t*>(value);
    const bool player = m->cls.id == kObjAudioPlayer;
    SLresult r;
    if (player && std::strcmp(key, kKeyStreamType) == 0) {
        r = static_cast<int32_t>(v) >= 0 && v <= 5 ? kSuccess : kParameterInvalid;
        if (r == kSuccess && m->state != kUnrealized) r = kPreconditionsViolated;
        if (r == kSuccess) m->stream_type = static_cast<int32_t>(v);
    } else if (std::strcmp(key, kKeyPerformanceMode) == 0) {
        r = v <= 3 ? kSuccess : kParameterInvalid;
        if (r == kSuccess && m->state != kUnrealized) r = kPreconditionsViolated;
        if (r == kSuccess) m->performance = v;
    } else if (!player && std::strcmp(key, kKeyRecordingPreset) == 0) {
        r = v <= 5 ? kSuccess : kParameterInvalid;
        if (r == kSuccess && m->state != kUnrealized) r = kPreconditionsViolated;
        if (r == kSuccess) m->preset = v;
    } else {
        r = kParameterInvalid;
    }
    return r;
}

GUEST SLresult Config_GetConfiguration(Itf* self, const char* key, uint32_t* size, void* value) {
    if (!key || !size) return kParameterInvalid;
    auto* m = as<Media>(self);
    std::lock_guard<std::mutex> hold(m->lock);
    const bool player = m->cls.id == kObjAudioPlayer;
    uint32_t v;
    if (player && std::strcmp(key, kKeyStreamType) == 0) {
        v = static_cast<uint32_t>(m->stream_type);
    } else if (std::strcmp(key, kKeyPerformanceMode) == 0) {
        v = m->performance;
    } else if (!player && std::strcmp(key, kKeyRecordingPreset) == 0) {
        v = m->preset;
    } else {
        return kParameterInvalid;
    }
    SLresult r = kSuccess;
    if (value) {
        if (*size < 4) {
            r = kBufferInsufficient;
        } else {
            std::memcpy(value, &v, 4);
        }
    }
    *size = 4;
    return r;
}

// The Java routing proxy (API 24): no AudioRouting to give.
GUEST SLresult Config_AcquireJavaProxy(Itf*, uint32_t, void** proxy) {
    if (proxy) *proxy = nullptr;
    return kFeatureUnsupported;
}
GUEST SLresult Config_ReleaseJavaProxy(Itf*, uint32_t) { return kPreconditionsViolated; }

const void* const kConfigVtbl[] = {
    reinterpret_cast<const void*>(&Config_SetConfiguration),
    reinterpret_cast<const void*>(&Config_GetConfiguration),
    reinterpret_cast<const void*>(&Config_AcquireJavaProxy),
    reinterpret_cast<const void*>(&Config_ReleaseJavaProxy),
};

// ---- SLMetadataExtractionItf -------------------------------------------------------------
// A decoder's PCM format, as Android's six ANDROID_KEY_PCMFORMAT_* keys: what
// the buffers it fills hold (16-bit stereo at the file's rate).

constexpr size_t kInfoSize = sizeof(MetadataInfo);

uint32_t pcm_value(const Player* p, uint32_t index) {
    switch (index) {
    case 0: return 2;
    case 1: return p->clip ? static_cast<uint32_t>(p->clip->rate) : 0;
    case 2:
    case 3: return 16;
    case 4: return audio::pcm::kFrontLeft | audio::pcm::kFrontRight;
    default: return kLittleEndian;
    }
}

GUEST SLresult Meta_GetItemCount(Itf* self, uint32_t* count) {
    if (!count) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    if (p->prep != Player::kReady) return kParameterInvalid;
    *count = 6;
    return kSuccess;
}
GUEST SLresult Meta_GetKeySize(Itf* self, uint32_t index, uint32_t* size) {
    if (!size) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    if (p->prep != Player::kReady || index >= 6) return kParameterInvalid;
    *size = static_cast<uint32_t>(std::strlen(kPcmKeys[index]) + 1 + kInfoSize);
    return kSuccess;
}
GUEST SLresult Meta_GetKey(Itf* self, uint32_t index, uint32_t size, MetadataInfo* key) {
    if (!key) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    const size_t len = index < 6 ? std::strlen(kPcmKeys[index]) + 1 : 0;
    if (p->prep != Player::kReady || index >= 6 || size < kInfoSize || size - kInfoSize < len) return kParameterInvalid;
    std::memcpy(key->data, kPcmKeys[index], len);
    key->encoding = kCharsetUtf8;
    std::memcpy(key->lang_country, "en", 3);
    key->size = static_cast<uint32_t>(len);
    return kSuccess;
}
GUEST SLresult Meta_GetValueSize(Itf* self, uint32_t index, uint32_t* size) {
    if (!size) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    if (p->prep != Player::kReady || index >= 6) return kParameterInvalid;
    *size = static_cast<uint32_t>(4 + kInfoSize);
    return kSuccess;
}
GUEST SLresult Meta_GetValue(Itf* self, uint32_t index, uint32_t size, MetadataInfo* value) {
    if (!value) return kParameterInvalid;
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    value->encoding = kCharsetBinary;
    std::memcpy(value->lang_country, "en", 3);
    if (p->prep != Player::kReady || index >= 6 || size < kInfoSize || size - kInfoSize != 4) return kParameterInvalid;
    const uint32_t v = pcm_value(p, index);
    std::memcpy(value->data, &v, 4);
    value->size = 4;
    return kSuccess;
}
GUEST SLresult Meta_AddKeyFilter(Itf* self, uint32_t, const void*, uint32_t, const char*, uint32_t, uint8_t) {
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    p->filtered = true;
    return kSuccess;
}
GUEST SLresult Meta_ClearKeyFilter(Itf* self) {
    auto* p = as<Player>(self);
    std::lock_guard<std::mutex> hold(p->lock);
    p->filtered = false;
    return kSuccess;
}

const void* const kMetadataVtbl[] = {
    reinterpret_cast<const void*>(&Meta_GetItemCount),  reinterpret_cast<const void*>(&Meta_GetKeySize),
    reinterpret_cast<const void*>(&Meta_GetKey),        reinterpret_cast<const void*>(&Meta_GetValueSize),
    reinterpret_cast<const void*>(&Meta_GetValue),      reinterpret_cast<const void*>(&Meta_AddKeyFilter),
    reinterpret_cast<const void*>(&Meta_ClearKeyFilter),
};

// ---- SLRecordItf -------------------------------------------------------------------------

GUEST SLresult Record_SetRecordState(Itf* self, uint32_t s) {
    if (s != kStopped && s != kPaused && s != kPlaying) return kParameterInvalid;
    auto* r = as<Recorder>(self);
    {
        std::lock_guard<std::mutex> hold(r->lock);
        if (s == r->transport) return kSuccess;
        if (s == kPlaying && r->capture && !r->capturing) {
            r->capturing = audio::capture_start(r->capture.get());
            if (!r->capturing) log("SetRecordState: the microphone did not start");
        } else if (s != kPlaying && r->capturing) {
            audio::capture_stop(r->capture.get());  // AudioRecord stops on pause too
            r->capturing = false;
        }
        if (s == kStopped) {
            r->recorded = 0;
            r->reset_timing(0);
        }
        r->overrun = false;
        r->transport = s;
        r->recording = s == kPlaying;
    }
    if (r->core) r->core->wake();
    return kSuccess;
}

GUEST SLresult Record_SetDurationLimit(Itf* self, uint32_t ms) {
    if (ms == 0) return kParameterInvalid;
    (void)self;  // kept by nobody: Android does not send HEADATLIMIT either
    return kSuccess;
}

GUEST SLresult Record_GetPosition(Itf* self, uint32_t* ms) {
    if (!ms) return kParameterInvalid;
    auto* r = as<Recorder>(self);
    std::lock_guard<std::mutex> hold(r->lock);
    *ms = r->position_ms();
    return kSuccess;
}

GUEST SLresult Record_SetCallbackEventsMask(Itf* self, uint32_t mask) {
    if (mask & ~(kRecHeadAtLimit | kHeadAtMarker | kHeadAtNewPos | kHeadMoving | kHeadStalled | kRecBufferFull)) {
        return kParameterInvalid;
    }
    auto* m = as<Media>(self);
    std::lock_guard<std::mutex> hold(m->lock);
    m->event_mask = mask;
    return kSuccess;
}

const void* const kRecordVtbl[] = {
    reinterpret_cast<const void*>(&Record_SetRecordState),
    reinterpret_cast<const void*>(&Play_GetPlayState),
    reinterpret_cast<const void*>(&Record_SetDurationLimit),
    reinterpret_cast<const void*>(&Record_GetPosition),
    reinterpret_cast<const void*>(&Events_RegisterCallback),
    reinterpret_cast<const void*>(&Record_SetCallbackEventsMask),
    reinterpret_cast<const void*>(&Events_GetCallbackEventsMask),
    reinterpret_cast<const void*>(&Events_SetMarkerPosition),
    reinterpret_cast<const void*>(&Events_ClearMarkerPosition),
    reinterpret_cast<const void*>(&Events_GetMarkerPosition),
    reinterpret_cast<const void*>(&Events_SetPositionUpdatePeriod),
    reinterpret_cast<const void*>(&Events_GetPositionUpdatePeriod),
};

void init_vtables(Object* o) {
    const void* const* tables[kKindCount] = {kObjectVtbl, kDimVtbl,     kEngineVtbl,   kCapsVtbl,   kSyncVtbl,
                                             kMixVtbl,    kPlayVtbl,    kQueueVtbl,    kVolumeVtbl, kMuteSoloVtbl,
                                             kSeekVtbl,   kPrefetchVtbl, kRateVtbl,     kConfigVtbl, kMetadataVtbl,
                                             kRecordVtbl};
    for (int k = 0; k < kKindCount; ++k) o->itfs[k].vtbl = tables[k];
}

// ---- the library's entry points -------------------------------------------------------------

GUEST SLresult sl_CreateEngine(Itf** out, uint32_t n_options, const EngineOption* options, uint32_t n,
                               const Guid* const* ids, const uint32_t* required) {
    if (!out) return kParameterInvalid;
    *out = nullptr;
    if (n_options > 0 && !options) return kParameterInvalid;
    bool thread_safe = true;
    for (uint32_t i = 0; i < n_options; ++i) {
        if (options[i].feature == kEngineOptionThreadSafe) {
            thread_safe = options[i].data != 0;
        } else if (options[i].feature != kEngineOptionLossOfControl) {
            return kParameterInvalid;
        }
    }
    std::lock_guard<std::mutex> hold(g_engine_lock);
    auto e = g_engine ? g_engine : std::make_shared<Engine>();
    uint32_t exposed = 0;
    const SLresult r = choose_interfaces(*e, n, ids, required, &exposed);
    if (r != kSuccess) return r;
    if (g_engine) {
        ++e->refs;  // Android's one engine, one more reference
        e->exposed |= exposed;
    } else {
        e->exposed = exposed;
        e->thread_safe = thread_safe;
        init_vtables(e.get());
        g_engine = e;
        publish(e);
        log("slCreateEngine");
    }
    *out = e->itf(kObject);
    return kSuccess;
}

GUEST SLresult sl_QueryNumSupportedEngineInterfaces(uint32_t* count) {
    if (!count) return kParameterInvalid;
    *count = static_cast<uint32_t>(class_iids(kEngineClass).size());
    return kSuccess;
}

GUEST SLresult sl_QuerySupportedEngineInterfaces(uint32_t index, const Guid** out) {
    if (!out) return kParameterInvalid;
    *out = nullptr;
    const auto ids = class_iids(kEngineClass);
    if (index >= ids.size()) return kParameterInvalid;
    *out = ids[index];
    return kSuccess;
}

}  // namespace

void* native_opensles_resolve(const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, "slCreateEngine") == 0) return reinterpret_cast<void*>(&sl_CreateEngine);
    if (std::strcmp(name, "slQueryNumSupportedEngineInterfaces") == 0) {
        return reinterpret_cast<void*>(&sl_QueryNumSupportedEngineInterfaces);
    }
    if (std::strcmp(name, "slQuerySupportedEngineInterfaces") == 0) {
        return reinterpret_cast<void*>(&sl_QuerySupportedEngineInterfaces);
    }
    if (std::strncmp(name, "SL_IID_", 7) == 0) {
        const Guid* const* vars = iid_vars();
        for (size_t i = 0; i < kIidCount; ++i) {
            if (std::strcmp(kIids[i].name, name) == 0) return const_cast<const Guid**>(&vars[i]);
        }
    }
    return nullptr;
}

}  // namespace winapk::host
