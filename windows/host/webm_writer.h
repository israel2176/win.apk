// webm_writer.h -- a WebM file from encoded samples, on libwebm's mkvmuxer: what MediaMuxer's
// MUXER_OUTPUT_WEBM writes (ndk_media_muxer.cpp's AMediaMuxer). The tracks AOSP 14's WebmWriter
// takes: VP8 and VP9 video, Opus and Vorbis audio, with their codec data as MediaFormat carries
// it (what webm_demux.h gives back):
//  - Opus: csd-0 the OpusHead, csd-1 the codec delay and csd-2 the seek pre-roll (nanoseconds,
//    8 bytes little-endian); without them, the delay is the OpusHead's pre-skip and the pre-roll
//    the 80 ms RFC 7845 asks a decoder to discard after a seek;
//  - Vorbis: csd-0 the identification header, csd-1 the setup header; the comment header, which
//    MediaFormat does not carry, is written empty.
// No Windows call: the same code builds anywhere, which is how it is checked
// (test/ndk_muxer_test.cpp).
//
// Each track's times are kept as given less that track's first sample's, as AOSP 14's
// WebmWriter does, so every track starts the file at 0, in the Segment's millisecond timecodes.
// An empty sample (an encoder's end-of-stream buffer) is taken and not written. The file is finished (its duration,
// Cues and SeekHead written) by finish(); one that never was has none of them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace winapk::webm {

struct TrackFormat {
    std::string mime;  // video/x-vnd.on2.vp8, video/x-vnd.on2.vp9, audio/opus, audio/vorbis
    int width = 0, height = 0;
    int sample_rate = 0, channels = 0;
    std::vector<uint8_t> csd0, csd1, csd2;
};

class Writer {
public:
    // Writes into `file` from its start; the writer owns it from here (finish() or the destructor
    // closes it). Null when `file` is.
    static std::unique_ptr<Writer> adopt(std::FILE* file);
    ~Writer();

    // Whether `mime` is a track a WebM file holds here.
    static bool takes(const std::string& mime);
    // Before start(). The track's index, or -1 for a format it cannot write.
    int add_track(const TrackFormat& format);
    bool start();
    // One frame in decoding order. A `config` buffer is codec data, not a frame: not written.
    bool write(int track, const uint8_t* data, size_t size, int64_t pts_us, bool key, bool config);
    // Writes the Cues and the headers' sizes and closes the file. False: the file is not whole.
    bool finish();

private:
    struct Impl;
    explicit Writer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace winapk::webm
