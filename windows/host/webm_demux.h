// webm_demux.h -- WebM and Matroska files demuxed by libwebm's mkvparser:
// video_decode.h's Demuxer (MediaExtractor's) reads a file through this when
// it starts with an EBML header, as Android reads one through its
// MatroskaExtractor, so that a WebM opens whatever Media Foundation has
// installed. Tracks and samples come out in MediaExtractor's form, after
// MatroskaExtractor: VP8, VP9 and AV1 with their CodecPrivate in csd-0; H.264
// and HEVC as Annex-B with their parameter sets in csd-0 (and H.264's PPS in
// csd-1); Opus with its OpusHead in csd-0, the codec delay and the seek
// pre-roll (ns) in csd-1 and csd-2; Vorbis with its identification and setup
// headers in csd-0 and csd-1; AAC raw with its AudioSpecificConfig in csd-0.
// Plain C++, no Windows call besides opening the file.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "video_decode.h"

namespace winapk::video {

class WebmReader {
public:
    // Whether the file starts with an EBML header (WebM, Matroska).
    static bool is_ebml(const std::wstring& path);
    // Null when the file cannot be read or has no audio or video track.
    static std::unique_ptr<WebmReader> open(const std::wstring& path);
    ~WebmReader();

    const std::vector<Demuxer::Track>& tracks() const;
    int64_t duration_us() const;
    void select(int index, bool on);
    // The next sample of a selected track, in file order. False at the end
    // and on a read error.
    bool next(Demuxer::Sample* out);
    // The next sample read is the sync sample at or before `us` of the
    // selected video track (else of the first selected track): through the
    // file's Cues when it has them.
    bool seek(int64_t us);

private:
    struct Impl;
    explicit WebmReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace winapk::video
