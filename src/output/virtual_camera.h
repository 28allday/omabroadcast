#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "engine/frame.h"

// The program as a camera other apps can open: a PipeWire video source with
// the camera role, which browsers and calling apps reach through the camera
// portal. It runs its own clock, one picture per frame handed to it, in
// NV12, I420 or YUY2 as the app asks; limited-range BT.709 like the encoders
// get. Pictures are only wanted while an app is taking them.
class VirtualCamera {
public:
    struct Settings {
        int width = 1920, height = 1080;
        int fpsNum = 30, fpsDen = 1;
        std::string nodeName = "omabroadcast-camera";
        std::string description = "OmaBroadcast Camera";
    };
    enum class Format { None, Nv12, I420, Yuy2 };

    VirtualCamera();
    ~VirtualCamera();

    bool start(const Settings &settings, std::string *error);
    void stop();
    bool running() const { return m_running.load(); }
    // An app is taking pictures, in this format.
    bool inUse() const { return m_format.load() != Format::None && m_streaming.load(); }
    Format format() const { return m_format.load(); }
    // Which layout to hand over: NV12 for NV12, planar for I420 and YUY2.
    bool wantsNv12() const { return m_format.load() == Format::Nv12; }

    // Copies the picture out and sends it. A size other than the one
    // started with renegotiates with the app. Any thread.
    void push(const engine::YuvFrame &frame);

    uint64_t framesSent() const { return m_sent.load(); }
    // Pictures replaced by a newer one before the app took them.
    uint64_t framesDropped() const { return m_dropped.load(); }
    // The PipeWire node id, once it has one (for the status line).
    uint32_t nodeId() const { return m_nodeId.load(); }
    std::string error() const;
    // Told on PipeWire's thread when an app starts or stops taking pictures.
    std::function<void()> onStateChanged;

    struct Impl;

private:
    friend struct Impl;
    void teardown();
    Impl *d;
    std::atomic<bool> m_running{false}, m_streaming{false};
    std::atomic<Format> m_format{Format::None};
    std::atomic<uint64_t> m_sent{0}, m_dropped{0};
    std::atomic<uint32_t> m_nodeId{0};
};
