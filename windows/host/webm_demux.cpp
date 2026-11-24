// webm_demux.cpp -- see webm_demux.h. The tracks' formats follow AOSP's
// MatroskaExtractor (frameworks/av media/extractors/mkv), which Android reads
// these files with.

#include "webm_demux.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <mkvparser/mkvparser.h>

#ifdef _WIN32
#define winapk_fseek _fseeki64
#define winapk_ftell _ftelli64
#else
#define winapk_fseek fseeko
#define winapk_ftell ftello
#endif

namespace winapk::video {

namespace {

FILE* open_file(const std::wstring& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"rb");
#else
    std::string narrow;
    for (wchar_t c : path) narrow.push_back(static_cast<char>(c));  // tests pass ASCII paths
    return std::fopen(narrow.c_str(), "rb");
#endif
}

// mkvparser's view of the file: the whole of it is available at once.
class FileReader : public mkvparser::IMkvReader {
public:
    explicit FileReader(FILE* f) : f_(f) {
        winapk_fseek(f_, 0, SEEK_END);
        size_ = winapk_ftell(f_);
    }
    ~FileReader() override { std::fclose(f_); }
    int Read(long long pos, long len, unsigned char* buf) override {
        if (pos < 0 || len < 0 || pos + len > size_) return -1;
        if (len == 0) return 0;
        if (winapk_fseek(f_, pos, SEEK_SET) != 0) return -1;
        return std::fread(buf, 1, static_cast<size_t>(len), f_) == static_cast<size_t>(len) ? 0 : -1;
    }
    int Length(long long* total, long long* available) override {
        if (total) *total = size_;
        if (available) *available = size_;
        return 0;
    }

private:
    FILE* f_;
    long long size_ = 0;
};

const uint8_t kStartCode[4] = {0, 0, 0, 1};

void append_nal(std::vector<uint8_t>* out, const uint8_t* p, size_t n) {
    out->insert(out->end(), kStartCode, kStartCode + 4);
    out->insert(out->end(), p, p + n);
}

// An AVCDecoderConfigurationRecord's SPSs (csd0) and PPSs (csd1) as Annex-B,
// and the size of the samples' NAL length fields. False when it is not one.
bool parse_avcc(const uint8_t* p, size_t n, std::vector<uint8_t>* csd0, std::vector<uint8_t>* csd1, int* len_size) {
    if (n < 7 || p[0] != 1) return false;
    *len_size = (p[4] & 3) + 1;
    size_t at = 6;
    for (int set = 0; set < 2; ++set) {
        int count = set == 0 ? (p[5] & 0x1f) : 0;
        if (set == 1) {
            if (at >= n) return false;
            count = p[at++];
        }
        for (int i = 0; i < count; ++i) {
            if (at + 2 > n) return false;
            const size_t len = (static_cast<size_t>(p[at]) << 8) | p[at + 1];
            at += 2;
            if (at + len > n) return false;
            append_nal(set == 0 ? csd0 : csd1, p + at, len);
            at += len;
        }
    }
    return true;
}

// An HEVCDecoderConfigurationRecord's parameter sets as Annex-B (all in
// csd0, as Android has them), and the NAL length size.
bool parse_hvcc(const uint8_t* p, size_t n, std::vector<uint8_t>* csd0, int* len_size) {
    if (n < 23) return false;
    *len_size = (p[21] & 3) + 1;
    const int arrays = p[22];
    size_t at = 23;
    for (int a = 0; a < arrays; ++a) {
        if (at + 3 > n) return false;
        const int count = (p[at + 1] << 8) | p[at + 2];
        at += 3;
        for (int i = 0; i < count; ++i) {
            if (at + 2 > n) return false;
            const size_t len = (static_cast<size_t>(p[at]) << 8) | p[at + 1];
            at += 2;
            if (at + len > n) return false;
            append_nal(csd0, p + at, len);
            at += len;
        }
    }
    return true;
}

// Length-prefixed NAL units to start codes. Unchanged when the lengths do not
// add up (the data was Annex-B already).
void to_annexb(std::vector<uint8_t>* d, int len_size) {
    std::vector<uint8_t> out;
    out.reserve(d->size() + 16);
    size_t at = 0;
    while (at + static_cast<size_t>(len_size) <= d->size()) {
        size_t len = 0;
        for (int i = 0; i < len_size; ++i) len = (len << 8) | (*d)[at + static_cast<size_t>(i)];
        at += static_cast<size_t>(len_size);
        if (len > d->size() - at) return;
        append_nal(&out, d->data() + at, len);
        at += len;
    }
    if (at != d->size()) return;
    d->swap(out);
}

// Vorbis' three headers, Xiph-laced in its CodecPrivate: the first
// (identification) and third (setup) are what Android's csd-0 and csd-1 hold.
bool split_vorbis(const uint8_t* p, size_t n, std::vector<uint8_t>* ident, std::vector<uint8_t>* setup) {
    if (n < 3 || p[0] != 2) return false;
    size_t at = 1, sizes[2] = {0, 0};
    for (size_t& s : sizes) {
        while (at < n && p[at] == 255) {
            s += 255;
            ++at;
        }
        if (at >= n) return false;
        s += p[at++];
    }
    if (at + sizes[0] + sizes[1] > n) return false;
    ident->assign(p + at, p + at + sizes[0]);
    const size_t third = at + sizes[0] + sizes[1];
    setup->assign(p + third, p + n);
    return !ident->empty() && !setup->empty();
}

void put_le64(std::vector<uint8_t>* out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out->push_back(static_cast<uint8_t>(v >> (8 * i)));
}

int aac_rate_index(int rate) {
    static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    for (int i = 0; i < 13; ++i) {
        if (kRates[i] == rate) return i;
    }
    return -1;
}

bool starts_with(const char* s, const char* prefix) { return s && std::strncmp(s, prefix, std::strlen(prefix)) == 0; }

}  // namespace

struct WebmReader::Impl {
    std::unique_ptr<FileReader> reader;
    std::unique_ptr<mkvparser::Segment> segment;
    std::vector<Demuxer::Track> tracks;
    std::vector<long long> numbers;  // each track's Matroska track number
    std::vector<int> nal_sizes;      // H.264/HEVC: the samples' NAL length size; 0 = as stored
    std::vector<bool> selected;
    int64_t duration_us = 0;

    // Where reading stands: the cluster and its next block entry.
    struct Pos {
        const mkvparser::Cluster* cluster = nullptr;
        long index = 0;
    };
    Pos pos;
    std::vector<Demuxer::Sample> queue;  // the rest of a laced block

    int track_of(long long number) const {
        for (size_t i = 0; i < numbers.size(); ++i) {
            if (numbers[i] == number) return static_cast<int>(i);
        }
        return -1;
    }

    // The block entry at `p`, and `p` moved past it, loading clusters as
    // reading reaches them. False at the end of the file and on errors.
    bool step(Pos* p, const mkvparser::BlockEntry** out) {
        bool parsed_all = false;
        while (p->cluster && !p->cluster->EOS()) {
            const mkvparser::BlockEntry* be = nullptr;
            long r = p->cluster->GetEntry(p->index, be);
            if (r == mkvparser::E_BUFFER_NOT_FULL && !parsed_all) {
                long long at = 0;
                long len = 0;
                r = p->cluster->Parse(at, len);
                if (r < 0) return false;
                parsed_all = r > 0;
                continue;
            }
            if (r < 0 && r != mkvparser::E_BUFFER_NOT_FULL) return false;
            if (r == 1 && be) {
                ++p->index;
                *out = be;
                return true;
            }
            // This cluster is done: the next one.
            const mkvparser::Cluster* next = nullptr;
            long long at = 0;
            long len = 0;
            if (segment->ParseNext(p->cluster, next, at, len) != 0 || !next) {
                p->cluster = nullptr;
                return false;
            }
            p->cluster = next;
            p->index = 0;
            parsed_all = false;
        }
        return false;
    }

    bool add_track(const mkvparser::Track* t) {
        const long type = t->GetType();
        if (type != mkvparser::Track::kVideo && type != mkvparser::Track::kAudio) return false;
        const char* id = t->GetCodecId();
        size_t priv_size = 0;
        const unsigned char* priv = t->GetCodecPrivate(priv_size);
        Demuxer::Track out;
        int nal_size = 0;
        if (type == mkvparser::Track::kVideo) {
            const auto* v = static_cast<const mkvparser::VideoTrack*>(t);
            out.video = true;
            out.width = static_cast<int>(v->GetWidth());
            out.height = static_cast<int>(v->GetHeight());
            out.frame_rate = v->GetFrameRate();
            out.mime = "video/x-unknown";
            if (starts_with(id, "V_VP8")) {
                out.mime = "video/x-vnd.on2.vp8";
            } else if (starts_with(id, "V_VP9")) {
                out.mime = "video/x-vnd.on2.vp9";
                if (priv && priv_size) out.csd0.assign(priv, priv + priv_size);  // the VP9 CodecPrivate
            } else if (starts_with(id, "V_AV1")) {
                out.mime = "video/av01";
                if (priv && priv_size) out.csd0.assign(priv, priv + priv_size);  // the av1C box
            } else if (starts_with(id, "V_MPEG4/ISO/AVC")) {
                out.mime = "video/avc";
                if (!priv || !parse_avcc(priv, priv_size, &out.csd0, &out.csd1, &nal_size)) nal_size = 0;
            } else if (starts_with(id, "V_MPEGH/ISO/HEVC")) {
                out.mime = "video/hevc";
                if (!priv || !parse_hvcc(priv, priv_size, &out.csd0, &nal_size)) nal_size = 0;
            } else if (starts_with(id, "V_MPEG4/ISO/")) {
                out.mime = "video/mp4v-es";
                if (priv && priv_size) out.csd0.assign(priv, priv + priv_size);
            } else if (starts_with(id, "V_MPEG2")) {
                out.mime = "video/mpeg2";
            }
        } else {
            const auto* a = static_cast<const mkvparser::AudioTrack*>(t);
            out.sample_rate = static_cast<int>(a->GetSamplingRate());
            out.channels = static_cast<int>(a->GetChannels());
            out.mime = "audio/x-unknown";
            if (starts_with(id, "A_OPUS")) {
                out.mime = "audio/opus";
                if (priv && priv_size) out.csd0.assign(priv, priv + priv_size);
                put_le64(&out.csd1, t->GetCodecDelay());
                put_le64(&out.csd2, t->GetSeekPreRoll());
            } else if (starts_with(id, "A_VORBIS")) {
                out.mime = "audio/vorbis";
                if (!priv || !split_vorbis(priv, priv_size, &out.csd0, &out.csd1)) {
                    out.csd0.clear();
                    out.csd1.clear();
                }
            } else if (starts_with(id, "A_AAC")) {
                out.mime = "audio/mp4a-latm";
                if (priv && priv_size) {
                    out.csd0.assign(priv, priv + priv_size);
                } else if (aac_rate_index(out.sample_rate) >= 0) {  // AAC-LC, as the codec ID names it
                    const int idx = aac_rate_index(out.sample_rate);
                    out.csd0 = {static_cast<uint8_t>((2 << 3) | (idx >> 1)),
                                static_cast<uint8_t>(((idx & 1) << 7) | (out.channels << 3))};
                }
            } else if (starts_with(id, "A_MPEG/L3")) {
                out.mime = "audio/mpeg";
            } else if (starts_with(id, "A_FLAC")) {
                out.mime = "audio/flac";
                if (priv && priv_size) out.csd0.assign(priv, priv + priv_size);
            } else if (starts_with(id, "A_PCM/INT/LIT")) {
                out.mime = "audio/raw";
            }
        }
        if (const char* lang = t->GetLanguage()) out.language = lang;
        tracks.push_back(std::move(out));
        numbers.push_back(t->GetNumber());
        nal_sizes.push_back(nal_size);
        selected.push_back(false);
        return true;
    }

    // The frames of a block of a selected track, onto `queue`.
    bool read_block(const mkvparser::BlockEntry* be) {
        const mkvparser::Block* b = be->GetBlock();
        const int track = track_of(b->GetTrackNumber());
        if (track < 0 || !selected[static_cast<size_t>(track)]) return true;
        const Demuxer::Track& t = tracks[static_cast<size_t>(track)];
        const int64_t pts_us = b->GetTime(be->GetCluster()) / 1000;
        for (int i = 0; i < b->GetFrameCount(); ++i) {
            const mkvparser::Block::Frame& f = b->GetFrame(i);
            if (f.len <= 0) continue;
            Demuxer::Sample s;
            s.track = track;
            s.pts_us = pts_us;
            s.key = !t.video || b->IsKey();
            s.data.resize(static_cast<size_t>(f.len));
            if (f.Read(reader.get(), s.data.data()) != 0) return false;
            if (nal_sizes[static_cast<size_t>(track)]) to_annexb(&s.data, nal_sizes[static_cast<size_t>(track)]);
            queue.push_back(std::move(s));
        }
        return true;
    }
};

WebmReader::WebmReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WebmReader::~WebmReader() = default;

bool WebmReader::is_ebml(const std::wstring& path) {
    FILE* f = open_file(path);
    if (!f) return false;
    uint8_t magic[4] = {0, 0, 0, 0};
    const bool ok = std::fread(magic, 1, 4, f) == 4 && magic[0] == 0x1a && magic[1] == 0x45 && magic[2] == 0xdf &&
                    magic[3] == 0xa3;
    std::fclose(f);
    return ok;
}

std::unique_ptr<WebmReader> WebmReader::open(const std::wstring& path) {
    FILE* f = open_file(path);
    if (!f) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->reader = std::make_unique<FileReader>(f);
    long long pos = 0;
    mkvparser::EBMLHeader ebml;
    if (ebml.Parse(impl->reader.get(), pos) < 0) return nullptr;
    mkvparser::Segment* segment = nullptr;
    if (mkvparser::Segment::CreateInstance(impl->reader.get(), pos, segment) != 0 || !segment) return nullptr;
    impl->segment.reset(segment);
    if (segment->ParseHeaders() != 0) return nullptr;
    long len = 0;
    if (segment->LoadCluster(pos, len) < 0) return nullptr;  // > 0: no cluster at all, an empty file
    const mkvparser::Tracks* tracks = segment->GetTracks();
    if (!tracks) return nullptr;
    for (unsigned long i = 0; i < tracks->GetTracksCount(); ++i) {
        if (const mkvparser::Track* t = tracks->GetTrackByIndex(i)) impl->add_track(t);
    }
    if (impl->tracks.empty()) return nullptr;
    if (const mkvparser::SegmentInfo* info = segment->GetInfo()) {
        const long long ns = info->GetDuration();
        if (ns > 0) impl->duration_us = ns / 1000;
    }
    impl->pos.cluster = segment->GetFirst();
    return std::unique_ptr<WebmReader>(new WebmReader(std::move(impl)));
}

const std::vector<Demuxer::Track>& WebmReader::tracks() const { return impl_->tracks; }
int64_t WebmReader::duration_us() const { return impl_->duration_us; }

void WebmReader::select(int index, bool on) {
    Impl& d = *impl_;
    if (index < 0 || index >= static_cast<int>(d.tracks.size())) return;
    d.selected[static_cast<size_t>(index)] = on;
    if (!on) {
        d.queue.erase(std::remove_if(d.queue.begin(), d.queue.end(),
                                     [&](const Demuxer::Sample& s) { return s.track == index; }),
                      d.queue.end());
    }
}

bool WebmReader::next(Demuxer::Sample* out) {
    Impl& d = *impl_;
    while (d.queue.empty()) {
        const mkvparser::BlockEntry* be = nullptr;
        if (!d.step(&d.pos, &be) || !d.read_block(be)) return false;
    }
    *out = std::move(d.queue.front());
    d.queue.erase(d.queue.begin());
    return true;
}

bool WebmReader::seek(int64_t us) {
    Impl& d = *impl_;
    if (us < 0) us = 0;
    const long long ns = us * 1000;
    // The track the seek is for: the selected video track, else the first
    // selected one, else the first.
    int ref = -1;
    for (size_t i = 0; i < d.tracks.size(); ++i) {
        if (d.selected[i] && (ref < 0 || (d.tracks[i].video && !d.tracks[static_cast<size_t>(ref)].video))) {
            ref = static_cast<int>(i);
        }
    }
    if (ref < 0) ref = 0;
    const long long number = d.numbers[static_cast<size_t>(ref)];
    const bool video = d.tracks[static_cast<size_t>(ref)].video;

    // Where to start looking: the cluster the Cues name for the time, else
    // the first one.
    Impl::Pos start;
    start.cluster = d.segment->GetFirst();
    if (const mkvparser::Cues* cues = d.segment->GetCues()) {
        while (!cues->DoneParsing()) cues->LoadCuePoint();
        const mkvparser::CuePoint* cp = nullptr;
        const mkvparser::CuePoint::TrackPosition* tp = nullptr;
        const mkvparser::Track* t = d.segment->GetTracks()->GetTrackByNumber(number);
        if (t && cues->Find(ns, t, cp, tp) && tp) {
            const mkvparser::Cluster* c = d.segment->FindOrPreloadCluster(tp->m_pos);
            if (c && !c->EOS()) start.cluster = c;
        }
    }
    // From there, the last sync block of the track at or before the time
    // (the first block of the track when none is).
    Impl::Pos p = start, best = start;
    bool found = false;
    for (;;) {
        const Impl::Pos here = p;
        const mkvparser::BlockEntry* be = nullptr;
        if (!d.step(&p, &be)) break;
        const mkvparser::Block* b = be->GetBlock();
        if (b->GetTrackNumber() != number) continue;
        const bool after = b->GetTime(be->GetCluster()) > ns;
        if (after && found) break;
        if (after || !video || b->IsKey()) {
            best = here;
            found = true;
            if (after) break;
        }
    }
    d.pos = best;
    d.queue.clear();
    return true;
}

}  // namespace winapk::video
