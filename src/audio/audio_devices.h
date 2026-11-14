#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// PipeWire inputs, sink monitors and application playback streams. Names and
// application keys are saved choices; serials address exact runtime nodes.
// Only hardware inputs are eligible for automatic microphone selection.
struct AudioNode {
    enum Kind { Input, Output, Application };
    Kind kind = Input;
    uint32_t id = 0;
    std::string name;         // node.name: stable across runs, what a choice is saved as
    std::string description;  // for people
    bool hardware = false;
    int priority = 0;         // priority.session; the session manager's own ranking
    std::string serial;      // runtime identity, never saved as a device choice
    std::string application; // stable application identity shared by its streams
    std::string applicationName;
    bool operator==(const AudioNode &) const = default;
};

using AudioInput = AudioNode;

bool describeAudioNode(uint32_t id, const std::map<std::string, std::string> &props, AudioNode *out);

// Whether a node with these properties is an audio input, and what it is.
bool describeAudioInput(uint32_t id, const std::map<std::string, std::string> &props, AudioInput *out);

// Which input the microphone channel should capture. `choice` is "" for
// automatic, "none" for off, otherwise an input's node.name.
struct MicrophonePick {
    enum State { Using, NoneFound, Off, Missing };
    State state = NoneFound;
    std::string name;         // node.name to capture; empty unless Using
    std::string description;
};
MicrophonePick pickMicrophone(const std::vector<AudioInput> &inputs, const std::string &choice,
                              const std::string &defaultName);

struct pw_thread_loop;

class AudioDevices {
public:
    // Called on PipeWire's thread whenever the inputs or the default change.
    using Changed = std::function<void()>;

    AudioDevices();
    ~AudioDevices();

    bool start(Changed changed, std::string *error);
    void stop();

    std::vector<AudioInput> inputs() const;
    std::vector<AudioNode> nodes() const;
    std::string defaultOutput() const;
    std::string defaultInput() const;  // node.name of the session's default source
    // False once the connection to PipeWire has gone (it restarted, or was never
    // there) and the lists have been emptied. Nothing comes back by itself: start again.
    bool connected() const { return m_connected.load(); }

    struct Impl;

private:
    Impl *d;
    std::atomic<bool> m_connected{false};
};
