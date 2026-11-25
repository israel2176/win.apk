// soft_codecs.h -- the software decoders built into the runtime, for what Media Foundation lacks
// on a machine (VP8, VP9 and AV1 need Microsoft's codec packs from the Store; Opus has no Windows
// decoder at all), as Android lists its c2.android software codecs after the hardware ones:
//  - VP8 and VP9: libvpx (VP9 profiles 0 to 3: 8, 10 and 12-bit, 4:2:0 to 4:4:4);
//  - AV1: dav1d (main and high profiles, 8 and 10-bit, film grain applied);
//  - Opus: libopus, mono to 255 channels (multistream), at 48 kHz.
// video_decode.cpp's Decoder and AudioDecoder (MediaCodec's) open one of these when Media
// Foundation has no decoder for the mime or refuses the stream. Plain C++, no Windows call:
// test/soft_codecs_test.cpp runs them anywhere. Built by cmake/soft_codecs.cmake; the
// libraries' licenses are in THIRD_PARTY_NOTICES.txt.
//
// The video decoders use the PC's cores: libvpx's VP9 threads by tile and by row, VP8 by token
// partition, dav1d by frame and by tile (its own sizing: the logical cores, and a frame delay of
// about their square root). A decoded picture is not copied: it comes out as a video_decode.h
// Picture over the decoder's own buffer (VP9: buffers from a pool this side owns, which libvpx
// decodes into; AV1: dav1d's reference-counted picture), held until its consumer converts it;
// only VP8, whose decoder reuses its buffer at the next call, is copied, into pooled memory.
// Pictures are never allocated per frame once the pools are warm. Corrupt input is refused
// (decode() false) without harm to the decoder, which takes up again at the next key frame;
// libvpx and dav1d refuse pictures over 8192x8192.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "video_decode.h"

namespace winapk::video::soft {

// The video mimes a software decoder exists for: "video/x-vnd.on2.vp8", "video/x-vnd.on2.vp9",
// "video/av01".
const std::vector<std::string>& video_mimes();
bool decodes_video(const std::string& mime);

// MediaCodecList's table for a software-decoded mime, in NativeMedia.nativeVideoCapabilities'
// layout: {maxWidth, maxHeight, maxBlocks (16x16), nPairs, profile0, level0, ...}, the
// constants MediaCodecInfo.CodecProfileLevel names. The profiles are those Android's
// c2.android decoders for the same formats declare (VP8 main; VP9 0 and 2; AV1 main 8 and
// main 10), the HDR ones left out: the pictures go to an SDR surface, so an app should not
// pick this decoder for HDR. The sizes are this build's (SIMD and threads: up to 4096 wide or
// tall, 4096x2304's blocks). Empty for any other mime.
std::vector<int> video_capabilities(const std::string& mime);

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    // Decodes one access unit; the pictures it completes are appended to `out` in presentation
    // order, stamped with their own access unit's `pts_us`. A decoder that works on several
    // frames at once (dav1d) gives a picture some calls later. False: the unit was refused
    // (corrupt, or not of this format); the decoder stays usable.
    virtual bool decode(const uint8_t* au, size_t size, int64_t pts_us, std::vector<std::shared_ptr<Frame>>* out) = 0;
    // Whatever is still inside, at the end of the stream.
    virtual void drain(std::vector<std::shared_ptr<Frame>>* out) = 0;
    // Drops everything in flight (MediaCodec.flush, a seek); the next unit should be a key frame.
    virtual void flush() = 0;
    virtual const char* name() const = 0;
};

// Null for a mime decodes_video() does not list, or when the library cannot start.
std::unique_ptr<VideoDecoder> open_video(const std::string& mime);

// Opus (RFC 6716) as Android's c2.android.opus.decoder gives it: 48 kHz, 16-bit interleaved
// PCM, the stream's pre-skip dropped at its start and its seek pre-roll after a flush, the
// header's output gain applied, and the channels of a surround stream (mapping family 1, RFC
// 7845 5.1.1.2's Vorbis order) put in Android's order (front left, front right, centre, LFE,
// back left, back right, ..., side left, side right: AudioFormat's CHANNEL_OUT_* bits from
// lowest), so that AudioTrack plays each where it belongs.
class OpusDecoder {
public:
    // head: MediaFormat's csd-0. Either the OpusHead identification header (RFC 7845 5.1;
    // mapping families 0, 1, 2 and 255), with csd-1 and csd-2 the codec delay and the seek
    // pre-roll in nanoseconds (8-byte little-endian, as MatroskaExtractor gives them; empty:
    // the header's pre-skip and 80 ms), or Android's marked form, which carries all three in one
    // buffer ("AOPUSHDR", "AOPUSDLY", "AOPUSPRL", each followed by its 8-byte little-endian
    // length). Without either, a mono or stereo stream of `channels`. Null when nothing
    // describes a stream libopus can decode.
    static std::unique_ptr<OpusDecoder> open(const std::vector<uint8_t>& head, int channels,
                                             const std::vector<uint8_t>& csd1 = {},
                                             const std::vector<uint8_t>& csd2 = {});
    ~OpusDecoder();
    OpusDecoder(const OpusDecoder&) = delete;
    OpusDecoder& operator=(const OpusDecoder&) = delete;

    // One Opus packet's PCM (empty while the pre-skip or pre-roll is being dropped, or for a
    // packet libopus refuses).
    std::vector<int16_t> decode(const uint8_t* packet, size_t size);
    // The same, appended to `out`.
    void decode(const uint8_t* packet, size_t size, std::vector<int16_t>* out);
    // Drops the decoder's state (a seek): the next packets' first seek pre-roll is dropped.
    void flush();
    int channels() const { return channels_; }
    // AudioFormat's CHANNEL_OUT_* mask of the output's channels, 0 where no layout is defined
    // (family 2 and 255 streams, or more than 8 channels).
    int channel_mask() const { return mask_; }
    // Samples per channel dropped at the start and after a flush.
    int pre_skip() const { return pre_skip_; }
    int seek_preroll() const { return preroll_; }
    static constexpr int kSampleRate = 48000;

private:
    OpusDecoder() = default;
    void* decoder_ = nullptr;  // OpusMSDecoder
    int channels_ = 0;
    int mask_ = 0;
    int pre_skip_ = 0, preroll_ = 0;
    int skip_ = 0;              // samples per channel still to drop
    std::vector<int16_t> pcm_;  // one packet's worth, reused
};

}  // namespace winapk::video::soft
