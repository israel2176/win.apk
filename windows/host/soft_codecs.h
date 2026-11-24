// soft_codecs.h -- the software decoders built into the runtime, for what
// Media Foundation lacks on a machine (VP8, VP9 and AV1 need Microsoft's codec
// packs from the Store; Opus has no Windows decoder at all):
//  - VP8 and VP9: libvpx, decode only;
//  - AV1: dav1d;
//  - Opus: libopus, decode only.
// video_decode.cpp's Decoder and AudioDecoder (MediaCodec's) open one of these
// when Media Foundation has no decoder for the mime or refuses the stream, as
// Android lists its c2.android software codecs after the hardware ones. Plain
// C++, no Windows call: test/soft_codecs_test.cpp runs them anywhere.
// Built by cmake/soft_codecs.cmake; the libraries' licenses are in
// package/THIRD_PARTY_NOTICES.txt.
//
// Adapted from the other runtime (https://example.invalid/other-runtime),
// compat/media/video/soft_video.cc, Copyright (C) 2026 its authors,
// SPDX-License-Identifier: Apache-2.0. Modified for Win.apk.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "video_decode.h"

namespace winapk::video::soft {

// The video mimes a software decoder exists for: "video/x-vnd.on2.vp8",
// "video/x-vnd.on2.vp9", "video/av01".
const std::vector<std::string>& video_mimes();
bool decodes_video(const std::string& mime);

// MediaCodecList's profile/level table for a software-decoded mime, in
// NativeMedia.nativeVideoCapabilities' layout: {maxWidth, maxHeight,
// maxMacroblocks, nPairs, profile0, level0, ...} (MediaCodecInfo's
// constants). AOSP's own declaration for the same libraries
// (media_codecs_google_c2_video.xml's limits, the c2.android components'
// profiles); empty for any other mime.
std::vector<int> video_capabilities(const std::string& mime);

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    // Decodes one access unit; the pictures it completes are appended to
    // `out` in presentation order, at `pts_us`. False: the decoder refused it.
    virtual bool decode(const uint8_t* au, size_t size, int64_t pts_us, std::vector<std::shared_ptr<Frame>>* out) = 0;
    // Whatever is still inside, at the end of the stream.
    virtual void drain(std::vector<std::shared_ptr<Frame>>* out) = 0;
    // Drops everything in flight (MediaCodec.flush, a seek).
    virtual void flush() = 0;
    virtual const char* name() const = 0;
};

// Null for a mime decodes_video() does not list, or when the library cannot
// start.
std::unique_ptr<VideoDecoder> open_video(const std::string& mime);

// Opus (RFC 6716) at 48 kHz, as Android's c2.android.opus.decoder: 16-bit
// interleaved PCM, the stream's pre-skip dropped at its start and the
// header's output gain applied.
class OpusDecoder {
public:
    // head: MediaFormat's csd-0, the "OpusHead" identification header (RFC
    // 7845 5.1), channel mapping families 0, 1 and 255 included. Without one,
    // a mono or stereo stream of `channels`. Null when neither describes a
    // stream libopus can decode.
    static std::unique_ptr<OpusDecoder> open(const std::vector<uint8_t>& head, int channels);
    ~OpusDecoder();
    // One Opus packet's PCM (empty while the pre-skip is being dropped, or for
    // a packet libopus refuses).
    std::vector<int16_t> decode(const uint8_t* packet, size_t size);
    int channels() const { return channels_; }
    static constexpr int kSampleRate = 48000;

private:
    OpusDecoder() = default;
    void* decoder_ = nullptr;  // OpusMSDecoder
    int channels_ = 0;
    int skip_ = 0;  // samples per channel still to drop
};

}  // namespace winapk::video::soft
