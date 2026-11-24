// opensles_test.cpp -- winapk-opensles-test: libOpenSLES (host/native_opensles.cpp)
// and its PCM conversion and resampler (host/audio_pcm.h), without a JVM or a
// guest library, called as guest code calls them: through the exported entry
// points and the interfaces' vtables, System V.
//
//   winapk-opensles-test <dir with tone.wav, tone.ogg>
//
// Without an audio device the mixer keeps time silently, so this runs on CI
// machines and under Wine. The microphone is a fake (below): a known ramp, and
// a RECORD_AUDIO answer the test switches. The guest-side hooks are stood in
// for too: host_callable (no arm64 code), the shim's pread64 (CRT descriptors,
// as guest descriptors are) and __system_property_get, and android_host_path
// ("/host/<path>" is <path>).

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include "../host/audio_capture.h"
#include "../host/audio_pcm.h"
#include "../host/native_opensles.h"
#include "winapk/arm64_guest.h"
#include "winapk/guest_abi.h"
#include "winapk/mini_bionic.h"

// ---- the stand-ins ------------------------------------------------------------------------

namespace winapk {
namespace arm64 {
void* host_callable(void* fn) { return fn; }
}  // namespace arm64

namespace {
WINAPK_GUEST_ABI int64_t test_pread(int fd, void* buf, size_t n, int64_t off) {
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    return _read(fd, buf, static_cast<unsigned>(n));
}
WINAPK_GUEST_ABI int test_property_get(const char* name, char* value) {
    if (std::strcmp(name, "ro.build.version.sdk") != 0) return 0;
    std::strcpy(value, "34");
    return 2;
}
}  // namespace

void* mini_bionic_resolve(const char* name) {
    if (std::strcmp(name, "pread64") == 0) return reinterpret_cast<void*>(&test_pread);
    if (std::strcmp(name, "__system_property_get") == 0) return reinterpret_cast<void*>(&test_property_get);
    return nullptr;
}
std::wstring android_host_path(const char* path) {
    std::string p = path;
    if (p.rfind("/host/", 0) == 0) p = p.substr(6);
    return std::wstring(p.begin(), p.end());
}

// The microphone: sample k of channel c is ((k * 7 + c * 3) % 200 - 100) / 128.
namespace audio {
std::atomic<bool> g_permitted{true};
std::atomic<int> g_captures_running{0};
class Capture {
public:
    int rate, channels;
    int64_t next = 0;
    int64_t start_ns = 0;
    bool running = false;
};
bool microphone_allowed() { return true; }
bool record_audio_permitted() { return g_permitted.load(); }
std::shared_ptr<Capture> capture_create(int rate, int channels, int) {
    auto c = std::make_shared<Capture>();
    c->rate = rate;
    c->channels = channels;
    return c;
}
bool capture_start(Capture* c) {
    c->running = true;
    c->start_ns = static_cast<int64_t>(GetTickCount64()) * 1000000;
    c->next = 0;
    ++g_captures_running;
    return true;
}
void capture_stop(Capture* c) {
    if (c->running) --g_captures_running;
    c->running = false;
}
float fake_sample(int64_t k, int c) { return static_cast<float>(((k * 7 + c * 3) % 200) - 100) / 128.0f; }
size_t capture_read(Capture* c, float* out, size_t n, bool) {
    if (!c->running) return 0;
    // What a real device has captured by now.
    const int64_t now = static_cast<int64_t>(GetTickCount64()) * 1000000;
    const int64_t have = (now - c->start_ns) * c->rate / 1000000000 - c->next;
    const size_t frames = std::min<size_t>(n / c->channels, static_cast<size_t>(std::max<int64_t>(0, have)));
    for (size_t f = 0; f < frames; ++f) {
        for (int ch = 0; ch < c->channels; ++ch) out[f * c->channels + ch] = fake_sample(c->next + f, ch);
    }
    c->next += static_cast<int64_t>(frames);
    return frames * c->channels;
}
}  // namespace audio
}  // namespace winapk

namespace {

namespace pcm = winapk::audio::pcm;
constexpr double kPi = 3.14159265358979323846;

// ---- OpenSL ES, as a guest sees it ------------------------------------------------------------

using SLresult = uint32_t;
using SLuint32 = uint32_t;
using SlId = const void*;
using Itf = const void* const* const*;  // a pointer to a pointer to a vtable

constexpr SLresult kSuccess = 0, kPreconditions = 1, kParameterInvalid = 2, kBufferInsufficient = 7,
                   kContentUnsupported = 9, kFeatureUnsupported = 12;
constexpr SLuint32 kStopped = 1, kPaused = 2, kPlaying = 3;
constexpr SLuint32 kHeadAtEnd = 1, kHeadAtMarker = 2, kHeadAtNewPos = 4;
constexpr SLuint32 kTimeUnknown = 0xFFFFFFFF;

int g_failures = 0;
void check(const std::string& what, bool ok, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++g_failures;
}

SlId iid(const char* name) {
    auto* var = static_cast<SlId*>(winapk::host::native_opensles_resolve(name));
    return var ? *var : nullptr;
}

// vtable entry `i` of interface `itf`, called System V.
template <typename R, typename... A>
R call(Itf itf, int i, A... a) {
    using F = R(WINAPK_GUEST_ABI*)(Itf, A...);
    return reinterpret_cast<F>(const_cast<void*>((*itf)[i]))(itf, a...);
}

// SLObjectItf
SLresult realize(Itf o, SLuint32 async = 0) { return call<SLresult>(o, 0, async); }
SLuint32 object_state(Itf o) {
    SLuint32 s = 0;
    call<SLresult>(o, 2, &s);
    return s;
}
SLresult get_itf(Itf o, const char* name, Itf* out) { return call<SLresult>(o, 3, iid(name), out); }
Itf get(Itf o, const char* name) {
    Itf out = nullptr;
    get_itf(o, name, &out);
    return out;
}
void destroy(Itf o) { call<void>(o, 6); }
// SLPlayItf
SLresult set_play(Itf play, SLuint32 s) { return call<SLresult>(play, 0, s); }
SLuint32 play_state(Itf play) {
    SLuint32 s = 0;
    call<SLresult>(play, 1, &s);
    return s;
}
SLuint32 duration(Itf play) {
    SLuint32 ms = 0;
    call<SLresult>(play, 2, &ms);
    return ms;
}
SLuint32 position(Itf play) {
    SLuint32 ms = 0;
    call<SLresult>(play, 3, &ms);
    return ms;
}
// SLAndroidSimpleBufferQueueItf
SLresult enqueue(Itf bq, const void* p, SLuint32 n) { return call<SLresult>(bq, 0, p, n); }
struct QueueState {
    SLuint32 count, index;
};
QueueState queue_state(Itf bq) {
    QueueState s{};
    call<SLresult>(bq, 2, &s);
    return s;
}

struct Locator {
    SLuint32 type, n;
};
struct FdLocator {
    SLuint32 type;
    int32_t fd;
    int64_t offset, length;
};
struct UriLocator {
    SLuint32 type;
    const char* uri;
};
struct IoLocator {
    SLuint32 type, device_type, device_id;
    void* device;
};
struct MixLocator {
    SLuint32 type;
    Itf mix;
};
struct Pcm {
    SLuint32 format_type, channels, milli_hz, bits, container, mask, endianness, representation;
};
struct Mime {
    SLuint32 type;
    const char* mime;
    SLuint32 container;
};
struct Source {
    const void* locator;
    const void* format;
};

Itf g_engine = nullptr, g_mix = nullptr;

Itf create_player(const Source& src, const Source& sink, std::vector<const char*> names,
                  std::vector<SLuint32> required, SLresult* r) {
    std::vector<SlId> ids;
    for (auto* n : names) ids.push_back(iid(n));
    Itf p = nullptr;
    *r = call<SLresult>(g_engine, 2, &p, &src, &sink, static_cast<SLuint32>(ids.size()), ids.data(),
                        required.data());
    return *r == kSuccess ? p : nullptr;
}

// A buffer-queue player of `pcm`, with the queue, volume and prefetch status.
Itf queue_player(const Pcm& fmt, SLresult* r, SLuint32 buffers = 2, const char* extra = nullptr,
                 SLuint32 extra_required = 0) {
    static Locator bq;
    bq = {0x800007BD, buffers};
    static Pcm f;
    f = fmt;
    static MixLocator out;
    out = {4, g_mix};
    std::vector<const char*> names = {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", "SL_IID_VOLUME", "SL_IID_PREFETCHSTATUS"};
    std::vector<SLuint32> req = {1, 1, 0};
    if (extra) {
        names.push_back(extra);
        req.push_back(extra_required);
    }
    return create_player({&bq, &f}, {&out, nullptr}, names, req, r);
}

Pcm pcm16(SLuint32 channels, SLuint32 hz = 44100) {
    return {2, channels, hz * 1000, 16, 16, channels == 1 ? 4u : 3u, 2, 0};
}

// ---- callbacks ----------------------------------------------------------------------------

// A buffer-queue player's callback: counts, notes its thread, re-enqueues the
// next buffer unless told to stop. in_callback catches two at once.
struct Feed {
    std::vector<std::vector<uint8_t>> bufs;
    std::atomic<int> calls{0};
    std::atomic<int> concurrent{0};
    std::atomic<bool> overlap{false};
    std::atomic<bool> refill{true};
    std::atomic<DWORD> thread{0};
    std::atomic<bool> other_thread{false};
    int next = 0;
    int sleep_ms = 0;
    std::atomic<bool> slept_done{false};
    bool destroy_self = false;
    Itf object = nullptr;
    std::vector<SLuint32> index_seen;
    std::mutex m;
};
WINAPK_GUEST_ABI void on_buffer(Itf queue, void* ctx) {
    auto* f = static_cast<Feed*>(ctx);
    if (++f->concurrent > 1) f->overlap = true;
    const DWORD me = GetCurrentThreadId();
    DWORD expected = 0;
    if (!f->thread.compare_exchange_strong(expected, me) && expected != me) f->other_thread = true;
    {
        std::lock_guard<std::mutex> hold(f->m);
        f->index_seen.push_back(queue_state(queue).index);
    }
    ++f->calls;
    if (f->destroy_self) {
        --f->concurrent;
        destroy(f->object);
        return;
    }
    if (f->sleep_ms) {
        Sleep(static_cast<DWORD>(f->sleep_ms));
        f->slept_done = true;
    }
    if (f->refill) {
        auto& b = f->bufs[static_cast<size_t>(f->next++ % static_cast<int>(f->bufs.size()))];
        enqueue(queue, b.data(), static_cast<SLuint32>(b.size()));
    }
    --f->concurrent;
}

struct Events {
    std::atomic<int> at_end{0}, marker{0}, new_pos{0}, stalled{0};
    std::atomic<DWORD> thread{0};
    std::vector<SLuint32> prefetch;  // events, in order
    std::vector<int16_t> levels;
    std::mutex m;
    Itf prefetch_itf = nullptr;
};
WINAPK_GUEST_ABI void on_play(Itf, void* ctx, SLuint32 event) {
    auto* e = static_cast<Events*>(ctx);
    e->thread = GetCurrentThreadId();
    if (event & kHeadAtEnd) ++e->at_end;
    if (event & kHeadAtMarker) ++e->marker;
    if (event & kHeadAtNewPos) ++e->new_pos;
    if (event & 0x10) ++e->stalled;
}
WINAPK_GUEST_ABI void on_prefetch(Itf itf, void* ctx, SLuint32 event) {
    auto* e = static_cast<Events*>(ctx);
    int16_t level = -1;
    call<SLresult>(itf, 1, &level);
    std::lock_guard<std::mutex> hold(e->m);
    e->prefetch.push_back(event);
    e->levels.push_back(level);
}

void watch(Itf player, Events* e, SLuint32 mask) {
    Itf play = get(player, "SL_IID_PLAY");
    call<SLresult>(play, 4, reinterpret_cast<void*>(&on_play), static_cast<void*>(e));
    call<SLresult>(play, 5, mask);
    if (Itf pf = get(player, "SL_IID_PREFETCHSTATUS")) {
        e->prefetch_itf = pf;
        call<SLresult>(pf, 2, reinterpret_cast<void*>(&on_prefetch), static_cast<void*>(e));
        call<SLresult>(pf, 3, SLuint32{3});
    }
}

template <typename P>
bool wait_for(P pred, int ms) {
    for (int i = 0; i < ms / 5; ++i) {
        if (pred()) return true;
        Sleep(5);
    }
    return pred();
}

std::vector<uint8_t> sine16(size_t frames, int channels, double step) {
    std::vector<uint8_t> b(frames * channels * 2);
    for (size_t i = 0; i < frames; ++i) {
        const int16_t s = static_cast<int16_t>(6000 * std::sin(i * step));
        for (int c = 0; c < channels; ++c) std::memcpy(&b[(i * channels + c) * 2], &s, 2);
    }
    return b;
}

// ---- audio_pcm.h ----------------------------------------------------------------------------

void test_pcm() {
    std::printf("PCM conversion and resampling\n");
    using pcm::Sample;
    const float values[] = {-1.0f, -0.5f, 0.0f, 0.25f, 0.999f};
    for (Sample s : {Sample::kU8, Sample::kS16, Sample::kS24, Sample::kS32, Sample::kF32}) {
        float worst = 0;
        for (float v : values) {
            uint8_t b[4] = {};
            pcm::write_sample(s, v, b);
            worst = std::max(worst, std::fabs(pcm::read_sample(s, b) - v));
        }
        const float tolerance = s == Sample::kU8 ? 1 / 64.0f : s == Sample::kS16 ? 1 / 16384.0f : 1e-6f;
        check("sample format " + std::to_string(static_cast<int>(s)) + " round trip", worst <= tolerance,
              std::to_string(worst));
    }
    uint8_t s24[3] = {0x00, 0x00, 0x80};  // -2^23
    check("24-bit sign extension", pcm::read_sample(Sample::kS24, s24) == -1.0f);

    // 5.1 folds the centre and surrounds into the sides at -3 dB, without the LFE.
    const uint32_t m51 = pcm::default_out_mask(6);
    const pcm::Downmix d = pcm::downmix_for(6, m51);
    std::vector<float> out;
    const int16_t frame[6] = {1000, 0, 2000, 30000, 0, 4000};  // FL FR FC LFE BL BR
    pcm::to_stereo(Sample::kS16, d, reinterpret_cast<const uint8_t*>(frame), 1, &out);
    const float l = (1000 + 2000 * 0.70710678f) / 32768.0f, r = (2000 * 0.70710678f + 4000 * 0.70710678f) / 32768.0f;
    check("5.1 downmix", std::fabs(out[0] - l) < 1e-5f && std::fabs(out[1] - r) < 1e-5f,
          std::to_string(out[0]) + " " + std::to_string(out[1]));
    out.clear();
    const int16_t mono[1] = {16384};
    pcm::to_stereo(Sample::kS16, pcm::downmix_for(1, 4), reinterpret_cast<const uint8_t*>(mono), 1, &out);
    check("mono plays at full level on both sides", out[0] == 0.5f && out[1] == 0.5f);
    check("masks: 5.1 accepted, a lone side refused, top refused",
          pcm::player_mask_ok(m51, 6) && !pcm::player_mask_ok(0x3 | 0x200, 3) && !pcm::player_mask_ok(0x3 | 0x800, 3));

    // At a ratio of 1 the resampler copies, with no delay.
    pcm::Resampler same;
    std::vector<float> in(200), got(200);
    for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(i) / 200;
    same.push(in.data(), 100);
    const size_t n1 = same.pull(got.data(), 100);
    check("resampler: 1:1 copies", n1 == 100 && std::memcmp(in.data(), got.data(), 200 * sizeof(float)) == 0);

    // 1 kHz at 44.1 kHz to 48 kHz: the same tone, the same level.
    auto tone = [](double hz, int rate, size_t frames) {
        std::vector<float> t(frames * 2);
        for (size_t i = 0; i < frames; ++i) {
            t[i * 2] = t[i * 2 + 1] = static_cast<float>(0.5 * std::sin(2 * kPi * hz * i / rate));
        }
        return t;
    };
    pcm::Resampler up;
    up.set_ratio(44100.0 / 48000.0);
    std::vector<float> src = tone(1000, 44100, 44100);
    up.push(src.data(), 44100);
    std::vector<float> res(48000 * 2);
    const size_t made = up.pull(res.data(), 48000);
    double peak = 0, ref = 0, err = 0;
    for (size_t i = 2000; i < made - 2000; ++i) {
        const double want = 0.5 * std::sin(2 * kPi * 1000 * i / 48000.0);
        peak = std::max(peak, std::fabs(static_cast<double>(res[i * 2])));
        err += (res[i * 2] - want) * (res[i * 2] - want);
        ref += want * want;
    }
    const double snr = 10 * std::log10(ref / std::max(err, 1e-20));
    check("resampler 44.1k -> 48k: frames", made > 47900 && made <= 48000, std::to_string(made));
    check("resampler 44.1k -> 48k: a 1 kHz tone, level and phase kept", std::fabs(peak - 0.5) < 0.01 && snr > 60,
          "peak " + std::to_string(peak) + ", SNR " + std::to_string(snr) + " dB");

    // Going down, a tone above the new Nyquist is filtered out, not folded back.
    pcm::Resampler down;
    down.set_ratio(96000.0 / 48000.0);
    src = tone(30000, 96000, 48000);
    down.push(src.data(), 48000);
    res.assign(24000 * 2, 0);
    const size_t made2 = down.pull(res.data(), 24000);
    double alias = 0;
    for (size_t i = 1000; i < made2; ++i) alias = std::max(alias, std::fabs(static_cast<double>(res[i * 2])));
    check("resampler 96k -> 48k: 30 kHz does not alias", 20 * std::log10(std::max(alias, 1e-9) / 0.5) < -50,
          std::to_string(20 * std::log10(std::max(alias, 1e-9) / 0.5)) + " dB");
}

// ---- the engine ---------------------------------------------------------------------------

DWORD g_object_cb_thread = 0;
std::atomic<int> g_object_cb{0};
SLresult g_object_cb_result = 99;
WINAPK_GUEST_ABI void on_object(Itf, const void*, SLuint32 event, SLresult result, SLuint32, void*) {
    if (event == 2) {
        g_object_cb_result = result;
        g_object_cb_thread = GetCurrentThreadId();
        ++g_object_cb;
    }
}

bool test_engine() {
    std::printf("Engine\n");
    using CreateEngine = SLresult(WINAPK_GUEST_ABI*)(Itf*, SLuint32, const void*, SLuint32, const SlId*,
                                                      const SLuint32*);
    auto create = reinterpret_cast<CreateEngine>(winapk::host::native_opensles_resolve("slCreateEngine"));
    check("slCreateEngine exported, SL_IID_ENGINE is a GUID", create && iid("SL_IID_ENGINE"));
    check("SL_IID_ANDROIDSIMPLEBUFFERQUEUE is AOSP's",
          iid("SL_IID_ANDROIDSIMPLEBUFFERQUEUE") &&
              *static_cast<const uint32_t*>(iid("SL_IID_ANDROIDSIMPLEBUFFERQUEUE")) == 0x198e4940);
    if (!create) return false;
    using QueryNum = SLresult(WINAPK_GUEST_ABI*)(SLuint32*);
    SLuint32 n = 0;
    reinterpret_cast<QueryNum>(winapk::host::native_opensles_resolve("slQueryNumSupportedEngineInterfaces"))(&n);
    check("slQueryNumSupportedEngineInterfaces", n == 5, std::to_string(n));

    SlId ids[] = {iid("SL_IID_ENGINE")};
    SLuint32 req[] = {1};
    struct Option {
        SLuint32 feature, data;
    } opt{1, 1};
    check("slCreateEngine", create(&g_engine, 1, &opt, 1, ids, req) == kSuccess && g_engine);
    Itf out = nullptr;
    check("engine: SLEngineItf before Realize is a precondition violation",
          get_itf(g_engine, "SL_IID_ENGINE", &out) == kPreconditions && !out);
    call<SLresult>(g_engine, 4, reinterpret_cast<void*>(&on_object), nullptr);
    check("engine: Realize, asynchronously", realize(g_engine, 1) == kSuccess);
    check("engine: the async termination callback, on another thread",
          wait_for([] { return g_object_cb.load() == 1; }, 1000) && g_object_cb_result == kSuccess &&
              g_object_cb_thread != GetCurrentThreadId());
    Itf engine_obj = g_engine;
    g_engine = get(engine_obj, "SL_IID_ENGINE");
    check("engine: SLEngineItf", g_engine != nullptr);

    // Android's engine is shared: a second create is the same object.
    Itf second = nullptr;
    check("slCreateEngine again: the same engine", create(&second, 0, nullptr, 0, nullptr, nullptr) == kSuccess &&
                                                       second == engine_obj);
    check("engine: Realize again is not an error", realize(second) == kSuccess);
    destroy(second);
    check("engine: alive after one of its two Destroys", object_state(engine_obj) == 2);

    SLuint32 count = 0;
    call<SLresult>(g_engine, 10, SLuint32{0x1004}, &count);  // SL_OBJECTID_AUDIOPLAYER
    check("engine: an audio player's interfaces", count == 12, std::to_string(count));
    call<SLresult>(g_engine, 10, SLuint32{0x1006}, &count);  // MIDI player
    check("engine: no MIDI player", count == 0);
    char ext[64] = {};
    int16_t len = sizeof(ext);
    check("engine: the extension names the SDK level",
          call<SLresult>(g_engine, 13, SLuint32{0}, ext, &len) == kSuccess &&
              std::string(ext) == "ANDROID_SDK_LEVEL_34",
          ext);
    Itf caps = get(engine_obj, "SL_IID_ENGINECAPABILITIES");
    int16_t major = 0, minor = 0, step = 0;
    SLuint32 safe = 0;
    check("engine capabilities: API 1.0.1, thread-safe",
          caps && call<SLresult>(caps, 3, &major, &minor, &step) == kSuccess && major == 1 && minor == 0 &&
              step == 1 && call<SLresult>(caps, 6, &safe) == kSuccess && safe == 1);
    check("engine: no I/O device capabilities interface",
          get_itf(engine_obj, "SL_IID_AUDIOIODEVICECAPABILITIES", &out) == kFeatureUnsupported);

    Itf mix_obj = nullptr;
    SlId reverb[] = {iid("SL_IID_ENVIRONMENTALREVERB")};
    SLuint32 yes[] = {1}, no[] = {0};
    check("CreateOutputMix: reverb required is refused",
          call<SLresult>(g_engine, 7, &mix_obj, SLuint32{1}, reverb, yes) == kFeatureUnsupported);
    check("CreateOutputMix: reverb optional", call<SLresult>(g_engine, 7, &mix_obj, SLuint32{1}, reverb, no) ==
                                                  kSuccess &&
                                              realize(mix_obj) == kSuccess);
    check("output mix: no reverb interface", get(mix_obj, "SL_IID_ENVIRONMENTALREVERB") == nullptr);
    g_mix = mix_obj;

    // The mixer starts with the first player; looking for a device can take a
    // few hundred ms (Wine's), which the timing checks below must not count.
    SLresult r = 0;
    Locator bq{0x800007BD, 1};
    Pcm f = pcm16(1);
    MixLocator mix{4, g_mix};
    if (Itf warm = create_player({&bq, &f}, {&mix, nullptr}, {}, {}, &r)) {
        realize(warm);
        destroy(warm);
    }
    Sleep(800);
    return g_engine != nullptr;
}

// ---- buffer-queue players -----------------------------------------------------------------

void test_queue_player() {
    std::printf("Buffer-queue player\n");
    SLresult r = 0;
    Itf p = queue_player(pcm16(1), &r);
    check("CreateAudioPlayer: 16-bit mono", p != nullptr, std::to_string(r));
    if (!p) return;
    Itf config = nullptr;
    check("player: no configuration interface it did not ask for",
          get_itf(p, "SL_IID_ANDROIDCONFIGURATION", &config) == kFeatureUnsupported);  // not asked for
    check("player: Realize", realize(p) == kSuccess);
    Itf play = get(p, "SL_IID_PLAY"), bq = get(p, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"), vol = get(p, "SL_IID_VOLUME");
    check("player: play, queue and volume", play && bq && vol);
    check("player: SL_IID_BUFFERQUEUE is the same queue", get(p, "SL_IID_BUFFERQUEUE") == bq);
    check("player: no seek on a buffer queue", get(p, "SL_IID_SEEK") == nullptr);
    check("player: no mute/solo on mono", get(p, "SL_IID_MUTESOLO") == nullptr);
    check("player: duration unknown", duration(play) == kTimeUnknown);
    check("volume: above 0 mB is refused", call<SLresult>(vol, 0, int16_t{1}) == kParameterInvalid);
    int16_t level = 0;
    call<SLresult>(vol, 0, int16_t{-600});
    call<SLresult>(vol, 1, &level);
    check("volume: level kept", level == -600);

    Feed feed;
    for (int i = 0; i < 3; ++i) feed.bufs.push_back(sine16(4410, 1, 0.0627));  // 100 ms each
    Events ev;
    watch(p, &ev, kHeadAtEnd | kHeadAtMarker | kHeadAtNewPos);
    call<SLresult>(play, 7, SLuint32{250});   // a marker at 250 ms
    call<SLresult>(play, 10, SLuint32{100});  // every 100 ms
    check("queue: RegisterCallback while stopped", call<SLresult>(bq, 3, reinterpret_cast<void*>(&on_buffer),
                                                                   static_cast<void*>(&feed)) == kSuccess);
    check("queue: enqueue null is refused", enqueue(bq, nullptr, 4) == kParameterInvalid);
    enqueue(bq, feed.bufs[0].data(), static_cast<SLuint32>(feed.bufs[0].size()));
    enqueue(bq, feed.bufs[1].data(), static_cast<SLuint32>(feed.bufs[1].size()));
    feed.next = 2;
    check("queue: a third buffer does not fit in two",
          enqueue(bq, feed.bufs[2].data(), static_cast<SLuint32>(feed.bufs[2].size())) == kBufferInsufficient);
    check("queue: GetState before playing", queue_state(bq).count == 2 && queue_state(bq).index == 0);
    const DWORD t0 = GetTickCount();
    set_play(play, kPlaying);
    check("queue: RegisterCallback while playing is a precondition violation",
          call<SLresult>(bq, 3, reinterpret_cast<void*>(&on_buffer), static_cast<void*>(&feed)) == kPreconditions);
    Sleep(600);
    const SLuint32 ms = position(play);
    const DWORD elapsed = GetTickCount() - t0;
    const QueueState qs = queue_state(bq);
    check("player: buffers come back as they are taken", feed.calls >= 5 && feed.calls <= 9,
          std::to_string(feed.calls.load()));
    check("player: callbacks on one thread, not the app's, never two at once",
          !feed.other_thread && feed.thread != GetCurrentThreadId() && !feed.overlap);
    {
        std::lock_guard<std::mutex> hold(feed.m);
        bool ordered = true;
        for (size_t i = 0; i < feed.index_seen.size(); ++i) ordered = ordered && feed.index_seen[i] == i + 1;
        check("queue: each callback sees the index one further", ordered);
    }
    // Low latency: what is heard trails what was handed back by about the lead (tens of ms).
    check("player: position follows real time", ms + 150 >= elapsed && ms <= elapsed + 20,
          std::to_string(ms) + " ms after " + std::to_string(elapsed) + " ms");
    check("queue: GetState counts the buffers taken", qs.index == static_cast<SLuint32>(feed.calls.load()) &&
                                                          qs.count <= 2,
          std::to_string(qs.count) + " queued, index " + std::to_string(qs.index));
    check("play: the marker once, the period events every 100 ms", ev.marker == 1 && ev.new_pos >= 4 && ev.new_pos <= 6,
          std::to_string(ev.marker.load()) + " marker, " + std::to_string(ev.new_pos.load()) + " new pos");
    check("play: events on the callback thread", ev.thread == feed.thread);

    // Running dry: HEADATEND once, still PLAYING, prefetch UNDERFLOW; refilled: SUFFICIENTDATA.
    feed.refill = false;
    check("starved: HEADATEND, still playing", wait_for([&] { return ev.at_end.load() == 1; }, 1000) &&
                                                   play_state(play) == kPlaying);
    Sleep(100);
    check("starved: HEADATEND only once", ev.at_end == 1);
    SLuint32 status = 0;
    call<SLresult>(ev.prefetch_itf, 0, &status);
    check("starved: prefetch status UNDERFLOW", status == 1);
    const int before = feed.calls;
    feed.refill = true;
    enqueue(bq, feed.bufs[0].data(), static_cast<SLuint32>(feed.bufs[0].size()));
    check("refilled: playing again", wait_for([&] { return feed.calls > before + 1; }, 1000));
    call<SLresult>(ev.prefetch_itf, 0, &status);
    check("refilled: prefetch status SUFFICIENTDATA", status == 2);

    // Stop: position back to 0; the queue keeps its buffers, as Android's does.
    feed.refill = false;
    set_play(play, kStopped);
    const QueueState stopped = queue_state(bq);
    check("stop: position 0", position(play) == 0);
    Sleep(100);
    check("stop: no buffers taken while stopped", queue_state(bq).count == stopped.count);
    call<SLresult>(bq, 1);
    check("Clear: the queue emptied", queue_state(bq).count == 0 && queue_state(bq).index == 0);
    const int calls = feed.calls;
    destroy(p);
    Sleep(100);
    check("Destroy: no callback after it returned", feed.calls == calls);
}

// Every PCM format Android accepts plays; what it refuses is refused with its codes.
void test_formats() {
    std::printf("Formats\n");
    struct Case {
        const char* what;
        Pcm fmt;
        size_t frame_bytes;
    };
    const Case cases[] = {
        {"8-bit unsigned mono", {2, 1, 22050000, 8, 8, 4, 2, 0}, 1},
        {"16-bit stereo 48 kHz", pcm16(2, 48000), 4},
        {"24-bit stereo", {2, 2, 44100000, 24, 24, 3, 2, 0}, 6},
        {"32-bit integer stereo", {4, 2, 44100000, 32, 32, 3, 2, 1}, 8},
        {"float stereo (PCM_EX)", {4, 2, 48000000, 32, 32, 3, 2, 3}, 8},
        {"5.1 16-bit", {2, 6, 48000000, 16, 16, 0x3F, 2, 0}, 12},
        {"7.1 float", {4, 8, 48000000, 32, 32, 0x63F, 2, 3}, 32},
        {"8 kHz mono", pcm16(1, 8000), 2},
        {"192 kHz stereo", pcm16(2, 192000), 4},
        {"no mask: the default for 2", {2, 2, 44100000, 16, 16, 0, 2, 0}, 4},
    };
    for (const auto& c : cases) {
        SLresult r = 0;
        Itf p = queue_player(c.fmt, &r);
        if (!p) {
            check(std::string(c.what) + ": created", false, std::to_string(r));
            continue;
        }
        realize(p);
        Feed f;
        f.bufs.push_back(std::vector<uint8_t>(c.frame_bytes * (c.fmt.milli_hz / 1000 / 50), 0));  // 20 ms
        f.bufs.push_back(f.bufs[0]);
        Itf bq = get(p, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
        call<SLresult>(bq, 3, reinterpret_cast<void*>(&on_buffer), static_cast<void*>(&f));
        enqueue(bq, f.bufs[0].data(), static_cast<SLuint32>(f.bufs[0].size()));
        f.next = 1;
        set_play(get(p, "SL_IID_PLAY"), kPlaying);
        check(std::string(c.what) + ": plays", wait_for([&] { return f.calls >= 4; }, 1000),
              std::to_string(f.calls.load()) + " buffers");
        destroy(p);
    }
    struct Bad {
        const char* what;
        Pcm fmt;
        SLresult want;
    };
    const Bad bad[] = {
        {"big-endian", {2, 2, 44100000, 16, 16, 3, 1, 0}, kContentUnsupported},
        {"24 bits in 32", {2, 2, 44100000, 24, 32, 3, 2, 0}, kContentUnsupported},
        {"mask of 2 for 1 channel", {2, 1, 44100000, 16, 16, 3, 2, 0}, kParameterInvalid},
        {"9 channels", {2, 9, 44100000, 16, 16, 0x1FF, 2, 0}, kContentUnsupported},
        {"4 kHz", pcm16(2, 4000), kContentUnsupported},
        {"no such byte order", {2, 2, 44100000, 16, 16, 3, 7, 0}, kParameterInvalid},
        {"float in 16 bits", {4, 2, 44100000, 16, 16, 3, 2, 3}, kParameterInvalid},
        {"a lone side channel", {2, 3, 44100000, 16, 16, 0x203, 2, 0}, kContentUnsupported},
    };
    for (const auto& b : bad) {
        SLresult r = 0;
        Itf p = queue_player(b.fmt, &r);
        check(std::string("refused: ") + b.what, !p && r == b.want, std::to_string(r));
        if (p) destroy(p);
    }
    SLresult r = 0;
    Itf p = queue_player(pcm16(2), &r, 2, "SL_IID_SEEK", 1);
    check("CreateAudioPlayer: seeking required of a queue is refused", !p && r == kFeatureUnsupported);
    p = queue_player(pcm16(1), &r, 2, "SL_IID_MUTESOLO", 1);
    check("CreateAudioPlayer: mute/solo required of mono is refused", !p && r == kFeatureUnsupported);
    p = queue_player(pcm16(2), &r, 2, "SL_IID_MUTESOLO", 1);
    check("CreateAudioPlayer: mute/solo on stereo", p != nullptr);
    if (p) {
        realize(p);
        Itf ms = get(p, "SL_IID_MUTESOLO");
        uint8_t n = 0;
        check("mute/solo: two channels, channel 2 refused",
              ms && call<SLresult>(ms, 4, &n) == kSuccess && n == 2 &&
                  call<SLresult>(ms, 0, uint8_t{2}, SLuint32{1}) == kParameterInvalid &&
                  call<SLresult>(ms, 2, uint8_t{1}, SLuint32{1}) == kSuccess);
        destroy(p);
    }
}

// Low latency: two 5 ms buffers at the output's rate, refilled from the
// callback as games do, play without running dry, with only tens of ms
// between handing a buffer over and hearing it.
void test_small_buffers() {
    std::printf("Small buffers\n");
    SLresult r = 0;
    Itf p = queue_player(pcm16(2, 48000), &r);
    if (!p) return check("CreateAudioPlayer: 48 kHz stereo", false, std::to_string(r));
    realize(p);
    Feed f;
    f.bufs.assign(2, sine16(240, 2, 0.05));
    Events ev;
    watch(p, &ev, kHeadAtEnd | 0x10);
    Itf bq = get(p, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"), play = get(p, "SL_IID_PLAY");
    call<SLresult>(bq, 3, reinterpret_cast<void*>(&on_buffer), static_cast<void*>(&f));
    enqueue(bq, f.bufs[0].data(), 960);
    enqueue(bq, f.bufs[1].data(), 960);
    const DWORD t0 = GetTickCount();
    set_play(play, kPlaying);
    Sleep(1000);
    const SLuint32 heard = position(play);
    const double handed = f.calls * 5.0;  // ms of audio given back to the app
    const DWORD elapsed = GetTickCount() - t0;
    check("5 ms buffers: no starvation, no stall", ev.at_end == 0 && ev.stalled == 0,
          std::to_string(ev.at_end.load()) + " ends, " + std::to_string(ev.stalled.load()) + " stalls");
    check("5 ms buffers: about 200 a second", f.calls >= 180 && f.calls <= 215, std::to_string(f.calls.load()));
    check("5 ms buffers: handed over at most 40 ms before heard", handed - heard <= 40 && handed >= heard,
          std::to_string(handed - heard) + " ms, " + std::to_string(elapsed) + " ms elapsed");
    destroy(p);
}

// A playback rate of 2x plays twice as fast; Destroy waits for a running callback.
void test_rate_and_destroy() {
    std::printf("Playback rate, Destroy\n");
    SLresult r = 0;
    Itf p = queue_player(pcm16(1), &r, 2, "SL_IID_PLAYBACKRATE", 0);
    if (!p) return check("CreateAudioPlayer with a playback rate", false, std::to_string(r));
    realize(p);
    Itf rate = get(p, "SL_IID_PLAYBACKRATE");
    int16_t min = 0, max = 0, step = 0;
    SLuint32 caps = 0;
    check("rate: 500-2000 permille, pitch follows", rate &&
                                                       call<SLresult>(rate, 5, uint8_t{0}, &min, &max, &step, &caps) ==
                                                           kSuccess &&
                                                       min == 500 && max == 2000 && caps == 0x400);
    check("rate: 3x refused", call<SLresult>(rate, 0, int16_t{3000}) == kParameterInvalid);
    call<SLresult>(rate, 0, int16_t{2000});
    Feed f;
    f.bufs.assign(2, sine16(4410, 1, 0.05));
    Itf bq = get(p, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"), play = get(p, "SL_IID_PLAY");
    call<SLresult>(bq, 3, reinterpret_cast<void*>(&on_buffer), static_cast<void*>(&f));
    enqueue(bq, f.bufs[0].data(), static_cast<SLuint32>(f.bufs[0].size()));
    enqueue(bq, f.bufs[1].data(), static_cast<SLuint32>(f.bufs[1].size()));
    const DWORD t0 = GetTickCount();
    set_play(play, kPlaying);
    Sleep(500);
    const double speed = static_cast<double>(position(play)) / (GetTickCount() - t0);
    check("rate 2x: the position runs about twice as fast", speed > 1.6 && speed < 2.3, std::to_string(speed));

    // Destroy from another thread while a callback runs: it returns after it.
    f.sleep_ms = 150;
    f.slept_done = false;
    const int seen = f.calls;
    wait_for([&] { return f.calls > seen; }, 1000);  // a callback has started (and sleeps)
    destroy(p);
    check("Destroy waits for the callback in progress", f.slept_done.load());
    const int after = f.calls;
    Sleep(150);
    check("Destroy: nothing called after", f.calls == after);

    // Destroy from inside its own callback.
    Itf pd = queue_player(pcm16(1), &r);
    if (!pd) return;
    realize(pd);
    Feed d;
    d.bufs.assign(1, std::vector<uint8_t>(882, 0));
    d.destroy_self = true;
    d.object = pd;
    Itf dq = get(pd, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
    call<SLresult>(dq, 3, reinterpret_cast<void*>(&on_buffer), static_cast<void*>(&d));
    enqueue(dq, d.bufs[0].data(), 882);
    set_play(get(pd, "SL_IID_PLAY"), kPlaying);
    check("Destroy from its own callback", wait_for([&] { return d.calls == 1; }, 1000));
    Sleep(50);
    check("Destroy from its own callback: called once", d.calls == 1);
}

// ---- file players -------------------------------------------------------------------------

Itf file_player(const void* locator, SLresult* r, bool to_queue = false) {
    static Mime mime{1, nullptr, 1};  // SL_DATAFORMAT_MIME, SL_CONTAINERTYPE_UNSPECIFIED
    static MixLocator out;
    out = {4, g_mix};
    static Locator bq{0x800007BD, 2};
    static Pcm pcm = pcm16(2);
    if (to_queue) {
        return create_player({locator, &mime}, {&bq, &pcm},
                             {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", "SL_IID_PREFETCHSTATUS", "SL_IID_METADATAEXTRACTION",
                              "SL_IID_SEEK"},
                             {1, 1, 1, 1}, r);
    }
    return create_player({locator, &mime}, {&out, nullptr},
                         {"SL_IID_SEEK", "SL_IID_VOLUME", "SL_IID_PREFETCHSTATUS", "SL_IID_MUTESOLO"}, {1, 1, 1, 0}, r);
}

void check_file_player(const std::string& what, Itf p, SLuint32 expect_ms) {
    check(what + ": Realize", realize(p) == kSuccess);
    Itf play = get(p, "SL_IID_PLAY"), seek = get(p, "SL_IID_SEEK");
    check(what + ": duration unknown before it is prepared", duration(play) == kTimeUnknown);
    Events ev;
    watch(p, &ev, kHeadAtEnd);
    set_play(play, kPaused);  // prepares, as on Android
    check(what + ": prepared: fill level, then status", wait_for([&] {
              std::lock_guard<std::mutex> hold(ev.m);
              return ev.prefetch.size() == 2;
          }, 3000) && ev.prefetch[0] == 2 && ev.prefetch[1] == 1 && ev.levels[1] == 1000);
    const SLuint32 ms = duration(play);
    check(what + ": duration", ms + 30 >= expect_ms && ms <= expect_ms + 30, std::to_string(ms));
    set_play(play, kPlaying);
    Sleep(200);
    const SLuint32 pos = position(play);
    check(what + ": position advances", pos > 100 && pos < 350, std::to_string(pos));
    check(what + ": HEADATEND", wait_for([&] { return ev.at_end == 1; }, 1500));
    check(what + ": paused at the end, position at the end",
          play_state(play) == kPaused && position(play) == ms, std::to_string(position(play)));
    // Played again: from the start, as MediaPlayer does.
    set_play(play, kPlaying);
    Sleep(60);
    check(what + ": played again from the start", position(play) < 150, std::to_string(position(play)));
    // Seek near the end: HEADATEND comes soon.
    call<SLresult>(seek, 0, ms - 100, SLuint32{2});
    const DWORD t0 = GetTickCount();
    check(what + ": seek near the end", wait_for([&] { return ev.at_end == 2; }, 1000) && GetTickCount() - t0 < 300,
          std::to_string(GetTickCount() - t0) + " ms");
    // Looping: no end, the position wraps.
    check(what + ": a loop of part of the file is unsupported",
          call<SLresult>(seek, 1, SLuint32{1}, SLuint32{100}, SLuint32{200}) == kFeatureUnsupported);
    call<SLresult>(seek, 1, SLuint32{1}, SLuint32{0}, kTimeUnknown);
    set_play(play, kPlaying);
    Sleep(ms + 300);
    check(what + ": looping: no HEADATEND, the position wraps", ev.at_end == 2 && position(play) < ms,
          std::to_string(position(play)));
    destroy(p);
}

void test_file_players(const std::string& dir) {
    std::printf("File players\n");
    const std::string wav = dir + "\\tone.wav";
    const int fd = _open(wav.c_str(), _O_RDONLY | _O_BINARY);
    FdLocator fl{0x800007BC, fd, 0, -1};  // SL_DATALOCATOR_ANDROIDFD, to the end
    SLresult r = 0;
    Itf pf = fd >= 0 ? file_player(&fl, &r) : nullptr;
    if (fd >= 0) _close(fd);  // read at Realize... and before: closing it after is safe
    check("CreateAudioPlayer: Android fd", pf != nullptr, std::to_string(r));
    if (pf) check_file_player("fd player", pf, 501);

    std::string uri = "file:///host/" + wav;
    for (char& ch : uri) {
        if (ch == '\\') ch = '/';
    }
    UriLocator ul{1, uri.c_str()};
    Itf pu = file_player(&ul, &r);
    check("CreateAudioPlayer: file URI", pu != nullptr, std::to_string(r));
    if (pu) check_file_player("URI player", pu, 501);

    std::string ogg = "/host/" + dir + "\\tone.ogg";
    UriLocator ol{1, ogg.c_str()};
    Itf po = file_player(&ol, &r);
    if (po) {
        realize(po);
        Events ev;
        watch(po, &ev, kHeadAtEnd);
        set_play(get(po, "SL_IID_PLAY"), kPlaying);
        check("Ogg Vorbis by path: plays to its end", wait_for([&] { return ev.at_end == 1; }, 3000));
        destroy(po);
    }

    UriLocator content{1, "content://media/external/audio/1"};
    check("CreateAudioPlayer: a content URI is refused", !file_player(&content, &r) && r == kContentUnsupported);
    // A server that is not there: Realize succeeds; preparing reports the
    // error the Android way, one callback with both events at level 0.
    UriLocator http{1, "http://127.0.0.1:9/a.ogg"};
    Itf ph = file_player(&http, &r);
    check("CreateAudioPlayer: http URI", ph != nullptr, std::to_string(r));
    if (ph) {
        check("http player: Realize", realize(ph) == kSuccess);
        Events ev;
        watch(ph, &ev, kHeadAtEnd);
        set_play(get(ph, "SL_IID_PLAY"), kPlaying);
        check("http player: the prefetch error", wait_for([&] {
                  std::lock_guard<std::mutex> hold(ev.m);
                  return ev.prefetch.size() == 1;
              }, 10000) && ev.prefetch[0] == 3 && ev.levels[0] == 0);
        destroy(ph);
    }
    // With a server to download from (WINAPK_TEST_HTTP_BASE=http://host:port
    // serving this directory), an http player plays the file.
    if (const char* base = std::getenv("WINAPK_TEST_HTTP_BASE")) {
        const std::string url = std::string(base) + "/tone.wav";
        UriLocator ok{1, url.c_str()};
        Itf pw = file_player(&ok, &r);
        if (pw) {
            realize(pw);
            Events ev;
            watch(pw, &ev, kHeadAtEnd);
            set_play(get(pw, "SL_IID_PLAY"), kPlaying);
            check("http player: downloads and plays to its end", wait_for([&] { return ev.at_end == 1; }, 5000) &&
                                                                     duration(get(pw, "SL_IID_PLAY")) == 500);
            destroy(pw);
        }
    }

    // Decode to PCM into the app's buffers.
    const int fd2 = _open(wav.c_str(), _O_RDONLY | _O_BINARY);
    FdLocator fl2{0x800007BC, fd2, 0, -1};
    Itf pd = fd2 >= 0 ? file_player(&fl2, &r, true) : nullptr;
    if (fd2 >= 0) _close(fd2);
    check("CreateAudioPlayer: decode to a buffer queue", pd != nullptr, std::to_string(r));
    if (!pd) return;
    realize(pd);
    Itf play = get(pd, "SL_IID_PLAY"), bq = get(pd, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
    Itf meta = get(pd, "SL_IID_METADATAEXTRACTION");
    Events ev;
    watch(pd, &ev, kHeadAtEnd);
    Feed f;
    f.bufs.assign(2, std::vector<uint8_t>(4096, 0xAA));
    call<SLresult>(bq, 3, reinterpret_cast<void*>(&on_buffer), static_cast<void*>(&f));
    set_play(play, kPaused);
    wait_for([&] {
        std::lock_guard<std::mutex> hold(ev.m);
        return ev.prefetch.size() == 2;
    }, 3000);
    SLuint32 items = 0;
    call<SLresult>(meta, 0, &items);
    std::vector<uint32_t> values;
    std::string keys;
    for (SLuint32 i = 0; i < items; ++i) {
        SLuint32 ksize = 0, vsize = 0;
        call<SLresult>(meta, 1, i, &ksize);
        std::vector<uint8_t> k(ksize), v(32);
        call<SLresult>(meta, 2, i, ksize, k.data());
        call<SLresult>(meta, 3, i, &vsize);
        call<SLresult>(meta, 4, i, vsize, v.data());
        keys += reinterpret_cast<const char*>(k.data() + 24) + std::string(" ");
        uint32_t val = 0;
        std::memcpy(&val, v.data() + 24, 4);
        values.push_back(val);
    }
    check("decoder: six PCM format keys", items == 6 && keys.find("AndroidPcmFormatSampleRate") != std::string::npos,
          keys);
    check("decoder: 16-bit stereo at the file's rate",
          values.size() == 6 && values[0] == 2 && values[1] == 16000 && values[2] == 16 && values[5] == 2);
    enqueue(bq, f.bufs[0].data(), 4096);
    enqueue(bq, f.bufs[1].data(), 4096);
    f.next = 0;
    set_play(play, kPlaying);
    check("decoder: HEADATEND once the file is out", wait_for([&] { return ev.at_end == 1; }, 2000));
    // 501 ms at 16 kHz is 8017 frames: 32068 bytes, 8 buffers of 4096 (the last one padded).
    check("decoder: every frame delivered", f.calls == 8, std::to_string(f.calls.load()));
    destroy(pd);
}

// ---- recorders ----------------------------------------------------------------------------

struct Recording {
    std::vector<std::vector<int16_t>> bufs;
    std::vector<int16_t> got;
    std::atomic<int> calls{0};
    int next = 0;
    std::mutex m;
};
WINAPK_GUEST_ABI void on_recorded(Itf queue, void* ctx) {
    auto* r = static_cast<Recording*>(ctx);
    auto& b = r->bufs[static_cast<size_t>(r->next++ % 2)];
    {
        std::lock_guard<std::mutex> hold(r->m);
        r->got.insert(r->got.end(), b.begin(), b.end());
    }
    ++r->calls;
    enqueue(queue, b.data(), static_cast<SLuint32>(b.size() * 2));
}

Itf recorder(const Pcm& fmt, SLresult* r) {
    static IoLocator io{3, 1, 0xFFFFFFFF, nullptr};
    static Locator bq{0x800007BD, 2};
    static Pcm f;
    f = fmt;
    Source src{&io, nullptr}, sink{&bq, &f};
    SlId ids[] = {iid("SL_IID_ANDROIDSIMPLEBUFFERQUEUE"), iid("SL_IID_ANDROIDCONFIGURATION")};
    SLuint32 req[] = {1, 0};
    Itf rec = nullptr;
    *r = call<SLresult>(g_engine, 3, &rec, &src, &sink, SLuint32{2}, ids, req);
    return *r == kSuccess ? rec : nullptr;
}

void test_recorder() {
    std::printf("Recorder\n");
    SLresult r = 0;
    Pcm stereo = pcm16(2, 16000);
    Itf rec = recorder(stereo, &r);
    check("CreateAudioRecorder", rec != nullptr, std::to_string(r));
    if (!rec) return;
    winapk::audio::g_permitted = false;
    check("recorder: without RECORD_AUDIO, Realize fails as AudioRecord's init does",
          realize(rec) == kContentUnsupported && object_state(rec) == 1);
    winapk::audio::g_permitted = true;
    // The recording preset is set before Realize, as on Android.
    Itf config = get(rec, "SL_IID_ANDROIDCONFIGURATION");
    SLuint32 preset = 4, size = 4;  // VOICE_COMMUNICATION
    check("recorder: preset before Realize", config && call<SLresult>(config, 0, "androidRecordingPreset", &preset,
                                                                      SLuint32{4}) == kSuccess);
    check("recorder: Realize with RECORD_AUDIO", realize(rec) == kSuccess);
    preset = 0;
    check("recorder: preset kept, not settable once realized",
          call<SLresult>(config, 1, "androidRecordingPreset", &size, &preset) == kSuccess && preset == 4 &&
              call<SLresult>(config, 0, "androidRecordingPreset", &preset, SLuint32{4}) == kPreconditions);
    Itf record = get(rec, "SL_IID_RECORD"), bq = get(rec, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
    Recording data;
    data.bufs.assign(2, std::vector<int16_t>(160 * 2));  // 10 ms stereo
    call<SLresult>(bq, 3, reinterpret_cast<void*>(&on_recorded), static_cast<void*>(&data));
    enqueue(bq, data.bufs[0].data(), 640);
    enqueue(bq, data.bufs[1].data(), 640);
    call<SLresult>(record, 0, SLuint32{3});  // RECORDING
    check("recorder: the microphone runs while recording", winapk::audio::g_captures_running == 1);
    Sleep(400);
    call<SLresult>(record, 0, SLuint32{1});
    SLuint32 ms = 0;
    check("recorder: the microphone stops with the recording", winapk::audio::g_captures_running == 0);
    check("recorder: buffers filled in real time", data.calls >= 25 && data.calls <= 45,
          std::to_string(data.calls.load()));
    call<SLresult>(record, 3, &ms);
    check("recorder: position back to 0 when stopped", ms == 0);
    bool exact = true;
    {
        std::lock_guard<std::mutex> hold(data.m);
        for (size_t i = 0; i < std::min<size_t>(data.got.size(), 4000) && exact; ++i) {
            const float v = winapk::audio::fake_sample(static_cast<int64_t>(i / 2), static_cast<int>(i % 2));
            const int16_t want = static_cast<int16_t>(std::lround(v * 32768.0));
            exact = data.got[i] == want;
        }
    }
    check("recorder: the samples, in order, as 16-bit", exact && data.got.size() >= 4000);
    destroy(rec);

    // A float recorder (PCM_EX) and a refused 3-channel one.
    Pcm fl{4, 1, 48000000, 32, 32, 1, 2, 3};
    Itf rf = recorder(fl, &r);
    check("CreateAudioRecorder: float", rf && realize(rf) == kSuccess, std::to_string(r));
    if (rf) destroy(rf);
    Pcm three{2, 3, 48000000, 16, 16, 7, 2, 0};
    check("CreateAudioRecorder: 3 channels refused", !recorder(three, &r) && r == kContentUnsupported);
    Pcm bad{2, 1, 16000000, 16, 16, 1, 7, 0};
    check("CreateAudioRecorder: no such byte order", !recorder(bad, &r) && r == kParameterInvalid);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: winapk-opensles-test <dir with tone.wav>\n");
        return 2;
    }
    const std::string dir = argv[1];
    test_pcm();
    if (test_engine()) {
        test_queue_player();
        test_formats();
        test_small_buffers();
        test_rate_and_destroy();
        test_file_players(dir);
        test_recorder();
    }
    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    std::fflush(stdout);
    return g_failures;
}
