#include "encodings.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cmath>

Encodings::~Encodings() {
    std::vector<std::shared_ptr<EncodedStream>> open;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto &e : m_entries) if (!e->closing) { e->users = 1; open.push_back(e->stream); }
    }
    for (auto &s : open) release(s);
    {
        // No more sound is coming: finish now.
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto &e : m_entries) e->audioDone = true;
    }
    m_audioFinished.notify_all();
    waitClosed();
}

std::shared_ptr<EncodedStream> Encodings::acquire(const EncodedStream::Settings &settings, std::string *error, bool *shared) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        reap();
        for (auto &e : m_entries) {
            if (e->closing || !e->stream->running() || !e->stream->settings().sameEncoding(settings)) continue;
            ++e->users;
            if (shared) *shared = true;
            return e->stream;
        }
    }
    if (shared) *shared = false;
    // Opening takes a while on some hardware: not under the lock the frame
    // loop and the audio worker take.
    auto stream = std::make_shared<EncodedStream>();
    stream->onFrameDone = [this](engine::YuvFrame &&frame) { if (onFrameDone) onFrameDone(std::move(frame)); };
    // Weak: the message may be handled after the encoder is gone, and another
    // can be made where it was.
    std::weak_ptr<EncodedStream> weak = stream;
    stream->onError = [this, weak](const std::string &message) { if (onError) onError(weak, message); };
    if (!stream->start(settings, error)) return nullptr;
    auto entry = std::make_unique<Entry>();
    entry->stream = stream;
    entry->users = 1;
    entry->endNs = stream->startTimeNs();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.push_back(std::move(entry));
    return stream;
}

void Encodings::release(const std::shared_ptr<EncodedStream> &stream) {
    std::lock_guard<std::mutex> lock(m_mutex);
    reap();
    for (auto &e : m_entries) {
        if (e->stream != stream || e->closing) continue;
        if (--e->users > 0) return;
        e->closing = true;
        e->audioDone = !e->stream->settings().audioTracks || m_mixedEndNs >= e->endNs || !e->stream->running();
        Entry *entry = e.get();
        e->closer = std::thread([this, entry] {
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                // The sound normally arrives within the mixer's latency; a
                // stalled mixer must not hold the file open for ever.
                m_audioFinished.wait_for(lock, std::chrono::seconds(10), [entry] { return entry->audioDone; });
            }
            entry->stream->stop();  // flushes; its sinks end
            std::lock_guard<std::mutex> lock(m_mutex);
            entry->finished = true;
        });
        return;
    }
}

int64_t Encodings::pictureEnd(const std::shared_ptr<EncodedStream> &stream) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto &e : m_entries) if (e->stream == stream) return e->endNs;
    return stream ? stream->startTimeNs() : 0;
}

void Encodings::reap() {
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if ((*it)->finished) {
            if ((*it)->closer.joinable()) (*it)->closer.join();
            std::erase(m_gpuRefused, (*it)->stream.get());
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
}

void Encodings::waitClosed() {
    std::vector<std::thread> closers;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto &e : m_entries) if (e->closer.joinable()) closers.push_back(std::move(e->closer));
    }
    for (std::thread &t : closers) t.join();
    std::lock_guard<std::mutex> lock(m_mutex);
    std::erase_if(m_entries, [](const auto &e) { return e->finished; });
}

bool Encodings::busy() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::any_of(m_entries.begin(), m_entries.end(), [](const auto &e) { return !e->finished; });
}

Encodings::Wanted Encodings::wanted() const {
    Wanted w;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto &e : m_entries) {
        if (e->closing || onGpu(*e->stream)) continue;
        const EncodedStream::Settings &s = e->stream->settings();
        const Layout l{s.output(), s.width, s.height};
        if (std::find(w.layouts.begin(), w.layouts.end(), l) == w.layouts.end()) w.layouts.push_back(l);
    }
    return w;
}

void Encodings::deliverPicture(engine::YuvFrame &&frame, const engine::OutputColour &colour, int64_t timeNs, int64_t durationNs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Entry *> targets;
    for (auto &e : m_entries)
        if (!e->closing && e->stream->settings().output() == colour &&
            e->stream->settings().width == frame.width && e->stream->settings().height == frame.height)
            targets.push_back(e.get());
    if (targets.empty()) {
        if (onFrameDone) onFrameDone(std::move(frame));
        return;
    }
    for (size_t i = 0; i < targets.size(); ++i) {
        targets[i]->stream->pushVideo(i + 1 < targets.size() ? engine::YuvFrame(frame) : std::move(frame), timeNs);
        targets[i]->endNs = timeNs + durationNs;
    }
}

bool Encodings::onGpu(const EncodedStream &stream) const {
    return m_gpuDevice && stream.settings().encoder.hwType == AV_HWDEVICE_TYPE_VULKAN && stream.hardwareDevice() == m_gpuDevice &&
           std::find(m_gpuRefused.begin(), m_gpuRefused.end(), &stream) == m_gpuRefused.end();
}

void Encodings::refuseGpu(const std::shared_ptr<EncodedStream> &stream) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (std::find(m_gpuRefused.begin(), m_gpuRefused.end(), stream.get()) == m_gpuRefused.end())
        m_gpuRefused.push_back(stream.get());
}

void Encodings::setGpuDevice(const void *device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_gpuDevice = device;
}

std::vector<Encodings::GpuTarget> Encodings::gpuTargets() const {
    std::vector<GpuTarget> out;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto &e : m_entries) {
        if (e->closing || !onGpu(*e->stream)) continue;
        const EncodedStream::Settings &s = e->stream->settings();
        out.push_back({e->stream, Layout{s.output(), s.width, s.height}});
    }
    return out;
}

void Encodings::deliverGpuFrame(const std::shared_ptr<EncodedStream> &stream, AVFrame *frame, int64_t timeNs, int64_t durationNs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto &e : m_entries) {
        if (e->stream != stream || e->closing) continue;
        e->stream->pushGpuFrame(frame, timeNs);
        e->endNs = timeNs + durationNs;
        return;
    }
    av_frame_free(&frame);
}

unsigned Encodings::tracksWanted() const {
    unsigned tracks = 0;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto &e : m_entries) if (!e->audioDone) tracks |= e->stream->settings().audioTracks;
    return tracks;
}

void Encodings::deliverAudio(float *const tracks[6], int frames, int64_t whenNs, int sampleRate, int channels) {
    // Sample timestamps in [start, end of the last picture).
    const auto boundary = [&](int64_t time) {
        return int(std::clamp<int64_t>(int64_t(std::ceil((time - whenNs) * (sampleRate / 1e9L))), 0, frames));
    };
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto &e : m_entries) {
        if (e->audioDone) continue;
        const unsigned want = e->stream->settings().audioTracks;
        const int first = boundary(e->stream->startTimeNs());
        const int end = e->closing ? boundary(e->endNs) : frames;
        if (end <= first) continue;
        for (int t = 0; t < 6; ++t)
            if ((want & (1u << t)) && tracks[t]) e->stream->pushAudio(tracks[t] + size_t(first) * channels, end - first, t);
    }
}

void Encodings::audioReached(int64_t mixedEndNs) {
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mixedEndNs = mixedEndNs;
        for (auto &e : m_entries) {
            if (!e->closing || e->audioDone) continue;
            if (mixedEndNs >= e->endNs || !e->stream->running()) {
                e->audioDone = true;
                finished = true;
            }
        }
    }
    if (finished) m_audioFinished.notify_all();
}
