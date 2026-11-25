// soft_codecs.cpp -- the built-in VP8/VP9 (libvpx), AV1 (dav1d) and Opus (libopus) decoders
// (soft_codecs.h). Written against each library's public API: vpx/vpx_decoder.h with
// vpx/vpx_frame_buffer.h, dav1d/dav1d.h, opus_multistream.h.

#include "soft_codecs.h"

#include <dav1d/dav1d.h>
#include <opus_multistream.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_frame_buffer.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <mutex>
#include <thread>

namespace winapk::video::soft {

namespace {

constexpr const char* kVp8 = "video/x-vnd.on2.vp8";
constexpr const char* kVp9 = "video/x-vnd.on2.vp9";
constexpr const char* kAv1 = "video/av01";

// libvpx's limit is its configuration's (--size-limit, soft_codecs_config.sh); dav1d's is set here.
constexpr unsigned kMaxPixels = 8192u * 8192u;

static_assert(sizeof(void*) >= sizeof(int64_t), "a presentation time travels in libvpx's user_priv pointer");

// Decoding threads for libvpx: one per core, up to 8 (VP9 cannot use more than its tile
// columns and rows; VP8 than its token partitions). dav1d's are sized below.
int vpx_threads() {
    const unsigned n = std::thread::hardware_concurrency();
    return static_cast<int>(std::clamp(n, 1u, 8u));
}

// ---- Pooled picture memory -------------------------------------------------------------------

// Buffers for decoded pictures, kept for reuse: a picture's buffer goes back to the pool when its
// last holder (the decoder, a Frame on its way to the app) lets it go, so a stream at a steady
// size allocates only while the pool warms up. Shared by the decoder and the pictures it gave
// out, which may outlive it (MediaCodec.release with a frame still held).
class BufferPool {
public:
    struct Buffer {
        std::vector<uint8_t> mem;
        std::atomic<int> refs{0};
    };

    // A buffer of at least `size` bytes with one reference. A new or enlarged buffer is zeroed
    // (libvpx's frame buffer contract asks it of fresh memory); a reused one keeps its bytes.
    Buffer* acquire(size_t size) {
        std::lock_guard<std::mutex> hold(lock_);
        Buffer* b = nullptr;
        for (size_t i = 0; i < free_.size(); ++i) {
            if (free_[i]->mem.size() >= size) {
                b = free_[i];
                free_.erase(free_.begin() + static_cast<ptrdiff_t>(i));
                break;
            }
        }
        if (!b && !free_.empty()) {  // none big enough: enlarge one rather than keep both
            b = free_.back();
            free_.pop_back();
            b->mem.assign(size, 0);
        }
        if (!b) {
            all_.push_back(std::make_unique<Buffer>());
            b = all_.back().get();
            b->mem.assign(size, 0);
        }
        b->refs.store(1, std::memory_order_relaxed);
        return b;
    }
    static void ref(Buffer* b) { b->refs.fetch_add(1, std::memory_order_relaxed); }
    void unref(Buffer* b) {
        if (b->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        std::lock_guard<std::mutex> hold(lock_);
        free_.push_back(b);
    }

private:
    std::mutex lock_;
    std::vector<std::unique_ptr<Buffer>> all_;
    std::vector<Buffer*> free_;
};

// A picture over a pooled buffer.
struct PooledPicture : Picture {
    std::shared_ptr<BufferPool> pool;
    BufferPool::Buffer* buffer = nullptr;
    ~PooledPicture() override {
        if (buffer) pool->unref(buffer);
    }
};

// ---- Colour ----------------------------------------------------------------------------------

ColorAspects vpx_color(const vpx_image_t* img) {
    ColorAspects c;
    switch (img->cs) {
        case VPX_CS_BT_601:
        case VPX_CS_SMPTE_170: c.standard = ColorAspects::kBt601Ntsc; break;
        case VPX_CS_BT_709:
        case VPX_CS_SRGB: c.standard = ColorAspects::kBt709; break;
        case VPX_CS_BT_2020: c.standard = ColorAspects::kBt2020; break;
        default: break;  // unknown, SMPTE 240M, reserved: unsaid
    }
    c.range = img->range == VPX_CR_FULL_RANGE ? ColorAspects::kFull : ColorAspects::kLimited;
    // VP9 has no transfer characteristics in its bitstream: the container's (WebM's Colour) say it.
    return c;
}

// AV1's colour description (the sequence header's, ITU-T H.273's code points) in MediaFormat's
// terms, as Android's ColorUtils maps the same code points.
ColorAspects av1_color(const Dav1dPicture& p) {
    ColorAspects c;
    if (const Dav1dSequenceHeader* s = p.seq_hdr) {
        switch (s->mtrx) {
            case DAV1D_MC_BT709: c.standard = ColorAspects::kBt709; break;
            case DAV1D_MC_BT470BG: c.standard = ColorAspects::kBt601Pal; break;
            case DAV1D_MC_BT601: c.standard = ColorAspects::kBt601Ntsc; break;
            case DAV1D_MC_BT2020_NCL:
            case DAV1D_MC_BT2020_CL: c.standard = ColorAspects::kBt2020; break;
            default:  // unsaid by the matrix: the primaries
                if (s->pri == DAV1D_COLOR_PRI_BT709)
                    c.standard = ColorAspects::kBt709;
                else if (s->pri == DAV1D_COLOR_PRI_BT470BG)
                    c.standard = ColorAspects::kBt601Pal;
                else if (s->pri == DAV1D_COLOR_PRI_BT601)
                    c.standard = ColorAspects::kBt601Ntsc;
                else if (s->pri == DAV1D_COLOR_PRI_BT2020)
                    c.standard = ColorAspects::kBt2020;
                break;
        }
        switch (s->trc) {
            case DAV1D_TRC_BT709:
            case DAV1D_TRC_BT601:
            case DAV1D_TRC_BT2020_10BIT:
            case DAV1D_TRC_BT2020_12BIT: c.transfer = ColorAspects::kSdrVideo; break;
            case DAV1D_TRC_SMPTE2084: c.transfer = ColorAspects::kSt2084; break;
            case DAV1D_TRC_HLG: c.transfer = ColorAspects::kHlg; break;
            case DAV1D_TRC_LINEAR: c.transfer = ColorAspects::kLinear; break;
            default: break;
        }
        c.range = s->color_range ? ColorAspects::kFull : ColorAspects::kLimited;
    }
    if (p.mastering_display || p.content_light) {
        // KEY_HDR_STATIC_INFO (video_decode.h). AV1's mastering display gives the primaries R,
        // G, B in 0.16 fixed point; Android wants 0.00002 units: v / 65536 * 50000.
        uint16_t v[12] = {};
        if (const Dav1dMasteringDisplay* m = p.mastering_display) {
            auto chroma = [](uint16_t x) { return static_cast<uint16_t>((x * 50000u + 32768u) / 65536u); };
            for (int i = 0; i < 3; ++i) {
                v[2 * i] = chroma(m->primaries[i][0]);
                v[2 * i + 1] = chroma(m->primaries[i][1]);
            }
            v[6] = chroma(m->white_point[0]);
            v[7] = chroma(m->white_point[1]);
            v[8] = static_cast<uint16_t>(std::min<uint32_t>((m->max_luminance + 128) >> 8, 65535));  // 24.8: cd/m2
            // 18.14 to 0.0001 cd/m2.
            v[9] = static_cast<uint16_t>(std::min<uint64_t>((m->min_luminance * 10000ull + 8192) >> 14, 65535));
        }
        if (const Dav1dContentLightLevel* l = p.content_light) {
            v[10] = l->max_content_light_level;
            v[11] = l->max_frame_average_light_level;
        }
        c.hdr_static_info.assign(25, 0);
        for (int i = 0; i < 12; ++i) {
            c.hdr_static_info[1 + 2 * i] = static_cast<uint8_t>(v[i] & 0xff);
            c.hdr_static_info[2 + 2 * i] = static_cast<uint8_t>(v[i] >> 8);
        }
    }
    return c;
}

// ---- libvpx: VP8 and VP9 ---------------------------------------------------------------------

class VpxDecoder final : public VideoDecoder {
public:
    static std::unique_ptr<VideoDecoder> open(bool vp9) {
        std::unique_ptr<VpxDecoder> d(new VpxDecoder(vp9));
        vpx_codec_dec_cfg_t cfg{};
        cfg.threads = static_cast<unsigned>(vpx_threads());
        if (vpx_codec_dec_init(&d->ctx_, vp9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx(), &cfg, 0) != VPX_CODEC_OK) {
            return nullptr;
        }
        d->live_ = true;
        if (vp9) {
            // Row-based threading as well as by tile: most streams have few tile columns.
            vpx_codec_control(&d->ctx_, VP9D_SET_ROW_MT, 1);
            // libvpx decodes into this side's buffers, so a picture leaves without a copy.
            if (vpx_codec_set_frame_buffer_functions(&d->ctx_, &VpxDecoder::get_buffer, &VpxDecoder::release_buffer,
                                                     d->pool_.get()) != VPX_CODEC_OK) {
                return nullptr;
            }
        }
        return d;
    }
    ~VpxDecoder() override {
        if (live_) vpx_codec_destroy(&ctx_);
    }

    bool decode(const uint8_t* au, size_t size, int64_t pts_us, std::vector<std::shared_ptr<Frame>>* out) override {
        if (!au || size == 0) return true;
        if (size > UINT_MAX) return false;
        // The presentation time rides with the unit and comes back on its picture (user_priv).
        const vpx_codec_err_t err = vpx_codec_decode(&ctx_, au, static_cast<unsigned>(size),
                                                     reinterpret_cast<void*>(static_cast<intptr_t>(pts_us)), 0);
        collect(out);
        return err == VPX_CODEC_OK;
    }
    void drain(std::vector<std::shared_ptr<Frame>>* out) override {
        vpx_codec_decode(&ctx_, nullptr, 0, nullptr, 0);
        collect(out);
    }
    void flush() override {
        // libvpx keeps no queue of its own beyond what a null decode returns; what follows a
        // flush is a key frame, which resets its references.
        std::vector<std::shared_ptr<Frame>> dropped;
        drain(&dropped);
    }
    const char* name() const override { return vp9_ ? "libvpx VP9" : "libvpx VP8"; }

private:
    explicit VpxDecoder(bool vp9) : vp9_(vp9), pool_(std::make_shared<BufferPool>()) {}

    static int get_buffer(void* priv, size_t min_size, vpx_codec_frame_buffer_t* fb) {
        BufferPool::Buffer* b = static_cast<BufferPool*>(priv)->acquire(min_size);
        fb->data = b->mem.data();
        fb->size = b->mem.size();
        fb->priv = b;
        return 0;
    }
    static int release_buffer(void* priv, vpx_codec_frame_buffer_t* fb) {
        if (fb->priv) static_cast<BufferPool*>(priv)->unref(static_cast<BufferPool::Buffer*>(fb->priv));
        return 0;
    }

    void collect(std::vector<std::shared_ptr<Frame>>* out) {
        vpx_codec_iter_t it = nullptr;
        while (const vpx_image_t* img = vpx_codec_get_frame(&ctx_, &it)) {
            if (auto f = frame_of(img)) out->push_back(std::move(f));
        }
    }

    std::shared_ptr<Frame> frame_of(const vpx_image_t* img) {
        if (img->d_w == 0 || img->d_h == 0 || img->x_chroma_shift > 1 || img->y_chroma_shift > 1) return nullptr;
        const bool wide = (img->fmt & VPX_IMG_FMT_HIGHBITDEPTH) != 0;
        auto pic = std::make_shared<PooledPicture>();
        pic->pool = pool_;
        pic->width = static_cast<int>(img->d_w);
        pic->height = static_cast<int>(img->d_h);
        pic->bit_depth = static_cast<int>(img->bit_depth);
        pic->chroma_shift_x = static_cast<int>(img->x_chroma_shift);
        pic->chroma_shift_y = static_cast<int>(img->y_chroma_shift);
        pic->color = vpx_color(img);
        if (img->fb_priv && (wide == (img->bit_depth > 8))) {
            // VP9 decoded into a pooled buffer: hold it as it is.
            pic->buffer = static_cast<BufferPool::Buffer*>(img->fb_priv);
            BufferPool::ref(pic->buffer);
            for (int i = 0; i < 3; ++i) {
                pic->plane[i] = img->planes[i];
                pic->stride[i] = img->stride[i];
            }
        } else if (!copy_planes(img, wide, pic.get())) {
            return nullptr;
        }
        auto f = std::make_shared<Frame>();
        f->width = pic->width;
        f->height = pic->height;
        f->pts_us = static_cast<int64_t>(reinterpret_cast<intptr_t>(img->user_priv));
        f->picture = std::move(pic);
        return f;
    }

    // VP8 gives its own buffer, which the next decode reuses: copy the visible picture into a
    // pooled one (and an 8-bit picture libvpx stored in 16-bit samples down to 8).
    bool copy_planes(const vpx_image_t* img, bool wide, PooledPicture* pic) {
        const int in_bytes = wide ? 2 : 1;
        const int out_bytes = pic->bit_depth > 8 ? 2 : 1;
        int w[3], h[3];
        size_t offset[3], total = 0;
        for (int i = 0; i < 3; ++i) {
            w[i] = i == 0 ? pic->width : (pic->width + (1 << pic->chroma_shift_x) - 1) >> pic->chroma_shift_x;
            h[i] = i == 0 ? pic->height : (pic->height + (1 << pic->chroma_shift_y) - 1) >> pic->chroma_shift_y;
            pic->stride[i] = (w[i] * out_bytes + 31) & ~31;
            offset[i] = total;
            total += static_cast<size_t>(pic->stride[i]) * h[i];
        }
        pic->buffer = pool_->acquire(total);
        for (int i = 0; i < 3; ++i) {
            uint8_t* dst = pic->buffer->mem.data() + offset[i];
            pic->plane[i] = dst;
            for (int y = 0; y < h[i]; ++y) {
                const uint8_t* s = img->planes[i] + static_cast<ptrdiff_t>(img->stride[i]) * y;
                uint8_t* d = dst + static_cast<size_t>(pic->stride[i]) * y;
                if (in_bytes == out_bytes) {
                    std::memcpy(d, s, static_cast<size_t>(w[i]) * out_bytes);
                } else {
                    const uint16_t* s16 = reinterpret_cast<const uint16_t*>(s);
                    for (int x = 0; x < w[i]; ++x) d[x] = static_cast<uint8_t>(std::min<int>(s16[x], 255));
                }
            }
        }
        return true;
    }

    bool vp9_;
    bool live_ = false;
    vpx_codec_ctx_t ctx_{};
    std::shared_ptr<BufferPool> pool_;
};

// ---- dav1d: AV1 ------------------------------------------------------------------------------

// Copies of the access units dav1d still reads (with frame threads it reads a unit after
// dav1d_send_data returns), kept for reuse. dav1d hands a copy back through its free callback,
// from any of its threads.
class InputPool {
public:
    struct Slot {
        InputPool* pool = nullptr;
        std::vector<uint8_t> mem;
    };
    Slot* acquire(const uint8_t* data, size_t size) {
        Slot* s = nullptr;
        {
            std::lock_guard<std::mutex> hold(lock_);
            if (!free_.empty()) {
                s = free_.back();
                free_.pop_back();
            } else {
                all_.push_back(std::make_unique<Slot>());
                s = all_.back().get();
                s->pool = this;
            }
        }
        s->mem.assign(data, data + size);
        return s;
    }
    static void release(const uint8_t*, void* cookie) {
        Slot* s = static_cast<Slot*>(cookie);
        std::lock_guard<std::mutex> hold(s->pool->lock_);
        s->pool->free_.push_back(s);
    }

private:
    std::mutex lock_;
    std::vector<std::unique_ptr<Slot>> all_;
    std::vector<Slot*> free_;
};

// A dav1d picture, held by its reference (dav1d's picture memory is pooled and reference counted,
// and may outlive the decoder).
struct Dav1dHeld : Picture {
    Dav1dPicture pic{};
    ~Dav1dHeld() override { dav1d_picture_unref(&pic); }
};

class Dav1dDecoder final : public VideoDecoder {
public:
    static std::unique_ptr<VideoDecoder> open() {
        std::unique_ptr<Dav1dDecoder> d(new Dav1dDecoder());
        Dav1dSettings s;
        dav1d_default_settings(&s);
        // Frame and tile threads, one per core up to 16; the frame delay dav1d derives from
        // them (about their square root) is what lets frames decode in parallel.
        s.n_threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 16u));
        s.max_frame_delay = 0;
        s.apply_grain = 1;  // as Android's AV1 decoders: the picture as its author meant it shown
        s.all_layers = 0;   // one picture per temporal unit, the highest spatial layer
        s.frame_size_limit = kMaxPixels;
        if (dav1d_open(&d->ctx_, &s) < 0) return nullptr;
        return d;
    }
    ~Dav1dDecoder() override {
        // Before inputs_ goes: closing hands back the units dav1d still holds.
        if (ctx_) dav1d_close(&ctx_);
    }

    bool decode(const uint8_t* au, size_t size, int64_t pts_us, std::vector<std::shared_ptr<Frame>>* out) override {
        if (!au || size == 0) return true;
        InputPool::Slot* slot = inputs_.acquire(au, size);
        Dav1dData data{};
        if (dav1d_data_wrap(&data, slot->mem.data(), size, &InputPool::release, slot) < 0) {
            InputPool::release(nullptr, slot);
            return false;
        }
        data.m.timestamp = pts_us;
        bool ok = true;
        do {
            // Not consumed (EAGAIN) while dav1d's frame queue is full: taking its pictures out
            // makes room.
            const int sent = dav1d_send_data(ctx_, &data);
            if (sent < 0 && sent != DAV1D_ERR(EAGAIN)) {
                ok = false;
                break;
            }
            if (!take_pictures(out)) {
                ok = false;
                break;
            }
        } while (data.sz > 0);
        if (data.sz > 0) dav1d_data_unref(&data);
        return ok;
    }
    void drain(std::vector<std::shared_ptr<Frame>>* out) override {
        // With no more data, dav1d_get_picture finishes the frames in flight one by one.
        take_pictures(out);
    }
    void flush() override { dav1d_flush(ctx_); }
    const char* name() const override { return "dav1d AV1"; }

private:
    Dav1dDecoder() = default;

    // Every picture dav1d has ready. False on a decoding error (that unit is lost; dav1d goes on).
    bool take_pictures(std::vector<std::shared_ptr<Frame>>* out) {
        for (;;) {
            auto held = std::make_shared<Dav1dHeld>();
            const int got = dav1d_get_picture(ctx_, &held->pic);
            if (got == DAV1D_ERR(EAGAIN)) return true;
            if (got < 0) return false;
            if (auto f = frame_of(std::move(held))) out->push_back(std::move(f));
        }
    }

    static std::shared_ptr<Frame> frame_of(std::shared_ptr<Dav1dHeld> held) {
        const Dav1dPicture& p = held->pic;
        if (p.p.w <= 0 || p.p.h <= 0 || !p.data[0]) return nullptr;
        held->width = p.p.w;
        held->height = p.p.h;
        held->bit_depth = p.p.bpc;
        held->monochrome = p.p.layout == DAV1D_PIXEL_LAYOUT_I400;
        held->chroma_shift_x = p.p.layout == DAV1D_PIXEL_LAYOUT_I444 ? 0 : 1;
        held->chroma_shift_y = p.p.layout == DAV1D_PIXEL_LAYOUT_I420 || held->monochrome ? 1 : 0;
        for (int i = 0; i < 3; ++i) {
            held->plane[i] = static_cast<const uint8_t*>(p.data[i]);
            held->stride[i] = static_cast<int>(p.stride[i == 0 ? 0 : 1]);
        }
        held->color = av1_color(p);
        auto f = std::make_shared<Frame>();
        f->width = held->width;
        f->height = held->height;
        f->pts_us = p.m.timestamp;
        f->picture = std::move(held);
        return f;
    }

    InputPool inputs_;
    Dav1dContext* ctx_ = nullptr;
};

// ---- Opus ------------------------------------------------------------------------------------

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint64_t le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
    return v;
}

// RFC 7845 5.1.1.2's Vorbis channel order to Android's (AudioFormat's channel bits from lowest:
// FL, FR, FC, LFE, BL, BR, FLC, FRC, BC, SL, SR): for n channels, Android's channel i is the
// stream's channel kVorbisToAndroid[n - 1][i]; and the CHANNEL_OUT_* mask of the result.
constexpr int kVorbisToAndroid[8][8] = {
    {0},                       // mono
    {0, 1},                    // L R
    {0, 2, 1},                 // L C R            -> FL FR FC
    {0, 1, 2, 3},              // FL FR RL RR      -> FL FR BL BR
    {0, 2, 1, 3, 4},           // FL C FR RL RR    -> FL FR FC BL BR
    {0, 2, 1, 5, 3, 4},        // + LFE            -> FL FR FC LFE BL BR
    {0, 2, 1, 6, 5, 3, 4},     // FL C FR SL SR RC LFE -> FL FR FC LFE BC SL SR
    {0, 2, 1, 7, 5, 6, 3, 4},  // FL C FR SL SR RL RR LFE -> FL FR FC LFE BL BR SL SR
};
constexpr int kAndroidMask[8] = {0x4, 0xc, 0x1c, 0xcc, 0xdc, 0xfc, 0x1c3c, 0x18fc};

struct OpusHead {
    int channels = 0, pre_skip = 0, gain = 0, family = 0, streams = 1, coupled = 0;
    unsigned char mapping[255] = {};
};

// RFC 7845 5.1. Families 0 (mono, stereo), 1 (Vorbis order, up to 8), 2 (ambisonics, decoded as
// its channels) and 255 (no defined layout); 3 (projection) is not taken.
bool parse_head(const uint8_t* d, size_t n, OpusHead* h) {
    if (n < 19 || std::memcmp(d, "OpusHead", 8) != 0 || (d[8] & 0xf0) != 0) return false;
    h->channels = d[9];
    h->pre_skip = le16(d + 10);
    h->gain = static_cast<int16_t>(le16(d + 16));
    h->family = d[18];
    if (h->channels < 1) return false;
    if (h->family == 0) {
        if (h->channels > 2) return false;
        h->streams = 1;
        h->coupled = h->channels - 1;
        h->mapping[0] = 0;
        h->mapping[1] = 1;
        return true;
    }
    if (h->family != 1 && h->family != 2 && h->family != 255) return false;
    if (h->family == 1 && h->channels > 8) return false;
    if (n < 21u + static_cast<unsigned>(h->channels)) return false;
    h->streams = d[19];
    h->coupled = d[20];
    if (h->streams < 1 || h->coupled > h->streams || h->streams + h->coupled > 255) return false;
    for (int i = 0; i < h->channels; ++i) {
        h->mapping[i] = d[21 + i];
        if (h->mapping[i] != 255 && h->mapping[i] >= h->streams + h->coupled) return false;
    }
    return true;
}

// Android's marked csd-0 (its OpusHeader: what MediaCodec's Opus decoder takes in one buffer):
// "AOPUSHDR" + length + OpusHead, "AOPUSDLY" + length + codec delay (ns), "AOPUSPRL" + length +
// seek pre-roll (ns); each length is 8 bytes, little-endian.
bool parse_marked(const std::vector<uint8_t>& csd, std::vector<uint8_t>* head, uint64_t* delay_ns,
                  uint64_t* preroll_ns) {
    if (csd.size() < 16 || std::memcmp(csd.data(), "AOPUSHDR", 8) != 0) return false;
    for (size_t at = 0; at + 16 <= csd.size();) {
        const uint8_t* p = csd.data() + at;
        const uint64_t len = le64(p + 8);
        if (len > csd.size() - at - 16) return false;
        const uint8_t* body = p + 16;
        if (std::memcmp(p, "AOPUSHDR", 8) == 0)
            head->assign(body, body + len);
        else if (std::memcmp(p, "AOPUSDLY", 8) == 0 && len == 8)
            *delay_ns = le64(body);
        else if (std::memcmp(p, "AOPUSPRL", 8) == 0 && len == 8)
            *preroll_ns = le64(body);
        at += 16 + static_cast<size_t>(len);
    }
    return !head->empty();
}

int samples_of_ns(uint64_t ns) {
    return static_cast<int>(std::min<uint64_t>(ns / 1000 * OpusDecoder::kSampleRate / 1000000, 5760 * 8));
}

}  // namespace

const std::vector<std::string>& video_mimes() {
    static const std::vector<std::string> mimes = {kVp8, kVp9, kAv1};
    return mimes;
}

bool decodes_video(const std::string& mime) { return mime == kVp8 || mime == kVp9 || mime == kAv1; }

std::vector<int> video_capabilities(const std::string& mime) {
    // MediaCodecInfo.CodecProfileLevel: VP8ProfileMain 1 at VP8Level_Version0 1; VP9Profile0 1
    // and VP9Profile2 4 at VP9Level5 0x400; AV1ProfileMain8 1 and AV1ProfileMain10 2 at
    // AV1Level5 0x1000 (4096x2176 at 30 frames a second, as level 5 of each format allows).
    constexpr int kBlocks4k = 4096 * 2304 / 256;
    if (mime == kVp8) return {2048, 2048, 2048 * 2048 / 256, 1, 0x1, 0x1};
    if (mime == kVp9) return {4096, 4096, kBlocks4k, 2, 0x1, 0x400, 0x4, 0x400};
    if (mime == kAv1) return {4096, 4096, kBlocks4k, 2, 0x1, 0x1000, 0x2, 0x1000};
    return {};
}

std::unique_ptr<VideoDecoder> open_video(const std::string& mime) {
    if (mime == kVp8) return VpxDecoder::open(false);
    if (mime == kVp9) return VpxDecoder::open(true);
    if (mime == kAv1) return Dav1dDecoder::open();
    return nullptr;
}

std::unique_ptr<OpusDecoder> OpusDecoder::open(const std::vector<uint8_t>& csd0, int channels,
                                               const std::vector<uint8_t>& csd1, const std::vector<uint8_t>& csd2) {
    std::vector<uint8_t> head_bytes = csd0;
    uint64_t delay_ns = UINT64_MAX, preroll_ns = UINT64_MAX;
    if (!csd0.empty() && !parse_marked(csd0, &head_bytes, &delay_ns, &preroll_ns)) head_bytes = csd0;
    if (csd1.size() == 8) delay_ns = le64(csd1.data());
    if (csd2.size() == 8) preroll_ns = le64(csd2.data());

    OpusHead h;
    if (!head_bytes.empty()) {
        if (!parse_head(head_bytes.data(), head_bytes.size(), &h)) return nullptr;
    } else {
        if (channels < 1 || channels > 2) return nullptr;
        h.channels = channels;
        h.coupled = channels - 1;
        h.mapping[1] = 1;
    }
    std::unique_ptr<OpusDecoder> d(new OpusDecoder());
    d->channels_ = h.channels;
    // A surround stream in Vorbis order: libopus writes output channel i from mapping[i], so
    // permuting the table puts the channels in Android's order at no cost.
    unsigned char mapping[255];
    std::memcpy(mapping, h.mapping, sizeof(mapping));
    if (h.family <= 1 && h.channels <= 8) {
        for (int i = 0; i < h.channels; ++i) mapping[i] = h.mapping[kVorbisToAndroid[h.channels - 1][i]];
        d->mask_ = kAndroidMask[h.channels - 1];
    }
    int err = OPUS_OK;
    OpusMSDecoder* ms = opus_multistream_decoder_create(kSampleRate, h.channels, h.streams, h.coupled, mapping, &err);
    if (!ms || err != OPUS_OK) return nullptr;
    d->decoder_ = ms;
    if (h.gain != 0) opus_multistream_decoder_ctl(ms, OPUS_SET_GAIN(h.gain));
    d->pre_skip_ = delay_ns != UINT64_MAX ? samples_of_ns(delay_ns) : h.pre_skip;
    d->preroll_ = preroll_ns != UINT64_MAX ? samples_of_ns(preroll_ns) : 3840;  // RFC 7845 4.6: 80 ms
    d->skip_ = d->pre_skip_;
    d->pcm_.resize(static_cast<size_t>(5760) * h.channels);  // 120 ms, the longest packet
    return d;
}

OpusDecoder::~OpusDecoder() {
    if (decoder_) opus_multistream_decoder_destroy(static_cast<OpusMSDecoder*>(decoder_));
}

std::vector<int16_t> OpusDecoder::decode(const uint8_t* packet, size_t size) {
    std::vector<int16_t> out;
    decode(packet, size, &out);
    return out;
}

void OpusDecoder::decode(const uint8_t* packet, size_t size, std::vector<int16_t>* out) {
    if (!packet || size == 0 || size > INT_MAX) return;
    const int n = opus_multistream_decode(static_cast<OpusMSDecoder*>(decoder_), packet, static_cast<opus_int32>(size),
                                          pcm_.data(), 5760, 0);
    if (n <= 0) return;
    const int drop = std::min(n, skip_);
    skip_ -= drop;
    out->insert(out->end(), pcm_.begin() + static_cast<ptrdiff_t>(drop) * channels_,
                pcm_.begin() + static_cast<ptrdiff_t>(n) * channels_);
}

void OpusDecoder::flush() {
    opus_multistream_decoder_ctl(static_cast<OpusMSDecoder*>(decoder_), OPUS_RESET_STATE);
    skip_ = preroll_;
}

}  // namespace winapk::video::soft
