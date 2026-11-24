// opensles_test.cpp -- winapk-opensles-test: libOpenSLES (host/native_opensles.cpp)
// without a JVM or a guest library, called as guest code calls it: through
// the exported entry points and the interfaces' vtables, System V.
//
//   winapk-opensles-test <dir with tone.wav>
//
// Checks: the engine and the output mix; a buffer-queue player of 16-bit mono
// whose callback re-enqueues, with the position advancing; a 24-bit stereo
// player (the converted path); an interface asked for as required that a
// buffer-queue player lacks; a player destroyed from its own callback; an fd
// player and a file-URI player of test/audio/tone.wav (duration, the
// head-at-end event, paused at the end); a recorder (realized only when the
// machine has a capture device). Without an audio device the mixer keeps
// time silently, so this runs on CI machines too.
//
// The guest-side hooks the file needs are stood in for here: host_callable
// (no arm64 code), the shim's pread64 (CRT descriptors, as guest descriptors
// are) and android_host_path ("/host/<path>" is <path>).

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <io.h>
#include <windows.h>

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
}  // namespace

void* mini_bionic_resolve(const char* name) {
    return std::strcmp(name, "pread64") == 0 ? reinterpret_cast<void*>(&test_pread) : nullptr;
}
std::wstring android_host_path(const char* path) {
    std::string p = path;
    if (p.rfind("/host/", 0) == 0) p = p.substr(6);
    return std::wstring(p.begin(), p.end());
}
}  // namespace winapk

namespace {

// ---- OpenSL ES, as a guest sees it ------------------------------------------------------------

using SLresult = uint32_t;
using SLuint32 = uint32_t;
using SlId = const void*;
using Itf = const void* const* const*;  // a pointer to a pointer to a vtable

constexpr SLresult kSuccess = 0, kParameterInvalid = 2, kContentUnsupported = 9, kPermissionDenied = 11,
                   kFeatureUnsupported = 12;

SlId iid(const char* name) {
    auto* var = static_cast<SlId*>(winapk::host::native_opensles_resolve(name));
    return var ? *var : nullptr;
}

// vtable entry `i` of interface `itf`, as a System V function of type F.
template <typename F>
F fn(Itf itf, int i) {
    return reinterpret_cast<F>(const_cast<void*>((*itf)[i]));
}

using RealizeFn = SLresult(WINAPK_GUEST_ABI*)(Itf, SLuint32);
using GetStateFn = SLresult(WINAPK_GUEST_ABI*)(Itf, SLuint32*);
using GetInterfaceFn = SLresult(WINAPK_GUEST_ABI*)(Itf, SlId, Itf*);
using DestroyFn = void(WINAPK_GUEST_ABI*)(Itf);
SLresult realize(Itf o) { return fn<RealizeFn>(o, 0)(o, 0); }
Itf get(Itf o, const char* name) {
    Itf out = nullptr;
    fn<GetInterfaceFn>(o, 3)(o, iid(name), &out);
    return out;
}
void destroy(Itf o) { fn<DestroyFn>(o, 6)(o); }

struct Locator {
    SLuint32 type;
    SLuint32 n;
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
struct Pcm {
    SLuint32 format_type, channels, milli_hz, bits, container, mask, endianness;
};
struct Source {
    const void* locator;
    const void* format;
};

using CreateEngineFn = SLresult(WINAPK_GUEST_ABI*)(Itf*, SLuint32, const void*, SLuint32, const SlId*,
                                                   const SLuint32*);
using CreateOutputMixFn = SLresult(WINAPK_GUEST_ABI*)(Itf, Itf*, SLuint32, const SlId*, const SLuint32*);
using CreatePlayerFn = SLresult(WINAPK_GUEST_ABI*)(Itf, Itf*, const Source*, const Source*, SLuint32, const SlId*,
                                                   const SLuint32*);
using QueryNumFn = SLresult(WINAPK_GUEST_ABI*)(Itf, SLuint32, SLuint32*);
using SetStateFn = SLresult(WINAPK_GUEST_ABI*)(Itf, SLuint32);
using GetMsFn = SLresult(WINAPK_GUEST_ABI*)(Itf, SLuint32*);
using RegisterFn = SLresult(WINAPK_GUEST_ABI*)(Itf, void*, void*);
using EnqueueFn = SLresult(WINAPK_GUEST_ABI*)(Itf, const void*, SLuint32);
using QueueStateFn = SLresult(WINAPK_GUEST_ABI*)(Itf, SLuint32*);
using SetLevelFn = SLresult(WINAPK_GUEST_ABI*)(Itf, int32_t);

int g_failures = 0;
void check(const char* what, bool ok, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++g_failures;
}

// A buffer-queue player's callback: re-enqueues the buffer it was given.
struct QueueCtx {
    std::vector<uint8_t> buf;
    std::atomic<int> calls{0};
    bool destroy_self = false;
    Itf object = nullptr;
};
WINAPK_GUEST_ABI void on_buffer(Itf queue, void* ctx) {
    auto* c = static_cast<QueueCtx*>(ctx);
    ++c->calls;
    if (c->destroy_self) {
        destroy(c->object);
        return;
    }
    fn<EnqueueFn>(queue, 0)(queue, c->buf.data(), static_cast<SLuint32>(c->buf.size()));
}

std::atomic<int> g_play_events{0};
WINAPK_GUEST_ABI void on_play_event(Itf, void*, SLuint32 event) {
    if (event & 1) ++g_play_events;  // SL_PLAYEVENT_HEADATEND
}

// A buffer-queue player of `channels` x `bits` at 44.1 kHz; null on failure.
Itf queue_player(Itf engine, Itf mix, SLuint32 channels, SLuint32 bits, SLresult* result,
                 const char* required = nullptr) {
    Locator bq{0x800007BD, 2};  // SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE
    Pcm pcm{2, channels, 44100000, bits, bits, channels == 1 ? 4u : 3u, 2};
    Source src{&bq, &pcm};
    struct {
        SLuint32 type;
        Itf mix;
    } out{4, mix};  // SL_DATALOCATOR_OUTPUTMIX
    Source sink{&out, nullptr};
    SlId ids[] = {iid("SL_IID_ANDROIDSIMPLEBUFFERQUEUE"), iid("SL_IID_VOLUME"), required ? iid(required) : nullptr};
    SLuint32 req[] = {1, 0, 1};
    Itf p = nullptr;
    *result = fn<CreatePlayerFn>(engine, 2)(engine, &p, &src, &sink, required ? 3 : 2, ids, req);
    return *result == kSuccess ? p : nullptr;
}

Itf file_player(Itf engine, Itf mix, const void* locator, SLresult* result) {
    struct Mime {
        SLuint32 type;
        const char* mime;
        SLuint32 container;
    } mime{3, nullptr, 1};  // SL_DATAFORMAT_MIME, SL_CONTAINERTYPE_UNSPECIFIED
    Source src{locator, &mime};
    struct {
        SLuint32 type;
        Itf mix;
    } out{4, mix};
    Source sink{&out, nullptr};
    SlId ids[] = {iid("SL_IID_SEEK"), iid("SL_IID_VOLUME")};
    SLuint32 req[] = {1, 1};
    Itf p = nullptr;
    *result = fn<CreatePlayerFn>(engine, 2)(engine, &p, &src, &sink, 2, ids, req);
    return *result == kSuccess ? p : nullptr;
}

// Plays a file player to its end and checks what it reports.
void check_file_player(const char* what, Itf p) {
    check((std::string(what) + ": realize decodes the file").c_str(), realize(p) == kSuccess);
    Itf play = get(p, "SL_IID_PLAY");
    SLuint32 ms = 0;
    fn<GetMsFn>(play, 2)(play, &ms);
    check((std::string(what) + ": duration of the half-second tone").c_str(), ms >= 450 && ms <= 600,
          std::to_string(ms));
    g_play_events = 0;
    fn<RegisterFn>(play, 4)(play, reinterpret_cast<void*>(&on_play_event), nullptr);
    fn<SetStateFn>(play, 5)(play, 1);  // SL_PLAYEVENT_HEADATEND
    fn<SetStateFn>(play, 0)(play, 3);  // PLAYING
    Sleep(200);
    fn<GetMsFn>(play, 3)(play, &ms);
    check((std::string(what) + ": position advances").c_str(), ms > 50 && ms < 450, std::to_string(ms));
    for (int i = 0; i < 200 && g_play_events == 0; ++i) Sleep(10);
    check((std::string(what) + ": head-at-end event").c_str(), g_play_events == 1);
    SLuint32 state = 0;
    fn<GetStateFn>(play, 1)(play, &state);
    check((std::string(what) + ": paused at the end").c_str(), state == 2, std::to_string(state));
    destroy(p);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: winapk-opensles-test <dir with tone.wav>\n");
        return 2;
    }
    const std::string wav = std::string(argv[1]) + "\\tone.wav";
    std::printf("OpenSL ES\n");

    // The engine.
    auto create = reinterpret_cast<CreateEngineFn>(winapk::host::native_opensles_resolve("slCreateEngine"));
    check("SL_IID_ENGINE is a GUID", iid("SL_IID_ENGINE") != nullptr);
    Itf engine_obj = nullptr;
    SlId engine_ids[] = {iid("SL_IID_ENGINE")};
    SLuint32 engine_req[] = {1};
    check("slCreateEngine", create && create(&engine_obj, 0, nullptr, 1, engine_ids, engine_req) == kSuccess);
    check("engine: GetInterface before Realize is refused", get(engine_obj, "SL_IID_ENGINE") == nullptr);
    check("engine: Realize", realize(engine_obj) == kSuccess);
    Itf engine = get(engine_obj, "SL_IID_ENGINE");
    check("engine: SLEngineItf", engine != nullptr);
    SLuint32 n = 0;
    fn<QueryNumFn>(engine, 10)(engine, 0x1004, &n);  // SL_OBJECTID_AUDIOPLAYER
    check("engine: an audio player's interfaces", n >= 6, std::to_string(n));

    Itf mix = nullptr;
    check("CreateOutputMix", fn<CreateOutputMixFn>(engine, 7)(engine, &mix, 0, nullptr, nullptr) == kSuccess &&
                                 realize(mix) == kSuccess);

    // A 16-bit mono buffer-queue player, 100 ms buffers, re-enqueued by its callback.
    SLresult r = 0;
    Itf p = queue_player(engine, mix, 1, 16, &r);
    check("CreateAudioPlayer: buffer queue, 16-bit mono", p != nullptr, std::to_string(r));
    QueueCtx ctx;
    ctx.buf.resize(4410 * 2);
    for (size_t i = 0; i < 4410; ++i) {
        const int16_t s = static_cast<int16_t>(6000 * std::sin(i * 0.0627));
        std::memcpy(&ctx.buf[i * 2], &s, 2);
    }
    if (p) {
        check("player: Realize", realize(p) == kSuccess);
        Itf play = get(p, "SL_IID_PLAY");
        Itf bq = get(p, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
        Itf vol = get(p, "SL_IID_VOLUME");
        check("player: play, queue and volume interfaces", play && bq && vol);
        check("player: no seek on a buffer queue", get(p, "SL_IID_SEEK") == nullptr);
        fn<SetLevelFn>(vol, 0)(vol, -600);
        fn<RegisterFn>(bq, 3)(bq, reinterpret_cast<void*>(&on_buffer), &ctx);
        fn<EnqueueFn>(bq, 0)(bq, ctx.buf.data(), static_cast<SLuint32>(ctx.buf.size()));
        fn<EnqueueFn>(bq, 0)(bq, ctx.buf.data(), static_cast<SLuint32>(ctx.buf.size()));
        check("queue: a third buffer does not fit in two",
              fn<EnqueueFn>(bq, 0)(bq, ctx.buf.data(), static_cast<SLuint32>(ctx.buf.size())) == 7);
        fn<SetStateFn>(play, 0)(play, 3);
        Sleep(600);
        SLuint32 ms = 0;
        fn<GetMsFn>(play, 3)(play, &ms);
        SLuint32 qs[2] = {};
        fn<QueueStateFn>(bq, 2)(bq, qs);
        check("player: the callback re-enqueues as buffers are taken", ctx.calls >= 4,
              std::to_string(ctx.calls.load()));
        check("player: position advances", ms >= 300 && ms <= 900, std::to_string(ms));
        check("queue: GetState counts the buffers taken", qs[1] >= 4 && qs[0] <= 2,
              std::to_string(qs[0]) + " queued, index " + std::to_string(qs[1]));
        fn<SetStateFn>(play, 0)(play, 1);  // STOPPED clears the queue
        fn<QueueStateFn>(bq, 2)(bq, qs);
        check("player: stop clears the queue", qs[0] == 0);
        destroy(p);
    }

    // 24-bit stereo: converted to float on the way in.
    Itf p24 = queue_player(engine, mix, 2, 24, &r);
    check("CreateAudioPlayer: 24-bit stereo", p24 != nullptr, std::to_string(r));
    if (p24) {
        realize(p24);
        QueueCtx c24;
        c24.buf.assign(4410 * 6, 0);
        Itf bq = get(p24, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
        fn<RegisterFn>(bq, 3)(bq, reinterpret_cast<void*>(&on_buffer), &c24);
        fn<EnqueueFn>(bq, 0)(bq, c24.buf.data(), static_cast<SLuint32>(c24.buf.size()));
        Itf play = get(p24, "SL_IID_PLAY");
        fn<SetStateFn>(play, 0)(play, 3);
        for (int i = 0; i < 100 && c24.calls < 2; ++i) Sleep(10);
        check("24-bit player: buffers are taken", c24.calls >= 2, std::to_string(c24.calls.load()));
        destroy(p24);
    }

    check("CreateAudioPlayer: a required interface it lacks",
          queue_player(engine, mix, 1, 16, &r, "SL_IID_SEEK") == nullptr && r == kFeatureUnsupported,
          std::to_string(r));

    // Destroyed from inside its own callback (the feeder frees it).
    Itf pd = queue_player(engine, mix, 1, 16, &r);
    if (pd) {
        realize(pd);
        QueueCtx cd;
        cd.buf.assign(441 * 2, 0);
        cd.destroy_self = true;
        cd.object = pd;
        Itf bq = get(pd, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
        fn<RegisterFn>(bq, 3)(bq, reinterpret_cast<void*>(&on_buffer), &cd);
        fn<EnqueueFn>(bq, 0)(bq, cd.buf.data(), static_cast<SLuint32>(cd.buf.size()));
        Itf play = get(pd, "SL_IID_PLAY");
        fn<SetStateFn>(play, 0)(play, 3);
        for (int i = 0; i < 100 && cd.calls < 1; ++i) Sleep(10);
        Sleep(50);
        check("player: Destroy from its own callback", cd.calls == 1);
    }

    // An fd player (as AAsset_openFileDescriptor's) and a file URI player.
    const int fd = _open(wav.c_str(), _O_RDONLY | _O_BINARY);
    FdLocator fl{0x800007BC, fd, 0, -1};  // SL_DATALOCATOR_ANDROIDFD, to the end
    Itf pf = fd >= 0 ? file_player(engine, mix, &fl, &r) : nullptr;
    check("CreateAudioPlayer: Android fd", pf != nullptr, std::to_string(r));
    if (pf) check_file_player("fd player", pf);
    if (fd >= 0) _close(fd);

    std::string uri = "file:///host/" + wav;
    for (char& ch : uri) {
        if (ch == '\\') ch = '/';
    }
    UriLocator ul{1, uri.c_str()};
    Itf pu = file_player(engine, mix, &ul, &r);
    check("CreateAudioPlayer: file URI", pu != nullptr, std::to_string(r));
    if (pu) check_file_player("URI player", pu);
    UriLocator http{1, "http://example.invalid/a.ogg"};
    check("CreateAudioPlayer: an http URI is refused", file_player(engine, mix, &http, &r) == nullptr &&
                                                           r == kContentUnsupported);

    // A recorder: created (no JVM: RECORD_AUDIO is not modelled); realized
    // only where there is a capture device.
    IoLocator io{3, 1, 0xFFFFFFFF, nullptr};
    Locator rbq{0x800007BD, 2};
    Pcm rpcm{2, 1, 16000000, 16, 16, 4, 2};
    Source rsrc{&io, nullptr}, rsink{&rbq, &rpcm};
    SlId rids[] = {iid("SL_IID_ANDROIDSIMPLEBUFFERQUEUE")};
    SLuint32 rreq[] = {1};
    Itf rec = nullptr;
    using CreateRecorderFn = CreatePlayerFn;
    r = fn<CreateRecorderFn>(engine, 3)(engine, &rec, &rsrc, &rsink, 1, rids, rreq);
    check("CreateAudioRecorder", r == kSuccess && rec, std::to_string(r));
    if (rec) {
        const SLresult rr = realize(rec);
        check("recorder: Realize (or no capture device)", rr == kSuccess || rr == kContentUnsupported,
              rr == kSuccess ? "realized" : "no capture device");
        destroy(rec);
    }
    Pcm bad{2, 1, 16000000, 16, 16, 4, 7};  // no such byte order
    Source bsink{&rbq, &bad};
    check("CreateAudioRecorder: a bad descriptor", fn<CreateRecorderFn>(engine, 3)(engine, &rec, &rsrc, &bsink, 1, rids,
                                                                                    rreq) != kSuccess);
    (void)kParameterInvalid;
    (void)kPermissionDenied;

    destroy(mix);
    destroy(engine_obj);
    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures;
}
