#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <map>
#include <string>
#include <vector>

#include "engine/frame.h"

// One independent Wayland connection, shared by its catalog and captures.
// All protocol objects belong to its worker. Consumers own immutable pictures;
// neither discovery nor a slow compositor can block the UI or render thread.
class DirectCapture {
public:
    struct Target {
        std::string key, name, appId, title;
        bool window = false, focused = false;
        bool operator==(const Target &) const = default;
    };
    struct Catalog {
        bool ready = false, displays = false, windows = false;
        std::string error;
        std::vector<Target> targets;
    };
    struct Settings {
        bool window = false, focused = false, cursor = true;
        std::string key, output, appId, title;
        int fps = 60;
        // Empty for CPU consumers; explicit modifiers supported by the renderer.
        std::string renderNode;
        std::map<uint32_t, std::vector<uint64_t>> modifiers;
        bool operator==(const Settings &) const = default;
    };
    struct Frame {
        int width = 0, height = 0, stride = 0;
        engine::PixelFormat format = engine::PixelFormat::BGRA;
        bool premultiplied = false;
        int64_t timeNs = 0;
        uint64_t generation = 0;
        std::vector<uint8_t> pixels;
        engine::DmaBufFrame dma;
        bool dmabuf = false;
        // Separate from allocation ownership: caches may retain memory, but
        // only a live frame lease prevents the capture worker reusing it.
        std::shared_ptr<const void> lease;
        engine::CpuFrame input() const { return {width, height, stride, format, pixels.data(), premultiplied}; }
    };
    enum class State { Waiting, Live, Unsupported, Failed, Stopped };
    class Stream {
    public:
        ~Stream();
        void configure(const Settings &settings);
        void stop();
        void disableDmaBuf();
        void setDmaBufDevice(const std::string &node, const std::map<uint32_t,std::vector<uint64_t>> &modifiers);
        // Latest frame, at most once. A target switch clears availability before
        // another picture arrives, so an old window cannot linger on output.
        std::shared_ptr<const Frame> acquire();
        State state() const;
        std::string error() const;
        bool hasPicture() const;
        int width() const;
        int height() const;
        uint64_t received() const;
        uint64_t dropped() const;
        uint64_t generation() const;
    private:
        friend class DirectCapture;
        struct Data;
        explicit Stream(std::shared_ptr<Data> data);
        std::shared_ptr<Data> d;
    };

    // Empty socket uses WAYLAND_DISPLAY.
    explicit DirectCapture(std::string socket = {});
    ~DirectCapture();
    Catalog catalog();
    std::shared_ptr<Stream> start(const Settings &settings);
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
