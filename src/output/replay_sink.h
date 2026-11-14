#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "file_sink.h"

// Keeps the last stretch of encoded output in memory, from a keyframe, and
// writes it to a file when asked: what it holds up to the instant asked for,
// plus whatever the encoders were still working on at that instant. The file
// is written by a FileSink of its own, so a save never holds up the buffer.
class ReplaySink : public QueuedSink {
public:
    struct Settings {
        int64_t seconds = 20;               // keep at least this long; 0 = no time limit
        int64_t bytes = 512ll * 1024 * 1024; // and no more than this; 0 = no size limit
        // Packets from before this are not wanted (a sink joining a running
        // encoder starts at its first keyframe from here).
        int64_t startNs = 0;
    };
    struct Stats {
        int64_t heldNs = 0;      // from the first keyframe held to the newest picture
        uint64_t heldBytes = 0;
        int saving = 0;          // saves still being written
    };
    // Told on the sink's thread once a save is written (its path) or has failed.
    using Done = std::function<void(const std::string &path, const std::string &error)>;

    ReplaySink();
    ~ReplaySink() override;

    void start(const Settings &settings, std::shared_ptr<const EncodedLayout> layout);
    // Writes what is held up to `atNs` (encoder clock) into a new file; the
    // settings' startNs is chosen here. The last `seconds` before `atNs`, from
    // the keyframe that covers them.
    void save(const FileSink::Settings &file, int64_t atNs, Done done);
    Stats stats() const;

    // Asked for a keyframe when the newest stretch alone outgrows what it
    // keeps: only at a keyframe can the oldest part go.
    std::function<void(int64_t)> wantKeyframe;

protected:
    void handle(const EncodedPacket &p) override;
    void idle() override;
    void finish() override;

private:
    struct Request {
        FileSink::Settings file;
        int64_t atNs = 0;
        Done done;
    };
    struct Save;
    void startSaves();
    void trim();
    void reap(bool wait);
    void dropFront(size_t keyIndex);

    Settings m_settings;
    std::shared_ptr<const EncodedLayout> m_layout;
    std::deque<EncodedPacket> m_ring;  // in arrival order; the first picture is a keyframe
    bool m_haveKey = false;
    uint64_t m_bytes = 0;
    int64_t m_newestNs = INT64_MIN;    // end of the newest picture
    int64_t m_lastKeyNs = INT64_MIN;
    uint64_t m_bytesSinceKey = 0;
    bool m_keyAsked = false;
    std::vector<std::unique_ptr<Save>> m_saves;
    mutable std::mutex m_stateMutex;   // the requests and stats below
    std::vector<Request> m_requests;
    Stats m_stats;
};
