#pragma once

#include <functional>
#include <memory>
#include <string>

#include "encoded_stream.h"
#include "file_sink.h"

// Writes one file: the canvas as it is handed over, plus the mixed audio. One
// encoder and one file sink, for callers that need nothing more.
class Recorder {
public:
    struct Settings {
        std::string path;
        std::string format;        // a fileFormats() id; empty = from the extension
        EncoderInfo encoder;
        int width = 0, height = 0;
        int fpsNum = 60, fpsDen = 1;
        engine::OutputColour colour; // space, range, levels; the format is encoder.swFormat
        int quality = 23;          // CRF / CQ scale, lower is better
        int bitrateKbps = 0;       // > 0 switches to a bitrate target
        int audioBitrateKbps = 160;
        bool audio = true;
        unsigned audioTracks = 1; // bit mask of up to six independently encoded tracks
        int sampleRate = 48000;
        int channels = 2;
        // Monotonic; video pts and audio sample 0 count from here. 0 starts the
        // clock once the encoders are open, so the time they take to open (and
        // any stall it causes) is not inside the recording.
        int64_t startTimeNs = 0;
    };
    struct Stats {
        uint64_t framesEncoded = 0;
        uint64_t framesDropped = 0;
        uint64_t bytesWritten = 0;
        int64_t durationNs = 0;
        double encodeMs = 0;       // rolling average per frame
        uint64_t gpuFrames = 0;    // of those encoded, drawn on the GPU (no readback)
    };

    Recorder() = default;
    ~Recorder();

    bool start(const Settings &settings, std::string *error);
    // The recording's time zero, valid after a successful start().
    int64_t startTimeNs() const { return m_stream.startTimeNs(); }
    void stop();
    bool recording() const { return m_stream.running(); }

    // Takes the frame. `timeNs` is the monotonic capture time.
    void pushVideo(engine::YuvFrame &&frame, int64_t timeNs) { m_stream.pushVideo(std::move(frame), timeNs); }
    // Interleaved float, consecutive frames as the mixer pulls them.
    void pushAudio(const float *interleaved, int frames, int track = 0) { m_stream.pushAudio(interleaved, frames, track); }

    Stats stats() const;
    // Set to be told when the recorder gives up mid-file.
    std::function<void(const std::string &)> onError;
    // Gets each encoded frame's buffer back for reuse.
    std::function<void(engine::YuvFrame &&)> onFrameDone;

    // Expand %Y %y %m %d %H %M %S and "%%" in a filename template.
    static std::string expandTemplate(const std::string &tmpl, int64_t unixSeconds);
    // The format a path's extension names; Matroska when none does.
    static std::string formatForPath(const std::string &path);

private:
    EncodedStream m_stream;
    std::shared_ptr<FileSink> m_file;
};
