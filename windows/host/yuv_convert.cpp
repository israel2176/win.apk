// yuv_convert.cpp -- video_decode.h's YUV to ARGB conversions: NV12/P010 (what
// Media Foundation's decoders give) and I420 (what the software decoders give,
// soft_codecs.h). Plain C++, no Windows call, so that a test can run it
// anywhere.

#include <cstddef>
#include <cstdint>

#include "video_decode.h"

namespace winapk::video {

namespace {

// One 4:2:0 picture to ARGB. The chroma samples of a pixel pair are at
// u[col / 2 * step] and v[col / 2 * step] of its chroma row; with `sixteen_bit`,
// every sample is two bytes, little-endian, the value in the top bits.
void convert(const uint8_t* y, int y_stride, const uint8_t* u, const uint8_t* v, int uv_stride, int step, int width,
             int height, bool sixteen_bit, bool bt709, bool full_range, int rotation, uint32_t* out) {
    // 16.16 fixed point. Limited range scales Y by 255/219 and chroma by
    // 255/224 (ITU-R BT.601 / BT.709 conversion, as every player does).
    const int ys = full_range ? 65536 : 76309;
    int rv, gu, gv, bu;
    if (bt709) {
        rv = full_range ? 103206 : 117506;
        gu = full_range ? 12277 : 13959;
        gv = full_range ? 30679 : 34925;
        bu = full_range ? 121609 : 138438;
    } else {
        rv = full_range ? 91881 : 104597;
        gu = full_range ? 22554 : 25675;
        gv = full_range ? 46802 : 53279;
        bu = full_range ? 116130 : 132201;
    }
    const int ybias = full_range ? 0 : 16;
    const int out_w = (rotation == 90 || rotation == 270) ? height : width;
    auto clamp = [](int c) { return static_cast<uint32_t>(c < 0 ? 0 : c > 255 ? 255 : c); };
    const int bytes = sixteen_bit ? 2 : 1;
    for (int row = 0; row < height; ++row) {
        const uint8_t* yr = y + static_cast<size_t>(row) * y_stride;
        const uint8_t* ur = u + static_cast<size_t>(row / 2) * uv_stride;
        const uint8_t* vr = v + static_cast<size_t>(row / 2) * uv_stride;
        // Where this row's pixels land, and how far apart: the four rotations
        // are one base and one step each.
        uint32_t* p;
        ptrdiff_t dst_step;
        switch (rotation) {
            case 90: p = out + (height - 1 - row); dst_step = out_w; break;
            case 180: p = out + static_cast<size_t>(height - 1 - row) * out_w + (width - 1); dst_step = -1; break;
            case 270: p = out + static_cast<size_t>(width - 1) * out_w + row; dst_step = -out_w; break;
            default: p = out + static_cast<size_t>(row) * out_w; dst_step = 1; break;
        }
        for (int col = 0; col < width; ++col, p += dst_step) {
            const size_t c = static_cast<size_t>(col / 2) * step * bytes;
            int yv, cb, cr;
            if (sixteen_bit) {
                yv = yr[col * 2 + 1];  // the top 8 of the 10 bits: the byte above the low one
                cb = ur[c + 1];
                cr = vr[c + 1];
            } else {
                yv = yr[col];
                cb = ur[c];
                cr = vr[c];
            }
            const int yy = (yv - ybias) * ys;
            const int du = cb - 128, dv = cr - 128;
            const uint32_t r = clamp((yy + rv * dv + 32768) >> 16);
            const uint32_t g = clamp((yy - gu * du - gv * dv + 32768) >> 16);
            const uint32_t b = clamp((yy + bu * du + 32768) >> 16);
            *p = 0xff000000u | (r << 16) | (g << 8) | b;
        }
    }
}

}  // namespace

void yuv420_to_argb(const uint8_t* y, int y_stride, const uint8_t* uv, int uv_stride, int width, int height,
                    bool sixteen_bit, bool bt709, bool full_range, int rotation, uint32_t* out) {
    // Interleaved Cb,Cr: a pair's Cr is one sample after its Cb, two samples on.
    convert(y, y_stride, uv, uv + (sixteen_bit ? 2 : 1), uv_stride, 2, width, height, sixteen_bit, bt709, full_range,
            rotation, out);
}

void i420_to_argb(const uint8_t* y, int y_stride, const uint8_t* u, const uint8_t* v, int uv_stride, int width,
                  int height, bool bt709, bool full_range, uint32_t* out) {
    convert(y, y_stride, u, v, uv_stride, 1, width, height, false, bt709, full_range, 0, out);
}

}  // namespace winapk::video
