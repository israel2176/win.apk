// soft_codecs.cpp -- see soft_codecs.h.
//
// Adapted from the other runtime (https://example.invalid/other-runtime),
// compat/media/video/soft_video.cc, Copyright (C) 2026 its authors,
// SPDX-License-Identifier: Apache-2.0. Modified for Win.apk: pictures come out
// as video_decode.h's ARGB frames, and Opus is decoded here too.

#include "soft_codecs.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

#include <dav1d/dav1d.h>
#include <opus_multistream.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

namespace winapk::video::soft {

namespace {

constexpr const char* kVp8 = "video/x-vnd.on2.vp8";
constexpr const char* kVp9 = "video/x-vnd.on2.vp9";
constexpr const char* kAv1 = "video/av01";

// Decoding threads: what the machine has, up to four (more buys little at
// the sizes a phone app plays, and each holds its own buffers).
int threads() {
    const int n = static_cast<int>(std::thread::hardware_concurrency());
    return std::clamp(n, 1, 4);
}

// ---- pictures ----------------------------------------------------------------------------

struct Plane {
    const uint8_t* data = nullptr;
    ptrdiff_t stride = 0;
};

// The sample at (x, y) as 8 bits: a 16-bit one is `depth` bits deep.
inline uint8_t sample8(const Plane& p, int x, int y, bool high, int shift) {
    const uint8_t* row = p.data + static_cast<ptrdiff_t>(y) * p.stride;
    if (!high) return row[x];
    uint16_t v;
    std::memcpy(&v, row + 2 * static_cast<ptrdiff_t>(x), sizeof(v));
    const int s = v >> shift;
    return static_cast<uint8_t>(s > 255 ? 255 : s);
}

// A decoded w x h picture (planes Y, Cb, Cr; chroma subsampled by ss_x/ss_y,
// none at all without chroma; 16-bit samples `depth` deep when `high`) as an
// ARGB frame. 8-bit 4:2:0, nearly every stream, converts straight from the
// decoder's planes; anything else is first brought to 8-bit 4:2:0, deeper
// samples shifted down and fuller chroma subsampled, as the other runtime does.
std::shared_ptr<Frame> to_frame(int w, int h, const Plane (&p)[3], bool high, int depth, int ss_x, int ss_y,
                                bool has_chroma, bool bt709, bool full_range, int64_t pts_us) {
    if (w <= 0 || h <= 0) return nullptr;
    auto f = std::make_shared<Frame>();
    f->width = w;
    f->height = h;
    f->pts_us = pts_us;
    f->argb.resize(static_cast<size_t>(w) * h);
    if (!high && has_chroma && ss_x && ss_y && p[1].stride == p[2].stride) {
        i420_to_argb(p[0].data, static_cast<int>(p[0].stride), p[1].data, p[2].data, static_cast<int>(p[1].stride), w,
                     h, bt709, full_range, f->argb.data());
        return f;
    }
    const int shift = high ? std::max(depth - 8, 0) : 0;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    std::vector<uint8_t> y(static_cast<size_t>(w) * h), u(static_cast<size_t>(cw) * ch, 128),
        v(static_cast<size_t>(cw) * ch, 128);
    for (int r = 0; r < h; ++r) {
        for (int x = 0; x < w; ++x) y[static_cast<size_t>(r) * w + x] = sample8(p[0], x, r, high, shift);
    }
    if (has_chroma) {
        // The source's chroma size, for clamping.
        const int sw = ss_x ? (w + 1) / 2 : w, sh = ss_y ? (h + 1) / 2 : h;
        for (int r = 0; r < ch; ++r) {
            const int sy = std::min(ss_y ? r : 2 * r, sh - 1);
            for (int x = 0; x < cw; ++x) {
                const int sx = std::min(ss_x ? x : 2 * x, sw - 1);
                u[static_cast<size_t>(r) * cw + x] = sample8(p[1], sx, sy, high, shift);
                v[static_cast<size_t>(r) * cw + x] = sample8(p[2], sx, sy, high, shift);
            }
        }
    }
    i420_to_argb(y.data(), w, u.data(), v.data(), cw, w, h, bt709, full_range, f->argb.data());
    return f;
}

// ---- VP8 and VP9: libvpx -----------------------------------------------------------------

class VpxDecoder : public VideoDecoder {
public:
    ~VpxDecoder() override {
        if (open_) vpx_codec_destroy(&ctx_);
    }

    bool init(bool vp9) {
        vp9_ = vp9;
        vpx_codec_dec_cfg_t cfg = {};
        cfg.threads = static_cast<unsigned>(threads());
        const vpx_codec_err_t err = vpx_codec_dec_init(&ctx_, vp9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx(), &cfg, 0);
        if (err != VPX_CODEC_OK) {
            std::fprintf(stderr, "[media] %s: %s\n", name(), vpx_codec_err_to_string(err));
            return false;
        }
        open_ = true;
        return true;
    }

    const char* name() const override { return vp9_ ? "libvpx VP9" : "libvpx VP8"; }

    bool decode(const uint8_t* au, size_t size, int64_t pts_us, std::vector<std::shared_ptr<Frame>>* out) override {
        if (!au || !size) return true;
        last_pts_ = pts_us;
        const vpx_codec_err_t err = vpx_codec_decode(&ctx_, au, static_cast<unsigned>(size), nullptr, 0);
        if (err != VPX_CODEC_OK) {
            const char* detail = vpx_codec_error_detail(&ctx_);
            std::fprintf(stderr, "[media] %s: %s%s%s\n", name(), vpx_codec_err_to_string(err), detail ? ": " : "",
                         detail ? detail : "");
            return false;
        }
        pull(out, pts_us);
        return true;
    }

    void drain(std::vector<std::shared_ptr<Frame>>* out) override {
        vpx_codec_decode(&ctx_, nullptr, 0, nullptr, 0);
        pull(out, last_pts_);
    }

    // libvpx keeps no queue between calls: every picture an access unit
    // completes comes out with it, so there is nothing to drop.
    void flush() override {}

private:
    void pull(std::vector<std::shared_ptr<Frame>>* out, int64_t pts_us) {
        vpx_codec_iter_t iter = nullptr;
        while (vpx_image_t* img = vpx_codec_get_frame(&ctx_, &iter)) {
            const bool high = (img->fmt & VPX_IMG_FMT_HIGHBITDEPTH) != 0;
            Plane p[3];
            for (int i = 0; i < 3; ++i) {
                p[i].data = img->planes[i];
                p[i].stride = img->stride[i];
            }
            const bool bt709 = img->cs == VPX_CS_BT_709 || img->cs == VPX_CS_BT_2020 ||
                               (img->cs == VPX_CS_UNKNOWN && img->d_h >= 720);
            auto f = to_frame(static_cast<int>(img->d_w), static_cast<int>(img->d_h), p, high,
                              static_cast<int>(img->bit_depth), static_cast<int>(img->x_chroma_shift),
                              static_cast<int>(img->y_chroma_shift), true, bt709, img->range == VPX_CR_FULL_RANGE,
                              pts_us);
            if (f) out->push_back(std::move(f));
        }
    }

    vpx_codec_ctx_t ctx_ = {};
    bool open_ = false;
    bool vp9_ = false;
    int64_t last_pts_ = 0;
};

// ---- AV1: dav1d --------------------------------------------------------------------------

class Dav1dDecoder : public VideoDecoder {
public:
    ~Dav1dDecoder() override {
        if (c_) dav1d_close(&c_);
    }

    bool init() {
        Dav1dSettings s;
        dav1d_default_settings(&s);
        s.n_threads = threads();
        // One picture in flight: each temporal unit's picture comes out with
        // it, as MediaCodec's callers expect of a decoder without reordering.
        s.max_frame_delay = 1;
        const int r = dav1d_open(&c_, &s);
        if (r < 0) {
            std::fprintf(stderr, "[media] dav1d: dav1d_open %d\n", r);
            c_ = nullptr;
            return false;
        }
        return true;
    }

    const char* name() const override { return "dav1d AV1"; }

    bool decode(const uint8_t* au, size_t size, int64_t pts_us, std::vector<std::shared_ptr<Frame>>* out) override {
        if (!au || !size) return true;
        Dav1dData data = {};
        uint8_t* p = dav1d_data_create(&data, size);
        if (!p) return false;
        std::memcpy(p, au, size);
        data.m.timestamp = pts_us;
        // dav1d takes the data in parts while its picture queue is full:
        // take pictures out between the parts.
        for (int guard = 0; data.sz > 0; ++guard) {
            const int r = dav1d_send_data(c_, &data);
            if ((r < 0 && r != DAV1D_ERR(EAGAIN)) || guard > 1000) {
                std::fprintf(stderr, "[media] dav1d: dav1d_send_data %d\n", r);
                dav1d_data_unref(&data);
                return false;
            }
            if (!pull(out)) {
                dav1d_data_unref(&data);
                return false;
            }
        }
        return pull(out);
    }

    void drain(std::vector<std::shared_ptr<Frame>>* out) override {
        // With nothing more to send, every picture still inside comes out.
        for (int guard = 0; guard < 64; ++guard) {
            const size_t before = out->size();
            if (!pull(out) || out->size() == before) break;
        }
    }

    void flush() override { dav1d_flush(c_); }

private:
    bool pull(std::vector<std::shared_ptr<Frame>>* out) {
        for (;;) {
            Dav1dPicture pic = {};
            const int r = dav1d_get_picture(c_, &pic);
            if (r == DAV1D_ERR(EAGAIN)) return true;
            if (r < 0) {
                std::fprintf(stderr, "[media] dav1d: dav1d_get_picture %d\n", r);
                return false;
            }
            Plane p[3];
            p[0].data = static_cast<const uint8_t*>(pic.data[0]);
            p[0].stride = pic.stride[0];
            for (int i = 1; i < 3; ++i) {
                p[i].data = static_cast<const uint8_t*>(pic.data[i]);
                p[i].stride = pic.stride[1];
            }
            const Dav1dPixelLayout layout = pic.p.layout;
            const Dav1dSequenceHeader* seq = pic.seq_hdr;
            const bool bt709 = seq && (seq->mtrx == DAV1D_MC_BT709 || seq->mtrx == DAV1D_MC_BT2020_NCL ||
                                       (seq->mtrx == DAV1D_MC_UNKNOWN && pic.p.h >= 720));
            auto f = to_frame(pic.p.w, pic.p.h, p, pic.p.bpc > 8, pic.p.bpc, layout == DAV1D_PIXEL_LAYOUT_I444 ? 0 : 1,
                              layout == DAV1D_PIXEL_LAYOUT_I420 ? 1 : 0, layout != DAV1D_PIXEL_LAYOUT_I400, bt709,
                              seq && seq->color_range, pic.m.timestamp);
            dav1d_picture_unref(&pic);
            if (f) out->push_back(std::move(f));
        }
    }

    Dav1dContext* c_ = nullptr;
};

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

}  // namespace

// ---- the mime list -----------------------------------------------------------------------

const std::vector<std::string>& video_mimes() {
    static const std::vector<std::string> mimes = {kVp8, kVp9, kAv1};
    return mimes;
}

bool decodes_video(const std::string& mime) { return mime == kVp8 || mime == kVp9 || mime == kAv1; }

std::vector<int> video_capabilities(const std::string& mime) {
    // MediaCodecInfo.CodecProfileLevel's values.
    constexpr int kVp8ProfileMain = 0x01, kVp8LevelVersion0 = 0x01;
    constexpr int kVp9Profile0 = 0x01, kVp9Profile2 = 0x04, kVp9Level5 = 0x100;
    constexpr int kAv1Main8 = 0x1, kAv1Main10 = 0x2, kAv1Level73 = 0x800000;
    // media_codecs_google_c2_video.xml: each of these decoders is offered up
    // to 2048x2048 and 16384 16x16 blocks; above that an app (ExoPlayer)
    // picks a smaller track, as on a device.
    if (mime == kVp8) return {2048, 2048, 16384, 1, kVp8ProfileMain, kVp8LevelVersion0};
    // C2SoftVpxDec: profiles 0 and 2 (10-bit, shown here at 8 bits) to level 5.
    if (mime == kVp9) return {2048, 2048, 16384, 2, kVp9Profile0, kVp9Level5, kVp9Profile2, kVp9Level5};
    // C2SoftDav1dDec: the main profile, 8 and 10 bits, to level 7.3.
    if (mime == kAv1) return {2048, 2048, 16384, 2, kAv1Main8, kAv1Level73, kAv1Main10, kAv1Level73};
    return {};
}

std::unique_ptr<VideoDecoder> open_video(const std::string& mime) {
    if (mime == kVp8 || mime == kVp9) {
        auto d = std::make_unique<VpxDecoder>();
        if (!d->init(mime == kVp9)) return nullptr;
        return d;
    }
    if (mime == kAv1) {
        auto d = std::make_unique<Dav1dDecoder>();
        if (!d->init()) return nullptr;
        return d;
    }
    return nullptr;
}

// ---- Opus --------------------------------------------------------------------------------

std::unique_ptr<OpusDecoder> OpusDecoder::open(const std::vector<uint8_t>& head, int channels) {
    int streams = 1, coupled = 0, gain = 0, pre_skip = 0;
    unsigned char mapping[255] = {0, 1};
    if (head.size() >= 19 && std::memcmp(head.data(), "OpusHead", 8) == 0) {
        // RFC 7845 5.1: version, channel count, pre-skip, input rate, output
        // gain (Q7.8 dB), mapping family, then for families other than 0 the
        // stream count, the coupled count and one mapping byte per channel.
        channels = head[9];
        pre_skip = le16(&head[10]);
        gain = static_cast<int16_t>(le16(&head[16]));
        const int family = head[18];
        if (family == 0) {
            if (channels < 1 || channels > 2) return nullptr;
            coupled = channels - 1;
        } else {
            if (head.size() < static_cast<size_t>(21 + channels) || channels < 1) return nullptr;
            streams = head[19];
            coupled = head[20];
            std::memcpy(mapping, &head[21], static_cast<size_t>(channels));
        }
    } else {
        if (channels < 1 || channels > 2) return nullptr;
        coupled = channels - 1;
    }
    int err = 0;
    OpusMSDecoder* dec = opus_multistream_decoder_create(kSampleRate, channels, streams, coupled, mapping, &err);
    if (!dec || err != OPUS_OK) {
        std::fprintf(stderr, "[media] libopus: %s\n", opus_strerror(err));
        if (dec) opus_multistream_decoder_destroy(dec);
        return nullptr;
    }
    if (gain) opus_multistream_decoder_ctl(dec, OPUS_SET_GAIN(gain));
    std::unique_ptr<OpusDecoder> d(new OpusDecoder());
    d->decoder_ = dec;
    d->channels_ = channels;
    d->skip_ = pre_skip;
    return d;
}

OpusDecoder::~OpusDecoder() {
    if (decoder_) opus_multistream_decoder_destroy(static_cast<OpusMSDecoder*>(decoder_));
}

std::vector<int16_t> OpusDecoder::decode(const uint8_t* packet, size_t size) {
    std::vector<int16_t> pcm;
    if (!packet || !size) return pcm;
    constexpr int kMaxFrame = 5760;  // 120 ms at 48 kHz, the longest packet
    pcm.resize(static_cast<size_t>(kMaxFrame) * channels_);
    const int n = opus_multistream_decode(static_cast<OpusMSDecoder*>(decoder_), packet, static_cast<opus_int32>(size),
                                          pcm.data(), kMaxFrame, 0);
    if (n < 0) {
        std::fprintf(stderr, "[media] libopus: %s\n", opus_strerror(n));
        pcm.clear();
        return pcm;
    }
    pcm.resize(static_cast<size_t>(n) * channels_);
    const int drop = std::min(skip_, n);
    if (drop > 0) {
        pcm.erase(pcm.begin(), pcm.begin() + static_cast<ptrdiff_t>(drop) * channels_);
        skip_ -= drop;
    }
    return pcm;
}

}  // namespace winapk::video::soft
