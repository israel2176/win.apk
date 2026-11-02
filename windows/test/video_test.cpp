// video_test.cpp -- winapk-video-test: video on Media Foundation without a JVM.
//
//   winapk-video-test <dir with color.mp4>
//
// color.mp4 (test/video) is one second of 64x48 H.264 at 25 fps, red in the
// top half and blue in the bottom (so a flipped or mis-converted picture
// fails), with a 440 Hz mono AAC tone; made with ffmpeg's color, drawbox and
// sine sources. Checks: the YUV conversion on known values, in every rotation;
// the source reader decodes the file to the right pictures and sound and
// seeks; the elementary-stream decoder (MediaCodec's) decodes the file's
// access units; the player paces pictures against the sound, seeks, loops and
// completes. Without an audio device the engine keeps time silently, so this
// runs on CI machines.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "../host/video_decode.h"
#include "../host/video_player.h"

namespace vd = winapk::video;

namespace {

int g_failures = 0;

void check(const char* what, bool ok, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++g_failures;
}

std::string hex(uint32_t px) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", px);
    return buf;
}

bool near_color(uint32_t px, int r, int g, int b, int tol = 24) {
    return std::abs(static_cast<int>((px >> 16) & 0xff) - r) <= tol &&
           std::abs(static_cast<int>((px >> 8) & 0xff) - g) <= tol && std::abs(static_cast<int>(px & 0xff) - b) <= tol &&
           (px >> 24) == 0xff;
}

// Red over blue, as the file is: top rows red, bottom rows blue.
bool red_over_blue(const vd::Frame& f) {
    if (f.width < 2 || f.height < 4) return false;
    const uint32_t top = f.argb[static_cast<size_t>(f.height / 4) * f.width + f.width / 2];
    const uint32_t bottom = f.argb[static_cast<size_t>(f.height * 3 / 4) * f.width + f.width / 2];
    return near_color(top, 255, 0, 0) && near_color(bottom, 0, 0, 255);
}

void test_conversion() {
    // A 4x2 NV12 picture: limited-range BT.601 white, black, red and blue
    // (Y,Cb,Cr = 235,128,128 / 16,128,128 / 81,90,240 / 41,240,110), the left
    // pair sharing one chroma sample and the right pair another.
    const uint8_t y[2][4] = {{235, 16, 81, 41}, {235, 16, 81, 41}};
    const uint8_t uv[4] = {128, 128, 128, 128};
    uint32_t out[8];
    vd::yuv420_to_argb(&y[0][0], 4, uv, 4, 4, 2, false, false, false, 0, out);
    check("601 white", near_color(out[0], 255, 255, 255, 4), hex(out[0]));
    check("601 black", near_color(out[1], 0, 0, 0, 4), hex(out[1]));
    // Red (81,90,240) and blue (41,240,110) proper, one pixel each.
    const uint8_t yr[1] = {81}, uvr[2] = {90, 240};
    vd::yuv420_to_argb(yr, 1, uvr, 2, 1, 1, false, false, false, 0, out);
    check("601 red", near_color(out[0], 255, 0, 0, 6), hex(out[0]));
    const uint8_t yb[1] = {41}, uvb[2] = {240, 110};
    vd::yuv420_to_argb(yb, 1, uvb, 2, 1, 1, false, false, false, 0, out);
    check("601 blue", near_color(out[0], 0, 0, 255, 6), hex(out[0]));
    // BT.709 white and full-range black, and a P010 white (235 << 8 in 16 bits).
    const uint8_t y709[1] = {235}, uvn[2] = {128, 128};
    vd::yuv420_to_argb(y709, 1, uvn, 2, 1, 1, false, true, false, 0, out);
    check("709 white", near_color(out[0], 255, 255, 255, 4), hex(out[0]));
    const uint8_t yfull[1] = {0};
    vd::yuv420_to_argb(yfull, 1, uvn, 2, 1, 1, false, false, true, 0, out);
    check("full-range black", near_color(out[0], 0, 0, 0, 2), hex(out[0]));
    const uint8_t y16[2] = {0x00, 0xeb}, uv16[4] = {0x00, 0x80, 0x00, 0x80};
    vd::yuv420_to_argb(y16, 2, uv16, 4, 1, 1, true, false, false, 0, out);
    check("P010 white", near_color(out[0], 255, 255, 255, 4), hex(out[0]));

    // Rotation: a 2x2 picture of distinct greys (16, 81, 145, 235 in reading
    // order), turned.
    const uint8_t yg[2][2] = {{16, 81}, {145, 235}};
    const uint8_t uvg[2] = {128, 128};
    auto grey = [](uint32_t px) { return static_cast<int>(px & 0xff); };
    vd::yuv420_to_argb(&yg[0][0], 2, uvg, 2, 2, 2, false, false, false, 90, out);
    auto about = [&](uint32_t px, int v) { return std::abs(grey(px) - v) <= 6; };
    check("rotate 90", about(out[0], 150) && about(out[1], 0) && about(out[2], 255) && about(out[3], 76),
          hex(out[0]) + " " + hex(out[1]) + " " + hex(out[2]) + " " + hex(out[3]));
    vd::yuv420_to_argb(&yg[0][0], 2, uvg, 2, 2, 2, false, false, false, 180, out);
    check("rotate 180", about(out[0], 255) && about(out[1], 150) && about(out[2], 76) && about(out[3], 0),
          hex(out[0]) + " " + hex(out[1]) + " " + hex(out[2]) + " " + hex(out[3]));
    vd::yuv420_to_argb(&yg[0][0], 2, uvg, 2, 2, 2, false, false, false, 270, out);
    check("rotate 270", about(out[0], 76) && about(out[1], 255) && about(out[2], 0) && about(out[3], 150),
          hex(out[0]) + " " + hex(out[1]) + " " + hex(out[2]) + " " + hex(out[3]));
}

void test_source(const std::wstring& path) {
    auto src = vd::Source::open(path);
    check("source opens", src != nullptr);
    if (!src) return;
    char detail[256];
    std::snprintf(detail, sizeof(detail), "%dx%d, %lld us, audio %d Hz x%d", src->width(), src->height(),
                  static_cast<long long>(src->duration_us()), src->audio_rate(), src->audio_channels());
    check("source describes the file",
          src->has_video() && src->has_audio() && src->width() == 64 && src->height() == 48 &&
              src->duration_us() > 900000 && src->duration_us() < 1200000 && src->audio_rate() == 44100,
          detail);
    int frames = 0, audio = 0;
    int64_t last_pts = -1;
    bool ordered = true, colours = true;
    float peak = 0;
    for (;;) {
        vd::Source::Packet p;
        if (!src->read(&p)) {
            check("source read error", false);
            break;
        }
        if (p.kind == vd::Source::Packet::kEnd) break;
        if (p.kind == vd::Source::Packet::kVideo) {
            ++frames;
            if (p.frame->pts_us <= last_pts) ordered = false;
            last_pts = p.frame->pts_us;
            if (!red_over_blue(*p.frame)) {
                colours = false;
                if (frames == 1) {
                    std::printf("    first frame: top %s bottom %s\n",
                                hex(p.frame->argb[12 * 64 + 32]).c_str(), hex(p.frame->argb[36 * 64 + 32]).c_str());
                }
            }
        } else {
            ++audio;
            for (float s : p.pcm) peak = std::max(peak, std::fabs(s));
        }
    }
    std::snprintf(detail, sizeof(detail), "%d frames, %d audio packets, peak %.2f", frames, audio, peak);
    check("source decodes 25 frames in order, red over blue, with sound",
          frames == 25 && ordered && colours && audio > 10 && peak > 0.05, detail);
    // Seek to the middle: the next picture is at or after 0.5 s (the key frame
    // before it is decoded through, not delivered).
    check("source seeks", src->seek(500000));
    vd::Source::Packet p;
    while (src->read_video(&p) && p.kind == vd::Source::Packet::kVideo && p.pts_us < 480000) {
    }
    std::snprintf(detail, sizeof(detail), "pts %lld", static_cast<long long>(p.pts_us));
    check("source reads on after a seek", p.kind == vd::Source::Packet::kVideo && p.pts_us >= 480000 &&
                                                  p.pts_us < 700000 && red_over_blue(*p.frame),
          detail);
}

void test_decoder(const std::wstring& path) {
    const std::vector<std::string>& mimes = vd::supported_mimes();
    std::string list;
    for (const std::string& m : mimes) list += (list.empty() ? "" : ", ") + m;
    std::printf("  decoders: %s\n", list.c_str());
    if (std::find(mimes.begin(), mimes.end(), "video/avc") == mimes.end()) {
        std::printf("  [SKIP] elementary-stream decode (no H.264 decoder here)\n");
        return;
    }
    auto src = vd::Source::open(path, false);
    check("compressed source opens", src && src->has_video() && src->video_mime() == "video/avc",
          src ? src->video_mime() : "none");
    if (!src) return;
    auto dec = vd::Decoder::open("video/avc", 64, 48, src->video_csd(), {});
    check("H.264 decoder opens", dec != nullptr);
    if (!dec) return;
    int in = 0, out = 0;
    bool colours = true, eos = false;
    int64_t last_pts = -1;
    bool ordered = true;
    auto drain = [&] {
        for (;;) {
            bool marker = false;
            auto f = dec->poll(&marker);
            if (marker) eos = true;
            if (!f) break;
            ++out;
            if (f->pts_us <= last_pts) ordered = false;
            last_pts = f->pts_us;
            if (!red_over_blue(*f)) colours = false;
        }
    };
    for (;;) {
        vd::Source::Packet p;
        if (!src->read_video(&p) || p.kind != vd::Source::Packet::kVideo) break;
        ++in;
        if (!dec->decode(p.data.data(), p.data.size(), p.pts_us, false)) check("decoder takes an access unit", false);
        drain();
        if (g_failures) break;
    }
    dec->decode(nullptr, 0, 0, true);
    drain();
    char detail[128];
    std::snprintf(detail, sizeof(detail), "%d in, %d out, eos %d, %dx%d", in, out, eos, dec->width(), dec->height());
    check("decoder yields every picture, in order, red over blue, then eos",
          in == 25 && out == 25 && ordered && colours && eos && dec->width() == 64 && dec->height() == 48, detail);
    // Flush, then decode from the start again: the parameter sets are re-sent.
    dec->flush();
    src->seek(0);
    out = 0;
    for (int i = 0; i < 25; ++i) {
        vd::Source::Packet p;
        if (!src->read_video(&p) || p.kind != vd::Source::Packet::kVideo) break;
        dec->decode(p.data.data(), p.data.size(), p.pts_us, false);
        drain();
    }
    dec->decode(nullptr, 0, 0, true);
    drain();
    check("decoder decodes again after a flush", out == 25, std::to_string(out));
}

void test_player(const std::wstring& path) {
    auto player = vd::Player::open(vd::Source::open(path));
    check("player opens", player != nullptr);
    if (!player) return;
    check("player size and duration", player->width() == 64 && player->height() == 48 && player->duration_ms() > 900 &&
                                          player->duration_ms() < 1200);
    bool released = false;
    // Positioned at the start: the first picture comes at once, nothing after.
    auto first = player->await_frame(2000, &released);
    check("player shows the first picture before start", first && first->pts_us == 0 && red_over_blue(*first));
    check("player holds the next picture until start", !player->await_frame(150, &released) && !released);
    check("player is not playing yet", !player->playing() && player->position_ms() == 0);

    const auto t0 = std::chrono::steady_clock::now();
    player->start();
    int frames = 0;
    int64_t last_pts = -1;
    bool ordered = true;
    std::vector<int> errors;  // ms between a picture's pts and when it was handed out
    for (;;) {
        auto f = player->await_frame(1500, &released);
        if (!f) break;
        ++frames;
        if (f->pts_us <= last_pts) ordered = false;
        last_pts = f->pts_us;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        errors.push_back(static_cast<int>(elapsed.count() - f->pts_us / 1000));
    }
    const int worst = errors.empty() ? 0 : *std::max_element(errors.begin(), errors.end(),
                                                               [](int a, int b) { return std::abs(a) < std::abs(b); });
    char detail[256];
    std::snprintf(detail, sizeof(detail), "%d pictures, worst timing error %d ms, last pts %lld", frames, worst,
                  static_cast<long long>(last_pts));
    // The sound buffers a quarter second ahead; a picture may wait for the
    // clock by that much at most, and the machine may be slow.
    check("player paces the pictures to the sound", frames >= 20 && ordered && std::abs(worst) < 400, detail);
    for (int i = 0; i < 100 && player->playing(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::snprintf(detail, sizeof(detail), "position %d ms of %d", player->position_ms(), player->duration_ms());
    check("player completes", !player->playing() && player->position_ms() == player->duration_ms(), detail);

    // A seek while paused shows the picture there; start then plays on.
    player->seek(500);
    auto at = player->await_frame(2000, &released);
    std::snprintf(detail, sizeof(detail), "pts %lld, position %d", at ? static_cast<long long>(at->pts_us) : -1LL,
                  player->position_ms());
    check("player seeks", at && at->pts_us >= 480000 && at->pts_us < 700000 && player->position_ms() >= 480 &&
                              player->position_ms() < 700,
          detail);
    player->start();
    frames = 0;
    while (auto f = player->await_frame(1500, &released)) ++frames;
    check("player plays on from the seek", frames >= 8 && frames <= 14, std::to_string(frames));

    // Looping: back to the start after the end, pictures keep coming.
    player->set_looping(true);
    player->seek(800);
    player->start();
    frames = 0;
    bool wrapped = false;
    last_pts = -1;
    const auto loop_t0 = std::chrono::steady_clock::now();
    const auto until = loop_t0 + std::chrono::milliseconds(1500);
    std::string trace;  // what came out and when, for a failure's report
    while (std::chrono::steady_clock::now() < until) {
        auto f = player->await_frame(500, &released);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - loop_t0);
        if (!f) {
            trace += " null@" + std::to_string(ms.count()) + "(pos " + std::to_string(player->position_ms()) +
                     (player->playing() ? " playing)" : " stopped)");
            continue;
        }
        ++frames;
        if (f->pts_us < last_pts) wrapped = true;
        last_pts = f->pts_us;
        if (frames <= 3 || frames % 10 == 0) trace += " " + std::to_string(f->pts_us / 1000) + "@" + std::to_string(ms.count());
    }
    std::snprintf(detail, sizeof(detail), "%d frames, wrapped %d, playing %d, position %d;%s", frames, wrapped ? 1 : 0,
                  player->playing() ? 1 : 0, player->position_ms(), trace.substr(0, 60).c_str());
    check("player loops", wrapped && frames >= 15 && player->playing(), detail);
    player->pause();
    check("player pauses", !player->playing() && !player->await_frame(100, &released));
    player->release();
    check("player's wait returns on release", !player->await_frame(100, &released) && released);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: winapk-video-test <dir with color.mp4>\n");
        return 2;
    }
    const std::string dir = argv[1];
    const std::wstring path(dir.begin(), dir.end());
    std::printf("YUV conversion\n");
    test_conversion();
    // Wine's Media Foundation needs GStreamer for H.264 and AAC, which a bare
    // container lacks; Windows has both.
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) {
        std::printf("  [SKIP] source, decoder and player (Wine)\n");
    } else {
        std::printf("Source reader\n");
        test_source(path + L"\\color.mp4");
        std::printf("Elementary-stream decoder\n");
        test_decoder(path + L"\\color.mp4");
        std::printf("Player\n");
        test_player(path + L"\\color.mp4");
    }
    std::printf("%s\n", g_failures ? "FAILED" : "OK");
    return g_failures ? 1 : 0;
}
