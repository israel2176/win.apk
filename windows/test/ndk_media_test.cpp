// ndk_media_test.cpp -- winapk-ndk-media-test: libmediandk's AMediaExtractor, AMediaCodec,
// AMediaDataSource and AMediaMuxer (host/ndk_media_codec.cpp, host/ndk_media_muxer.cpp,
// host/native_media.cpp) without a JVM or a guest library, called as guest code calls them:
// through the exported names, System V.
//
//   winapk-ndk-media-test <dir with color.mp4, color.webm>
//
// color.mp4 and color.webm (test/video) are one second of 64x48 video at 25 fps, red in the top
// half and blue in the bottom (H.264 and VP9), with a 440 Hz mono tone (AAC and Opus). Checks:
// the extractor gives both tracks with their codec data and every sample, and seeks; a decoder
// made from a track's format gives INFO_OUTPUT_FORMAT_CHANGED, then 25 I420 pictures of the
// right colours in presentation order, then an empty end-of-stream buffer; the audio decoders
// give 16-bit PCM of the tone; codec data queued as BUFFER_FLAG_CODEC_CONFIG buffers instead
// of the format's works too; the asynchronous callbacks deliver the same; a descriptor and a
// data source of the app's read like the file; the names are MediaCodecList's. A Windows
// without Media Foundation's H.264 or AAC decoder skips those and says so.
//
// Then the encoders and the muxer, as an app records: 30 red-over-blue I420 pictures through
// the H.264 encoder and a second of the tone through the AAC encoder (the output format with
// its codec data first, the codec data as a CODEC_CONFIG buffer, a key frame first and one on
// request, the end of stream), both written to an MP4 by AMediaMuxer through a descriptor, and
// that file read back by AMediaExtractor and decoded; and color.webm's tracks copied by
// AMediaExtractor into a new WebM by AMediaMuxer, decoded like the original. A Windows without
// Media Foundation's encoders skips the first and says so (the muxer's own checks are
// test/ndk_muxer_test.cpp's).
//
// The stand-ins: arm64::host_callable (no arm64 code), android_host_path (a host path is
// itself) and the native window (a frame shown is counted).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <io.h>

#include <windows.h>

#include "../host/native_media.h"
#include "../host/native_window.h"
#include "winapk/guest_abi.h"

// ---- the stand-ins ---------------------------------------------------------------------------

namespace winapk {
namespace arm64 {
void* host_callable(void* fn) { return fn; }
}  // namespace arm64
std::wstring android_host_path(const char* path) {
    std::string p = path;
    return std::wstring(p.begin(), p.end());
}
namespace host {
namespace {
int g_window = 0;
std::atomic<int> g_frames_shown{0};
}  // namespace
const void* native_window_handle() { return &g_window; }
void guest_frame_present(const uint32_t*, int, int) { ++g_frames_shown; }
}  // namespace host
}  // namespace winapk

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
struct Callbacks {
    void(GUEST* input)(void*, void*, int32_t);
    void(GUEST* output)(void*, void*, int32_t, BufferInfo*);
    void(GUEST* format)(void*, void*, void*);
    void(GUEST* error)(void*, void*, int, int32_t, const char*);
};

// The functions, by their NDK names.
struct Ndk {
    void*(GUEST* format_new)() = fn<decltype(format_new)>("AMediaFormat_new");
    int(GUEST* format_delete)(void*) = fn<decltype(format_delete)>("AMediaFormat_delete");
    bool(GUEST* get_int32)(void*, const char*, int32_t*) = fn<decltype(get_int32)>("AMediaFormat_getInt32");
    bool(GUEST* get_int64)(void*, const char*, int64_t*) = fn<decltype(get_int64)>("AMediaFormat_getInt64");
    bool(GUEST* get_string)(void*, const char*, const char**) = fn<decltype(get_string)>("AMediaFormat_getString");
    bool(GUEST* get_buffer)(void*, const char*, void**, size_t*) = fn<decltype(get_buffer)>("AMediaFormat_getBuffer");
    void(GUEST* set_string)(void*, const char*, const char*) = fn<decltype(set_string)>("AMediaFormat_setString");
    void(GUEST* set_int32)(void*, const char*, int32_t) = fn<decltype(set_int32)>("AMediaFormat_setInt32");
    int(GUEST* copy)(void*, void*) = fn<decltype(copy)>("AMediaFormat_copy");

    void*(GUEST* ex_new)() = fn<decltype(ex_new)>("AMediaExtractor_new");
    int(GUEST* ex_delete)(void*) = fn<decltype(ex_delete)>("AMediaExtractor_delete");
    int(GUEST* ex_source)(void*, const char*) = fn<decltype(ex_source)>("AMediaExtractor_setDataSource");
    int(GUEST* ex_source_fd)(void*, int, int64_t, int64_t) = fn<decltype(ex_source_fd)>("AMediaExtractor_setDataSourceFd");
    int(GUEST* ex_source_custom)(void*, void*) = fn<decltype(ex_source_custom)>("AMediaExtractor_setDataSourceCustom");
    size_t(GUEST* ex_tracks)(void*) = fn<decltype(ex_tracks)>("AMediaExtractor_getTrackCount");
    void*(GUEST* ex_track_format)(void*, size_t) = fn<decltype(ex_track_format)>("AMediaExtractor_getTrackFormat");
    int(GUEST* ex_select)(void*, size_t) = fn<decltype(ex_select)>("AMediaExtractor_selectTrack");
    int(GUEST* ex_unselect)(void*, size_t) = fn<decltype(ex_unselect)>("AMediaExtractor_unselectTrack");
    bool(GUEST* ex_advance)(void*) = fn<decltype(ex_advance)>("AMediaExtractor_advance");
    int(GUEST* ex_seek)(void*, int64_t, int) = fn<decltype(ex_seek)>("AMediaExtractor_seekTo");
    int64_t(GUEST* ex_read)(void*, uint8_t*, size_t) = fn<decltype(ex_read)>("AMediaExtractor_readSampleData");
    int64_t(GUEST* ex_time)(void*) = fn<decltype(ex_time)>("AMediaExtractor_getSampleTime");
    int(GUEST* ex_track)(void*) = fn<decltype(ex_track)>("AMediaExtractor_getSampleTrackIndex");
    uint32_t(GUEST* ex_flags)(void*) = fn<decltype(ex_flags)>("AMediaExtractor_getSampleFlags");

    void*(GUEST* ds_new)() = fn<decltype(ds_new)>("AMediaDataSource_new");
    void(GUEST* ds_delete)(void*) = fn<decltype(ds_delete)>("AMediaDataSource_delete");
    void(GUEST* ds_userdata)(void*, void*) = fn<decltype(ds_userdata)>("AMediaDataSource_setUserdata");
    void(GUEST* ds_read_at)(void*, int64_t(GUEST*)(void*, int64_t, void*, size_t)) =
        fn<decltype(ds_read_at)>("AMediaDataSource_setReadAt");
    void(GUEST* ds_size)(void*, int64_t(GUEST*)(void*)) = fn<decltype(ds_size)>("AMediaDataSource_setGetSize");

    void*(GUEST* by_type)(const char*) = fn<decltype(by_type)>("AMediaCodec_createDecoderByType");
    void*(GUEST* encoder)(const char*) = fn<decltype(encoder)>("AMediaCodec_createEncoderByType");
    void*(GUEST* by_name)(const char*) = fn<decltype(by_name)>("AMediaCodec_createCodecByName");
    int(GUEST* get_name)(void*, char**) = fn<decltype(get_name)>("AMediaCodec_getName");
    void(GUEST* release_name)(void*, char*) = fn<decltype(release_name)>("AMediaCodec_releaseName");
    int(GUEST* codec_delete)(void*) = fn<decltype(codec_delete)>("AMediaCodec_delete");
    int(GUEST* configure)(void*, void*, const void*, void*, uint32_t) = fn<decltype(configure)>("AMediaCodec_configure");
    int(GUEST* async)(void*, Callbacks, void*) = fn<decltype(async)>("AMediaCodec_setAsyncNotifyCallback");
    int(GUEST* start)(void*) = fn<decltype(start)>("AMediaCodec_start");
    int(GUEST* stop)(void*) = fn<decltype(stop)>("AMediaCodec_stop");
    int(GUEST* flush)(void*) = fn<decltype(flush)>("AMediaCodec_flush");
    int64_t(GUEST* dq_in)(void*, int64_t) = fn<decltype(dq_in)>("AMediaCodec_dequeueInputBuffer");
    uint8_t*(GUEST* in_buf)(void*, size_t, size_t*) = fn<decltype(in_buf)>("AMediaCodec_getInputBuffer");
    int(GUEST* queue)(void*, size_t, int64_t, size_t, uint64_t, uint32_t) = fn<decltype(queue)>("AMediaCodec_queueInputBuffer");
    int64_t(GUEST* dq_out)(void*, BufferInfo*, int64_t) = fn<decltype(dq_out)>("AMediaCodec_dequeueOutputBuffer");
    uint8_t*(GUEST* out_buf)(void*, size_t, size_t*) = fn<decltype(out_buf)>("AMediaCodec_getOutputBuffer");
    void*(GUEST* out_format)(void*) = fn<decltype(out_format)>("AMediaCodec_getOutputFormat");
    int(GUEST* release)(void*, size_t, bool) = fn<decltype(release)>("AMediaCodec_releaseOutputBuffer");
    int(GUEST* set_params)(void*, void*) = fn<decltype(set_params)>("AMediaCodec_setParameters");

    void*(GUEST* mux_new)(int, int) = fn<decltype(mux_new)>("AMediaMuxer_new");
    int(GUEST* mux_delete)(void*) = fn<decltype(mux_delete)>("AMediaMuxer_delete");
    ssize_t(GUEST* mux_add)(void*, void*) = fn<decltype(mux_add)>("AMediaMuxer_addTrack");
    int(GUEST* mux_start)(void*) = fn<decltype(mux_start)>("AMediaMuxer_start");
    int(GUEST* mux_write)(void*, size_t, const uint8_t*, const BufferInfo*) =
        fn<decltype(mux_write)>("AMediaMuxer_writeSampleData");
    int(GUEST* mux_stop)(void*) = fn<decltype(mux_stop)>("AMediaMuxer_stop");
};

constexpr int64_t kFormatChanged = -2;
constexpr uint32_t kKeyFrame = 1, kCodecConfig = 2, kEndOfStream = 4;
constexpr int kMuxMpeg4 = 0, kMuxWebm = 1;

std::string mime_of(const Ndk& n, void* format) {
    const char* m = nullptr;
    return n.get_string(format, "mime", &m) && m ? m : "";
}

// The track of the extractor whose mime starts with `kind` ("video/", "audio/"), or -1.
int track_of(const Ndk& n, void* ex, const char* kind) {
    for (size_t i = 0; i < n.ex_tracks(ex); ++i) {
        void* f = n.ex_track_format(ex, i);
        const bool match = mime_of(n, f).rfind(kind, 0) == 0;
        n.format_delete(f);
        if (match) return static_cast<int>(i);
    }
    return -1;
}

// What a decode gave.
struct Decoded {
    bool opened = false;
    bool format_first = false;  // INFO_OUTPUT_FORMAT_CHANGED came before any buffer
    bool eos = false;
    int buffers = 0;
    std::vector<int64_t> pts;
    int width = 0, height = 0, stride = 0, slice_height = 0, color_format = 0;
    int rate = 0, channels = 0;
    int track_rate = 0;  // the extractor's sample-rate for the track
    std::vector<uint8_t> first_picture;  // I420, stride x slice_height luma
    std::vector<int16_t> pcm;
};

void read_output_format(const Ndk& n, void* codec, Decoded* d) {
    void* f = n.out_format(codec);
    int32_t v = 0;
    if (n.get_int32(f, "width", &v)) d->width = v;
    if (n.get_int32(f, "height", &v)) d->height = v;
    if (n.get_int32(f, "stride", &v)) d->stride = v;
    if (n.get_int32(f, "slice-height", &v)) d->slice_height = v;
    if (n.get_int32(f, "color-format", &v)) d->color_format = v;
    if (n.get_int32(f, "sample-rate", &v)) d->rate = v;
    if (n.get_int32(f, "channel-count", &v)) d->channels = v;
    n.format_delete(f);
}

void take_output(const Ndk& n, void* codec, int64_t index, const BufferInfo& info, bool video, Decoded* d) {
    if (info.flags & kEndOfStream) d->eos = true;
    if (info.size > 0) {
        ++d->buffers;
        d->pts.push_back(info.presentationTimeUs);
        size_t cap = 0;
        const uint8_t* p = n.out_buf(codec, static_cast<size_t>(index), &cap);
        if (p && video && d->first_picture.empty()) d->first_picture.assign(p + info.offset, p + info.offset + info.size);
        if (p && !video) {
            const auto* s = reinterpret_cast<const int16_t*>(p + info.offset);
            d->pcm.insert(d->pcm.end(), s, s + info.size / 2);
        }
    }
    n.release(codec, static_cast<size_t>(index), false);
}

// Decodes the track of `kind` of `path` synchronously. With in_band, the format's csd-* are
// taken out and queued as CODEC_CONFIG buffers instead.
Decoded decode_sync(const Ndk& n, const std::string& path, const char* kind, bool in_band) {
    Decoded d;
    void* ex = n.ex_new();
    if (n.ex_source(ex, path.c_str()) != 0) return d;
    const int track = track_of(n, ex, kind);
    if (track < 0) return d;
    void* format = n.ex_track_format(ex, static_cast<size_t>(track));
    const std::string mime = mime_of(n, format);
    const bool video = mime.rfind("video/", 0) == 0;
    int32_t track_rate = 0;
    if (n.get_int32(format, "sample-rate", &track_rate)) d.track_rate = track_rate;
    std::vector<std::vector<uint8_t>> csd;
    void* config = format;
    if (in_band) {
        for (const char* key : {"csd-0", "csd-1", "csd-2"}) {
            void* data = nullptr;
            size_t size = 0;
            if (n.get_buffer(format, key, &data, &size))
                csd.emplace_back(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
        }
        config = n.format_new();
        n.set_string(config, "mime", mime.c_str());
        int32_t v = 0;
        for (const char* key : {"width", "height", "sample-rate", "channel-count", "max-input-size"})
            if (n.get_int32(format, key, &v)) n.set_int32(config, key, v);
    }
    void* codec = n.by_type(mime.c_str());
    if (!codec || n.configure(codec, config, nullptr, nullptr, 0) != 0 || n.start(codec) != 0) {
        if (codec) n.codec_delete(codec);
        if (config != format) n.format_delete(config);
        n.format_delete(format);
        n.ex_delete(ex);
        return d;
    }
    d.opened = true;
    n.ex_select(ex, static_cast<size_t>(track));
    size_t next_csd = 0;
    bool input_done = false, seen_buffer = false;
    for (int spins = 0; spins < 10000 && !d.eos; ++spins) {
        if (!input_done) {
            const int64_t i = n.dq_in(codec, 0);
            if (i >= 0) {
                size_t cap = 0;
                uint8_t* buf = n.in_buf(codec, static_cast<size_t>(i), &cap);
                if (next_csd < csd.size()) {
                    std::memcpy(buf, csd[next_csd].data(), csd[next_csd].size());
                    n.queue(codec, static_cast<size_t>(i), 0, csd[next_csd].size(), 0, kCodecConfig);
                    ++next_csd;
                } else {
                    const int64_t size = n.ex_read(ex, buf, cap);
                    if (size < 0) {
                        n.queue(codec, static_cast<size_t>(i), 0, 0, 0, kEndOfStream);
                        input_done = true;
                    } else {
                        n.queue(codec, static_cast<size_t>(i), 0, static_cast<size_t>(size),
                                static_cast<uint64_t>(n.ex_time(ex)), 0);
                        n.ex_advance(ex);
                    }
                }
            }
        }
        BufferInfo info{};
        const int64_t o = n.dq_out(codec, &info, 0);
        if (o == kFormatChanged) {
            if (!seen_buffer) d.format_first = true;
            read_output_format(n, codec, &d);
        } else if (o >= 0) {
            seen_buffer = true;
            take_output(n, codec, o, info, video, &d);
        }
    }
    n.stop(codec);
    n.codec_delete(codec);
    if (config != format) n.format_delete(config);
    n.format_delete(format);
    n.ex_delete(ex);
    return d;
}

// Red over blue: the top-left and bottom-left luma and the chroma of each half (BT.601 limited:
// red 81,90,240, blue 41,240,110), within a coder's tolerance.
bool red_over_blue(const Decoded& d, std::string* detail) {
    if (d.first_picture.empty() || d.stride <= 0 || d.slice_height <= 0) return false;
    const uint8_t* y = d.first_picture.data();
    const uint8_t* cb = y + d.stride * d.slice_height;
    const uint8_t* cr = cb + (d.stride / 2) * (d.slice_height / 2);
    auto close_to = [](int v, int want) { return std::abs(v - want) <= 12; };
    const int top = y[d.stride * 4 + 4], bottom = y[d.stride * (d.height - 4) + 4];
    const int cb_top = cb[(d.stride / 2) * 2 + 2], cr_top = cr[(d.stride / 2) * 2 + 2];
    const int cb_bottom = cb[(d.stride / 2) * (d.height / 2 - 2) + 2], cr_bottom = cr[(d.stride / 2) * (d.height / 2 - 2) + 2];
    char buf[160];
    std::snprintf(buf, sizeof buf, "Y %d/%d, Cb %d/%d, Cr %d/%d", top, bottom, cb_top, cb_bottom, cr_top, cr_bottom);
    *detail = buf;
    return close_to(top, 81) && close_to(bottom, 41) && close_to(cb_top, 90) && close_to(cr_top, 240) && close_to(cb_bottom, 240) &&
           close_to(cr_bottom, 110);
}

bool ascending(const std::vector<int64_t>& v) {
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] <= v[i - 1]) return false;
    return true;
}

void check_video(const char* what, const Decoded& d) {
    std::string detail;
    check((std::string(what) + ": output format first").c_str(), d.format_first);
    check((std::string(what) + ": 64x48 I420").c_str(), d.width == 64 && d.height == 48 && d.color_format == 19,
          std::to_string(d.width) + "x" + std::to_string(d.height) + " color-format " + std::to_string(d.color_format));
    check((std::string(what) + ": 25 pictures in order").c_str(), d.buffers == 25 && ascending(d.pts),
          std::to_string(d.buffers));
    check((std::string(what) + ": red over blue").c_str(), red_over_blue(d, &detail), detail);
    check((std::string(what) + ": end of stream").c_str(), d.eos);
}

// The tone's level: its RMS against full scale; a 440 Hz sine at any reasonable level is well above 0.01.
double rms(const std::vector<int16_t>& pcm) {
    double sum = 0;
    for (int16_t s : pcm) sum += static_cast<double>(s) * s;
    return pcm.empty() ? 0 : std::sqrt(sum / pcm.size()) / 32768.0;
}

void check_audio(const char* what, const Decoded& d, int rate) {
    check((std::string(what) + ": output format first").c_str(), d.format_first);
    check((std::string(what) + ": mono at the stream's rate").c_str(), d.channels == 1 && d.rate == rate,
          std::to_string(d.rate) + " Hz, " + std::to_string(d.channels) + " channel(s)");
    const double seconds = d.rate > 0 ? static_cast<double>(d.pcm.size()) / d.rate : 0;
    check((std::string(what) + ": about a second of the tone").c_str(),
          seconds > 0.8 && seconds < 1.3 && rms(d.pcm) > 0.01,
          std::to_string(seconds) + " s, rms " + std::to_string(rms(d.pcm)));
    check((std::string(what) + ": end of stream").c_str(), d.eos);
}

// The extractor on its own: both tracks, every video sample with a key frame first, a seek.
void extractor_checks(const Ndk& n, const std::string& path, const char* video_mime, const char* audio_mime) {
    void* ex = n.ex_new();
    check("extractor: opens", n.ex_source(ex, path.c_str()) == 0, path);
    check("extractor: two tracks", n.ex_tracks(ex) == 2, std::to_string(n.ex_tracks(ex)));
    const int v = track_of(n, ex, "video/"), a = track_of(n, ex, "audio/");
    void* vf = v >= 0 ? n.ex_track_format(ex, static_cast<size_t>(v)) : nullptr;
    void* af = a >= 0 ? n.ex_track_format(ex, static_cast<size_t>(a)) : nullptr;
    int32_t w = 0, h = 0, rate = 0, channels = 0;
    int64_t duration = 0;
    void* data = nullptr;
    size_t size = 0;
    check("extractor: video track", vf && mime_of(n, vf) == video_mime && n.get_int32(vf, "width", &w) && w == 64 &&
                                        n.get_int32(vf, "height", &h) && h == 48,
          vf ? mime_of(n, vf) : "none");
    check("extractor: audio track", af && mime_of(n, af) == audio_mime && n.get_int32(af, "sample-rate", &rate) &&
                                        n.get_int32(af, "channel-count", &channels) && channels == 1,
          af ? mime_of(n, af) + " " + std::to_string(rate) : "none");
    check("extractor: duration", vf && n.get_int64(vf, "durationUs", &duration) && duration > 900000 && duration < 1200000,
          std::to_string(duration));
    if (std::string(video_mime) == "video/avc")
        check("extractor: H.264 codec data", vf && n.get_buffer(vf, "csd-0", &data, &size) && size > 4);
    if (vf) n.format_delete(vf);
    if (af) n.format_delete(af);

    n.ex_select(ex, static_cast<size_t>(v));
    std::vector<uint8_t> buf(1 << 20);
    int samples = 0;
    bool only_video = true, first_key = false;
    std::vector<int64_t> times;
    do {
        const int64_t got = n.ex_read(ex, buf.data(), buf.size());
        if (got < 0) break;
        if (samples == 0) first_key = (n.ex_flags(ex) & 1) != 0;
        only_video = only_video && n.ex_track(ex) == v;
        times.push_back(n.ex_time(ex));
        ++samples;
    } while (n.ex_advance(ex));
    check("extractor: 25 video samples, a key frame first", samples == 25 && only_video && first_key,
          std::to_string(samples));
    check("extractor: at the end", n.ex_read(ex, buf.data(), buf.size()) < 0 && n.ex_time(ex) == -1 &&
                                       n.ex_track(ex) == -1);
    check("extractor: seek to the start", n.ex_seek(ex, 0, 0) == 0 && n.ex_read(ex, buf.data(), buf.size()) > 0 &&
                                              n.ex_time(ex) == (times.empty() ? -2 : times[0]));
    n.ex_select(ex, static_cast<size_t>(a));
    n.ex_unselect(ex, static_cast<size_t>(v));
    n.ex_seek(ex, 0, 0);
    int audio = 0;
    do {
        if (n.ex_read(ex, buf.data(), buf.size()) < 0) break;
        audio += n.ex_track(ex) == a;
    } while (n.ex_advance(ex));
    check("extractor: audio samples alone", audio > 10, std::to_string(audio));
    n.ex_delete(ex);
}

// The same file through a descriptor's range (an asset in an APK: the file at an offset in a
// bigger one) and through a data source of the app's.
struct Memory {
    std::vector<uint8_t> bytes;
};
GUEST int64_t memory_read(void* user, int64_t offset, void* buffer, size_t size) {
    auto* m = static_cast<Memory*>(user);
    if (offset >= static_cast<int64_t>(m->bytes.size())) return 0;
    const size_t n = std::min<size_t>(size, m->bytes.size() - static_cast<size_t>(offset));
    std::memcpy(buffer, m->bytes.data() + offset, n);
    return static_cast<int64_t>(n);
}
GUEST int64_t memory_size(void* user) { return static_cast<int64_t>(static_cast<Memory*>(user)->bytes.size()); }

int samples_of(const Ndk& n, void* ex) {
    if (n.ex_tracks(ex) != 2) return -1;
    n.ex_select(ex, 0);
    n.ex_select(ex, 1);
    std::vector<uint8_t> buf(1 << 20);
    int count = 0;
    do {
        if (n.ex_read(ex, buf.data(), buf.size()) < 0) break;
        ++count;
    } while (n.ex_advance(ex));
    return count;
}

void source_checks(const Ndk& n, const std::string& path) {
    Memory m;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        uint8_t b[4096];
        size_t got;
        while ((got = std::fread(b, 1, sizeof b, f)) > 0) m.bytes.insert(m.bytes.end(), b, b + got);
        std::fclose(f);
    }
    void* ex = n.ex_new();
    n.ex_source(ex, path.c_str());
    const int want = samples_of(n, ex);
    n.ex_delete(ex);

    // 1000 bytes of something else, then the file, then 500 more.
    char dir[MAX_PATH], tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    GetTempFileNameA(dir, "ndk", 0, tmp);
    if (FILE* f = std::fopen(tmp, "wb")) {
        std::vector<uint8_t> pad(1000, 0x5a);
        std::fwrite(pad.data(), 1, pad.size(), f);
        std::fwrite(m.bytes.data(), 1, m.bytes.size(), f);
        std::fwrite(pad.data(), 1, 500, f);
        std::fclose(f);
    }
    const int fd = _open(tmp, _O_RDONLY | _O_BINARY);
    ex = n.ex_new();
    const int status = n.ex_source_fd(ex, fd, 1000, static_cast<int64_t>(m.bytes.size()));
    const int got = samples_of(n, ex);
    check("extractor: a descriptor's range", status == 0 && got == want && want > 0,
          std::to_string(got) + " of " + std::to_string(want) + " samples");
    n.ex_delete(ex);
    _close(fd);
    DeleteFileA(tmp);

    void* ds = n.ds_new();
    n.ds_userdata(ds, &m);
    n.ds_read_at(ds, memory_read);
    n.ds_size(ds, memory_size);
    ex = n.ex_new();
    const int custom = n.ex_source_custom(ex, ds);
    const int got_custom = samples_of(n, ex);
    check("extractor: an app's data source", custom == 0 && got_custom == want,
          std::to_string(got_custom) + " of " + std::to_string(want) + " samples");
    n.ex_delete(ex);
    n.ds_delete(ds);
}

// The asynchronous mode: the app feeds input from onAsyncInputAvailable and takes output from
// onAsyncOutputAvailable, on the codec's thread.
struct AsyncRun {
    const Ndk* n = nullptr;
    void* ex = nullptr;
    void* codec = nullptr;
    std::mutex lock;
    std::condition_variable cv;
    bool done = false, input_done = false, format_first = false, seen_buffer = false;
    int buffers = 0;
    std::vector<int64_t> pts;
    bool other_thread = true;
    std::thread::id caller;
};
GUEST void on_input(void* codec, void* user, int32_t index) {
    auto* r = static_cast<AsyncRun*>(user);
    const Ndk& n = *r->n;
    if (std::this_thread::get_id() == r->caller) r->other_thread = false;
    if (r->input_done) return;
    size_t cap = 0;
    uint8_t* buf = n.in_buf(codec, static_cast<size_t>(index), &cap);
    const int64_t size = n.ex_read(r->ex, buf, cap);
    if (size < 0) {
        r->input_done = true;
        n.queue(codec, static_cast<size_t>(index), 0, 0, 0, kEndOfStream);
        return;
    }
    const int64_t t = n.ex_time(r->ex);
    n.ex_advance(r->ex);
    n.queue(codec, static_cast<size_t>(index), 0, static_cast<size_t>(size), static_cast<uint64_t>(t), 0);
}
GUEST void on_output(void* codec, void* user, int32_t index, BufferInfo* info) {
    auto* r = static_cast<AsyncRun*>(user);
    if (info->size > 0) {
        r->seen_buffer = true;
        ++r->buffers;
        r->pts.push_back(info->presentationTimeUs);
    }
    const bool eos = (info->flags & kEndOfStream) != 0;
    r->n->release(codec, static_cast<size_t>(index), false);
    if (eos) {
        std::lock_guard<std::mutex> hold(r->lock);
        r->done = true;
        r->cv.notify_all();
    }
}
GUEST void on_format(void*, void* user, void* format) {
    auto* r = static_cast<AsyncRun*>(user);
    if (!r->seen_buffer) r->format_first = true;
    r->n->format_delete(format);
}
GUEST void on_error(void*, void* user, int, int32_t, const char*) {
    auto* r = static_cast<AsyncRun*>(user);
    std::lock_guard<std::mutex> hold(r->lock);
    r->done = true;
    r->cv.notify_all();
}

void async_checks(const Ndk& n, const std::string& path) {
    AsyncRun r;
    r.n = &n;
    r.caller = std::this_thread::get_id();
    r.ex = n.ex_new();
    n.ex_source(r.ex, path.c_str());
    const int track = track_of(n, r.ex, "video/");
    void* format = n.ex_track_format(r.ex, static_cast<size_t>(track));
    n.ex_select(r.ex, static_cast<size_t>(track));
    r.codec = n.by_type(mime_of(n, format).c_str());
    const Callbacks cb{on_input, on_output, on_format, on_error};
    const bool set = r.codec && n.async(r.codec, cb, &r) == 0;
    const bool configured = set && n.configure(r.codec, format, nullptr, nullptr, 0) == 0;
    const bool started = configured && n.start(r.codec) == 0;
    check("async: set up", started);
    if (started) {
        BufferInfo info{};
        check("async: dequeue refused", n.dq_in(r.codec, 0) < 0 && n.dq_out(r.codec, &info, 0) < kFormatChanged - 1);
        std::unique_lock<std::mutex> hold(r.lock);
        r.cv.wait_for(hold, std::chrono::seconds(20), [&] { return r.done; });
    }
    if (r.codec) {
        n.stop(r.codec);
        n.codec_delete(r.codec);
    }
    check("async: 25 pictures in order, format first, on the codec's thread",
          r.buffers == 25 && ascending(r.pts) && r.format_first && r.other_thread, std::to_string(r.buffers));
    n.format_delete(format);
    n.ex_delete(r.ex);
}

// A surface: no memory in the output buffers, and a released picture with render is shown.
void surface_checks(const Ndk& n, const std::string& path) {
    void* ex = n.ex_new();
    n.ex_source(ex, path.c_str());
    const int track = track_of(n, ex, "video/");
    void* format = n.ex_track_format(ex, static_cast<size_t>(track));
    n.ex_select(ex, static_cast<size_t>(track));
    void* codec = n.by_type(mime_of(n, format).c_str());
    const bool ok = codec && n.configure(codec, format, winapk::host::native_window_handle(), nullptr, 0) == 0 &&
                    n.start(codec) == 0;
    int shown_before = winapk::host::g_frames_shown, buffers = 0;
    bool null_memory = true, eos = false, input_done = false;
    for (int spins = 0; ok && spins < 10000 && !eos; ++spins) {
        if (!input_done) {
            const int64_t i = n.dq_in(codec, 0);
            if (i >= 0) {
                size_t cap = 0;
                uint8_t* buf = n.in_buf(codec, static_cast<size_t>(i), &cap);
                const int64_t size = n.ex_read(ex, buf, cap);
                if (size < 0) {
                    n.queue(codec, static_cast<size_t>(i), 0, 0, 0, kEndOfStream);
                    input_done = true;
                } else {
                    n.queue(codec, static_cast<size_t>(i), 0, static_cast<size_t>(size),
                            static_cast<uint64_t>(n.ex_time(ex)), 0);
                    n.ex_advance(ex);
                }
            }
        }
        BufferInfo info{};
        const int64_t o = n.dq_out(codec, &info, 0);
        if (o >= 0) {
            size_t cap = 0;
            null_memory = null_memory && n.out_buf(codec, static_cast<size_t>(o), &cap) == nullptr;
            if (info.flags & kEndOfStream) eos = true;
            if (info.size > 0) ++buffers;
            n.release(codec, static_cast<size_t>(o), true);
        }
    }
    check("surface: 25 pictures shown, no buffer memory",
          ok && buffers == 25 && winapk::host::g_frames_shown - shown_before == 25 && null_memory,
          std::to_string(winapk::host::g_frames_shown - shown_before) + " shown");
    if (codec) n.codec_delete(codec);
    n.format_delete(format);
    n.ex_delete(ex);
}


// ---- the encoders and the muxer --------------------------------------------------------------

std::string temp_path(const char* prefix) {
    char dir[MAX_PATH], tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    GetTempFileNameA(dir, prefix, 0, tmp);
    return tmp;
}

// What an encoder gave: its output formats and buffers in order.
struct Encoded {
    bool opened = false, format_first = false, config_second = false, eos = false;
    void* format = nullptr;  // the announced output format (the app deletes it)
    std::vector<std::vector<uint8_t>> data;
    std::vector<BufferInfo> info;
};

// Feeds `inputs` (each with its time) to a configured, started encoder and drains it.
void run_encoder(const Ndk& n, void* codec, const std::vector<std::vector<uint8_t>>& inputs,
                 const std::vector<int64_t>& times, int request_sync_at, Encoded* out) {
    size_t next = 0;
    bool input_done = false;
    int outputs = 0;
    for (int spins = 0; spins < 20000 && !out->eos; ++spins) {
        if (!input_done) {
            const int64_t i = n.dq_in(codec, 0);
            if (i >= 0) {
                size_t cap = 0;
                uint8_t* buf = n.in_buf(codec, static_cast<size_t>(i), &cap);
                if (next == inputs.size()) {
                    n.queue(codec, static_cast<size_t>(i), 0, 0, 0, kEndOfStream);
                    input_done = true;
                } else {
                    if (static_cast<int>(next) == request_sync_at) {
                        void* p = n.format_new();
                        n.set_int32(p, "request-sync", 0);
                        n.set_params(codec, p);
                        n.format_delete(p);
                    }
                    const std::vector<uint8_t>& in = inputs[next];
                    const size_t size = std::min(cap, in.size());
                    std::memcpy(buf, in.data(), size);
                    n.queue(codec, static_cast<size_t>(i), 0, size, static_cast<uint64_t>(times[next]), 0);
                    ++next;
                }
            }
        }
        BufferInfo info{};
        const int64_t o = n.dq_out(codec, &info, 0);
        if (o == kFormatChanged) {
            if (outputs == 0) out->format_first = true;
            if (out->format) n.format_delete(out->format);
            out->format = n.out_format(codec);
        } else if (o >= 0) {
            if (outputs == 0) out->config_second = (info.flags & kCodecConfig) != 0;
            ++outputs;
            if (info.flags & kEndOfStream) out->eos = true;
            size_t cap = 0;
            const uint8_t* p = n.out_buf(codec, static_cast<size_t>(o), &cap);
            if (info.size > 0 && p) {
                out->data.emplace_back(p + info.offset, p + info.offset + info.size);
                out->info.push_back(info);
            }
            n.release(codec, static_cast<size_t>(o), false);
        }
    }
}

// An I420 picture, red over blue (BT.601 limited, as red_over_blue reads it back).
std::vector<uint8_t> red_over_blue_i420(int w, int h) {
    std::vector<uint8_t> pic(static_cast<size_t>(w) * h * 3 / 2);
    uint8_t* y = pic.data();
    uint8_t* u = y + w * h;
    uint8_t* v = u + (w / 2) * (h / 2);
    for (int r = 0; r < h; ++r) std::memset(y + r * w, r < h / 2 ? 81 : 41, static_cast<size_t>(w));
    for (int r = 0; r < h / 2; ++r) {
        std::memset(u + r * (w / 2), r < h / 4 ? 90 : 240, static_cast<size_t>(w / 2));
        std::memset(v + r * (w / 2), r < h / 4 ? 240 : 110, static_cast<size_t>(w / 2));
    }
    return pic;
}

Encoded encode_video(const Ndk& n, int w, int h, int frames) {
    Encoded e;
    void* codec = n.encoder("video/avc");
    if (!codec) return e;
    void* f = n.format_new();
    n.set_string(f, "mime", "video/avc");
    n.set_int32(f, "width", w);
    n.set_int32(f, "height", h);
    n.set_int32(f, "color-format", 19);  // COLOR_FormatYUV420Planar
    n.set_int32(f, "bitrate", 500000);
    n.set_int32(f, "frame-rate", 25);
    n.set_int32(f, "i-frame-interval", 10);
    check("H.264 encoder: a decoder's configure refused",
          n.configure(codec, f, nullptr, nullptr, 0) != 0);
    e.opened = n.configure(codec, f, nullptr, nullptr, 1 /* CONFIGURE_FLAG_ENCODE */) == 0 && n.start(codec) == 0;
    n.format_delete(f);
    if (e.opened) {
        const std::vector<uint8_t> pic = red_over_blue_i420(w, h);
        std::vector<std::vector<uint8_t>> inputs(static_cast<size_t>(frames), pic);
        std::vector<int64_t> times;
        for (int i = 0; i < frames; ++i) times.push_back(i * 40000ll);
        run_encoder(n, codec, inputs, times, 20, &e);
        n.stop(codec);
    }
    n.codec_delete(codec);
    return e;
}

Encoded encode_audio(const Ndk& n, int rate) {
    Encoded e;
    void* codec = n.encoder("audio/mp4a-latm");
    if (!codec) return e;
    void* f = n.format_new();
    n.set_string(f, "mime", "audio/mp4a-latm");
    n.set_int32(f, "sample-rate", rate);
    n.set_int32(f, "channel-count", 1);
    n.set_int32(f, "bitrate", 96000);
    e.opened = n.configure(codec, f, nullptr, nullptr, 1) == 0 && n.start(codec) == 0;
    n.format_delete(f);
    if (e.opened) {
        std::vector<std::vector<uint8_t>> inputs;
        std::vector<int64_t> times;
        for (int chunk = 0; chunk < 10; ++chunk) {  // a second, in tenths
            std::vector<uint8_t> pcm(static_cast<size_t>(rate / 10) * 2);
            for (int i = 0; i < rate / 10; ++i) {
                const auto s = static_cast<int16_t>(12000 * std::sin(2 * 3.14159265358979 * 440 * (chunk * (rate / 10) + i) / rate));
                pcm[static_cast<size_t>(i) * 2] = static_cast<uint8_t>(s & 0xff);
                pcm[static_cast<size_t>(i) * 2 + 1] = static_cast<uint8_t>((s >> 8) & 0xff);
            }
            inputs.push_back(std::move(pcm));
            times.push_back(chunk * 100000ll);
        }
        run_encoder(n, codec, inputs, times, -1, &e);
        n.stop(codec);
    }
    n.codec_delete(codec);
    return e;
}

bool has_buffer(const Ndk& n, void* format, const char* key) {
    void* data = nullptr;
    size_t size = 0;
    return format && n.get_buffer(format, key, &data, &size) && size > 0;
}

void encoder_checks(const Ndk& n) {
    std::printf("the encoders and AMediaMuxer: H.264 and AAC into an MP4\n");
    void* probe = n.encoder("video/avc");
    if (!probe) {
        std::printf("  [SKIP] no H.264 encoder on this Windows (Media Foundation)\n");
        return;
    }
    char* name = nullptr;
    check("named as MediaCodecList names it", n.get_name(probe, &name) == 0 && name &&
                                                  std::string(name) == "OMX.macapk.enc.video/avc",
          name ? name : "none");
    if (name) n.release_name(probe, name);
    n.codec_delete(probe);
    void* by_name = n.by_name("OMX.macapk.enc.video/avc");
    check("an encoder by MediaCodecList's name", by_name != nullptr);
    if (by_name) n.codec_delete(by_name);

    const int w = 320, h = 240, frames = 30;
    Encoded v = encode_video(n, w, h, frames);
    check("H.264: configured with FLAG_ENCODE and started", v.opened);
    check("H.264: the output format first, with the SPS and the PPS",
          v.format_first && has_buffer(n, v.format, "csd-0") && has_buffer(n, v.format, "csd-1"));
    check("H.264: then the codec data as a CODEC_CONFIG buffer", v.config_second);
    int pictures = 0, keys = 0;
    bool first_key = false, key_on_request = false;
    for (size_t i = 0; i < v.info.size(); ++i) {
        if (v.info[i].flags & kCodecConfig) continue;
        if (pictures == 0) first_key = (v.info[i].flags & kKeyFrame) != 0;
        if (v.info[i].presentationTimeUs == 20 * 40000) key_on_request = (v.info[i].flags & kKeyFrame) != 0;
        keys += (v.info[i].flags & kKeyFrame) ? 1 : 0;
        ++pictures;
    }
    check("H.264: 30 access units, a key frame first", pictures == frames && first_key, std::to_string(pictures));
    check("H.264: a key frame on request-sync", key_on_request, std::to_string(keys) + " key frames");
    check("H.264: end of stream", v.eos);

    const int rate = 44100;
    Encoded a = encode_audio(n, rate);
    if (!a.opened) std::printf("  [SKIP] no AAC encoder on this Windows (Media Foundation)\n");
    else {
        check("AAC: the output format first, with the AudioSpecificConfig", a.format_first && has_buffer(n, a.format, "csd-0"));
        check("AAC: about 43 frames and the end of stream", a.info.size() >= 40 && a.info.size() <= 48 && a.eos,
              std::to_string(a.info.size()));
    }

    // The app's own descriptor, as MediaMuxer's FileDescriptor; the muxer writes through a duplicate.
    const std::string mp4 = temp_path("mux");
    const int fd = _open(mp4.c_str(), _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY, 0644);
    void* m = n.mux_new(fd, kMuxMpeg4);
    _close(fd);
    const ssize_t vt = m && v.format ? n.mux_add(m, v.format) : -1;
    const ssize_t at = m && a.format ? n.mux_add(m, a.format) : -1;
    bool wrote = m && vt == 0 && (!a.opened || at == 1) && n.mux_start(m) == 0;
    for (size_t i = 0; wrote && i < v.data.size(); ++i)
        wrote = n.mux_write(m, 0, v.data[i].data(), &v.info[i]) == 0;
    for (size_t i = 0; wrote && a.opened && i < a.data.size(); ++i)
        wrote = n.mux_write(m, 1, a.data[i].data(), &a.info[i]) == 0;
    wrote = wrote && n.mux_stop(m) == 0;
    if (m) n.mux_delete(m);
    check("AMediaMuxer writes both into an MP4", wrote);

    Decoded back = decode_sync(n, mp4, "video/", false);
    std::string detail;
    check("the MP4's H.264 decodes: 30 pictures, red over blue",
          back.opened && back.buffers == frames && back.width == w && back.height == h && red_over_blue(back, &detail) &&
              back.eos,
          std::to_string(back.buffers) + ", " + detail);
    if (a.opened) {
        const Decoded tone = decode_sync(n, mp4, "audio/", false);
        if (tone.opened) check_audio("the MP4's AAC", tone, rate);
    }
    if (v.format) n.format_delete(v.format);
    if (a.format) n.format_delete(a.format);
    DeleteFileA(mp4.c_str());
}

// color.webm's tracks, copied by AMediaExtractor into a new WebM by AMediaMuxer.
void remux_checks(const Ndk& n, const std::string& webm) {
    std::printf("AMediaExtractor into AMediaMuxer: a WebM copied\n");
    const std::string out = temp_path("mkv");
    void* ex = n.ex_new();
    n.ex_source(ex, webm.c_str());
    const int fd = _open(out.c_str(), _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY, 0644);
    void* m = n.mux_new(fd, kMuxWebm);
    _close(fd);
    bool ok = m != nullptr && n.ex_tracks(ex) == 2;
    for (size_t i = 0; ok && i < n.ex_tracks(ex); ++i) {
        void* f = n.ex_track_format(ex, i);
        ok = n.mux_add(m, f) == static_cast<ssize_t>(i);
        n.format_delete(f);
        n.ex_select(ex, i);
    }
    ok = ok && n.mux_start(m) == 0;
    std::vector<uint8_t> buf(1 << 20);
    int samples = 0;
    while (ok) {
        const int64_t size = n.ex_read(ex, buf.data(), buf.size());
        if (size < 0) break;
        const BufferInfo info{0, static_cast<int32_t>(size), n.ex_time(ex), (n.ex_flags(ex) & 1) ? kKeyFrame : 0u};
        ok = n.mux_write(m, static_cast<size_t>(n.ex_track(ex)), buf.data(), &info) == 0;
        ++samples;
        n.ex_advance(ex);
    }
    ok = ok && n.mux_stop(m) == 0;
    if (m) n.mux_delete(m);
    n.ex_delete(ex);
    check("every sample written", ok && samples > 25, std::to_string(samples));
    check_video("the copy's VP9", decode_sync(n, out, "video/", false));
    check_audio("the copy's Opus", decode_sync(n, out, "audio/", false), 48000);
    DeleteFileA(out.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: winapk-ndk-media-test <dir with color.mp4, color.webm>\n");
        return 2;
    }
    const std::string dir = argv[1];
    const std::string mp4 = dir + "\\color.mp4", webm = dir + "\\color.webm";
    Ndk n;

    std::printf("names\n");
    void* c = n.by_type("video/x-vnd.on2.vp9");
    char* name = nullptr;
    check("a VP9 decoder (built in)", c && n.get_name(c, &name) == 0 && name &&
                                          std::string(name) == "OMX.macapk.dec.video/x-vnd.on2.vp9",
          name ? name : "none");
    if (name) n.release_name(c, name);
    if (c) n.codec_delete(c);
    void* byname = n.by_name("OMX.macapk.dec.audio/opus");
    check("by MediaCodecList's name", byname != nullptr);
    if (byname) n.codec_delete(byname);
    check("not by another device's name", n.by_name("c2.android.avc.decoder") == nullptr);
    check("no VP9 encoder", n.encoder("video/x-vnd.on2.vp9") == nullptr);
    check("not a decoder's name for an encoder", n.by_name("OMX.macapk.enc.video/x-vnd.on2.vp9") == nullptr);
    check("no decoder for an unknown type", n.by_type("video/x-unknown") == nullptr);

    std::printf("WebM: VP9 and Opus\n");
    extractor_checks(n, webm, "video/x-vnd.on2.vp9", "audio/opus");
    check_video("VP9", decode_sync(n, webm, "video/", false));
    check_audio("Opus", decode_sync(n, webm, "audio/", false), 48000);
    check_audio("Opus, codec data in band", decode_sync(n, webm, "audio/", true), 48000);
    source_checks(n, webm);
    async_checks(n, webm);
    surface_checks(n, webm);
    remux_checks(n, webm);

    std::printf("MP4: H.264 and AAC\n");
    void* avc = n.by_type("video/avc");
    if (!avc) {
        std::printf("  [SKIP] no H.264 decoder on this Windows (Media Foundation)\n");
    } else {
        n.codec_delete(avc);
        extractor_checks(n, mp4, "video/avc", "audio/mp4a-latm");
        check_video("H.264", decode_sync(n, mp4, "video/", false));
        check_video("H.264, codec data in band", decode_sync(n, mp4, "video/", true));
        const Decoded aac = decode_sync(n, mp4, "audio/", false);
        if (aac.opened) check_audio("AAC", aac, aac.track_rate);
        else std::printf("  [SKIP] no AAC decoder on this Windows (Media Foundation)\n");
        source_checks(n, mp4);
        async_checks(n, mp4);
        encoder_checks(n);
    }

    std::printf(g_failures ? "%d FAILED\n" : "ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
