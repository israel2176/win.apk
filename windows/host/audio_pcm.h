// audio_pcm.h -- PCM as Android apps hand it over, made into the mixer's
// stereo float, and back: sample formats, the channel downmix, and a
// band-limited resampler. No Windows call, no state shared between users, so
// test/opensles_test.cpp checks it directly. OpenSL ES's players and
// recorders (native_opensles.cpp) use it.
//
// Formats are those Android's OpenSL ES accepts (frameworks/wilhelm
// data.cpp, AudioPlayer_to_android.cpp): 8-bit unsigned, 16/24/32-bit signed
// little-endian integers with the container as wide as the sample, and
// 32-bit float (SLAndroidDataFormat_PCM_EX); 1 to 8 channels, interleaved in
// the order of their channel-mask bits.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace winapk::audio::pcm {

enum class Sample { kU8, kS16, kS24, kS32, kF32 };

inline size_t sample_bytes(Sample s) {
    switch (s) {
    case Sample::kU8: return 1;
    case Sample::kS16: return 2;
    case Sample::kS24: return 3;
    case Sample::kS32:
    case Sample::kF32: return 4;
    }
    return 0;
}

// One sample at `p` as float in [-1, 1].
inline float read_sample(Sample s, const uint8_t* p) {
    switch (s) {
    case Sample::kU8: return (static_cast<int>(p[0]) - 128) / 128.0f;
    case Sample::kS16: return static_cast<int16_t>(p[0] | (p[1] << 8)) / 32768.0f;
    case Sample::kS24: {
        // Into the top three bytes, then an arithmetic shift down: the sign follows.
        const uint32_t u = (static_cast<uint32_t>(p[0]) << 8) | (static_cast<uint32_t>(p[1]) << 16) |
                           (static_cast<uint32_t>(p[2]) << 24);
        const int32_t v = static_cast<int32_t>(u) >> 8;
        return v / 8388608.0f;
    }
    case Sample::kS32: {
        int32_t v;
        std::memcpy(&v, p, 4);
        return static_cast<float>(v / 2147483648.0);
    }
    case Sample::kF32: {
        float v;
        std::memcpy(&v, p, 4);
        return std::isfinite(v) ? v : 0.0f;
    }
    }
    return 0;
}

// One float sample written at `p`, rounded and clipped to the format.
inline void write_sample(Sample s, float v, uint8_t* p) {
    const double c = std::clamp(static_cast<double>(v), -1.0, 1.0);
    switch (s) {
    case Sample::kU8: p[0] = static_cast<uint8_t>(std::clamp(std::lround(c * 128.0) + 128, 0L, 255L)); return;
    case Sample::kS16: {
        const int16_t x = static_cast<int16_t>(std::clamp(std::lround(c * 32768.0), -32768L, 32767L));
        std::memcpy(p, &x, 2);
        return;
    }
    case Sample::kS24: {
        const int32_t x = static_cast<int32_t>(std::clamp<long long>(std::llround(c * 8388608.0), -8388608, 8388607));
        p[0] = static_cast<uint8_t>(x);
        p[1] = static_cast<uint8_t>(x >> 8);
        p[2] = static_cast<uint8_t>(x >> 16);
        return;
    }
    case Sample::kS32: {
        const int32_t x =
            static_cast<int32_t>(std::clamp<long long>(std::llround(c * 2147483648.0), INT32_MIN, INT32_MAX));
        std::memcpy(p, &x, 4);
        return;
    }
    case Sample::kF32: std::memcpy(p, &v, 4); return;
    }
}

// OpenSL ES's speaker bits (SL_SPEAKER_*); SL_ANDROID_SPEAKER_NON_POSITIONAL
// marks an index mask (channel n is bit n, no position).
constexpr uint32_t kFrontLeft = 0x1, kFrontRight = 0x2, kFrontCenter = 0x4, kLowFrequency = 0x8, kBackLeft = 0x10,
                   kBackRight = 0x20, kFrontLeftOfCenter = 0x40, kFrontRightOfCenter = 0x80, kBackCenter = 0x100,
                   kSideLeft = 0x200, kSideRight = 0x400, kTopCenter = 0x800, kTopFrontLeft = 0x1000,
                   kTopFrontCenter = 0x2000, kTopFrontRight = 0x4000, kTopBackLeft = 0x8000,
                   kTopBackCenter = 0x10000, kTopBackRight = 0x20000;
constexpr uint32_t kNonPositional = 0x80000000u;

inline int popcount(uint32_t m) {
    int n = 0;
    for (; m; m &= m - 1) ++n;
    return n;
}

// The mask Android gives an output of `channels` channels when the app gives
// none (sles_channel_out_mask_from_count): 0 when there is no default.
inline uint32_t default_out_mask(int channels) {
    switch (channels) {
    case 1: return kFrontLeft;
    case 2: return kFrontLeft | kFrontRight;
    case 3: return kFrontLeft | kFrontRight | kFrontCenter;
    case 4: return kFrontLeft | kFrontRight | kBackLeft | kBackRight;
    case 5: return kFrontLeft | kFrontRight | kFrontCenter | kBackLeft | kBackRight;
    case 6: return kFrontLeft | kFrontRight | kFrontCenter | kLowFrequency | kBackLeft | kBackRight;
    case 7: return kFrontLeft | kFrontRight | kFrontCenter | kLowFrequency | kBackLeft | kBackRight | kBackCenter;
    case 8:
        return kFrontLeft | kFrontRight | kFrontCenter | kLowFrequency | kBackLeft | kBackRight | kSideLeft |
               kSideRight;
    default: return 0;
    }
}

// Whether Android's player takes this mask for `channels` channels
// (android_audioPlayer_validateChannelMask): an index mask of that many bits,
// or a positional one its downmixer handles: front left and right present
// beyond mono, sides and backs in pairs, nothing above or between.
inline bool player_mask_ok(uint32_t mask, int channels) {
    if (channels < 1 || channels > 8) return false;
    if (mask & kNonPositional) return popcount(mask & ~kNonPositional) == channels;
    if (popcount(mask) != channels) return false;
    const uint32_t unsupported = kFrontLeftOfCenter | kFrontRightOfCenter | kTopCenter | kTopFrontLeft |
                                 kTopFrontCenter | kTopFrontRight | kTopBackLeft | kTopBackCenter | kTopBackRight;
    if (mask & unsupported) return false;
    if (channels > 1 && (mask & (kFrontLeft | kFrontRight)) != (kFrontLeft | kFrontRight)) return false;
    const uint32_t sides = kSideLeft | kSideRight, backs = kBackLeft | kBackRight;
    if ((mask & sides) != 0 && (mask & sides) != sides) return false;
    if ((mask & backs) != 0 && (mask & backs) != backs) return false;
    return true;
}

// How each input channel reaches the left and right outputs.
struct Downmix {
    int channels = 2;
    float left[8] = {}, right[8] = {};
};

// Mono is copied to both sides at full level (an AudioTrack's mono); stereo
// stays as it is; more channels fold down the way a stereo downmix does
// (ITU-R BS.775: centre and surrounds at -3 dB into their side, the LFE left
// out). An index mask has no positions: its first two channels are left and
// right.
inline Downmix downmix_for(int channels, uint32_t mask) {
    Downmix d;
    d.channels = channels;
    if (channels == 1) {
        d.left[0] = d.right[0] = 1.0f;
        return d;
    }
    if ((mask & kNonPositional) || mask == 0) {
        d.left[0] = 1.0f;
        d.right[1] = 1.0f;
        return d;
    }
    constexpr float k = 0.70710678f;
    int c = 0;
    for (uint32_t bit = 1; bit && c < channels; bit <<= 1) {
        if (!(mask & bit)) continue;
        float l = 0, r = 0;
        switch (bit) {
        case kFrontLeft: l = 1; break;
        case kFrontRight: r = 1; break;
        case kFrontCenter: l = r = k; break;
        case kBackLeft:
        case kSideLeft: l = k; break;
        case kBackRight:
        case kSideRight: r = k; break;
        case kBackCenter: l = r = 0.5f; break;
        default: break;  // the LFE
        }
        d.left[c] = l;
        d.right[c] = r;
        ++c;
    }
    return d;
}

// `frames` interleaved frames of `sample` at `in`, as stereo float appended to `out`.
inline void to_stereo(Sample sample, const Downmix& d, const uint8_t* in, size_t frames, std::vector<float>* out) {
    const size_t sb = sample_bytes(sample), fb = sb * static_cast<size_t>(d.channels);
    const size_t at = out->size();
    out->resize(at + frames * 2);
    float* o = out->data() + at;
    for (size_t f = 0; f < frames; ++f, in += fb) {
        float l = 0, r = 0;
        for (int c = 0; c < d.channels; ++c) {
            const float v = read_sample(sample, in + c * sb);
            l += v * d.left[c];
            r += v * d.right[c];
        }
        o[f * 2] = l;
        o[f * 2 + 1] = r;
    }
}

// A band-limited resampler for interleaved stereo float: a Kaiser-windowed
// sinc, its taps for 256 phases between two input samples, interpolated
// between neighbouring phases. The cutoff follows the lower of the two rates
// (with a little room below Nyquist), so going down does not alias; at a
// ratio of exactly 1 it copies, adding no delay.
//
// ratio = input frames per output frame (in_rate * speed / out_rate). The
// input is timed so that output frame k is input frame pos(k): positions are
// kernel centres, and the history before the first input is silence.
class Resampler {
public:
    static constexpr int kPhases = 256;
    static constexpr int kZeroCrossings = 16;  // per side at full bandwidth

    Resampler() { reset(); }

    // A new ratio keeps the input and position: a playback rate change glides.
    void set_ratio(double ratio) {
        if (!(ratio > 0)) ratio = 1;
        if (ratio == ratio_) return;
        ratio_ = ratio;
        if (ratio_ == 1.0) pos_ = std::floor(pos_ + 0.5);  // copying wants whole positions
        const double cutoff = std::min(1.0, 1.0 / ratio_) * 0.94;
        const int half = static_cast<int>(std::ceil(kZeroCrossings / cutoff));
        if (cutoff != cutoff_ || half != half_) build(cutoff, half);
    }
    double ratio() const { return ratio_; }
    // Taps on each side of the centre: the input the resampler needs past a position.
    int half_width() const { return ratio_ == 1.0 ? 0 : half_; }

    // Forgets all input; the next input frame is position 0.
    void reset() {
        in_.assign(static_cast<size_t>(kMaxHalf) * 2, 0.0f);  // silent history
        base_ = -kMaxHalf;
        pos_ = 0;
    }

    void push(const float* stereo, size_t frames) { in_.insert(in_.end(), stereo, stereo + frames * 2); }
    void push_silence(size_t frames) { in_.resize(in_.size() + frames * 2, 0.0f); }

    // Input frames past the current position not yet resampled.
    double pending() const { return static_cast<double>(end()) - pos_; }
    // The position of the next output frame, in input frames since reset().
    double position() const { return pos_; }

    // Writes up to `max_out` stereo frames to `out`; returns how many the
    // input so far allows.
    size_t pull(float* out, size_t max_out) {
        size_t n = 0;
        if (ratio_ == 1.0) {
            for (; n < max_out; ++n) {
                const int64_t i = static_cast<int64_t>(pos_);
                if (i >= end()) break;
                const size_t k = static_cast<size_t>(i - base_) * 2;
                out[n * 2] = in_[k];
                out[n * 2 + 1] = in_[k + 1];
                pos_ += 1;
            }
        } else {
            for (; n < max_out; ++n) {
                const double fl = std::floor(pos_);
                const int64_t i = static_cast<int64_t>(fl);
                if (i + half_ >= end()) break;
                const double phase = (pos_ - fl) * kPhases;
                const int p = std::min(static_cast<int>(phase), kPhases - 1);
                const float a = static_cast<float>(phase - p);
                const float* k0 = &taps_[static_cast<size_t>(p) * width()];
                const float* k1 = k0 + width();
                const float* x = &in_[static_cast<size_t>(i - half_ + 1 - base_) * 2];
                float l = 0, r = 0;
                for (int t = 0; t < width(); ++t) {
                    const float w = k0[t] + (k1[t] - k0[t]) * a;
                    l += x[t * 2] * w;
                    r += x[t * 2 + 1] * w;
                }
                out[n * 2] = l;
                out[n * 2 + 1] = r;
                pos_ += ratio_;
            }
        }
        trim();
        return n;
    }

private:
    static constexpr int kMaxHalf = 128;  // the widest kernel kept (down by up to 7.5x)
    static constexpr double kPi = 3.14159265358979323846;

    int64_t end() const { return base_ + static_cast<int64_t>(in_.size() / 2); }
    int width() const { return half_ * 2; }

    static double bessel_i0(double x) {
        double sum = 1, term = 1;
        for (int k = 1; k < 32; ++k) {
            term *= (x / (2 * k)) * (x / (2 * k));
            sum += term;
        }
        return sum;
    }

    // taps_[p][t]: the weight of input frame floor(pos) - half + 1 + t for a
    // position p / kPhases past floor(pos); kPhases + 1 rows, so phase p+1
    // always exists. Each row sums to 1 (no ripple at DC).
    void build(double cutoff, int half) {
        cutoff_ = cutoff;
        half_ = std::min(half, kMaxHalf);
        const double beta = 8.0, norm = bessel_i0(beta);
        taps_.assign(static_cast<size_t>(kPhases + 1) * width(), 0.0f);
        for (int p = 0; p <= kPhases; ++p) {
            double sum = 0;
            std::vector<double> row(static_cast<size_t>(width()));
            for (int t = 0; t < width(); ++t) {
                const double x = (t - half_ + 1) - static_cast<double>(p) / kPhases;  // distance from the centre
                const double u = x / half_;
                double w = 0;
                if (u > -1 && u < 1) {
                    const double s = x == 0 ? 1.0 : std::sin(kPi * cutoff * x) / (kPi * cutoff * x);
                    w = cutoff * s * bessel_i0(beta * std::sqrt(1 - u * u)) / norm;
                }
                row[static_cast<size_t>(t)] = w;
                sum += w;
            }
            for (int t = 0; t < width(); ++t) {
                taps_[static_cast<size_t>(p) * width() + t] = static_cast<float>(row[static_cast<size_t>(t)] / sum);
            }
        }
    }

    // Drops input no future position can reach.
    void trim() {
        const int64_t keep_from = static_cast<int64_t>(std::floor(pos_)) - kMaxHalf;
        if (keep_from - base_ > 4096) {
            in_.erase(in_.begin(), in_.begin() + static_cast<ptrdiff_t>((keep_from - base_) * 2));
            base_ = keep_from;
        }
    }

    std::vector<float> in_;  // stereo frames from input frame base_ on
    int64_t base_ = 0;
    double pos_ = 0;
    double ratio_ = 1;
    double cutoff_ = 0;
    int half_ = 1;
    std::vector<float> taps_;
};

}  // namespace winapk::audio::pcm
