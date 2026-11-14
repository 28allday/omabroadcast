#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "capture/picture.h"
#include "capture/video_devices.h"
#include "engine/frame.h"

// One capture device (camera, HDMI capture card) streaming in one mode.
// Uncompressed frames are copied out of PipeWire's buffers as they arrive;
// MJPEG is decoded on a worker thread so a slow JPEG never holds up the frame
// loop. Keeps the newest frame only.
class DeviceStream {
public:
    using Picture = ::Picture;

    DeviceStream();
    ~DeviceStream();

    // The device as VideoDevices found it: its node id now, its name to keep.
    bool start(const VideoDevice &device, const VideoMode &mode, std::string *error);
    void stop();
    bool running() const { return m_running.load(); }

    // Swaps in the newest frame when there is one since the last call.
    bool acquire(Picture &out);

    int width() const { return m_width.load(); }
    int height() const { return m_height.load(); }
    uint64_t framesReceived() const { return m_received.load(); }
    uint64_t framesDropped() const { return m_dropped.load(); }   // replaced before they were used
    // Different pictures a second over the last two seconds (a card showing a
    // 50 Hz signal in a 60 Hz mode repeats one in six); 0 until there are two.
    double pictureRate() const;
    std::string error() const;
    VideoMode mode() const { return m_mode; }

    struct Impl;

private:
    friend struct Impl;
    Impl *d;
    VideoMode m_mode;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_width{0}, m_height{0};
    std::atomic<uint64_t> m_received{0}, m_dropped{0};
};
