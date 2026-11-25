// webm_demux.cpp -- WebM and Matroska through libwebm's mkvparser (webm_demux.h): mkvparser.h's
// Segment, Tracks, Cluster, BlockEntry and Cues, read through an IMkvReader over the file.

#include "webm_demux.h"

#include <mkvparser/mkvparser.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace winapk::video {

namespace {

// A sample larger than this is taken for damage, not read.
constexpr long kMaxFrame = 64L << 20;

FILE* open_file(const std::wstring& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"rb");
#else
    std::string narrow;
    for (wchar_t c : path) {  // tests on POSIX pass ASCII paths; UTF-8 for the rest
        if (c < 0x80)
            narrow += static_cast<char>(c);
        else if (c < 0x800)
            narrow += static_cast<char>(0xc0 | c >> 6), narrow += static_cast<char>(0x80 | (c & 0x3f));
        else {
            narrow += static_cast<char>(0xe0 | ((c >> 12) & 0x0f));
            narrow += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
            narrow += static_cast<char>(0x80 | (c & 0x3f));
        }
    }
    return std::fopen(narrow.c_str(), "rb");
#endif
}

bool seek_file(FILE* f, long long pos) {
#ifdef _WIN32
    return _fseeki64(f, pos, SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(pos), SEEK_SET) == 0;
#endif
}

// mkvparser's view of the file: reads at a position, and the file's length.
class FileReader final : public mkvparser::IMkvReader {
public:
    explicit FileReader(FILE* f) : file_(f) {
        if (seek_file(f, 0) && std::fseek(f, 0, SEEK_END) == 0) {
#ifdef _WIN32
            length_ = _ftelli64(f);
#else
            length_ = ftello(f);
#endif
        }
    }
    ~FileReader() override {
        if (file_) std::fclose(file_);
    }
    int Read(long long pos, long len, unsigned char* buf) override {
        if (pos < 0 || len < 0 || pos > length_ || len > length_ - pos) return -1;
        if (len == 0) return 0;
        if (pos != at_ && !seek_file(file_, pos)) return -1;
        const size_t got = std::fread(buf, 1, static_cast<size_t>(len), file_);
        at_ = pos + static_cast<long long>(got);
        return got == static_cast<size_t>(len) ? 0 : -1;
    }
    int Length(long long* total, long long* available) override {
        if (length_ < 0) return -1;
        if (total) *total = length_;
        if (available) *available = length_;
        return 0;
    }

private:
    FILE* file_;
    long long length_ = -1;
    long long at_ = -1;
};

// Matroska's Colour element (its values are ISO/IEC 23091-2's code points, as AV1's) in
// MediaFormat's terms, as Android's MatroskaExtractor maps them.
ColorAspects colour_of(const mkvparser::Colour* c) {
    ColorAspects a;
    if (!c) return a;
    const long long none = mkvparser::Colour::kValueNotPresent;
    switch (c->matrix_coefficients) {
        case 1: a.standard = ColorAspects::kBt709; break;
        case 5: a.standard = ColorAspects::kBt601Pal; break;
        case 6: a.standard = ColorAspects::kBt601Ntsc; break;
        case 9:
        case 10: a.standard = ColorAspects::kBt2020; break;
        default:
            if (c->primaries == 1)
                a.standard = ColorAspects::kBt709;
            else if (c->primaries == 5)
                a.standard = ColorAspects::kBt601Pal;
            else if (c->primaries == 6)
                a.standard = ColorAspects::kBt601Ntsc;
            else if (c->primaries == 9)
                a.standard = ColorAspects::kBt2020;
            break;
    }
    switch (c->transfer_characteristics) {
        case 1:
        case 6:
        case 14:
        case 15: a.transfer = ColorAspects::kSdrVideo; break;
        case 16: a.transfer = ColorAspects::kSt2084; break;
        case 18: a.transfer = ColorAspects::kHlg; break;
        case 8: a.transfer = ColorAspects::kLinear; break;
        default: break;
    }
    if (c->range == 1) a.range = ColorAspects::kLimited;
    if (c->range == 2) a.range = ColorAspects::kFull;
    const mkvparser::MasteringMetadata* m = c->mastering_metadata;
    const bool light = (c->max_cll != none && c->max_cll > 0) || (c->max_fall != none && c->max_fall > 0);
    if (m || light) {
        uint16_t v[12] = {};
        auto u16 = [](double x) { return static_cast<uint16_t>(std::clamp(std::lround(x), 0L, 65535L)); };
        if (m) {
            const mkvparser::PrimaryChromaticity* p[4] = {m->r, m->g, m->b, m->white_point};
            for (int i = 0; i < 4; ++i) {
                if (!p[i]) continue;
                v[2 * i] = u16(p[i]->x * 50000.0);  // 0.00002 units
                v[2 * i + 1] = u16(p[i]->y * 50000.0);
            }
            if (m->luminance_max != mkvparser::MasteringMetadata::kValueNotPresent) v[8] = u16(m->luminance_max);
            if (m->luminance_min != mkvparser::MasteringMetadata::kValueNotPresent) {
                v[9] = u16(m->luminance_min * 10000.0);  // 0.0001 cd/m2
            }
        }
        if (c->max_cll != none) v[10] = u16(static_cast<double>(c->max_cll));
        if (c->max_fall != none) v[11] = u16(static_cast<double>(c->max_fall));
        a.hdr_static_info.assign(25, 0);
        for (int i = 0; i < 12; ++i) {
            a.hdr_static_info[1 + 2 * i] = static_cast<uint8_t>(v[i] & 0xff);
            a.hdr_static_info[2 + 2 * i] = static_cast<uint8_t>(v[i] >> 8);
        }
    }
    return a;
}

void put_le64(std::vector<uint8_t>* out, uint64_t v) {
    out->resize(8);
    for (int i = 0; i < 8; ++i) (*out)[static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
}

const uint8_t kStartCode[4] = {0, 0, 0, 1};

void append_nal(std::vector<uint8_t>* out, const uint8_t* nal, size_t n) {
    out->insert(out->end(), kStartCode, kStartCode + 4);
    out->insert(out->end(), nal, nal + n);
}

// An AVCDecoderConfigurationRecord (ISO/IEC 14496-15 5.3.3.1): its SPS into csd0 and PPS into
// csd1 with start codes; the NAL length size. False when malformed.
bool parse_avcc(const uint8_t* d, size_t n, std::vector<uint8_t>* csd0, std::vector<uint8_t>* csd1, int* nal_len) {
    if (n < 7 || d[0] != 1) return false;
    *nal_len = (d[4] & 3) + 1;
    size_t at = 5;
    for (int list = 0; list < 2; ++list) {
        if (at >= n) return false;
        const int count = list == 0 ? d[at] & 0x1f : d[at];
        ++at;
        for (int i = 0; i < count; ++i) {
            if (at + 2 > n) return false;
            const size_t len = static_cast<size_t>(d[at] << 8 | d[at + 1]);
            at += 2;
            if (len > n - at) return false;
            append_nal(list == 0 ? csd0 : csd1, d + at, len);
            at += len;
        }
    }
    return !csd0->empty();
}

// An HEVCDecoderConfigurationRecord (ISO/IEC 14496-15 8.3.3.1): every NAL unit of its arrays into
// csd0 with start codes; the NAL length size.
bool parse_hvcc(const uint8_t* d, size_t n, std::vector<uint8_t>* csd0, int* nal_len) {
    if (n < 23) return false;
    *nal_len = (d[21] & 3) + 1;
    const int arrays = d[22];
    size_t at = 23;
    for (int a = 0; a < arrays; ++a) {
        if (at + 3 > n) return false;
        const int count = d[at + 1] << 8 | d[at + 2];
        at += 3;
        for (int i = 0; i < count; ++i) {
            if (at + 2 > n) return false;
            const size_t len = static_cast<size_t>(d[at] << 8 | d[at + 1]);
            at += 2;
            if (len > n - at) return false;
            append_nal(csd0, d + at, len);
            at += len;
        }
    }
    return !csd0->empty();
}

// Vorbis' CodecPrivate: its three headers, Xiph-laced. The identification header (csd0) and the
// setup header (csd1); the comment header between them is not wanted.
bool parse_vorbis(const uint8_t* d, size_t n, std::vector<uint8_t>* csd0, std::vector<uint8_t>* csd1) {
    if (n < 3 || d[0] != 2) return false;
    size_t at = 1, len[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        while (at < n && d[at] == 255) len[i] += 255, ++at;
        if (at >= n) return false;
        len[i] += d[at++];
    }
    if (len[0] + len[1] > n - at) return false;
    csd0->assign(d + at, d + at + len[0]);
    const size_t setup = at + len[0] + len[1];
    csd1->assign(d + setup, d + n);
    return !csd0->empty() && !csd1->empty();
}

}  // namespace

struct WebmReader::Impl {
    struct Source {
        const mkvparser::Track* track = nullptr;
        bool selected = false;
        int nal_len = 0;             // H.264/HEVC: length-prefixed NAL units to Annex-B
        std::vector<uint8_t> strip;  // Matroska's header stripping: bytes each frame lost
    };

    std::unique_ptr<FileReader> reader;
    std::unique_ptr<mkvparser::Segment> segment;
    std::vector<Demuxer::Track> tracks;
    std::vector<Source> sources;  // beside tracks
    int64_t duration_us = 0;

    // The read position: a block of a cluster, and the next of its frames.
    const mkvparser::Cluster* cluster = nullptr;
    const mkvparser::BlockEntry* entry = nullptr;
    int frame = 0;
    bool started = false, done = false;
    std::vector<uint8_t> raw;  // one frame as stored, reused

    int index_of(long long number) const {
        for (size_t i = 0; i < sources.size(); ++i) {
            if (sources[i].track->GetNumber() == number) return static_cast<int>(i);
        }
        return -1;
    }

    // The first block entry, or the next one, across clusters. False at the end.
    bool advance() {
        frame = 0;
        if (!started) {
            started = true;
            cluster = segment->GetFirst();
            entry = nullptr;
        }
        while (cluster && !cluster->EOS()) {
            const mkvparser::BlockEntry* next = nullptr;
            const long status = entry ? cluster->GetNext(entry, next) : cluster->GetFirst(next);
            if (status < 0) return false;  // damage: what came before stands
            if (next && !next->EOS()) {
                entry = next;
                return true;
            }
            cluster = segment->GetNext(cluster);
            entry = nullptr;
        }
        return false;
    }

    bool read_frame(const mkvparser::Block* block, const Source& src, Demuxer::Sample* out) {
        const mkvparser::Block::Frame& f = block->GetFrame(frame);
        if (f.len <= 0 || f.len > kMaxFrame) return false;
        raw.resize(static_cast<size_t>(f.len));
        if (f.Read(reader.get(), raw.data()) < 0) return false;
        out->data.clear();
        if (src.nal_len == 0) {
            out->data.reserve(src.strip.size() + raw.size());
            out->data.assign(src.strip.begin(), src.strip.end());
            out->data.insert(out->data.end(), raw.begin(), raw.end());
            return true;
        }
        // Length-prefixed NAL units to start codes.
        std::vector<uint8_t> whole(src.strip);
        whole.insert(whole.end(), raw.begin(), raw.end());
        for (size_t at = 0; at + static_cast<size_t>(src.nal_len) <= whole.size();) {
            size_t len = 0;
            for (int i = 0; i < src.nal_len; ++i) len = len << 8 | whole[at + static_cast<size_t>(i)];
            at += static_cast<size_t>(src.nal_len);
            if (len > whole.size() - at) return false;
            append_nal(&out->data, whole.data() + at, len);
            at += len;
        }
        return true;
    }

    // Where reading starts for a seek to `ns` on `ref` (an index into sources): the last key
    // block of that track at or before the time, searched from `from` (a Cues hit, or the
    // cluster holding the time) on, and from the clusters before it when that one has none.
    bool position_at(int64_t ns, int ref, const mkvparser::Cluster* from) {
        std::vector<const mkvparser::Cluster*> before;  // clusters up to `from`, for stepping back
        for (const mkvparser::Cluster* c = segment->GetFirst(); c && !c->EOS(); c = segment->GetNext(c)) {
            before.push_back(c);
            if (c == from) break;
        }
        if (before.empty() || before.back() != from) return false;
        const long long number = sources[static_cast<size_t>(ref)].track->GetNumber();
        for (size_t start = before.size(); start-- > 0;) {
            const mkvparser::Cluster* best_cluster = nullptr;
            const mkvparser::BlockEntry* best_entry = nullptr;
            bool past = false;
            for (const mkvparser::Cluster* c = before[start]; c && !c->EOS() && !past; c = segment->GetNext(c)) {
                const mkvparser::BlockEntry* e = nullptr;
                if (c->GetFirst(e) < 0) break;
                while (e && !e->EOS()) {
                    const mkvparser::Block* b = e->GetBlock();
                    if (b->GetTrackNumber() == number) {
                        if (b->GetTime(c) > ns) {
                            past = true;
                            break;
                        }
                        if (b->IsKey()) best_cluster = c, best_entry = e;
                    }
                    if (c->GetNext(e, e) < 0) break;
                }
            }
            if (best_entry) {
                cluster = best_cluster;
                entry = best_entry;  // next() reads this block's frames first
                frame = 0;
                started = true;
                done = false;
                return true;
            }
        }
        return false;
    }
};

WebmReader::WebmReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WebmReader::~WebmReader() = default;

bool WebmReader::is_ebml(const std::wstring& path) {
    FILE* f = open_file(path);
    if (!f) return false;
    unsigned char magic[4] = {};
    const bool ok =
        std::fread(magic, 1, 4, f) == 4 && magic[0] == 0x1a && magic[1] == 0x45 && magic[2] == 0xdf && magic[3] == 0xa3;
    std::fclose(f);
    return ok;
}

std::unique_ptr<WebmReader> WebmReader::open(const std::wstring& path) {
    FILE* f = open_file(path);
    if (!f) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->reader = std::make_unique<FileReader>(f);
    long long pos = 0;
    mkvparser::EBMLHeader header;
    if (header.Parse(impl->reader.get(), pos) < 0 || !header.m_docType ||
        (std::strcmp(header.m_docType, "webm") != 0 && std::strcmp(header.m_docType, "matroska") != 0)) {
        return nullptr;
    }
    mkvparser::Segment* segment = nullptr;
    if (mkvparser::Segment::CreateInstance(impl->reader.get(), pos, segment) != 0 || !segment) return nullptr;
    impl->segment.reset(segment);
    // Every cluster's place (not its blocks, read as they are reached): what seeking without
    // Cues needs. A cut file loads up to its cut.
    if (segment->Load() < 0 && segment->GetCount() == 0) return nullptr;
    if (const mkvparser::SegmentInfo* info = segment->GetInfo())
        impl->duration_us = std::max(0LL, info->GetDuration() / 1000);

    const mkvparser::Tracks* tracks = segment->GetTracks();
    if (!tracks) return nullptr;
    for (unsigned long i = 0; i < tracks->GetTracksCount(); ++i) {
        const mkvparser::Track* t = tracks->GetTrackByIndex(i);
        if (!t || (t->GetType() != mkvparser::Track::kVideo && t->GetType() != mkvparser::Track::kAudio)) continue;
        Demuxer::Track out;
        Impl::Source src;
        src.track = t;
        out.video = t->GetType() == mkvparser::Track::kVideo;
        const std::string codec = t->GetCodecId() ? t->GetCodecId() : "";
        size_t private_size = 0;
        const uint8_t* priv = t->GetCodecPrivate(private_size);
        std::vector<uint8_t> codec_private;
        if (priv && private_size) codec_private.assign(priv, priv + private_size);
        bool usable = true;
        // Header stripping is the one content encoding read here; compressed or encrypted
        // tracks are listed, unnamed.
        for (unsigned long e = 0; e < t->GetContentEncodingCount(); ++e) {
            const mkvparser::ContentEncoding* enc = t->GetContentEncodingByIndex(e);
            if (!enc) continue;
            if (enc->GetEncryptionCount() > 0) usable = false;
            for (unsigned long c = 0; c < enc->GetCompressionCount(); ++c) {
                const mkvparser::ContentEncoding::ContentCompression* comp = enc->GetCompressionByIndex(c);
                if (comp && comp->algo == 3 && comp->settings && comp->settings_len > 0) {
                    src.strip.assign(comp->settings, comp->settings + comp->settings_len);
                } else {
                    usable = false;
                }
            }
        }
        if (out.video) {
            const auto* v = static_cast<const mkvparser::VideoTrack*>(t);
            out.width = static_cast<int>(std::clamp(v->GetWidth(), 0LL, 65535LL));
            out.height = static_cast<int>(std::clamp(v->GetHeight(), 0LL, 65535LL));
            out.frame_rate = v->GetFrameRate();
            if (out.frame_rate <= 0 && t->GetDefaultDuration() > 0)
                out.frame_rate = 1e9 / static_cast<double>(t->GetDefaultDuration());
            out.color = colour_of(v->GetColour());
            if (!usable)
                out.mime = "video/x-unknown";
            else if (codec == "V_VP8")
                out.mime = "video/x-vnd.on2.vp8";
            else if (codec == "V_VP9")
                out.mime = "video/x-vnd.on2.vp9";
            else if (codec == "V_AV1")
                out.mime = "video/av01";
            else if (codec == "V_MPEG4/ISO/AVC" &&
                     parse_avcc(codec_private.data(), codec_private.size(), &out.csd0, &out.csd1, &src.nal_len)) {
                out.mime = "video/avc";
                codec_private.clear();
            } else if (codec == "V_MPEGH/ISO/HEVC" &&
                       parse_hvcc(codec_private.data(), codec_private.size(), &out.csd0, &src.nal_len)) {
                out.mime = "video/hevc";
                codec_private.clear();
            } else {
                out.mime = "video/x-unknown";
            }
            if (out.mime == "video/x-vnd.on2.vp8" || out.mime == "video/x-vnd.on2.vp9" || out.mime == "video/av01") {
                out.csd0 = codec_private;
            }
        } else {
            const auto* a = static_cast<const mkvparser::AudioTrack*>(t);
            out.sample_rate = static_cast<int>(std::lround(std::clamp(a->GetSamplingRate(), 0.0, 1e7)));
            out.channels = static_cast<int>(std::clamp(a->GetChannels(), 0LL, 255LL));
            if (!usable) {
                out.mime = "audio/x-unknown";
            } else if (codec == "A_OPUS") {
                out.mime = "audio/opus";
                out.csd0 = codec_private;
                // MatroskaExtractor's csd-1 and csd-2: the codec delay and seek pre-roll in ns;
                // unsaid, the OpusHead's pre-skip and RFC 7845's 80 ms.
                uint64_t delay = t->GetCodecDelay();
                if (delay == 0 && codec_private.size() >= 12) {
                    delay = (codec_private[10] | codec_private[11] << 8) * 1000000000ull / 48000;
                }
                const uint64_t preroll = t->GetSeekPreRoll() ? t->GetSeekPreRoll() : 80000000ull;
                put_le64(&out.csd1, delay);
                put_le64(&out.csd2, preroll);
                out.sample_rate = 48000;  // Opus decodes at 48 kHz whatever the header says it was made from
            } else if (codec == "A_VORBIS" &&
                       parse_vorbis(codec_private.data(), codec_private.size(), &out.csd0, &out.csd1)) {
                out.mime = "audio/vorbis";
            } else if (codec.rfind("A_AAC", 0) == 0 && !codec_private.empty()) {
                out.mime = "audio/mp4a-latm";
                out.csd0 = codec_private;
            } else if (codec == "A_MPEG/L3") {
                out.mime = "audio/mpeg";
            } else if (codec == "A_FLAC") {
                out.mime = "audio/flac";
                out.csd0 = codec_private;
            } else {
                out.mime = "audio/x-unknown";
            }
        }
        const char* lang = t->GetLanguage();
        out.language = lang && *lang ? lang : "eng";  // Matroska's default language
        impl->tracks.push_back(std::move(out));
        impl->sources.push_back(std::move(src));
    }
    if (impl->tracks.empty()) return nullptr;
    return std::unique_ptr<WebmReader>(new WebmReader(std::move(impl)));
}

const std::vector<Demuxer::Track>& WebmReader::tracks() const { return impl_->tracks; }

int64_t WebmReader::duration_us() const { return impl_->duration_us; }

void WebmReader::select(int index, bool on) {
    if (index >= 0 && static_cast<size_t>(index) < impl_->sources.size()) {
        impl_->sources[static_cast<size_t>(index)].selected = on;
    }
}

bool WebmReader::next(Demuxer::Sample* out) {
    Impl& m = *impl_;
    for (;;) {
        if (m.done) return false;
        const mkvparser::Block* block = m.entry && !m.entry->EOS() ? m.entry->GetBlock() : nullptr;
        if (!block || m.frame >= block->GetFrameCount()) {
            if (!m.advance()) {
                m.done = true;
                return false;
            }
            continue;
        }
        const int index = m.index_of(block->GetTrackNumber());
        if (index < 0 || !m.sources[static_cast<size_t>(index)].selected) {
            m.frame = block->GetFrameCount();  // not wanted: on to the next block
            continue;
        }
        out->track = index;
        out->pts_us = block->GetTime(m.cluster) / 1000;
        out->key = block->IsKey();
        const bool ok = m.read_frame(block, m.sources[static_cast<size_t>(index)], out);
        ++m.frame;
        if (!ok) {
            m.done = true;  // a read past the file's end, or a frame that cannot be: stop here
            return false;
        }
        return true;
    }
}

bool WebmReader::seek(int64_t us) {
    Impl& m = *impl_;
    int ref = -1;
    for (size_t i = 0; i < m.sources.size(); ++i) {
        if (!m.sources[i].selected) continue;
        if (ref < 0 || (m.tracks[i].video && !m.tracks[static_cast<size_t>(ref)].video)) ref = static_cast<int>(i);
    }
    if (ref < 0) ref = 0;
    const int64_t ns = std::max<int64_t>(0, us) * 1000;
    const mkvparser::Track* track = m.sources[static_cast<size_t>(ref)].track;
    const mkvparser::Cluster* from = nullptr;
    if (const mkvparser::Cues* cues = m.segment->GetCues()) {
        const mkvparser::CuePoint* cp = nullptr;
        const mkvparser::CuePoint::TrackPosition* tp = nullptr;
        if (cues->Find(ns, track, cp, tp) && cp && tp) {
            if (const mkvparser::BlockEntry* e = cues->GetBlock(cp, tp)) from = e->GetCluster();
        }
    }
    if (!from) {
        // No Cues (or none for the time): the last cluster starting at or before it.
        for (const mkvparser::Cluster* c = m.segment->GetFirst(); c && !c->EOS(); c = m.segment->GetNext(c)) {
            if (c->GetTime() > ns && from) break;
            from = c;
        }
    }
    return from && m.position_at(ns, ref, from);
}

}  // namespace winapk::video
