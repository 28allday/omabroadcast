#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "encoded_stream.h"

// The encoders at work and how the canvas and the mixed sound reach them.
// Outputs asking for the same settings share one encoder. Every picture goes
// to each open encoder; the sound goes from each one's start to the end of
// its last picture, so a stop never cuts the sound short or runs it long.
// The frame loop and the audio worker call in from their own threads.
class Encodings {
public:
    Encodings() = default;
    ~Encodings();

    // An open encoder with these settings, or a new one. Each acquire is
    // matched by a release.
    std::shared_ptr<EncodedStream> acquire(const EncodedStream::Settings &settings, std::string *error,
                                           bool *shared = nullptr);
    // When nothing uses it any more: no more pictures, the sound runs on to
    // the end of the last one, then it flushes and closes, off this thread.
    void release(const std::shared_ptr<EncodedStream> &stream);
    // The end of the last picture handed to it (monotonic): where an output
    // stopping now ends.
    int64_t pictureEnd(const std::shared_ptr<EncodedStream> &stream) const;
    // Waits for every closing encoder to finish.
    void waitClosed();
    // Whether any encoder is open or still closing.
    bool busy() const;

    // Which pictures the open encoders take: format, colour and size, each once.
    struct Layout {
        engine::OutputColour colour;
        int width = 0, height = 0;
        bool operator==(const Layout &) const = default;
    };
    struct Wanted {
        std::vector<Layout> layouts;
        bool any() const { return !layouts.empty(); }
    };
    Wanted wanted() const;
    // One picture in one layout and size, to every open encoder that takes it
    // (`colour` is what it was read out as).
    // `durationNs` is one frame: the sound runs to the end of it.
    void deliverPicture(engine::YuvFrame &&frame, const engine::OutputColour &colour, int64_t timeNs, int64_t durationNs);
    // The hardware device the engine composites on (AVHWDeviceContext data;
    // null when it has none of FFmpeg's): a Vulkan encoder on it takes its
    // pictures on the GPU instead of read back, so it is in no layout.
    void setGpuDevice(const void *device);
    struct GpuTarget {
        std::shared_ptr<EncodedStream> stream;
        Layout layout;
    };
    std::vector<GpuTarget> gpuTargets() const;
    // A picture the engine drew into the encoder's own frame.
    void deliverGpuFrame(const std::shared_ptr<EncodedStream> &stream, struct AVFrame *frame, int64_t timeNs,
                         int64_t durationNs);
    // The engine could not draw into its frames: it takes read-back pictures
    // from now on.
    void refuseGpu(const std::shared_ptr<EncodedStream> &stream);

    // Which mixer tracks the open encoders take (bit mask).
    unsigned tracksWanted() const;
    // `frames` of each track starting at `whenNs`; tracks[t] may be null for
    // tracks nobody takes.
    void deliverAudio(float *const tracks[6], int frames, int64_t whenNs, int sampleRate, int channels);
    // Mixing has reached this instant: closing encoders whose sound is
    // complete may finish.
    void audioReached(int64_t mixedEndNs);

    // Gets each encoded picture's buffer back for reuse.
    std::function<void(engine::YuvFrame &&)> onFrameDone;
    // An encoder gave up; every output on it has failed. On its thread.
    std::function<void(const std::weak_ptr<EncodedStream> &, const std::string &)> onError;

private:
    struct Entry {
        std::shared_ptr<EncodedStream> stream;
        int users = 0;
        int64_t endNs = 0;          // end of the last picture handed over
        bool closing = false;
        bool audioDone = false;     // the sound has reached endNs
        bool finished = false;      // closed; the closer can be joined
        std::thread closer;
    };
    mutable std::mutex m_mutex;
    std::condition_variable m_audioFinished;
    std::vector<std::unique_ptr<Entry>> m_entries;
    int64_t m_mixedEndNs = 0;
    const void *m_gpuDevice = nullptr;
    std::vector<const EncodedStream *> m_gpuRefused;
    bool onGpu(const EncodedStream &stream) const;  // under m_mutex
    void reap();                    // under m_mutex: joins finished closers
};
