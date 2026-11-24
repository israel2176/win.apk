// yuv_convert.cpp -- pictures from Y'CbCr to what their consumers take (video_decode.h): ARGB for
// a Surface and Bitmap.setPixels, I420 or P010 for MediaCodec's ByteBuffer and Image output.
// Plain C++, no Windows call, so that test/soft_codecs_test.cpp runs it anywhere.
//
// The colour arithmetic is ITU-R's: the standard's Kr and Kb (BT.601, BT.709, BT.2020's
// non-constant luminance) give the matrix, the range gives the offsets and scales (limited:
// Y' 16..235 and C 16..240 at 8 bits, times 2^(n-8) at n bits; full: 0..2^n-1). Every sample is
// first widened to 16 bits (v << (16 - n)), so one integer path serves 8, 10 and 12-bit
// pictures; the result keeps 20 fractional bits until the last step, where an 8-bit picture is
// rounded and a deeper one gets a 4x4 ordered (Bayer) dither instead, so that 10-bit gradients
// do not band when shown on 8 bits. Chroma is taken from the sample covering the pixel (no
// filter), as for every 4:2:0 path here.

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

#include "video_decode.h"

namespace winapk::video {

namespace {

constexpr int kFrac = 20;  // fractional bits of the fixed-point results

// The 4x4 Bayer matrix: thresholds 0..15 in an order that spreads them evenly.
constexpr int kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// What to add before the final shift of a value with kFrac fractional bits: half a step for a
// picture of 8 bits, the dither threshold (2t + 1) / 32 of a step for a deeper one.
inline int rounding(bool dither, int x, int y) {
    return dither ? (2 * kBayer[y & 3][x & 3] + 1) << (kFrac - 5) : 1 << (kFrac - 1);
}

inline uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

// Y'CbCr (16-bit samples) to 8-bit R'G'B': out = ymul * (Y - yoff) + c * (C - 32768), in kFrac
// fixed point.
struct Matrix {
    int ymul = 0, yoff = 0;
    int rv = 0, gu = 0, gv = 0, bu = 0;

    Matrix(int standard, bool full, int bit_depth) {
        double kr = 0.299, kb = 0.114;  // BT.601, also what an unsaid standard is taken for
        if (standard == ColorAspects::kBt709) kr = 0.2126, kb = 0.0722;
        if (standard == ColorAspects::kBt2020) kr = 0.2627, kb = 0.0593;
        const double kg = 1.0 - kr - kb;
        double yscale, cscale;  // 16-bit units per unit of Y' (0..1) and of C (-0.5..0.5)
        if (full) {
            const double top = static_cast<double>((1 << bit_depth) - 1) * (1 << (16 - bit_depth));
            yscale = cscale = top;
            yoff = 0;
        } else {
            yscale = 219.0 * 256;
            cscale = 224.0 * 256;
            yoff = 16 * 256;
        }
        const double one = static_cast<double>(1 << kFrac) * 255.0;
        ymul = static_cast<int>(one / yscale + 0.5);
        rv = static_cast<int>(one * 2 * (1 - kr) / cscale + 0.5);
        bu = static_cast<int>(one * 2 * (1 - kb) / cscale + 0.5);
        gu = static_cast<int>(one * 2 * kb * (1 - kb) / kg / cscale + 0.5);
        gv = static_cast<int>(one * 2 * kr * (1 - kr) / kg / cscale + 0.5);
    }
};

// One row of a planar picture to ARGB. The samples are uint8_t or uint16_t; `widen` brings one
// to 16 bits.
template <typename TY, typename TC>
void row_to_argb(const Matrix& m, const TY* yrow, const TC* urow, const TC* vrow, int shift_x, int widen, int width,
                 int y, bool dither, uint32_t* out) {
    for (int x = 0; x < width; ++x) {
        const int luma = m.ymul * ((static_cast<int>(yrow[x]) << widen) - m.yoff);
        int r = luma, g = luma, b = luma;
        if (urow) {
            const int cx = x >> shift_x;
            const int u = (static_cast<int>(urow[cx]) << widen) - 32768;
            const int v = (static_cast<int>(vrow[cx]) << widen) - 32768;
            // At most 2^15 * 6600 per term: no int overflow at any depth or range.
            r += m.rv * v;
            g -= m.gu * u + m.gv * v;
            b += m.bu * u;
        }
        const int add = rounding(dither, x, y);
        out[x] = 0xff000000u | static_cast<uint32_t>(clamp8((r + add) >> kFrac)) << 16 |
                 static_cast<uint32_t>(clamp8((g + add) >> kFrac)) << 8 | clamp8((b + add) >> kFrac);
    }
}

template <typename T>
const T* row(const uint8_t* plane, int stride, int y) {
    return reinterpret_cast<const T*>(plane + static_cast<ptrdiff_t>(stride) * y);
}

template <typename T>
void picture_to_argb(const Picture& p, uint32_t* out) {
    const bool full = p.color.range == ColorAspects::kFull;
    const Matrix m(p.color.standard, full, p.bit_depth);
    const int widen = 16 - (sizeof(T) == 1 ? 8 : p.bit_depth);
    const bool dither = p.bit_depth > 8;
    for (int y = 0; y < p.height; ++y) {
        const int cy = y >> p.chroma_shift_y;
        const T* u = p.monochrome ? nullptr : row<T>(p.plane[1], p.stride[1], cy);
        const T* v = p.monochrome ? nullptr : row<T>(p.plane[2], p.stride[2], cy);
        row_to_argb(m, row<T>(p.plane[0], p.stride[0], y), u, v, p.chroma_shift_x, widen, p.width, y, dither,
                    out + static_cast<size_t>(y) * p.width);
    }
}

// One plane of samples to 8 bits: copied, or dithered down from more.
template <typename T>
void plane_to_8(const uint8_t* src, int src_stride, int bit_depth, int width, int height, uint8_t* dst,
                int dst_stride) {
    for (int y = 0; y < height; ++y) {
        const T* s = row<T>(src, src_stride, y);
        uint8_t* d = dst + static_cast<ptrdiff_t>(dst_stride) * y;
        if (sizeof(T) == 1) {
            std::memcpy(d, s, static_cast<size_t>(width));
            continue;
        }
        const int shift = bit_depth - 8;
        for (int x = 0; x < width; ++x) {
            const int t = kBayer[y & 3][x & 3] >> (4 - std::min(shift, 4));
            d[x] = clamp8((static_cast<int>(s[x]) + t) >> shift);
        }
    }
}

// A chroma plane of any subsampling to 4:2:0 at depth `bit_depth`, averaged where it has more
// samples than 4:2:0 (4:2:2, 4:4:4), into a 16-bit buffer (cw x ch). Grey for no plane.
template <typename T>
void chroma_to_420(const Picture& p, int plane, int cw, int ch, std::vector<uint16_t>* out) {
    out->assign(static_cast<size_t>(cw) * ch, static_cast<uint16_t>(1 << (p.bit_depth - 1)));
    if (p.monochrome) return;
    const int fx = 1 << (1 - p.chroma_shift_x), fy = 1 << (1 - p.chroma_shift_y);  // source samples per 4:2:0 one
    const int src_w = (p.width + (1 << p.chroma_shift_x) - 1) >> p.chroma_shift_x;
    const int src_h = (p.height + (1 << p.chroma_shift_y) - 1) >> p.chroma_shift_y;
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            int sum = 0, n = 0;
            for (int dy = 0; dy < fy; ++dy) {
                const int sy = std::min(y * fy + dy, src_h - 1);
                const T* s = row<T>(p.plane[plane], p.stride[plane], sy);
                for (int dx = 0; dx < fx; ++dx) {
                    sum += s[std::min(x * fx + dx, src_w - 1)];
                    ++n;
                }
            }
            (*out)[static_cast<size_t>(y) * cw + x] = static_cast<uint16_t>((sum + n / 2) / n);
        }
    }
}

template <typename T>
void picture_to_yuv(const Picture& p, const YuvLayout& l, uint8_t* out) {
    const int cw = (p.width + 1) / 2, ch = (p.height + 1) / 2;
    uint8_t* yplane = out;
    uint8_t* cplane = out + static_cast<size_t>(l.stride) * l.slice_height;
    const bool direct420 = p.chroma_shift_x == 1 && p.chroma_shift_y == 1 && !p.monochrome;
    std::vector<uint16_t> c[2];
    if (!direct420) {
        chroma_to_420<T>(p, 1, cw, ch, &c[0]);
        chroma_to_420<T>(p, 2, cw, ch, &c[1]);
    }
    if (l.format == YuvLayout::kI420) {
        plane_to_8<T>(p.plane[0], p.stride[0], p.bit_depth, p.width, p.height, yplane, l.stride);
        const int cstride = l.stride / 2;
        for (int k = 0; k < 2; ++k) {
            uint8_t* dst = cplane + static_cast<size_t>(cstride) * (l.slice_height / 2) * k;
            if (direct420) {
                plane_to_8<T>(p.plane[1 + k], p.stride[1 + k], p.bit_depth, cw, ch, dst, cstride);
            } else {
                // 16-bit samples of the picture's depth (8 bits: shifted by nothing).
                plane_to_8<uint16_t>(reinterpret_cast<const uint8_t*>(c[k].data()), cw * 2, p.bit_depth, cw, ch, dst,
                                     cstride);
            }
        }
        return;
    }
    // P010: the value in the top bits of each 16-bit sample.
    const int up = 16 - (sizeof(T) == 1 ? 8 : p.bit_depth);
    for (int y = 0; y < p.height; ++y) {
        const T* s = row<T>(p.plane[0], p.stride[0], y);
        uint16_t* d = reinterpret_cast<uint16_t*>(yplane + static_cast<size_t>(l.stride) * y);
        for (int x = 0; x < p.width; ++x) d[x] = static_cast<uint16_t>(s[x] << up);
    }
    for (int y = 0; y < ch; ++y) {
        uint16_t* d = reinterpret_cast<uint16_t*>(cplane + static_cast<size_t>(l.stride) * y);
        const T* u = direct420 ? row<T>(p.plane[1], p.stride[1], y) : nullptr;
        const T* v = direct420 ? row<T>(p.plane[2], p.stride[2], y) : nullptr;
        for (int x = 0; x < cw; ++x) {
            const int cu = direct420 ? u[x] : c[0][static_cast<size_t>(y) * cw + x];
            const int cv = direct420 ? v[x] : c[1][static_cast<size_t>(y) * cw + x];
            d[2 * x] = static_cast<uint16_t>(cu << up);
            d[2 * x + 1] = static_cast<uint16_t>(cv << up);
        }
    }
}

// BT.601 limited range from 8-bit R'G'B', in kFrac fixed point: BT.601's forward matrix with the
// 219 and 224 scales folded in ((65.481, 128.553, 24.966) / 255 and so on, times 2^20).
inline int y601(int r, int g, int b) { return (16 << kFrac) + r * 269262 + g * 528618 + b * 102662; }
inline int u601(int r, int g, int b) { return (128 << kFrac) - r * 155424 - g * 305127 + b * 460551; }
inline int v601(int r, int g, int b) { return (128 << kFrac) + r * 460551 - g * 385654 - b * 74897; }

void argb_to_yuv(const Frame& f, const YuvLayout& l, uint8_t* out) {
    const int cw = (f.width + 1) / 2, ch = (f.height + 1) / 2;
    uint8_t* yplane = out;
    uint8_t* cplane = out + static_cast<size_t>(l.stride) * l.slice_height;
    const bool p010 = l.format == YuvLayout::kP010;
    auto put = [&](uint8_t* plane, int stride, int x, int y, int value) {
        const uint8_t v = clamp8((value + (1 << (kFrac - 1))) >> kFrac);
        if (p010)
            reinterpret_cast<uint16_t*>(plane + static_cast<size_t>(stride) * y)[x] = static_cast<uint16_t>(v << 8);
        else
            plane[static_cast<size_t>(stride) * y + x] = v;
    };
    for (int y = 0; y < f.height; ++y) {
        const uint32_t* s = f.argb.data() + static_cast<size_t>(y) * f.width;
        for (int x = 0; x < f.width; ++x) {
            put(yplane, l.stride, x, y, y601((s[x] >> 16) & 0xff, (s[x] >> 8) & 0xff, s[x] & 0xff));
        }
    }
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            int r = 0, g = 0, b = 0, n = 0;
            for (int dy = 0; dy < 2 && 2 * y + dy < f.height; ++dy) {
                for (int dx = 0; dx < 2 && 2 * x + dx < f.width; ++dx) {
                    const uint32_t px = f.argb[static_cast<size_t>(2 * y + dy) * f.width + 2 * x + dx];
                    r += (px >> 16) & 0xff;
                    g += (px >> 8) & 0xff;
                    b += px & 0xff;
                    ++n;
                }
            }
            r = (r + n / 2) / n, g = (g + n / 2) / n, b = (b + n / 2) / n;
            if (p010) {
                uint16_t* d = reinterpret_cast<uint16_t*>(cplane + static_cast<size_t>(l.stride) * y);
                d[2 * x] = static_cast<uint16_t>(clamp8((u601(r, g, b) + (1 << (kFrac - 1))) >> kFrac) << 8);
                d[2 * x + 1] = static_cast<uint16_t>(clamp8((v601(r, g, b) + (1 << (kFrac - 1))) >> kFrac) << 8);
            } else {
                const int cstride = l.stride / 2;
                put(cplane, cstride, x, y, u601(r, g, b));
                put(cplane + static_cast<size_t>(cstride) * (l.slice_height / 2), cstride, x, y, v601(r, g, b));
            }
        }
    }
}

bool usable(const Picture& p) {
    if (p.width <= 0 || p.height <= 0 || !p.plane[0] || p.bit_depth < 8 || p.bit_depth > 16) return false;
    if (p.chroma_shift_x < 0 || p.chroma_shift_x > 1 || p.chroma_shift_y < 0 || p.chroma_shift_y > 1) return false;
    return p.monochrome || (p.plane[1] && p.plane[2]);
}

}  // namespace

void yuv420_to_argb(const uint8_t* y, int y_stride, const uint8_t* uv, int uv_stride, int width, int height,
                    bool sixteen_bit, bool bt709, bool full_range, int rotation, uint32_t* out) {
    // NV12 and P010 have their chroma interleaved: rows of Cb,Cr pairs, read with a step of 2.
    const Matrix m(bt709 ? ColorAspects::kBt709 : ColorAspects::kBt601Ntsc, full_range, sixteen_bit ? 16 : 8);
    std::vector<uint32_t> line(static_cast<size_t>(width));
    std::vector<uint16_t> u(static_cast<size_t>((width + 1) / 2)), v(u.size());
    for (int r = 0; r < height; ++r) {
        const uint8_t* yrow = y + static_cast<ptrdiff_t>(y_stride) * r;
        const uint8_t* crow = uv + static_cast<ptrdiff_t>(uv_stride) * (r / 2);
        uint32_t* dst = rotation == 0 ? out + static_cast<size_t>(r) * width : line.data();
        if (sixteen_bit) {
            const uint16_t* c16 = reinterpret_cast<const uint16_t*>(crow);
            for (size_t i = 0; i < u.size(); ++i) u[i] = c16[2 * i], v[i] = c16[2 * i + 1];
            row_to_argb(m, reinterpret_cast<const uint16_t*>(yrow), u.data(), v.data(), 1, 0, width, r, true, dst);
        } else {
            for (size_t i = 0; i < u.size(); ++i) u[i] = crow[2 * i], v[i] = crow[2 * i + 1];
            row_to_argb(m, yrow, u.data(), v.data(), 1, 8, width, r, false, dst);
        }
        if (rotation == 0) continue;
        for (int c = 0; c < width; ++c) {
            // Where pixel (c, r) lands, turned clockwise.
            size_t at;
            if (rotation == 90)
                at = static_cast<size_t>(c) * height + (height - 1 - r);
            else if (rotation == 180)
                at = static_cast<size_t>(height - 1 - r) * width + (width - 1 - c);
            else
                at = static_cast<size_t>(width - 1 - c) * height + r;
            out[at] = line[static_cast<size_t>(c)];
        }
    }
}

bool frame_to_argb(const Frame& f, uint32_t* out) {
    if (!f.argb.empty()) {
        std::memcpy(out, f.argb.data(), f.argb.size() * sizeof(uint32_t));
        return true;
    }
    if (!f.picture || !usable(*f.picture)) return false;
    if (f.picture->bit_depth > 8)
        picture_to_argb<uint16_t>(*f.picture, out);
    else
        picture_to_argb<uint8_t>(*f.picture, out);
    return true;
}

std::vector<uint32_t> frame_argb(const Frame& f) {
    std::vector<uint32_t> out(static_cast<size_t>(std::max(0, f.width)) * std::max(0, f.height));
    if (out.empty() || !frame_to_argb(f, out.data())) out.clear();
    return out;
}

size_t YuvLayout::size() const {
    // I420: two chroma planes of (stride / 2) x (slice_height / 2); P010: one of stride x
    // (slice_height / 2). Either way half the luma plane's bytes.
    const size_t luma = static_cast<size_t>(stride) * slice_height;
    return luma + luma / 2;
}

YuvLayout YuvLayout::of(const Frame& f, Format format) {
    YuvLayout l;
    l.format = format;
    l.width = f.width;
    l.height = f.height;
    // Rows 16-aligned as Android's software decoders allocate theirs, an even number of them so
    // that the chroma plane starts at stride * slice_height and holds slice_height / 2 rows.
    const int aligned = (f.width + 15) & ~15;
    l.stride = format == kI420 ? aligned : aligned * 2;
    l.slice_height = (f.height + 1) & ~1;
    return l;
}

bool frame_to_yuv(const Frame& f, const YuvLayout& l, uint8_t* out) {
    if (l.width != f.width || l.height != f.height || f.width <= 0 || f.height <= 0) return false;
    if (!f.argb.empty()) {
        if (f.argb.size() < static_cast<size_t>(f.width) * f.height) return false;
        argb_to_yuv(f, l, out);
        return true;
    }
    if (!f.picture || !usable(*f.picture) || f.picture->width != f.width || f.picture->height != f.height) return false;
    if (f.picture->bit_depth > 8)
        picture_to_yuv<uint16_t>(*f.picture, l, out);
    else
        picture_to_yuv<uint8_t>(*f.picture, l, out);
    return true;
}

ColorAspects frame_color(const Frame& f) {
    if (f.picture) return f.picture->color;
    ColorAspects c;
    c.standard = ColorAspects::kBt601Ntsc;
    c.range = ColorAspects::kLimited;
    c.transfer = ColorAspects::kSdrVideo;
    return c;
}

}  // namespace winapk::video
