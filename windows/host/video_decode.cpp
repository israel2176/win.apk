// video_decode.cpp -- see video_decode.h.

#include "video_decode.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <propvarutil.h>
#include <shlwapi.h>

// In mfplat since Windows 7; older MinGW headers do not declare it.
STDAPI MFCreateMFByteStreamOnStream(IStream* stream, IMFByteStream** out);

namespace winapk::video {

namespace {

template <typename T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T** operator&() { return &p; }
    T* operator->() { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Media Foundation is started once for the process; every thread that uses it
// gets COM (a HotSpot thread has none; a second init on a thread that has one
// is harmless).
bool mf_ready() {
    static const bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return started;
}

// ---- pictures ------------------------------------------------------------------------

// How an uncompressed video media type lays its pictures out.
struct Layout {
    int width = 0, height = 0;    // the coded picture
    int crop_x = 0, crop_y = 0;   // the display aperture inside it
    int crop_w = 0, crop_h = 0;
    int stride = 0;               // bytes per row, 0 = tightly packed
    bool p010 = false;
    bool bt709 = false, full_range = false;
    int rotation = 0;
};

bool read_layout(IMFMediaType* type, Layout* out) {
    UINT32 w = 0, h = 0;
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0) return false;
    GUID sub = GUID_NULL;
    type->GetGUID(MF_MT_SUBTYPE, &sub);
    out->p010 = sub == MFVideoFormat_P010;
    out->width = static_cast<int>(w);
    out->height = static_cast<int>(h);
    out->crop_x = out->crop_y = 0;
    out->crop_w = out->width;
    out->crop_h = out->height;
    // A decoder codes whole macroblocks (1088 lines for a 1080p stream) and
    // names the visible part in the aperture.
    MFVideoArea area{};
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area),
                                nullptr)) &&
        area.Area.cx > 0 && area.Area.cy > 0 && area.OffsetX.value >= 0 && area.OffsetY.value >= 0 &&
        area.OffsetX.value + area.Area.cx <= out->width && area.OffsetY.value + area.Area.cy <= out->height) {
        out->crop_x = area.OffsetX.value;
        out->crop_y = area.OffsetY.value;
        out->crop_w = area.Area.cx;
        out->crop_h = area.Area.cy;
    }
    UINT32 v = 0;
    out->stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &v)) ? static_cast<int>(static_cast<INT32>(v)) : 0;
    if (out->stride < 0) out->stride = 0;  // NV12 is never bottom-up; a negative value is a type's confusion
    // Unsaid, the matrix follows the picture size, as players do.
    out->bt709 = SUCCEEDED(type->GetUINT32(MF_MT_YUV_MATRIX, &v)) ? v == MFVideoTransferMatrix_BT709
                                                                   : out->crop_h >= 720;
    out->full_range = SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &v)) && v == MFNominalRange_0_255;
    out->rotation = SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_ROTATION, &v)) ? static_cast<int>(v) % 360 : 0;
    if (out->rotation % 90 != 0) out->rotation = 0;
    return true;
}

// A sample's picture, converted.
std::shared_ptr<Frame> frame_from_sample(IMFSample* sample, const Layout& lay) {
    Com<IMFMediaBuffer> buf;
    if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return nullptr;
    BYTE* base = nullptr;
    LONG pitch = 0;
    DWORD len = 0;
    Com<IMF2DBuffer> b2;
    const bool two_d = SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&base, &pitch));
    if (!two_d) {
        if (FAILED(buf->Lock(&base, nullptr, &len))) return nullptr;
        pitch = lay.stride > 0 ? lay.stride : lay.width * (lay.p010 ? 2 : 1);
        if (len < static_cast<DWORD>(pitch) * lay.height * 3 / 2) {
            buf->Unlock();
            return nullptr;
        }
    } else if (pitch <= 0) {
        b2->Unlock2D();
        return nullptr;
    }
    auto f = std::make_shared<Frame>();
    const bool turned = lay.rotation == 90 || lay.rotation == 270;
    f->width = turned ? lay.crop_h : lay.crop_w;
    f->height = turned ? lay.crop_w : lay.crop_h;
    f->argb.resize(static_cast<size_t>(f->width) * f->height);
    const int bpp = lay.p010 ? 2 : 1;
    const uint8_t* y = base + static_cast<size_t>(lay.crop_y) * pitch + static_cast<size_t>(lay.crop_x) * bpp;
    const uint8_t* uv = base + static_cast<size_t>(lay.height) * pitch + static_cast<size_t>(lay.crop_y / 2) * pitch +
                        static_cast<size_t>(lay.crop_x & ~1) * bpp;
    yuv420_to_argb(y, pitch, uv, pitch, lay.crop_w, lay.crop_h, lay.p010, lay.bt709, lay.full_range, lay.rotation,
                   f->argb.data());
    if (two_d) b2->Unlock2D();
    else buf->Unlock();
    LONGLONG t = 0;
    f->pts_us = SUCCEEDED(sample->GetSampleTime(&t)) ? t / 10 : 0;
    return f;
}

// ---- the mime table ---------------------------------------------------------------------

struct Mime {
    const char* mime;
    const GUID& subtype;
};

const Mime kMimes[] = {
    {"video/avc", MFVideoFormat_H264},
    {"video/hevc", MFVideoFormat_HEVC_ES},
    {"video/x-vnd.on2.vp9", MFVideoFormat_VP90},
    {"video/x-vnd.on2.vp8", MFVideoFormat_VP80},
    {"video/av01", MFVideoFormat_AV1},
    {"video/mp4v-es", MFVideoFormat_MP4V},
    {"video/3gpp", MFVideoFormat_H263},
    {"video/mpeg2", MFVideoFormat_MPEG2},
};

const Mime* mime_entry(const std::string& mime) {
    for (const Mime& m : kMimes) {
        if (mime == m.mime) return &m;
    }
    return nullptr;
}

// A synchronous decoder MFT for the subtype. Hardware decoders are
// asynchronous MFTs with an event-driven protocol of their own, so what is
// used here is the software (or DXVA-less) one Windows registers; it is
// enough for what the window shows, and it is the honest answer to
// nativeVideoIsHardware.
bool create_decoder(const GUID& category, const GUID& subtype, IMFTransform** out) {
    MFT_REGISTER_TYPE_INFO in{category == MFT_CATEGORY_AUDIO_DECODER ? MFMediaType_Audio : MFMediaType_Video, subtype};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    if (FAILED(MFTEnumEx(category, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, nullptr, &acts, &n)) ||
        !acts) {
        return false;
    }
    bool ok = false;
    for (UINT32 i = 0; i < n; ++i) {
        if (!ok && SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(out)))) ok = true;
        acts[i]->Release();
    }
    CoTaskMemFree(acts);
    return ok;
}

}  // namespace

// ---- yuv420_to_argb ----------------------------------------------------------------------

void yuv420_to_argb(const uint8_t* y, int y_stride, const uint8_t* uv, int uv_stride, int width, int height,
                    bool sixteen_bit, bool bt709, bool full_range, int rotation, uint32_t* out) {
    // 16.16 fixed point. Limited range scales Y by 255/219 and chroma by
    // 255/224 (ITU-R BT.601 / BT.709 conversion, as every player does).
    const int ys = full_range ? 65536 : 76309;
    int rv, gu, gv, bu;
    if (bt709) {
        rv = full_range ? 103206 : 117506;
        gu = full_range ? 12277 : 13959;
        gv = full_range ? 30679 : 34925;
        bu = full_range ? 121609 : 138438;
    } else {
        rv = full_range ? 91881 : 104597;
        gu = full_range ? 22554 : 25675;
        gv = full_range ? 46802 : 53279;
        bu = full_range ? 116130 : 132201;
    }
    const int ybias = full_range ? 0 : 16;
    const int out_w = (rotation == 90 || rotation == 270) ? height : width;
    auto clamp = [](int v) { return static_cast<uint32_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
    for (int row = 0; row < height; ++row) {
        const uint8_t* yr = y + static_cast<size_t>(row) * y_stride;
        const uint8_t* uvr = uv + static_cast<size_t>(row / 2) * uv_stride;
        // Where this row's pixels land, and how far apart: the four rotations
        // are one base and one step each.
        uint32_t* p;
        ptrdiff_t step;
        switch (rotation) {
            case 90: p = out + (height - 1 - row); step = out_w; break;
            case 180: p = out + static_cast<size_t>(height - 1 - row) * out_w + (width - 1); step = -1; break;
            case 270: p = out + static_cast<size_t>(width - 1) * out_w + row; step = -out_w; break;
            default: p = out + static_cast<size_t>(row) * out_w; step = 1; break;
        }
        for (int col = 0; col < width; ++col, p += step) {
            int yv, u, v;
            if (sixteen_bit) {
                yv = yr[col * 2 + 1];  // the top 8 of the 10 bits: the byte above the low one
                u = uvr[(col & ~1) * 2 + 1];
                v = uvr[(col & ~1) * 2 + 3];
            } else {
                yv = yr[col];
                u = uvr[col & ~1];
                v = uvr[(col & ~1) + 1];
            }
            const int yy = (yv - ybias) * ys;
            const int du = u - 128, dv = v - 128;
            const uint32_t r = clamp((yy + rv * dv + 32768) >> 16);
            const uint32_t g = clamp((yy - gu * du - gv * dv + 32768) >> 16);
            const uint32_t b = clamp((yy + bu * du + 32768) >> 16);
            *p = 0xff000000u | (r << 16) | (g << 8) | b;
        }
    }
}

// ---- supported_mimes ---------------------------------------------------------------------

const std::vector<std::string>& supported_mimes() {
    static const std::vector<std::string> mimes = [] {
        std::vector<std::string> found;
        if (!mf_ready()) return found;
        for (const Mime& m : kMimes) {
            Com<IMFTransform> mft;
            if (create_decoder(MFT_CATEGORY_VIDEO_DECODER, m.subtype, &mft)) found.push_back(m.mime);
        }
        return found;
    }();
    return mimes;
}

// ---- Decoder -----------------------------------------------------------------------------

struct Decoder::Impl {
    Com<IMFTransform> mft;
    Layout layout;
    std::vector<uint8_t> csd;  // sent ahead of the next access unit
    bool csd_pending = true;
    bool provides_samples = false;
    DWORD out_size = 0, out_align = 0;
    std::vector<std::shared_ptr<Frame>> ready;  // oldest first
    bool eos_marker = false;

    // The decoder's output type: NV12 first, P010 (10-bit) when that is all
    // it offers.
    bool choose_output() {
        for (int pass = 0; pass < 2; ++pass) {
            const GUID& want = pass == 0 ? MFVideoFormat_NV12 : MFVideoFormat_P010;
            for (DWORD i = 0;; ++i) {
                Com<IMFMediaType> t;
                if (FAILED(mft->GetOutputAvailableType(0, i, &t))) break;
                GUID sub = GUID_NULL;
                if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub)) || sub != want) continue;
                if (FAILED(mft->SetOutputType(0, t.p, 0))) continue;
                Com<IMFMediaType> cur;
                if (FAILED(mft->GetOutputCurrentType(0, &cur)) || !read_layout(cur.p, &layout)) return false;
                MFT_OUTPUT_STREAM_INFO info{};
                if (FAILED(mft->GetOutputStreamInfo(0, &info))) return false;
                provides_samples =
                    (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
                out_size = info.cbSize;
                out_align = info.cbAlignment;
                return true;
            }
        }
        return false;
    }

    // Everything the decoder has ready.
    void drain() {
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER ob{};
            Com<IMFSample> own;
            if (!provides_samples) {
                Com<IMFMediaBuffer> mb;
                if (FAILED(MFCreateSample(&own)) ||
                    FAILED(MFCreateAlignedMemoryBuffer(out_size, out_align ? out_align : MF_16_BYTE_ALIGNMENT, &mb)) ||
                    FAILED(own->AddBuffer(mb.p))) {
                    return;
                }
                ob.pSample = own.p;
            }
            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                // A new resolution (or the first real one): renegotiate and go on.
                if (ob.pSample && !own) ob.pSample->Release();
                if (!choose_output()) return;
                continue;
            }
            if (FAILED(hr)) {  // MF_E_TRANSFORM_NEED_MORE_INPUT, or a real error
                if (ob.pSample && !own) ob.pSample->Release();
                return;
            }
            if (ob.pSample) {
                if (auto f = frame_from_sample(ob.pSample, layout)) ready.push_back(std::move(f));
                if (!own) ob.pSample->Release();
            }
        }
    }
};

Decoder::Decoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Decoder::~Decoder() = default;

std::unique_ptr<Decoder> Decoder::open(const std::string& mime, int width, int height, const std::vector<uint8_t>& csd0,
                                       const std::vector<uint8_t>& csd1) {
    const Mime* m = mime_entry(mime);
    if (!m || !mf_ready()) return nullptr;
    auto impl = std::make_unique<Impl>();
    if (!create_decoder(MFT_CATEGORY_VIDEO_DECODER, m->subtype, &impl->mft)) return nullptr;
    Com<IMFMediaType> in;
    if (FAILED(MFCreateMediaType(&in)) || FAILED(in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(in->SetGUID(MF_MT_SUBTYPE, m->subtype))) {
        return nullptr;
    }
    if (width > 0 && height > 0) MFSetAttributeSize(in.p, MF_MT_FRAME_SIZE, width, height);
    impl->csd = csd0;
    impl->csd.insert(impl->csd.end(), csd1.begin(), csd1.end());
    if (!impl->csd.empty()) in->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, impl->csd.data(), static_cast<UINT32>(impl->csd.size()));
    if (FAILED(impl->mft->SetInputType(0, in.p, 0)) || !impl->choose_output()) return nullptr;
    impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return std::unique_ptr<Decoder>(new Decoder(std::move(impl)));
}

bool Decoder::decode(const uint8_t* au, size_t size, int64_t pts_us, bool eos) {
    Impl& d = *impl_;
    if (au && size) {
        // The parameter sets go in-band ahead of the first access unit (and
        // again after a flush), which every Annex-B decoder takes; for the
        // codecs without any, csd is empty.
        const size_t head = d.csd_pending ? d.csd.size() : 0;
        Com<IMFSample> sample;
        Com<IMFMediaBuffer> mb;
        BYTE* p = nullptr;
        if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(head + size), &mb)) ||
            FAILED(mb->Lock(&p, nullptr, nullptr))) {
            return false;
        }
        if (head) std::memcpy(p, d.csd.data(), head);
        std::memcpy(p + head, au, size);
        mb->Unlock();
        mb->SetCurrentLength(static_cast<DWORD>(head + size));
        sample->AddBuffer(mb.p);
        sample->SetSampleTime(pts_us * 10);
        d.csd_pending = false;
        HRESULT hr = d.mft->ProcessInput(0, sample.p, 0);
        for (int tries = 0; hr == MF_E_NOTACCEPTING && tries < 8; ++tries) {
            d.drain();  // full: make room, then offer it again
            hr = d.mft->ProcessInput(0, sample.p, 0);
        }
        if (FAILED(hr)) return false;
        d.drain();
    }
    if (eos) {
        d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        d.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        d.drain();
        d.eos_marker = true;
    }
    return true;
}

std::shared_ptr<Frame> Decoder::poll(bool* eos) {
    Impl& d = *impl_;
    *eos = false;
    if (!d.ready.empty()) {
        std::shared_ptr<Frame> f = std::move(d.ready.front());
        d.ready.erase(d.ready.begin());
        return f;
    }
    if (d.eos_marker) {
        d.eos_marker = false;
        *eos = true;
    }
    return nullptr;
}

void Decoder::flush() {
    Impl& d = *impl_;
    d.mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    d.ready.clear();
    d.eos_marker = false;
    d.csd_pending = true;
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}

int Decoder::width() const {
    const Layout& l = impl_->layout;
    return (l.rotation == 90 || l.rotation == 270) ? l.crop_h : l.crop_w;
}
int Decoder::height() const {
    const Layout& l = impl_->layout;
    return (l.rotation == 90 || l.rotation == 270) ? l.crop_w : l.crop_h;
}

// ---- AudioDecoder ------------------------------------------------------------------------

struct AudioDecoder::Impl {
    Com<IMFTransform> mft;
    int rate = 0, channels = 0;
    bool provides_samples = false;
    DWORD out_size = 0, out_align = 0;

    bool choose_output() {
        for (DWORD i = 0;; ++i) {
            Com<IMFMediaType> t;
            if (FAILED(mft->GetOutputAvailableType(0, i, &t))) return false;
            GUID sub = GUID_NULL;
            UINT32 bits = 0;
            if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub)) || sub != MFAudioFormat_PCM ||
                FAILED(t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)) || bits != 16 ||
                FAILED(mft->SetOutputType(0, t.p, 0))) {
                continue;
            }
            UINT32 r = 0, c = 0;
            t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r);
            t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &c);
            rate = static_cast<int>(r);
            channels = static_cast<int>(c);
            MFT_OUTPUT_STREAM_INFO info{};
            if (FAILED(mft->GetOutputStreamInfo(0, &info))) return false;
            provides_samples =
                (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            out_size = info.cbSize ? info.cbSize : 65536;
            out_align = info.cbAlignment;
            return rate > 0 && channels > 0;
        }
    }

    void drain(std::vector<int16_t>* out) {
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER ob{};
            Com<IMFSample> own;
            if (!provides_samples) {
                Com<IMFMediaBuffer> mb;
                if (FAILED(MFCreateSample(&own)) ||
                    FAILED(MFCreateAlignedMemoryBuffer(out_size, out_align ? out_align : MF_16_BYTE_ALIGNMENT, &mb)) ||
                    FAILED(own->AddBuffer(mb.p))) {
                    return;
                }
                ob.pSample = own.p;
            }
            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (ob.pSample && !own) ob.pSample->Release();
                if (!choose_output()) return;
                continue;
            }
            if (FAILED(hr)) {
                if (ob.pSample && !own) ob.pSample->Release();
                return;
            }
            if (ob.pSample) {
                Com<IMFMediaBuffer> buf;
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(ob.pSample->ConvertToContiguousBuffer(&buf)) && SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                    const size_t n = len / 2;
                    const size_t at = out->size();
                    out->resize(at + n);
                    std::memcpy(out->data() + at, p, n * 2);
                    buf->Unlock();
                }
                if (!own) ob.pSample->Release();
            }
        }
    }
};

AudioDecoder::AudioDecoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AudioDecoder::~AudioDecoder() = default;

std::unique_ptr<AudioDecoder> AudioDecoder::open(int format_id, int sample_rate, int channels,
                                                 const std::vector<uint8_t>& csd0) {
    if ((format_id != 1 && format_id != 2) || sample_rate <= 0 || channels <= 0 || !mf_ready()) return nullptr;
    const GUID& subtype = format_id == 1 ? MFAudioFormat_AAC : MFAudioFormat_MP3;
    auto impl = std::make_unique<Impl>();
    if (!create_decoder(MFT_CATEGORY_AUDIO_DECODER, subtype, &impl->mft)) return nullptr;
    Com<IMFMediaType> in;
    if (FAILED(MFCreateMediaType(&in)) || FAILED(in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
        FAILED(in->SetGUID(MF_MT_SUBTYPE, subtype)) ||
        FAILED(in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(sample_rate))) ||
        FAILED(in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(channels)))) {
        return nullptr;
    }
    if (format_id == 1) {
        // Raw AAC access units (no ADTS). The AAC decoder reads the stream's
        // AudioSpecificConfig from MF_MT_USER_DATA after the 12 bytes of
        // HEAACWAVEINFO that follow its WAVEFORMATEX: payload type, profile
        // level indication, structure type, two reserved fields.
        std::vector<uint8_t> user(12, 0);
        user[2] = 0xfe;  // wAudioProfileLevelIndication: unknown
        user.insert(user.end(), csd0.begin(), csd0.end());
        in->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        in->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0xfe);
        in->SetBlob(MF_MT_USER_DATA, user.data(), static_cast<UINT32>(user.size()));
    }
    if (FAILED(impl->mft->SetInputType(0, in.p, 0)) || !impl->choose_output()) return nullptr;
    impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return std::unique_ptr<AudioDecoder>(new AudioDecoder(std::move(impl)));
}

std::vector<int16_t> AudioDecoder::decode(const uint8_t* au, size_t size, bool eos) {
    Impl& d = *impl_;
    std::vector<int16_t> pcm;
    if (au && size) {
        Com<IMFSample> sample;
        Com<IMFMediaBuffer> mb;
        BYTE* p = nullptr;
        if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &mb)) ||
            FAILED(mb->Lock(&p, nullptr, nullptr))) {
            return pcm;
        }
        std::memcpy(p, au, size);
        mb->Unlock();
        mb->SetCurrentLength(static_cast<DWORD>(size));
        sample->AddBuffer(mb.p);
        HRESULT hr = d.mft->ProcessInput(0, sample.p, 0);
        for (int tries = 0; hr == MF_E_NOTACCEPTING && tries < 8; ++tries) {
            d.drain(&pcm);
            hr = d.mft->ProcessInput(0, sample.p, 0);
        }
        d.drain(&pcm);
    }
    if (eos) {
        d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        d.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        d.drain(&pcm);
    }
    return pcm;
}

int AudioDecoder::sample_rate() const { return impl_->rate; }
int AudioDecoder::channels() const { return impl_->channels; }

// ---- Source ------------------------------------------------------------------------------

struct Source::Impl {
    Com<IMFSourceReader> reader;
    Com<IStream> memory;  // keeps a memory source's bytes alive
    std::vector<uint8_t> bytes;
    bool decode_video = true;
    bool video = false, audio = false;
    bool video_done = false, audio_done = false;
    Layout layout;
    std::string mime;
    std::vector<uint8_t> csd;
    int rate = 0, channels = 0;
    int64_t duration_us = 0;

    static constexpr DWORD kVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    static constexpr DWORD kAudio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

    bool setup() {
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        Com<IMFMediaType> native;
        if (SUCCEEDED(reader->GetNativeMediaType(kVideo, 0, &native))) {
            if (decode_video) {
                // NV12, the decoders' own output; P010 for 10-bit streams
                // whose decoder offers nothing else.
                for (int pass = 0; pass < 2 && !video; ++pass) {
                    Com<IMFMediaType> want;
                    if (FAILED(MFCreateMediaType(&want)) || FAILED(want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
                        FAILED(want->SetGUID(MF_MT_SUBTYPE, pass == 0 ? MFVideoFormat_NV12 : MFVideoFormat_P010))) {
                        break;
                    }
                    video = SUCCEEDED(reader->SetCurrentMediaType(kVideo, nullptr, want.p)) && read_video_type();
                }
            } else {
                GUID sub = GUID_NULL;
                native->GetGUID(MF_MT_SUBTYPE, &sub);
                for (const Mime& m : kMimes) {
                    if (m.subtype == sub) mime = m.mime;
                }
                UINT32 n = 0;
                if (!mime.empty() && SUCCEEDED(native->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &n)) && n) {
                    csd.resize(n);
                    native->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, csd.data(), n, nullptr);
                }
                video = !mime.empty() && read_layout(native.p, &layout);
            }
            if (video) reader->SetStreamSelection(kVideo, TRUE);
        }
        Com<IMFMediaType> want, got;
        if (SUCCEEDED(MFCreateMediaType(&want)) && SUCCEEDED(want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) &&
            SUCCEEDED(want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float)) &&
            SUCCEEDED(reader->SetCurrentMediaType(kAudio, nullptr, want.p)) &&
            SUCCEEDED(reader->GetCurrentMediaType(kAudio, &got))) {
            UINT32 r = 0, c = 0;
            got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r);
            got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &c);
            rate = static_cast<int>(r);
            channels = static_cast<int>(c);
            audio = rate > 0 && channels > 0 && SUCCEEDED(reader->SetStreamSelection(kAudio, TRUE));
        }
        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION,
                                                       &var)) &&
            var.vt == VT_UI8) {
            duration_us = static_cast<int64_t>(var.uhVal.QuadPart / 10);
        }
        PropVariantClear(&var);
        video_done = !video;
        audio_done = !audio;
        find_indices();
        return video || audio;
    }

    bool read_video_type() {
        Com<IMFMediaType> cur;
        return SUCCEEDED(reader->GetCurrentMediaType(kVideo, &cur)) && read_layout(cur.p, &layout);
    }

    // One sample of `stream` (ANY for whichever comes first) into `out`.
    bool read(DWORD stream, Packet* out) {
        for (;;) {
            if (video_done && audio_done) {
                out->kind = Packet::kEnd;
                return true;
            }
            if (stream == kVideo && video_done) {
                out->kind = Packet::kEnd;
                return true;
            }
            if (stream == kAudio && audio_done) {
                out->kind = Packet::kEnd;
                return true;
            }
            DWORD index = 0, flags = 0;
            LONGLONG ts = 0;
            Com<IMFSample> sample;
            if (FAILED(reader->ReadSample(stream, 0, &index, &flags, &ts, &sample))) return false;
            if (flags & MF_SOURCE_READERF_ERROR) return false;
            const bool is_video = video && index == video_index;
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                if (is_video && decode_video) read_video_type();
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                if (is_video) video_done = true;
                else audio_done = true;
            }
            if (!sample) continue;
            out->pts_us = ts / 10;
            if (is_video) {
                out->kind = Packet::kVideo;
                if (decode_video) {
                    out->frame = frame_from_sample(sample.p, layout);
                    if (!out->frame) continue;
                    out->frame->pts_us = out->pts_us;
                } else {
                    Com<IMFMediaBuffer> buf;
                    BYTE* p = nullptr;
                    DWORD len = 0;
                    if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || FAILED(buf->Lock(&p, nullptr, &len))) continue;
                    out->data.assign(p, p + len);
                    buf->Unlock();
                }
                return true;
            }
            out->kind = Packet::kAudio;
            Com<IMFMediaBuffer> buf;
            BYTE* p = nullptr;
            DWORD len = 0;
            if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || FAILED(buf->Lock(&p, nullptr, &len))) continue;
            out->pcm.assign(reinterpret_cast<const float*>(p), reinterpret_cast<const float*>(p) + len / 4);
            buf->Unlock();
            return true;
        }
    }

    // The reader numbers streams as the file does; ReadSample reports that
    // number, not the FIRST_* alias it was asked for.
    DWORD video_index = 0xffffffffu, audio_index = 0xffffffffu;
    void find_indices() {
        for (DWORD i = 0;; ++i) {
            Com<IMFMediaType> t;
            if (FAILED(reader->GetNativeMediaType(i, 0, &t))) break;
            GUID major = GUID_NULL;
            t->GetGUID(MF_MT_MAJOR_TYPE, &major);
            if (major == MFMediaType_Video && video_index == 0xffffffffu) video_index = i;
            if (major == MFMediaType_Audio && audio_index == 0xffffffffu) audio_index = i;
        }
    }
};

Source::Source(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Source::~Source() = default;

std::unique_ptr<Source> Source::open(const std::wstring& path_or_url, bool decode_video) {
    if (!mf_ready()) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->decode_video = decode_video;
    Com<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 2))) return nullptr;
    // The colour converter and deinterlacer the reader inserts when a stream's
    // decoder cannot give NV12 itself.
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (FAILED(MFCreateSourceReaderFromURL(path_or_url.c_str(), attrs.p, &impl->reader)) || !impl->setup()) {
        return nullptr;
    }
    return std::unique_ptr<Source>(new Source(std::move(impl)));
}

std::unique_ptr<Source> Source::open(std::vector<uint8_t> bytes, bool decode_video) {
    if (bytes.empty() || !mf_ready()) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->decode_video = decode_video;
    impl->bytes = std::move(bytes);
    impl->memory.p = SHCreateMemStream(impl->bytes.data(), static_cast<UINT>(impl->bytes.size()));
    Com<IMFByteStream> stream;
    Com<IMFAttributes> attrs;
    if (!impl->memory || FAILED(MFCreateMFByteStreamOnStream(impl->memory.p, &stream)) ||
        FAILED(MFCreateAttributes(&attrs, 2))) {
        return nullptr;
    }
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (FAILED(MFCreateSourceReaderFromByteStream(stream.p, attrs.p, &impl->reader)) || !impl->setup()) return nullptr;
    return std::unique_ptr<Source>(new Source(std::move(impl)));
}

bool Source::has_video() const { return impl_->video; }
bool Source::has_audio() const { return impl_->audio; }
int Source::width() const {
    const Layout& l = impl_->layout;
    return (l.rotation == 90 || l.rotation == 270) ? l.crop_h : l.crop_w;
}
int Source::height() const {
    const Layout& l = impl_->layout;
    return (l.rotation == 90 || l.rotation == 270) ? l.crop_w : l.crop_h;
}
int Source::rotation() const { return impl_->layout.rotation; }
int64_t Source::duration_us() const { return impl_->duration_us; }
int Source::audio_rate() const { return impl_->rate; }
int Source::audio_channels() const { return impl_->channels; }
const std::string& Source::video_mime() const { return impl_->mime; }
const std::vector<uint8_t>& Source::video_csd() const { return impl_->csd; }

bool Source::read(Packet* out) { return impl_->read(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), out); }
bool Source::read_video(Packet* out) { return impl_->read(Impl::kVideo, out); }
bool Source::read_audio(Packet* out) { return impl_->read(Impl::kAudio, out); }

bool Source::seek(int64_t us) {
    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = us < 0 ? 0 : us * 10;
    const bool ok = SUCCEEDED(impl_->reader->SetCurrentPosition(GUID_NULL, var));
    PropVariantClear(&var);
    if (ok) {
        impl_->video_done = !impl_->video;
        impl_->audio_done = !impl_->audio;
    }
    return ok;
}

}  // namespace winapk::video
