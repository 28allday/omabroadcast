#pragma once

#include <functional>
#include <string>

// An output of the app's own in the PipeWire graph: a sink nothing hears but
// whoever records its monitor. A browser source's page plays into one, so
// its sound reaches the mixer and never the speakers. It lasts as long as
// this object (and goes with the app's connection, should the app die);
// it never becomes the desktop's default (lowest priority) and the app's
// own pickers do not offer it.
class PrivateSink {
public:
    // `ready` runs once on PipeWire's thread: true when the sink exists and
    // can be played into, false when it could not be made.
    PrivateSink(const std::string &nodeName, const std::string &description, int channels, std::function<void(bool)> ready);
    ~PrivateSink();
    PrivateSink(const PrivateSink &) = delete;
    PrivateSink &operator=(const PrivateSink &) = delete;

    const std::string &name() const { return m_name; }
    // Whether something that plays through PulseAudio (as browsers do)
    // reaches PipeWire here; without it a page would play to the speakers.
    static bool pulseAvailable();

    struct Impl;

private:
    std::string m_name;
    Impl *d;
};
