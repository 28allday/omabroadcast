#pragma once
#include <array>
#include <cstdint>
#include <vector>

// Per-channel post-fader peaks. The interpolator is for measurement only;
// it adds no delay to the audio sent to outputs. Caller provides synchronization.
class PeakMeter {
public:
    struct Reading { std::vector<float> levels; float maximum = 0; };
    explicit PeakMeter(int channels = 2);
    void setTruePeak(bool enabled);
    void reset();
    void resetMaximum();
    void process(const float *frame, int64_t nowNs);
    Reading reading(int64_t nowNs) const;
    static double decay(double previousDb, double measuredDb, double seconds, double dbPerSecond);
private:
    static constexpr int taps = 64, phases = 8;
    struct Channel {
        std::array<float, taps * 2> history{};
        float peak = 0, maximum = 0;
        int64_t at = 0;
    };
    static const std::array<std::array<float, taps>, phases-1> &coefficients();
    std::vector<Channel> m_channels;
    int m_write = 0;
    bool m_truePeak = false;
};
