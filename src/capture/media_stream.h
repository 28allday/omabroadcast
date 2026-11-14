#pragma once

#include "audio/format.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "capture/picture.h"

// A media file or a network stream, decoded on its own thread.
//
// Files play at their own pace from the moment they start (or restart), loop
// when asked, and hold their last frame when they end. A network input (SRT,
// RTMP, …) listens for its sender and shows frames as they arrive, so a phone
// streaming to it behaves like a camera; when the sender stops it listens
// again. Video is decoded on the GPU where a hardware decoder opens, in
// software otherwise; audio comes out as float PCM in the requested format stamped with
// the monotonic time it is due.
class MediaStream {
public:
    struct Options {
        std::string input;      // a path, or a URL (srt://, rtmp://, …)
        bool loop = false;      // files: start again at the end
        float volume = 1.f;
        AudioFormat audioFormat{};
        double speed = 1.0;
        bool closed = false;   // inactive sources wait without opening input
        // A listener's: what a sender must have. Never part of `input`, which is shown and saved.
        std::string passphrase;
    };
    using AudioSink = std::function<void(int64_t timeNs, const float *interleaved, int frames)>;

    MediaStream();
    ~MediaStream();

    bool start(const Options &options);
    // Waits a moment for the decoders to close. A worker the system is holding
    // (an open or a read that does not return) is left behind instead.
    void stop();
    // Files: back to the start, as if just opened.
    void restart();
    struct Playback {
        double position = 0, duration = 0, speed = 1;
        bool paused = false, seekable = false;
    };
    Playback playback() const;
    bool seek(double seconds);
    // Files: true once the decoder has done everything asked of it (the picture
    // of a seek while paused, or the file to its end) and every picture it made
    // has been taken by acquire(): nothing more comes until it is asked again.
    bool settled() const;
    bool setPaused(bool paused);
    bool setSpeed(double speed);
    void setClosed(bool closed); // close asynchronously; reopening starts at zero
    bool closed() const;
    // True while opening/decoding; false after the worker releases input resources.
    bool opened() const;

    // The newest picture due by `nowNs`, when there is one not handed out yet.
    bool acquire(Picture &out, int64_t nowNs);
    // Hands `sink` decoded audio near its due time. Keeps the newest due
    // picture when rendering is slow, so the picture queue cannot stall audio.
    void pumpAudio(int64_t nowNs, const AudioSink &sink);

    // "opening", "listening", "playing", "paused", "ended", "failed: …"
    std::string state() const;
    bool network() const;
    bool hardware() const;
    std::string codec() const;
    int width() const;
    int height() const;
    // A sound stream a listener would not take, as it plays on without: the
    // stream's codec, empty when there is none.
    std::string refusedSound() const;
    // Pictures a second, as the file says; 0 until known or without video.
    double frameRate() const;
    uint64_t framesDecoded() const;
    // Software decoders sharing the pictures (big MJPEG); 0 when there is one.
    int parallelDecoders() const;

    // Whether this input is a network URL rather than a file.
    static bool isNetwork(const std::string &input);
    // Whether it waits for a sender rather than going out to fetch from somewhere.
    static bool isListener(const std::string &input);
    // Waits, at most this long, for workers still closing, those left behind by
    // stop() among them. Gives how many are still out: one the system is holding
    // stays out, and is left to it.
    static int settle(int milliseconds);
    // For checking. How many times an input has been opened, by every stream
    // together: a seek, a step, a pause, a resume or another speed goes on in
    // the file already open and adds none. And those made to fail, as on an
    // input that cannot seek, so each is a new opening again.
    static uint64_t openings();
    static void refuseSeeksInPlace(bool refuse);

    struct Impl;

private:
    // Replaced only by stop(), on the main thread, as the frame and audio
    // threads go on asking.
    std::atomic<std::shared_ptr<Impl>> m_d;
};
