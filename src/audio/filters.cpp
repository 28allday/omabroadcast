#include "filters.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
double amplitude(double db) { return std::pow(10.0, db / 20.0); }
double decibels(double value) { return 20.0 * std::log10(std::max(1e-12, value)); }
double smooth(double current, double target, double coefficient) {
    const double value = target + coefficient * (current - target);
    return std::abs(value - target) < 1e-12 ? target : value;
}
float sample(double value) {
    // Leave 120 dB of internal headroom, but keep corrupt input and extreme
    // stacked gains from poisoning feedback/filter history with infinities.
    if (!std::isfinite(value) || std::abs(value) < 1e-20) return 0.f;
    return float(std::clamp(value, -1e6, 1e6));
}
}

const std::vector<AudioFilters::Definition> &AudioFilters::definitions() {
    static const std::vector<Definition> types{
        {Gain, "gain", "Gain", "Adjust level before the channel fader.", {{"db", "Gain", "dB", 0, -60, 30, 0.1}}},
        {Gate, "gate", "Noise gate", "Open above the opening threshold; close below the closing threshold after the hold time.",
         {{"openDb", "Open above", "dB", -30, -96, 0, 0.1}, {"closeDb", "Close below", "dB", -36, -96, 0, 0.1},
          {"attackMs", "Attack", "ms", 5, 0, 1000, 1}, {"holdMs", "Hold", "ms", 100, 0, 2000, 1}, {"releaseMs", "Release", "ms", 100, 0, 2000, 1}}},
        {Compressor, "compressor", "Compressor", "Reduce levels above the threshold. Channels share one detector to preserve the stereo image.",
         {{"thresholdDb", "Threshold", "dB", -18, -60, 0, 0.1}, {"ratio", "Ratio", ":1", 4, 1, 20, 0.1},
          {"attackMs", "Attack", "ms", 10, 0, 1000, 1}, {"releaseMs", "Release", "ms", 100, 0, 2000, 1}, {"makeupDb", "Makeup", "dB", 0, -30, 30, 0.1}}},
        {Limiter, "limiter", "Limiter", "Instant sample-peak limiting with a controlled release. Later gain changes or summed channels can exceed this ceiling.",
         {{"ceilingDb", "Ceiling", "dB", -1, -60, 0, 0.1}, {"releaseMs", "Release", "ms", 60, 0, 2000, 1}}},
        {Expander, "expander", "Expander", "Reduce quiet signals below the threshold, with a maximum attenuation.",
         {{"thresholdDb", "Threshold", "dB", -40, -96, 0, 0.1}, {"ratio", "Ratio", ":1", 2, 1, 20, 0.1},
          {"attackMs", "Attack", "ms", 5, 0, 1000, 1}, {"releaseMs", "Release", "ms", 100, 0, 2000, 1}, {"rangeDb", "Maximum reduction", "dB", 60, 0, 96, 0.1}}},
        {Upward, "upward", "Upward compressor", "Raise signals below the threshold by at most the maximum boost. Digital silence remains silent.",
         {{"thresholdDb", "Threshold", "dB", -24, -96, 0, 0.1}, {"ratio", "Ratio", ":1", 2, 1, 20, 0.1},
          {"attackMs", "Attack", "ms", 5, 0, 1000, 1}, {"releaseMs", "Release", "ms", 100, 0, 2000, 1}, {"boostDb", "Maximum boost", "dB", 12, 0, 30, 0.1}}},
        {Invert, "invert", "Invert polarity", "Reverse the polarity of every channel.", {}},
        {Equalizer, "eq", "3-band EQ", "Low shelf at 120 Hz, mid bell at 1 kHz (Q 0.7), high shelf at 6 kHz.",
         {{"lowDb", "Low", "dB", 0, -24, 24, 0.1}, {"midDb", "Mid", "dB", 0, -24, 24, 0.1}, {"highDb", "High", "dB", 0, -24, 24, 0.1}}},
        {Deesser, "deesser", "De-esser", "A high-frequency detector reduces the whole signal when sibilance exceeds the threshold.",
         {{"frequency", "Detector frequency", "Hz", 6000, 1000, 16000, 100}, {"thresholdDb", "Threshold", "dB", -24, -60, 0, 0.1},
          {"ratio", "Ratio", ":1", 4, 1, 20, 0.1}, {"attackMs", "Attack", "ms", 1, 0, 1000, 1},
          {"releaseMs", "Release", "ms", 60, 0, 2000, 1}, {"rangeDb", "Maximum reduction", "dB", 12, 0, 30, 0.1}}},
        {Delay, "delay", "Delay", "Delay audio by the chosen time. Dry/wet blends the original and delayed signal; feedback repeats the delay.",
         {{"timeMs", "Delay", "ms", 100, 0, 2000, 1}, {"wet", "Wet", "%", 100, 0, 100, 1}, {"feedback", "Feedback", "%", 0, 0, 90, 1}}},
        {Spectral, "spectral", "Spectral noise suppression", "Reduce quiet frequency bands. Threshold is the level of each frequency bin. Channels share a mask. Adds 21.3 ms at 48 kHz; use a negative sync offset to compensate if needed.",
         {{"thresholdDb", "Threshold", "dB", -50, -96, 0, 0.1}, {"reductionDb", "Reduction", "dB", 20, 0, 60, 0.1},
          {"attackMs", "Attack", "ms", 5, 0, 500, 1}, {"releaseMs", "Release", "ms", 100, 0, 2000, 1}}},
        {Rnnoise, "rnnoise", "RNNoise speech suppression", "Suppress background noise around speech. Intended for voice, not music. Uses 10 ms blocks at 48 kHz and adds processing delay; use a negative sync offset to compensate if needed.", {}}
    };
    return types;
}

AudioFilters::Config AudioFilters::defaults(Type type, const std::string &id) {
    Config config; config.type = type; config.id = id;
    for (const auto &definition : definitions()) if (definition.type == type)
        for (size_t i = 0; i < definition.parameters.size(); ++i) config.values[i] = definition.parameters[i].initial;
    return config;
}

AudioFilters::AudioFilters(int rate, int channels) : m_rate(std::max(1, rate)), m_channels(std::max(1, channels)) {}

double AudioFilters::Biquad::process(double value, int channel) {
    auto &m = memory[size_t(channel)];
    const double result = b0 * value + m.z1;
    m.z1 = b1 * value - a1 * result + m.z2;
    m.z2 = b2 * value - a2 * result;
    if (std::abs(m.z1) < 1e-20) m.z1 = 0;
    if (std::abs(m.z2) < 1e-20) m.z2 = 0;
    return result;
}

void AudioFilters::configure(const std::vector<Config> &configs) {
    bool same = configs.size() == m_filters.size();
    for (size_t i = 0; same && i < configs.size(); ++i) same = configs[i] == m_filters[i].config;
    if (same) return;
    std::vector<Filter> next;
    for (auto config : configs) {
        if (next.size() == 16) break;
        const auto def = std::find_if(definitions().begin(), definitions().end(), [&](const auto &d) { return d.type == config.type; });
        if (def == definitions().end()) continue;
        for (size_t i = 0; i < def->parameters.size(); ++i) {
            const auto &p = def->parameters[i];
            config.values[i] = std::isfinite(config.values[i]) ? std::clamp(config.values[i], p.minimum, p.maximum) : p.initial;
        }
        if (config.type == Gate) config.values[1] = std::min(config.values[0], config.values[1]);
        auto existing = std::find_if(m_filters.begin(), m_filters.end(), [&](const auto &f) {
            return f.config.id == config.id && f.config.type == config.type && f.config.enabled == config.enabled;
        });
        const bool fresh = existing == m_filters.end();
        Filter filter = fresh ? Filter{} : std::move(*existing);
        if (!fresh) m_filters.erase(existing);
        filter.config = config;
        prepare(filter, fresh);
        next.push_back(std::move(filter));
    }
    m_filters = std::move(next);
}

void AudioFilters::reset() {
    for (auto &filter : m_filters) prepare(filter, true);
}

void AudioFilters::prepare(Filter &f, bool fresh) {
    const auto &v = f.config.values;
    if (fresh) { f.envelope = 0; f.gain = f.config.type == Gate ? 0 : 1; f.hold = 0; f.open = false; f.delayAt = 0; }
    const auto coefficient = [&](double ms) { return ms <= 0 ? 0.0 : std::exp(-1000.0 / (ms * m_rate)); };
    int attackIndex = 2, releaseIndex = 3;
    if (f.config.type == Gate) releaseIndex = 4;
    if (f.config.type == Deesser) { attackIndex = 3; releaseIndex = 4; }
    f.attack = coefficient(v[attackIndex]); f.release = coefficient(v[releaseIndex]);
    if (f.config.type == Limiter) { f.attack = 0; f.release = coefficient(v[1]); }
    if (f.config.type == Delay) {
        const size_t size = size_t(std::llround(v[0] * m_rate / 1000.0)) * m_channels;
        if (fresh || size != f.delay.size()) { f.delay.assign(size, 0.f); f.delayAt = 0; }
    }
    if (f.config.type == Spectral || f.config.type == Rnnoise) {
        if (fresh || !f.suppression)
            f.suppression = std::make_unique<NoiseSuppression>(f.config.type == Spectral ? NoiseSuppression::Spectral : NoiseSuppression::Speech, m_rate, m_channels);
        f.suppression->configure(v[0], v[1], v[2], v[3]);
    }
    if (f.config.type != Equalizer && f.config.type != Deesser) return;
    // Biquad transfer functions from the public W3C Audio EQ Cookbook:
    // https://www.w3.org/TR/audio-eq-cookbook/ . Shelf slope S=1.
    for (int band = 0; band < (f.config.type == Equalizer ? 3 : 1); ++band) {
        auto &eq = f.eq[size_t(band)];
        if (fresh) eq.memory.assign(size_t(m_channels), {});
        const double freq = f.config.type == Deesser ? v[0] : band == 0 ? 120 : band == 1 ? 1000 : 6000;
        const double w = 2 * std::numbers::pi * std::clamp(freq / m_rate, 0.00001, 0.45);
        const double co = std::cos(w), si = std::sin(w);
        double b0, b1, b2, a0, a1, a2;
        if (f.config.type == Deesser) {
            const double alpha = si / std::sqrt(2.0);
            b0 = (1 + co) / 2; b1 = -(1 + co); b2 = b0;
            a0 = 1 + alpha; a1 = -2 * co; a2 = 1 - alpha;
        } else {
            const double a = std::pow(10.0, v[size_t(band)] / 40.0);
            if (band == 1) {
                const double alpha = si / 1.4;
                b0 = 1 + alpha * a; b1 = -2 * co; b2 = 1 - alpha * a;
                a0 = 1 + alpha / a; a1 = -2 * co; a2 = 1 - alpha / a;
            } else {
                const double term = si * std::sqrt(2 * a);
                const double sign = band == 0 ? 1 : -1;
                b0 = a * ((a + 1) - sign * (a - 1) * co + term);
                b1 = 2 * sign * a * ((a - 1) - sign * (a + 1) * co);
                b2 = a * ((a + 1) - sign * (a - 1) * co - term);
                a0 = (a + 1) + sign * (a - 1) * co + term;
                a1 = -2 * sign * ((a - 1) + sign * (a + 1) * co);
                a2 = (a + 1) + sign * (a - 1) * co - term;
            }
        }
        eq.b0 = b0 / a0; eq.b1 = b1 / a0; eq.b2 = b2 / a0; eq.a1 = a1 / a0; eq.a2 = a2 / a0;
    }
}

void AudioFilters::process(float *frame) {
    if (m_filters.empty()) return;
    for (int c = 0; c < m_channels; ++c) frame[c] = sample(frame[c]);
    for (auto &f : m_filters) {
        if (!f.config.enabled) continue;
        const auto &v = f.config.values;
        const auto type = f.config.type;
        if (f.suppression) { f.suppression->process(frame); continue; }
        if (type == Delay) {
            if (f.delay.empty()) continue;
            for (int c = 0; c < m_channels; ++c) {
                float &delayed = f.delay[f.delayAt];
                const float input = frame[c];
                frame[c] = sample(input * (1 - v[1] / 100) + delayed * v[1] / 100);
                delayed = sample(input + delayed * v[2] / 100);
                f.delayAt = (f.delayAt + 1) % f.delay.size();
            }
            continue;
        }
        if (type == Equalizer) {
            for (int c = 0; c < m_channels; ++c) {
                double sample = frame[c];
                for (auto &eq : f.eq) sample = eq.process(sample, c);
                frame[c] = ::sample(sample);
            }
            continue;
        }
        double gain = type == Gain ? amplitude(v[0]) : type == Invert ? -1 : 1;
        if (type != Gain && type != Invert) {
            double peak = 0;
            for (int c = 0; c < m_channels; ++c) {
                const double sample = type == Deesser ? f.eq[0].process(frame[c], c) : frame[c];
                peak = std::max(peak, std::abs(sample));
            }
            // A fast peak hold detector keeps dynamics stable across waveform
            // zero crossings; the user controls the applied gain envelope.
            f.envelope = std::max(peak, f.envelope * std::exp(-1000.0 / (10 * m_rate)));
            if (f.envelope < 1e-12) f.envelope = 0;
            const double db = decibels(f.envelope);
            double target = 1;
            if (type == Gate) {
                if (db >= v[0]) f.open = true;
                if (f.open) {
                    if (db >= v[1]) f.hold = int(v[3] * m_rate / 1000);
                    else if (f.hold > 0) --f.hold;
                    else f.open = false;
                }
                target = f.open ? 1 : 0;
                f.gain = smooth(f.gain, target, target > f.gain ? f.attack : f.release);
            } else if (type == Limiter) {
                target = peak > 0 ? std::min(1.0, amplitude(v[0]) / peak) : 1;
                f.gain = target < f.gain ? target : smooth(f.gain, target, f.release);
            } else {
                double change = 0;
                if (type == Compressor) change = -std::max(0.0, db - v[0]) * (1 - 1 / v[1]);
                if (type == Expander) change = -std::min(v[4], std::max(0.0, v[0] - db) * (v[1] - 1));
                if (type == Upward) change = std::min(v[4], std::max(0.0, v[0] - db) * (1 - 1 / v[1]));
                if (type == Deesser) change = -std::min(v[5], std::max(0.0, db - v[1]) * (1 - 1 / v[2]));
                target = amplitude(change);
                const bool attacking = type == Expander || type == Upward ? target > f.gain : target < f.gain;
                f.gain = smooth(f.gain, target, attacking ? f.attack : f.release);
            }
            gain = f.gain * (type == Compressor ? amplitude(v[4]) : 1);
        }
        for (int c = 0; c < m_channels; ++c) frame[c] = sample(frame[c] * gain);
    }
    for (int c = 0; c < m_channels; ++c) if (!std::isfinite(frame[c])) frame[c] = 0;
}
