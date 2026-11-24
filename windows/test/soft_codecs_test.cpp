// soft_codecs_test.cpp -- winapk-soft-codecs-test: the software decoders
// (host/soft_codecs.h) and the WebM demuxer (host/webm_demux.h) on their own,
// with no Media Foundation, so the same checks hold on any Windows (and build
// on Linux too: neither needs a Windows call).
//
//   winapk-soft-codecs-test <dir with the test/video files>
//
// The files (test/video) are one second of 64x48 video at 25 fps, red in the
// top half and blue in the bottom, a key frame every ten, made with ffmpeg's
// color and drawbox sources: color.webm VP9 (profile 0) with a 440 Hz mono
// Opus tone, color_vp8.webm VP8, color_vp9_10bit.webm VP9 profile 2 (10-bit),
// color_av1.webm AV1. Checks: each file's tracks and formats, every picture
// decoded the right way up and coloured, the Opus tone's length and pitch,
// and seeking to a key frame.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

bool near_color(uint32_t px, int r, int g, int b, int tol = 40) {
    return std::abs(static_cast<int>((px >> 16) & 0xff) - r) <= tol &&
           std::abs(static_cast<int>((px >> 8) & 0xff) - g) <= tol && std::abs(static_cast<int>(px & 0xff) - b) <= tol &&
           (px >> 24) == 0xff;
}

bool red_over_blue(const vd::Frame& f) {
    if (f.width != 64 || f.height != 48 || f.argb.size() != 64u * 48u) return false;
    return near_color(f.argb[8 * 64 + 32], 255, 0, 0) && near_color(f.argb[40 * 64 + 32], 0, 0, 255);
}

std::wstring path_of(const std::string& dir, const char* name) {
    const std::string p = dir + "/" + name;
    return std::wstring(p.begin(), p.end());
}

int track_of(const vd::WebmReader& r, const char* mime) {
    for (size_t i = 0; i < r.tracks().size(); ++i) {
        if (r.tracks()[i].mime == mime) return static_cast<int>(i);
    }
    return -1;
}

// Decodes every picture of a file's video track.
void video_file(const std::string& dir, const char* name, const char* mime) {
    std::printf("%s (%s)\n", name, mime);
    const std::wstring path = path_of(dir, name);
    check("starts with an EBML header", vd::WebmReader::is_ebml(path));
    auto r = vd::WebmReader::open(path);
    check("opens", r != nullptr);
    if (!r) return;
    const int v = track_of(*r, mime);
    check("has the video track", v >= 0);
    if (v < 0) return;
    const vd::Demuxer::Track& t = r->tracks()[static_cast<size_t>(v)];
    check("its size", t.video && t.width == 64 && t.height == 48,
          std::to_string(t.width) + "x" + std::to_string(t.height));
    check("its duration", r->duration_us() > 900000 && r->duration_us() < 1100000, std::to_string(r->duration_us()));
    r->select(v, true);
    auto dec = vd::soft::open_video(mime);
    check("a decoder opens", dec != nullptr);
    if (!dec) return;
    std::vector<std::shared_ptr<vd::Frame>> frames;
    vd::Demuxer::Sample s;
    int samples = 0, keys = 0;
    bool decoded = true;
    while (r->next(&s)) {
        ++samples;
        keys += s.key ? 1 : 0;
        decoded = dec->decode(s.data.data(), s.data.size(), s.pts_us, &frames) && decoded;
    }
    dec->drain(&frames);
    check("25 samples, key frames among them", samples == 25 && keys >= 2 && keys < 25,
          std::to_string(samples) + " samples, " + std::to_string(keys) + " key");
    check("every sample decodes", decoded);
    check("25 pictures", frames.size() == 25, std::to_string(frames.size()));
    bool colours = !frames.empty(), times = true;
    for (size_t i = 0; i < frames.size(); ++i) {
        colours = colours && red_over_blue(*frames[i]);
        times = times && frames[i]->pts_us == static_cast<int64_t>(i) * 40000;
    }
    char px[64] = "";
    if (!frames.empty() && frames[0]->argb.size() == 64u * 48u) {
        std::snprintf(px, sizeof(px), "top %08x bottom %08x", frames[0]->argb[8 * 64 + 32], frames[0]->argb[40 * 64 + 32]);
    }
    check("red over blue in every picture", colours, px);
    check("presentation times 40 ms apart", times);

    // A seek lands on the key frame at or before the time: the next sample
    // read is a key frame no later than 0.5 s, decodable on its own.
    check("seeks", r->seek(500000));
    const bool got = r->next(&s);
    check("to a key frame at or before the time", got && s.key && s.pts_us <= 500000 && s.pts_us >= 200000,
          got ? std::to_string(s.pts_us) : "nothing read");
    auto fresh = vd::soft::open_video(mime);
    std::vector<std::shared_ptr<vd::Frame>> after;
    if (got && fresh) fresh->decode(s.data.data(), s.data.size(), s.pts_us, &after);
    check("which decodes on its own", after.size() == 1 && red_over_blue(*after[0]));
    dec->flush();
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
    const vd::Demuxer::Track& t = r->tracks()[static_cast<size_t>(a)];
    check("its format", !t.video && t.sample_rate == 48000 && t.channels == 1,
          std::to_string(t.sample_rate) + " Hz, " + std::to_string(t.channels) + " channel(s)");
    check("csd-0 is the OpusHead", t.csd0.size() >= 19 && std::string(t.csd0.begin(), t.csd0.begin() + 8) == "OpusHead");
    check("csd-1 and csd-2 are 8 bytes each", t.csd1.size() == 8 && t.csd2.size() == 8);
    auto dec = vd::soft::OpusDecoder::open(t.csd0, t.channels);
    check("a decoder opens", dec != nullptr);
    if (!dec) return;
    r->select(a, true);
    std::vector<int16_t> pcm;
    vd::Demuxer::Sample s;
    while (r->next(&s)) {
        const std::vector<int16_t> part = dec->decode(s.data.data(), s.data.size());
        pcm.insert(pcm.end(), part.begin(), part.end());
    }
    // One second at 48 kHz, less what the encoder's padding leaves over.
    check("about a second of sound", pcm.size() > 46000 && pcm.size() < 50000, std::to_string(pcm.size()) + " samples");
    // 440 Hz: about 880 sign changes in a second.
    int crossings = 0, peak = 0;
    for (size_t i = 1; i < pcm.size(); ++i) {
        crossings += (pcm[i - 1] < 0) != (pcm[i] < 0) ? 1 : 0;
        peak = std::max(peak, std::abs(static_cast<int>(pcm[i])));
    }
    check("a 440 Hz tone", crossings > 800 && crossings < 960 && peak > 2000,
          std::to_string(crossings) + " zero crossings, peak " + std::to_string(peak));
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
    check("MediaCodecList tables for each", vd::soft::video_capabilities("video/x-vnd.on2.vp9").size() == 8 &&
                                                vd::soft::video_capabilities("video/av01").size() == 8 &&
                                                vd::soft::video_capabilities("video/x-vnd.on2.vp8").size() == 6 &&
                                                vd::soft::video_capabilities("video/avc").empty());
    video_file(dir, "color.webm", "video/x-vnd.on2.vp9");
    video_file(dir, "color_vp8.webm", "video/x-vnd.on2.vp8");
    video_file(dir, "color_vp9_10bit.webm", "video/x-vnd.on2.vp9");
    video_file(dir, "color_av1.webm", "video/av01");
    opus_file(dir);
    // Not a WebM: refused, not misread.
    check("an MP4 is not taken for a WebM", !vd::WebmReader::is_ebml(path_of(dir, "color.mp4")) &&
                                                vd::WebmReader::open(path_of(dir, "color.mp4")) == nullptr);
    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
