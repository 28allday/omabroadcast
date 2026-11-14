#include "peak_meter.h"
#include <algorithm>
#include <cmath>
#include <numbers>

PeakMeter::PeakMeter(int channels) : m_channels(size_t(std::max(1, channels))) {}

const std::array<std::array<float, PeakMeter::taps>, PeakMeter::phases-1> &PeakMeter::coefficients() {
    // Own 8x polyphase windowed-sinc reconstruction, unity at DC. Eight phases
    // reduce sampling-grid under-read compared with the minimum 4x approach
    // described in ITU-R BS.1770 Annex 2. See EBU Tech 3341 signals 15–23.
    static const auto table = [] {
        std::array<std::array<float, taps>, phases-1> result{};
        for (int phase = 1; phase < phases; ++phase) {
            double sum = 0;
            for (int k = 0; k < taps; ++k) {
                const double x = k - (taps/2 - 1 + double(phase)/phases);
                const double sinc = std::abs(x) < 1e-12 ? 1 : std::sin(std::numbers::pi*x)/(std::numbers::pi*x);
                const double w = 0.42 + 0.5*std::cos(std::numbers::pi*x/(taps/2)) + 0.08*std::cos(2*std::numbers::pi*x/(taps/2));
                result[phase-1][k] = float(sinc*w);
                sum += sinc*w;
            }
            for (auto &value : result[phase-1]) value = float(value/sum);
        }
        return result;
    }();
    return table;
}

void PeakMeter::setTruePeak(bool enabled) {
    if (m_truePeak == enabled) return;
    if (enabled) (void)coefficients(); // initialize outside the processing loop
    m_truePeak = enabled;
    reset();
}
void PeakMeter::reset() { for (auto &channel : m_channels) channel = {}; m_write = 0; }
void PeakMeter::resetMaximum() { for (auto &channel : m_channels) channel.maximum = 0; }

void PeakMeter::process(const float *frame, int64_t nowNs) {
    for (size_t c = 0; c < m_channels.size(); ++c) {
        auto &channel = m_channels[c];
        const float value = std::isfinite(frame[c]) ? frame[c] : 0.f;
        float peak = std::abs(value);
        if (m_truePeak) {
            channel.history[m_write] = channel.history[m_write + taps] = value;
            // The mirrored ring presents a contiguous oldest-to-newest window.
            const float *history = channel.history.data() + ((m_write+1) % taps);
            for (const auto &phase : coefficients()) {
                double reconstructed = 0;
                for (int k = 0; k < taps; ++k) reconstructed += history[k] * double(phase[k]);
                peak = std::max(peak, float(std::abs(reconstructed)));
            }
        }
        channel.maximum = std::max(channel.maximum, peak);
        // Catch short transients between UI polls; presentation decay is timed
        // in dB/s separately and does not change this measured peak.
        if (peak >= channel.peak || nowNs - channel.at >= 100'000'000) {
            channel.peak = peak;
            channel.at = nowNs;
        }
    }
    m_write = (m_write + 1) % taps;
}

PeakMeter::Reading PeakMeter::reading(int64_t nowNs) const {
    Reading result;
    result.levels.reserve(m_channels.size());
    for (const auto &channel : m_channels) {
        result.levels.push_back(nowNs - channel.at < 150'000'000 ? channel.peak : 0.f);
        result.maximum = std::max(result.maximum, channel.maximum);
    }
    return result;
}

double PeakMeter::decay(double previousDb, double measuredDb, double seconds, double dbPerSecond) {
    return std::max(measuredDb, std::max(-60.0, previousDb - std::max(0.0, seconds) * dbPerSecond));
}
