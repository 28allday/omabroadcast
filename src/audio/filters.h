#pragma once

#include "noise_suppression.h"
#include <array>
#include <string>
#include <vector>

// Ordered DSP on the live audio clock. Library-owned state is never cloned.
class AudioFilters {
public:
    enum Type { Gain, Gate, Compressor, Limiter, Expander, Upward, Invert, Equalizer, Deesser, Delay, Spectral, Rnnoise };
    struct Parameter { const char *key, *label, *unit; double initial, minimum, maximum, step; };
    struct Definition { Type type; const char *key, *label, *description; std::vector<Parameter> parameters; };
    struct Config {
        std::string id;
        Type type = Gain;
        bool enabled = true;
        std::array<double, 8> values{};
        bool operator==(const Config &) const = default;
    };
    static const std::vector<Definition> &definitions();
    static Config defaults(Type type, const std::string &id = {});
    explicit AudioFilters(int rate = 48000, int channels = 2);
    AudioFilters(const AudioFilters &) = delete;
    AudioFilters &operator=(const AudioFilters &) = delete;
    AudioFilters(AudioFilters &&) = default;
    AudioFilters &operator=(AudioFilters &&) = default;
    void configure(const std::vector<Config> &configs);
    void reset();
    void process(float *frame); // one interleaved frame, no allocation
private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        struct Memory { double z1 = 0, z2 = 0; };
        std::vector<Memory> memory;
        double process(double value, int channel);
    };
    struct Filter {
        Config config;
        std::unique_ptr<NoiseSuppression> suppression;
        std::array<Biquad, 3> eq;
        std::vector<float> delay;
        size_t delayAt = 0;
        double envelope = 0, gain = 1, attack = 0, release = 0;
        int hold = 0;
        bool open = false;
    };
    void prepare(Filter &filter, bool fresh);
    int m_rate, m_channels;
    std::vector<Filter> m_filters;
};
