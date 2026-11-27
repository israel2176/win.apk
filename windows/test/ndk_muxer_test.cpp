// ndk_muxer_test.cpp -- winapk-ndk-muxer-test: libmediandk's AMediaMuxer (host/ndk_media_muxer.cpp
// on host/webm_writer.h and host/mp4_writer.h), AMediaFormat and the crypto answers
// (host/native_media.cpp), called as an app's native code calls them: through the exported
// names, System V. Nothing here calls Windows, so it runs on Linux too (test/soft_codecs/
// CMakeLists.txt, under AddressSanitizer in CI) as well as in the Windows build.
//
//   winapk-ndk-muxer-test <dir with the test/video files>
//
// Checks:
//  - WebM: color.webm (VP9 and an Opus tone) and color_vp8.webm read by webm_demux.h, every
//    sample written to a new file through AMediaMuxer (from an offset in a bigger buffer, as
//    MediaCodec's output buffers hold them), the file read back: the same tracks, sizes and
//    codec data (the OpusHead, its codec delay and seek pre-roll), every sample's bytes, time
//    and sync flag; a muxer deleted while started still finishes its file, as MediaMuxer's
//    destructor stops it;
//  - MP4 and 3GPP: H.264 and AAC samples written, the boxes read back (the brand, two tracks of
//    the right handler and sample count, the first sample length-prefixed in mdat), a
//    BUFFER_FLAG_CODEC_CONFIG buffer taken as the codec data and not as a sample;
//  - the contract: formats a muxer cannot write (Ogg, HEIF) and no descriptor give null;
//    tracks a file cannot hold are AMEDIA_ERROR_UNSUPPORTED; calls out of order are
//    AMEDIA_ERROR_INVALID_OPERATION; bad angles, places and track indexes
//    AMEDIA_ERROR_INVALID_PARAMETER; getTrackCount and getTrackFormat;
//  - no DRM scheme: AMediaCrypto and AMediaDrm say no to Widevine and ClearKey;
//    AMediaCodecCryptoInfo keeps what it is given.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <sys/types.h>

#include "../host/native_media.h"
#include "../host/webm_demux.h"
#include "winapk/guest_abi.h"

namespace vd = winapk::video;

// ---- the stand-ins ---------------------------------------------------------------------------

namespace winapk::host {
// The codecs and the extractor (ndk_media_codec.cpp) are Windows'; this test has none.
void* ndk_media_codec_resolve(const char*) { return nullptr; }
}  // namespace winapk::host

namespace {

int g_failures = 0;

void check(const char* what, bool ok, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++g_failures;
}

template <typename F>
F fn(const char* name) {
    void* p = winapk::host::native_media_resolve(name);
    if (!p) {
        std::printf("  [FAIL] %s is not exported\n", name);
        std::exit(1);
    }
    return reinterpret_cast<F>(p);
}

#define GUEST WINAPK_GUEST_ABI
struct BufferInfo {
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
};

constexpr int kOk = 0, kErrUnsupported = -10002, kErrInvalidParameter = -10004, kErrInvalidOperation = -10005;
constexpr int kMpeg4 = 0, kWebm = 1, kThreeGpp = 2, kHeif = 3, kOgg = 4;
constexpr uint32_t kKeyFrame = 1, kCodecConfig = 2;

struct Ndk {
    void*(GUEST* format_new)() = fn<decltype(format_new)>("AMediaFormat_new");
    int(GUEST* format_delete)(void*) = fn<decltype(format_delete)>("AMediaFormat_delete");
    bool(GUEST* get_string)(void*, const char*, const char**) = fn<decltype(get_string)>("AMediaFormat_getString");
    void(GUEST* set_string)(void*, const char*, const char*) = fn<decltype(set_string)>("AMediaFormat_setString");
    void(GUEST* set_int32)(void*, const char*, int32_t) = fn<decltype(set_int32)>("AMediaFormat_setInt32");
    void(GUEST* set_buffer)(void*, const char*, const void*, size_t) = fn<decltype(set_buffer)>("AMediaFormat_setBuffer");

    void*(GUEST* mux_new)(int, int) = fn<decltype(mux_new)>("AMediaMuxer_new");
    int(GUEST* mux_delete)(void*) = fn<decltype(mux_delete)>("AMediaMuxer_delete");
    void*(GUEST* mux_append)(int, int) = fn<decltype(mux_append)>("AMediaMuxer_append");
    int(GUEST* mux_location)(void*, float, float) = fn<decltype(mux_location)>("AMediaMuxer_setLocation");
    int(GUEST* mux_orientation)(void*, int) = fn<decltype(mux_orientation)>("AMediaMuxer_setOrientationHint");
    ssize_t(GUEST* mux_add)(void*, void*) = fn<decltype(mux_add)>("AMediaMuxer_addTrack");
    int(GUEST* mux_start)(void*) = fn<decltype(mux_start)>("AMediaMuxer_start");
    int(GUEST* mux_write)(void*, size_t, const uint8_t*, const BufferInfo*) =
        fn<decltype(mux_write)>("AMediaMuxer_writeSampleData");
    int(GUEST* mux_stop)(void*) = fn<decltype(mux_stop)>("AMediaMuxer_stop");
    ssize_t(GUEST* mux_tracks)(void*) = fn<decltype(mux_tracks)>("AMediaMuxer_getTrackCount");
    void*(GUEST* mux_track_format)(void*, size_t) = fn<decltype(mux_track_format)>("AMediaMuxer_getTrackFormat");

    bool(GUEST* crypto_supported)(const uint8_t*) = fn<decltype(crypto_supported)>("AMediaCrypto_isCryptoSchemeSupported");
    void*(GUEST* crypto_new)(const uint8_t*, const void*, size_t) = fn<decltype(crypto_new)>("AMediaCrypto_new");
    bool(GUEST* drm_supported)(const uint8_t*, const char*) = fn<decltype(drm_supported)>("AMediaDrm_isCryptoSchemeSupported");
    void*(GUEST* drm_create)(const uint8_t*) = fn<decltype(drm_create)>("AMediaDrm_createByUUID");
    void*(GUEST* info_new)(int, const uint8_t*, const uint8_t*, int, const size_t*, const size_t*) =
        fn<decltype(info_new)>("AMediaCodecCryptoInfo_new");
    int(GUEST* info_delete)(void*) = fn<decltype(info_delete)>("AMediaCodecCryptoInfo_delete");
    size_t(GUEST* info_count)(void*) = fn<decltype(info_count)>("AMediaCodecCryptoInfo_getNumSubSamples");
    int(GUEST* info_key)(void*, uint8_t*) = fn<decltype(info_key)>("AMediaCodecCryptoInfo_getKey");
    int(GUEST* info_iv)(void*, uint8_t*) = fn<decltype(info_iv)>("AMediaCodecCryptoInfo_getIV");
    int(GUEST* info_mode)(void*) = fn<decltype(info_mode)>("AMediaCodecCryptoInfo_getMode");
    int(GUEST* info_clear)(void*, size_t*) = fn<decltype(info_clear)>("AMediaCodecCryptoInfo_getClearBytes");
    int(GUEST* info_encrypted)(void*, size_t*) = fn<decltype(info_encrypted)>("AMediaCodecCryptoInfo_getEncryptedBytes");
};

// ---- files -----------------------------------------------------------------------------------

std::string g_out_dir;

std::string out_path(const char* name) { return g_out_dir + "/" + name; }

int open_for_writing(const std::string& path) {
#ifdef _WIN32
    return _open(path.c_str(), _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY, 0644);
#else
    return open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
#endif
}
void close_fd(int fd) {
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> out;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        uint8_t b[8192];
        size_t got;
        while ((got = std::fread(b, 1, sizeof b, f)) > 0) out.insert(out.end(), b, b + got);
        std::fclose(f);
    }
    return out;
}

std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// ---- WebM ------------------------------------------------------------------------------------

struct Sample {
    int track;
    int64_t pts;
    bool key;
    std::vector<uint8_t> data;
};

struct Contents {
    std::vector<vd::Demuxer::Track> tracks;
    std::vector<Sample> samples;
};

Contents read_webm(const std::string& path) {
    Contents c;
    auto r = vd::WebmReader::open(wide(path));
    if (!r) return c;
    c.tracks = r->tracks();
    for (size_t i = 0; i < c.tracks.size(); ++i) r->select(static_cast<int>(i), true);
    vd::Demuxer::Sample s;
    while (r->next(&s)) c.samples.push_back({s.track, s.pts_us, s.key, s.data});
    return c;
}

void* format_of(const Ndk& n, const vd::Demuxer::Track& t) {
    void* f = n.format_new();
    n.set_string(f, "mime", t.mime.c_str());
    if (t.video) {
        n.set_int32(f, "width", t.width);
        n.set_int32(f, "height", t.height);
    } else {
        n.set_int32(f, "sample-rate", t.sample_rate);
        n.set_int32(f, "channel-count", t.channels);
    }
    if (!t.csd0.empty()) n.set_buffer(f, "csd-0", t.csd0.data(), t.csd0.size());
    if (!t.csd1.empty()) n.set_buffer(f, "csd-1", t.csd1.data(), t.csd1.size());
    if (!t.csd2.empty()) n.set_buffer(f, "csd-2", t.csd2.data(), t.csd2.size());
    return f;
}

// Writes `in` to `path` as WebM; with stop false the muxer is deleted while started.
bool write_webm(const Ndk& n, const Contents& in, const std::string& path, bool stop) {
    const int fd = open_for_writing(path);
    void* m = n.mux_new(fd, kWebm);
    close_fd(fd);  // the muxer has its own duplicate
    if (!m) return false;
    bool ok = true;
    for (const auto& t : in.tracks) {
        void* f = format_of(n, t);
        ok = ok && n.mux_add(m, f) >= 0;
        n.format_delete(f);
    }
    ok = ok && n.mux_start(m) == kOk;
    std::vector<uint8_t> buf;
    for (const Sample& s : in.samples) {
        // Three bytes in, as an output buffer's offset may put it.
        buf.assign(3, 0xee);
        buf.insert(buf.end(), s.data.begin(), s.data.end());
        const BufferInfo info{3, static_cast<int32_t>(s.data.size()), s.pts, s.key ? kKeyFrame : 0u};
        ok = ok && n.mux_write(m, static_cast<size_t>(s.track), buf.data(), &info) == kOk;
    }
    if (stop) ok = ok && n.mux_stop(m) == kOk && n.mux_stop(m) == kErrInvalidOperation;
    n.mux_delete(m);
    return ok;
}

// Writes `in` to `path` as WebM with every sample of the first sample's track `late_us` later, so
// that the other track's first sample, written after it, is the earlier one, then an empty
// end-of-stream buffer on each track, as an encoder's last output is passed on.
bool write_webm_late_track(const Ndk& n, const Contents& in, const std::string& path, int64_t late_us) {
    if (in.samples.empty()) return false;
    const int late_track = in.samples.front().track;
    const int fd = open_for_writing(path);
    void* m = n.mux_new(fd, kWebm);
    close_fd(fd);
    if (!m) return false;
    bool ok = true;
    for (const auto& t : in.tracks) {
        void* f = format_of(n, t);
        ok = ok && n.mux_add(m, f) >= 0;
        n.format_delete(f);
    }
    ok = ok && n.mux_start(m) == kOk;
    int64_t last = 0;
    for (const Sample& s : in.samples) {
        const int64_t pts = s.pts + (s.track == late_track ? late_us : 0);
        last = std::max(last, pts);
        const BufferInfo info{0, static_cast<int32_t>(s.data.size()), pts, s.key ? kKeyFrame : 0u};
        ok = ok && n.mux_write(m, static_cast<size_t>(s.track), s.data.data(), &info) == kOk;
    }
    constexpr uint32_t kEndOfStream = 4;
    const uint8_t none = 0;
    for (size_t t = 0; t < in.tracks.size(); ++t) {
        const BufferInfo eos{0, 0, last, kEndOfStream};
        ok = ok && n.mux_write(m, t, &none, &eos) == kOk;
    }
    ok = ok && n.mux_stop(m) == kOk;
    n.mux_delete(m);
    return ok;
}

void compare(const char* what, const Contents& a, const Contents& b) {
    const std::string w = what;
    bool same_tracks = a.tracks.size() == b.tracks.size() && !a.tracks.empty();
    for (size_t i = 0; same_tracks && i < a.tracks.size(); ++i) {
        const auto &x = a.tracks[i], &y = b.tracks[i];
        same_tracks = x.mime == y.mime && x.width == y.width && x.height == y.height &&
                      x.sample_rate == y.sample_rate && x.channels == y.channels && x.csd0 == y.csd0 &&
                      x.csd1 == y.csd1 && x.csd2 == y.csd2;
    }
    check((w + ": the same tracks and codec data").c_str(), same_tracks,
          std::to_string(a.tracks.size()) + " / " + std::to_string(b.tracks.size()) + " tracks");
    size_t same = 0;
    std::string first_diff;
    for (size_t i = 0; i < a.samples.size() && i < b.samples.size(); ++i) {
        const Sample &x = a.samples[i], &y = b.samples[i];
        const bool video = a.tracks[static_cast<size_t>(x.track)].video;
        // Matroska keeps times in milliseconds; the files start at 0.
        if (x.track == y.track && x.data == y.data && std::llabs(x.pts - y.pts) < 1000 && (!video || x.key == y.key)) {
            ++same;
        } else if (first_diff.empty()) {
            first_diff = ", sample " + std::to_string(i) + " differs (" + std::to_string(x.pts) + " / " +
                         std::to_string(y.pts) + ")";
        }
    }
    check((w + ": every sample, its time and sync flag").c_str(),
          same == a.samples.size() && a.samples.size() == b.samples.size() && !a.samples.empty(),
          std::to_string(same) + " of " + std::to_string(a.samples.size()) + " / " + std::to_string(b.samples.size()) +
              first_diff);
}

void webm_checks(const Ndk& n, const std::string& dir) {
    for (const char* name : {"color.webm", "color_vp8.webm"}) {
        std::printf("WebM: %s\n", name);
        const Contents in = read_webm(dir + "/" + name);
        check("the file reads", !in.samples.empty(), std::to_string(in.samples.size()) + " samples");
        const std::string out = out_path((std::string("remux-") + name).c_str());
        check("written through AMediaMuxer", write_webm(n, in, out, true));
        compare("read back", in, read_webm(out));
    }
    std::printf("WebM: deleted while started\n");
    const Contents in = read_webm(dir + "/color.webm");
    {
        // Each track's times start at its own first sample's, as AOSP's WebmWriter keeps them: a
        // track whose first sample is written second but is earlier is not cut to 0.
        std::printf("WebM: a track starting later, and end-of-stream buffers\n");
        const std::string late = out_path("remux-late-track.webm");
        check("two tracks to write", in.tracks.size() == 2);
        check("written, the empty end-of-stream buffers taken", write_webm_late_track(n, in, late, 500000));
        compare("each track from its own start", in, read_webm(late));
    }
    const std::string out = out_path("remux-unstopped.webm");
    write_webm(n, in, out, false);
    compare("deleted without stop", in, read_webm(out));
}

// ---- MP4 -------------------------------------------------------------------------------------

struct Box {
    std::string type;
    size_t body = 0, end = 0;  // offsets into the file
};

uint32_t be32(const std::vector<uint8_t>& f, size_t at) {
    return at + 4 <= f.size() ? (uint32_t{f[at]} << 24 | uint32_t{f[at + 1]} << 16 | uint32_t{f[at + 2]} << 8 | f[at + 3])
                              : 0;
}

std::vector<Box> boxes(const std::vector<uint8_t>& f, size_t from, size_t to) {
    std::vector<Box> out;
    while (from + 8 <= to) {
        uint64_t size = be32(f, from);
        size_t header = 8;
        if (size == 1) {
            size = uint64_t{be32(f, from + 8)} << 32 | be32(f, from + 12);
            header = 16;
        }
        if (size < header || from + size > to) break;
        out.push_back({std::string(reinterpret_cast<const char*>(&f[from + 4]), 4), from + header,
                       static_cast<size_t>(from + size)});
        from += static_cast<size_t>(size);
    }
    return out;
}

const Box* find(const std::vector<Box>& list, const char* type) {
    for (const Box& b : list)
        if (b.type == type) return &b;
    return nullptr;
}

// The box at the path ("mdia/minf/stbl/stsz") under `parent`.
const Box* under(const std::vector<uint8_t>& f, const Box& parent, const std::vector<const char*>& path,
                 std::vector<std::vector<Box>>* keep) {
    const Box* at = &parent;
    for (const char* type : path) {
        keep->push_back(boxes(f, at->body, at->end));
        at = find(keep->back(), type);
        if (!at) return nullptr;
    }
    return at;
}

// A made-up SPS and PPS (the writer only copies them into avcC) and access units around them.
const std::vector<uint8_t> kSps = {0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1e, 0xda, 0x02, 0x80, 0xbf, 0xe5};
const std::vector<uint8_t> kPps = {0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80};

std::vector<uint8_t> access_unit(bool key, int i) {
    std::vector<uint8_t> au = {0, 0, 0, 1, static_cast<uint8_t>(key ? 0x65 : 0x41)};
    for (int k = 0; k < 40 + i; ++k) au.push_back(static_cast<uint8_t>(i * 7 + k));
    return au;
}

void mp4_checks(const Ndk& n, int format, const char* label) {
    std::printf("%s: H.264 and AAC\n", label);
    const std::string path = out_path(format == kThreeGpp ? "mux.3gp" : "mux.mp4");
    const int fd = open_for_writing(path);
    void* m = n.mux_new(fd, format);
    close_fd(fd);
    check("a muxer", m != nullptr);
    if (!m) return;

    BufferInfo early{0, 4, 0, 0};
    const uint8_t four[4] = {1, 2, 3, 4};
    check("writeSampleData before start: invalid operation", n.mux_write(m, 0, four, &early) == kErrInvalidOperation);
    check("start with no track: invalid operation", n.mux_start(m) == kErrInvalidOperation);
    check("an angle of 45: invalid parameter", n.mux_orientation(m, 45) == kErrInvalidParameter);
    check("a latitude of 100: invalid parameter", n.mux_location(m, 100, 0) == kErrInvalidParameter);
    check("an angle of 90 and a place", n.mux_orientation(m, 90) == kOk && n.mux_location(m, 32.08f, 34.78f) == kOk);

    void* hevc = n.format_new();
    n.set_string(hevc, "mime", "video/hevc");
    n.set_int32(hevc, "width", 64);
    n.set_int32(hevc, "height", 48);
    check("HEVC in MP4: unsupported here", n.mux_add(m, hevc) == kErrUnsupported);
    n.format_delete(hevc);

    // The video track's format has no codec data: it comes as a CODEC_CONFIG buffer, as an
    // encoder's first output buffer brings it.
    void* video = n.format_new();
    n.set_string(video, "mime", "video/avc");
    n.set_int32(video, "width", 64);
    n.set_int32(video, "height", 48);
    void* audio = n.format_new();
    n.set_string(audio, "mime", "audio/mp4a-latm");
    n.set_int32(audio, "sample-rate", 44100);
    n.set_int32(audio, "channel-count", 1);
    const uint8_t asc[2] = {0x12, 0x08};  // AAC-LC, 44100 Hz, mono
    n.set_buffer(audio, "csd-0", asc, sizeof asc);
    const ssize_t v = n.mux_add(m, video), a = n.mux_add(m, audio);
    check("two tracks", v == 0 && a == 1 && n.mux_tracks(m) == 2);
    void* back = n.mux_track_format(m, 1);
    const char* mime = nullptr;
    check("getTrackFormat", back && n.get_string(back, "mime", &mime) && std::string(mime) == "audio/mp4a-latm");
    if (back) n.format_delete(back);
    n.format_delete(video);
    n.format_delete(audio);

    check("started", n.mux_start(m) == kOk);
    void* late = n.format_new();
    n.set_string(late, "mime", "audio/mp4a-latm");
    check("addTrack after start: invalid operation", n.mux_add(m, late) == kErrInvalidOperation);
    n.format_delete(late);
    check("setOrientationHint after start: invalid operation", n.mux_orientation(m, 0) == kErrInvalidOperation);
    BufferInfo bad{0, 4, 0, 0};
    check("track 5: invalid parameter", n.mux_write(m, 5, four, &bad) == kErrInvalidParameter);

    std::vector<uint8_t> config = kSps;
    config.insert(config.end(), kPps.begin(), kPps.end());
    BufferInfo ci{0, static_cast<int32_t>(config.size()), 0, kCodecConfig};
    bool wrote = n.mux_write(m, 0, config.data(), &ci) == kOk;
    std::vector<uint8_t> first;
    for (int i = 0; i < 10; ++i) {
        const std::vector<uint8_t> au = access_unit(i == 0, i);
        if (i == 0) first = au;
        const BufferInfo info{0, static_cast<int32_t>(au.size()), i * 40000, i == 0 ? kKeyFrame : 0u};
        wrote = wrote && n.mux_write(m, 0, au.data(), &info) == kOk;
        for (int k = 0; k < 2; ++k) {
            std::vector<uint8_t> frame(100, static_cast<uint8_t>(i * 2 + k));
            const BufferInfo ai{0, 100, (i * 2 + k) * 23220, 0};
            wrote = wrote && n.mux_write(m, 1, frame.data(), &ai) == kOk;
        }
    }
    check("ten pictures and twenty AAC frames written", wrote);
    check("stopped", n.mux_stop(m) == kOk);
    check("written after stop: invalid operation", n.mux_write(m, 0, first.data(), &ci) == kErrInvalidOperation);
    n.mux_delete(m);

    const std::vector<uint8_t> f = read_file(path);
    const std::vector<Box> top = boxes(f, 0, f.size());
    const Box* ftyp = find(top, "ftyp");
    const std::string brand = ftyp ? std::string(reinterpret_cast<const char*>(&f[ftyp->body]), 4) : "";
    check("ftyp, mdat, moov", ftyp && find(top, "mdat") && find(top, "moov"), brand);
    check("the brand", brand == (format == kThreeGpp ? "3gp4" : "mp42"), brand);
    const Box* moov = find(top, "moov");
    std::vector<Box> traks;
    if (moov)
        for (const Box& b : boxes(f, moov->body, moov->end))
            if (b.type == "trak") traks.push_back(b);
    std::vector<std::vector<Box>> keep;
    std::string handlers, counts;
    for (const Box& t : traks) {
        const Box* hdlr = under(f, t, {"mdia", "hdlr"}, &keep);
        const Box* stsz = under(f, t, {"mdia", "minf", "stbl", "stsz"}, &keep);
        handlers += hdlr ? std::string(reinterpret_cast<const char*>(&f[hdlr->body + 8]), 4) + " " : "? ";
        counts += stsz ? std::to_string(be32(f, stsz->body + 8)) + " " : "? ";
    }
    check("a video track of 10 samples, a sound track of 20", handlers == "vide soun " && counts == "10 20 ",
          handlers + "/ " + counts);
    // avcC holds the SPS and PPS the CODEC_CONFIG buffer brought.
    const Box* avcc = traks.empty() ? nullptr
                                    : under(f, traks[0], {"mdia", "minf", "stbl", "stsd"}, &keep);
    bool sps_in_avcc = false;
    if (avcc) {
        const std::vector<uint8_t> sps(kSps.begin() + 4, kSps.end());
        sps_in_avcc = std::search(f.begin() + static_cast<long>(avcc->body), f.begin() + static_cast<long>(avcc->end),
                                  sps.begin(), sps.end()) != f.begin() + static_cast<long>(avcc->end);
    }
    check("the codec data in avcC, not a sample", sps_in_avcc);
    // The first access unit in mdat: its NAL unit behind a 4-byte length instead of a start code.
    std::vector<uint8_t> prefixed = {0, 0, 0, static_cast<uint8_t>(first.size() - 4)};
    prefixed.insert(prefixed.end(), first.begin() + 4, first.end());
    const Box* mdat = find(top, "mdat");
    check("the first picture length-prefixed in mdat",
          mdat && std::search(f.begin() + static_cast<long>(mdat->body), f.begin() + static_cast<long>(mdat->end),
                              prefixed.begin(), prefixed.end()) != f.begin() + static_cast<long>(mdat->end));
}

// ---- the contract ----------------------------------------------------------------------------

void contract_checks(const Ndk& n) {
    std::printf("the contract\n");
    const std::string path = out_path("contract.webm");
    int fd = open_for_writing(path);
    check("no Ogg output", n.mux_new(fd, kOgg) == nullptr);
    check("no HEIF output", n.mux_new(fd, kHeif) == nullptr);
    check("no appending", n.mux_append(fd, 0) == nullptr);
    void* m = n.mux_new(fd, kWebm);
    close_fd(fd);
    check("no descriptor: null", n.mux_new(-1, kMpeg4) == nullptr);
    void* avc = n.format_new();
    n.set_string(avc, "mime", "video/avc");
    n.set_int32(avc, "width", 64);
    n.set_int32(avc, "height", 48);
    check("H.264 in WebM: unsupported", m && n.mux_add(m, avc) == kErrUnsupported);
    n.format_delete(avc);
    void* vp9 = n.format_new();
    n.set_string(vp9, "mime", "video/x-vnd.on2.vp9");
    n.set_int32(vp9, "width", 64);
    n.set_int32(vp9, "height", 48);
    check("one VP9 track", m && n.mux_add(m, vp9) == 0);
    check("a second video track in WebM: unsupported", m && n.mux_add(m, vp9) == kErrUnsupported);
    n.format_delete(vp9);
    void* opus = n.format_new();
    n.set_string(opus, "mime", "audio/opus");
    n.set_int32(opus, "sample-rate", 48000);
    n.set_int32(opus, "channel-count", 1);
    check("Opus without its OpusHead: invalid parameter", m && n.mux_add(m, opus) == kErrInvalidParameter);
    n.format_delete(opus);
    check("stop before start: invalid operation", m && n.mux_stop(m) == kErrInvalidOperation);
    if (m) n.mux_delete(m);
}

void crypto_checks(const Ndk& n) {
    std::printf("crypto\n");
    const uint8_t widevine[16] = {0xed, 0xef, 0x8b, 0xa9, 0x79, 0xd6, 0x4a, 0xce,
                                  0xa3, 0xc8, 0x27, 0xdc, 0xd5, 0x1d, 0x21, 0xed};
    const uint8_t clearkey[16] = {0xe2, 0x71, 0x9d, 0x58, 0xa9, 0x85, 0xb3, 0xc9,
                                  0x78, 0x1a, 0xb0, 0x30, 0xaf, 0x78, 0xd3, 0x0e};
    check("no Widevine, no ClearKey",
          !n.crypto_supported(widevine) && !n.crypto_supported(clearkey) && !n.drm_supported(widevine, "video/mp4") &&
              n.drm_create(widevine) == nullptr && n.crypto_new(clearkey, nullptr, 0) == nullptr);
    uint8_t key[16], iv[16];
    for (int i = 0; i < 16; ++i) key[i] = static_cast<uint8_t>(i), iv[i] = static_cast<uint8_t>(0xf0 + i);
    const size_t clear[2] = {10, 20}, encrypted[2] = {100, 200};
    void* info = n.info_new(2, key, iv, 1, clear, encrypted);
    uint8_t key_back[16] = {}, iv_back[16] = {};
    size_t clear_back[2] = {}, encrypted_back[2] = {};
    const bool kept = info && n.info_count(info) == 2 && n.info_key(info, key_back) == kOk &&
                      n.info_iv(info, iv_back) == kOk && n.info_mode(info) == 1 &&
                      n.info_clear(info, clear_back) == kOk && n.info_encrypted(info, encrypted_back) == kOk &&
                      std::memcmp(key, key_back, 16) == 0 && std::memcmp(iv, iv_back, 16) == 0 &&
                      clear_back[1] == 20 && encrypted_back[1] == 200;
    check("AMediaCodecCryptoInfo keeps what it is given", kept);
    if (info) n.info_delete(info);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: winapk-ndk-muxer-test <dir with the test/video files> [out dir]\n");
        return 2;
    }
    const std::string dir = argv[1];
    if (argc > 2) {
        g_out_dir = argv[2];
    } else {
#ifdef _WIN32
        const char* tmp = std::getenv("TEMP");
#else
        const char* tmp = std::getenv("TMPDIR");
#endif
        g_out_dir = tmp && *tmp ? tmp : (
#ifdef _WIN32
                                            "."
#else
                                            "/tmp"
#endif
                                        );
    }
    Ndk n;
    webm_checks(n, dir);
    mp4_checks(n, kMpeg4, "MP4");
    mp4_checks(n, kThreeGpp, "3GPP");
    contract_checks(n);
    crypto_checks(n);
    std::printf(g_failures ? "%d FAILED\n" : "ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
