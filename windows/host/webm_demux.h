// webm_demux.h -- WebM and Matroska files read by libwebm's mkvparser, for video_decode.h's
// Demuxer (MediaExtractor's): a file that starts with an EBML header goes through here, as
// Android reads one through its MatroskaExtractor, so that a WebM opens whatever Media
// Foundation has installed. Tracks and samples in MediaExtractor's form, after
// MatroskaExtractor's:
//  - VP8, VP9 and AV1 with their CodecPrivate (if any) in csd-0, and the track's Colour element
//    (matrix, range, transfer, mastering display, MaxCLL/MaxFALL) as the track's ColorAspects;
//  - H.264 and HEVC samples as Annex-B, their parameter sets from the avcC/hvcC with start codes
//    in csd-0 (H.264's PPS in csd-1);
//  - Opus with its OpusHead in csd-0 and the codec delay and seek pre-roll (nanoseconds, 8
//    bytes little-endian) in csd-1 and csd-2;
//  - Vorbis with its identification and setup headers in csd-0 and csd-1;
//  - AAC raw with its AudioSpecificConfig in csd-0; MP3; FLAC with its CodecPrivate in csd-0.
// Other tracks (subtitles, other codecs) are listed under "video/x-unknown" or
// "audio/x-unknown", or not at all. A corrupt or cut file gives what it has up to the damage.
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
    // The next sample of a selected track, in file order. False at the end and on a read error.
    bool next(Demuxer::Sample* out);
    // The next sample read is the sync sample at or before `us` of the selected video track
    // (else of the first selected track), found through the file's Cues when it has them;
    // samples of the other selected tracks follow from there.
    bool seek(int64_t us);

private:
    struct Impl;
    explicit WebmReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace winapk::video
