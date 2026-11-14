#include "mixer.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {
int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

AudioMixer::AudioMixer(int sampleRate, int channels, int latencyMs, int ringSeconds)
    : m_rate(std::max(1, sampleRate)), m_channels(std::max(1, channels)),
      m_latencyFrames(int64_t(m_rate) * std::max(0, latencyMs) / 1000),
      m_ringFrames(int64_t(m_rate) * std::max(1, ringSeconds)), m_peaks(m_channels, 0.f) {}

bool AudioMixer::setFormat(int sampleRate, int channels) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running || sampleRate <= 0 || channels < 1 || channels > 8) return false;
    const int seconds = int(m_ringFrames / m_rate);
    const int latencyMs = int(m_latencyFrames * 1000 / m_rate);
    m_rate = sampleRate; m_channels = channels;
    m_ringFrames = int64_t(m_rate) * seconds;
    m_latencyFrames = int64_t(m_rate) * latencyMs / 1000;
    m_peaks.assign(channels, 0);
    for (auto &[id, channel] : m_sources) {
        channel.filters = AudioFilters(m_rate, m_channels);
        channel.filters.configure(channel.filterConfigs);
        channel.meter = PeakMeter(channels); channel.meter.setTruePeak(m_truePeakMeter);
        channel.frame.assign(channels, 0); channel.peak = 0;
        for (auto &[input, ring] : channel.inputs) ring.assign(size_t(m_ringFrames)*channels, 0);
        for (auto &[input, age] : channel.arrivalAge) age = 0;
    }
    return true;
}

AudioMixer::Channel AudioMixer::makeChannel(bool defaultInput) const {
    Channel channel;
    channel.filters = AudioFilters(m_rate, m_channels);
    channel.meter = PeakMeter(m_channels);
    channel.meter.setTruePeak(m_truePeakMeter);
    channel.frame.resize(size_t(m_channels));
    if (defaultInput) channel.arrivalAge[0] = 0;
    if (defaultInput) channel.inputs[0].resize(static_cast<size_t>(m_ringFrames) * m_channels, 0.f);
    return channel;
}

void AudioMixer::setLatencyMs(int latencyMs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const int64_t frames = int64_t(m_rate) * std::clamp(latencyMs, 0, 500) / 1000;
    if (frames == m_latencyFrames) return;
    m_latencyFrames = frames;
    if (!m_running || !m_clockDriven) return;
    int64_t lookahead = 0;
    for (const auto &[id, channel] : m_sources)
        lookahead = std::max(lookahead, -int64_t(channel.processing.syncMs) * m_rate / 1000);
    const int64_t next = std::max(m_readCursor, m_clockEnd - frames - captureAgeLocked() - lookahead);
    // Reducing latency discards the obsolete backlog. Otherwise it would
    // merely move into the playback queue and keep the old monitoring delay.
    for (auto &[id, channel] : m_sources) {
        for (auto &[input, ring] : channel.inputs)
            for (int64_t i = m_readCursor; i < std::min(next, m_readCursor + m_ringFrames); ++i)
                std::fill_n(&ring[size_t(i % m_ringFrames) * m_channels], m_channels, 0.f);
        if (next > m_readCursor) channel.filters.reset();
        channel.peak = 0.f;
        channel.meter.reset();
    }
    m_readCursor = next;
}

void AudioMixer::setSourceCount(int n) {
    std::lock_guard<std::mutex> lock(m_mutex);
    n = std::max(0, n);
    for (auto it = m_sources.lower_bound(n); it != m_sources.end();) it = m_sources.erase(it);
    for (int i = 0; i < n; ++i) if (!m_sources.count(i)) m_sources.emplace(i, makeChannel());
    m_nextSource = std::max(m_nextSource, n);
}

int AudioMixer::addSource(bool defaultInput) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const int id = m_nextSource++;
    m_sources.emplace(id, makeChannel(defaultInput));
    return id;
}

int AudioMixer::addInput(int source) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_sources.find(source);
    if (it == m_sources.end()) return -1;
    const int input = it->second.nextInput++;
    it->second.arrivalAge[input] = 0;
    it->second.inputs[input].resize(static_cast<size_t>(m_ringFrames) * m_channels, 0.f);
    return input;
}

void AudioMixer::removeInput(int source, int input) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_sources.find(source); it != m_sources.end()) {
        it->second.inputs.erase(input);
        it->second.arrivalAge.erase(input);
        it->second.nextFrame.erase(input);
        it->second.peak = 0.f;
        it->second.meter.reset();
    }
}

void AudioMixer::removeSource(int source) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sources.erase(source);
}

void AudioMixer::setRouting(int source, Routing routing) {
    std::lock_guard<std::mutex> lock(m_mutex);
    routing.tracks &= 63;
    if (auto it = m_sources.find(source); it != m_sources.end()) it->second.routing = routing;
}

bool AudioMixer::sourceActive(int source) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_sources.find(source);
    return it != m_sources.end() && it->second.active;
}

void AudioMixer::setProcessing(int source, Processing processing) {
    std::lock_guard<std::mutex> lock(m_mutex);
    processing.balance = std::isfinite(processing.balance) ? std::clamp(processing.balance, -1.f, 1.f) : 0.f;
    // Keep delayed samples and negative-offset lookahead inside the ring.
    const int availableMs = int(std::max<int64_t>(0, m_ringFrames - m_latencyFrames) * 1000 / m_rate);
    processing.syncMs = std::clamp(processing.syncMs, -std::min(1000, availableMs / 3),
                                  std::min(2000, availableMs / 3 * 2));
    if (auto it = m_sources.find(source); it != m_sources.end()) {
        if (processing.syncMs != it->second.processing.syncMs) {
            for (auto &[id, ring] : it->second.inputs) std::fill(ring.begin(), ring.end(), 0.f);
            // A small change would otherwise count as following on, and do nothing.
            it->second.nextFrame.clear();
            it->second.filters.reset();
            it->second.peak = 0.f;
            it->second.meter.reset();
        }
        it->second.processing = processing;
    }
}

void AudioMixer::setFilters(int source, const std::vector<AudioFilters::Config> &filters) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_sources.find(source); it != m_sources.end()) {
        it->second.filters.configure(filters);
        it->second.filterConfigs = filters;
    }
}

void AudioMixer::setGain(int source, float gain) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_sources.find(source); it != m_sources.end())
        it->second.gain = std::isfinite(gain) ? std::clamp(gain, 0.f, 20.f) : 1.f;
}

void AudioMixer::setMuted(int source, bool muted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_sources.find(source); it != m_sources.end()) it->second.muted = muted;
}

void AudioMixer::setPushSettings(int source, PushControl::Settings settings) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_sources.find(source); it != m_sources.end()) it->second.push.configure(settings);
}
void AudioMixer::holdPush(int source, bool talk, bool held, unsigned origin) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_sources.find(source); it != m_sources.end() && it->second.active)
        it->second.push.hold(talk, held, origin, m_readCursor, m_rate);
}
void AudioMixer::cancelPush(int source, unsigned origins) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto &[id, channel] : m_sources) if (source < 0 || source == id) channel.push.cancel(origins);
}
bool AudioMixer::pushHeld(int source, bool talk) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_sources.find(source);
    return it != m_sources.end() && it->second.push.held(talk);
}

void AudioMixer::setActive(int source, bool active) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_sources.find(source); it != m_sources.end()) {
        if (it->second.active != active) it->second.filters.reset();
        it->second.active = active;
        if (!active) { it->second.push.cancel(); it->second.peak = 0.f; it->second.meter.reset(); }
    }
}

void AudioMixer::setAirGain(int source, float gain, int64_t rampNs, int64_t delayNs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_sources.find(source); it != m_sources.end()) {
        Channel &c = it->second;
        c.airTarget = std::clamp(gain, 0.f, 1.f);
        c.airWait = std::max<int64_t>(0, delayNs * m_rate / 1'000'000'000);
        const int64_t frames = rampNs * m_rate / 1'000'000'000;
        if (frames <= 0 && c.airWait == 0) { c.air = c.airTarget; c.airStep = 0.f; }
        else c.airStep = (c.airTarget - c.air) / float(std::max<int64_t>(1, frames));
    }
}

float AudioMixer::airGain(int source) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_sources.find(source);
    return it != m_sources.end() ? it->second.air : 1.f;
}

void AudioMixer::clearSource(int source) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_sources.find(source); it != m_sources.end()) {
        for (auto &[id, age] : it->second.arrivalAge) age = 0;
        it->second.nextFrame.clear();
        for (auto &[id, ring] : it->second.inputs) std::fill(ring.begin(), ring.end(), 0.f);
        it->second.filters.reset();
        it->second.push.cancel();
        it->second.peak = 0.f;
        it->second.meter.reset();
    }
}

float AudioMixer::sourcePeak(int source) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_sources.find(source);
    if (it == m_sources.end() || it->second.muted || !it->second.active || !m_running || !it->second.push.audible(m_readCursor)) return 0.f;
    return nowNs() - it->second.peakTime < 150'000'000 ? it->second.peak : 0.f;
}

PeakMeter::Reading AudioMixer::sourceMeter(int source) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_sources.find(source);
    if (it == m_sources.end()) return {std::vector<float>(m_channels, 0.f), 0.f};
    auto result = it->second.meter.reading(nowNs());
    if (!m_running || !it->second.active || it->second.muted || !it->second.push.audible(m_readCursor)) std::fill(result.levels.begin(), result.levels.end(), 0.f);
    return result;
}

void AudioMixer::resetMeterMaximum(int source) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_sources.find(source); it != m_sources.end()) it->second.meter.resetMaximum();
}

void AudioMixer::setTruePeakMeter(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_truePeakMeter == enabled) return;
    m_truePeakMeter = enabled;
    for (auto &[id, channel] : m_sources) channel.meter.setTruePeak(enabled);
}

uint64_t AudioMixer::lateFrames() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_late;
}

std::vector<float> AudioMixer::lastPeaks() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_peaks;
}

void AudioMixer::start(int64_t timeNs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_startNs = timeNs;
    m_running = true;
    m_readCursor = m_newestEnd = m_clockEnd = 0;
    m_clockDriven = false;
    m_late = 0;
    std::fill(m_peaks.begin(), m_peaks.end(), 0.f);
    for (auto &[id, channel] : m_sources) {
        for (auto &[id, ring] : channel.inputs) std::fill(ring.begin(), ring.end(), 0.f);
        channel.filters.reset();
        channel.push.cancel();
        for (auto &[input, age] : channel.arrivalAge) age = 0;
        channel.nextFrame.clear();
        channel.peak = 0.f;
        channel.meter.reset();
    }
}

void AudioMixer::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_running = false;
}

void AudioMixer::advanceTo(int64_t timeNs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running) return;
    m_clockDriven = true;
    m_clockEnd = std::max(m_clockEnd, frameIndexFor(timeNs));
}

int64_t AudioMixer::frameIndexFor(int64_t timeNs) const {
    return static_cast<int64_t>(std::llround((timeNs - m_startNs) / 1e9L * m_rate));
}

void AudioMixer::push(int source, int64_t timeNs, const float *in, int frames, int input, int64_t arrivalTimeNs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_sources.find(source);
    if (!m_running || !in || frames <= 0 || it == m_sources.end()) return;
    const auto buffer = it->second.inputs.find(input);
    if (buffer == it->second.inputs.end()) return;
    if (arrivalTimeNs > 0) {
        // Application capture can be a graph cycle older than device capture.
        // Keep the requested latency as scheduling margin after that observed
        // transport delay; do not silently discard the start of every buffer.
        auto &age = it->second.arrivalAge[input];
        const int64_t measured = std::clamp<int64_t>(frameIndexFor(arrivalTimeNs) - frameIndexFor(timeNs), 0, m_rate / 2);
        // Up at once; back down slowly once a stall has passed (the excess a
        // third smaller each second at a hundred callbacks a second), so one
        // late callback does not hold the latency up for good.
        age = measured >= age ? measured : age - std::max<int64_t>(1, (age - measured) / 256);
    }
    int64_t pos = frameIndexFor(timeNs) + int64_t(it->second.processing.syncMs) * m_rate / 1000;
    // Within a couple of milliseconds of where the last chunk ended: taken as
    // following on from it, so time-stamp jitter leaves no gap or overlap
    // (each a click). Further off, the time stamp wins (a real jump).
    if (auto next = it->second.nextFrame.find(input); next != it->second.nextFrame.end() &&
        std::abs(pos - next->second) <= int64_t(m_rate) / 500)
        pos = next->second;
    it->second.nextFrame[input] = pos + frames;
    int64_t end = pos + frames;
    if (end <= m_readCursor) { m_late += std::max<int64_t>(0, end - std::max<int64_t>(0, pos)); return; }
    if (pos < m_readCursor) {
        m_late += m_readCursor - std::max<int64_t>(0, pos);
        in += (m_readCursor - pos) * m_channels;
        pos = m_readCursor;
    }
    end = std::min(end, m_readCursor + m_ringFrames);
    for (int64_t f = pos; f < end; ++f, in += m_channels) {
        float *slot = &buffer->second[static_cast<size_t>(f % m_ringFrames) * m_channels];
        for (int c = 0; c < m_channels; ++c) slot[c] = std::isfinite(in[c]) ? in[c] : 0.f;
    }
    m_newestEnd = std::max(m_newestEnd, end);
}

int64_t AudioMixer::captureAgeLocked() const {
    int64_t age = 0;
    for (const auto &[id, channel] : m_sources)
        if (channel.active && !channel.muted && channel.gain > 0)
            for (const auto &[input, frames] : channel.arrivalAge) age = std::max(age, frames);
    return age;
}

int AudioMixer::pull(float *out, int frames, int64_t *firstFrameTimeNs) {
    if (!out) return 0;
    Buses buses; buses.tracks[0] = out;
    return pull(buses, frames, firstFrameTimeNs);
}

int AudioMixer::pull(const Buses &buses, int frames, int64_t *firstFrameTimeNs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    int64_t lookahead = 0;
    for (const auto &[id, channel] : m_sources)
        lookahead = std::max(lookahead, -int64_t(channel.processing.syncMs) * m_rate / 1000);
    const int64_t readyEnd = m_clockDriven ? m_clockEnd : m_newestEnd;
    if (!m_running || frames <= 0 || m_readCursor + frames > readyEnd - m_latencyFrames - captureAgeLocked() - lookahead) return 0;
    return mixLocked(buses, frames, m_readCursor, firstFrameTimeNs);
}

int AudioMixer::drain(float *out, int maxFrames, int64_t endTimeNs, int64_t *firstFrameTimeNs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running || !out || maxFrames <= 0) return 0;
    const int frames = int(std::clamp<int64_t>(frameIndexFor(endTimeNs) - m_readCursor, 0, maxFrames));
    Buses buses; buses.tracks[0] = out;
    return frames > 0 ? mixLocked(buses, frames, m_readCursor, firstFrameTimeNs) : 0;
}

int AudioMixer::mixLocked(const Buses &buses, int frames, int64_t cursor, int64_t *firstFrameTimeNs) {
    if (firstFrameTimeNs) *firstFrameTimeNs = m_startNs + static_cast<int64_t>(cursor * 1'000'000'000.0L / m_rate);
    for (float *out : buses.tracks) if (out) std::fill(out, out + size_t(frames) * m_channels, 0.f);
    if (buses.monitor) std::fill(buses.monitor, buses.monitor + size_t(frames) * m_channels, 0.f);
    bool soloing = false;
    for (const auto &[id, channel] : m_sources)
        if (channel.active && channel.routing.solo) soloing = true;
    const int64_t now = nowNs();
    for (auto &[id, channel] : m_sources) {
        float peak = 0.f;
        const float gain = channel.muted || !channel.active ? 0.f : channel.gain;
        for (int i = 0; i < frames; ++i) {
            const size_t pos = static_cast<size_t>((cursor + i) % m_ringFrames) * m_channels;
            float mono = 0.f;
            for (int c = 0; c < m_channels; ++c) {
                float sum = 0.f;
                for (auto &[input, ring] : channel.inputs) {
                    sum += ring[pos + c];
                    ring[pos + c] = 0.f;
                }
                channel.frame[size_t(c)] = channel.active ? sum : 0.f;
            }
            // Sum producers, then filters, mono, balance and fader. Muting
            // keeps DSP running; hiding resets history and suppresses tails.
            if (channel.active) channel.filters.process(channel.frame.data());
            if (channel.processing.mono) {
                for (float value : channel.frame) mono += value;
                mono /= m_channels;
            }
            if (channel.airWait > 0) --channel.airWait;
            else if (channel.airStep != 0.f) {
                channel.air += channel.airStep;
                if ((channel.airStep > 0.f) == (channel.air >= channel.airTarget)) { channel.air = channel.airTarget; channel.airStep = 0.f; }
            }
            for (int c = 0; c < m_channels; ++c) {
                const float sum = (channel.processing.mono ? mono : channel.frame[size_t(c)]) * channel.air;
                float balanceGain = 1.f;
                if (m_channels >= 2) {
                    if (c == 0) balanceGain = 1.f - std::max(0.f, channel.processing.balance);
                    if (c == 1) balanceGain = 1.f + std::min(0.f, channel.processing.balance);
                }
                const float v = channel.push.audible(cursor + i) ? sum * gain * balanceGain : 0.f;
                channel.frame[size_t(c)] = v;
                const size_t at = size_t(i) * m_channels + c;
                if (channel.routing.monitoring != Monitoring::Only)
                    for (size_t t = 0; t < buses.tracks.size(); ++t)
                        if (buses.tracks[t] && (channel.routing.tracks & (1u << t))) buses.tracks[t][at] += v;
                const bool monitored = soloing ? channel.routing.solo : channel.routing.monitoring != Monitoring::Off;
                if (buses.monitor && monitored) buses.monitor[at] += v;
                peak = std::max(peak, std::fabs(v));
            }
            channel.meter.process(channel.frame.data(), now);
        }
        // Hold peaks briefly so a UI poll cannot miss a short transient.
        if (peak >= channel.peak || now - channel.peakTime >= 100'000'000) {
            channel.peak = peak;
            channel.peakTime = now;
        }
    }
    std::fill(m_peaks.begin(), m_peaks.end(), 0.f);
    for (size_t i = 0; i < size_t(frames) * m_channels; ++i) {
        for (float *out : buses.tracks) if (out) out[i] = std::clamp(out[i], -1.f, 1.f);
        if (buses.monitor) buses.monitor[i] = std::clamp(buses.monitor[i], -1.f, 1.f);
        if (buses.tracks[0]) m_peaks[i % m_channels] = std::max(m_peaks[i % m_channels], std::fabs(buses.tracks[0][i]));
    }
    m_readCursor += frames;
    return frames;
}
