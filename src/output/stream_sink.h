#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <string>

#include "queued_sink.h"

struct AVFormatContext;
struct AVBSFContext;

// Sends encoded packets to a server: RTMP and RTMPS as FLV, SRT as MPEG-TS,
// WHIP as WebRTC, HLS as uploaded segments. It connects, and
// reconnects after a drop, on its own thread; an encoder it shares with a
// recording never waits on the network. When the network falls behind it
// drops pictures up to the next keyframe rather than letting the delay grow,
// and can ask for a lower bitrate.
class StreamSink : public QueuedSink {
public:
    // Custom is any FFmpeg muxer to any address FFmpeg can write to.
    enum class Protocol { Rtmp, Srt, Whip, Hls, Custom };
    struct Settings {
        std::string url;
        // What the URL is spoken in; guessed from it when not given.
        bool protocolGiven = false;
        Protocol protocol = Protocol::Rtmp;
        std::string bearerToken;     // WHIP
        std::string muxer;           // Custom: FFmpeg's name for the container
        // "key=value …", the container's own options, set after ours; one
        // nothing takes fails the connection by name.
        std::string muxerOptions;
        std::string bindAddress;     // the local address to send from; "" = any
        int64_t clockZeroNs = 0;     // the encoder's zero, monotonic
        int64_t startNs = 0;         // encoder time it starts from
        int delaySeconds = 0;        // held back this long before sending
        // With a delay: what arrives while reconnecting is kept, and sending
        // carries on from the first keyframe after the cut, the delay growing
        // by the outage, rather than skipping to now. Up to keepBytes.
        bool preserveDelay = false;
        size_t keepBytes = size_t(1) << 30;
        int ipFamily = 0;            // 4 or 6 to use only that; 0 = either
        bool lowLatency = false;     // send each packet at once (TCP_NODELAY)
        bool reconnect = true;
        int retryDelaySeconds = 2;   // the first wait; later ones grow
        int maxRetries = 20;         // 0 = for ever
        int dropAfterMs = 700;       // behind by this much: drop to a keyframe
        int timeoutSeconds = 10;     // connecting or a stalled send
        bool dynamicBitrate = false;
        int bitrateKbps = 0;         // what the encoder was asked for
    };
    enum class State { Connecting, Live, Reconnecting, Stopped, Failed };
    struct Stats {
        State state = State::Connecting;
        std::string message;         // the last error, for people
        uint64_t bytes = 0;
        double kbps = 0;             // over the last couple of seconds
        uint64_t framesSent = 0, framesDropped = 0;
        int reconnects = 0;
        int lagMs = 0;               // how far behind sending is
        int bitrateKbps = 0;         // the current target, with dynamic bitrate
        bool bitrateHeld = false;    // it asked for another and the encoder could not follow
        int delayMs = 0;             // the delay now: more after a preserved reconnect
        uint64_t heldBytes = 0;      // waiting to go out (the delay and any outage)
    };

    StreamSink();
    ~StreamSink() override;

    // Which protocol a URL is in; false when it names none we speak.
    static bool protocolFor(const std::string &url, Protocol *protocol);
    static const char *protocolName(Protocol protocol);
    // Whether this FFmpeg can send it.
    static bool protocolAvailable(Protocol protocol);
    // What the protocol can carry, or why not; Custom asks the muxer.
    static bool carries(Protocol protocol, AVCodecID video, AVCodecID audio, std::string *why,
                        const std::string &muxer = {});

    // Checks the settings now; connecting happens on the sink's thread.
    bool start(const Settings &settings, std::shared_ptr<const EncodedLayout> layout, std::string *error);
    // Stops sending at this instant (encoder clock) and closes cleanly; a
    // stream not yet live stops at once.
    void finishAt(int64_t ns);

    Stats stats() const;
    Protocol protocol() const { return m_settings.protocol; }
    std::function<void(State)> onState;
    // Asked for a keyframe on the first picture at or after this instant.
    std::function<void(int64_t)> wantKeyframe;
    // Dynamic bitrate: the target it now wants; false when the encoder cannot
    // follow, and the target stays what it opened at.
    std::function<bool(int)> onBitrate;

protected:
    void handle(const EncodedPacket &p) override;
    void idle() override;
    void finish() override;
    void overflow() override;

private:
    bool connect(std::string *error);
    void disconnect(bool cleanly);
    void lost(const std::string &why);
    bool send(const EncodedPacket &p);
    // Waits for the delay, drops when behind, sends; false when the connection went.
    bool deliver(const EncodedPacket &p);
    bool preserving() const { return m_settings.preserveDelay && m_settings.delaySeconds > 0; }
    void keep(const EncodedPacket &p, bool front = false);
    void setState(State state, const std::string &message = {});
    void adjustBitrate(int lagMs, int64_t now);
    void askForBitrate(int kbps);
    static int interrupted(void *opaque);

    Settings m_settings;
    std::shared_ptr<const EncodedLayout> m_layout;
    AVFormatContext *m_fmt = nullptr;
    std::vector<AVBSFContext *> m_bsf;
    std::atomic<bool> m_abort{false};
    std::atomic<int64_t> m_finishAt{INT64_MAX};
    bool m_hopeless = false;         // a failure no retry would mend
    bool m_waitingKey = true;        // after a (re)connect: from a keyframe
    int64_t m_keyAfter = 0;
    bool m_dropping = false;
    int64_t m_zeroNs = -1;           // the stream's time zero on the encoder clock
    int64_t m_nextAttemptNs = 0;
    int m_retries = 0;
    int m_bitrateKbps = 0;
    bool m_bitrateHeld = false;
    std::deque<EncodedPacket> m_backlog;   // kept through an outage (preserveDelay)
    size_t m_backlogBytes = 0;
    int64_t m_cutNs = -1;            // the last picture sent
    bool m_resuming = false;         // from the cut: the delay grows to fit
    int64_t m_shiftNs = 0;           // how much it has grown
    int64_t m_congestedSinceNs = 0, m_clearSinceNs = 0, m_loweredNs = 0;
    std::vector<std::pair<int64_t, uint64_t>> m_rate;  // (time, bytes) samples
    mutable std::mutex m_statsMutex;
    Stats m_stats;
};
