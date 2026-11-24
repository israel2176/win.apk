// soft_codecs_test.cpp -- winapk-soft-codecs-test: the software decoders (host/soft_codecs.h),
// the picture conversions (host/yuv_convert.cpp) and the WebM reader (host/webm_demux.h) on their
// own, with no Media Foundation, so the same checks hold on any Windows, and on Linux
// (test/soft_codecs/CMakeLists.txt: under AddressSanitizer there).
//
//   winapk-soft-codecs-test <dir with the test/video files>
//
// The files are made by test/video/make_soft_codec_files.sh (it says what each holds): one
// second of 64x48 video at 25 fps, red in the top half and blue in the bottom, a key frame every
// ten, in VP8, VP9 (profiles 0, 1 and 2, and at 63x47), AV1 (8-bit, and 10-bit tagged PQ), with
// a mono Opus tone beside the first, and a 5.1 Opus file with a tone of its own in each channel.
// Checks: tracks and formats; every picture decoded, the right way up and coloured, at its own
// time; the pictures as ARGB, I420 and P010 (crop, stride, dither); the colour the stream
// declares; pooled picture memory reused and alive past its decoder; flush and seek; Opus' pre-skip,
// pre-roll, gain-free level and surround channel order; and corrupt or cut input refused without
// a crash.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../host/soft_codecs.h"
#include "../host/video_decode.h"
#include "../host/webm_demux.h"

namespace vd = winapk::video;

namespace {

int g_failures = 0;

void check(const char* what, bool ok, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++g_failures;
}

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08x", v);
    return b;
}

bool near_color(uint32_t px, int r, int g, int b, int tol = 40) {
    return std::abs(static_cast<int>((px >> 16) & 0xff) - r) <= tol &&
           std::abs(static_cast<int>((px >> 8) & 0xff) - g) <= tol &&
           std::abs(static_cast<int>(px & 0xff) - b) <= tol && (px >> 24) == 0xff;
}

// Red in the top half, blue in the bottom, sampled a quarter in from each edge.
bool red_over_blue(const vd::Frame& f) {
    const std::vector<uint32_t> px = vd::frame_argb(f);
    if (px.size() != static_cast<size_t>(f.width) * f.height || f.width < 4 || f.height < 4) return false;
    return near_color(px[static_cast<size_t>(f.height / 4) * f.width + f.width / 2], 255, 0, 0) &&
           near_color(px[static_cast<size_t>(f.height * 3 / 4) * f.width + f.width / 2], 0, 0, 255);
}

std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::wstring path_of(const std::string& dir, const char* name) { return wide(dir + "/" + name); }

int track_of(const vd::WebmReader& r, const char* mime) {
    for (size_t i = 0; i < r.tracks().size(); ++i) {
        if (r.tracks()[i].mime == mime) return static_cast<int>(i);
    }
    return -1;
}

std::vector<vd::Demuxer::Sample> all_samples(vd::WebmReader& r, int track) {
    r.select(track, true);
    std::vector<vd::Demuxer::Sample> out;
    vd::Demuxer::Sample s;
    while (r.next(&s)) out.push_back(s);
    return out;
}

// ---- Pictures ----------------------------------------------------------------------------------

// The I420 and P010 forms of a red-over-blue frame: BT.601 limited range Y' is 81 for red and
// 41 for blue (8 bits), and the layout's crop, stride and chroma are as YuvLayout says.
void yuv_forms(const vd::Frame& f, const char* label) {
    const vd::YuvLayout i420 = vd::YuvLayout::of(f, vd::YuvLayout::kI420);
    std::vector<uint8_t> buf(i420.size());
    const bool ok = vd::frame_to_yuv(f, i420, buf.data());
    const int top = buf[static_cast<size_t>(f.height / 4) * i420.stride + f.width / 2];
    const int bottom = buf[static_cast<size_t>(f.height * 3 / 4) * i420.stride + f.width / 2];
    const uint8_t* cr = buf.data() + static_cast<size_t>(i420.stride) * i420.slice_height +
                        static_cast<size_t>(i420.stride / 2) * (i420.slice_height / 2);
    const int red_cr = cr[static_cast<size_t>(f.height / 8) * (i420.stride / 2) + f.width / 4];
    std::string what = std::string(label) + ": I420, stride " + std::to_string(i420.stride) + ", slice " +
                       std::to_string(i420.slice_height);
    check(what.c_str(),
          ok && i420.stride % 16 == 0 && i420.stride >= f.width && i420.slice_height >= f.height &&
              std::abs(top - 81) <= 12 && std::abs(bottom - 41) <= 12 && red_cr > 200,
          "Y' " + std::to_string(top) + "/" + std::to_string(bottom) + ", red Cr " + std::to_string(red_cr));
    const vd::YuvLayout p010 = vd::YuvLayout::of(f, vd::YuvLayout::kP010);
    std::vector<uint8_t> wide_buf(p010.size());
    const bool ok10 = vd::frame_to_yuv(f, p010, wide_buf.data());
    auto at16 = [&](size_t byte) { return wide_buf[byte] | wide_buf[byte + 1] << 8; };
    const int top10 = at16(static_cast<size_t>(f.height / 4) * p010.stride + (f.width / 2) * 2);
    const int cr10 = at16(static_cast<size_t>(p010.stride) * p010.slice_height +
                          static_cast<size_t>(f.height / 8) * p010.stride + (f.width / 4) * 4 + 2);
    what = std::string(label) + ": P010 (value in the top 10 bits)";
    check(what.c_str(), ok10 && (top10 & 0x3f) == 0 && std::abs((top10 >> 8) - 81) <= 12 && (cr10 >> 8) > 200,
          "Y' " + std::to_string(top10 >> 6) + " of 1023, Cr " + std::to_string(cr10 >> 6));
}

struct VideoCase {
    const char* file;
    const char* mime;
    int width, height, bit_depth, shift_x;
};

void video_file(const std::string& dir, const VideoCase& c) {
    std::printf("%s (%s)\n", c.file, c.mime);
    const std::wstring path = path_of(dir, c.file);
    check("starts with an EBML header", vd::WebmReader::is_ebml(path));
    auto r = vd::WebmReader::open(path);
    check("opens", r != nullptr);
    if (!r) return;
    const int v = track_of(*r, c.mime);
    check("has the video track", v >= 0);
    if (v < 0) return;
    const vd::Demuxer::Track t = r->tracks()[static_cast<size_t>(v)];
    check("its size", t.video && t.width == c.width && t.height == c.height,
          std::to_string(t.width) + "x" + std::to_string(t.height));
    check("its duration", r->duration_us() > 900000 && r->duration_us() < 1100000, std::to_string(r->duration_us()));
    const std::vector<vd::Demuxer::Sample> samples = all_samples(*r, v);
    int keys = 0;
    for (const auto& s : samples) keys += s.key ? 1 : 0;
    check("25 samples, key frames among them", samples.size() == 25 && keys >= 2 && keys < 25,
          std::to_string(samples.size()) + " samples, " + std::to_string(keys) + " key");

    auto dec = vd::soft::open_video(c.mime);
    check("a decoder opens", dec != nullptr);
    if (!dec) return;
    std::vector<std::shared_ptr<vd::Frame>> frames;
    bool decoded = true;
    for (const auto& s : samples) decoded = dec->decode(s.data.data(), s.data.size(), s.pts_us, &frames) && decoded;
    dec->drain(&frames);
    check("every sample decodes", decoded);
    check("25 pictures", frames.size() == 25, std::to_string(frames.size()));
    bool colours = !frames.empty(), times = true, shape = true;
    for (size_t i = 0; i < frames.size(); ++i) {
        const vd::Frame& f = *frames[i];
        colours = colours && red_over_blue(f);
        times = times && f.pts_us == static_cast<int64_t>(i) * 40000;
        shape = shape && f.picture && f.argb.empty() && f.width == c.width && f.height == c.height &&
                f.picture->bit_depth == c.bit_depth && f.picture->chroma_shift_x == c.shift_x;
    }
    std::string px;
    if (!frames.empty()) {
        const std::vector<uint32_t> a = vd::frame_argb(*frames[0]);
        if (a.size() == static_cast<size_t>(c.width) * c.height) {
            px = "top " + hex(a[static_cast<size_t>(c.height / 4) * c.width + c.width / 2]) + " bottom " +
                 hex(a[static_cast<size_t>(c.height * 3 / 4) * c.width + c.width / 2]);
        }
    }
    check("held in the decoder's own planes, of the stream's depth and sampling", shape);
    check("red over blue in every picture", colours, px);
    check("presentation times 40 ms apart", times);
    if (!frames.empty()) yuv_forms(*frames.back(), "the last picture");

    // The pictures outlive their decoder: the pool's memory stays theirs (ASan would see a
    // use after free).
    dec.reset();
    bool alive = !frames.empty();
    for (const auto& f : frames) alive = alive && red_over_blue(*f);
    check("pictures stay valid after the decoder is gone", alive);
    frames.clear();

    // A steady stream reuses its picture memory: decoding it again while letting each picture
    // go touches only a few buffers.
    dec = vd::soft::open_video(c.mime);
    std::set<const uint8_t*> planes;
    for (const auto& s : samples) {
        std::vector<std::shared_ptr<vd::Frame>> out;
        dec->decode(s.data.data(), s.data.size(), s.pts_us, &out);
        for (const auto& f : out) planes.insert(f->picture->plane[0]);
    }
    check("picture memory is reused", !planes.empty() && planes.size() <= 16,
          std::to_string(planes.size()) + " distinct buffers for 25 pictures");

    // Flush mid-stream, then a key frame: only what follows comes out.
    std::vector<std::shared_ptr<vd::Frame>> after;
    dec->flush();
    size_t key = 0;
    for (size_t i = samples.size() / 2; i < samples.size(); ++i) {
        if (samples[i].key) {
            key = i;
            break;
        }
    }
    for (size_t i = key; i < samples.size(); ++i)
        dec->decode(samples[i].data.data(), samples[i].data.size(), samples[i].pts_us, &after);
    dec->drain(&after);
    check("after a flush, decoding resumes at a key frame",
          key > 0 && after.size() == samples.size() - key && after[0]->pts_us == samples[key].pts_us &&
              red_over_blue(*after.back()),
          std::to_string(after.size()) + " pictures from sample " + std::to_string(key));

    // A seek lands on the key frame at or before the time.
    check("seeks", r->seek(500000));
    vd::Demuxer::Sample s;
    const bool got = r->next(&s);
    check("to a key frame at or before the time", got && s.key && s.pts_us <= 500000 && s.pts_us >= 200000,
          got ? std::to_string(s.pts_us) : "nothing read");
    auto fresh = vd::soft::open_video(c.mime);
    std::vector<std::shared_ptr<vd::Frame>> one;
    if (got && fresh) {
        fresh->decode(s.data.data(), s.data.size(), s.pts_us, &one);
        fresh->drain(&one);
    }
    check("which decodes on its own", one.size() == 1 && red_over_blue(*one[0]));
}

// AV1 tagged BT.2020 PQ, 10-bit: the decoder reports what the sequence header says, the reader
// what the Colour element says; P010 keeps the ten bits, I420 dithers them down.
void hdr_file(const std::string& dir) {
    std::printf("color_av1_hdr.webm (colour)\n");
    auto r = vd::WebmReader::open(path_of(dir, "color_av1_hdr.webm"));
    if (!r) {
        check("opens", false);
        return;
    }
    const int v = track_of(*r, "video/av01");
    if (v < 0) {
        check("has the AV1 track", false);
        return;
    }
    const vd::ColorAspects tc = r->tracks()[static_cast<size_t>(v)].color;
    check("the track's Colour: BT.2020, PQ, limited",
          tc.standard == vd::ColorAspects::kBt2020 && tc.transfer == vd::ColorAspects::kSt2084 &&
              tc.range == vd::ColorAspects::kLimited,
          std::to_string(tc.standard) + "/" + std::to_string(tc.transfer) + "/" + std::to_string(tc.range));
    auto dec = vd::soft::open_video("video/av01");
    std::vector<std::shared_ptr<vd::Frame>> frames;
    for (const auto& s : all_samples(*r, v)) dec->decode(s.data.data(), s.data.size(), s.pts_us, &frames);
    dec->drain(&frames);
    if (frames.empty()) {
        check("decodes", false);
        return;
    }
    const vd::ColorAspects pc = vd::frame_color(*frames[0]);
    check("the picture's: BT.2020, PQ, limited, 10-bit",
          pc.standard == vd::ColorAspects::kBt2020 && pc.transfer == vd::ColorAspects::kSt2084 &&
              pc.range == vd::ColorAspects::kLimited && frames[0]->picture->bit_depth == 10,
          std::to_string(pc.standard) + "/" + std::to_string(pc.transfer) + "/" + std::to_string(pc.range));
}

// The conversions on made-up pictures: matrices, ranges, depths, subsampling.
struct TestPicture : vd::Picture {
    std::vector<uint16_t> y16, u16, v16;
    std::vector<uint8_t> y8, u8, v8;
};

std::shared_ptr<vd::Frame> flat_frame(int w, int h, int depth, int Y, int U, int V, int standard, int range) {
    auto p = std::make_shared<TestPicture>();
    p->width = w;
    p->height = h;
    p->bit_depth = depth;
    p->color.standard = standard;
    p->color.range = range;
    const size_t luma = static_cast<size_t>(w) * h, chroma = static_cast<size_t>((w + 1) / 2) * ((h + 1) / 2);
    if (depth > 8) {
        p->y16.assign(luma, static_cast<uint16_t>(Y));
        p->u16.assign(chroma, static_cast<uint16_t>(U));
        p->v16.assign(chroma, static_cast<uint16_t>(V));
        p->plane[0] = reinterpret_cast<const uint8_t*>(p->y16.data());
        p->plane[1] = reinterpret_cast<const uint8_t*>(p->u16.data());
        p->plane[2] = reinterpret_cast<const uint8_t*>(p->v16.data());
        p->stride[0] = w * 2;
        p->stride[1] = p->stride[2] = (w + 1) / 2 * 2;
    } else {
        p->y8.assign(luma, static_cast<uint8_t>(Y));
        p->u8.assign(chroma, static_cast<uint8_t>(U));
        p->v8.assign(chroma, static_cast<uint8_t>(V));
        p->plane[0] = p->y8.data();
        p->plane[1] = p->u8.data();
        p->plane[2] = p->v8.data();
        p->stride[0] = w;
        p->stride[1] = p->stride[2] = (w + 1) / 2;
    }
    auto f = std::make_shared<vd::Frame>();
    f->width = w;
    f->height = h;
    f->picture = p;
    return f;
}

void conversions() {
    std::printf("conversions\n");
    using C = vd::ColorAspects;
    auto one = [](const vd::Frame& f) { return vd::frame_argb(f)[0]; };
    check("BT.601 limited white",
          near_color(one(*flat_frame(2, 2, 8, 235, 128, 128, 0, C::kLimited)), 255, 255, 255, 2));
    check("BT.601 limited red",
          near_color(one(*flat_frame(2, 2, 8, 81, 90, 240, C::kBt601Ntsc, C::kLimited)), 255, 0, 0, 4),
          hex(one(*flat_frame(2, 2, 8, 81, 90, 240, C::kBt601Ntsc, C::kLimited))));
    // BT.709 red: Y' 63, Cb 102, Cr 240 (limited); BT.2020 red: 74, 97, 240.
    check("BT.709 limited red",
          near_color(one(*flat_frame(2, 2, 8, 63, 102, 240, C::kBt709, C::kLimited)), 255, 0, 0, 5),
          hex(one(*flat_frame(2, 2, 8, 63, 102, 240, C::kBt709, C::kLimited))));
    check("BT.2020 limited red",
          near_color(one(*flat_frame(2, 2, 8, 74, 97, 240, C::kBt2020, C::kLimited)), 255, 0, 0, 5),
          hex(one(*flat_frame(2, 2, 8, 74, 97, 240, C::kBt2020, C::kLimited))));
    check("full-range black and white",
          near_color(one(*flat_frame(2, 2, 8, 0, 128, 128, C::kBt709, C::kFull)), 0, 0, 0, 1) &&
              near_color(one(*flat_frame(2, 2, 8, 255, 128, 128, C::kBt709, C::kFull)), 255, 255, 255, 1));
    check("10-bit limited white (940)",
          near_color(one(*flat_frame(2, 2, 10, 940, 512, 512, 0, C::kLimited)), 255, 255, 255, 2));
    check("12-bit full-range mid grey",
          near_color(one(*flat_frame(2, 2, 12, 2048, 2048, 2048, C::kBt709, C::kFull)), 128, 128, 128, 2));

    // Dither: a flat 10-bit grey between two 8-bit levels comes out as a mix of both, averaging
    // the true value; I420 the same. An 8-bit picture is not dithered.
    const auto grey = flat_frame(16, 16, 10, 514, 512, 512, C::kBt709, C::kFull);  // 514 / 1023 * 255 = 128.1
    const std::vector<uint32_t> px = vd::frame_argb(*grey);
    std::set<int> levels;
    double sum = 0;
    for (uint32_t p : px) levels.insert(static_cast<int>(p & 0xff)), sum += p & 0xff;
    check("10-bit to ARGB is dithered", levels.size() == 2 && std::abs(sum / px.size() - 128.1) < 0.3,
          std::to_string(levels.size()) + " levels, mean " + std::to_string(sum / px.size()));
    const vd::YuvLayout l = vd::YuvLayout::of(*grey, vd::YuvLayout::kI420);
    std::vector<uint8_t> buf(l.size());
    vd::frame_to_yuv(*grey, l, buf.data());
    double ysum = 0;
    std::set<int> ylevels;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x)
            ysum += buf[static_cast<size_t>(y) * l.stride + x],
                ylevels.insert(buf[static_cast<size_t>(y) * l.stride + x]);
    }
    check("10-bit to I420 is dithered", ylevels.size() == 2 && std::abs(ysum / 256 - 128.5) < 0.2,
          "mean " + std::to_string(ysum / 256));
    const std::vector<uint32_t> flat8 = vd::frame_argb(*flat_frame(8, 8, 8, 128, 128, 128, C::kBt709, C::kFull));
    check("8-bit is not", std::all_of(flat8.begin(), flat8.end(), [&](uint32_t p) { return p == flat8[0]; }));

    // 4:4:4 averaged down to 4:2:0: a 2x2 block of two chroma values gives their mean.
    auto f444 = flat_frame(2, 2, 8, 128, 100, 128, C::kBt709, C::kLimited);
    auto* p = const_cast<TestPicture*>(static_cast<const TestPicture*>(f444->picture.get()));
    p->chroma_shift_x = p->chroma_shift_y = 0;
    p->u8 = {100, 120, 100, 120};
    p->v8 = {128, 128, 128, 128};
    p->plane[1] = p->u8.data();
    p->plane[2] = p->v8.data();
    p->stride[1] = p->stride[2] = 2;
    const vd::YuvLayout l444 = vd::YuvLayout::of(*f444, vd::YuvLayout::kI420);
    std::vector<uint8_t> b444(l444.size());
    vd::frame_to_yuv(*f444, l444, b444.data());
    check("4:4:4 chroma averaged to 4:2:0", b444[static_cast<size_t>(l444.stride) * l444.slice_height] == 110,
          std::to_string(b444[static_cast<size_t>(l444.stride) * l444.slice_height]));

    // A picture of a million pixels or more is converted in bands on several threads: every row
    // comes out as it would on its own.
    {
        const int w = 1280, h = 1024;
        auto big = flat_frame(w, h, 8, 16, 128, 128, C::kBt709, C::kLimited);
        auto* bp = const_cast<TestPicture*>(static_cast<const TestPicture*>(big->picture.get()));
        for (int y = 0; y < h; ++y) std::fill_n(bp->y8.begin() + static_cast<ptrdiff_t>(y) * w, w, 16 + y % 220);
        const std::vector<uint32_t> all = vd::frame_argb(*big);
        bool rows = all.size() == static_cast<size_t>(w) * h;
        for (int y = 0; y < h && rows; ++y) {
            const uint32_t want = one(*flat_frame(2, 2, 8, 16 + y % 220, 128, 128, C::kBt709, C::kLimited));
            rows = all[static_cast<size_t>(y) * w] == want && all[static_cast<size_t>(y) * w + w - 1] == want;
        }
        check("a large picture, converted in bands, row for row", rows);
    }

    // An ARGB frame (Media Foundation's) to I420: BT.601 limited, as frame_color says.
    vd::Frame argb;
    argb.width = argb.height = 2;
    argb.argb.assign(4, 0xffff0000u);
    const vd::YuvLayout la = vd::YuvLayout::of(argb, vd::YuvLayout::kI420);
    std::vector<uint8_t> ba(la.size());
    vd::frame_to_yuv(argb, la, ba.data());
    const vd::ColorAspects ca = vd::frame_color(argb);
    check("ARGB red to BT.601 limited I420",
          std::abs(ba[0] - 81) <= 1 && ca.standard == C::kBt601Ntsc && ca.range == C::kLimited, std::to_string(ba[0]));
}

// ---- Opus --------------------------------------------------------------------------------------

int zero_crossings(const std::vector<int16_t>& pcm, int channels, int channel) {
    int n = 0;
    for (size_t i = static_cast<size_t>(channels + channel); i < pcm.size(); i += static_cast<size_t>(channels)) {
        n += (pcm[i - static_cast<size_t>(channels)] < 0) != (pcm[i] < 0) ? 1 : 0;
    }
    return n;
}

void opus_file(const std::string& dir) {
    std::printf("color.webm (audio/opus)\n");
    auto r = vd::WebmReader::open(path_of(dir, "color.webm"));
    if (!r) {
        check("opens", false);
        return;
    }
    const int a = track_of(*r, "audio/opus");
    check("has the Opus track", a >= 0);
    if (a < 0) return;
    const vd::Demuxer::Track t = r->tracks()[static_cast<size_t>(a)];
    check("its format", !t.video && t.sample_rate == 48000 && t.channels == 1,
          std::to_string(t.sample_rate) + " Hz, " + std::to_string(t.channels) + " channel(s)");
    check("csd-0 is the OpusHead", t.csd0.size() >= 19 && std::memcmp(t.csd0.data(), "OpusHead", 8) == 0);
    check("csd-1 and csd-2 are 8 bytes each", t.csd1.size() == 8 && t.csd2.size() == 8);
    auto dec = vd::soft::OpusDecoder::open(t.csd0, t.channels, t.csd1, t.csd2);
    check("a decoder opens", dec != nullptr);
    if (!dec) return;
    const int head_skip = t.csd0[10] | t.csd0[11] << 8;
    check("pre-skip from csd-1, as the OpusHead's", dec->pre_skip() == head_skip && dec->seek_preroll() == 3840,
          std::to_string(dec->pre_skip()) + " and " + std::to_string(dec->seek_preroll()));
    check("mono's channel mask", dec->channel_mask() == 0x4);
    const std::vector<vd::Demuxer::Sample> samples = all_samples(*r, a);
    std::vector<int16_t> pcm;
    for (const auto& s : samples) dec->decode(s.data.data(), s.data.size(), &pcm);
    check("about a second of sound, the pre-skip dropped", pcm.size() > 46000 && pcm.size() < 50000,
          std::to_string(pcm.size()) + " samples");
    int peak = 0;
    for (int16_t v : pcm) peak = std::max(peak, std::abs(static_cast<int>(v)));
    const int crossings = zero_crossings(pcm, 1, 0);
    check("a 440 Hz tone", crossings > 800 && crossings < 960 && peak > 2000,
          std::to_string(crossings) + " zero crossings, peak " + std::to_string(peak));

    // A flush drops the seek pre-roll from what follows.
    dec->flush();
    std::vector<int16_t> again;
    for (size_t i = samples.size() / 2; i < samples.size(); ++i)
        dec->decode(samples[i].data.data(), samples[i].data.size(), &again);
    // The same packets on a new decoder lose its pre-skip instead.
    std::vector<int16_t> fresh_pcm;
    auto fresh = vd::soft::OpusDecoder::open(t.csd0, t.channels, t.csd1, t.csd2);
    for (size_t i = samples.size() / 2; i < samples.size(); ++i) {
        fresh->decode(samples[i].data.data(), samples[i].data.size(), &fresh_pcm);
    }
    check("after a flush, the seek pre-roll (80 ms) is dropped", fresh_pcm.size() + head_skip == again.size() + 3840,
          std::to_string(again.size()) + " after the flush, " + std::to_string(fresh_pcm.size()) + " fresh");

    // Android's marked csd-0 carries the same three in one buffer.
    std::vector<uint8_t> marked;
    auto mark = [&](const char* tag, const std::vector<uint8_t>& body) {
        marked.insert(marked.end(), tag, tag + 8);
        for (int i = 0; i < 8; ++i)
            marked.push_back(static_cast<uint8_t>(static_cast<uint64_t>(body.size()) >> (8 * i)));
        marked.insert(marked.end(), body.begin(), body.end());
    };
    mark("AOPUSHDR", t.csd0);
    mark("AOPUSDLY", t.csd1);
    std::vector<uint8_t> preroll(8, 0);
    preroll[0] = 0x00, preroll[1] = 0x2d, preroll[2] = 0x31, preroll[3] = 0x01;  // 20000000 ns: 20 ms
    mark("AOPUSPRL", preroll);
    auto m = vd::soft::OpusDecoder::open(marked, 0);
    check("Android's marked csd-0", m && m->channels() == 1 && m->pre_skip() == head_skip && m->seek_preroll() == 960,
          m ? std::to_string(m->seek_preroll()) : "refused");
    check("a header of another kind is refused", !vd::soft::OpusDecoder::open({'O', 'p', 'u', 's'}, 2));
    check("no header: a plain stereo stream", vd::soft::OpusDecoder::open({}, 2) != nullptr);
}

void surround_file(const std::string& dir) {
    std::printf("surround.webm (5.1 Opus)\n");
    auto r = vd::WebmReader::open(path_of(dir, "surround.webm"));
    const int a = r ? track_of(*r, "audio/opus") : -1;
    if (a < 0) {
        check("opens with an Opus track", false);
        return;
    }
    const vd::Demuxer::Track t = r->tracks()[static_cast<size_t>(a)];
    auto dec = vd::soft::OpusDecoder::open(t.csd0, t.channels, t.csd1, t.csd2);
    check("six channels, family 1, 5.1's mask",
          dec && dec->channels() == 6 && t.csd0[18] == 1 && dec->channel_mask() == 0xfc);
    if (!dec) return;
    std::vector<int16_t> pcm;
    for (const auto& s : all_samples(*r, a)) dec->decode(s.data.data(), s.data.size(), &pcm);
    // FL FR FC LFE BL BR: 300, 500, 700, 60, 900, 1100 Hz, about 2 crossings per Hz.
    const int hz[6] = {300, 500, 700, 60, 900, 1100};
    std::string got;
    bool order = pcm.size() > 6 * 40000;
    for (int ch = 0; ch < 6; ++ch) {
        const int n = zero_crossings(pcm, 6, ch);
        got += std::to_string(n / 2) + " ";
        order = order && std::abs(n / 2 - hz[ch]) <= hz[ch] / 8 + 8;
    }
    check("each tone in its Android channel (FL FR FC LFE BL BR)", order, got + "Hz");
}

// ---- Damage ------------------------------------------------------------------------------------

// Corrupt access units and cut files: refused or decoded as far as they go, never a crash, and
// the decoder still good for the real stream afterwards.
void damage(const std::string& dir) {
    std::printf("damaged input\n");
    std::mt19937 rng(1234);
    const VideoCase files[] = {{"color.webm", "video/x-vnd.on2.vp9", 0, 0, 0, 0},
                               {"color_vp8.webm", "video/x-vnd.on2.vp8", 0, 0, 0, 0},
                               {"color_vp9_10bit.webm", "video/x-vnd.on2.vp9", 0, 0, 0, 0},
                               {"color_av1.webm", "video/av01", 0, 0, 0, 0}};
    for (const VideoCase& c : files) {
        auto r = vd::WebmReader::open(path_of(dir, c.file));
        const int v = r ? track_of(*r, c.mime) : -1;
        if (v < 0) continue;
        const std::vector<vd::Demuxer::Sample> samples = all_samples(*r, v);
        auto dec = vd::soft::open_video(c.mime);
        std::vector<std::shared_ptr<vd::Frame>> junk;
        int refused = 0;
        for (int round = 0; round < 40; ++round) {
            std::vector<uint8_t> d = samples[static_cast<size_t>(round) % samples.size()].data;
            switch (round % 4) {
                case 0:  // bytes flipped
                    for (int i = 0; i < 8; ++i) d[rng() % d.size()] ^= static_cast<uint8_t>(1 + rng() % 255);
                    break;
                case 1: d.resize(d.size() / 2 + 1); break;  // cut short
                case 2:                                     // noise
                    for (auto& b : d) b = static_cast<uint8_t>(rng());
                    break;
                default: d.assign(1 + rng() % 4, 0xff); break;  // a few bytes
            }
            refused += dec->decode(d.data(), d.size(), round * 40000, &junk) ? 0 : 1;
            for (const auto& f : junk) vd::frame_argb(*f);  // whatever came out converts
            junk.clear();
        }
        dec->drain(&junk);
        junk.clear();
        dec->flush();
        std::vector<std::shared_ptr<vd::Frame>> good;
        for (const auto& s : samples) dec->decode(s.data.data(), s.data.size(), s.pts_us, &good);
        dec->drain(&good);
        const std::string what = std::string(c.file) + ": corrupt units, then the stream again";
        check(what.c_str(), refused > 0 && good.size() == 25 && red_over_blue(*good.back()),
              std::to_string(refused) + " of 40 refused, then " + std::to_string(good.size()) + " pictures");
    }

    // A cut file reads up to the cut; a file of noise behind an EBML magic is refused.
    std::ifstream in(std::filesystem::path(dir) / "color.webm", std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "winapk-soft-codecs-test.webm";
    bool cut_ok = true;
    int read_total = 0;
    for (size_t len : {bytes.size() * 3 / 4, bytes.size() / 2, size_t{600}, size_t{60}}) {
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(len));
        }
        auto r = vd::WebmReader::open(tmp.wstring());
        if (!r) continue;
        for (size_t i = 0; i < r->tracks().size(); ++i) r->select(static_cast<int>(i), true);
        vd::Demuxer::Sample s;
        while (r->next(&s)) ++read_total;
        cut_ok = cut_ok && r->seek(700000) | true;
    }
    check("cut files read up to the cut", cut_ok && read_total > 0, std::to_string(read_total) + " samples in all");
    std::vector<char> noise(4096);
    for (auto& b : noise) b = static_cast<char>(rng());
    noise[0] = 0x1a, noise[1] = 0x45, noise[2] = static_cast<char>(0xdf), noise[3] = static_cast<char>(0xa3);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(noise.data(), static_cast<std::streamsize>(noise.size()));
    }
    check("noise behind an EBML header is refused", vd::WebmReader::open(tmp.wstring()) == nullptr);
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <dir with the test/video files>\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    std::printf("software video mimes:");
    for (const std::string& m : vd::soft::video_mimes()) std::printf(" %s", m.c_str());
    std::printf("\n");
    const std::vector<int> vp9 = vd::soft::video_capabilities("video/x-vnd.on2.vp9");
    check("MediaCodecList tables for each, profiles without HDR",
          vp9.size() == 8 && vp9[0] == 4096 && vp9[4] == 1 && vp9[6] == 4 &&
              vd::soft::video_capabilities("video/av01").size() == 8 &&
              vd::soft::video_capabilities("video/x-vnd.on2.vp8").size() == 6 &&
              vd::soft::video_capabilities("video/avc").empty() && !vd::soft::open_video("video/avc"));
    conversions();
    const VideoCase cases[] = {
        {"color.webm", "video/x-vnd.on2.vp9", 64, 48, 8, 1},
        {"color_vp8.webm", "video/x-vnd.on2.vp8", 64, 48, 8, 1},
        {"color_vp9_10bit.webm", "video/x-vnd.on2.vp9", 64, 48, 10, 1},
        {"color_vp9_444.webm", "video/x-vnd.on2.vp9", 64, 48, 8, 0},
        {"color_vp9_odd.webm", "video/x-vnd.on2.vp9", 63, 47, 8, 1},
        {"color_av1.webm", "video/av01", 64, 48, 8, 1},
    };
    for (const VideoCase& c : cases) video_file(dir, c);
    hdr_file(dir);
    opus_file(dir);
    surround_file(dir);
    damage(dir);
    // Not a WebM: refused, not misread.
    check("an MP4 is not taken for a WebM", !vd::WebmReader::is_ebml(path_of(dir, "color.mp4")) &&
                                                vd::WebmReader::open(path_of(dir, "color.mp4")) == nullptr);
    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
