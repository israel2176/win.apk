// print_layout.h -- where a printed PDF page goes on the printer's paper, and in what order the pages
// and copies are sent: the arithmetic alone, with no Windows call, so that test/print_layout_test.cpp
// checks it (native_printing.cpp does the printing).
//
// Android's print UI hands a print service the PDF the app wrote for the paper the user chose, and the
// service puts each page on the sheet. Here the printer is a Windows printer: its device context's
// origin is the corner of the printable area, not of the sheet, so a page laid out for this paper is
// drawn one PDF unit to one unit of paper, shifted by the unprintable edge, and a page of any other
// size is scaled to fit the sheet and centred, as Android's print services do with a document of
// another size. The page is rendered as a picture (Windows.Data.Pdf) in horizontal bands, so that a
// page at the printer's resolution never needs one bitmap of hundreds of megabytes.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace winapk::host {

// What a printer's device context reports (GetDeviceCaps), in device pixels.
struct PrinterGeometry {
    int dpi_x = 0, dpi_y = 0;            // LOGPIXELSX, LOGPIXELSY
    int paper_w = 0, paper_h = 0;        // PHYSICALWIDTH, PHYSICALHEIGHT: the whole sheet
    int offset_x = 0, offset_y = 0;      // PHYSICALOFFSETX, PHYSICALOFFSETY: the unprintable left/top edge
    int printable_w = 0, printable_h = 0;  // HORZRES, VERTRES
};

// The sheet as Android's PrintAttributes see it: mils (thousandths of an inch) in the orientation
// chosen, with the unprintable edges as the minimum margins.
struct PaperMils {
    int width = 0, height = 0;
    int left = 0, top = 0, right = 0, bottom = 0;
};

inline int to_mils(int px, int dpi) {
    return dpi > 0 ? static_cast<int>(std::lround(static_cast<double>(px) * 1000.0 / dpi)) : 0;
}

inline PaperMils paper_mils(const PrinterGeometry& g) {
    PaperMils m;
    m.width = to_mils(g.paper_w, g.dpi_x);
    m.height = to_mils(g.paper_h, g.dpi_y);
    m.left = to_mils(g.offset_x, g.dpi_x);
    m.top = to_mils(g.offset_y, g.dpi_y);
    m.right = std::max(0, to_mils(g.paper_w - g.offset_x - g.printable_w, g.dpi_x));
    m.bottom = std::max(0, to_mils(g.paper_h - g.offset_y - g.printable_h, g.dpi_y));
    return m;
}

// Where one page goes, in the device context's coordinates (printable-area origin), and the size of
// the picture rendered for it.
struct PagePlacement {
    double x = 0, y = 0, w = 0, h = 0;  // device pixels; x, y may be negative (the unprintable edge)
    int render_w = 0, render_h = 0;     // the picture's pixels
    bool scaled = false;                // false: one PDF unit is one unit of paper
};

// The resolution a page is rendered at: the printer's, but no finer than this (a 1200 dpi printer gets
// a 600 dpi picture, stretched by its driver).
constexpr int kMaxRenderDpi = 600;

// A page of page_w x page_h DIPs (1/96 inch, Windows.Data.Pdf's PdfPage.Size, rotation applied) on the
// printer's sheet.
inline PagePlacement place_page(double page_w, double page_h, const PrinterGeometry& g) {
    PagePlacement p;
    if (page_w <= 0 || page_h <= 0 || g.dpi_x <= 0 || g.dpi_y <= 0 || g.paper_w <= 0 || g.paper_h <= 0) return p;
    const double natural_w = page_w * g.dpi_x / 96.0, natural_h = page_h * g.dpi_y / 96.0;
    // Within 2% (and a fraction of a millimetre of rounding in either the app's or the driver's sizes)
    // the page is this paper's.
    const bool fits = std::fabs(natural_w - g.paper_w) <= 0.02 * g.paper_w &&
                      std::fabs(natural_h - g.paper_h) <= 0.02 * g.paper_h;
    double scale = 1.0;
    if (!fits) {
        scale = std::min(g.paper_w / natural_w, g.paper_h / natural_h);
        p.scaled = true;
    }
    p.w = natural_w * scale;
    p.h = natural_h * scale;
    p.x = (g.paper_w - p.w) / 2.0 - g.offset_x;
    p.y = (g.paper_h - p.h) / 2.0 - g.offset_y;
    const double cut_x = std::min(1.0, static_cast<double>(kMaxRenderDpi) / g.dpi_x);
    const double cut_y = std::min(1.0, static_cast<double>(kMaxRenderDpi) / g.dpi_y);
    p.render_w = std::max(1, static_cast<int>(std::lround(p.w * cut_x)));
    p.render_h = std::max(1, static_cast<int>(std::lround(p.h * cut_y)));
    return p;
}

// One band of a page: the rows [row, row + rows) of the rendered picture, the same strip of the PDF
// page in DIPs, and where it goes on the device.
struct PageBand {
    int row = 0, rows = 0;
    double src_y = 0, src_h = 0;  // DIPs of the page
    int dst_y = 0, dst_h = 0;     // device pixels
};

// The largest picture one band may be, in bytes of 32-bit pixels.
constexpr int64_t kMaxBandBytes = 32ll << 20;

// The bands a page placed at `p` is rendered and drawn in, top to bottom. Each band's device rows
// start where the previous one's end, so no row is left unpainted or painted twice.
inline std::vector<PageBand> page_bands(const PagePlacement& p, double page_h, int64_t max_bytes = kMaxBandBytes) {
    std::vector<PageBand> bands;
    if (p.render_w <= 0 || p.render_h <= 0 || page_h <= 0) return bands;
    const int64_t row_bytes = static_cast<int64_t>(p.render_w) * 4;
    const int per = static_cast<int>(std::max<int64_t>(1, std::min<int64_t>(p.render_h, max_bytes / row_bytes)));
    auto dev_row = [&](int row) { return static_cast<int>(std::lround(p.y + p.h * row / p.render_h)); };
    for (int row = 0; row < p.render_h; row += per) {
        PageBand b;
        b.row = row;
        b.rows = std::min(per, p.render_h - row);
        b.src_y = page_h * row / p.render_h;
        b.src_h = page_h * b.rows / p.render_h;
        b.dst_y = dev_row(row);
        b.dst_h = dev_row(row + b.rows) - b.dst_y;
        bands.push_back(b);
    }
    return bands;
}

// The pages sent to the printer, in order: `pages` (indices into the PDF; empty is every page of a
// document of `page_count`), `copies` times. Copies the driver makes itself (DEVMODE's dmCopies) are
// not counted here: `copies` is what the print dialog left to the program. Collated, the whole
// document is sent once per copy (1 2 3 1 2 3); not collated, each page is sent `copies` times in a
// row (1 1 2 2 3 3). Pages past the document's end are left out.
inline std::vector<int> print_sequence(const std::vector<int>& pages, int page_count, int copies, bool collate) {
    std::vector<int> doc;
    if (pages.empty()) {
        for (int i = 0; i < page_count; ++i) doc.push_back(i);
    } else {
        for (int i : pages) {
            if (i >= 0 && i < page_count) doc.push_back(i);
        }
    }
    copies = std::max(1, copies);
    std::vector<int> seq;
    if (collate) {
        for (int c = 0; c < copies; ++c) seq.insert(seq.end(), doc.begin(), doc.end());
    } else {
        for (int i : doc) seq.insert(seq.end(), static_cast<size_t>(copies), i);
    }
    return seq;
}

}  // namespace winapk::host
