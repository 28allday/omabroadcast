#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "encoded_stream.h"

// A sink with its own thread: the encoder only queues, and the slow part
// (disk, network) happens here. Subclasses take packets in order in
// handle() and are told once, in finish(), that nothing more is coming.
// Their destructors call stopThread() first: the thread calls into them.
class QueuedSink : public PacketSink {
public:
    ~QueuedSink() override { stopThread(); }

    void begin(std::shared_ptr<const EncodedLayout> layout) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_layout = std::move(layout);
    }
    void packet(const EncodedPacket &p) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_ended || m_overflowed) return;
            if (m_queuedBytes + size_t(p.packet->size) > kMaxQueuedBytes) m_overflowed = true;
            else {
                m_queue.push_back(p);
                m_queuedBytes += size_t(p.packet->size);
            }
        }
        m_wake.notify_one();
    }
    // What it may hold before it gives up: a sink this far behind (a disk
    // too slow, a very long delay) would otherwise take all the memory.
    static constexpr size_t kMaxQueuedBytes = size_t(4) << 30;
    void end() override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ended = true;
        }
        m_wake.notify_one();
    }
    bool done() const override { return m_done.load(); }

    // Bytes waiting to be handled: how far behind the slow part is.
    size_t queuedBytes() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queuedBytes;
    }
    // Told on the sink's thread once it has closed up, however it ended.
    std::function<void()> onFinished;
    // Waits for the thread to finish (after end() or once the sink is done).
    void join() {
        if (m_thread.joinable()) m_thread.join();
    }

protected:
    void startThread() {
        m_done.store(false);
        m_thread = std::thread([this] { run(); });
    }
    // Ends now: what is queued is dropped.
    void stopThread() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ended = true;
            m_abandon = true;
        }
        m_wake.notify_one();
        join();
    }
    // Wakes handle()'s caller to look at its own state (a stop, a split).
    void poke() { m_wake.notify_one(); }
    // Stops taking packets: done() turns true and the thread finishes.
    void markDone() { m_done.store(true); }
    std::shared_ptr<const EncodedLayout> layout() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_layout;
    }

    virtual void handle(const EncodedPacket &p) = 0;
    // Called with nothing queued, after a poke or a timeout: time-based work.
    virtual void idle() {}
    // Nothing more is coming; close up.
    virtual void finish() = 0;
    // It fell kMaxQueuedBytes behind and takes no more (on its thread):
    // says why and stops; finish() follows.
    virtual void overflow() { markDone(); }
    // How long idle() may wait between calls; 0 waits for packets only.
    int idleMs = 0;

private:
    void run() {
        for (;;) {
            EncodedPacket p;
            bool have = false;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                const auto ready = [&] { return m_abandon || m_ended || !m_queue.empty() || m_done.load() || m_overflowed; };
                if (idleMs > 0) m_wake.wait_for(lock, std::chrono::milliseconds(idleMs), ready);
                else m_wake.wait(lock, ready);
                if (m_abandon) break;
                if (m_overflowed && !m_overflowTold) {
                    m_overflowTold = true;
                    lock.unlock();
                    overflow();
                    continue;
                }
                if (!m_queue.empty()) {
                    p = std::move(m_queue.front());
                    m_queue.pop_front();
                    m_queuedBytes -= size_t(p.packet->size);
                    have = true;
                } else if (m_ended || m_done.load()) {
                    break;
                }
            }
            if (have && !m_done.load()) handle(p);
            else if (!have) idle();
            if (m_done.load()) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_queue.clear();
                m_queuedBytes = 0;
            }
        }
        finish();
        m_done.store(true);
        if (onFinished) onFinished();
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<EncodedPacket> m_queue;
    size_t m_queuedBytes = 0;
    bool m_ended = false, m_abandon = false;
    bool m_overflowed = false, m_overflowTold = false;
    std::atomic<bool> m_done{false};
    std::shared_ptr<const EncodedLayout> m_layout;
    std::thread m_thread;
};
