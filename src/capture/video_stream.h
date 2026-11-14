#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "engine/frame.h"

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct pw_buffer;
struct spa_hook;

// A PipeWire video capture stream, as the screen-cast portal hands one out.
// Negotiates DMA-BUF (with the modifiers the engine can import) and falls
// back to shared memory. Keeps only the newest frame: the consumer calls
// acquire() for it, uses it, then release() to hand the buffer back.
//
// Callbacks from PipeWire run on its own thread; acquire/release may be
// called from any single consumer thread.
class VideoStream {
public:
    struct Frame {
        bool dmabuf = false;
        engine::DmaBufFrame dma;
        engine::CpuFrame cpu;
        int64_t timeNs = 0;      // CLOCK_MONOTONIC when the frame arrived
        int64_t durationNs = 0;
        bool discontinuity = false;
        pw_buffer *raw = nullptr;
    };
    using ModifierLookup = std::function<std::vector<uint64_t>(uint32_t fourcc)>;
    // The DRM formats it offers, which the lookup is asked about.
    static std::vector<uint32_t> fourccs();
    // Whether a picture of this size, four bytes a pixel, lies inside `size` bytes
    // as a producer describes it. Sets a stride of none to the rows' own width.
    static bool pictureFits(uint32_t offset, int *stride, int width, int height, uint32_t size);

    VideoStream();
    ~VideoStream();

    // `fd` is the portal's PipeWire connection; the stream takes ownership.
    // A negative fd connects to the user's own PipeWire instead (any node the
    // user can see, e.g. a test pattern).
    bool start(int fd, uint32_t nodeId, ModifierLookup modifiers, std::string *error);
    // Renegotiate without DMA-BUF, for when the engine could not import one.
    void disableDmaBuf();
    void stop();

    bool acquire(Frame &out);
    void release(Frame &frame);

    bool streaming() const { return m_streaming.load(); }
    std::string error() const;
    int width() const { return m_width.load(); }
    int height() const { return m_height.load(); }
    bool usingDmaBuf() const { return m_dmabuf.load(); }
    uint64_t framesReceived() const { return m_received.load(); }
    uint64_t framesDropped() const { return m_dropped.load(); }

    struct Impl;

private:
    friend struct Impl;
    Impl *d;

    std::atomic<bool> m_streaming{false};
    std::atomic<bool> m_dmabuf{false};
    std::atomic<int> m_width{0}, m_height{0};
    std::atomic<uint64_t> m_received{0}, m_dropped{0};
};
