#pragma once

#include "format.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

struct pw_thread_loop;
struct pw_context;
struct pw_core;

// Captures what the desktop plays and what the microphone hears, straight
// from PipeWire, as float PCM in the selected format. Source 0 is the desktop (the
// default sink's monitor), source 1 the input named by setMicrophone() —
// that input only: if it goes away the channel falls silent rather than
// quietly moving to another device.
class AudioCapture {
public:
    enum Source { Desktop = 0, Microphone = 1, Count = 2 };
    // Called on PipeWire's thread with interleaved frames stamped
    // with the monotonic time their first sample was heard/played.
    using Sink = std::function<void(int source, int64_t timeNs, const float *interleaved, int frames)>;

    AudioCapture();
    ~AudioCapture();

    // For a source-specific input, false avoids opening another desktop capture.
    bool start(Sink sink, std::string *error, bool captureDesktop = true);
    void stop();
    void setLatencyFrames(int frames);
    // Only while stopped; returns false if capture is already open.
    bool setFormat(AudioFormat format);
    // node.name of the input to capture; empty captures nothing. Safe to call
    // before start() and while running.
    void setMicrophone(const std::string &nodeName);
    // Opens or closes the capture of the desktop's sound while running.
    void setDesktop(bool capture);
    // An input, sink monitor or application playback stream, addressed by
    // node.name or object.serial. Uses the Microphone slot; no desktop copy.
    void setTarget(const std::string &target, bool sinkMonitor);

    std::function<void()> onStateChanged; // configured before start; runs on PipeWire
    std::string error() const;
    bool running(int source) const { return m_running[source].load(); }
    int quantumFrames(int source) const { return m_quantum[source].load(); }
    int64_t delayNs(int source) const { return m_delay[source].load(); }
    float peak(int source, int channel) const { return m_peak[source][channel].load(); }

    struct Impl;

private:
    Impl *d;
    std::atomic<bool> m_running[Count];
    std::atomic<int> m_quantum[Count]{};
    std::atomic<int64_t> m_delay[Count]{};
    std::atomic<float> m_peak[Count][8];
    friend struct Impl;
};
