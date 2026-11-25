// frame_times.h -- how long the app's frames take, kept for the whole run and logged once at exit
// (windows/README.md, "Frame time").
//
// The view tree's frames are drawn in software into a bitmap and copied to the window, on AOSP's
// graphics (Skia, through the native runtime). AOSP's views' compositor (aosp_views' Compositor)
// times each frame that reaches the present and hands the times over (com.macapk.loader.FrameTimes,
// native_cocoa.cpp): layout (0 there: ViewRootImpl's layout is inside draw), draw (the frame's
// vsync to the compose: the view tree's layout and draw), pixels (the frame
// read out of its bitmap: on AOSP's graphics a Bitmap.getPixels copy), present (the host copying
// it to the window) and total (the whole frame, the app's animation callbacks and the system bars
// included). They go into fixed histograms, a few hundred kilobytes of address space
// whose untouched pages are never committed, so recording costs one lock and five increments a
// frame and needs no switch. When the process ends, one line:
//
//   [win.apk] frame times (aosp graphics): 1131 frames in 18.9 s, 59.8/s, the first 1.5 s of each
//   window left out; ms median/p90/p99/max: layout 0.10/0.15/0.40/2.10, draw 3.10/4.20/7.90/12.30,
//   pixels 1.00/1.20/2.00/3.00, present 0.80/1.10/2.00/3.10, total 6.50/8.00/12.10/17.00
//
// (one line in the log). The first M6 warm-up frames of every window (its first 90 vsyncs: class
// loading, the JIT's first passes) are left out when later frames exist, else they are what the
// line reports, saying so. Percentiles are nearest-rank over 50 us buckets (the middle of the
// bucket); max is exact. CI's smoke job reads the line of the frame bench (windows/test/framebench).
//
// No Windows call here: test/frame_times_test.cpp drives it.

#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace winapk::host::frame_times {

enum Part { kLayout, kDraw, kPixels, kPresent, kTotal, kParts };
inline const char* const kPartNames[kParts] = {"layout", "draw", "pixels", "present", "total"};

constexpr int64_t kBucketNs = 50'000;  // 50 us
constexpr int kBuckets = 4000;         // up to 200 ms; a longer frame lands in the last bucket (max is exact)

// One set of frames: a histogram per part.
struct Histograms {
    uint32_t counts[kParts][kBuckets] = {};
    int64_t max_ns[kParts] = {};
    uint64_t frames = 0;
    int64_t first_ns = 0, last_ns = 0;  // when the first and the last frame ended (the recorder's clock)

    void add(const int64_t ns[kParts], int64_t now_ns) {
        for (int p = 0; p < kParts; ++p) {
            const int64_t v = ns[p] < 0 ? 0 : ns[p];
            int64_t b = v / kBucketNs;
            if (b >= kBuckets) b = kBuckets - 1;
            ++counts[p][b];
            if (v > max_ns[p]) max_ns[p] = v;
        }
        if (frames++ == 0) first_ns = now_ns;
        last_ns = now_ns;
    }

    // Nearest rank: the smallest value at least q of the frames are at or below, as the middle of
    // its bucket, in ms. Never above the exact max.
    double percentile_ms(Part p, double q) const {
        if (frames == 0) return 0;
        uint64_t rank = static_cast<uint64_t>(q * static_cast<double>(frames) + 0.999999);
        if (rank < 1) rank = 1;
        if (rank > frames) rank = frames;
        uint64_t seen = 0;
        for (int b = 0; b < kBuckets; ++b) {
            seen += counts[p][b];
            if (seen >= rank) {
                const double mid = (static_cast<double>(b) + 0.5) * kBucketNs / 1e6;
                const double max = static_cast<double>(max_ns[p]) / 1e6;
                return mid < max ? mid : max;
            }
        }
        return static_cast<double>(max_ns[p]) / 1e6;
    }
};

// The run's frames: the warm-up ones apart from the rest.
struct Recorder {
    std::mutex lock;
    Histograms steady, warmup;

    void add(int64_t layout_ns, int64_t draw_ns, int64_t pixels_ns, int64_t present_ns, int64_t total_ns,
             bool is_warmup, int64_t now_ns) {
        const int64_t ns[kParts] = {layout_ns, draw_ns, pixels_ns, present_ns, total_ns};
        std::lock_guard<std::mutex> hold(lock);
        (is_warmup ? warmup : steady).add(ns, now_ns);
    }

    // The line (without the "[win.apk] " prefix), or empty when no frame was recorded. `graphics`
    // names the engine ("aosp", the only one now; the line keeps the name for the logs' readers).
    std::string summary(const char* graphics) {
        std::lock_guard<std::mutex> hold(lock);
        const bool use_steady = steady.frames > 0;
        const Histograms& h = use_steady ? steady : warmup;
        if (h.frames == 0) return {};
        std::string line;
        char buf[160];
        const double span_s = static_cast<double>(h.last_ns - h.first_ns) / 1e9;
        std::snprintf(buf, sizeof buf, "frame times (%s graphics): %llu frames in %.1f s", graphics,
                      static_cast<unsigned long long>(h.frames), span_s);
        line += buf;
        if (h.frames > 1 && span_s > 0) {
            std::snprintf(buf, sizeof buf, ", %.1f/s", static_cast<double>(h.frames - 1) / span_s);
            line += buf;
        }
        line += use_steady ? ", the first 1.5 s of each window left out"
                           : ", all in the first 1.5 s of a window (warm-up)";
        line += "; ms median/p90/p99/max:";
        for (int p = 0; p < kParts; ++p) {
            const Part part = static_cast<Part>(p);
            std::snprintf(buf, sizeof buf, "%s %s %.2f/%.2f/%.2f/%.2f", p ? "," : "", kPartNames[p],
                          h.percentile_ms(part, 0.5), h.percentile_ms(part, 0.9), h.percentile_ms(part, 0.99),
                          static_cast<double>(h.max_ns[p]) / 1e6);
            line += buf;
        }
        return line;
    }
};

}  // namespace winapk::host::frame_times
