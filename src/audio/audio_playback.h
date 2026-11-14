#pragma once

#include "format.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

// A dedicated, explicitly targeted PipeWire monitoring stream. Its bounded
// SPSC ring never allocates or waits in the realtime callback. Unplugging the
// selected sink cannot redirect monitoring to another output.
class AudioPlayback {
public:
    AudioPlayback();
    ~AudioPlayback();
    bool start(const std::string &targetSerial, std::string *error);
    // Before start: the stream's PipeWire name and the name people see.
    void setName(const std::string &node, const std::string &description);
    void stop();
    void setLatencyFrames(int frames);
    bool setFormat(AudioFormat format); // only while stopped
    void push(const float *interleaved, int frames);
    bool running() const { return m_running.load(); }
    std::string error() const;
    uint64_t droppedFrames() const;
    uint64_t queuedFrames() const;
    uint64_t underrunFrames() const;
    int quantumFrames() const;
    std::function<void()> onStateChanged;
    struct Impl;
private:
    Impl *d;
    std::atomic<bool> m_running{false};
};
