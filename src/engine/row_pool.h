#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#include <sched.h>

namespace engine {

// How many threads draw, the caller included, out of `cpus` the process may
// run on. Above four, less a quarter for the encoder and capture, which have
// threads of their own. On four or fewer, all of them: they draw only for part
// of a tick, and a picture late for want of a thread costs more than the encoder
// waiting a moment (measured, 1080p60 from a clip: on four, 13 pictures of 500
// late with three threads, none with four; on two, 199 of 617 made with one, 355
// with two, the encoder dropping none). Past 16 memory is what limits a picture.
inline int drawingThreadsFor(int cpus) {
    if (cpus < 1) cpus = 1;
    return cpus <= 4 ? cpus : std::min(cpus - cpus / 4, 16);
}
// The same on this machine, as this process is placed on it.
// OMABROADCAST_CPU_THREADS overrides.
inline int drawingThreads() {
    if (const char *wanted = std::getenv("OMABROADCAST_CPU_THREADS")) {
        const int n = std::atoi(wanted);
        if (n > 0) return std::min(n, 64);
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    return drawingThreadsFor(sched_getaffinity(0, sizeof set, &set) == 0 ? CPU_COUNT(&set) : int(std::thread::hardware_concurrency()));
}

// Bands of rows on a set of threads, the caller among them. Threads start with
// the first picture big enough to share, and between bands wait a moment before
// sleeping, as a tick asks for a dozen of these in a row. Nothing thrown leaves
// a worker: the first exception stops the bands not yet begun and is thrown
// again on the caller, once every worker is out of the job.
class RowPool {
public:
    explicit RowPool(int threads) : m_threads(threads) {}
    ~RowPool() {
        m_stop.store(true);
        m_epoch.fetch_add(1);
        m_epoch.notify_all();
        for (std::thread &t : m_workers) t.join();
    }
    RowPool(const RowPool &) = delete;
    RowPool &operator=(const RowPool &) = delete;

    // body(from, to) for rows first..last, each row exactly once. `perRow` is
    // about how many pixels a row holds: too little work to be worth waking
    // anybody for stays on this thread, as does a call from inside a band.
    template <class Body>
    void run(int first, int last, int perRow, Body &&body) {
        const int rows = last - first;
        if (rows <= 0) return;
        if (m_threads <= 1 || rows == 1 || int64_t(rows) * std::max(perRow, 1) < kWorthSharing || m_busy.exchange(true)) {
            body(first, last);
            return;
        }
        using B = std::remove_reference_t<Body>;
        struct Release { std::atomic<bool> &busy; ~Release() { busy.store(false); } } release{m_busy};
        start();
        // Quite a few bands to a thread: one held up by something else on the
        // machine then costs little, as the others take what it has not begun.
        m_call = [](void *b, int from, int to) { (*static_cast<B *>(b))(from, to); };
        m_body = const_cast<void *>(static_cast<const void *>(&body));
        m_next.store(first);
        m_end = last;
        m_band = std::max(1, rows / (m_threads * 4));
        m_open.store(true);
        m_epoch.fetch_add(1);
        m_epoch.notify_all();
        take();
        // A worker counts itself in before looking at m_open, so past this it
        // is either counted, and waited for, or will find the job shut.
        m_open.store(false);
        for (int inside = m_inside.load(); inside > 0; inside = m_inside.load()) m_inside.wait(inside);
        if (m_failed) {
            std::exception_ptr failed;
            std::swap(failed, m_failed);
            std::rethrow_exception(failed);
        }
    }

private:
    using Call = void (*)(void *, int, int);
    static constexpr int64_t kWorthSharing = 32768;
    static constexpr int kSpins = 2000;  // some tens of microseconds

    void start() {
        if (m_started) return;
        m_started = true;
        // Short of threads or memory for their stacks: fewer of them still draw.
        try {
            for (int i = 1; i < m_threads; ++i) m_workers.emplace_back([this] { work(); });
        } catch (const std::system_error &) {
        } catch (const std::bad_alloc &) {
        }
    }
    void take() {
        const Call call = m_call;
        void *const body = m_body;
        const int end = m_end, band = m_band;
        for (;;) {
            const int from = m_next.fetch_add(band);
            if (from >= end) return;
            try {
                call(body, from, std::min(from + band, end));
            } catch (...) {
                m_next.store(end);
                std::lock_guard<std::mutex> lock(m_failure);
                if (!m_failed) m_failed = std::current_exception();
                return;
            }
        }
    }
    void work() {
        uint32_t seen = 0;
        for (;;) {
            uint32_t now = m_epoch.load();
            for (int i = 0; now == seen && i < kSpins; ++i, now = m_epoch.load()) {
#if defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#endif
            }
            if (now == seen) { m_epoch.wait(seen); continue; }
            seen = now;
            if (m_stop.load()) return;
            m_inside.fetch_add(1);
            if (m_open.load() && m_epoch.load() == seen) take();
            if (m_inside.fetch_sub(1) == 1) m_inside.notify_all();
        }
    }

    const int m_threads;
    bool m_started = false;
    std::vector<std::thread> m_workers;
    std::atomic<bool> m_busy{false}, m_stop{false}, m_open{false};
    std::atomic<uint32_t> m_epoch{0};
    std::atomic<int> m_next{0}, m_inside{0};
    // The job; written only with nobody inside and m_open false.
    Call m_call = nullptr;
    void *m_body = nullptr;
    int m_end = 0, m_band = 1;
    std::mutex m_failure;  // a leaf
    std::exception_ptr m_failed;
};

}  // namespace engine
