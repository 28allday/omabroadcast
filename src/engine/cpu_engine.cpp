// The CPU twin. Everything the Vulkan engine does, in plain loops, so there
// is always a working path. A picture is drawn a band of rows at a time on
// threads kept for the engine's life; every row depends only on what it reads,
// so how the bands fall changes nothing in the result.

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <new>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "engine.h"
#include "matting.h"
#include "row_pool.h"

namespace engine {
namespace {

// Four floats at once, lane by lane: SSE2 on a plain x86-64 build. Each lane is
// the IEEE operation the plain sum would do, in the same order.
typedef float V4 __attribute__((vector_size(16)));
inline V4 load4(const float *p) { V4 v; std::memcpy(&v, p, sizeof v); return v; }
inline void store4(float *p, V4 v) { std::memcpy(p, &v, sizeof v); }
inline V4 splat4(float v) { return V4{v, v, v, v}; }
// Colour multiplied by alpha; alpha as it is.
inline V4 assoc4(V4 p) { return p * V4{p[3], p[3], p[3], 1.f}; }
inline V4 atLeastZero4(V4 v) { const V4 zero = splat4(0.f); return v > zero ? v : zero; }

// floor and round for what a plain x86-64 has no instruction for (the library
// call is most of a sampler's time otherwise). Picture coordinates and code
// values fit an int and sit well inside float's whole-number range. Against
// roundf, of every float up to 65536 only 0.49999997 differs: 1 for 0.
inline int floorInt(float v) {
    const int i = static_cast<int>(v);
    return static_cast<float>(i) > v ? i - 1 : i;
}
inline int roundCode(float offset, float scale, float value, int top) {
    const float v = offset + scale * value;
    if (!(v > 0.f)) return 0;
    const int code = static_cast<int>(v + 0.5f);
    return code > top ? top : code;
}
inline uint8_t round8(float v) {
    if (!(v > 0.f)) return 0;
    const int code = static_cast<int>(v + 0.5f);
    return code > 255 ? 255 : uint8_t(code);
}

// The curves as tables with straight lines between entries, each within float's
// own rounding of frame.h's function over every float from 0 to 1 (worst 2.4e-7,
// under a thousandth of a 10-bit code), and the 8-bit input codes exact. Encoding
// is steepest near black, so it is looked up by the float's exponent and first
// nine fraction bits: entries stand evenly among the powers of two.
struct CurveTables {
    static constexpr int kEncodeBits = 9, kEncodeExponents = 9;
    static constexpr int kDecodeSteps = 4096, kLanczosSteps = 1024, kFallSteps = 128, kFallRange = 16;
    float fromFull[256], fromLimited[256];
    float encode[(kEncodeExponents << kEncodeBits) + 1];
    float decode[kDecodeSteps + 1];
    float lanczos[3 * kLanczosSteps + 1];
    float fall[kFallRange * kFallSteps + 1];
    CurveTables() {
        for (int v = 0; v < 256; ++v) {
            fromFull[v] = srgbToLinear(v / 255.f);
            fromLimited[v] = srgbToLinear(std::clamp((v - 16.f) / 219.f, 0.f, 1.f));
        }
        for (int i = 0; i <= (kEncodeExponents << kEncodeBits); ++i) {
            const double c = std::ldexp(1.0 + double(i & ((1 << kEncodeBits) - 1)) / (1 << kEncodeBits), (i >> kEncodeBits) - kEncodeExponents);
            encode[i] = float(1.055 * std::pow(c, 1.0 / 2.4) - 0.055);
        }
        for (int i = 0; i <= kDecodeSteps; ++i) decode[i] = float(std::pow((double(i) / kDecodeSteps + 0.055) / 1.055, 2.4));
        for (int i = 0; i <= 3 * kLanczosSteps; ++i) lanczos[i] = lanczosWeight(float(i) / kLanczosSteps);
        for (int i = 0; i <= kFallRange * kFallSteps; ++i) fall[i] = float(std::exp(-double(i) / kFallSteps));
    }
};
const CurveTables g_curves;

inline float encodeSrgb(float c) {
    if (c <= 0.f) return 0.f;
    if (!(c < 1.f)) return c >= 1.f ? 1.f : c;
    if (c <= 0.0031308f) return c * 12.92f;
    uint32_t bits;
    std::memcpy(&bits, &c, sizeof bits);
    // 2^-9 is the lowest power of two the curve's upper part reaches.
    const int shift = 23 - CurveTables::kEncodeBits;
    const uint32_t at = (bits >> shift) - ((127u - CurveTables::kEncodeExponents) << CurveTables::kEncodeBits);
    const float t = float(bits & ((1u << shift) - 1u)) * (1.f / float(1u << shift));
    const float a = g_curves.encode[at];
    return a + (g_curves.encode[at + 1] - a) * t;
}
inline float decodeSrgb(float e) {
    if (e <= 0.04045f) return e / 12.92f;
    if (!(e < 1.f)) return srgbToLinear(e);
    const float at = e * float(CurveTables::kDecodeSteps);
    const int i = static_cast<int>(at);
    const float a = g_curves.decode[i];
    return a + (g_curves.decode[i + 1] - a) * (at - float(i));
}
// exp(-x) for x >= 0, to under 1e-5: the likeness weights of noise reduction and
// the chroma key, where past the table's end a neighbour counts for nothing.
inline float fallTable(float x) {
    const float at = x * float(CurveTables::kFallSteps);
    if (!(at < float(CurveTables::kFallRange * CurveTables::kFallSteps))) return 0.f;
    const int i = static_cast<int>(at);
    const float a = g_curves.fall[i];
    return a + (g_curves.fall[i + 1] - a) * (at - float(i));
}
// lanczosWeight to within 8e-7.
inline float lanczosTable(float x) {
    x = x < 0.f ? -x : x;
    if (!(x < 3.f)) return 0.f;
    const float at = x * float(CurveTables::kLanczosSteps);
    const int i = static_cast<int>(at);
    const float a = g_curves.lanczos[i];
    return a + (g_curves.lanczos[i + 1] - a) * (at - float(i));
}

struct Sampled {
    float r, g, b, a;
};

Sampled straightSample(Sampled s) {
    if (s.a <= 0.f) return {0, 0, 0, 0};
    return {std::max(s.r / s.a, 0.f), std::max(s.g / s.a, 0.f), std::max(s.b / s.a, 0.f),
            std::clamp(s.a, 0.f, 1.f)};
}
inline V4 straight4(V4 s) {
    if (s[3] <= 0.f) return splat4(0.f);
    V4 o = atLeastZero4(s / splat4(s[3]));
    o[3] = std::clamp(s[3], 0.f, 1.f);
    return o;
}

// A picture as a loop reads it: copied into the loop, its numbers need not be
// fetched again after every pixel written (the build lets any store alias them).
struct Pic {
    const float *data;
    int width, height;
    explicit Pic(const FrameF &f) : data(f.rgba.data()), width(f.width), height(f.height) {}
    const float *px(int x, int y) const { return data + (static_cast<size_t>(y) * width + x) * 4; }
};

// Bilinear sample with clamp-to-edge, texel centres at +0.5, like a Vulkan
// sampler with VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE. Return associated colour:
// transparent texels have no influence on the colour of their neighbours.
inline V4 bilinear4(const Pic &src, float sx, float sy) {
    const float fx = sx - 0.5f;
    const float fy = sy - 0.5f;
    int x0 = floorInt(fx);
    int y0 = floorInt(fy);
    const float tx = fx - x0;
    const float ty = fy - y0;
    const int x1 = std::clamp(x0 + 1, 0, src.width - 1);
    const int y1 = std::clamp(y0 + 1, 0, src.height - 1);
    x0 = std::clamp(x0, 0, src.width - 1);
    y0 = std::clamp(y0, 0, src.height - 1);
    const V4 a = assoc4(load4(src.px(x0, y0))), b = assoc4(load4(src.px(x1, y0)));
    const V4 d = assoc4(load4(src.px(x0, y1))), e = assoc4(load4(src.px(x1, y1)));
    const V4 top = a + (b - a) * tx;
    const V4 bottom = d + (e - d) * tx;
    return top + (bottom - top) * ty;
}
inline Sampled sampled(V4 v) { return {v[0], v[1], v[2], v[3]}; }

// Bicubic (radius 2) or Lanczos (radius 3) at source pixel position (sx, sy),
// taps clamped to the crop; the same order of sums as kernel() in composite.comp.
inline V4 kernel4(const Pic &src, float sx, float sy, int radius, bool lanczos, int loX, int loY, int hiX, int hiY) {
    const float px = sx - 0.5f, py = sy - 0.5f;
    const int ix = floorInt(px), iy = floorInt(py);
    const float fx = px - ix, fy = py - iy;
    float weightX[6];
    int column[6];
    for (int kx = 1 - radius; kx <= radius; ++kx) {
        weightX[kx + radius - 1] = lanczos ? lanczosTable(fx - kx) : cubicWeight(fx - kx);
        column[kx + radius - 1] = std::clamp(ix + kx, loX, hiX);
    }
    V4 sum = splat4(0.f);
    float wsum = 0;
    for (int ky = 1 - radius; ky <= radius; ++ky) {
        const float wy = lanczos ? lanczosTable(fy - ky) : cubicWeight(fy - ky);
        const float *row = src.px(0, std::clamp(iy + ky, loY, hiY));
        for (int k = 0; k < 2 * radius; ++k) {
            const float w = wy * weightX[k];
            sum += assoc4(load4(row + size_t(column[k]) * 4)) * w;
            wsum += w;
        }
    }
    return sum / splat4(wsum);
}

// One axis of a layer standing upright: for each canvas pixel along it, the
// source pixels it reads and their weights, worked as composite.comp works them
// for that pixel. A pixel whose position lands on a source pixel's centre, to
// within what the sums themselves round away, reads that pixel alone.
struct AxisTaps {
    int n = 1;                  // taps a pixel
    std::vector<int> at;        // n per pixel
    std::vector<float> weight;  // n per pixel, adding to 1; empty when n is 1
    std::vector<float> edge;    // how far inside the box
    int lo = 0, hi = 0;         // the source pixels read
    bool straight = false;      // one tap each, source pixels in order
};
struct AxisLayer {
    int first, count;       // canvas pixels
    float centre, half, length, u0, u1;
    int size;               // source pixels along this axis
    int tapLo, tapHi;
    float lo, hi;           // sampling limits, 0..1
    ScaleFilter filter;
    float footprint, step;
};
void buildAxis(AxisTaps &a, const AxisLayer &q) {
    a.at.resize(size_t(q.count));
    a.edge.resize(size_t(q.count));
    const bool kernel = q.filter == ScaleFilter::Bicubic || q.filter == ScaleFilter::Lanczos;
    const bool lanczos = q.filter == ScaleFilter::Lanczos;
    const int radius = lanczos ? 3 : 2;
    const int area = q.filter == ScaleFilter::Area ? areaTaps(q.footprint) : 1;
    const float near = std::max(2.5e-4f, float(q.size) * 2.4e-7f);
    auto position = [&](int i) {
        const float d = float(q.first + i) + 0.5f - q.centre;
        const float along = d + q.half;
        a.edge[size_t(i)] = std::min(along, q.length - along);
        return q.u0 + (q.u1 - q.u0) * (along / q.length);
    };
    bool single = area == 1;
    for (int i = 0; i < q.count; ++i) {
        const float r = position(i);
        if (q.filter == ScaleFilter::Point) {
            a.at[size_t(i)] = std::clamp(floorInt(r * q.size), q.tapLo, q.tapHi);
            continue;
        }
        const float p = (kernel ? r : std::clamp(r, q.lo, q.hi)) * q.size - 0.5f;
        const int whole = floorInt(p);
        const float t = p - float(whole);
        if (t > near && t < 1.f - near) { single = false; break; }
        const int tap = whole + (t > 0.5f ? 1 : 0);
        a.at[size_t(i)] = kernel ? std::clamp(tap, q.tapLo, q.tapHi) : std::clamp(tap, 0, q.size - 1);
    }
    if (single) {
        a.n = 1;
    } else {
        a.n = kernel ? 2 * radius : 2 * area;
        a.at.resize(size_t(q.count) * a.n);
        a.weight.resize(size_t(q.count) * a.n);
        for (int i = 0; i < q.count; ++i) {
            const float r = position(i);
            int *at = &a.at[size_t(i) * a.n];
            float *w = &a.weight[size_t(i) * a.n];
            if (kernel) {
                const float p = r * q.size - 0.5f;
                const int whole = floorInt(p);
                const float t = p - float(whole);
                float sum = 0;
                for (int k = 1 - radius; k <= radius; ++k) {
                    const float v = lanczos ? lanczosTable(t - k) : cubicWeight(t - k);
                    at[k + radius - 1] = std::clamp(whole + k, q.tapLo, q.tapHi);
                    w[k + radius - 1] = v;
                    sum += v;
                }
                for (int k = 0; k < a.n; ++k) w[k] /= sum;
                continue;
            }
            for (int j = 0; j < area; ++j) {
                const float o = (j + 0.5f) / area - 0.5f;
                const float p = std::clamp(r + o * q.step, q.lo, q.hi) * q.size - 0.5f;
                const int whole = floorInt(p);
                const float t = p - float(whole);
                at[j * 2] = std::clamp(whole, 0, q.size - 1);
                at[j * 2 + 1] = std::clamp(whole + 1, 0, q.size - 1);
                w[j * 2] = (1.f - t) / area;
                w[j * 2 + 1] = t / area;
            }
        }
    }
    a.lo = *std::min_element(a.at.begin(), a.at.end());
    a.hi = *std::max_element(a.at.begin(), a.at.end());
    a.straight = a.n == 1;
    for (int i = 1; a.straight && i < q.count; ++i) a.straight = a.at[size_t(i)] == a.at[size_t(i) - 1] + 1;
}

// A row of associated samples drawn over a row of the picture: as main() in
// composite.comp, straight-alpha "over" with the layer's opacity and coverage.
void blendRow(float *row, const float *samples, const float *edge, float edgeOther, int count, const Layer &l) {
    const float opacity = l.opacity;
    if (l.blend == BlendMode::Normal && l.blendSpace == BlendSpace::Linear) {
        for (int i = 0; i < count; ++i, row += 4, samples += 4) {
            const float cover = std::clamp(std::min(edge[i], edgeOther) + 0.5f, 0.f, 1.f);
            if (cover <= 0.f) continue;
            const V4 sample = load4(samples);
            if (sample[3] == 1.f && opacity == 1.f && cover == 1.f) {
                // Nothing of the backdrop shows.
                store4(row, atLeastZero4(sample));
                continue;
            }
            const V4 top = straight4(sample);
            const float a = top[3] * opacity * cover;
            const V4 backdrop = load4(row);
            const float remaining = backdrop[3] * (1.f - a);
            const float outAlpha = a + remaining;
            // Working frames are straight-alpha, including nested
            // studios. Store the unassociated colour so another layer
            // does not multiply it by alpha a second time.
            V4 out = splat4(0.f);
            if (outAlpha > 0.f) out = (top * a + backdrop * remaining) / outAlpha;
            out[3] = outAlpha;
            store4(row, out);
        }
        return;
    }
    const bool srgb = l.blendSpace == BlendSpace::Srgb;
    for (int i = 0; i < count; ++i, row += 4, samples += 4) {
        const float cover = std::clamp(std::min(edge[i], edgeOther) + 0.5f, 0.f, 1.f);
        if (cover <= 0.f) continue;
        const Sampled s = straightSample({samples[0], samples[1], samples[2], samples[3]});
        const float a = s.a * opacity * cover;
        float *d = row;
        float top[3] = {s.r, s.g, s.b}, backdrop[3] = {d[0], d[1], d[2]};
        if (srgb) for (int k = 0; k < 3; ++k) { top[k] = encodeSrgb(top[k]); backdrop[k] = encodeSrgb(backdrop[k]); }
        if (l.blend != BlendMode::Normal) {
            // As composite.comp: on display-encoded values, then back to linear.
            for (int k = 0; k < 3; ++k) {
                const float mixed = srgb ? blendChannel(static_cast<int>(l.blend), backdrop[k], top[k])
                    : decodeSrgb(blendChannel(static_cast<int>(l.blend), encodeSrgb(backdrop[k]), encodeSrgb(top[k])));
                top[k] = top[k] * (1.f - d[3]) + mixed * d[3];
            }
        }
        const float remaining = d[3] * (1.f - a);
        const float outAlpha = a + remaining;
        for (int k = 0; k < 3; ++k) {
            const float result = outAlpha > 0.f ? (top[k] * a + backdrop[k] * remaining) / outAlpha : 0.f;
            d[k] = srgb ? decodeSrgb(result) : result;
        }
        d[3] = outAlpha;
    }
}

class CpuEngine final : public Engine {
public:
    explicit CpuEngine(std::string note = {}) : m_note(std::move(note)) {}

    bool isGpu() const override { return false; }
    std::string deviceName() const override { return "CPU"; }
    std::string note() const override { return m_note; }

    int createSource() override {
        const int id = m_nextId++;
        m_sources.emplace(id, CpuSource{});
        return id;
    }
    void destroySource(int id) override { m_sources.erase(id); m_filterMs.erase(id); m_wrapMs.erase(id); }
    // A source's chain, and its light wrap as it is drawn.
    double filterMs(int id) const override {
        const auto it = m_filterMs.find(id), wrap = m_wrapMs.find(id);
        if (it == m_filterMs.end()) return -1;
        return it->second + (wrap == m_wrapMs.end() ? 0.0 : wrap->second);
    }
    void setSourceDenoise(int id, float strength) override {
        auto it = m_sources.find(id);
        if (it == m_sources.end()) return;
        if ((strength > 0.f) != (it->second.denoise > 0.f)) it->second.historyValid = false;
        it->second.denoise = strength;
        it->second.pendingProcess = true;
    }

    void setSourceDeinterlace(int id, DeinterlaceMode mode) override {
        const auto it = m_sources.find(id);
        if (it == m_sources.end() || it->second.deinterlace == mode) return;
        it->second.deinterlace = mode;
        resetSourceHistory(id);
    }
    void resetSourceHistory(int id) override {
        const auto it = m_sources.find(id); if (it == m_sources.end()) return;
        auto &s = it->second;
        s.previousValid = false; s.resetNext = true; s.fieldReady = false;
        s.historyValid = false; s.pendingProcess = true;
    }
    void renderSourceField(int id, int field, bool firstInFrame) override {
        const auto it = m_sources.find(id); if (it == m_sources.end()) return;
        auto &s = it->second;
        if (!s.hasFrame) return;
        field = field == 1 ? 1 : 0;
        if (s.deinterlace == DeinterlaceMode::Off) {
            if (!s.pendingProcess) return;
            denoise(s);
        } else {
            if (s.fieldReady && s.field == field && s.firstField == firstInFrame && !s.pendingProcess) return;
            deinterlace(s, field, firstInFrame);
            denoise(s);
        }
        s.pendingProcess = false; ++s.generation;
    }
    void setSourceFilters(int id, const std::vector<VideoFilter> &filters) override {
        const auto it = m_sources.find(id);
        if (it == m_sources.end() || it->second.filters == filters) return;
        auto &s = it->second;
        std::vector<std::remove_reference_t<decltype(s.steps)>::value_type> next(filters.size());
        for (size_t i = 0; i < filters.size(); ++i)
            if (filterStepKept(s.filters, filters, i)) next[i] = std::move(s.steps[i]);  // as in vk_engine.cpp
        s.filters = filters;
        s.steps = std::move(next);
        s.filtered = false;
        s.failure = FilterFailure::None;
        m_filterMs.erase(id);  // another chain: measured afresh
        m_wrapMs.erase(id);
    }
    void renderSourceFilters(int id, double seconds) override;
    FilterFailure sourceFilterFailure(int id) const override {
        const auto it = m_sources.find(id);
        return it == m_sources.end() ? FilterFailure::None : it->second.failure;
    }
    void setSourceHidden(int id, bool hidden) override {
        const auto it = m_sources.find(id);
        if (it != m_sources.end()) it->second.hide = hidden;
    }
    bool sourceHidden(int id) const override {
        const auto it = m_sources.find(id);
        return it != m_sources.end() && it->second.hide;
    }

    bool updateSource(int id, const CpuFrame &f) override {
        auto it = m_sources.find(id);
        if (it == m_sources.end() || !f.data || f.width <= 0 || f.height <= 0)
            return false;
        beginFrame(it->second, f.width, f.height);
        FrameF &dst = it->second.work;
        if (dst.width != f.width || dst.height != f.height)
            dst.resize(f.width, f.height);
        const bool alpha = hasAlpha(f.format);
        const bool bgr = f.format == PixelFormat::BGRx || f.format == PixelFormat::BGRA;
        auto encoded = [&](uint8_t value) {
            return f.limitedRange ? std::clamp((value - 16.f) / 219.f, 0.f, 1.f) : value / 255.f;
        };
        // Colour untouched by alpha: every value is one of 256 already worked out.
        const bool plain = !alpha || (!f.premultiplied && f.linearAlpha);
        const float *const light = f.limitedRange ? g_curves.fromLimited : g_curves.fromFull;
        m_pool.run(0, f.height, f.width, [&, red = bgr ? 2 : 0, blue = bgr ? 0 : 2](int from, int to) {
            const int width = f.width, stride = f.stride;
            const uint8_t *const data = f.data;
            for (int y = from; y < to; ++y) {
                const uint8_t *row = data + static_cast<size_t>(y) * stride;
                float *out = dst.px(0, y);
                if (plain) {
                    for (int x = 0; x < width; ++x, row += 4, out += 4) {
                        out[0] = light[row[red]];
                        out[1] = light[row[1]];
                        out[2] = light[row[blue]];
                        out[3] = alpha ? row[3] / 255.f : 1.f;
                    }
                    continue;
                }
                for (int x = 0; x < f.width; ++x, row += 4, out += 4) {
                    const uint8_t c0 = row[0], c1 = row[1], c2 = row[2], c3 = row[3];
                    const float a = alpha ? c3 / 255.f : 1.f;
                    const float scale = f.premultiplied ? (a > 0.f ? 1.f / a : 0.f) : 1.f;
                    const float r = std::min(1.f, encoded(bgr ? c2 : c0) * scale);
                    const float g = std::min(1.f, encoded(c1) * scale);
                    const float b = std::min(1.f, encoded(bgr ? c0 : c2) * scale);
                    const float factor = f.linearAlpha ? 1.f : a;
                    out[0] = factor > 0.f ? decodeSrgb(r * factor) / factor : 0.f;
                    out[1] = factor > 0.f ? decodeSrgb(g * factor) / factor : 0.f;
                    out[2] = factor > 0.f ? decodeSrgb(b * factor) / factor : 0.f;
                    out[3] = a;
                }
            }
        });
        finishFrame(it->second);
        return true;
    }

    bool updateSource(int, const DmaBufFrame &) override { return false; }

    // The same arithmetic as yuv_in.comp, read straight from the planes.
    bool updateSource(int id, const YuvInput &f) override {
        auto it = m_sources.find(id);
        if (it == m_sources.end() || !f.plane[0] || f.width <= 0 || f.height <= 0) return false;
        if (f.layout != YuvLayout::YUY2 &&
            (!f.plane[1] || (f.layout != YuvLayout::NV12 && !f.plane[2])))
            return false;
        if (f.depth < 8 || f.depth > 16) return false;
        beginFrame(it->second, f.width, f.height);
        FrameF &dst = it->second.work;
        if (dst.width != f.width || dst.height != f.height) dst.resize(f.width, f.height);
        float kr, kb;
        yuvCoefficients(f.matrix, &kr, &kb);
        const InputParams light = inputParams(f, m_levels);
        const int cw = f.layout == YuvLayout::I444 ? f.width : (f.width + 1) / 2, ch = (f.height + 1) / 2;
        const bool yuy2 = f.layout == YuvLayout::YUY2;
        const bool everyRow = chromaEveryRow(f.layout);
        const bool wide = f.depth > 8;
        const int shift = wide && f.msb ? 16 - f.depth : 0;
        const float top = float((1 << f.depth) - 1);
        // Sample i of a row: a byte, or a little-endian word.
        auto word = [&](const uint8_t *row, int i) -> float {
            if (!wide) return row[i];
            return float((row[i * 2] | (row[i * 2 + 1] << 8)) >> shift);
        };
        auto chroma = [&](int cx, int cy, float *cb, float *cr) {
            if (yuy2) {
                cy = std::clamp(cy, 0, f.height - 1);
                const uint8_t *row = f.plane[0] + static_cast<size_t>(cy) * f.stride[0];
                *cb = word(row, cx * 4 + 1);
                *cr = word(row, cx * 4 + 3);
                return;
            }
            cy = std::clamp(cy, 0, everyRow ? f.height - 1 : ch - 1);
            if (f.layout == YuvLayout::NV12) {
                const uint8_t *row = f.plane[1] + static_cast<size_t>(cy) * f.stride[1];
                *cb = word(row, cx * 2);
                *cr = word(row, cx * 2 + 1);
            } else {
                *cb = word(f.plane[1] + static_cast<size_t>(cy) * f.stride[1], cx);
                *cr = word(f.plane[2] + static_cast<size_t>(cy) * f.stride[2], cx);
            }
        };
        auto mix = [](float a, float b, float t) { return a * (1.f - t) + b * t; };
        const bool sdr = light.kind == 0 && !light.wide;
        const bool deinterlacing = it->second.deinterlace != DeinterlaceMode::Off;
        const bool plain = sdr && !wide && !f.alpha && !f.premultiplied;
        // yuvToRgb's sums, with its divisions turned to multiplications once.
        const float rangeY = f.fullRange ? 1.f / 255.f : 1.f / 219.f, rangeC = f.fullRange ? 1.f / 255.f : 1.f / 224.f;
        const float zeroY = f.fullRange ? 0.f : 16.f;
        m_pool.run(0, f.height, f.width, [&](int from, int to) {
            std::vector<float> rowChroma(size_t(cw) * 2);
            for (int y = from; y < to; ++y) {
                const uint8_t *yrow = f.plane[0] + static_cast<size_t>(y) * f.stride[0];
                const uint8_t *arow = f.alpha ? f.alpha + static_cast<size_t>(y) * f.alphaStride : nullptr;
                float *out = dst.px(0, y);
                const bool fields = !everyRow && ch > 1 && (f.interlaced || deinterlacing);
                const float fy = everyRow ? float(y) : (float(fields ? y / 2 : y) + 0.5f) * 0.5f - 0.5f;
                const int localY = floorInt(fy);
                const float ty = fy - float(localY);
                const int parity = y & 1;
                const int last = (ch - 1 - parity) / 2;
                const int y0 = fields ? 2 * std::clamp(localY, 0, last) + parity : localY;
                const int y1 = fields ? 2 * std::clamp(localY + 1, 0, last) + parity : localY + 1;
                // This row's chroma, between the two rows it falls on.
                for (int cx = 0; cx < cw; ++cx) {
                    float b0, r0, b1, r1;
                    chroma(cx, y0, &b0, &r0);
                    chroma(cx, y1, &b1, &r1);
                    rowChroma[size_t(cx) * 2] = mix(b0, b1, ty);
                    rowChroma[size_t(cx) * 2 + 1] = mix(r0, r1, ty);
                }
                if (plain) {
                    const int width = f.width, lumaStep = yuy2 ? 2 : 1, lastChroma = cw - 1;
                    const bool halved = f.layout != YuvLayout::I444;
                    const float toR = 2.f * (1.f - kr), toB = 2.f * (1.f - kb), toG = 1.f / (1.f - kr - kb), matR = kr, matB = kb;
                    const float scaleY = rangeY, scaleC = rangeC, baseY = zeroY;
                    const float *const c = rowChroma.data();
                    for (int x = 0; x < width; ++x, yrow += lumaStep, out += 4) {
                        // Half-width chroma sits between luma pixels: a quarter
                        // of the way along from one side or the other.
                        int c0 = x, c1 = x;
                        float tx = 0.f;
                        if (halved) {
                            c0 = (x & 1) ? x >> 1 : (x >> 1) - 1;
                            c1 = std::min(c0 + 1, lastChroma);
                            c0 = std::clamp(c0, 0, lastChroma);
                            tx = (x & 1) ? 0.25f : 0.75f;
                        }
                        const float Y = (float(*yrow) - baseY) * scaleY;
                        const float Pb = (c[c0 * 2] * (1.f - tx) + c[c1 * 2] * tx - 128.f) * scaleC;
                        const float Pr = (c[c0 * 2 + 1] * (1.f - tx) + c[c1 * 2 + 1] * tx - 128.f) * scaleC;
                        const float R = Y + toR * Pr, B = Y + toB * Pb;
                        const float G = (Y - matR * R - matB * B) * toG;
                        out[0] = decodeSrgb(R < 0.f ? 0.f : R > 1.f ? 1.f : R);
                        out[1] = decodeSrgb(G < 0.f ? 0.f : G > 1.f ? 1.f : G);
                        out[2] = decodeSrgb(B < 0.f ? 0.f : B > 1.f ? 1.f : B);
                        out[3] = 1.f;
                    }
                    continue;
                }
                for (int x = 0; x < f.width; ++x, out += 4) {
                    const float luma = yuy2 ? word(yrow, x * 2) : word(yrow, x);
                    const float fx = f.layout == YuvLayout::I444 ? float(x) : (float(x) + 0.5f) * 0.5f - 0.5f;
                    const int x0 = floorInt(fx);
                    const float tx = fx - float(x0);
                    const int c0 = std::clamp(x0, 0, cw - 1) * 2, c1 = std::clamp(x0 + 1, 0, cw - 1) * 2;
                    const float cb = mix(rowChroma[size_t(c0)], rowChroma[size_t(c1)], tx);
                    const float cr = mix(rowChroma[size_t(c0) + 1], rowChroma[size_t(c1) + 1], tx);
                    float r, g, b;
                    yuvToRgb(luma, cb, cr, f.fullRange, kr, kb, &r, &g, &b, f.depth);
                    const float a = arow ? word(arow, x) / top : 1.f;
                    if (f.premultiplied) {
                        const float scale = a > 0.f ? 1.f / a : 0.f;
                        r = std::min(1.f, r * scale); g = std::min(1.f, g * scale); b = std::min(1.f, b * scale);
                    }
                    const float factor = f.linearAlpha ? 1.f : a;
                    if (factor > 0.f) {
                        const float e[3] = {r * factor, g * factor, b * factor};
                        if (sdr) { out[0] = decodeSrgb(e[0]); out[1] = decodeSrgb(e[1]); out[2] = decodeSrgb(e[2]); }
                        else inputLinear(light, e, out);
                        for (int k = 0; k < 3; ++k) out[k] /= factor;
                    } else {
                        out[0] = out[1] = out[2] = 0.f;
                    }
                    out[3] = a;
                }
            }
        });
        finishFrame(it->second);
        return true;
    }

    void setHdrLevels(const HdrLevels &levels) override {
        if (levels == m_levels) return;
        m_levels = levels;
        for (auto &kv : m_sources) kv.second.filtered = false;  // tone maps read the white level
    }
    HdrLevels hdrLevels() const override { return m_levels; }

    void setCanvasSize(int w, int h) override {
        if (w <= 0 || h <= 0) return;
        if (m_canvas.width != w || m_canvas.height != h)
            m_canvas.resize(w, h);
    }
    int canvasWidth() const override { return m_canvas.width; }
    int canvasHeight() const override { return m_canvas.height; }

    void composite(const std::vector<Layer> &layers, const Color &bg) override { compositeTo(m_canvas, -1, layers, bg); }

    void compositeInto(int target, int width, int height, const std::vector<Layer> &layers, const Color &bg) override {
        auto it = m_sources.find(target);
        if (it == m_sources.end() || width <= 0 || height <= 0 || width > 16384 || height > 16384) return;
        // Into a second frame first: a layer may not read what is being drawn.
        // The two take turns, so a picture drawn every tick allocates nothing.
        FrameF &out = it->second.drawing;
        try {
            if (out.width != width || out.height != height) { out = {}; out.resize(width, height); }
            compositeTo(out, target, layers, bg);
        } catch (const std::bad_alloc &) {
            return;  // no memory for it: the picture it had stays
        }
        std::swap(it->second.work, out);
        it->second.denoise = 0.f;  // a drawn studio is not noisy
        ++it->second.generation;
    }

    void transitionInto(int target, int width, int height, int from, int to, const TransitionStyle &style,
                        float progress) override {
        if (target == from || target == to || width <= 0 || height <= 0) return;
        auto dst = m_sources.find(target), a = m_sources.find(from), b = m_sources.find(to);
        if (dst == m_sources.end() || a == m_sources.end() || b == m_sources.end()) return;
        const auto &old = shownOf(a->second), &next = shownOf(b->second);
        if (old.width <= 0 || next.width <= 0) return;
        const float t = std::clamp(progress, 0.f, 1.f), cp = std::clamp(style.colourPoint, 0.f, 1.f);
        const Sampled colour{style.colour.r * style.colour.a, style.colour.g * style.colour.a,
                             style.colour.b * style.colour.a, style.colour.a};
        const bool across = style.side == Side::Left || style.side == Side::Right;
        const float length = across ? float(width) : float(height), edge = t * length;
        auto mix = [](const Sampled &p, const Sampled &q, float k) {
            return Sampled{p.r + (q.r - p.r) * k, p.g + (q.g - p.g) * k, p.b + (q.b - p.b) * k, p.a + (q.a - p.a) * k};
        };
        FrameF &out = dst->second.drawing;
        if (out.width != width || out.height != height) { out = {}; out.resize(width, height); }
        m_pool.run(0, height, width, [&](int from, int to) {
            const Pic oldPic(old), nextPic(next);
            // A picture the transition's own size, read at this pixel, is this
            // pixel: the rest of a bilinear sample there is rounding.
            const bool oldSame = old.width == width && old.height == height, nextSame = next.width == width && next.height == height;
            if (style.type == Transition::Fade && oldSame && nextSame) {
                const float k = t;
                for (int y = from; y < to; ++y) {
                    const float *a = oldPic.px(0, y), *z = nextPic.px(0, y);
                    float *p = out.px(0, y);
                    for (int x = 0, w = width; x < w; ++x, a += 4, z += 4, p += 4) {
                        const V4 first = assoc4(load4(a));
                        store4(p, straight4(first + (assoc4(load4(z)) - first) * k));
                    }
                }
                return;
            }
            for (int y = from; y < to; ++y) for (int x = 0; x < width; ++x) {
                const float u = (x + .5f) / width, v = (y + .5f) / height;
                auto at = [&](const Pic &f, float su, float sv) { return sampled(bilinear4(f, su * f.width, sv * f.height)); };
                auto oldHere = [&] { return oldSame ? sampled(assoc4(load4(oldPic.px(x, y)))) : at(oldPic, u, v); };
                auto nextHere = [&] { return nextSame ? sampled(assoc4(load4(nextPic.px(x, y)))) : at(nextPic, u, v); };
                Sampled c;
                switch (style.type) {
                case Transition::Fade: c = mix(oldHere(), nextHere(), t); break;
                case Transition::FadeColour:
                    c = t < cp ? mix(oldHere(), colour, t / cp)
                               : mix(colour, nextHere(), cp >= 1.f ? 1.f : (t - cp) / (1.f - cp));
                    break;
                case Transition::Swipe:
                    c = sideDistance(style.side, x, y, width, height) < edge ? nextHere() : oldHere();
                    break;
                case Transition::Cover: case Transition::Uncover: case Transition::Slide: {
                    const float along = sideDistance(style.side, x, y, width, height);
                    float su = u, sv = v;
                    if (along < edge) {
                        if (style.type != Transition::Uncover) sidePoint(style.side, along + (length - edge), x, y, width, height, &su, &sv);
                        c = at(nextPic, su, sv);
                    } else {
                        if (style.type != Transition::Cover) sidePoint(style.side, along - edge, x, y, width, height, &su, &sv);
                        c = at(oldPic, su, sv);
                    }
                    break;
                }
                case Transition::Wipe:
                    c = mix(oldHere(), nextHere(),
                            wipeCoverage(wipeMaskValue(style.mask, x, y, width, height), t, style.softness, style.invert));
                    break;
                case Transition::Matte: {
                    const Sampled m = at(nextPic, style.matteRect[0] + (style.matteRect[2] - style.matteRect[0]) * u,
                                         style.matteRect[1] + (style.matteRect[3] - style.matteRect[1]) * v);
                    const float k = matteCoverage(m.r, m.g, m.b, style.invert);
                    c = oldHere();
                    c = {c.r * k, c.g * k, c.b * k, c.a * k};
                    break;
                }
                case Transition::Cut: c = t > 0.f ? nextHere() : oldHere(); break;
                }
                c = straightSample(c);
                float *p = out.px(x, y); p[0]=c.r; p[1]=c.g; p[2]=c.b; p[3]=c.a;
            }
        });
        std::swap(dst->second.work, out); dst->second.denoise = 0.f; ++dst->second.generation;
    }

    void compositeTo(FrameF &into, int skip, const std::vector<Layer> &layers, const Color &bg) {
        // Light wrap's pictures, once nothing has drawn with them for a while.
        ++m_composites;
        std::erase_if(m_wraps, [&](const Wrap &w) { return m_composites - w.used > kWrapIdleComposites; });
        if (into.width == 0) return;
        const V4 background{bg.r, bg.g, bg.b, bg.a};
        // An upright first layer clears each row as it draws it: most of a tick's
        // time in memory goes on whole pictures, and this is one fewer.
        bool cleared = false;
        for (const Layer &l : layers) {
            auto it = m_sources.find(l.source);
            if (l.source == skip || it == m_sources.end() || it->second.shown().width == 0 || l.w <= 0 || l.h <= 0)
                continue;
            const VideoFilter *wrap = it->second.filtered && !it->second.hide ? lightWrapOf(it->second.filters) : nullptr;
            const bool clearing = !cleared && !wrap && l.rotation == 0.f;
            if (!cleared && !clearing) clearRows(into, background, 0, into.height);
            cleared = true;
            if (!wrap) { drawLayer(into, shownOf(it->second), l, clearing ? &background : nullptr); continue; }
            const auto started = std::chrono::steady_clock::now();
            wrapLayer(into, it->second.shown(), l, *wrap);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            const auto known = m_wrapMs.find(l.source);
            if (known == m_wrapMs.end()) m_wrapMs[l.source] = ms;
            else known->second += (ms - known->second) * 0.1;
        }
        if (!cleared) clearRows(into, background, 0, into.height);
    }
    void clearRows(FrameF &into, const V4 &background, int first, int last) {
        m_pool.run(first, last, into.width, [&, colour = background](int from, int to) {
            const int width = into.width;
            for (int y = from; y < to; ++y) {
                float *row = into.px(0, y);
                for (int x = 0; x < width; ++x, row += 4) store4(row, colour);
            }
        });
    }
    // A layer with light wrap (lightWrapParams in frame.h), as wrapLayer in vk_engine.cpp.
    void wrapLayer(FrameF &into, const FrameF &src, const Layer &l, const VideoFilter &f) {
        const PixelBounds b = lightWrapRegion(f, l, into.width, into.height);
        if (b.empty()) return;
        const int scale = lightWrapScale(f, into.height);
        const int bw = b.x1 - b.x0, bh = b.y1 - b.y0, hw = (bw + scale - 1) / scale, hh = (bh + scale - 1) / scale;
        // Its pictures, kept for each region size as m_wraps is in vk_engine.cpp:
        // made anew they are two pictures the size of the region every frame.
        auto kept = std::find_if(m_wraps.begin(), m_wraps.end(), [&](const Wrap &w) { return w.width == bw && w.height == bh && w.scale == scale; });
        if (kept == m_wraps.end()) {
            if (m_wraps.size() >= kWrapSetsKept) {
                const auto oldest = std::min_element(m_wraps.begin(), m_wraps.end(), [](const Wrap &a, const Wrap &c) { return a.used < c.used; });
                if (m_composites - oldest->used > kWrapBusyComposites) m_wraps.erase(oldest);
            }
            m_wraps.emplace_back();
            kept = m_wraps.end() - 1;
            kept->width = bw; kept->height = bh; kept->scale = scale;
        }
        kept->used = m_composites;
        FrameF &drawn = kept->images[0], &lit = kept->images[1], &light = kept->images[2], &across = kept->images[3];
        // The steps below write every pixel of what they make; only the picture
        // the layer is drawn onto has to start clear.
        const auto sized = [](FrameF &f, int w, int h) {
            if (f.width == w && f.height == h) return true;
            f.resize(w, h);
            return false;
        };
        const bool whole = layerIsWhole(l, src.width, src.height, into.width, into.height);
        if (!whole) {
            if (sized(drawn, bw, bh)) clearRows(drawn, splat4(0.f), 0, bh);
            Layer plain = l;
            plain.x -= float(b.x0); plain.y -= float(b.y0);
            plain.blend = BlendMode::Normal;
            plain.blendSpace = BlendSpace::Linear;
            drawLayer(drawn, src, plain);
        }
        const FrameF &alone = whole ? src : drawn;
        sized(lit, bw, bh);
        sized(light, hw, hh);
        sized(across, hw, hh);
        runStep(into, light, f, lightWrapParams(f, 0, into.height, b.x0, b.y0, hw, hh), &alone);
        runStep(light, across, f, lightWrapParams(f, 1, into.height, b.x0, b.y0, hw, hh));
        runStep(across, light, f, lightWrapParams(f, 2, into.height, b.x0, b.y0, hw, hh));
        runStep(alone, lit, f, lightWrapParams(f, 3, into.height, b.x0, b.y0, hw, hh), &light);
        Layer region;
        region.x = float(b.x0); region.y = float(b.y0); region.w = float(bw); region.h = float(bh);
        region.filter = ScaleFilter::Point;
        region.blend = l.blend;
        region.blendSpace = l.blendSpace;
        drawLayer(into, lit, region);
    }

    // `clear`: only for an upright layer; what into holds is first replaced by this colour.
    void drawLayer(FrameF &into, const FrameF &src, const Layer &l, const V4 *clear = nullptr) {
        const float fx = std::fabs(l.u1 - l.u0) * src.width / l.w, fy = std::fabs(l.v1 - l.v0) * src.height / l.h;
        const ScaleFilter filter = resolveFilter(l.filter, fx, fy);
        // Taps stay inside the crop, as in composite.comp.
        // And inside the picture, whatever the layer asks for.
        const int tapLoX = std::clamp(static_cast<int>(std::floor(std::min(l.u0, l.u1) * src.width + 0.001f)), 0, src.width - 1);
        const int tapLoY = std::clamp(static_cast<int>(std::floor(std::min(l.v0, l.v1) * src.height + 0.001f)), 0, src.height - 1);
        const int tapHiX = std::clamp(static_cast<int>(std::ceil(std::max(l.u0, l.u1) * src.width - 0.001f)) - 1, tapLoX, src.width - 1);
        const int tapHiY = std::clamp(static_cast<int>(std::ceil(std::max(l.v0, l.v1) * src.height - 0.001f)) - 1, tapLoY, src.height - 1);
        const float stepU = (l.u1 > l.u0 ? 1.f : -1.f) * fx / src.width;
        const float stepV = (l.v1 > l.v0 ? 1.f : -1.f) * fy / src.height;
        const PixelBounds b = layerBounds(l, into.width, into.height);
        if (b.empty()) {
            if (clear) clearRows(into, *clear, 0, into.height);
            return;
        }
        if (clear) {
            clearRows(into, *clear, 0, b.y0);
            clearRows(into, *clear, b.y1, into.height);
        }
        const float hw = l.w * 0.5f, hh = l.h * 0.5f;
        const float cx = l.x + hw, cy = l.y + hh;
        const float tu = 0.5f / src.width, tv = 0.5f / src.height;
        const float loU = std::clamp(std::min(l.u0, l.u1), 0.f, 1.f) + tu, loV = std::clamp(std::min(l.v0, l.v1), 0.f, 1.f) + tv;
        const float hiU = std::max(loU, std::clamp(std::max(l.u0, l.u1), 0.f, 1.f) - tu);
        const float hiV = std::max(loV, std::clamp(std::max(l.v0, l.v1), 0.f, 1.f) - tv);
        const int count = b.x1 - b.x0;

        if (l.rotation == 0.f) {
            // Upright: columns and rows do not mix, so down the source once
            // for the whole row, then across, in place of taps × taps a pixel.
            AxisTaps across, down;
            buildAxis(across, {b.x0, count, cx, hw, l.w, l.u0, l.u1, src.width, tapLoX, tapHiX, loU, hiU, filter, fx, stepU});
            buildAxis(down, {b.y0, b.y1 - b.y0, cy, hh, l.h, l.v0, l.v1, src.height, tapLoY, tapHiY, loV, hiV, filter, fy, stepV});
            const int columns = across.hi - across.lo + 1;
            m_pool.run(b.y0, b.y1, std::max(count, columns), [&](int from, int to) {
                std::vector<float> column(size_t(columns) * 4), mixed(across.straight ? 0 : size_t(count) * 4);
                const Pic pic(src);
                const int width = count, reads = columns, firstRead = across.lo, nDown = down.n, nAcross = across.n, top = b.y0;
                float *const summed = column.data(), *const out = mixed.data();
                const int *const atDown = down.at.data(), *const atAcross = across.at.data();
                const float *const weightDown = down.weight.data(), *const weightAcross = across.weight.data();
                for (int y = from; y < to; ++y) {
                    const size_t row = size_t(y - top);
                    if (clear) {
                        const V4 colour = *clear;
                        float *p = into.px(0, y);
                        for (int x = 0, w = into.width; x < w; ++x, p += 4) store4(p, colour);
                    }
                    for (int k = 0; k < nDown; ++k) {
                        const float *in = pic.px(firstRead, atDown[row * nDown + k]);
                        float *sum = summed;
                        if (nDown == 1) {
                            for (int i = 0; i < reads; ++i, in += 4, sum += 4) store4(sum, assoc4(load4(in)));
                        } else if (k == 0) {
                            const float w = weightDown[row * nDown];
                            for (int i = 0; i < reads; ++i, in += 4, sum += 4) store4(sum, assoc4(load4(in)) * w);
                        } else {
                            const float w = weightDown[row * nDown + k];
                            for (int i = 0; i < reads; ++i, in += 4, sum += 4) store4(sum, load4(sum) + assoc4(load4(in)) * w);
                        }
                    }
                    const float *samples = summed;
                    if (!across.straight) {
                        samples = out;
                        for (int i = 0; i < width; ++i) {
                            const int *at = atAcross + size_t(i) * nAcross;
                            if (nAcross == 1) {
                                store4(out + size_t(i) * 4, load4(summed + size_t(at[0] - firstRead) * 4));
                                continue;
                            }
                            const float *w = weightAcross + size_t(i) * nAcross;
                            V4 sum = splat4(0.f);
                            for (int k = 0; k < nAcross; ++k) sum += load4(summed + size_t(at[k] - firstRead) * 4) * w[k];
                            store4(out + size_t(i) * 4, sum);
                        }
                    }
                    blendRow(into.px(b.x0, y), samples, across.edge.data(), down.edge[row], width, l);
                }
            });
            return;
        }

        const float cosR = std::cos(l.rotation), sinR = std::sin(l.rotation);
        const int nx = areaTaps(fx), ny = areaTaps(fy);
        const float inv = 1.f / float(nx * ny);
        m_pool.run(b.y0, b.y1, count, [&](int from, int to) {
            std::vector<float> samples(static_cast<size_t>(count) * 4), edges(static_cast<size_t>(count));
            const Pic pic(src);
            for (int y = from; y < to; ++y) {
                for (int i = 0; i < count; ++i) {
                    const float dx = (b.x0 + i) + 0.5f - cx, dy = y + 0.5f - cy;
                    const float qx = cosR * dx + sinR * dy + hw;
                    const float qy = -sinR * dx + cosR * dy + hh;
                    const float edge = std::min(std::min(qx, l.w - qx), std::min(qy, l.h - qy));
                    edges[size_t(i)] = edge;
                    if (edge + 0.5f <= 0.f) continue;
                    const float tx = qx / l.w, ty = qy / l.h;
                    const float ru = l.u0 + (l.u1 - l.u0) * tx, rv = l.v0 + (l.v1 - l.v0) * ty;
                    V4 s;
                    if (filter == ScaleFilter::Bicubic || filter == ScaleFilter::Lanczos) {
                        s = kernel4(pic, ru * pic.width, rv * pic.height, filter == ScaleFilter::Lanczos ? 3 : 2,
                                    filter == ScaleFilter::Lanczos, tapLoX, tapLoY, tapHiX, tapHiY);
                    } else if (filter == ScaleFilter::Area) {
                        s = splat4(0.f);
                        for (int ay = 0; ay < ny; ++ay)
                            for (int ax = 0; ax < nx; ++ax) {
                                const float ox = (ax + 0.5f) / nx - 0.5f, oy = (ay + 0.5f) / ny - 0.5f;
                                const float u = std::clamp(ru + ox * stepU, loU, hiU);
                                const float v = std::clamp(rv + oy * stepV, loV, hiV);
                                s += bilinear4(pic, u * pic.width, v * pic.height);
                            }
                        s *= inv;
                    } else if (filter == ScaleFilter::Point) {
                        s = assoc4(load4(pic.px(std::clamp(floorInt(ru * pic.width), tapLoX, tapHiX),
                                                std::clamp(floorInt(rv * pic.height), tapLoY, tapHiY))));
                    } else {
                        s = bilinear4(pic, std::clamp(ru, loU, hiU) * pic.width, std::clamp(rv, loV, hiV) * pic.height);
                    }
                    store4(&samples[size_t(i) * 4], s);
                }
                blendRow(into.px(b.x0, y), samples.data(), edges.data(), INFINITY, count, l);
            }
        });
    }

    bool readPreview(int w, int h, Bgra8Frame &out) override { return previewOf(m_canvas, w, h, out); }
    bool readSourcePreview(int source, int w, int h, Bgra8Frame &out) override {
        const auto it = m_sources.find(source);
        return it != m_sources.end() && previewOf(shownOf(it->second), w, h, out);
    }
    bool readSourceInput(int source, int w, int h, Bgra8Frame &out) override {
        const auto it = m_sources.find(source);
        return it != m_sources.end() && previewOf(it->second.base(), w, h, out);
    }
    bool previewOf(const FrameF &picture, int w, int h, Bgra8Frame &out) {
        if (picture.width == 0 || w <= 0 || h <= 0) return false;
        if (out.width != w || out.height != h) out.resize(w, h);
        m_pool.run(0, h, w, [&, sizeW = w, sizeH = h, bytes = out.data.data()](int from, int to) {
            const Pic pic(picture);
            const int width = sizeW, height = sizeH, picW = pic.width, picH = pic.height;
            for (int y = from; y < to; ++y) {
                uint8_t *row = bytes + static_cast<size_t>(y) * width * 4;
                for (int x = 0; x < width; ++x, row += 4) {
                    const V4 s = straight4(bilinear4(pic, (x + 0.5f) / width * picW, (y + 0.5f) / height * picH));
                    row[0] = round8(encodeSrgb(s[2]) * 255.f);
                    row[1] = round8(encodeSrgb(s[1]) * 255.f);
                    row[2] = round8(encodeSrgb(s[0]) * 255.f);
                    row[3] = round8(s[3] * 255.f);
                }
            }
        });
        return true;
    }

    // As kernelSample in yuv.comp: bicubic or Lanczos about canvas position
    // (cx, cy), stretched over the footprint when shrinking.
    static void outputKernel(const FrameF &pic, float cx, float cy, float fx, float fy, bool lanczos, float out[3]) {
        const float radius = lanczos ? 3.f : 2.f;
        const int loX = int(std::floor(cx - radius * fx)) + 1, hiX = int(std::floor(cx + radius * fx));
        const int loY = int(std::floor(cy - radius * fy)) + 1, hiY = int(std::floor(cy + radius * fy));
        float sum[3] = {0, 0, 0}, wsum = 0;
        for (int ty = loY; ty <= hiY; ++ty) {
            const float dy = (float(ty) - cy) / fy;
            const float wy = lanczos ? lanczosWeight(dy) : cubicWeight(dy);
            for (int tx = loX; tx <= hiX; ++tx) {
                const float dx = (float(tx) - cx) / fx;
                const float w = wy * (lanczos ? lanczosWeight(dx) : cubicWeight(dx));
                const float *t = pic.px(std::clamp(tx, 0, pic.width - 1), std::clamp(ty, 0, pic.height - 1));
                for (int k = 0; k < 3; ++k) sum[k] += w * t[k];
                wsum += w;
            }
        }
        for (int k = 0; k < 3; ++k) out[k] = std::max(sum[k] / wsum, 0.f);
    }

    // As yuv.comp: the texel at the picture's own size, else by the chosen
    // filter, by default the mean of ceil(scale)^2 bilinear samples, in
    // linear light, then encoded.
    static void outputEncoded(const FrameF &pic, const OutputParams &q, int x, int y, int w, int h, float rgb[3]) {
        const int W = pic.width, H = pic.height;
        x = std::min(x, w - 1);
        y = std::min(y, h - 1);
        if (w == W && h == H) {
            const float *p = pic.px(x, y);
            outputEncode(q, p, rgb);
            return;
        }
        const float sx = float(W) / float(w), sy = float(H) / float(h);
        const float cx = (float(x) + 0.5f) * sx - 0.5f, cy = (float(y) + 0.5f) * sy - 0.5f;
        if (q.filter == int32_t(ScaleFilter::Bilinear)) {
            const float fx = std::clamp(cx, 0.f, float(W - 1)), fy = std::clamp(cy, 0.f, float(H - 1));
            const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
            const int x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
            const float ax = fx - float(x0), ay = fy - float(y0);
            float c[3];
            for (int k = 0; k < 3; ++k) {
                const float top = pic.px(x0, y0)[k] + (pic.px(x1, y0)[k] - pic.px(x0, y0)[k]) * ax;
                const float bottom = pic.px(x0, y1)[k] + (pic.px(x1, y1)[k] - pic.px(x0, y1)[k]) * ax;
                c[k] = top + (bottom - top) * ay;
            }
            outputEncode(q, c, rgb);
            return;
        }
        if (q.filter == int32_t(ScaleFilter::Bicubic) || q.filter == int32_t(ScaleFilter::Lanczos)) {
            float c[3];
            outputKernel(pic, cx, cy, std::max(sx, 1.f), std::max(sy, 1.f), q.filter == int32_t(ScaleFilter::Lanczos), c);
            outputEncode(q, c, rgb);
            return;
        }
        const int kx = std::max(1, int(std::ceil(sx - 1e-4f))), ky = std::max(1, int(std::ceil(sy - 1e-4f)));
        const Pic view(pic);
        V4 sum = splat4(0.f);
        for (int j = 0; j < ky; ++j)
            for (int i = 0; i < kx; ++i) {
                const float fx = std::clamp((float(x) + (float(i) + 0.5f) / float(kx)) * sx - 0.5f, 0.f, float(W - 1));
                const float fy = std::clamp((float(y) + (float(j) + 0.5f) / float(ky)) * sy - 0.5f, 0.f, float(H - 1));
                const int x0 = floorInt(fx), y0 = floorInt(fy);
                const int x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
                const float ax = fx - float(x0), ay = fy - float(y0);
                const V4 a = load4(view.px(x0, y0)), d = load4(view.px(x0, y1));
                const V4 top = a + (load4(view.px(x1, y0)) - a) * ax;
                const V4 bottom = d + (load4(view.px(x1, y1)) - d) * ax;
                sum += top + (bottom - top) * ay;
            }
        sum /= float(kx * ky);
        if (q.transfer == 0) {
            for (int k = 0; k < 3; ++k) rgb[k] = encodeSrgb(sum[k]);
            return;
        }
        const float mean[3] = {sum[0], sum[1], sum[2]};
        outputEncode(q, mean, rgb);
    }

    bool readOutput(const OutputColour &colour, YuvFrame &out, int width, int height) override {
        return outputOf(m_canvas, colour, out, width, height);
    }
    bool readSourceOutput(int source, const OutputColour &colour, YuvFrame &out, int width, int height) override {
        const auto it = m_sources.find(source);
        return it != m_sources.end() && outputOf(shownOf(it->second), colour, out, width, height);
    }
    // Luma, and the chroma of a row of blocks (the mean of the encoded R'G'B'
    // each covers), as codes. Everything they use comes in by value, so a byte
    // written does not send the loop back to memory for it.
    static inline void putCode(uint8_t *at, int bytes, int shift, int code) {
        if (bytes == 1) { *at = uint8_t(code); return; }
        const int v = code << shift;
        at[0] = uint8_t(v & 255);
        at[1] = uint8_t(v >> 8);
    }
    static void lumaRow(uint8_t *out, const float *e, int w, const OutputParams &q, int top, int bytes, int shift) {
        const float kr = q.kr, kg = q.kg, kb = q.kb, offset = q.yOffset, scale = q.yScale;
        for (int x = 0; x < w; ++x, e += 3, out += bytes) putCode(out, bytes, shift, roundCode(offset, scale, kr * e[0] + kg * e[1] + kb * e[2], top));
    }
    static void chromaRow(uint8_t *first, uint8_t *second, const float *encoded, int w, int bw, int bh, const OutputParams &q, int top, int bytes, int shift) {
        const float kr = q.kr, kg = q.kg, kb = q.kb, offset = q.cOffset, scale = q.cScale, cbDiv = q.cbDiv, crDiv = q.crDiv;
        const float inv = 1.f / float(bw * bh);
        for (int bx = 0; bx < w; bx += bw) {
            float sr = 0, sg = 0, sb = 0;
            for (int dy = 0; dy < bh; ++dy)
                for (int dx = 0; dx < bw; ++dx) {
                    const float *e = encoded + (size_t(dy) * w + (bx + dx < w ? bx + dx : w - 1)) * 3;
                    sr += e[0]; sg += e[1]; sb += e[2];
                }
            const float r = sr * inv, g = sg * inv, b = sb * inv;
            const float yy = kr * r + kg * g + kb * b;
            const int cb = roundCode(offset, scale, (b - yy) / cbDiv, top), cr = roundCode(offset, scale, (r - yy) / crDiv, top);
            putCode(first, bytes, shift, cb);
            first += bytes;
            if (second) {
                putCode(second, bytes, shift, cr);
                second += bytes;
            } else {
                putCode(first, bytes, shift, cr);
                first += bytes;
            }
        }
    }

    bool outputOf(const FrameF &pic, const OutputColour &colour, YuvFrame &out, int width, int height) {
        if (pic.width == 0) return false;
        const int w = width > 0 ? width : pic.width, h = height > 0 ? height : pic.height;
        const ColourFormat format = colour.format;
        if (out.width != w || out.height != h || out.format != format) out.resize(w, h, format);
        const OutputParams q = outputParams(colour);
        const int top = maxCode(format), bytes = out.sampleBytes();
        const int shift = format == ColourFormat::P010 ? 6 : 0;
        uint8_t *const planeY = out.y.data(), *const planeU = out.u.data(), *const planeV = out.v.data();
        const int yRow = out.rowBytes(0), cRow = out.rowBytes(1), vRow = out.rowBytes(2);
        const bool rgb = formatIsRgb(format), halfW = chromaHalfWidth(format), halfH = chromaHalfHeight(format);
        const bool interleaved = chromaInterleaved(format);
        const int bw = halfW ? 2 : 1, bh = halfH ? 2 : 1;
        // The canvas's own size and the sRGB curve is what most outputs want.
        const bool direct = w == pic.width && h == pic.height && q.transfer == 0;
        m_pool.run(0, (h + bh - 1) / bh, w * bh, [&](int from, int to) {
            // A block row's encoded R'G'B'. A block past the edge of an odd
            // size takes the last pixel's colour for the part that is not there.
            std::vector<float> encoded(size_t(w) * 3 * bh);
            for (int band = from; band < to; ++band) {
                const int by = band * bh;
                for (int dy = 0; dy < bh; ++dy) {
                    float *e = &encoded[size_t(dy) * w * 3];
                    const int y = std::min(by + dy, h - 1);
                    if (direct) {
                        const float *p = pic.px(0, y);
                        for (int x = 0; x < w; ++x, p += 4, e += 3) {
                            e[0] = encodeSrgb(p[0]);
                            e[1] = encodeSrgb(p[1]);
                            e[2] = encodeSrgb(p[2]);
                        }
                    } else {
                        for (int x = 0; x < w; ++x, e += 3) outputEncoded(pic, q, x, y, w, h, e);
                    }
                    if (by + dy >= h) continue;
                    e = &encoded[size_t(dy) * w * 3];
                    if (rgb) {
                        uint8_t *px = planeY + size_t(by + dy) * yRow;
                        for (int x = 0; x < w; ++x, e += 3, px += 4) {
                            px[0] = uint8_t(roundCode(0.f, q.yScale, e[2], 255));
                            px[1] = uint8_t(roundCode(0.f, q.yScale, e[1], 255));
                            px[2] = uint8_t(roundCode(0.f, q.yScale, e[0], 255));
                            px[3] = 255;
                        }
                    } else {
                        lumaRow(planeY + size_t(by + dy) * yRow, e, w, q, top, bytes, shift);
                    }
                }
                if (rgb) continue;
                chromaRow(planeU + size_t(band) * cRow, interleaved ? nullptr : planeV + size_t(band) * vRow, encoded.data(), w, bw, bh, q, top, bytes, shift);
            }
        });
        return true;
    }

private:
    // A source: its converted frame and, with noise reduction on, two cleaned
    // frames used in turn (the other is the history), as in vk_engine.cpp.
    struct CpuSource {
        FrameF work, previous, fieldFrame;
        FrameF drawing;  // the next picture of a group or transition, then `work`'s turn
        DeinterlaceMode deinterlace = DeinterlaceMode::Off;
        bool hasFrame = false, previousValid = false, resetNext = false, fieldReady = false, pendingProcess = false;
        int field = 0;
        bool firstField = true;
        FrameF clean[2];
        int cleanIndex = 0;
        float denoise = 0.f;
        bool historyValid = false;
        std::vector<float> brightness;  // working space
        const FrameF &processed() const { return deinterlace != DeinterlaceMode::Off && fieldReady ? fieldFrame : work; }
        const FrameF &base() const { return denoise > 0.f && historyValid ? clean[cleanIndex] : processed(); }
        // Video filters: one picture per step, or a delay's held pictures,
        // oldest first, the shown one at the front.
        struct Step {
            FrameF out, mid;  // mid: a two-pass step's first pass, or the matte
            std::deque<std::pair<FrameF, double>> held;
            std::vector<FrameF> spare;  // a delay's pictures past their time, to copy the next into
            matting::Plan plan;      // RemoveBackground's network, its working space,
            std::vector<float> arena;
            size_t part = 0;         // the part of a run to do next (0: none under way)
            FrameF guide[6], back[2], cut;  // after the network, as recordGuided in vk_engine.cpp
            bool history = false;
            int flip = 0;
            double origin = 0;
            bool started = false;
            uint64_t pushed = ~0ull;
            const FrameF &output() const { return held.empty() ? out : held.front().first; }
        };
        std::vector<VideoFilter> filters;
        std::vector<Step> steps;
        bool filtered = false;
        uint64_t generation = 0, filteredGeneration = ~0ull;
        const FrameF &shown() const { return filtered && !steps.empty() ? steps.back().output() : base(); }
        bool hide = false;  // setSourceHidden
        bool hidden() const { return hide || (!filtered && chainFailsClosed(filters)); }
        FilterFailure failure = FilterFailure::None;
        double retryAt = 0;  // a chain that failed is not tried again before this
    };
    // What a source shows, or one clear picture when it must not be seen.
    const FrameF &shownOf(const CpuSource &s) const { return s.hidden() ? m_nothing : s.shown(); }
    const FrameF m_nothing = [] { FrameF f; f.resize(1, 1); return f; }();

    void renderChain(int id, CpuSource &s, double seconds);
    static void beginFrame(CpuSource &s, int w, int h) {
        s.previousValid = s.deinterlace != DeinterlaceMode::Off && s.hasFrame && !s.resetNext && s.work.width == w && s.work.height == h;
        if (s.deinterlace != DeinterlaceMode::Off) std::swap(s.work, s.previous);
        s.resetNext = false; s.fieldReady = false;
    }
    void finishFrame(CpuSource &s) {
        s.hasFrame = true; s.pendingProcess = true; ++s.generation;
        if (s.deinterlace == DeinterlaceMode::Off) { denoise(s); s.pendingProcess = false; }
    }

    // Independently authored spatial/temporal filter; deinterlace.comp follows
    // this arithmetic. The retained field is exact. Motion uses interpolation
    // along the least-changing local edge; still areas retain both fields.
    void deinterlace(CpuSource &s, int parity, bool first) {
        const auto &cur = s.work;
        auto &out = s.fieldFrame;
        if (out.width != cur.width || out.height != cur.height) out.resize(cur.width, cur.height);
        auto rows = [](int y, int height, int field) {
            field = std::min(field, height-1);
            const int last = field + 2*((height-1-field)/2);
            return std::array<int,2>{std::clamp(y-1, field, last), std::clamp(y+1, field, last)};
        };
        auto fieldSample = [&](const FrameF &frame, int x, int y, int field) {
            std::array<float,4> result{};
            field = std::min(field, frame.height-1);
            if ((y & 1) == field) std::copy_n(frame.px(x,y),4,result.begin());
            else {
                const auto r = rows(y,frame.height,field);
                for (int c=0;c<4;++c) result[c] = .5f*(frame.px(x,r[0])[c]+frame.px(x,r[1])[c]);
            }
            return result;
        };
        m_pool.run(0, cur.height, cur.width, [&](int from, int to) {
        for (int y=from;y<to;++y) for (int x=0;x<cur.width;++x) {
            auto spatial = fieldSample(cur,x,y,parity);
            const auto r = rows(y,cur.height,parity);
            if (s.deinterlace == DeinterlaceMode::Discard) {
                const int selected = std::min(parity,cur.height-1);
                const int last = selected+2*((cur.height-1-selected)/2);
                const int row = std::clamp((y/2)*2+selected,selected,last);
                std::copy_n(cur.px(x,row),4,spatial.begin());
            } else if (s.deinterlace == DeinterlaceMode::Blend) {
                const auto &neighbour = first && s.previousValid ? s.previous : cur;
                const auto other = fieldSample(neighbour,x,y,1-parity);
                for (int c=0;c<4;++c) spatial[c] = .5f*(spatial[c]+other[c]);
            } else if (s.deinterlace == DeinterlaceMode::Adaptive && (y&1) != parity) {
                float best = 1e10f;
                // Zero slope wins ties; a small slope penalty avoids jitter.
                for (int dx : {0,-1,1,-2,2}) {
                    float score = .001f * std::abs(dx);
                    for (int tap=-1;tap<=1;++tap) for (int c=0;c<3;++c)
                        score += std::abs(cur.px(std::clamp(x+tap-dx,0,cur.width-1),r[0])[c] -
                                          cur.px(std::clamp(x+tap+dx,0,cur.width-1),r[1])[c]);
                    if (score < best) {
                        best = score;
                        for (int c=0;c<4;++c) spatial[c] = .5f*(cur.px(std::clamp(x-dx,0,cur.width-1),r[0])[c] +
                                                                             cur.px(std::clamp(x+dx,0,cur.width-1),r[1])[c]);
                    }
                }
                if (s.previousValid) {
                    float change = 0;
                    for (int dy=-2;dy<=2;++dy) for (int dx=-1;dx<=1;++dx) for (int c=0;c<4;++c) {
                        const int xx=std::clamp(x+dx,0,cur.width-1), yy=std::clamp(y+dy,0,cur.height-1);
                        change = std::max(change,std::abs(cur.px(xx,yy)[c]-s.previous.px(xx,yy)[c]));
                    }
                    const float motion = std::clamp((change-.01f)/.03f,0.f,1.f);
                    for (int c=0;c<4;++c) spatial[c] = cur.px(x,y)[c] + motion*(spatial[c]-cur.px(x,y)[c]);
                }
            }
            std::copy(spatial.begin(),spatial.end(),out.px(x,y));
        }
        });
        s.fieldReady = true; s.field = parity; s.firstField = first;
    }

    // The same arithmetic as denoise.comp.
    void denoise(CpuSource &s) {
        if (s.denoise <= 0.f) {
            s.historyValid = false;
            std::vector<float>().swap(s.brightness);
            return;
        }
        const FrameF &cur = s.processed();
        const int next = 1 - s.cleanIndex;
        for (FrameF &f : s.clean)
            if (f.width != cur.width || f.height != cur.height) {
                f.resize(cur.width, cur.height);
                s.historyValid = false;
            }
        const DenoiseParams dp = denoiseParams(s.denoise);
        const FrameF &hist = s.clean[s.cleanIndex];
        FrameF &out = s.clean[next];
        auto luma = [](const float *c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
        // Each pixel is looked at 25 times as a neighbour: its brightness and
        // that brightness's root once, up front.
        const int W = cur.width, H = cur.height;
        const size_t pixels = size_t(W) * H;
        const bool history = s.historyValid;
        s.brightness.resize(pixels * (history ? 3 : 2));
        float *const lumaPlane = s.brightness.data(), *const rootPlane = lumaPlane + pixels, *const pastPlane = rootPlane + pixels;
        m_pool.run(0, H, W, [&](int from, int to) {
            for (int y = from; y < to; ++y)
                for (int x = 0; x < W; ++x) {
                    const size_t at = size_t(y) * W + x;
                    lumaPlane[at] = luma(cur.px(x, y));
                    rootPlane[at] = std::sqrt(std::max(lumaPlane[at], 0.f));
                    if (history) pastPlane[at] = std::sqrt(std::max(luma(hist.px(x, y)), 0.f));
                }
        });
        m_pool.run(0, H, W, [&](int from, int to) {
            const Pic now(cur), past(hist);
            const float closeness = 1.f / dp.sigma, threshold = dp.threshold, keepMin = dp.keepMin;
            for (int y = from; y < to; ++y) {
                float *o = out.px(0, y);
                for (int x = 0; x < W; ++x, o += 4) {
                    const float *c = now.px(x, y);
                    const size_t self = size_t(y) * W + x;
                    const float yc = lumaPlane[self], gc = rootPlane[self];
                    float cs[3] = {0, 0, 0}, ws = 0, change = 0;
                    for (int dy = -2; dy <= 2; ++dy) {
                        const int qy = std::clamp(y + dy, 0, H - 1);
                        for (int dx = -2; dx <= 2; ++dx) {
                            const int qx = std::clamp(x + dx, 0, W - 1);
                            const size_t at = size_t(qy) * W + qx;
                            const float *n = now.px(qx, qy);
                            const float yn = lumaPlane[at], gn = rootPlane[at];
                            const float dg = (gn - gc) * closeness;
                            const float w = fallTable(dg * dg);
                            for (int k = 0; k < 3; ++k) cs[k] += w * (n[k] - yn);
                            ws += w;
                            if (history && dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) change += gn - pastPlane[at];
                        }
                    }
                    float clean[3];
                    for (int k = 0; k < 3; ++k) clean[k] = yc + cs[k] / ws;
                    if (history) {
                        const float *h = past.px(x, y);
                        const float keep = std::clamp(std::fabs(change / 9.f) / threshold, keepMin, 1.f);
                        for (int k = 0; k < 3; ++k) clean[k] = h[k] + keep * (clean[k] - h[k]);
                    }
                    for (int k = 0; k < 3; ++k) o[k] = std::max(clean[k], 0.f);
                    o[3] = c[3];
                }
            }
        });
        s.cleanIndex = next;
        s.historyValid = true;
    }

    void runStep(const FrameF &in, FrameF &out, const VideoFilter &f, const FilterParams &q, const FrameF *aux = nullptr);
    static void stepRows(const FrameF &in, FrameF &out, const VideoFilter &f, const FilterParams &q, const FrameF *aux, const float *gaussian, int from, int to);

    std::unordered_map<int, CpuSource> m_sources;
    std::unordered_map<int, double> m_filterMs, m_wrapMs;  // each chain's and light wrap's time, smoothed
    HdrLevels m_levels;
    FrameF m_canvas;
    int m_nextId = 1;
    RowPool m_pool{drawingThreads()};
    // Light wrap's pictures for each region size, and the composite that last
    // used them (see kWrapSetsKept in frame.h).
    struct Wrap {
        int width = 0, height = 0, scale = 4;
        FrameF images[4];
        uint64_t used = 0;
    };
    std::vector<Wrap> m_wraps;
    uint64_t m_composites = 0;
    std::string m_note;
};

// The same arithmetic as filter.comp, on the same FilterParams.
void CpuEngine::runStep(const FrameF &in, FrameF &out, const VideoFilter &f, const FilterParams &q, const FrameF *aux) {
    // The blur's weights, once rather than a pixel at a time.
    std::vector<float> gaussian;
    if (q.kind == 10 || q.kind == 11) {
        gaussian.resize(size_t(q.ix) * 2 + 1);
        for (int k = -q.ix; k <= q.ix; ++k) {
            const float x = float(k) / q.p[0];
            gaussian[size_t(k + q.ix)] = std::exp(-0.5f * x * x);
        }
    }
    m_pool.run(0, out.height, out.width, [&](int from, int to) { stepRows(in, out, f, q, aux, gaussian.data(), from, to); });
}

void CpuEngine::stepRows(const FrameF &in, FrameF &out, const VideoFilter &f, const FilterParams &q, const FrameF *aux, const float *gaussian, int from, int to) {
    const float *P = q.p;
    const Pic pic(in);
    const int W = in.width, H = in.height, outWidth = out.width, outHeight = out.height;
    const float *const lut = f.lut ? f.lut->rgb.data() : nullptr;
    // The blur's second pass adds whole rows, so it reads along them.
    std::vector<float> sums(q.kind == 10 && (q.mode == 1 || q.mode == 3) && outWidth == W ? size_t(W) * 4 : 0);
    auto at = [&](int x, int y) { return pic.px(std::clamp(x, 0, W - 1), std::clamp(y, 0, H - 1)); };
    auto assoc = [](const float *c) { return Sampled{c[0] * c[3], c[1] * c[3], c[2] * c[3], c[3]}; };
    auto ramp = [](float t) { return t * t * (3.f - 2.f * t); };
    auto gauss = [&](int k) { return gaussian[k + q.ix]; };
    auto clamp01 = [](float v) { return std::clamp(v, 0.f, 1.f); };
    auto lutAt = [&](int r, int g, int b, int k) { return lut[((size_t(b) * q.ix + g) * q.ix + r) * 3 + k]; };
    auto scrollTap = [&](int x, int y) {
        if (q.mode == 1) { x = (x + 2 * W) % W; y = (y + 2 * H) % H; }
        else if (x < 0 || y < 0 || x >= W || y >= H) return Sampled{0, 0, 0, 0};
        return assoc(pic.px(x, y));
    };
    for (int y = from; y < to; ++y) {
        float *const written = out.px(0, y);
        if (!sums.empty()) {
            float wsum = 0;
            std::fill(sums.begin(), sums.end(), 0.f);
            for (int k = -q.ix; k <= q.ix; ++k) {
                matting::addScaled(sums.data(), pic.px(0, std::clamp(y + k, 0, H - 1)), gauss(k), W * 4);
                wsum += gauss(k);
            }
            const float *t = sums.data();
            float *o = written;
            for (int x = 0; x < W; ++x, t += 4, o += 4) store4(o, q.mode == 1 ? straight4(load4(t) / wsum) : load4(t) / wsum);
            continue;
        }
        for (int x = 0; x < outWidth; ++x) {
            const float *c = pic.px(std::min(x, W - 1), std::min(y, H - 1));
            float o[4] = {c[0], c[1], c[2], c[3]};
            switch (q.kind) {
            case 1: {
                float e[3];
                for (int k = 0; k < 3; ++k) e[k] = encodeSrgb(c[k]);
                if (P[3] != 1.f) for (int k = 0; k < 3; ++k) e[k] = std::pow(e[k], P[3]);
                for (int k = 0; k < 3; ++k)
                    o[k] = decodeSrgb(clamp01(P[k * 4] * e[0] + P[k * 4 + 1] * e[1] + P[k * 4 + 2] * e[2] + P[12 + k]));
                o[3] = c[3] * P[7];
                break;
            }
            case 23: {
                // Colour wheels, word for word as kind 23 in filter.comp.
                float y[3];
                for (int k = 0; k < 3; ++k) {
                    y[k] = std::max(encodeSrgb(c[k]), 0.f) * P[4 + k] + P[k];
                    y[k] = std::pow(std::max(y[k], 0.f), P[8 + k]) + P[12 + k];
                    y[k] = (y[k] - P[3]) * P[7] + P[3];
                }
                const float l = 0.2126f * y[0] + 0.7152f * y[1] + 0.0722f * y[2];
                for (int k = 0; k < 3; ++k) o[k] = decodeSrgb(clamp01(l + (y[k] - l) * P[11]));
                break;
            }
            case 2: {
                const float k = P[0];
                const Sampled n = assoc(at(x, y - 1)), s = assoc(at(x, y + 1)), w = assoc(at(x - 1, y)), e = assoc(at(x + 1, y)), m = assoc(c);
                Sampled r{m.r * (1.f + 4.f * k) - (((n.r + s.r) + w.r) + e.r) * k, m.g * (1.f + 4.f * k) - (((n.g + s.g) + w.g) + e.g) * k,
                          m.b * (1.f + 4.f * k) - (((n.b + s.b) + w.b) + e.b) * k, m.a * (1.f + 4.f * k) - (((n.a + s.a) + w.a) + e.a) * k};
                r.a = clamp01(r.a);
                r.r = std::clamp(r.r, 0.f, r.a); r.g = std::clamp(r.g, 0.f, r.a); r.b = std::clamp(r.b, 0.f, r.a);
                r = straightSample(r);
                o[0] = r.r; o[1] = r.g; o[2] = r.b; o[3] = r.a;
                break;
            }
            case 3: {
                // chromaKeyPixel: word for word as kind 3 in filter.comp.
                auto lumaOf = [](const float *v) { return 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2]; };
                const float yp = std::sqrt(std::max(lumaOf(c), 0.f));
                float sum[3] = {0, 0, 0}, wsum = 0;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i) {
                        const float *n = at(x + i * 2, y + j * 2);
                        const float dyp = (std::sqrt(std::max(lumaOf(n), 0.f)) - yp) * P[14];
                        const float w = fallTable(dyp * dyp);
                        for (int k = 0; k < 3; ++k) sum[k] += w * n[k];
                        wsum += w;
                    }
                float ea[3], e[3];
                for (int k = 0; k < 3; ++k) { ea[k] = encodeSrgb(sum[k] / wsum); e[k] = encodeSrgb(c[k]); }
                const float yy = lumaOf(e), ya = lumaOf(ea);
                const float cb = (ea[2] - ya) / 1.8556f, cr = (ea[0] - ya) / 1.5748f;
                const float d = cb * P[0] + cr * P[1], q = std::fabs(cr * P[0] - cb * P[1]);
                const float a = clamp01((P[3] - (d - P[5] * q) * P[2]) * P[4]);
                const float w = std::min((1.f - a) * 4.f, 1.f);
                const float ob = (e[2] - yy) / 1.8556f, orr = (e[0] - yy) / 1.5748f;
                const float mb = ob + (cb - ob) * w, mr = orr + (cr - orr) * w;
                float m[3] = {yy + 1.5748f * mr, 0, yy + 1.8556f * mb};
                m[1] = (yy - 0.2126f * m[0] - 0.0722f * m[2]) / 0.7152f;
                float fe[3];
                for (int k = 0; k < 3; ++k)
                    fe[k] = encodeSrgb(clamp01((decodeSrgb(clamp01(m[k])) - (1.f - a) * P[8 + k]) / std::max(a, 1e-3f)));
                const float fy = lumaOf(fe);
                float fb = (fe[2] - fy) / 1.8556f, fr = (fe[0] - fy) / 1.5748f;
                const float df = fb * P[0] + fr * P[1];
                const float nd = std::max(df, std::min(mb * P[0] + mr * P[1], 0.f));
                fb += P[0] * (nd - df);
                fr += P[1] * (nd - df);
                const float qf = std::fabs(fr * P[0] - fb * P[1]);
                const float s = std::max(nd - P[7] * a * a * qf, 0.f) * P[2] * P[6];
                float r[3] = {fy + 1.5748f * fr, 0, fy + 1.8556f * fb};
                r[1] = (fy - 0.2126f * r[0] - 0.0722f * r[2]) / 0.7152f;
                for (int k = 0; k < 3; ++k) o[k] = decodeSrgb(clamp01(r[k] - s * P[11 + k]));
                o[3] = c[3] * a;
                break;
            }
            case 4: {
                const float dr = encodeSrgb(c[0]) - P[0], dg = encodeSrgb(c[1]) - P[1], db = encodeSrgb(c[2]) - P[2];
                o[3] = c[3] * ramp(clamp01((std::sqrt(dr * dr + dg * dg + db * db) * 0.577350269f - P[3]) / P[4]));
                break;
            }
            case 5: {
                const float yy = 0.2126f * encodeSrgb(c[0]) + 0.7152f * encodeSrgb(c[1]) + 0.0722f * encodeSrgb(c[2]);
                o[3] = c[3] * clamp01((yy - P[0] + P[2]) / P[2]) * clamp01((P[1] + P[3] - yy) / P[3]);
                break;
            }
            case 6: {
                float e[3], xx[3], fr[3], l[3];
                int i[3];
                const int n = q.ix;
                for (int k = 0; k < 3; ++k) {
                    e[k] = encodeSrgb(c[k]);
                    xx[k] = clamp01(e[k] * P[k] + P[4 + k]);
                    const float pos = xx[k] * float(n - 1);
                    i[k] = std::min(floorInt(pos), n - 2);
                    fr[k] = pos - float(i[k]);
                }
                for (int k = 0; k < 3; ++k) {
                    if (q.mode == 2) {
                        const float a = lut[size_t(i[k]) * 3 + k], b = lut[size_t(i[k] + 1) * 3 + k];
                        l[k] = a + (b - a) * fr[k];
                        continue;
                    }
                    const int r = i[0], g = i[1], b = i[2];
                    const float fR = fr[0], fG = fr[1], fB = fr[2];
                    const float c000 = lutAt(r, g, b, k), c111 = lutAt(r + 1, g + 1, b + 1, k);
                    if (q.mode == 1) {
                        if (fR > fG) {
                            if (fG > fB) l[k] = (1.f - fR) * c000 + (fR - fG) * lutAt(r + 1, g, b, k) + (fG - fB) * lutAt(r + 1, g + 1, b, k) + fB * c111;
                            else if (fR > fB) l[k] = (1.f - fR) * c000 + (fR - fB) * lutAt(r + 1, g, b, k) + (fB - fG) * lutAt(r + 1, g, b + 1, k) + fG * c111;
                            else l[k] = (1.f - fB) * c000 + (fB - fR) * lutAt(r, g, b + 1, k) + (fR - fG) * lutAt(r + 1, g, b + 1, k) + fG * c111;
                        } else {
                            if (fB > fG) l[k] = (1.f - fB) * c000 + (fB - fG) * lutAt(r, g, b + 1, k) + (fG - fR) * lutAt(r, g + 1, b + 1, k) + fR * c111;
                            else if (fB > fR) l[k] = (1.f - fG) * c000 + (fG - fB) * lutAt(r, g + 1, b, k) + (fB - fR) * lutAt(r, g + 1, b + 1, k) + fR * c111;
                            else l[k] = (1.f - fG) * c000 + (fG - fR) * lutAt(r, g + 1, b, k) + (fR - fB) * lutAt(r + 1, g + 1, b, k) + fB * c111;
                        }
                    } else {
                        const float c100 = lutAt(r + 1, g, b, k), c010 = lutAt(r, g + 1, b, k), c110 = lutAt(r + 1, g + 1, b, k);
                        const float c001 = lutAt(r, g, b + 1, k), c101 = lutAt(r + 1, g, b + 1, k), c011 = lutAt(r, g + 1, b + 1, k);
                        const float x00 = c000 + (c100 - c000) * fR, x10 = c010 + (c110 - c010) * fR;
                        const float x01 = c001 + (c101 - c001) * fR, x11 = c011 + (c111 - c011) * fR;
                        const float y0 = x00 + (x10 - x00) * fG, y1 = x01 + (x11 - x01) * fG;
                        l[k] = y0 + (y1 - y0) * fB;
                    }
                }
                for (int k = 0; k < 3; ++k) o[k] = decodeSrgb(clamp01(e[k] + (l[k] - e[k]) * P[3]));
                break;
            }
            case 7: {
                const float mx = (x + 0.5f) / float(outWidth) * float(q.ix) - 0.5f, my = (y + 0.5f) / float(outHeight) * float(q.iy) - 0.5f;
                const int ix = floorInt(mx), iy = floorInt(my);
                const float fx = mx - float(ix), fy = my - float(iy);
                auto tap = [&](int tx, int ty, int k) {
                    tx = std::clamp(tx, 0, q.ix - 1); ty = std::clamp(ty, 0, q.iy - 1);
                    return f.image->rgba[(size_t(ty) * q.ix + tx) * 4 + k] / 255.f;
                };
                float v[4];
                for (int k = 0; k < 4; ++k) {
                    const float a = tap(ix, iy, k), b = tap(ix + 1, iy, k), d = tap(ix, iy + 1, k), e = tap(ix + 1, iy + 1, k);
                    const float top = a + (b - a) * fx, bottom = d + (e - d) * fx;
                    v[k] = top + (bottom - top) * fy;
                }
                const float k = P[3];
                if (q.mode == 0) o[3] = c[3] * (1.f + (v[3] - 1.f) * k);
                else if (q.mode == 1) o[3] = c[3] * (1.f + ((0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2]) - 1.f) * k);
                else {
                    const float w = v[3] * k;
                    for (int j = 0; j < 3; ++j) {
                        const float mk = decodeSrgb(v[j]) * P[j];
                        o[j] = q.mode == 2 ? c[j] + (c[j] * mk - c[j]) * w : q.mode == 3 ? c[j] + mk * w : std::max(c[j] - mk * w, 0.f);
                    }
                }
                break;
            }
            case 8: {
                const int sx = x + q.ix, sy = y + q.iy;
                if (sx >= 0 && sy >= 0 && sx < W && sy < H) std::copy_n(pic.px(sx, sy), 4, o);
                else std::fill_n(o, 4, 0.f);
                break;
            }
            case 9: {
                const float sx = float(x) - P[0], sy = float(y) - P[1];
                const int ix = floorInt(sx), iy = floorInt(sy);
                const float fx = sx - float(ix), fy = sy - float(iy);
                const Sampled a = scrollTap(ix, iy), b = scrollTap(ix + 1, iy), d = scrollTap(ix, iy + 1), e = scrollTap(ix + 1, iy + 1);
                auto mix = [&](float pa, float pb, float pd, float pe) {
                    const float top = pa + (pb - pa) * fx, bottom = pd + (pe - pd) * fx;
                    return top + (bottom - top) * fy;
                };
                const Sampled r = straightSample({mix(a.r, b.r, d.r, e.r), mix(a.g, b.g, d.g, e.g), mix(a.b, b.b, d.b, e.b), mix(a.a, b.a, d.a, e.a)});
                o[0] = r.r; o[1] = r.g; o[2] = r.b; o[3] = r.a;
                break;
            }
            case 10: {
                const bool across = q.mode == 0 || q.mode == 2;
                V4 sum = splat4(0.f);
                float wsum = 0;
                for (int k = -q.ix; k <= q.ix; ++k) {
                    const float w = gauss(k);
                    const V4 t = load4(across ? pic.px(std::clamp(x + k, 0, W - 1), y) : at(x, y + k));
                    sum += (q.mode == 0 ? assoc4(t) : t) * w;
                    wsum += w;
                }
                sum /= wsum;
                store4(o, q.mode == 1 ? straight4(sum) : sum);
                break;
            }
            case 11: {
                const int dx = int(P[1]), dy = int(P[2]);
                float sum = 0, wsum = 0;
                for (int k = -q.ix; k <= q.ix; ++k) {
                    const float w = gauss(k);
                    const int qx = q.mode == 0 ? x - dx + k : x, qy = q.mode == 0 ? y - dy : y + k;
                    const bool inside = qx >= 0 && qy >= 0 && qx < W && qy < H;
                    sum += w * (inside ? (q.mode == 0 ? pic.px(qx, qy)[3] : aux->px(qx, qy)[3]) : 0.f);
                    wsum += w;
                }
                const float s = sum / wsum;
                if (q.mode == 0) { o[0] = o[1] = o[2] = 0.f; o[3] = s; break; }
                const float sh = s * P[3], a = c[3], oa = a + sh * (1.f - a);
                for (int k = 0; k < 3; ++k) o[k] = oa > 0.f ? (c[k] * a + P[4 + k] * (sh * (1.f - a))) / oa : 0.f;
                o[3] = oa > 0.f ? oa : 0.f;
                break;
            }
            case 12: {
                // Hue kept: the brightest channel follows the curve, the others in proportion.
                const float m = std::max(std::max(c[0], c[1]), c[2]);
                const float k = m > 0.f ? toneMapValue(q.mode, P, m) / m : 0.f;
                for (int j = 0; j < 3; ++j) o[j] = std::max(c[j], 0.f) * k;
                break;
            }
            case 13:
                for (int j = 0; j < 3; ++j) o[j] = c[j] * P[0];
                break;
            case 14: {
                const float mx = (x + 0.5f) / float(outWidth) * float(q.ix) - 0.5f, my = (y + 0.5f) / float(outHeight) * float(q.iy) - 0.5f;
                const int ix = floorInt(mx), iy = floorInt(my);
                const float fx = mx - float(ix), fy = my - float(iy);
                float ab[3];
                for (int k = 0; k < 3; ++k) {
                    auto tap = [&](int tx, int ty) { return aux->px(std::clamp(tx, 0, q.ix - 1), std::clamp(ty, 0, q.iy - 1))[k]; };
                    const float a = tap(ix, iy), b = tap(ix + 1, iy), d = tap(ix, iy + 1), e = tap(ix + 1, iy + 1);
                    const float top = a + (b - a) * fx, bottom = d + (e - d) * fx;
                    ab[k] = top + (bottom - top) * fy;
                }
                const float l = 0.2126f * encodeSrgb(c[0]) + 0.7152f * encodeSrgb(c[1]) + 0.0722f * encodeSrgb(c[2]);
                const float guided = clamp01(ab[0] * l + ab[1]);
                const float v = ab[2] + (guided - ab[2]) * clamp01(P[2] * ab[2] * (1.f - ab[2]));
                o[3] = c[3] * clamp01((v - P[0]) * P[1]);
                break;
            }
            case 17: {
                const int x0 = x * W / out.width, x1 = std::max(x0 + 1, ((x + 1) * W + out.width - 1) / out.width);
                const int y0 = y * H / out.height, y1 = std::max(y0 + 1, ((y + 1) * H + out.height - 1) / out.height);
                float sum = 0, squares = 0;
                for (int j = y0; j < y1; ++j)
                    for (int i = x0; i < x1; ++i) {
                        const float *t = pic.px(std::min(i, W - 1), std::min(j, H - 1));
                        const float l = 0.2126f * encodeSrgb(t[0]) + 0.7152f * encodeSrgb(t[1]) + 0.0722f * encodeSrgb(t[2]);
                        sum += l;
                        squares += l * l;
                    }
                const float n = float((x1 - x0) * (y1 - y0)), bright = sum / n;
                const float *t = aux->px(x, y);
                const float keep = t[3] * P[0] * clamp01(1.f - std::fabs(bright - t[2]) * P[1]);
                const float mt = t[0] + (t[1] - t[0]) * keep;
                o[0] = bright; o[1] = mt; o[2] = squares / n; o[3] = bright * mt;
                break;
            }
            case 18: {
                const float a = (c[3] - c[0] * c[1]) / (c[2] - c[0] * c[0] + P[0]);
                o[0] = a; o[1] = c[1] - a * c[0]; o[2] = 0; o[3] = 1;
                break;
            }
            case 22: {
                o[0] = c[0]; o[1] = c[1]; o[2] = aux->px(x, y)[1]; o[3] = 1;
                break;
            }
            case 19: {
                // As in filter.comp: nothing of the last frame on the first.
                const float *last = aux->px(x, y);
                o[0] = c[0]; o[1] = q.mode == 1 ? last[1] : c[0]; o[2] = q.mode == 1 ? last[0] : 0.f; o[3] = q.mode == 1 ? 1.f : 0.f;
                break;
            }
            case 20: {
                Sampled sum{0, 0, 0, 0};
                for (int j = 0; j < q.ix; ++j)
                    for (int i = 0; i < q.ix; ++i) {
                        const Sampled s = assoc(pic.px(std::min(x * q.ix + i, W - 1), std::min(y * q.ix + j, H - 1)));
                        sum.r += s.r; sum.g += s.g; sum.b += s.b; sum.a += s.a;
                    }
                const float n = float(q.ix * q.ix);
                const Sampled r = straightSample({sum.r / n, sum.g / n, sum.b / n, sum.a / n});
                o[0] = r.r; o[1] = r.g; o[2] = r.b; o[3] = r.a;
                break;
            }
            case 21: {
                const float mx = (x + 0.5f) * P[0] - 0.5f, my = (y + 0.5f) * P[0] - 0.5f;
                const int ix = floorInt(mx), iy = floorInt(my);
                const float fx = mx - float(ix), fy = my - float(iy);
                float back[4];
                for (int k = 0; k < 4; ++k) {
                    auto tap = [&](int tx, int ty) { return aux->px(std::clamp(tx, 0, q.ix - 1), std::clamp(ty, 0, q.iy - 1))[k]; };
                    const float a = tap(ix, iy), b = tap(ix + 1, iy), d = tap(ix, iy + 1), e = tap(ix + 1, iy + 1);
                    const float top = a + (b - a) * fx, bottom = d + (e - d) * fx;
                    back[k] = top + (bottom - top) * fy;
                }
                const float oa = c[3] + back[3] * (1.f - c[3]);
                for (int k = 0; k < 3; ++k) o[k] = oa > 0.f ? (c[k] * c[3] + back[k] * back[3] * (1.f - c[3])) / oa : 0.f;
                o[3] = oa > 0.f ? oa : 0.f;
                break;
            }
            case 15: {
                const int n = int(P[0]);
                float light[3] = {0, 0, 0}, open = 0;
                for (int j = 0; j < n; ++j)
                    for (int i = 0; i < n; ++i) {
                        const int tx = x * n + i, ty = y * n + j;
                        const float u = 1.f - aux->px(std::min(tx, aux->width - 1), std::min(ty, aux->height - 1))[3];
                        const float *t = pic.px(std::clamp(tx + q.ix, 0, W - 1), std::clamp(ty + q.iy, 0, H - 1));
                        for (int k = 0; k < 3; ++k) light[k] += t[k] * t[3] * u;
                        open += u;
                    }
                for (int k = 0; k < 3; ++k) o[k] = open > 0.f ? light[k] / open : 0.f;
                o[3] = open / float(n * n);
                break;
            }
            case 16: {
                const float mx = (x + 0.5f) * P[1] - 0.5f, my = (y + 0.5f) * P[1] - 0.5f;
                const int ix = floorInt(mx), iy = floorInt(my);
                const float fx = mx - float(ix), fy = my - float(iy);
                float v[4];
                for (int k = 0; k < 4; ++k) {
                    auto tap = [&](int tx, int ty) { return aux->px(std::clamp(tx, 0, q.ix - 1), std::clamp(ty, 0, q.iy - 1))[k]; };
                    const float a = tap(ix, iy), b = tap(ix + 1, iy), d = tap(ix, iy + 1), e = tap(ix + 1, iy + 1);
                    const float top = a + (b - a) * fx, bottom = d + (e - d) * fx;
                    v[k] = top + (bottom - top) * fy;
                }
                const float edge = std::min(v[3] * 2.f, 1.f);
                for (int k = 0; k < 3; ++k)
                    o[k] = q.mode == 1 ? c[k] + (v[k] - c[k]) * clamp01(edge * P[0]) : c[k] + v[k] * edge * P[0];
                break;
            }
            default: break;
            }
            store4(written + size_t(x) * 4, load4(o));
        }
    }
}

void CpuEngine::renderSourceFilters(int id, double seconds) {
    const auto it = m_sources.find(id);
    if (it == m_sources.end()) return;
    CpuSource &s = it->second;
    try {
        renderChain(id, s, seconds);
    } catch (const std::bad_alloc &) {
        // No memory for its pictures: as on the GPU, the source goes unfiltered
        // rather than half drawn, and what the chain held is given back. A chain
        // that is there to hide it hides it still (chainFailsClosed), and says so.
        s.filtered = false;
        if (chainFailsClosed(s.filters)) { s.failure = FilterFailure::Memory; s.retryAt = seconds + 1; }
        for (auto &st : s.steps) { const bool started = st.started; const double origin = st.origin; st = {}; st.started = started; st.origin = origin; }
    }
}

void CpuEngine::renderChain(int id, CpuSource &s, double seconds) {
    if (s.filters.empty() || s.base().width == 0) { s.filtered = false; return; }
    if (s.failure != FilterFailure::None && seconds < s.retryAt) return;
    bool timed = false;
    for (const VideoFilter &f : s.filters) timed |= filterIsTimed(f);
    for (const auto &st : s.steps) timed |= st.part > 0;  // a network run under way goes on
    if (s.filtered && !timed && s.filteredGeneration == s.generation) return;
    const FrameF *cur = &s.base();
    bool fresh = s.generation != s.filteredGeneration || !s.filtered;  // the step's input is new
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < s.filters.size(); ++i) {
        const VideoFilter &f = s.filters[i];
        auto &st = s.steps[i];
        if (!st.started) { st.started = true; st.origin = seconds; }
        if (filterIsDelay(f)) {
            const double due = seconds - f.delayMs / 1000.0;
            // A picture past its time is kept to copy the next into, as the GPU's
            // `spare` is: a new one each frame is a picture's worth of memory asked
            // for and given back every frame.
            auto retire = [&] { st.spare.push_back(std::move(st.held.front().first)); st.held.pop_front(); };
            // Its pictures are 16 bytes a pixel, twice the GPU's.
            const size_t most = filterDelayCapacity(cur->width, cur->height, sizeof(float) * 4);
            if (f.type == VideoFilterType::RenderDelay || fresh || st.held.empty()) {
                while (st.held.size() >= 2 && st.held[1].second <= due) retire();
                while (st.held.size() >= most) retire();
                FrameF img;
                if (!st.spare.empty()) { img = std::move(st.spare.back()); st.spare.pop_back(); }
                img.rgba = cur->rgba;  // into the memory it has, when that is the right size
                img.width = cur->width;
                img.height = cur->height;
                st.held.emplace_back(std::move(img), seconds);
            }
            while (st.held.size() >= 2 && st.held[1].second <= due) retire();
            // A couple kept, and never past what the delay may hold in all.
            while (!st.spare.empty() && (st.spare.size() > 2 || st.held.size() + st.spare.size() > most)) st.spare.pop_back();
            cur = &st.held.front().first;
            fresh = true;
            continue;
        }
        int ow, oh;
        filterSize(f, cur->width, cur->height, &ow, &oh);
        if (st.out.width != ow || st.out.height != oh) st.out.resize(ow, oh);
        if (f.type == VideoFilterType::Scale) {
            m_pool.run(0, oh, ow, [&](int from, int to) { std::fill_n(st.out.px(0, from), size_t(to - from) * ow * 4, 0.f); });
            drawLayer(st.out, *cur, scaleLayer(f, cur->width, cur->height, ow, oh));
        } else if (f.type == VideoFilterType::Blur || f.type == VideoFilterType::Shadow) {
            const bool blur = f.type == VideoFilterType::Blur;
            if (st.mid.width != cur->width || st.mid.height != cur->height) st.mid.resize(cur->width, cur->height);
            runStep(*cur, st.mid, f, filterParams(f, cur->width, cur->height, 0, 0));
            runStep(blur ? st.mid : *cur, st.out, f, filterParams(f, cur->width, cur->height, 0, 1), blur ? nullptr : &st.mid);
        } else if (f.type == VideoFilterType::RemoveBackground) {
            // The network runs on new pictures only.
            const FilterParams q = filterParams(f, cur->width, cur->height, 0);
            const matting::Model &m = matting::model();
            // As recordMatte in vk_engine.cpp: the first matte whole, then a part a frame.
            const bool samePlan = st.plan.ok && st.plan.width == q.ix && st.plan.height == q.iy && st.plan.ends.size() == size_t(std::clamp(f.spread, 1, 8)) &&
                                  st.plan.passes.front().inW == cur->width && st.plan.passes.front().inH == cur->height;
            if (!samePlan) { st.plan = matting::plan(m, q.ix, q.iy, cur->width, cur->height, f.spread); st.part = 0; st.mid = {}; st.history = false; }
            if (!m.ok || !st.plan.ok) {
                s.filtered = false;
                s.failure = m.ok ? FilterFailure::Device : FilterFailure::Model;
                s.retryAt = seconds + 1;
                return;
            }
            size_t first = 0, last = st.plan.passes.size();
            bool run = true;
            if (st.mid.width != q.ix || st.mid.height != q.iy) st.part = 0;
            else if (st.part > 0) { first = st.plan.ends[st.part - 1]; last = st.plan.ends[st.part]; st.part = st.part + 1 < st.plan.ends.size() ? st.part + 1 : 0; }
            else if (fresh) { last = st.plan.ends[0]; st.part = st.plan.ends.size() > 1 ? 1 : 0; }
            else run = false;
            if (run) {
                matting::run(st.plan, m, *cur, st.arena, first, last, [this](int from, int to, int perRow, const std::function<void(int, int)> &body) { m_pool.run(from, to, perRow, body); });
                if (last == st.plan.passes.size()) {
                    if (st.mid.width != q.ix || st.mid.height != q.iy) st.mid.resize(q.ix, q.iy);
                    for (int i = 0; i < q.ix * q.iy; ++i) std::fill_n(&st.mid.rgba[size_t(i) * 4], 4, st.arena[size_t(st.plan.matte) + i]);
                }
            }
            // As recordGuided in vk_engine.cpp.
            const int w = cur->width, h = cur->height;
            for (FrameF &img : st.guide)
                if (img.width != q.ix || img.height != q.iy) img.resize(q.ix, q.iy);
            FrameF &history = st.guide[0], &previous = st.guide[1 + st.flip], &now = st.guide[2 - st.flip], &spare = st.guide[3],
                   &blurred = st.guide[4], &fit = st.guide[5];
            auto pass = [&](MattePass p) { return matteParams(f, p, w, h, st.history); };
            runStep(st.mid, history, f, pass(MattePass::History), &previous);
            runStep(*cur, now, f, pass(MattePass::Stats), &history);
            runStep(now, spare, f, pass(MattePass::Across));
            runStep(spare, blurred, f, pass(MattePass::Down));
            runStep(blurred, fit, f, pass(MattePass::Coefficients));
            runStep(fit, spare, f, pass(MattePass::Across));
            runStep(spare, blurred, f, pass(MattePass::Down));
            runStep(blurred, fit, f, pass(MattePass::Pack), &now);
            if (f.blurBackground) {
                if (st.cut.width != w || st.cut.height != h) st.cut.resize(w, h);
                for (FrameF &img : st.back)
                    if (img.width != (w + 3) / 4 || img.height != (h + 3) / 4) img.resize((w + 3) / 4, (h + 3) / 4);
                runStep(*cur, st.cut, f, q, &fit);
                runStep(*cur, st.back[0], f, pass(MattePass::BackDown));
                runStep(st.back[0], st.back[1], f, pass(MattePass::BackAcross));
                runStep(st.back[1], st.back[0], f, pass(MattePass::BackDownBlur));
                runStep(st.cut, st.out, f, pass(MattePass::BackOver), &st.back[0]);
            } else {
                runStep(*cur, st.out, f, q, &fit);
            }
            st.flip ^= 1;
            st.history = true;
        } else {
            runStep(*cur, st.out, f, filterParams(f, cur->width, cur->height, seconds - st.origin, 0, m_levels.sdrWhite));
        }
        cur = &st.out;
        fresh |= filterIsTimed(f);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    const auto known = m_filterMs.find(id);
    if (known == m_filterMs.end()) m_filterMs[id] = ms;
    else known->second += (ms - known->second) * 0.1;
    s.filtered = true;
    s.filteredGeneration = s.generation;
    s.failure = FilterFailure::None;
}

}  // namespace

std::unique_ptr<Engine> createCpuEngine(const std::string &note) { return std::make_unique<CpuEngine>(note); }

std::unique_ptr<Engine> Engine::create(Mode mode, int deviceIndex) {
    if (mode != Mode::Cpu) {
        std::string why;
        if (auto gpu = createVulkanEngine(deviceIndex, &why))
            return gpu;
        if (mode == Mode::Vulkan)
            return nullptr;
        return createCpuEngine(why);
    }
    return createCpuEngine();
}

}  // namespace engine
