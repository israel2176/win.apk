// print_layout_test.cpp -- winapk-print-layout-test: where a printed PDF page goes on the printer's
// paper and in what order pages and copies are sent (host/print_layout.h). No Windows call: it builds
// and runs anywhere.
//
// Checks: the sheet in mils with its unprintable edges as margins; a page laid out for the paper is
// drawn one to one, shifted by the unprintable edge; a page of another size is scaled to fit and
// centred; the picture is no finer than 600 dpi; the bands cover every device row once and stay
// under their byte budget; collated and uncollated copies; pages past the end left out.

#include <cmath>
#include <cstdio>
#include <vector>

#include "print_layout.h"

using namespace winapk::host;

namespace {

int g_failures = 0;
void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}
bool near(double a, double b, double tol = 0.51) { return std::fabs(a - b) <= tol; }

// A4 on a 600 dpi printer with 1/6 inch unprintable on every side.
PrinterGeometry a4_600() {
    PrinterGeometry g;
    g.dpi_x = g.dpi_y = 600;
    g.paper_w = 4961;  // 210 mm
    g.paper_h = 7016;  // 297 mm
    g.offset_x = g.offset_y = 100;
    g.printable_w = 4961 - 200;
    g.printable_h = 7016 - 200;
    return g;
}

}  // namespace

int main() {
    std::printf("paper in mils\n");
    const PaperMils m = paper_mils(a4_600());
    check("A4 is 8268 x 11693 mils", near(m.width, 8268, 1) && near(m.height, 11693, 1));
    check("1/6 inch edges are 167-mil margins", m.left == 167 && m.top == 167 && m.right == 167 && m.bottom == 167);
    PrinterGeometry none;
    check("no resolution: zero, not a division by zero", paper_mils(none).width == 0);

    std::printf("a page laid out for the paper\n");
    // A4 in DIPs: 210 mm = 793.7 DIPs, 297 mm = 1122.5 DIPs.
    const PagePlacement a = place_page(793.7, 1122.5, a4_600());
    check("drawn one to one", !a.scaled);
    check("at the sheet's corner, before the unprintable edge", near(a.x, -100, 1.5) && near(a.y, -100, 1.5));
    check("its size is the sheet's", near(a.w, 4961, 3) && near(a.h, 7016, 3));
    check("rendered at the printer's 600 dpi", near(a.render_w, a.w, 1) && near(a.render_h, a.h, 1));

    std::printf("a page of another size\n");
    // US Letter (816 x 1056 DIPs) on A4: narrower paper, so the width decides.
    const PagePlacement l = place_page(816, 1056, a4_600());
    check("scaled", l.scaled);
    check("fits the width", near(l.w, 4961, 1) && l.h <= 7016);
    check("centred vertically", near(l.y + 100, (7016 - l.h) / 2.0, 1));
    // A landscape page on portrait paper is fitted, not cut.
    const PagePlacement land = place_page(1122.5, 793.7, a4_600());
    check("landscape page fits the portrait sheet", land.w <= 4961.5 && land.h <= 7016.5 && land.scaled);

    std::printf("render resolution\n");
    PrinterGeometry fine = a4_600();
    fine.dpi_x = fine.dpi_y = 1200;
    fine.paper_w *= 2;
    fine.paper_h *= 2;
    fine.offset_x = fine.offset_y = 200;
    const PagePlacement f = place_page(793.7, 1122.5, fine);
    check("a 1200 dpi printer gets a 600 dpi picture", near(f.render_w, f.w / 2, 1) && near(f.render_h, f.h / 2, 1));
    check("invalid page: nothing", place_page(0, 100, a4_600()).render_w == 0);

    std::printf("bands\n");
    const std::vector<PageBand> bands = page_bands(a, 1122.5);
    bool contiguous = !bands.empty() && bands.front().row == 0;
    bool under_budget = true;
    int rows = 0;
    for (size_t i = 0; i < bands.size(); ++i) {
        rows += bands[i].rows;
        if (static_cast<int64_t>(bands[i].rows) * a.render_w * 4 > kMaxBandBytes) under_budget = false;
        if (i > 0 && bands[i].dst_y != bands[i - 1].dst_y + bands[i - 1].dst_h) contiguous = false;
        if (i > 0 && !near(bands[i].src_y, bands[i - 1].src_y + bands[i - 1].src_h, 1e-6)) contiguous = false;
    }
    check("more than one band for a 600 dpi A4 page", bands.size() > 1);
    check("each band within 32 MB", under_budget);
    check("the bands cover every picture row once", rows == a.render_h);
    check("device rows and page strips follow on without gaps", contiguous);
    check("the last band ends at the page's bottom",
          near(bands.back().dst_y + bands.back().dst_h, a.y + a.h, 1) &&
              near(bands.back().src_y + bands.back().src_h, 1122.5, 1e-6));
    const std::vector<PageBand> one = page_bands(a, 1122.5, int64_t{1} << 40);
    check("a large budget: one band", one.size() == 1 && one[0].rows == a.render_h);

    std::printf("pages and copies\n");
    check("every page once", print_sequence({}, 3, 1, true) == std::vector<int>({0, 1, 2}));
    check("collated copies", print_sequence({}, 3, 2, true) == std::vector<int>({0, 1, 2, 0, 1, 2}));
    check("uncollated copies", print_sequence({}, 3, 2, false) == std::vector<int>({0, 0, 1, 1, 2, 2}));
    check("chosen pages", print_sequence({2, 0}, 3, 1, true) == std::vector<int>({2, 0}));
    check("pages past the end left out", print_sequence({1, 5, -1}, 3, 1, true) == std::vector<int>({1}));
    check("zero copies is one", print_sequence({}, 2, 0, true) == std::vector<int>({0, 1}));

    std::printf(g_failures ? "FAILED (%d)\n" : "ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
