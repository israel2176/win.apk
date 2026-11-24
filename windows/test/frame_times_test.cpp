// frame_times_test.cpp -- winapk-frame-times-test: the frame-time record (host/frame_times.h), the
// percentiles and the line logged at exit. No window and no Windows call.
//
//   winapk-frame-times-test

#include <cmath>
#include <cstdio>
#include <string>

#include "frame_times.h"

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

using namespace winapk::host::frame_times;

}  // namespace

int main() {
    std::printf("an empty record\n");
    {
        Recorder r;
        check("no frames, no line", r.summary("aosp").empty());
    }

    std::printf("percentiles: nearest rank, the middle of a 50 us bucket, never above max\n");
    {
        auto* h = new Histograms;  // ~64 KB: not on the stack
        // 1..100 ms, one frame each, in the total; draw a constant 2 ms.
        for (int i = 1; i <= 100; ++i) {
            const int64_t ns[kParts] = {0, 2'000'000, 0, 0, i * 1'000'000LL};
            h->add(ns, i * 16'000'000LL);
        }
        check("100 frames", h->frames == 100);
        check("median of 1..100 ms is the 50th, the middle of its bucket (50.025)",
              near(h->percentile_ms(kTotal, 0.5), 50.025));
        check("p90 is the 90th", near(h->percentile_ms(kTotal, 0.9), 90.025));
        check("p99 is the 99th", near(h->percentile_ms(kTotal, 0.99), 99.025));
        check("p100 is max exactly", near(h->percentile_ms(kTotal, 1.0), 100.0));
        check("a constant: every percentile is the constant, capped at max",
              near(h->percentile_ms(kDraw, 0.5), 2.0) && near(h->percentile_ms(kDraw, 0.99), 2.0));
        check("zero stays zero", near(h->percentile_ms(kLayout, 0.9), 0.0));
        check("span: from the first frame's end to the last's", h->last_ns - h->first_ns == 99 * 16'000'000LL);
        delete h;
    }

    std::printf("a frame over 200 ms\n");
    {
        auto* h = new Histograms;
        const int64_t ns[kParts] = {-5, 0, 0, 0, 900'000'000LL};
        h->add(ns, 0);
        check("lands in the last bucket", h->counts[kTotal][kBuckets - 1] == 1);
        check("max is exact", h->max_ns[kTotal] == 900'000'000LL);
        check("a negative time counts as 0", h->counts[kLayout][0] == 1 && h->max_ns[kLayout] == 0);
        delete h;
    }

    std::printf("the line\n");
    {
        auto* r = new Recorder;
        for (int i = 0; i < 90; ++i) r->add(1'000'000, 50'000'000, 2'000'000, 1'000'000, 60'000'000, true, i * 16'666'667LL);
        std::string warm = r->summary("aosp");
        std::printf("    %s\n", warm.c_str());
        check("only warm-up frames: reported, and said so",
              warm.find("frame times (aosp graphics): 90 frames") == 0 &&
                  warm.find("(warm-up)") != std::string::npos);
        // 61 steady frames 1/60 s apart: 60 intervals in 1.0 s.
        for (int i = 0; i <= 60; ++i) {
            r->add(200'000, 3'400'000, 1'000'000, 800'000, 5'500'000, false, 10'000'000'000LL + i * 16'666'667LL);
        }
        const std::string line = r->summary("aosp");
        std::printf("    %s\n", line.c_str());
        check("the steady frames only", line.find("frame times (aosp graphics): 61 frames in 1.0 s, 60.0/s, "
                                                  "the first 1.5 s of each window left out;") == 0);
        check("draw's median, p90, p99, max", line.find("draw 3.40/3.40/3.40/3.40") != std::string::npos);
        check("every part in order",
              line.find("ms median/p90/p99/max: layout 0.20/0.20/0.20/0.20, draw 3.40/3.40/3.40/3.40, "
                        "pixels 1.00/1.00/1.00/1.00, present 0.80/0.80/0.80/0.80, total 5.50/5.50/5.50/5.50") !=
                  std::string::npos);
        delete r;
    }

    std::printf(g_failures ? "%d FAILED\n" : "ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
