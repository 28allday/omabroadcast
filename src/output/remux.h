#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Copies a recording's picture and sound into an MP4 (or another container)
// without encoding again, keeping chapters and track titles; MP4 and MOV get
// their index at the front so they start playing at once. Refuses, naming
// the codec, rather than leave a track out. `progress` is told 0–1 and may
// return false to stop; a stopped or failed copy leaves no file behind. A
// damaged source gives what could be read of it, a true return with `note`
// saying so.
bool remuxFile(const std::string &in, const std::string &out, std::string *error,
               const std::function<bool(double)> &progress = {}, std::string *note = nullptr);
// "name.mkv" → "name.mp4" beside it, numbered when that is taken.
std::string remuxTarget(const std::string &in, const std::string &extension = "mp4");

// Remuxes one file after another on a thread of its own.
class RemuxQueue {
public:
    enum class State { Waiting, Running, Done, Failed, Cancelled };
    struct Job {
        int id = 0;
        std::string in, out;       // out is chosen when it starts, if not given
        State state = State::Waiting;
        double progress = 0;
        std::string error;
        std::string note;          // done, but only part of a damaged file
    };

    RemuxQueue() = default;
    ~RemuxQueue();

    int add(const std::string &in, const std::string &out = {});
    void cancel(int id);
    void cancelAll();
    // Forgets jobs that have ended.
    void clearFinished();
    std::vector<Job> jobs() const;
    bool busy() const;

    // Told on the queue's thread whenever a job moves on (often, while running).
    std::function<void(const Job &)> onChanged;

private:
    void run();
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<Job> m_jobs;
    int m_nextId = 1;
    int m_cancelId = 0;                // the running job, when it should stop
    bool m_quit = false;
    std::thread m_thread;
};
