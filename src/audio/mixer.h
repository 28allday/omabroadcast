#pragma once

#include "filters.h"
#include "peak_meter.h"
#include "push_control.h"
#include <cstdint>
#include <array>
#include <map>
#include <mutex>
#include <vector>

// Timestamped source buffers on a shared timeline. Gain and mute are applied
// when pulling, so changes affect queued audio too. Missing input is silence.
// An application can have several inputs under one channel's fader/meter.
// Each input owns its ring: overlapping timestamps replace that input's
// samples, while distinct inputs are summed. All access is thread safe;
// channel and input ids are never reused after removal.
class AudioMixer {
public:
    AudioMixer(int sampleRate = 48000, int channels = 2, int latencyMs = 80, int ringSeconds = 4);
    int sampleRate() const { return m_rate; }
    int channels() const { return m_channels; }
    bool setFormat(int sampleRate, int channels); // only while stopped; preserves controls and ids
    void setSourceCount(int n);
    void setLatencyMs(int latencyMs);
    int addSource(bool defaultInput = true);
    int addInput(int source);
    void removeInput(int source, int input);
    void removeSource(int source);
    struct Processing {
        bool mono = false;
        float balance = 0.f; // -1 left, 0 centre, +1 right
        int syncMs = 0;      // positive delays, negative advances
    };
    enum class Monitoring { Off, Only, AndOutput };
    struct Routing {
        Monitoring monitoring = Monitoring::Off;
        bool solo = false; // solo affects monitoring only
        unsigned tracks = 1; // six output buses; bit 0 is track 1
    };
    struct Buses {
        std::array<float *, 6> tracks{};
        float *monitor = nullptr;
    };
    void setRouting(int source, Routing routing);
    bool sourceActive(int source) const;
    void setProcessing(int source, Processing processing);
    void setFilters(int source, const std::vector<AudioFilters::Config> &filters);
    void setGain(int source, float gain);
    void setMuted(int source, bool muted);
    void setPushSettings(int source, PushControl::Settings settings);
    void holdPush(int source, bool talk, bool held, unsigned origin);
    void cancelPush(int source = -1, unsigned origins = ~0u);
    bool pushHeld(int source, bool talk) const;
    void setActive(int source, bool active);
    // How much of a source goes out, 0..1, reached in a straight line over
    // `rampNs` of the mix's clock, starting `delayNs` from now: transitions
    // fade a studio's sound with its picture. Independent of the fader and mute.
    void setAirGain(int source, float gain, int64_t rampNs = 0, int64_t delayNs = 0);
    float airGain(int source) const;
    void clearSource(int source);
    float sourcePeak(int source) const;
    PeakMeter::Reading sourceMeter(int source) const;
    void resetMeterMaximum(int source);
    void setTruePeakMeter(bool enabled);

    void start(int64_t timeNs);
    void stop();
    // A live clock keeps output moving through silence. Future-dated inputs
    // must never drive this clock (in particular when sync delays are used).
    void advanceTo(int64_t timeNs);
    // arrivalTimeNs is optional, in the same monotonic clock as timeNs. Live
    // callers supply it before locking so the buffer includes capture age.
    void push(int source, int64_t timeNs, const float *interleaved, int frames, int input = 0, int64_t arrivalTimeNs = 0);
    int pull(float *out, int frames, int64_t *firstFrameTimeNs = nullptr);
    int pull(const Buses &buses, int frames, int64_t *firstFrameTimeNs = nullptr);
    // Finalize up to an explicit timeline boundary, ignoring lookahead. Missing
    // future input (e.g. a negative offset at clip end) becomes silence.
    int drain(float *out, int maxFrames, int64_t endTimeNs, int64_t *firstFrameTimeNs = nullptr);
    uint64_t lateFrames() const;
    std::vector<float> lastPeaks() const;

private:
    struct Channel {
        std::map<int, std::vector<float>> inputs;
        std::map<int, int64_t> arrivalAge; // capture graph delay, measured before taking the mixer lock
        std::map<int, int64_t> nextFrame;  // per input, where its last chunk ended: the next goes on from there
        int nextInput = 1;
        AudioFilters filters;
        std::vector<AudioFilters::Config> filterConfigs;
        PeakMeter meter;
        std::vector<float> frame;
        Processing processing;
        Routing routing;
        PushControl push;
        float gain = 1.f;
        bool muted = false;
        bool active = true;
        float air = 1.f, airTarget = 1.f, airStep = 0.f;
        int64_t airWait = 0;  // frames before the ramp starts
        float peak = 0.f;
        int64_t peakTime = 0;
    };
    int mixLocked(const Buses &buses, int frames, int64_t cursor, int64_t *firstFrameTimeNs);
    Channel makeChannel(bool defaultInput = true) const;
    int64_t frameIndexFor(int64_t timeNs) const;
    int64_t captureAgeLocked() const;
    int m_rate, m_channels;
    int64_t m_latencyFrames, m_ringFrames;
    std::map<int, Channel> m_sources;
    int m_nextSource = 0;
    std::vector<float> m_peaks;
    int64_t m_startNs = 0;
    bool m_running = false;
    bool m_truePeakMeter = false;
    int64_t m_readCursor = 0, m_newestEnd = 0, m_clockEnd = 0;
    bool m_clockDriven = false;
    uint64_t m_late = 0;
    mutable std::mutex m_mutex;
};
