// ndk_media_codec.cpp -- libmediandk's AMediaCodec (decoders and encoders), AMediaExtractor
// and AMediaDataSource for an app's native code, on what Java's MediaCodec and MediaExtractor
// use: video_decode.h's Decoder (Media Foundation's decoder MFTs, else the built-in VP8, VP9
// and AV1 decoders), AudioDecoder (AAC, MP3, Opus) and Demuxer (MP4 and the rest through Media
// Foundation, WebM and Matroska through mkvparser), media_encode.h's VideoEncoder (H.264) and
// AudioEncoder (AAC). No JVM is involved.
//
// The codec keeps NdkMediaCodec.h's contract:
//  - its name is MediaCodecList's ("OMX.macapk.dec.<mime>", "OMX.macapk.enc.<mime>"), so
//    createCodecByName takes what the Java list names and nothing else, as a device answers
//    only its own codecs' names;
//  - eight input buffers of KEY_MAX_INPUT_SIZE (else what Java's MediaCodec sizes), eight
//    output buffers; INFO_OUTPUT_FORMAT_CHANGED before the first output buffer and before any
//    buffer of a new size; an empty buffer with BUFFER_FLAG_END_OF_STREAM after the last;
//  - video comes out as I420 (COLOR_FormatYUV420Planar, 19), or P010 (54) when configure asks
//    for it, the layout the output format's stride, slice-height and crop keys describe, as
//    Android's software decoders give it; with a surface (the runtime's one native window) the
//    buffers have no memory (getOutputBuffer is null) and releaseOutputBuffer(render) shows the
//    picture on the window; audio comes out as 16-bit interleaved PCM;
//  - codec-specific data is the format's csd-0/csd-1, or the BUFFER_FLAG_CODEC_CONFIG buffers
//    queued before the first access unit;
//  - setAsyncNotifyCallback: the callbacks run on a thread of the codec's own, in order, with
//    no lock held, and dequeueInputBuffer / dequeueOutputBuffer are then refused.
// An encoder (createEncoderByType "video/avc" or "audio/mp4a-latm", configured with
// AMEDIACODEC_CONFIGURE_FLAG_ENCODE) keeps the same buffers and order, as Android's encoders give
// them: input buffers of one picture (KEY_COLOR_FORMAT 19, I420; 21 or 0x7f420888, NV12: the
// layouts Java's MediaCodec takes here) or of 16-bit PCM; INFO_OUTPUT_FORMAT_CHANGED with the
// codec data in csd-0 (and csd-1: H.264's SPS and PPS) first, then that codec data as a
// BUFFER_FLAG_CODEC_CONFIG buffer, then the frames, the key ones with BUFFER_FLAG_KEY_FRAME,
// then the empty end-of-stream buffer. setParameters takes "request-sync" and "video-bitrate".
// Coding is synchronous: queueInputBuffer decodes or encodes the access unit, so an output
// buffer is ready as soon as the codec gives one back; nothing is ever waited for, and a
// timeout passed to dequeue* only bounds a wait there is no need for.
//
// What differs from a device: the encoders are H.264 and AAC only, where Windows has Media
// Foundation's (createEncoderByType is null otherwise, as for a type a device has no encoder
// of); no input surface can be made for an encoder (configure refuses COLOR_FormatSurface);
// no secure decoder (queueSecureInputBuffer refuses) and no AMediaCrypto. An extractor reads
// a file, a URL, a descriptor or a data source; a descriptor's range and a data source are
// first copied to a temporary file the demuxer reads, removed with the extractor.

#include "native_media_format.h"

#include <windows.h>

#include <io.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "media_encode.h"
#include "native_window.h"
#include "video_decode.h"
#include "winapk/arm64_guest.h"
#include "winapk/guest_abi.h"
#include "winapk/mini_bionic.h"

namespace winapk::host {

namespace {

namespace vd = winapk::video;

// media_status_t (NdkMediaError.h).
constexpr int kOk = 0, kErrMalformed = -10001, kErrUnsupported = -10002,
              kErrInvalidObject = -10003, kErrInvalidParameter = -10004, kErrInvalidOperation = -10005,
              kErrEndOfStream = -10006, kErrIo = -10007;
// NdkMediaCodec.h.
constexpr int64_t kTryAgainLater = -1, kFormatChanged = -2;
constexpr uint32_t kFlagKeyFrame = 1, kFlagCodecConfig = 2, kFlagEndOfStream = 4;
constexpr uint32_t kConfigureEncode = 1;
// MediaCodecInfo.CodecCapabilities' color formats.
constexpr int kColorI420 = 19, kColorNv12 = 21, kColorP010 = 54, kColorSurface = 0x7f000789,
              kColorFlexible = 0x7f420888;
constexpr int kInBuffers = 8, kOutBuffers = 8, kInCapMin = 1 << 16, kInCapMax = 32 << 20;

struct BufferInfo {  // AMediaCodecBufferInfo
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
};

struct Codec;
struct AsyncCallbacks {  // AMediaCodecOnAsyncNotifyCallback
    void(WINAPK_GUEST_ABI* input)(Codec*, void* userdata, int32_t index);
    void(WINAPK_GUEST_ABI* output)(Codec*, void* userdata, int32_t index, BufferInfo* info);
    void(WINAPK_GUEST_ABI* format)(Codec*, void* userdata, MediaFormat* format);
    void(WINAPK_GUEST_ABI* error)(Codec*, void* userdata, int error, int32_t action, const char* detail);
};

// A guest function pointer as the host calls it: an arm64 app's through the translator's entry.
template <typename F>
F guest_fn(F fn) {
    return reinterpret_cast<F>(fn ? arm64::host_callable(reinterpret_cast<void*>(fn)) : nullptr);
}

// The audio decoders video_decode.h's AudioDecoder opens, by its format numbers.
int audio_format_of(const std::string& mime) {
    if (mime == "audio/mp4a-latm") return 1;
    if (mime == "audio/mpeg") return 2;
    if (mime == "audio/opus") return 3;
    return 0;
}
bool video_decodes(const std::string& mime) {
    const auto& mimes = vd::supported_mimes();
    return std::find(mimes.begin(), mimes.end(), mime) != mimes.end();
}
constexpr const char kNamePrefix[] = "OMX.macapk.dec.";  // MediaCodecList.java's
constexpr const char kEncoderPrefix[] = "OMX.macapk.enc.";

// Android's marked Opus csd (its OpusHeader.cpp), as Java's MediaCodec packs it for the decoder:
// "AOPUSHDR", "AOPUSDLY", "AOPUSPRL", each with an 8-byte little-endian length and its body.
std::vector<uint8_t> pack_opus(const std::vector<uint8_t>& head, const std::vector<uint8_t>& delay,
                               const std::vector<uint8_t>& preroll) {
    if (head.size() < 8 || std::memcmp(head.data(), "AOPUSHDR", 8) == 0) return head;
    std::vector<uint8_t> out;
    auto mark = [&](const char* tag, const std::vector<uint8_t>& body) {
        out.insert(out.end(), tag, tag + 8);
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(static_cast<uint64_t>(body.size()) >> (8 * i)));
        out.insert(out.end(), body.begin(), body.end());
    };
    mark("AOPUSHDR", head);
    if (delay.size() == 8) mark("AOPUSDLY", delay);
    if (preroll.size() == 8) mark("AOPUSPRL", preroll);
    return out;
}

// ---- AMediaCodec -----------------------------------------------------------------------------

struct Codec {
    std::mutex lock;
    std::string mime, name;
    bool video = false;
    int audio_format = 0;

    enum State { kCreated, kConfigured, kRunning } state = kCreated;
    int width = 0, height = 0, sample_rate = 0, channels = 0;
    vd::YuvLayout::Format yuv = vd::YuvLayout::kI420;
    const void* surface = nullptr;
    std::vector<uint8_t> csd0, csd1;  // the format's, or the CODEC_CONFIG buffers'
    bool csd_from_format = false;
    MediaFormat input_format;

    std::unique_ptr<vd::Decoder> vdec;
    std::unique_ptr<vd::AudioDecoder> adec;
    bool input_eos = false;

    // An encoder's.
    bool encoder = false;
    std::unique_ptr<vd::VideoEncoder> venc;
    std::unique_ptr<vd::AudioEncoder> aenc;
    vd::VideoEncoderConfig venc_config;
    vd::Pixels pixels = vd::Pixels::kNv12;
    int bitrate = 0;
    bool force_key = false;
    int64_t last_in_pts = INT64_MIN, last_out_pts = 0;
    bool format_announced = false;  // the output format with the codec data went out

    size_t in_capacity = kInCapMin;
    std::vector<std::vector<uint8_t>> in;
    std::deque<int> in_free;

    // A decoded output not yet in an output buffer.
    struct Pending {
        std::shared_ptr<vd::Frame> frame;
        std::vector<int16_t> pcm;
        int64_t pts = 0;
        uint32_t flags = 0;
        std::vector<uint8_t> coded;  // an encoder's output
    };
    std::deque<Pending> pending;
    struct Out {
        bool held = false;  // given to the app, not yet released
        std::vector<uint8_t> data;
        std::shared_ptr<vd::Frame> frame;  // surface mode
        BufferInfo info{};
    };
    std::vector<Out> out;
    // What dequeueOutputBuffer gives next, in order: an output buffer's index, or
    // kFormatChanged with the format it changes to.
    struct Ready {
        int64_t index;
        std::shared_ptr<MediaFormat> format;
    };
    std::deque<Ready> ready;
    std::shared_ptr<MediaFormat> output_format = std::make_shared<MediaFormat>();
    // The geometry the last announced output format has (video), or the rate and channels (audio).
    int out_a = -1, out_b = -1;

    // Async mode.
    AsyncCallbacks callbacks{};
    void* userdata = nullptr;
    bool async = false;
    struct Event {
        enum Kind { kInput, kOutput, kFormat, kError } kind;
        int32_t index = 0;
        BufferInfo info{};
        std::shared_ptr<MediaFormat> format;
        int error = 0;
    };
    std::deque<Event> events;
    std::condition_variable events_cv;
    std::thread events_thread;
    bool events_stop = false;
    int64_t generation = 0;  // a flush or a stop drops the events queued before it

    ~Codec() { stop_events(); }

    void stop_events() {
        {
            std::lock_guard<std::mutex> hold(lock);
            events_stop = true;
        }
        events_cv.notify_all();
        if (events_thread.joinable() && events_thread.get_id() != std::this_thread::get_id()) events_thread.join();
        if (events_thread.joinable()) events_thread.detach();
    }
};

void log_once(const char* what, const std::string& detail) {
    static std::mutex lock;
    static std::vector<std::string> said;
    const std::string key = std::string(what) + detail;
    std::lock_guard<std::mutex> hold(lock);
    if (std::find(said.begin(), said.end(), key) != said.end()) return;
    said.push_back(key);
    std::fprintf(stderr, "[win.apk] %s(\"%s\")\n", what, detail.c_str());
    std::fflush(stderr);
}

// Under the codec's lock: the events thread's queue.
void post(Codec* c, Codec::Event e) {
    if (!c->async) return;
    c->events.push_back(std::move(e));
    c->events_cv.notify_all();
}

void events_loop(Codec* c) {
    std::unique_lock<std::mutex> hold(c->lock);
    for (;;) {
        c->events_cv.wait(hold, [c] { return c->events_stop || !c->events.empty(); });
        if (c->events_stop) return;
        Codec::Event e = std::move(c->events.front());
        c->events.pop_front();
        const AsyncCallbacks cb = c->callbacks;
        void* user = c->userdata;
        hold.unlock();
        switch (e.kind) {
            case Codec::Event::kInput:
                if (cb.input) cb.input(c, user, e.index);
                break;
            case Codec::Event::kOutput:
                if (cb.output) cb.output(c, user, e.index, &e.info);
                break;
            case Codec::Event::kFormat:
                // A new format the callee owns, as AOSP's AMediaFormat_fromMsg gives it.
                if (cb.format) {
                    auto* f = new MediaFormat;
                    f->copy_from(*e.format);
                    cb.format(c, user, f);
                }
                break;
            case Codec::Event::kError:
                if (cb.error) cb.error(c, user, e.error, 0, "decoder error");
                break;
        }
        hold.lock();
    }
}

// The output format an output of this geometry has (NdkMediaFormat's keys, as Android's
// software decoders set them).
std::shared_ptr<MediaFormat> video_format(Codec* c, const vd::Frame& f) {
    auto fmt = std::make_shared<MediaFormat>();
    const vd::YuvLayout l = vd::YuvLayout::of(f, c->yuv);
    fmt->set_string("mime", "video/raw");
    fmt->set_int32("width", l.width);
    fmt->set_int32("height", l.height);
    fmt->set_int32("stride", l.stride);
    fmt->set_int32("slice-height", l.slice_height);
    fmt->set_int32("color-format", c->yuv == vd::YuvLayout::kP010 ? kColorP010 : kColorI420);
    fmt->set_int32("crop-left", 0);
    fmt->set_int32("crop-top", 0);
    fmt->set_int32("crop-right", l.width - 1);
    fmt->set_int32("crop-bottom", l.height - 1);
    const vd::ColorAspects color = vd::frame_color(f);
    if (color.standard) fmt->set_int32("color-standard", color.standard);
    if (color.range) fmt->set_int32("color-range", color.range);
    if (color.transfer) fmt->set_int32("color-transfer", color.transfer);
    if (!color.hdr_static_info.empty()) fmt->set_buffer("hdr-static-info", color.hdr_static_info);
    return fmt;
}
std::shared_ptr<MediaFormat> audio_format(int rate, int channels) {
    auto fmt = std::make_shared<MediaFormat>();
    fmt->set_string("mime", "audio/raw");
    fmt->set_int32("sample-rate", rate);
    fmt->set_int32("channel-count", channels);
    fmt->set_int32("pcm-encoding", 2);  // AudioFormat.ENCODING_PCM_16BIT
    return fmt;
}

// An encoder's output format: what it was configured with, and the codec data `config` holds
// (H.264's SPS and PPS, each behind its start code, as csd-0 and csd-1; AAC's
// AudioSpecificConfig as csd-0).
std::shared_ptr<MediaFormat> encoder_format(Codec* c, const std::vector<uint8_t>* config) {
    auto fmt = std::make_shared<MediaFormat>();
    fmt->set_string("mime", c->mime);
    if (c->video) {
        fmt->set_int32("width", c->width);
        fmt->set_int32("height", c->height);
        fmt->set_int32("frame-rate", std::max(1, static_cast<int>(c->venc_config.frame_rate + 0.5)));
    } else {
        fmt->set_int32("sample-rate", c->sample_rate);
        fmt->set_int32("channel-count", c->channels);
        fmt->set_int32("aac-profile", 2);  // MediaCodecInfo.CodecProfileLevel.AACObjectLC
    }
    if (c->bitrate > 0) fmt->set_int32("bitrate", c->bitrate);
    if (config && !config->empty()) {
        const std::vector<uint8_t>& b = *config;
        size_t second = 0;
        for (size_t i = 4; c->video && i + 4 <= b.size(); ++i) {
            if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 0 && b[i + 3] == 1) {
                second = i;
                break;
            }
        }
        if (second) {
            fmt->set_buffer("csd-0", std::vector<uint8_t>(b.begin(), b.begin() + static_cast<long>(second)));
            fmt->set_buffer("csd-1", std::vector<uint8_t>(b.begin() + static_cast<long>(second), b.end()));
        } else {
            fmt->set_buffer("csd-0", b);
        }
    }
    return fmt;
}

// Under the lock: moves decoded outputs into free output buffers, announcing a new format first.
void pump(Codec* c) {
    while (!c->pending.empty()) {
        int slot = -1;
        for (int i = 0; i < static_cast<int>(c->out.size()); ++i) {
            bool queued = false;
            for (const auto& r : c->ready) queued = queued || r.index == i;
            if (!c->out[i].held && !queued) {
                slot = i;
                break;
            }
        }
        if (slot < 0) return;  // the app holds every output buffer
        Codec::Pending p = std::move(c->pending.front());
        c->pending.pop_front();
        Codec::Out& o = c->out[slot];
        o.frame.reset();
        o.data.clear();
        o.info = BufferInfo{0, 0, p.pts, p.flags};
        if (c->encoder) {
            if (!c->format_announced && (!p.coded.empty() || (p.flags & kFlagEndOfStream))) {
                // Before the codec data, the format that carries it, as Android's encoders announce it.
                c->format_announced = true;
                auto fmt = encoder_format(c, (p.flags & kFlagCodecConfig) ? &p.coded : nullptr);
                c->ready.push_back({kFormatChanged, fmt});
                post(c, {Codec::Event::kFormat, 0, {}, fmt});
            }
            o.data = std::move(p.coded);
            o.info.size = static_cast<int32_t>(o.data.size());
        } else if (p.frame) {
            if (p.frame->width != c->out_a || p.frame->height != c->out_b) {
                c->out_a = p.frame->width, c->out_b = p.frame->height;
                auto fmt = video_format(c, *p.frame);
                c->ready.push_back({kFormatChanged, fmt});
                post(c, {Codec::Event::kFormat, 0, {}, fmt});
            }
            const vd::YuvLayout l = vd::YuvLayout::of(*p.frame, c->yuv);
            if (c->surface) {
                o.frame = p.frame;
            } else {
                o.data.resize(l.size());
                vd::frame_to_yuv(*p.frame, l, o.data.data());
            }
            o.info.size = static_cast<int32_t>(l.size());
        } else if (!p.pcm.empty()) {
            const int rate = c->adec ? c->adec->sample_rate() : c->sample_rate;
            const int channels = c->adec ? c->adec->channels() : c->channels;
            if (rate != c->out_a || channels != c->out_b) {
                c->out_a = rate, c->out_b = channels;
                auto fmt = audio_format(rate, channels);
                c->ready.push_back({kFormatChanged, fmt});
                post(c, {Codec::Event::kFormat, 0, {}, fmt});
            }
            o.data.resize(p.pcm.size() * sizeof(int16_t));
            std::memcpy(o.data.data(), p.pcm.data(), o.data.size());
            o.info.size = static_cast<int32_t>(o.data.size());
        }
        c->ready.push_back({slot, nullptr});
        post(c, {Codec::Event::kOutput, slot, o.info, nullptr});
    }
}

// In async mode the events carry the buffers: the ready list only keeps their order.
void hand_out_async(Codec* c) {
    if (!c->async) return;
    while (!c->ready.empty()) {
        Codec::Ready r = c->ready.front();
        c->ready.pop_front();
        if (r.index == kFormatChanged) c->output_format = r.format;
        else c->out[static_cast<size_t>(r.index)].held = true;
    }
}

bool open_decoder(Codec* c) {
    if (c->vdec || c->adec) return true;
    if (c->video) {
        c->vdec = vd::Decoder::open(c->mime, c->width, c->height, c->csd0, c->csd1);
        return c->vdec != nullptr;
    }
    c->adec = vd::AudioDecoder::open(c->audio_format, c->sample_rate, c->channels, c->csd0);
    return c->adec != nullptr;
}

// Whether the decoder for `mime` needs codec-specific data before its first access unit.
bool needs_csd(const std::string& mime) {
    return mime == "video/avc" || mime == "video/hevc" || mime == "audio/mp4a-latm" || mime == "audio/opus";
}

void reset_buffers(Codec* c) {
    c->in.assign(kInBuffers, std::vector<uint8_t>(c->in_capacity));
    c->in_free.clear();
    for (int i = 0; i < kInBuffers; ++i) c->in_free.push_back(i);
    c->out.assign(kOutBuffers, Codec::Out{});
    c->pending.clear();
    c->ready.clear();
    c->events.clear();
    c->input_eos = false;
}

Codec* create_decoder(const std::string& mime) {
    const int audio = audio_format_of(mime);
    if (!audio && !(mime.rfind("video/", 0) == 0 && video_decodes(mime))) {
        log_once("AMediaCodec: no decoder for", mime);
        return nullptr;
    }
    auto* c = new Codec;
    c->mime = mime;
    c->name = kNamePrefix + mime;
    c->video = audio == 0;
    c->audio_format = audio;
    return c;
}

// The encoders Java's MediaCodec has here, where Windows has them (media_encode.h).
Codec* create_encoder(const std::string& mime) {
    const bool video = mime == "video/avc";
    if (!(video ? vd::video_encoder_available() : mime == "audio/mp4a-latm" && vd::audio_encoder_available())) {
        log_once("AMediaCodec: no encoder for", mime);
        return nullptr;
    }
    auto* c = new Codec;
    c->mime = mime;
    c->name = kEncoderPrefix + mime;
    c->video = video;
    c->encoder = true;
    return c;
}

WINAPK_GUEST_ABI Codec* AMediaCodec_createDecoderByType(const char* mime) {
    return mime ? create_decoder(mime) : nullptr;
}
WINAPK_GUEST_ABI Codec* AMediaCodec_createEncoderByType(const char* mime) {
    return mime ? create_encoder(mime) : nullptr;
}
WINAPK_GUEST_ABI Codec* AMediaCodec_createCodecByName(const char* name) {
    if (name && std::strncmp(name, kNamePrefix, sizeof(kNamePrefix) - 1) == 0)
        return create_decoder(name + sizeof(kNamePrefix) - 1);
    if (name && std::strncmp(name, kEncoderPrefix, sizeof(kEncoderPrefix) - 1) == 0)
        return create_encoder(name + sizeof(kEncoderPrefix) - 1);
    log_once("AMediaCodec_createCodecByName: no codec of that name", name ? name : "");
    return nullptr;
}

// Under the lock: the encoder opened (again) from what configure stored.
bool open_encoder(Codec* c) {
    c->venc.reset();
    c->aenc.reset();
    if (c->video) c->venc = vd::VideoEncoder::open(c->venc_config);
    else c->aenc = vd::AudioEncoder::open(c->sample_rate, c->channels, c->bitrate);
    return c->venc || c->aenc;
}

// A number MediaFormat may hold as an int32 or as a float (KEY_FRAME_RATE, KEY_I_FRAME_INTERVAL).
double number_of(MediaFormat* f, const char* key, double otherwise) {
    int32_t v = 0;
    if (f->int32(key, &v)) return v;
    std::lock_guard<std::mutex> hold(f->lock);
    auto it = f->values.find(key);
    if (it != f->values.end() && (it->second.kind == FormatValue::kFloat || it->second.kind == FormatValue::kDouble))
        return it->second.d;
    return otherwise;
}

// configure for an encoder: what MediaCodec.java's configureEncoder takes, and the encoder opened.
int configure_encoder(Codec* c, MediaFormat* format) {
    int32_t v = 0;
    c->bitrate = format->int32("bitrate", &v) ? v : 0;
    if (c->video) {
        c->width = format->int32("width", &v) ? v : 0;
        c->height = format->int32("height", &v) ? v : 0;
        if (c->width < 16 || c->height < 16 || (c->width & 1) || (c->height & 1)) {
            log_once("AMediaCodec_configure: an H.264 encoder takes an even size of 16 or more, not",
                     std::to_string(c->width) + "x" + std::to_string(c->height));
            return kErrUnsupported;
        }
        const int color = format->int32("color-format", &v) ? v : 0;
        if (color == kColorSurface) {
            log_once("AMediaCodec_configure: no input surface for an encoder", c->mime);
            return kErrUnsupported;
        }
        if (color != kColorI420 && color != kColorNv12 && color != kColorFlexible) {
            log_once("AMediaCodec_configure: an encoder input color format not taken", std::to_string(color));
            return kErrUnsupported;
        }
        c->pixels = color == kColorI420 ? vd::Pixels::kI420 : vd::Pixels::kNv12;
        vd::VideoEncoderConfig& e = c->venc_config;
        e = vd::VideoEncoderConfig{};
        e.width = c->width;
        e.height = c->height;
        e.bitrate = c->bitrate;
        e.frame_rate = number_of(format, "frame-rate", 30);
        e.key_interval_s = number_of(format, "i-frame-interval", 1);
        e.profile = format->int32("profile", &v) ? v : 0;
        // Constrained baseline (0x10000) and constrained high (0x80000) are baseline and high to the encoder.
        if (e.profile == 0x10000) e.profile = 1;
        if (e.profile == 0x80000) e.profile = 8;
        e.bitrate_mode = format->int32("bitrate-mode", &v) ? v : 1;
        c->in_capacity = static_cast<size_t>(c->width) * c->height * 3 / 2;
    } else {
        c->sample_rate = format->int32("sample-rate", &v) ? v : 0;
        c->channels = format->int32("channel-count", &v) ? v : 0;
        if ((c->sample_rate != 44100 && c->sample_rate != 48000) || c->channels < 1 || c->channels > 2) {
            log_once("AMediaCodec_configure: the AAC encoder takes 44100 or 48000 Hz, mono or stereo, not",
                     std::to_string(c->sample_rate) + " Hz, " + std::to_string(c->channels) + " channel(s)");
            return kErrUnsupported;
        }
        c->in_capacity = kInCapMin;
    }
    if (format->int32("max-input-size", &v) && v > 0)
        c->in_capacity = std::max(c->in_capacity, static_cast<size_t>(std::min(v, kInCapMax)));
    if (!open_encoder(c)) {
        log_once("AMediaCodec_configure: the encoder refused the format", c->mime);
        return kErrUnsupported;
    }
    c->output_format = encoder_format(c, nullptr);
    return kOk;
}

// Under the lock: what the encoder has ready, pending in order.
void drain_encoder(Codec* c) {
    vd::Encoded e;
    while (c->venc ? c->venc->poll(&e) : c->aenc && c->aenc->poll(&e)) {
        uint32_t flags = 0;
        if (e.key) flags |= kFlagKeyFrame;
        if (e.config) flags |= kFlagCodecConfig;
        else c->last_out_pts = e.pts_us;
        Codec::Pending p;
        p.coded = std::move(e.data);
        p.pts = e.pts_us;
        p.flags = flags;
        c->pending.push_back(std::move(p));
        e = vd::Encoded{};
    }
}

// Under the lock: one input buffer into the encoder (or none, with eos).
int encode(Codec* c, const uint8_t* data, size_t size, int64_t pts, bool eos) {
    bool ok = true;
    if (size) {
        if (c->venc) {
            // Presentation times must rise: a producer's clock that repeats is nudged, as MediaCodec.java does.
            if (pts <= c->last_in_pts) pts = c->last_in_pts + 1;
            c->last_in_pts = pts;
            const bool key = c->force_key;
            c->force_key = false;
            ok = c->venc->encode(data, size, c->pixels, pts, key);
        } else if (c->aenc) {
            ok = c->aenc->encode(reinterpret_cast<const int16_t*>(data), size / 2, pts);
        }
    }
    if (eos) {
        if (c->venc) c->venc->finish();
        if (c->aenc) c->aenc->finish();
    }
    drain_encoder(c);
    if (eos) {
        Codec::Pending end;
        end.pts = c->last_out_pts;
        end.flags = kFlagEndOfStream;
        c->pending.push_back(std::move(end));
    }
    return ok ? kOk : kErrMalformed;
}
WINAPK_GUEST_ABI int AMediaCodec_delete(Codec* c) {
    if (!c) return kErrInvalidObject;
    c->stop_events();
    delete c;
    return kOk;
}
WINAPK_GUEST_ABI int AMediaCodec_getName(Codec* c, char** out) {
    if (!c || !out) return kErrInvalidParameter;
    *out = _strdup(c->name.c_str());
    return kOk;
}
WINAPK_GUEST_ABI void AMediaCodec_releaseName(Codec*, char* name) { std::free(name); }

WINAPK_GUEST_ABI int AMediaCodec_configure(Codec* c, MediaFormat* format, const void* surface, void* crypto,
                                           uint32_t flags) {
    if (!c) return kErrInvalidObject;
    // A decoder is not configured to encode, nor an encoder to decode.
    if (!format || ((flags & kConfigureEncode) != 0) != c->encoder) return kErrInvalidParameter;
    if (crypto) {
        log_once("AMediaCodec_configure: no secure decoder", c->mime);
        return kErrUnsupported;
    }
    std::lock_guard<std::mutex> hold(c->lock);
    if (c->state != Codec::kCreated) return kErrInvalidOperation;
    c->input_format.copy_from(*format);
    if (c->encoder) {
        if (surface) return kErrInvalidParameter;  // an encoder's output is buffers
        const int status = configure_encoder(c, format);
        if (status == kOk) c->state = Codec::kConfigured;
        return status;
    }
    int32_t v = 0;
    c->width = format->int32("width", &v) ? v : 0;
    c->height = format->int32("height", &v) ? v : 0;
    c->sample_rate = format->int32("sample-rate", &v) ? v : 0;
    c->channels = format->int32("channel-count", &v) ? v : 0;
    c->yuv = format->int32("color-format", &v) && v == kColorP010 ? vd::YuvLayout::kP010 : vd::YuvLayout::kI420;
    c->surface = surface && surface == native_window_handle() ? surface : nullptr;
    if (surface && !c->surface) log_once("AMediaCodec_configure: not the runtime's window; output to buffers", c->mime);
    c->csd0 = format->buffer("csd-0");
    c->csd1 = format->buffer("csd-1");
    if (c->audio_format == 3) c->csd0 = pack_opus(c->csd0, c->csd1, format->buffer("csd-2"));
    c->csd_from_format = !c->csd0.empty();
    // KEY_MAX_INPUT_SIZE, else Java MediaCodec's estimate: 3/4 of the picture's 16x16 blocks' pixels.
    int64_t cap = kInCapMin;
    if (format->int32("max-input-size", &v) && v > 0) cap = std::max<int64_t>(cap, v);
    else if (c->video && c->width > 0 && c->height > 0)
        cap = std::max<int64_t>(cap, int64_t{(c->width + 15) / 16} * ((c->height + 15) / 16) * 256 * 3 / 4);
    c->in_capacity = static_cast<size_t>(std::min<int64_t>(cap, kInCapMax));
    // Until the first output says otherwise: the configured picture or sound.
    if (c->video) {
        c->output_format = std::make_shared<MediaFormat>();
        c->output_format->set_string("mime", "video/raw");
        c->output_format->set_int32("width", c->width);
        c->output_format->set_int32("height", c->height);
        c->output_format->set_int32("color-format", c->yuv == vd::YuvLayout::kP010 ? kColorP010 : kColorI420);
    } else {
        c->output_format = audio_format(c->audio_format == 3 ? 48000 : c->sample_rate, c->channels);
    }
    // A decoder that has what it needs opens now, so that configure refuses a stream it cannot
    // take, as a device's does; one waiting for its CODEC_CONFIG buffers opens at the first
    // access unit.
    if (c->csd_from_format || !needs_csd(c->mime)) {
        if (!open_decoder(c)) {
            log_once("AMediaCodec_configure: the decoder refused the format", c->mime);
            return kErrUnsupported;
        }
    }
    c->state = Codec::kConfigured;
    return kOk;
}

WINAPK_GUEST_ABI int AMediaCodec_setAsyncNotifyCallback(Codec* c, AsyncCallbacks callbacks, void* userdata) {
    if (!c) return kErrInvalidObject;
    {
        std::lock_guard<std::mutex> hold(c->lock);
        if (c->state == Codec::kRunning) return kErrInvalidOperation;
        c->callbacks = {guest_fn(callbacks.input), guest_fn(callbacks.output), guest_fn(callbacks.format),
                        guest_fn(callbacks.error)};
        c->userdata = userdata;
        c->async = true;
        c->events_stop = false;
    }
    if (!c->events_thread.joinable()) c->events_thread = std::thread(events_loop, c);
    return kOk;
}

WINAPK_GUEST_ABI int AMediaCodec_start(Codec* c) {
    if (!c) return kErrInvalidObject;
    std::lock_guard<std::mutex> hold(c->lock);
    if (c->state == Codec::kCreated) return kErrInvalidOperation;
    if (c->state == Codec::kRunning) {
        // After a flush in async mode, start() resumes: every input buffer is offered again.
        if (c->async)
            for (int i : c->in_free) post(c, {Codec::Event::kInput, i});
        return kOk;
    }
    reset_buffers(c);
    c->out_a = c->out_b = -1;
    if (c->encoder && !c->venc && !c->aenc && !open_encoder(c)) return kErrUnsupported;
    c->format_announced = false;
    c->last_in_pts = INT64_MIN;
    c->last_out_pts = 0;
    c->force_key = false;
    c->state = Codec::kRunning;
    for (int i : c->in_free) post(c, {Codec::Event::kInput, i});
    return kOk;
}

WINAPK_GUEST_ABI int AMediaCodec_stop(Codec* c) {
    if (!c) return kErrInvalidObject;
    std::lock_guard<std::mutex> hold(c->lock);
    if (c->state != Codec::kRunning) return kOk;
    reset_buffers(c);
    if (c->vdec) c->vdec->flush();
    if (c->adec) c->adec->flush();
    // Stopped is uninitialized, as on a device: start() wants configure() first.
    c->state = Codec::kCreated;
    c->vdec.reset();
    c->adec.reset();
    c->venc.reset();
    c->aenc.reset();
    c->csd0.clear();
    c->csd1.clear();
    return kOk;
}

WINAPK_GUEST_ABI int AMediaCodec_flush(Codec* c) {
    if (!c) return kErrInvalidObject;
    std::lock_guard<std::mutex> hold(c->lock);
    if (c->state != Codec::kRunning) return kErrInvalidOperation;
    if (c->vdec) c->vdec->flush();
    if (c->adec) c->adec->flush();
    // An encoder starts over: what it holds is dropped, and its next frame is a key frame with
    // its codec data before it again.
    if (c->encoder && !open_encoder(c)) return kErrUnsupported;
    if (c->encoder) {
        c->format_announced = false;
        c->last_in_pts = INT64_MIN;
    }
    reset_buffers(c);
    c->out_a = c->out_b = -1;
    // Sync mode: every input buffer is free again. Async mode: none is offered until start().
    return kOk;
}

WINAPK_GUEST_ABI int64_t AMediaCodec_dequeueInputBuffer(Codec* c, int64_t) {
    if (!c) return kErrInvalidObject;
    std::lock_guard<std::mutex> hold(c->lock);
    if (c->state != Codec::kRunning || c->async) return kErrInvalidOperation;
    if (c->in_free.empty() || c->input_eos) return kTryAgainLater;
    const int i = c->in_free.front();
    c->in_free.pop_front();
    return i;
}

WINAPK_GUEST_ABI uint8_t* AMediaCodec_getInputBuffer(Codec* c, size_t index, size_t* size) {
    if (!c) return nullptr;
    std::lock_guard<std::mutex> hold(c->lock);
    if (c->state != Codec::kRunning || index >= c->in.size()) return nullptr;
    if (size) *size = c->in[index].size();
    return c->in[index].data();
}

// Under the lock: one access unit (or none, with eos) into the decoder, its outputs pending.
int decode(Codec* c, const uint8_t* data, size_t size, int64_t pts, bool eos) {
    if (!c->vdec && !c->adec && !open_decoder(c)) return kErrUnsupported;
    if (c->vdec) {
        if ((size || eos) && !c->vdec->decode(size ? data : nullptr, size, pts, eos)) {
            if (!eos) return kErrMalformed;
        }
        bool done = false;
        while (auto f = c->vdec->poll(&done)) c->pending.push_back({f, {}, f->pts_us, 0, {}});
        if (eos) {
            while (!done) {
                auto f = c->vdec->poll(&done);
                if (!f) break;
                c->pending.push_back({f, {}, f->pts_us, 0, {}});
            }
            c->pending.push_back({nullptr, {}, pts, kFlagEndOfStream, {}});
        }
    } else {
        std::vector<int16_t> pcm = c->adec->decode(size ? data : nullptr, size, eos);
        if (!pcm.empty()) c->pending.push_back({nullptr, std::move(pcm), pts, 0, {}});
        if (eos) c->pending.push_back({nullptr, {}, pts, kFlagEndOfStream, {}});
    }
    return kOk;
}

WINAPK_GUEST_ABI int AMediaCodec_queueInputBuffer(Codec* c, size_t index, int64_t offset, size_t size,
                                                  uint64_t pts, uint32_t flags) {
    if (!c) return kErrInvalidObject;
    std::lock_guard<std::mutex> hold(c->lock);
    if (c->state != Codec::kRunning) return kErrInvalidOperation;
    if (index >= c->in.size() || offset < 0 || static_cast<size_t>(offset) + size > c->in[index].size())
        return kErrInvalidParameter;
    if (c->input_eos) return kErrInvalidOperation;
    const uint8_t* data = c->in[index].data() + offset;
    const bool eos = (flags & kFlagEndOfStream) != 0;
    int status = kOk;
    if (c->encoder) {
        // Codec data has no meaning for an encoder's input: dropped.
        status = encode(c, data, (flags & kFlagCodecConfig) ? 0 : size, static_cast<int64_t>(pts), eos);
    } else if (flags & kFlagCodecConfig) {
        // Before the decoder opens these are its csd-0 and csd-1. After, they are dropped, as
        // Java's MediaCodec drops them: the decoder has its parameter sets, and new ones come in
        // band with the access units that use them.
        if (!c->vdec && !c->adec && size) {
            (c->csd0.empty() || c->csd_from_format ? c->csd0 : c->csd1).assign(data, data + size);
            c->csd_from_format = false;
            if (c->audio_format == 3) c->csd0 = pack_opus(c->csd0, {}, {});
        }
        if (eos) status = decode(c, nullptr, 0, static_cast<int64_t>(pts), true);
    } else {
        status = decode(c, data, size, static_cast<int64_t>(pts), eos);
    }
    if (eos) c->input_eos = true;
    if (status == kErrMalformed && c->encoder) {
        // A picture shorter than a whole one, usually: Android's encoders report it as an error.
        log_once("AMediaCodec: the encoder refused an input buffer", c->mime);
        post(c, {Codec::Event::kError, 0, {}, nullptr, status});
    } else if (status != kOk && status != kErrMalformed) {
        log_once("AMediaCodec: the decoder would not open", c->mime);
        post(c, {Codec::Event::kError, 0, {}, nullptr, status});
    }
    pump(c);
    hand_out_async(c);
    c->in_free.push_back(static_cast<int>(index));
    if (!c->input_eos) post(c, {Codec::Event::kInput, static_cast<int32_t>(index)});
    // A unit the decoder refused is dropped, as a device's decoder drops a corrupt one and goes on.
    return status == kErrMalformed && !c->encoder ? kOk : status;
}

WINAPK_GUEST_ABI int AMediaCodec_queueSecureInputBuffer(Codec* c, size_t, int64_t, void*, uint64_t, uint32_t) {
    return c ? kErrUnsupported : kErrInvalidObject;
}

WINAPK_GUEST_ABI int64_t AMediaCodec_dequeueOutputBuffer(Codec* c, BufferInfo* info, int64_t) {
    if (!c) return kErrInvalidObject;
    std::lock_guard<std::mutex> hold(c->lock);
    if (c->state != Codec::kRunning || c->async) return kErrInvalidOperation;
    pump(c);
    if (c->ready.empty()) return kTryAgainLater;
    Codec::Ready r = c->ready.front();
    c->ready.pop_front();
    if (r.index == kFormatChanged) {
        c->output_format = r.format;
        return kFormatChanged;
    }
    Codec::Out& o = c->out[static_cast<size_t>(r.index)];
    o.held = true;
    if (info) *info = o.info;
    return r.index;
}

WINAPK_GUEST_ABI uint8_t* AMediaCodec_getOutputBuffer(Codec* c, size_t index, size_t* size) {
    if (!c) return nullptr;
    std::lock_guard<std::mutex> hold(c->lock);
    if (c->state != Codec::kRunning || index >= c->out.size() || !c->out[index].held || c->surface) return nullptr;
    if (size) *size = c->out[index].data.size();
    return c->out[index].data.data();
}

WINAPK_GUEST_ABI MediaFormat* AMediaCodec_getOutputFormat(Codec* c) {
    if (!c) return nullptr;
    std::lock_guard<std::mutex> hold(c->lock);
    auto* f = new MediaFormat;
    f->copy_from(*c->output_format);
    return f;
}
WINAPK_GUEST_ABI MediaFormat* AMediaCodec_getInputFormat(Codec* c) {
    if (!c) return nullptr;
    std::lock_guard<std::mutex> hold(c->lock);
    auto* f = new MediaFormat;
    f->copy_from(c->input_format);
    return f;
}
WINAPK_GUEST_ABI MediaFormat* AMediaCodec_getBufferFormat(Codec* c, size_t index) {
    if (!c) return nullptr;
    std::lock_guard<std::mutex> hold(c->lock);
    if (index >= c->out.size() || !c->out[index].held) return nullptr;
    auto* f = new MediaFormat;
    f->copy_from(*c->output_format);
    return f;
}

int release_output(Codec* c, size_t index, bool render) {
    std::shared_ptr<vd::Frame> frame;
    {
        std::lock_guard<std::mutex> hold(c->lock);
        if (c->state != Codec::kRunning) return kErrInvalidOperation;
        if (index >= c->out.size() || !c->out[index].held) return kErrInvalidParameter;
        Codec::Out& o = c->out[index];
        o.held = false;
        if (render && c->surface) frame = std::move(o.frame);
        o.frame.reset();
        pump(c);
        hand_out_async(c);
    }
    if (frame) {  // converted and shown outside the lock: the window takes its time
        std::vector<uint32_t> argb = vd::frame_argb(*frame);
        if (!argb.empty()) guest_frame_present(argb.data(), frame->width, frame->height);
    }
    return kOk;
}
WINAPK_GUEST_ABI int AMediaCodec_releaseOutputBuffer(Codec* c, size_t index, bool render) {
    return c ? release_output(c, index, render) : kErrInvalidObject;
}
// The picture is shown now rather than at the given time: there is no presentation queue here.
WINAPK_GUEST_ABI int AMediaCodec_releaseOutputBufferAtTime(Codec* c, size_t index, int64_t) {
    return c ? release_output(c, index, true) : kErrInvalidObject;
}

WINAPK_GUEST_ABI int AMediaCodec_setOutputSurface(Codec* c, const void* surface) {
    if (!c) return kErrInvalidObject;
    std::lock_guard<std::mutex> hold(c->lock);
    // Android allows switching between surfaces, not between a surface and buffers.
    if (!c->surface || !surface || surface != native_window_handle()) return kErrInvalidOperation;
    c->surface = surface;
    return kOk;
}
// An encoder's "request-sync" (a key frame next) and "video-bitrate"; a decoder has none it uses.
WINAPK_GUEST_ABI int AMediaCodec_setParameters(Codec* c, MediaFormat* params) {
    if (!c) return kErrInvalidObject;
    if (!params) return kErrInvalidParameter;
    std::lock_guard<std::mutex> hold(c->lock);
    if (!c->encoder || !c->video) return kOk;
    {
        std::lock_guard<std::mutex> look(params->lock);
        if (params->values.count("request-sync")) c->force_key = true;
    }
    int32_t bps = 0;
    if (params->int32("video-bitrate", &bps) && bps > 0) {
        c->bitrate = bps;
        c->venc_config.bitrate = bps;
        if (c->venc) c->venc->set_bitrate(bps);
    }
    return kOk;
}
WINAPK_GUEST_ABI int AMediaCodec_signalEndOfInputStream(Codec* c) {
    return c ? kErrInvalidOperation : kErrInvalidObject;  // no codec here has an input surface
}
WINAPK_GUEST_ABI int AMediaCodec_createInputSurface(Codec* c, void** surface) {
    if (surface) *surface = nullptr;
    return c ? kErrInvalidOperation : kErrInvalidObject;
}
WINAPK_GUEST_ABI int AMediaCodec_createPersistentInputSurface(void** surface) {
    if (surface) *surface = nullptr;
    return kErrUnsupported;
}
WINAPK_GUEST_ABI int AMediaCodec_setInputSurface(Codec* c, void*) {
    return c ? kErrInvalidOperation : kErrInvalidObject;
}
WINAPK_GUEST_ABI bool AMediaCodecActionCode_isRecoverable(int32_t) { return false; }
WINAPK_GUEST_ABI bool AMediaCodecActionCode_isTransient(int32_t) { return false; }

// ---- AMediaDataSource ------------------------------------------------------------------------

struct DataSource {
    void* userdata = nullptr;
    int64_t(WINAPK_GUEST_ABI* read_at)(void* userdata, int64_t offset, void* buffer, size_t size) = nullptr;
    int64_t(WINAPK_GUEST_ABI* get_size)(void* userdata) = nullptr;
    void(WINAPK_GUEST_ABI* close)(void* userdata) = nullptr;
    int64_t(WINAPK_GUEST_ABI* get_available_size)(void* userdata, int64_t offset) = nullptr;
    std::string uri;  // AMediaDataSource_newUri's
};

WINAPK_GUEST_ABI DataSource* AMediaDataSource_new() { return new DataSource; }
WINAPK_GUEST_ABI DataSource* AMediaDataSource_newUri(const char* uri, int, const char* const*) {
    if (!uri) return nullptr;
    auto* d = new DataSource;
    d->uri = uri;
    return d;
}
WINAPK_GUEST_ABI void AMediaDataSource_delete(DataSource* d) { delete d; }
WINAPK_GUEST_ABI void AMediaDataSource_setUserdata(DataSource* d, void* userdata) {
    if (d) d->userdata = userdata;
}
WINAPK_GUEST_ABI void AMediaDataSource_setReadAt(DataSource* d, decltype(DataSource::read_at) fn) {
    if (d) d->read_at = guest_fn(fn);
}
WINAPK_GUEST_ABI void AMediaDataSource_setGetSize(DataSource* d, decltype(DataSource::get_size) fn) {
    if (d) d->get_size = guest_fn(fn);
}
WINAPK_GUEST_ABI void AMediaDataSource_setClose(DataSource* d, decltype(DataSource::close) fn) {
    if (d) d->close = guest_fn(fn);
}
WINAPK_GUEST_ABI void AMediaDataSource_setGetAvailableSize(DataSource* d,
                                                            decltype(DataSource::get_available_size) fn) {
    if (d) d->get_available_size = guest_fn(fn);
}
WINAPK_GUEST_ABI void AMediaDataSource_close(DataSource* d) {
    if (d && d->close) d->close(d->userdata);
}

// ---- AMediaExtractor -------------------------------------------------------------------------

struct Extractor {
    std::mutex lock;
    std::unique_ptr<vd::Demuxer> demuxer;
    std::wstring temp;  // the copy a descriptor's range or a data source was read into
    vd::Demuxer::Sample sample;
    bool have = false, done = false;

    ~Extractor() {
        demuxer.reset();
        if (!temp.empty()) DeleteFileW(temp.c_str());
    }
    // The current sample, read when first asked for.
    bool current() {
        if (!demuxer || done) return false;
        if (!have) {
            have = demuxer->read(&sample);
            done = !have;
        }
        return have;
    }
};

// A new temporary file's path, empty on failure.
std::wstring temp_file() {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"mex", 0, path)) return {};
    return path;
}

// Writes what `read(offset, buffer, size)` gives, from 0 until it gives nothing or `length` is
// reached (length < 0: no limit), into a new temporary file.
template <typename Read>
std::wstring copy_to_temp(int64_t length, Read read) {
    const std::wstring path = temp_file();
    if (path.empty()) return {};
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DeleteFileW(path.c_str());
        return {};
    }
    std::vector<uint8_t> buf(1 << 20);
    int64_t at = 0;
    bool ok = true;
    while (length < 0 || at < length) {
        const size_t want = static_cast<size_t>(length < 0 ? buf.size() : std::min<int64_t>(buf.size(), length - at));
        const int64_t got = read(at, buf.data(), want);
        if (got <= 0) {
            ok = got == 0 || length < 0;  // a source with no size ends with an error or nothing
            break;
        }
        DWORD wrote = 0;
        if (!WriteFile(h, buf.data(), static_cast<DWORD>(got), &wrote, nullptr) || wrote != got) {
            ok = false;
            break;
        }
        at += got;
    }
    CloseHandle(h);
    if (!ok || at == 0) {
        DeleteFileW(path.c_str());
        return {};
    }
    return path;
}

int open_demuxer(Extractor* e, const std::wstring& path, bool temp) {
    std::lock_guard<std::mutex> hold(e->lock);
    if (!e->temp.empty()) DeleteFileW(e->temp.c_str());
    e->temp = temp ? path : std::wstring();
    e->have = e->done = false;
    e->demuxer = vd::Demuxer::open(path);
    if (!e->demuxer) {
        log_once("AMediaExtractor: no demuxer reads the source", std::string(path.begin(), path.end()));
        return kErrUnsupported;
    }
    return kOk;
}

WINAPK_GUEST_ABI Extractor* AMediaExtractor_new() { return new Extractor; }
WINAPK_GUEST_ABI int AMediaExtractor_delete(Extractor* e) {
    delete e;
    return kOk;
}

WINAPK_GUEST_ABI int AMediaExtractor_setDataSource(Extractor* e, const char* location) {
    if (!e || !location) return kErrInvalidParameter;
    const std::string loc = location;
    if (loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, location, -1, nullptr, 0);
        std::wstring url(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, location, -1, url.data(), n);
        return open_demuxer(e, url, false);
    }
    const char* path = loc.rfind("file://", 0) == 0 ? location + 7 : location;
    return open_demuxer(e, winapk::android_host_path(path), false);
}

WINAPK_GUEST_ABI int AMediaExtractor_setDataSourceFd(Extractor* e, int fd, int64_t offset, int64_t length) {
    if (!e || fd < 0 || offset < 0) return kErrInvalidParameter;
    // Guest descriptors are CRT descriptors (native_asset.cpp): read the range through a
    // duplicate of the handle, so that the app's own file position stays where it was.
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) return kErrInvalidParameter;
    LARGE_INTEGER total{};
    if (!GetFileSizeEx(h, &total)) return kErrIo;
    if (length <= 0 || offset + length > total.QuadPart) length = total.QuadPart - offset;
    if (length <= 0) return kErrInvalidParameter;
    const std::wstring path = copy_to_temp(length, [&](int64_t at, uint8_t* buf, size_t size) -> int64_t {
        OVERLAPPED where{};
        const int64_t pos = offset + at;
        where.Offset = static_cast<DWORD>(pos);
        where.OffsetHigh = static_cast<DWORD>(pos >> 32);
        DWORD got = 0;
        return ReadFile(h, buf, static_cast<DWORD>(size), &got, &where) ? got : -1;
    });
    if (path.empty()) return kErrIo;
    return open_demuxer(e, path, true);
}

WINAPK_GUEST_ABI int AMediaExtractor_setDataSourceCustom(Extractor* e, DataSource* d) {
    if (!e || !d) return kErrInvalidParameter;
    if (!d->uri.empty()) return AMediaExtractor_setDataSource(e, d->uri.c_str());
    if (!d->read_at) return kErrInvalidParameter;
    const int64_t size = d->get_size ? d->get_size(d->userdata) : -1;
    const std::wstring path = copy_to_temp(size, [d](int64_t at, uint8_t* buf, size_t want) -> int64_t {
        return d->read_at(d->userdata, at, buf, want);
    });
    if (path.empty()) return kErrIo;
    return open_demuxer(e, path, true);
}

WINAPK_GUEST_ABI size_t AMediaExtractor_getTrackCount(Extractor* e) {
    if (!e) return 0;
    std::lock_guard<std::mutex> hold(e->lock);
    return e->demuxer ? static_cast<size_t>(e->demuxer->track_count()) : 0;
}

// MediaExtractor's track format keys, as AOSP's extractors set them.
MediaFormat* track_format(const vd::Demuxer& d, int index) {
    const vd::Demuxer::Track& t = d.track(index);
    auto* f = new MediaFormat;
    f->set_string("mime", t.mime);
    if (d.duration_us() > 0) f->set_int64("durationUs", d.duration_us());
    if (t.video) {
        f->set_int32("width", t.width);
        f->set_int32("height", t.height);
        if (t.rotation) f->set_int32("rotation-degrees", t.rotation);
        if (t.frame_rate > 0) f->set_int32("frame-rate", static_cast<int32_t>(t.frame_rate + 0.5));
        if (t.color.standard) f->set_int32("color-standard", t.color.standard);
        if (t.color.range) f->set_int32("color-range", t.color.range);
        if (t.color.transfer) f->set_int32("color-transfer", t.color.transfer);
        if (!t.color.hdr_static_info.empty()) f->set_buffer("hdr-static-info", t.color.hdr_static_info);
        if (t.width > 0 && t.height > 0)
            f->set_int32("max-input-size", std::max(kInCapMin, ((t.width + 15) / 16) * ((t.height + 15) / 16) * 192));
    } else {
        if (t.sample_rate) f->set_int32("sample-rate", t.sample_rate);
        if (t.channels) f->set_int32("channel-count", t.channels);
        f->set_int32("max-input-size", kInCapMin);
    }
    if (t.bitrate) f->set_int32("bitrate", t.bitrate);
    if (!t.csd0.empty()) f->set_buffer("csd-0", t.csd0);
    if (!t.csd1.empty()) f->set_buffer("csd-1", t.csd1);
    if (!t.csd2.empty()) f->set_buffer("csd-2", t.csd2);
    f->set_string("language", t.language.empty() ? "und" : t.language);
    return f;
}

WINAPK_GUEST_ABI MediaFormat* AMediaExtractor_getTrackFormat(Extractor* e, size_t index) {
    if (!e) return nullptr;
    std::lock_guard<std::mutex> hold(e->lock);
    if (!e->demuxer || index >= static_cast<size_t>(e->demuxer->track_count())) return nullptr;
    return track_format(*e->demuxer, static_cast<int>(index));
}

WINAPK_GUEST_ABI MediaFormat* AMediaExtractor_getFileFormat(Extractor* e) {
    if (!e) return nullptr;
    std::lock_guard<std::mutex> hold(e->lock);
    if (!e->demuxer) return nullptr;
    auto* f = new MediaFormat;
    if (e->demuxer->duration_us() > 0) f->set_int64("durationUs", e->demuxer->duration_us());
    return f;
}

WINAPK_GUEST_ABI int AMediaExtractor_selectTrack(Extractor* e, size_t index) {
    if (!e) return kErrInvalidObject;
    std::lock_guard<std::mutex> hold(e->lock);
    if (!e->demuxer || index >= static_cast<size_t>(e->demuxer->track_count())) return kErrInvalidParameter;
    e->demuxer->select(static_cast<int>(index), true);
    e->done = false;
    return kOk;
}
WINAPK_GUEST_ABI int AMediaExtractor_unselectTrack(Extractor* e, size_t index) {
    if (!e) return kErrInvalidObject;
    std::lock_guard<std::mutex> hold(e->lock);
    if (!e->demuxer || index >= static_cast<size_t>(e->demuxer->track_count())) return kErrInvalidParameter;
    e->demuxer->select(static_cast<int>(index), false);
    if (e->have && e->sample.track == static_cast<int>(index)) e->have = false;
    return kOk;
}

WINAPK_GUEST_ABI bool AMediaExtractor_advance(Extractor* e) {
    if (!e) return false;
    std::lock_guard<std::mutex> hold(e->lock);
    if (!e->current()) return false;
    e->have = false;
    return e->current();
}

WINAPK_GUEST_ABI int AMediaExtractor_seekTo(Extractor* e, int64_t us, int mode) {
    if (!e) return kErrInvalidObject;
    std::lock_guard<std::mutex> hold(e->lock);
    if (!e->demuxer) return kErrInvalidOperation;
    if (mode < 0 || mode > 2) return kErrInvalidParameter;
    e->have = e->done = false;
    return e->demuxer->seek(us, static_cast<vd::Demuxer::SeekMode>(mode)) ? kOk : kErrIo;
}

WINAPK_GUEST_ABI int64_t AMediaExtractor_readSampleData(Extractor* e, uint8_t* buffer, size_t capacity) {
    if (!e || !buffer) return -1;
    std::lock_guard<std::mutex> hold(e->lock);
    if (!e->current() || e->sample.data.size() > capacity) return -1;
    std::memcpy(buffer, e->sample.data.data(), e->sample.data.size());
    return static_cast<int64_t>(e->sample.data.size());
}
WINAPK_GUEST_ABI int64_t AMediaExtractor_getSampleSize(Extractor* e) {
    if (!e) return -1;
    std::lock_guard<std::mutex> hold(e->lock);
    return e->current() ? static_cast<int64_t>(e->sample.data.size()) : -1;
}
WINAPK_GUEST_ABI int64_t AMediaExtractor_getSampleTime(Extractor* e) {
    if (!e) return -1;
    std::lock_guard<std::mutex> hold(e->lock);
    return e->current() ? e->sample.pts_us : -1;
}
WINAPK_GUEST_ABI int AMediaExtractor_getSampleTrackIndex(Extractor* e) {
    if (!e) return -1;
    std::lock_guard<std::mutex> hold(e->lock);
    return e->current() ? e->sample.track : -1;
}
WINAPK_GUEST_ABI uint32_t AMediaExtractor_getSampleFlags(Extractor* e) {
    if (!e) return 0;
    std::lock_guard<std::mutex> hold(e->lock);
    return e->current() && e->sample.key ? 1u : 0u;  // AMEDIAEXTRACTOR_SAMPLE_FLAG_SYNC
}
WINAPK_GUEST_ABI int AMediaExtractor_getSampleFormat(Extractor* e, MediaFormat* f) {
    if (!e || !f) return kErrInvalidParameter;
    std::lock_guard<std::mutex> hold(e->lock);
    if (!e->current()) return kErrEndOfStream;
    {
        std::lock_guard<std::mutex> clear(f->lock);
        f->values.clear();
    }
    f->set_int64("timeUs", e->sample.pts_us);
    f->set_int32("is-sync-frame", e->sample.key ? 1 : 0);
    f->set_int32("track-index", e->sample.track);
    return kOk;
}
WINAPK_GUEST_ABI int64_t AMediaExtractor_getCachedDuration(Extractor*) { return -1; }  // a local source
WINAPK_GUEST_ABI void* AMediaExtractor_getPsshInfo(Extractor*) { return nullptr; }     // nothing is encrypted
WINAPK_GUEST_ABI void* AMediaExtractor_getSampleCryptoInfo(Extractor*) { return nullptr; }

struct Function {
    const char* name;
    void* fn;
};
#define WINAPK_NDK_MEDIA(fn) {#fn, reinterpret_cast<void*>(&fn)}
const Function kFunctions[] = {
    WINAPK_NDK_MEDIA(AMediaCodec_createDecoderByType), WINAPK_NDK_MEDIA(AMediaCodec_createEncoderByType),
    WINAPK_NDK_MEDIA(AMediaCodec_createCodecByName), WINAPK_NDK_MEDIA(AMediaCodec_delete),
    WINAPK_NDK_MEDIA(AMediaCodec_getName), WINAPK_NDK_MEDIA(AMediaCodec_releaseName),
    WINAPK_NDK_MEDIA(AMediaCodec_configure), WINAPK_NDK_MEDIA(AMediaCodec_setAsyncNotifyCallback),
    WINAPK_NDK_MEDIA(AMediaCodec_start), WINAPK_NDK_MEDIA(AMediaCodec_stop), WINAPK_NDK_MEDIA(AMediaCodec_flush),
    WINAPK_NDK_MEDIA(AMediaCodec_dequeueInputBuffer), WINAPK_NDK_MEDIA(AMediaCodec_getInputBuffer),
    WINAPK_NDK_MEDIA(AMediaCodec_queueInputBuffer), WINAPK_NDK_MEDIA(AMediaCodec_queueSecureInputBuffer),
    WINAPK_NDK_MEDIA(AMediaCodec_dequeueOutputBuffer), WINAPK_NDK_MEDIA(AMediaCodec_getOutputBuffer),
    WINAPK_NDK_MEDIA(AMediaCodec_getOutputFormat), WINAPK_NDK_MEDIA(AMediaCodec_getInputFormat),
    WINAPK_NDK_MEDIA(AMediaCodec_getBufferFormat), WINAPK_NDK_MEDIA(AMediaCodec_releaseOutputBuffer),
    WINAPK_NDK_MEDIA(AMediaCodec_releaseOutputBufferAtTime), WINAPK_NDK_MEDIA(AMediaCodec_setOutputSurface),
    WINAPK_NDK_MEDIA(AMediaCodec_setParameters), WINAPK_NDK_MEDIA(AMediaCodec_signalEndOfInputStream),
    WINAPK_NDK_MEDIA(AMediaCodec_createInputSurface), WINAPK_NDK_MEDIA(AMediaCodec_createPersistentInputSurface),
    WINAPK_NDK_MEDIA(AMediaCodec_setInputSurface), WINAPK_NDK_MEDIA(AMediaCodecActionCode_isRecoverable),
    WINAPK_NDK_MEDIA(AMediaCodecActionCode_isTransient),
    WINAPK_NDK_MEDIA(AMediaDataSource_new), WINAPK_NDK_MEDIA(AMediaDataSource_newUri),
    WINAPK_NDK_MEDIA(AMediaDataSource_delete), WINAPK_NDK_MEDIA(AMediaDataSource_setUserdata),
    WINAPK_NDK_MEDIA(AMediaDataSource_setReadAt), WINAPK_NDK_MEDIA(AMediaDataSource_setGetSize),
    WINAPK_NDK_MEDIA(AMediaDataSource_setClose), WINAPK_NDK_MEDIA(AMediaDataSource_setGetAvailableSize),
    WINAPK_NDK_MEDIA(AMediaDataSource_close),
    WINAPK_NDK_MEDIA(AMediaExtractor_new), WINAPK_NDK_MEDIA(AMediaExtractor_delete),
    WINAPK_NDK_MEDIA(AMediaExtractor_setDataSource), WINAPK_NDK_MEDIA(AMediaExtractor_setDataSourceFd),
    WINAPK_NDK_MEDIA(AMediaExtractor_setDataSourceCustom), WINAPK_NDK_MEDIA(AMediaExtractor_getTrackCount),
    WINAPK_NDK_MEDIA(AMediaExtractor_getTrackFormat), WINAPK_NDK_MEDIA(AMediaExtractor_getFileFormat),
    WINAPK_NDK_MEDIA(AMediaExtractor_selectTrack), WINAPK_NDK_MEDIA(AMediaExtractor_unselectTrack),
    WINAPK_NDK_MEDIA(AMediaExtractor_advance), WINAPK_NDK_MEDIA(AMediaExtractor_seekTo),
    WINAPK_NDK_MEDIA(AMediaExtractor_readSampleData), WINAPK_NDK_MEDIA(AMediaExtractor_getSampleSize),
    WINAPK_NDK_MEDIA(AMediaExtractor_getSampleTime), WINAPK_NDK_MEDIA(AMediaExtractor_getSampleTrackIndex),
    WINAPK_NDK_MEDIA(AMediaExtractor_getSampleFlags), WINAPK_NDK_MEDIA(AMediaExtractor_getSampleFormat),
    WINAPK_NDK_MEDIA(AMediaExtractor_getCachedDuration), WINAPK_NDK_MEDIA(AMediaExtractor_getPsshInfo),
    WINAPK_NDK_MEDIA(AMediaExtractor_getSampleCryptoInfo),
};
#undef WINAPK_NDK_MEDIA

}  // namespace

void* ndk_media_codec_resolve(const char* name) {
    for (const Function& f : kFunctions) {
        if (std::strcmp(name, f.name) == 0) return f.fn;
    }
    return nullptr;
}

}  // namespace winapk::host
