#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/frame.h"

// A decoded Y'CbCr or RGBA picture its consumer owns: what capture devices and media
// files hand the frame loop, ready for Engine::updateSource.
struct Picture {
    engine::YuvLayout layout = engine::YuvLayout::NV12;
    engine::YuvMatrix matrix = engine::YuvMatrix::BT709;
    bool fullRange = false;
    // Y'CbCr above 8 bits: little-endian words of `depth` bits, high-aligned with msb (P010).
    int depth = 8;
    bool msb = false;
    engine::Transfer transfer = engine::Transfer::Sdr;
    bool wideGamut = false;  // BT.2020 primaries
    int width = 0, height = 0;
    std::vector<uint8_t> plane[3];
    int stride[3] = {0, 0, 0};
    bool rgba = false; // packed RGBA in plane[0]; otherwise Y'CbCr planes
    std::vector<uint8_t> alpha;
    int alphaStride = 0;
    bool premultiplied = false;
    int64_t timeNs = 0;     // when it is due (CLOCK_MONOTONIC)
    int64_t durationNs = 0;
    int64_t mediaTimeNs = 0; // file position, independent of presentation time
    bool interlaced = false;
    bool discontinuity = false;
    int firstField = -1;    // -1 unknown, 0 top, 1 bottom

    // A source's colour settings, when the stream labels itself wrongly:
    // range "limited"/"full", matrix "bt601"/"bt709"/"bt2020"; "auto" (or
    // anything else) keeps what the stream said.
    void overrideColour(const std::string &range, const std::string &matrixName) {
        if (range == "limited") fullRange = false;
        else if (range == "full") fullRange = true;
        if (matrixName == "bt601") matrix = engine::YuvMatrix::BT601;
        else if (matrixName == "bt709") matrix = engine::YuvMatrix::BT709;
        else if (matrixName == "bt2020") matrix = engine::YuvMatrix::BT2020;
    }

    engine::YuvInput input(bool linearAlpha = true) const {
        engine::YuvInput in;
        in.width = width;
        in.height = height;
        in.layout = layout;
        in.matrix = matrix;
        in.fullRange = fullRange;
        in.interlaced = interlaced;
        in.alpha = alpha.empty() ? nullptr : alpha.data();
        in.alphaStride = alphaStride;
        in.premultiplied = premultiplied;
        in.linearAlpha = linearAlpha;
        in.depth = depth;
        in.msb = msb;
        in.transfer = transfer;
        in.wideGamut = wideGamut;
        for (int i = 0; i < 3; ++i) {
            in.plane[i] = plane[i].empty() ? nullptr : plane[i].data();
            in.stride[i] = stride[i];
        }
        return in;
    }

    engine::CpuFrame rgbInput(bool linearAlpha = true) const {
        return {width, height, stride[0], engine::PixelFormat::RGBA, plane[0].data(), premultiplied, !fullRange, linearAlpha};
    }
};

// Many cameras label raw Y'CbCr as limited range and send full range. A
// limited signal's white is 235; superwhite above it is a sliver, never a
// plateau. Clipped highlights (or blacks) piled up at the very ends of the
// code range mean full range; once seen it stays decided until the format
// changes.
class RangeDetector {
public:
    void reset() { m_samples = m_outside = 0; m_full = false; }
    bool full() const { return m_full; }
    void add(const Picture &p) {
        if (m_full || p.rgba || p.plane[0].empty() || p.depth > 8) return;
        const int step = p.layout == engine::YuvLayout::YUY2 ? 2 : 1;  // Y is every other byte
        for (int y = 0; y < p.height; y += 8) {
            const uint8_t *row = p.plane[0].data() + size_t(y) * p.stride[0];
            for (int x = 0; x < p.width; x += 8) {
                const uint8_t v = row[x * step];
                ++m_samples;
                if (v >= 250 || v <= 2) ++m_outside;
            }
        }
        if (m_samples >= 20000 && m_outside * 100 > m_samples) m_full = true;  // over 1%
    }

private:
    uint64_t m_samples = 0, m_outside = 0;
    bool m_full = false;
};
