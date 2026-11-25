// video_decode.h -- video on Media Foundation, without a JVM: the pieces
// native_media_video.cpp binds to Java and test/video_test.cpp checks.
//
// Three shapes, after what the Java side asks for:
//  - Decoder: an elementary-stream decoder (MediaCodec's shape, through
//    com.macapk.loader.NativeMedia): access units in, pictures out, on the
//    synchronous decoder MFTs Windows registers (H.264 always; HEVC, VP9, VP8
//    and AV1 when their codec packs are installed), else on the software
//    decoders built in (soft_codecs.h: VP8, VP9, AV1).
//  - AudioDecoder: the same for AAC and MP3 (Media Foundation's) and Opus
//    (libopus, soft_codecs.h): MediaCodec's audio shape.
//  - Source: a file, URL or memory buffer demuxed and decoded by an
//    IMFSourceReader (MediaPlayer's shape): video pictures and float PCM in
//    presentation order, with seeking.
//  - Demuxer: a file or URL demuxed and not decoded (MediaExtractor's shape):
//    every track's format and its access units, as MediaCodec takes them;
//    a WebM or Matroska file through mkvparser (webm_demux.h).
// Media Foundation's pictures come out as ARGB ints (0xAARRGGBB, alpha 0xff,
// row stride = width), the layout the Java side presents, with any rotation
// the container asks for already applied; the software decoders' as the
// decoder's own planes (Picture), converted only by the consumer.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace winapk::video {

// A picture's colour, in MediaFormat's terms (KEY_COLOR_STANDARD, KEY_COLOR_RANGE,
// KEY_COLOR_TRANSFER, KEY_HDR_STATIC_INFO); 0 or empty where the stream says nothing.
struct ColorAspects {
    enum Standard { kBt709 = 1, kBt601Pal = 2, kBt601Ntsc = 4, kBt2020 = 6 };
    enum Range { kFull = 1, kLimited = 2 };
    enum Transfer { kLinear = 1, kSdrVideo = 3, kSt2084 = 6, kHlg = 7 };
    int standard = 0, range = 0, transfer = 0;
    // CTA-861.3's static metadata type 1, as Android lays it out: a zero byte, then twelve
    // little-endian uint16: the R, G, B and white points' x,y (0.00002 units), the mastering
    // display's max (1 cd/m2) and min (0.0001 cd/m2) luminance, MaxCLL and MaxFALL.
    std::vector<uint8_t> hdr_static_info;
};

// A software decoder's picture, still in the decoder's own memory: planar Y'CbCr (4:2:0, 4:2:2
// or 4:4:4, or Y' alone), 8-bit samples, or 16-bit ones holding `bit_depth` bits in their low
// bits. Whoever holds one keeps that memory from being reused; the last reference gives it back
// (soft_codecs.cpp). width and height are the visible picture (the crop), not the coded size;
// strides are in bytes.
struct Picture {
    virtual ~Picture() = default;
    int width = 0, height = 0;
    int bit_depth = 8;
    int chroma_shift_x = 1, chroma_shift_y = 1;  // 4:2:0
    bool monochrome = false;                     // no chroma planes: grey
    const uint8_t* plane[3] = {nullptr, nullptr, nullptr};
    int stride[3] = {0, 0, 0};
    ColorAspects color;
};

// One decoded picture: Media Foundation's as ARGB, a software decoder's as its Picture.
struct Frame {
    int width = 0, height = 0;
    int64_t pts_us = 0;
    std::vector<uint32_t> argb;              // width * height, or empty when `picture` is set
    std::shared_ptr<const Picture> picture;  // set by the software decoders (soft_codecs.h)
};

// The rest is yuv_convert.cpp.

// An NV12 (8-bit) or P010 (16-bit samples, the value in the top 10 bits) picture
// to ARGB, rotated clockwise by `rotation` degrees (0, 90, 180 or 270): `out`
// holds height x width pixels for 90 and 270, width x height otherwise. Strides
// are in bytes; the chroma plane is half the height, with interleaved Cb,Cr.
// Media Foundation's pictures.
void yuv420_to_argb(const uint8_t* y, int y_stride, const uint8_t* uv, int uv_stride, int width, int height,
                    bool sixteen_bit, bool bt709, bool full_range, int rotation, uint32_t* out);

// The frame as ARGB (0xAARRGGBB, row stride = width) into `out`, width * height pixels:
// copied, or converted from its Picture by the picture's colour standard and range, 10 and
// 12-bit dithered to 8. False for an empty frame.
bool frame_to_argb(const Frame& f, uint32_t* out);
// The same into a new vector, for code that wants one.
std::vector<uint32_t> frame_argb(const Frame& f);

// What MediaCodec's ByteBuffer and Image output hold: a 4:2:0 picture of the frame's size in
// one buffer, Y' then Cb then Cr (kI420, 8-bit; the chroma rows stride / 2 bytes apart) or Y'
// then interleaved CbCr (kP010: 16-bit samples, the value in the top 10 bits; chroma rows
// stride bytes apart), luma rows `stride` bytes apart and the luma plane `slice_height` rows
// tall. Deeper pictures are dithered down to I420, 8-bit ones widened to P010, 4:2:2 and 4:4:4
// averaged down to 4:2:0, as Android's software decoders give 4:2:0 only.
struct YuvLayout {
    enum Format { kI420 = 0, kP010 = 1 };
    Format format = kI420;
    int width = 0, height = 0, stride = 0, slice_height = 0;
    size_t size() const;
    static YuvLayout of(const Frame& f, Format format);
};
bool frame_to_yuv(const Frame& f, const YuvLayout& layout, uint8_t* out);
// The colour of frame_to_yuv's planes: the picture's own, or for an ARGB frame the BT.601
// limited range frame_to_yuv converts it to.
ColorAspects frame_color(const Frame& f);

// The video mimes (AOSP's names: "video/avc", "video/hevc", ...) a decoder
// exists for on this machine, probed once by trying to create each one, and
// the ones a built-in software decoder takes.
const std::vector<std::string>& supported_mimes();
// Whether Media Foundation itself has a decoder for the mime on this machine.
bool media_foundation_decodes(const std::string& mime);

class Decoder {
public:
    // csd0/csd1 are MediaFormat's "csd-0"/"csd-1" as the app gave them (Annex-B
    // parameter sets for H.264 and HEVC; empty for codecs without any). Null
    // when no decoder exists for `mime` or it rejects the stream.
    static std::unique_ptr<Decoder> open(const std::string& mime, int width, int height,
                                         const std::vector<uint8_t>& csd0, const std::vector<uint8_t>& csd1);
    ~Decoder();

    // Feeds one access unit (au may be null with eos). Decoding is synchronous,
    // but a decoder reorders and buffers, so the picture comes out of poll()
    // later. False: the decoder refused the data.
    bool decode(const uint8_t* au, size_t size, int64_t pts_us, bool eos);
    // The next picture in presentation order, or null. After decode(eos) and
    // the last picture, one call returns null with *eos set.
    std::shared_ptr<Frame> poll(bool* eos);
    // Drops everything decoded and in flight.
    void flush();
    int width() const;
    int height() const;

private:
    struct Impl;
    explicit Decoder(std::unique_ptr<Impl> impl);
    static std::unique_ptr<Impl> open_mf(const std::string& mime, int width, int height,
                                         const std::vector<uint8_t>& csd0, const std::vector<uint8_t>& csd1);
    std::unique_ptr<Impl> impl_;
};

class AudioDecoder {
public:
    // format_id as MediaCodec.java numbers them: 1 AAC (csd0 = the
    // AudioSpecificConfig), 2 MP3, 3 Opus (csd0 = the OpusHead, or Android's
    // marked form with csd-1 and csd-2 in it: soft_codecs.h; the output is
    // 48 kHz). Null for the others.
    static std::unique_ptr<AudioDecoder> open(int format_id, int sample_rate, int channels,
                                              const std::vector<uint8_t>& csd0);
    ~AudioDecoder();
    // The PCM (16-bit, interleaved) this access unit yields; may be empty while
    // the decoder primes. With eos, whatever is left.
    std::vector<int16_t> decode(const uint8_t* au, size_t size, bool eos);
    // Drops what is in flight (MediaCodec.flush, a seek); Opus then drops its seek pre-roll.
    void flush();
    int sample_rate() const;
    int channels() const;

private:
    struct Impl;
    explicit AudioDecoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class Source {
public:
    struct Packet {
        enum Kind { kVideo, kAudio, kEnd };
        Kind kind = kEnd;
        int64_t pts_us = 0;
        std::shared_ptr<Frame> frame;  // kVideo, decoded
        std::vector<uint8_t> data;     // kVideo, compressed (open with decode_video = false)
        std::vector<float> pcm;        // kAudio: interleaved, audio_channels() wide
    };

    // A path or an http(s) URL. With decode_video false the video stream is
    // read compressed (video_mime(), video_csd(), Packet::data): what a
    // MediaExtractor sees, and what test/video_test.cpp feeds a Decoder.
    static std::unique_ptr<Source> open(const std::wstring& path_or_url, bool decode_video = true);
    static std::unique_ptr<Source> open(std::vector<uint8_t> bytes, bool decode_video = true);
    ~Source();

    bool has_video() const;
    bool has_audio() const;
    int width() const;   // after rotation
    int height() const;
    int rotation() const;
    int64_t duration_us() const;
    int audio_rate() const;
    int audio_channels() const;
    const std::string& video_mime() const;
    const std::vector<uint8_t>& video_csd() const;

    // The next packet of either stream, in file order; kEnd once both are
    // done. False on a read error.
    bool read(Packet* out);
    // Reads of the video stream alone, or of the audio stream alone (kEnd
    // when that stream is done).
    bool read_video(Packet* out);
    bool read_audio(Packet* out);
    // The next reads start at the key frame at or before `us`.
    bool seek(int64_t us);

private:
    struct Impl;
    explicit Source(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class Demuxer {
public:
    struct Track {
        bool video = false;
        std::string mime;  // AOSP's names; "video/x-unknown" or "audio/x-unknown" for a stream nothing here names
        int width = 0, height = 0, rotation = 0;
        double frame_rate = 0;
        int sample_rate = 0, channels = 0;
        int bitrate = 0;  // bits per second, 0 = unsaid
        // MediaFormat's "csd-0" / "csd-1" / "csd-2": H.264's SPS and PPS and
        // HEVC's parameter sets (in csd0) with start codes, AAC's
        // AudioSpecificConfig, and what webm_demux.h gives a WebM's tracks.
        std::vector<uint8_t> csd0, csd1, csd2;
        std::string language;
        ColorAspects color;  // what the container says of a video track's colour
    };
    struct Sample {
        int track = -1;
        int64_t pts_us = 0;
        bool key = false;
        std::vector<uint8_t> data;  // H.264 and HEVC as Annex-B, AAC raw (no ADTS)
    };
    enum SeekMode { kPreviousSync = 0, kNextSync = 1, kClosestSync = 2 };  // MediaExtractor's numbers

    // A host path or an http(s) URL. A file that starts with an EBML header
    // (WebM, Matroska) is read by webm_demux.h, anything else by Media
    // Foundation. Null when neither can read it.
    static std::unique_ptr<Demuxer> open(const std::wstring& path_or_url);
    ~Demuxer();

    int track_count() const;
    const Track& track(int index) const;
    int64_t duration_us() const;
    // No track is selected at first; read() returns samples of selected tracks only.
    void select(int index, bool on);
    // The next sample, in the order the file interleaves them. False at the
    // end of every selected track, and on a read error.
    bool read(Sample* out);
    // The next read starts at a sync sample chosen by `mode` around `us`.
    bool seek(int64_t us, SeekMode mode);

private:
    struct Impl;
    explicit Demuxer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace winapk::video
