#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Video capture devices as PipeWire sees them — USB and USB-C cameras and
// HDMI capture cards, anything behind a V4L2 or libcamera device — with every
// mode each one offers, kept current as they are plugged in and out. Screen
// casts and other software video nodes are not devices and are not listed.
struct VideoMode {
    // P010 and I010: 10 bits a sample (capture cards), high-aligned and planar.
    enum class Format { NV12, I420, YUY2, MJPEG, P010, I010 };
    Format format = Format::NV12;
    int width = 0, height = 0;
    int fpsNum = 0, fpsDen = 1;

    double fps() const { return fpsDen > 0 ? double(fpsNum) / fpsDen : 0; }
    // "1920x1080@60/1:NV12": what a source saves to ask for this mode again.
    std::string key() const;
    // "1920 × 1080 · 60 fps · NV12", for people.
    std::string label() const;
    bool operator==(const VideoMode &o) const = default;
};

struct VideoDevice {
    uint32_t id = 0;
    std::string serial;       // object.serial: how a stream names this node as its target
    std::string name;         // node.name: stable for the same device on the same port
    std::string description;
    std::string api;          // "v4l2", "libcamera"
    std::string path;         // the V4L2 device file, for its picture controls; empty otherwise
    std::vector<VideoMode> modes;
};

// The device's best mode, or the one `wanted` names when it still offers it:
// most pixels a second, then most pixels, then uncompressed before MJPEG.
// With `canvasFps`, the device's largest picture at the canvas's frame rate
// comes first when it offers one, so a card set to 50 runs at 50.
VideoMode pickVideoMode(const std::vector<VideoMode> &modes, const std::string &wanted = {}, double canvasFps = 0);

// A source whose picture changes `pictureRate` times a second, on a canvas of
// `canvasFps`: the frame rate (as num/den) at which every picture would show
// for as long as the others, when the canvas's makes some show longer and
// motion judder (50 Hz on 60, 24 on 60). False when it already fits, or the
// rate is not steady at a standard one (a still picture changes rarely).
bool evenFrameRate(double pictureRate, double canvasFps, int *num, int *den);

class VideoDevices {
public:
    using Changed = std::function<void()>;   // on PipeWire's thread

    VideoDevices();
    ~VideoDevices();

    bool start(Changed changed, std::string *error);
    void stop();
    std::vector<VideoDevice> devices() const;
    // False once the connection to PipeWire has gone (it restarted, or was never
    // there) and the list has been emptied. Nothing comes back by itself: start again.
    bool connected() const { return m_connected.load(); }

    struct Impl;

private:
    Impl *d;
    std::atomic<bool> m_connected{false};
};
