#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "encoders.h"
#include "engine/frame.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

// How the video encoder spends its bits, in terms that mean the same thing on
// every encoder family; encoderVideoOptions() maps it onto each one.
struct VideoRate {
    enum class Mode { Quality, Cbr, Vbr, Cqp, Lossless };
    Mode mode = Mode::Quality;
    int quality = 23;          // CRF / CQ / QP scale, lower is better
    int bitrateKbps = 6000;    // CBR and VBR target
    int maxKbps = 0;           // VBR peak; 0 = 1.5 × target
    int bufferKbps = 0;        // decoder buffer; 0 = one second of the peak
    int keyframeSeconds = 2;   // 0 = encoder default
    int bFrames = 0;
    std::string preset;        // "" = ours; else the encoder's own preset name
    std::string profile;       // "" = encoder default; e.g. "high", "main"
    std::string tune;          // "" = ours
    int lookahead = 0;         // frames; 0 = off
    // Where the encoder can split each picture across its engines (its
    // split_encode_mode): one of that option's own values; "" = its choice.
    std::string splitEncode;
    std::string options;       // "key=value key=value", the encoder's own options, last word

    bool operator==(const VideoRate &) const = default;
};

enum class AudioCodec { Aac, Opus, Flac, Alac, Pcm16, Pcm24, PcmFloat };
const char *audioCodecName(AudioCodec codec);         // "aac", "opus", "flac", …
AudioCodec audioCodecFromName(const std::string &name); // unknown → AAC

// What one set of encoders produces: the streams every sink muxes or sends.
// Codec parameters are filled once the encoders are open and never change.
struct EncodedLayout {
    AVCodecParameters *video = nullptr;
    AVRational videoTimeBase{1, 60};
    AVRational frameRate{60, 1};
    struct Audio {
        int track = 0;          // mixer track, 0–5
        AVCodecParameters *params = nullptr;
        AVRational timeBase{1, 48000};
    };
    std::vector<Audio> audio;
    ~EncodedLayout();
};

// One encoded packet: index 0 is video, 1… the layout's audio streams in
// order. Times are in the stream's time base from the encoder's own zero;
// ptsNs/endNs are the same instant in nanoseconds, for sinks that keep time.
struct EncodedPacket {
    std::shared_ptr<AVPacket> packet;
    int stream = 0;
    bool key = false;
    int64_t ptsNs = 0, endNs = 0;
    int64_t madeNs = 0;   // monotonic: when the encoder handed it over
};

// Takes encoded packets. Called on the encoder thread: queue and return, never
// block, or every other output on the same encoder waits too.
class PacketSink {
public:
    virtual ~PacketSink() = default;
    virtual void begin(std::shared_ptr<const EncodedLayout> layout) = 0;
    virtual void packet(const EncodedPacket &packet) = 0;
    // The encoder flushed and stopped: nothing more is coming.
    virtual void end() = 0;
    // Whether it wants no more packets (it finished, or gave up).
    virtual bool done() const = 0;
};

// The canvas and its audio tracks encoded once at one set of settings, for
// every output that wants exactly that. Video frames arrive from the engine
// thread, audio from the mixer; its own thread encodes and hands packets to
// each sink. A full video queue drops frames rather than stalling the
// compositor.
class EncodedStream {
public:
    struct Settings {
        EncoderInfo encoder;
        int width = 0, height = 0;
        int fpsNum = 60, fpsDen = 1;
        // Colour space, range and levels; the format is the encoder's swFormat,
        // which the pictures pushed must be in (see output()).
        engine::OutputColour colour;
        VideoRate video;
        unsigned audioTracks = 1;  // bit mask of up to six independently encoded tracks
        AudioCodec audioCodec = AudioCodec::Aac;
        int audioBitrateKbps = 160;
        int sampleRate = 48000;
        int channels = 2;
        // The mixer's channels where it has more than the encoder takes: the
        // sound is folded down to `channels` here. 0 = the same.
        int sourceChannels = 0;
        int inputChannels() const { return sourceChannels > 0 ? sourceChannels : channels; }
        // Monotonic; video pts and audio sample 0 count from here. 0 starts the
        // clock once the encoders are open, so the time they take to open (and
        // any stall it causes) is not inside the output.
        int64_t startTimeNs = 0;

        // Whether two outputs asking for these can share one encoder.
        bool sameEncoding(const Settings &o) const;
        // The pictures it takes: `colour` in the encoder's format.
        engine::OutputColour output() const;
    };
    struct Stats {
        uint64_t framesEncoded = 0;
        uint64_t framesDropped = 0;   // the encoder fell behind the canvas
        uint64_t gpuFrames = 0;       // of those encoded, how many came drawn on the GPU (no readback)
        double encodeMs = 0;          // rolling average per frame
        int64_t durationNs = 0;
    };

    EncodedStream();
    ~EncodedStream();

    bool start(const Settings &settings, std::string *error);
    // The time zero, valid after a successful start().
    int64_t startTimeNs() const { return m_startNs; }
    const Settings &settings() const { return m_settings; }
    std::shared_ptr<const EncodedLayout> layout() const { return m_layout; }
    // Flushes the encoders and ends every sink.
    void stop();
    bool running() const { return m_running.load(); }

    // A sink joining a running encoder starts at its first keyframe from
    // `fromNs` (on the encoder's clock), which it asks for so it need not wait
    // a whole interval.
    void attach(std::shared_ptr<PacketSink> sink, int64_t fromNs = 0);
    void detach(const PacketSink *sink);
    // A keyframe on the first picture at or after this instant (encoder
    // clock); 0 is the next picture.
    void requestKeyframe(int64_t atNs = 0);
    // Changes the bitrate of a CBR/VBR encoding from the next frame, where
    // the encoder can (encoderFollowsBitrate); false where it cannot, and
    // while another output shares it, which would be lowered too.
    bool setBitrate(int kbps);

    // Takes the frame. `timeNs` is the monotonic capture time.
    void pushVideo(engine::YuvFrame &&frame, int64_t timeNs);
    // A Vulkan encoder on the engine's own device takes its pictures on the
    // GPU: an empty frame from its pool (null when it has none), which the
    // engine fills and hands back with pushGpuFrame, which owns it from then
    // on. hardwareDevice() is the device context (AVHWDeviceContext data) it
    // runs on, to tell whether it is the engine's. Safe while it closes.
    const void *hardwareDevice() const;
    struct AVFrame *newGpuFrame();
    void pushGpuFrame(struct AVFrame *frame, int64_t timeNs);
    // Interleaved float in the input's channels, consecutive frames as the
    // mixer pulls them.
    void pushAudio(const float *interleaved, int frames, int track = 0);

    Stats stats() const;
    // Why it gave up; empty while it runs or after a clean stop.
    std::string failure() const;
    // Told when an encoder gives up.
    std::function<void(const std::string &)> onError;
    // Gets each encoded frame's buffer back for reuse.
    std::function<void(engine::YuvFrame &&)> onFrameDone;

    struct Impl;

private:
    void run();
    void dropQueued();  // frees what is still queued

    Impl *d;
    struct AVBufferRef *m_gpuFrames = nullptr;  // its Vulkan frame pool, kept until destroyed
    const void *m_gpuDevice = nullptr;
    Settings m_settings;
    int64_t m_startNs = 0;
    std::shared_ptr<EncodedLayout> m_layout;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::mutex m_keyMutex;
    std::vector<int64_t> m_keyAt;      // pending keyframe requests
    std::atomic<int> m_bitrateKbps{0};
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    struct Queued {
        engine::YuvFrame frame;
        int64_t timeNs;
        struct AVFrame *gpu = nullptr;  // instead of `frame`
    };
    std::deque<Queued> m_video;
    std::array<std::vector<float>, 6> m_audio;
    Stats m_stats;
    std::string m_failure;             // with m_mutex
    std::mutex m_sinkMutex;
    std::vector<std::shared_ptr<PacketSink>> m_sinks;
    bool m_sinksEnded = false;         // with m_sinkMutex: a sink attached now ends at once
};

// "key=value key=value" into pairs; quoting with "" keeps spaces.
std::vector<std::pair<std::string, std::string>> parseEncoderOptions(const std::string &text);

// The kept FFmpeg hardware device of this type on this device (see below),
// made now if need be, then with these extra device extensions ('+' between
// them). A new reference, or null with `result` set.
AVBufferRef *keptHardwareDevice(AVHWDeviceType type, const std::string &device, const char *extensions, int *result);

// Makes the FFmpeg hardware device an encoder needs, on a thread of its own,
// so the output that opens it next finds it ready. Outputs keep these devices:
// making one (FFmpeg's own Vulkan instance and device) takes about half a
// second, and on NVIDIA's driver every other Vulkan submission in the process
// waits for it meanwhile, the frame loop's among them.
void warmHardwareDevice(const EncoderInfo &encoder);

// Lets go of the kept devices `keep` says no to: those of a GPU the picture
// has moved off, which an open device would hold awake (a laptop's discrete
// card on battery). An output still using one holds its own reference; the
// next one to want it makes it again.
void releaseHardwareDevices(const std::function<bool(AVHWDeviceType type, const std::string &device)> &keep);
// The devices kept now, by type and device.
std::vector<std::pair<AVHWDeviceType, std::string>> keptHardwareDevices();
