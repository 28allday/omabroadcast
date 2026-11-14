#include "replay_sink.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <mutex>

namespace {
constexpr int64_t kSecondNs = 1'000'000'000;
constexpr size_t kMaxBeforeKey = 4000;   // sound kept while waiting for the first keyframe
}  // namespace

// A file being written from the buffer; its sink tells us, from its own
// thread, how it went.
struct ReplaySink::Save {
    struct Outcome {
        std::mutex mutex;
        std::string error;
        std::atomic<bool> finished{false};
    };
    std::shared_ptr<FileSink> file;
    std::shared_ptr<Outcome> outcome = std::make_shared<Outcome>();
    Done done;
};

ReplaySink::ReplaySink() { idleMs = 100; }

ReplaySink::~ReplaySink() { stopThread(); }

void ReplaySink::start(const Settings &settings, std::shared_ptr<const EncodedLayout> layout) {
    m_settings = settings;
    // Something has to bound it.
    if (m_settings.seconds <= 0 && m_settings.bytes <= 0) m_settings.seconds = 20;
    m_layout = std::move(layout);
    startThread();
}

void ReplaySink::save(const FileSink::Settings &file, int64_t atNs, Done done) {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_requests.push_back({file, atNs, std::move(done)});
    }
    poke();
}

ReplaySink::Stats ReplaySink::stats() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_stats;
}

// Drops everything before the keyframe at `keyIndex`, except sound that runs
// past it: the sound arrives ahead of the pictures, so some of what belongs
// after the keyframe is queued before it.
void ReplaySink::dropFront(size_t keyIndex) {
    const int64_t keyNs = m_ring[keyIndex].ptsNs;
    std::vector<EncodedPacket> kept;
    for (size_t i = 0; i < keyIndex; ++i) {
        const EncodedPacket &p = m_ring[i];
        if (p.stream != 0 && p.endNs >= keyNs) kept.push_back(p);
        else m_bytes -= uint64_t(p.packet->size);
    }
    m_ring.erase(m_ring.begin(), m_ring.begin() + std::ptrdiff_t(keyIndex));
    m_ring.insert(m_ring.begin(), kept.begin(), kept.end());
}

// Lets go of whole stretches between keyframes while what is left still
// covers the time asked for, or while it is over its size.
void ReplaySink::trim() {
    for (;;) {
        size_t first = SIZE_MAX, second = SIZE_MAX;
        for (size_t i = 0; i < m_ring.size(); ++i) {
            const EncodedPacket &p = m_ring[i];
            if (p.stream != 0 || !p.key) continue;
            if (first == SIZE_MAX) first = i;
            else { second = i; break; }
        }
        if (second == SIZE_MAX) return;
        const bool coveredWithout = m_settings.seconds > 0 && m_newestNs - m_ring[second].ptsNs >= m_settings.seconds * kSecondNs;
        const bool tooBig = m_settings.bytes > 0 && m_bytes > uint64_t(m_settings.bytes);
        if (!coveredWithout && !tooBig) return;
        dropFront(second);
    }
}

void ReplaySink::startSaves() {
    std::vector<Request> requests;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        requests.swap(m_requests);
    }
    for (Request &r : requests) {
        // The latest keyframe that still covers the time wanted before the
        // save; the first one held when there is less than that.
        int64_t from = INT64_MIN;
        for (const EncodedPacket &p : m_ring) {
            if (p.stream != 0 || !p.key) continue;
            if (from == INT64_MIN || (m_settings.seconds > 0 && p.ptsNs <= r.atNs - m_settings.seconds * kSecondNs)) from = p.ptsNs;
        }
        if (from == INT64_MIN || from >= r.atNs) {
            if (r.done) r.done({}, "Nothing is held yet: the replay buffer has only just started.");
            continue;
        }
        auto save = std::make_unique<Save>();
        save->done = std::move(r.done);
        save->file = std::make_shared<FileSink>();
        std::weak_ptr<Save::Outcome> outcome = save->outcome;
        save->file->onError = [outcome](const std::string &message) {
            if (auto o = outcome.lock()) {
                std::lock_guard<std::mutex> lock(o->mutex);
                if (o->error.empty()) o->error = message;
            }
        };
        save->file->onFinished = [outcome] { if (auto o = outcome.lock()) o->finished.store(true); };
        FileSink::Settings fs = r.file;
        fs.startNs = from;
        fs.splitSeconds = 0;
        fs.splitBytes = 0;
        std::string error;
        if (!save->file->start(fs, m_layout, &error)) {
            if (save->done) save->done({}, error);
            continue;
        }
        save->file->finishAt(r.atNs);
        for (const EncodedPacket &p : m_ring) save->file->packet(p);
        m_saves.push_back(std::move(save));
    }
}

void ReplaySink::reap(bool wait) {
    for (auto it = m_saves.begin(); it != m_saves.end();) {
        Save &s = **it;
        if (!wait && !s.outcome->finished.load()) { ++it; continue; }
        s.file->join();
        const std::vector<std::string> files = s.file->closedFiles();
        std::string error;
        {
            std::lock_guard<std::mutex> lock(s.outcome->mutex);
            error = s.outcome->error;
        }
        if (files.empty() && error.empty()) error = "Nothing was written.";
        if (s.done) s.done(files.empty() ? std::string() : files.front(), error);
        it = m_saves.erase(it);
    }
}

void ReplaySink::handle(const EncodedPacket &p) {
    startSaves();
    if (!m_haveKey) {
        if (p.stream == 0) {
            // Joining a running encoder, or nothing held yet: from a keyframe.
            if (!p.key || p.ptsNs < m_settings.startNs) return;
            m_ring.push_back(p);
            m_bytes += uint64_t(p.packet->size);
            m_haveKey = true;
            dropFront(m_ring.size() - 1);
        } else {
            if (p.endNs < m_settings.startNs) return;
            m_ring.push_back(p);
            m_bytes += uint64_t(p.packet->size);
            if (m_ring.size() > kMaxBeforeKey) {
                m_bytes -= uint64_t(m_ring.front().packet->size);
                m_ring.pop_front();
            }
        }
    } else {
        m_ring.push_back(p);
        m_bytes += uint64_t(p.packet->size);
    }
    if (p.stream == 0 && m_haveKey) {
        m_newestNs = std::max(m_newestNs, p.endNs);
        if (p.key) {
            m_lastKeyNs = p.ptsNs;
            m_bytesSinceKey = 0;
            m_keyAsked = false;
        }
    }
    if (m_haveKey) m_bytesSinceKey += uint64_t(p.packet->size);
    const bool longStretch = (m_settings.seconds > 0 && m_newestNs - m_lastKeyNs >= m_settings.seconds * kSecondNs) ||
                             (m_settings.bytes > 0 && m_bytesSinceKey > uint64_t(m_settings.bytes) / 2);
    if (longStretch && !m_keyAsked && wantKeyframe) {
        m_keyAsked = true;
        wantKeyframe(0);
    }
    trim();
    for (const auto &s : m_saves)
        if (!s->file->done()) s->file->packet(p);
    reap(false);

    int64_t firstKey = INT64_MIN;
    for (const EncodedPacket &q : m_ring)
        if (q.stream == 0 && q.key) { firstKey = q.ptsNs; break; }
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_stats.heldNs = firstKey == INT64_MIN ? 0 : std::max<int64_t>(0, m_newestNs - firstKey);
    m_stats.heldBytes = m_bytes;
    m_stats.saving = int(m_saves.size());
}

void ReplaySink::idle() {
    startSaves();
    reap(false);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_stats.saving = int(m_saves.size());
}

void ReplaySink::finish() {
    // A save asked for just before the end still gets what is held.
    startSaves();
    for (const auto &s : m_saves) s->file->end();
    reap(true);
    m_ring.clear();
    m_bytes = 0;
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_stats = Stats{};
}
