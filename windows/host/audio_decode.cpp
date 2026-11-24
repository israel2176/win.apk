// audio_decode.cpp -- audio_engine.h's decode(): WAV, Ogg Vorbis and Ogg Opus
// here, everything else through Media Foundation.

#include "audio_engine.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>

#include "soft_codecs.h"

extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate,
                                        short** output);
// In mfplat since Windows 7; older MinGW headers do not declare it.
STDAPI MFCreateMFByteStreamOnStream(IStream* stream, IMFByteStream** out);

namespace winapk::audio {

namespace {

template <typename T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() { return p; }
    explicit operator bool() const { return p != nullptr; }
};

}  // namespace

namespace {

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// Interleaved samples of any channel count to stereo.
void append_stereo(Clip* clip, const float* in, size_t frames, int channels) {
    clip->samples.reserve(clip->samples.size() + frames * 2);
    for (size_t f = 0; f < frames; ++f) {
        const float l = in[f * channels];
        const float r = channels > 1 ? in[f * channels + 1] : l;
        clip->samples.push_back(l);
        clip->samples.push_back(r);
    }
}

std::shared_ptr<Clip> decode_wav(const uint8_t* d, size_t n) {
    if (n < 12 || std::memcmp(d, "RIFF", 4) || std::memcmp(d + 8, "WAVE", 4)) return nullptr;
    int format = 0, channels = 0, rate = 0, bits = 0;
    const uint8_t* data = nullptr;
    size_t data_len = 0;
    for (size_t at = 12; at + 8 <= n;) {
        const uint32_t len = le32(d + at + 4);
        const uint8_t* body = d + at + 8;
        const size_t avail = std::min<size_t>(len, n - at - 8);
        if (!std::memcmp(d + at, "fmt ", 4) && avail >= 16) {
            format = le16(body);
            channels = le16(body + 2);
            rate = static_cast<int>(le32(body + 4));
            bits = le16(body + 14);
            if (format == 0xFFFE && avail >= 26) format = le16(body + 24);  // WAVE_FORMAT_EXTENSIBLE's subformat
        } else if (!std::memcmp(d + at, "data", 4)) {
            data = body;
            data_len = avail;
        }
        at += 8 + len + (len & 1);
    }
    if (!data || channels <= 0 || rate <= 0 || !(format == 1 || format == 3)) return nullptr;
    const int bytes = bits / 8;
    if (bytes <= 0 || (format == 3 && bits != 32)) return nullptr;
    const size_t frames = data_len / (bytes * channels);
    std::vector<float> f(frames * channels);
    for (size_t i = 0; i < f.size(); ++i) {
        const uint8_t* s = data + i * bytes;
        switch (format == 3 ? 0 : bits) {
            case 0: std::memcpy(&f[i], s, 4); break;
            case 8: f[i] = (s[0] - 128) / 128.0f; break;
            case 16: f[i] = static_cast<int16_t>(le16(s)) / 32768.0f; break;
            case 24: {
                int32_t v = s[0] | (s[1] << 8) | (s[2] << 16);
                if (v & 0x800000) v -= 0x1000000;  // sign-extend
                f[i] = static_cast<float>(v) / 8388608.0f;
                break;
            }
            case 32: f[i] = static_cast<int32_t>(le32(s)) / 2147483648.0f; break;
            default: return nullptr;
        }
    }
    auto clip = std::make_shared<Clip>();
    clip->rate = rate;
    append_stereo(clip.get(), f.data(), frames, channels);
    return clip;
}

std::shared_ptr<Clip> decode_vorbis(const uint8_t* d, size_t n) {
    if (n < 4 || std::memcmp(d, "OggS", 4)) return nullptr;
    int channels = 0, rate = 0;
    short* pcm = nullptr;
    const int frames = stb_vorbis_decode_memory(d, static_cast<int>(n), &channels, &rate, &pcm);
    if (frames <= 0 || !pcm || channels <= 0) {
        std::free(pcm);
        return nullptr;
    }
    std::vector<float> f(static_cast<size_t>(frames) * channels);
    for (size_t i = 0; i < f.size(); ++i) f[i] = pcm[i] / 32768.0f;
    std::free(pcm);
    auto clip = std::make_shared<Clip>();
    clip->rate = rate;
    append_stereo(clip.get(), f.data(), static_cast<size_t>(frames), channels);
    return clip;
}

// Ogg Opus (RFC 7845; a ".opus" file), which Windows has no decoder for:
// libopus through soft_codecs.h. The first logical stream's packets are put
// together from the pages; the OpusHead opens the decoder (which drops its
// pre-skip) and the last page's granule position trims the end, as a player
// must.
std::shared_ptr<Clip> decode_opus(const uint8_t* d, size_t n) {
    if (n < 4 || std::memcmp(d, "OggS", 4)) return nullptr;
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> partial;
    uint32_t serial = 0;
    bool have_serial = false;
    int64_t last_granule = -1;
    for (size_t at = 0; at + 27 <= n && std::memcmp(d + at, "OggS", 4) == 0;) {
        const uint8_t* page = d + at;
        const size_t segments = page[26];
        if (at + 27 + segments > n) break;
        size_t body = 0;
        for (size_t i = 0; i < segments; ++i) body += page[27 + i];
        if (at + 27 + segments + body > n) break;
        const uint32_t this_serial = le32(page + 14);
        if (!have_serial) {
            serial = this_serial;
            have_serial = true;
        }
        if (this_serial == serial) {
            int64_t granule = 0;
            for (int i = 7; i >= 0; --i) granule = (granule << 8) | page[6 + i];
            if (granule >= 0) last_granule = granule;
            const uint8_t* p = page + 27 + segments;
            for (size_t i = 0; i < segments; ++i) {
                const size_t len = page[27 + i];
                partial.insert(partial.end(), p, p + len);
                p += len;
                if (len < 255) {  // the packet ends here; 255 continues it
                    packets.push_back(std::move(partial));
                    partial.clear();
                }
            }
        }
        at += 27 + segments + body;
    }
    if (packets.size() < 3 || packets[0].size() < 19 || std::memcmp(packets[0].data(), "OpusHead", 8)) return nullptr;
    auto dec = video::soft::OpusDecoder::open(packets[0], 0);
    if (!dec) return nullptr;
    const int channels = dec->channels();
    const int pre_skip = le16(&packets[0][10]);
    auto clip = std::make_shared<Clip>();
    clip->rate = video::soft::OpusDecoder::kSampleRate;
    std::vector<float> f;
    for (size_t i = 2; i < packets.size(); ++i) {  // [1] is OpusTags
        const std::vector<int16_t> pcm = dec->decode(packets[i].data(), packets[i].size());
        f.resize(pcm.size());
        for (size_t s = 0; s < pcm.size(); ++s) f[s] = pcm[s] / 32768.0f;
        append_stereo(clip.get(), f.data(), pcm.size() / static_cast<size_t>(channels), channels);
    }
    // The stream's true length: its final granule position, less the pre-skip.
    if (last_granule > pre_skip && static_cast<size_t>(last_granule - pre_skip) < clip->frames()) {
        clip->samples.resize(static_cast<size_t>(last_granule - pre_skip) * 2);
    }
    return clip->samples.empty() ? nullptr : clip;
}

// Media Foundation: MP3, AAC/M4A, WMA, and any codec Windows has installed.
std::shared_ptr<Clip> decode_mf(const uint8_t* d, size_t n) {
    static const bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    if (!started) return nullptr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Com<IStream> mem;
    mem.p = SHCreateMemStream(d, static_cast<UINT>(n));
    Com<IMFByteStream> bytes;
    Com<IMFSourceReader> reader;
    Com<IMFMediaType> want;
    Com<IMFMediaType> got;
    if (!mem || FAILED(MFCreateMFByteStreamOnStream(mem.p, &bytes)) ||
        FAILED(MFCreateSourceReaderFromByteStream(bytes.p, nullptr, &reader)) ||
        FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE)) ||
        FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE)) ||
        FAILED(MFCreateMediaType(&want)) || FAILED(want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
        FAILED(want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float)) ||
        FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, want.p)) ||
        FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &got))) {
        return nullptr;
    }
    UINT32 channels = 0, rate = 0;
    got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    if (!channels || !rate) return nullptr;
    auto clip = std::make_shared<Clip>();
    clip->rate = static_cast<int>(rate);
    for (;;) {
        DWORD flags = 0;
        Com<IMFSample> sample;
        if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags,
                                      nullptr, &sample))) {
            break;
        }
        if (sample) {
            Com<IMFMediaBuffer> buf;
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                append_stereo(clip.get(), reinterpret_cast<const float*>(p), len / (4 * channels), static_cast<int>(channels));
                buf->Unlock();
            }
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    return clip->samples.empty() ? nullptr : clip;
}

}  // namespace

std::shared_ptr<const Clip> decode(const uint8_t* data, size_t size) {
    if (!data || size == 0) return nullptr;
    if (auto c = decode_wav(data, size)) return c;
    if (auto c = decode_vorbis(data, size)) return c;
    if (auto c = decode_opus(data, size)) return c;
    return decode_mf(data, size);
}

}  // namespace winapk::audio
