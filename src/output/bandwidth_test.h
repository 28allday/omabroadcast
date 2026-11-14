#pragma once

#include <QObject>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// How fast this machine can upload to a streaming server, measured on the
// real path: the same StreamSink a stream uses (same protocols, reconnect
// off), to the server the stream would use.
//
// It runs only where the test cannot go live: a service that documents a test
// mode (StreamService::testKeySuffix, added to the key), or a custom server,
// which is the person's own.
//
// The method. No hardware encoder is opened here (that would stall the frame
// thread): the software H.264 encoder makes, once and in memory, one second
// of a busy 1280×720 picture at 30 frames a second for each step of a ladder
// of bitrates (2.5 Mb/s up by half each time to maxKbps, or 20 Mb/s when none
// is given), each second starting on a keyframe, with a second of quiet AAC
// sound (Opus where the protocol needs it). Those seconds are sent looped and
// re-timed, paced in real time as a live stream is, starting on the lowest
// step and moving up a step every seconds/steps (at least 1.5 s). The socket's
// send buffer is kept to about a tenth of a second at the top rate where the
// protocol takes the option, so a link that cannot keep up shows within a
// step rather than after seconds of filling the buffer.
//
// A step passes when, through it, the sink dropped nothing, stayed under
// 150 ms behind by its end (350 ms at worst in its second half) and never
// stalled for 400 ms. The ramp stops at the first step that fails. Once
// sending falls 100 ms behind and stays at least 50 ms behind, the socket is
// full and what it takes from then on is what the link carries: when a step
// fails after at least 0.3 s of that (or of the time since it first fell
// behind, when one big picture took it straight from caught up to failing),
// the rate through that stretch is measuredKbps (even below a step that
// passed while its buffer was still filling). Otherwise measuredKbps is the
// highest rate that passed, or what the failing step got through in its
// second half if that is more. recommendedKbps is three quarters of it, kept
// to the service's ceiling. A server that refuses, or does not answer, ends
// it with ok false and the reason.
class BandwidthTest : public QObject {
    Q_OBJECT
public:
    struct Settings {
        std::string serviceId;       // "custom" or "" for one's own server
        std::string server;          // empty for a service: its first server
        std::string key;
        std::string user, password;
        int maxKbps = 0;             // the top of the ramp; 0 = 20 Mb/s
        int seconds = 10;            // the ramp's length, once connected
        // As the stream's target has them, so the test takes the same road.
        std::string muxerOptions;    // "key=value …" for the connection and container
        std::string bindAddress;     // "" = any
        int ipFamily = 0;            // 4 or 6 for only that; 0 = either
    };
    struct Result {
        bool ok = false;
        std::string error;           // for people, when not ok
        int measuredKbps = 0;
        int recommendedKbps = 0;
        double droppedFraction = 0;  // of the pictures sent over the whole test
        int connectMs = 0;           // from the first packet to live
    };

    explicit BandwidthTest(QObject *parent = nullptr);
    ~BandwidthTest() override;

    // Whether a test can run for this service without going live; why not,
    // for people, when it cannot.
    static bool canTest(const std::string &serviceId, std::string *why);
    // Where the test sends: the server with the key (and the service's test
    // mode on it, exactly once) and any sign-in. Empty, with why, when it
    // cannot be tested or built.
    static std::string testUrl(const Settings &settings, std::string *why);
    // The bitrates it steps through, kb/s, lowest first.
    static std::vector<int> steps(int maxKbps);

    // Starts (stopping one already running); progress() as it goes, then
    // finished() once.
    void start(const Settings &settings);
    // Stops at once; finished() follows with ok false.
    void cancel();
    bool running() const { return m_running; }
    Result result() const { return m_result; }

signals:
    // fraction of the ramp done (0 while connecting), and the rate it sends at.
    void progress(double fraction, int currentKbps);
    void finished(const BandwidthTest::Result &result);

private:
    void run(Settings settings, std::string url, unsigned serial);
    void post(unsigned serial, double fraction, int kbps);
    void done(unsigned serial, const Result &result);
    void join();

    std::thread m_thread;
    std::atomic<bool> m_cancel{false};
    bool m_running = false;
    unsigned m_serial = 0;
    Result m_result;
};
