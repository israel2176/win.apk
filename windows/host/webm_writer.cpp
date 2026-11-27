// webm_writer.cpp -- see webm_writer.h.

#include "webm_writer.h"

#include <algorithm>
#include <cstring>

#include "mkvmuxer/mkvmuxer.h"

namespace winapk::webm {

namespace {

// mkvmuxer's output: a stdio file, seekable, positions 64-bit.
class FileOut : public mkvmuxer::IMkvWriter {
public:
    explicit FileOut(std::FILE* file) : file_(file) {}
    ~FileOut() override { close(); }

    mkvmuxer::int32 Write(const void* buf, mkvmuxer::uint32 len) override {
        if (!file_ || (len && std::fwrite(buf, 1, len, file_) != len)) return -1;
        return 0;
    }
    mkvmuxer::int64 Position() const override {
        if (!file_) return -1;
#ifdef _WIN32
        return _ftelli64(file_);
#else
        return static_cast<int64_t>(ftello(file_));
#endif
    }
    mkvmuxer::int32 Position(mkvmuxer::int64 position) override {
        if (!file_) return -1;
#ifdef _WIN32
        return _fseeki64(file_, position, SEEK_SET) == 0 ? 0 : -1;
#else
        return fseeko(file_, static_cast<off_t>(position), SEEK_SET) == 0 ? 0 : -1;
#endif
    }
    bool Seekable() const override { return true; }
    void ElementStartNotify(mkvmuxer::uint64, mkvmuxer::int64) override {}

    bool close() {
        if (!file_) return true;
        const bool ok = std::fclose(file_) == 0;
        file_ = nullptr;
        return ok;
    }

private:
    std::FILE* file_;
};

uint64_t le64(const std::vector<uint8_t>& v) {
    uint64_t x = 0;
    for (size_t i = 0; i < 8 && i < v.size(); ++i) x |= static_cast<uint64_t>(v[i]) << (8 * i);
    return x;
}

// Xiph lacing of a Vorbis track's three headers, Matroska's CodecPrivate for A_VORBIS.
std::vector<uint8_t> vorbis_private(const std::vector<uint8_t>& id, const std::vector<uint8_t>& setup) {
    // An empty comment header: packet type 3, "vorbis", a vendor string of length 0, no
    // comments, the framing bit.
    const uint8_t comment[] = {3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1};
    std::vector<uint8_t> out{2};
    for (size_t n : {id.size(), sizeof(comment)}) {
        for (; n >= 255; n -= 255) out.push_back(255);
        out.push_back(static_cast<uint8_t>(n));
    }
    out.insert(out.end(), id.begin(), id.end());
    out.insert(out.end(), comment, comment + sizeof(comment));
    out.insert(out.end(), setup.begin(), setup.end());
    return out;
}

constexpr uint64_t kOpusPreRollNs = 80000000;  // RFC 7845, section 4.6

}  // namespace

struct Writer::Impl {
    FileOut out;
    mkvmuxer::Segment segment;
    std::vector<TrackFormat> formats;
    std::vector<uint64_t> numbers;  // each track's Matroska number
    bool started = false, failed = false, finished = false;
    // Each track's first time: its samples' times are kept less it, as AOSP's WebmWriter does
    // (WebmFrameMediaSourceThread), so every track starts the file at 0.
    std::vector<bool> have_first;
    std::vector<int64_t> first_us;
    bool any_frame = false;

    explicit Impl(std::FILE* file) : out(file) {}
};

Writer::Writer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Writer::~Writer() = default;

std::unique_ptr<Writer> Writer::adopt(std::FILE* file) {
    if (!file) return nullptr;
    auto impl = std::make_unique<Impl>(file);
    if (!impl->segment.Init(&impl->out)) return nullptr;
    impl->segment.set_mode(mkvmuxer::Segment::kFile);
    impl->segment.GetSegmentInfo()->set_writing_app("Win.apk");
    impl->segment.GetSegmentInfo()->set_muxing_app("Win.apk");
    return std::unique_ptr<Writer>(new Writer(std::move(impl)));
}

bool Writer::takes(const std::string& mime) {
    return mime == "video/x-vnd.on2.vp8" || mime == "video/x-vnd.on2.vp9" || mime == "audio/opus" ||
           mime == "audio/vorbis";
}

int Writer::add_track(const TrackFormat& f) {
    Impl& w = *impl_;
    if (w.started || !takes(f.mime)) return -1;
    const bool video = f.mime.rfind("video/", 0) == 0;
    uint64_t number = 0;
    if (video) {
        if (f.width <= 0 || f.height <= 0) return -1;
        number = w.segment.AddVideoTrack(f.width, f.height, 0);
        auto* t = number ? static_cast<mkvmuxer::VideoTrack*>(w.segment.GetTrackByNumber(number)) : nullptr;
        if (!t) return -1;
        t->set_codec_id(f.mime == "video/x-vnd.on2.vp8" ? mkvmuxer::Tracks::kVp8CodecId
                                                         : mkvmuxer::Tracks::kVp9CodecId);
        if (!f.csd0.empty() && !t->SetCodecPrivate(f.csd0.data(), f.csd0.size())) return -1;
    } else {
        if (f.sample_rate <= 0 || f.channels <= 0) return -1;
        std::vector<uint8_t> priv;
        if (f.mime == "audio/opus") {
            if (f.csd0.size() < 19) return -1;  // an OpusHead is 19 bytes or more
            priv = f.csd0;
        } else {
            if (f.csd0.empty() || f.csd1.empty()) return -1;
            priv = vorbis_private(f.csd0, f.csd1);
        }
        number = w.segment.AddAudioTrack(f.sample_rate, f.channels, 0);
        auto* t = number ? static_cast<mkvmuxer::AudioTrack*>(w.segment.GetTrackByNumber(number)) : nullptr;
        if (!t) return -1;
        if (f.mime == "audio/opus") {
            t->set_codec_id(mkvmuxer::Tracks::kOpusCodecId);
            const uint64_t pre_skip = static_cast<uint64_t>(f.csd0[10] | (f.csd0[11] << 8));
            t->set_codec_delay(f.csd1.size() == 8 ? le64(f.csd1) : pre_skip * 1000000000ull / 48000);
            t->set_seek_pre_roll(f.csd2.size() == 8 ? le64(f.csd2) : kOpusPreRollNs);
            t->set_bit_depth(16);
        } else {
            t->set_codec_id(mkvmuxer::Tracks::kVorbisCodecId);
        }
        if (!t->SetCodecPrivate(priv.data(), priv.size())) return -1;
    }
    w.formats.push_back(f);
    w.numbers.push_back(number);
    w.have_first.push_back(false);
    w.first_us.push_back(0);
    return static_cast<int>(w.formats.size() - 1);
}

bool Writer::start() {
    Impl& w = *impl_;
    if (w.started || w.formats.empty()) return false;
    w.started = true;
    return true;
}

bool Writer::write(int track, const uint8_t* data, size_t size, int64_t pts_us, bool key, bool config) {
    Impl& w = *impl_;
    if (!w.started || w.finished || w.failed || track < 0 || track >= static_cast<int>(w.numbers.size()))
        return false;
    if (config) return true;  // the track's codec data came with its format
    // An empty sample (an encoder's end-of-stream buffer) holds no frame: nothing to write, as
    // MediaMuxer takes it.
    if (!size) return true;
    if (!data) return false;
    const size_t t = static_cast<size_t>(track);
    if (!w.have_first[t]) {
        w.have_first[t] = true;
        w.first_us[t] = pts_us;
    }
    const int64_t at = std::max<int64_t>(0, pts_us - w.first_us[t]);
    // Every Opus and Vorbis frame stands alone.
    const bool sync = key || w.formats[t].mime.rfind("audio/", 0) == 0;
    if (!w.segment.AddFrame(data, size, w.numbers[t], static_cast<uint64_t>(at) * 1000, sync)) {
        w.failed = true;
        return false;
    }
    w.any_frame = true;
    return true;
}

bool Writer::finish() {
    Impl& w = *impl_;
    if (w.finished) return false;
    w.finished = true;
    const bool ok = w.started && w.any_frame && !w.failed && w.segment.Finalize();
    return w.out.close() && ok;
}

}  // namespace winapk::webm
