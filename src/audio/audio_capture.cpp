#include "audio_capture.h"
#include "pipewire_format.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <time.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace {
int64_t monotonicNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000ll + ts.tv_nsec;
}
std::once_flag gPwInit;
std::atomic<unsigned> gStreamId{0};
}  // namespace

struct AudioCapture::Impl {
    struct Stream {
        Impl *impl = nullptr;
        int index = 0;
        pw_stream *stream = nullptr;
        spa_hook listener{};
    };

    AudioCapture *owner = nullptr;
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    Stream streams[Count];
    Sink sink;
    mutable std::mutex errorMutex;
    std::string error;
    std::string microphone;  // target name or serial; empty for none
    bool sinkMonitor = false;
    int latencyFrames = 480;
    AudioFormat format;

    static void process(void *data) {
        auto *s = static_cast<Stream *>(data);
        pw_buffer *b = pw_stream_dequeue_buffer(s->stream);
        if (!b) return;
        if (!b->buffer || b->buffer->n_datas == 0) {
            pw_stream_queue_buffer(s->stream, b);
            return;
        }
        const spa_data &d = b->buffer->datas[0];
        if (d.data && d.chunk && d.chunk->size > 0 && d.chunk->offset < d.maxsize) {
            const int channels = s->impl->format.channels;
            const uint32_t bytes = std::min(d.chunk->size, d.maxsize - d.chunk->offset);
            const int frames = int(bytes / (sizeof(float) * channels));
            const float *samples = reinterpret_cast<const float *>(static_cast<const uint8_t *>(d.data) + d.chunk->offset);
            // The buffer's graph-cycle timestamp survives delayed callbacks;
            // wall-clock arrival times can overlap adjacent chunks.
            int64_t when = b->time ? int64_t(b->time) : monotonicNs();
            pw_time t{};
            int64_t delay = 0;
            if (pw_stream_get_time_n(s->stream, &t, sizeof t) == 0 && t.rate.denom > 0)
                delay = t.delay * 1'000'000'000ll * int64_t(t.rate.num) / int64_t(t.rate.denom);
            when -= delay;
            s->impl->owner->m_quantum[s->index].store(frames);
            s->impl->owner->m_delay[s->index].store(delay);
            float peak[8]{};
            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < channels; ++c)
                    peak[c] = std::max(peak[c], std::fabs(samples[i * channels + c]));
            for (int c = 0; c < channels; ++c) s->impl->owner->m_peak[s->index][c].store(peak[c]);
            if (s->impl->sink) s->impl->sink(s->index, when, samples, frames);
        }
        pw_stream_queue_buffer(s->stream, b);
    }

    static void stateChanged(void *data, pw_stream_state, pw_stream_state state, const char *error) {
        auto *s = static_cast<Stream *>(data);
        s->impl->owner->m_running[s->index].store(state == PW_STREAM_STATE_STREAMING);
        if (state == PW_STREAM_STATE_ERROR) {
            std::lock_guard<std::mutex> lock(s->impl->errorMutex);
            s->impl->error = error ? error : "Audio stream failed";
        }
        if (s->impl->owner->onStateChanged) s->impl->owner->onStateChanged();
    }

    static const pw_stream_events &events() {
        static const pw_stream_events e = [] {
            pw_stream_events ev{};
            ev.version = PW_VERSION_STREAM_EVENTS;
            ev.process = process;
            ev.state_changed = stateChanged;
            return ev;
        }();
        return e;
    }

    bool connect(Stream &s, int index, std::string *error) {
        s.impl = this;
        s.index = index;
        const std::string nodeName = "omabroadcast-audio-" + std::to_string(++gStreamId);
        pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                                                 PW_KEY_MEDIA_ROLE, "Production", PW_KEY_NODE_NAME,
                                                 nodeName.c_str(),
                                                 PW_KEY_APP_ID, "org.omarchy.OmaBroadcast",
                                                 "state.restore-props", "false",
                                                 "state.restore-target", "false",
                                                 PW_KEY_NODE_LATENCY, (std::to_string(latencyFrames) + "/" + std::to_string(format.rate)).c_str(), nullptr);
        if (index == Desktop) {
            pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
        } else {
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, microphone.c_str());
            if (sinkMonitor) pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
            // Never stand in another input for the chosen one.
            pw_properties_set(props, "node.dont-fallback", "true");
            pw_properties_set(props, "node.dont-reconnect", "true");
        }
        s.stream = pw_stream_new(core, index == Desktop ? "Desktop audio" : "Microphone", props);
        pw_stream_add_listener(s.stream, &s.listener, &events(), &s);

        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
        auto info = pipeWireFormat(format);
        const spa_pod *params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
        // Not on the graph's realtime thread: the sink takes the mixer's lock,
        // which the mix holds while it runs, and a wait there would stall every
        // app's sound. The buffer's own time (above) keeps the stamps exact.
        const int r = pw_stream_connect(s.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                                        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                                     PW_STREAM_FLAG_MAP_BUFFERS),
                                        params, 1);
        if (r < 0) {
            if (error) *error = std::string("audio stream: ") + spa_strerror(r);
            return false;
        }
        return true;
    }
};

AudioCapture::AudioCapture() : d(new Impl) {
    d->owner = this;
    for (auto &r : m_running) r.store(false);
    for (auto &p : m_peak) for (auto &c : p) c.store(0.f);
    std::call_once(gPwInit, [] { pw_init(nullptr, nullptr); });
}

AudioCapture::~AudioCapture() {
    stop();
    delete d;
}

bool AudioCapture::start(Sink sink, std::string *error, bool captureDesktop) {
    stop();
    d->sink = std::move(sink);
    { std::lock_guard<std::mutex> lock(d->errorMutex); d->error.clear(); }
    d->loop = pw_thread_loop_new("omabroadcast-audio", nullptr);
    d->context = pw_context_new(pw_thread_loop_get_loop(d->loop), nullptr, 0);
    if (pw_thread_loop_start(d->loop) < 0) {
        if (error) *error = "PipeWire audio loop did not start";
        stop();
        return false;
    }
    pw_thread_loop_lock(d->loop);
    d->core = pw_context_connect(d->context, nullptr, 0);
    if (!d->core) {
        pw_thread_loop_unlock(d->loop);
        if (error) *error = "No PipeWire server";
        stop();
        return false;
    }
    bool ok = !captureDesktop || d->connect(d->streams[Desktop], Desktop, error);
    if (ok && !d->microphone.empty()) ok = d->connect(d->streams[Microphone], Microphone, error);
    pw_thread_loop_unlock(d->loop);
    if (!ok) stop();
    return ok;
}

void AudioCapture::setLatencyFrames(int frames) {
    frames = std::clamp(frames, 32, 4800);
    if (d->loop) pw_thread_loop_lock(d->loop);
    d->latencyFrames = frames;
    const std::string value = std::to_string(frames) + "/" + std::to_string(d->format.rate);
    const spa_dict_item item{PW_KEY_NODE_LATENCY, value.c_str()};
    const spa_dict props = SPA_DICT_INIT(&item, 1);
    for (auto &s : d->streams) if (s.stream) pw_stream_update_properties(s.stream, &props);
    if (d->loop) pw_thread_loop_unlock(d->loop);
}

void AudioCapture::setMicrophone(const std::string &nodeName) { setTarget(nodeName, false); }

void AudioCapture::setDesktop(bool capture) {
    if (!d->loop) return;
    pw_thread_loop_lock(d->loop);
    Impl::Stream &s = d->streams[Desktop];
    if (capture && !s.stream && d->core) {
        std::string why;
        if (!d->connect(s, Desktop, &why)) {
            std::lock_guard<std::mutex> lock(d->errorMutex);
            d->error = why;
        }
    } else if (!capture && s.stream) {
        pw_stream_disconnect(s.stream);
        pw_stream_destroy(s.stream);
        s.stream = nullptr;
        m_running[Desktop].store(false);
        for (auto &c : m_peak[Desktop]) c.store(0.f);
    }
    pw_thread_loop_unlock(d->loop);
}

void AudioCapture::setTarget(const std::string &nodeName, bool sinkMonitor) {
    if (!d->loop) {
        d->microphone = nodeName;
        d->sinkMonitor = sinkMonitor;
        return;
    }
    pw_thread_loop_lock(d->loop);
    if (nodeName == d->microphone && sinkMonitor == d->sinkMonitor && (nodeName.empty() || d->streams[Microphone].stream)) {
        pw_thread_loop_unlock(d->loop);
        return;
    }
    d->microphone = nodeName;
    d->sinkMonitor = sinkMonitor;
    {
        std::lock_guard<std::mutex> lock(d->errorMutex);
        d->error.clear();
    }
    Impl::Stream &s = d->streams[Microphone];
    if (s.stream) {
        pw_stream_disconnect(s.stream);
        pw_stream_destroy(s.stream);
        s.stream = nullptr;
    }
    m_running[Microphone].store(false);
    for (auto &c : m_peak[Microphone]) c.store(0.f);
    if (!nodeName.empty() && d->core) {
        std::string why;
        if (!d->connect(s, Microphone, &why)) {
            std::lock_guard<std::mutex> lock(d->errorMutex);
            d->error = why;
        }
    }
    pw_thread_loop_unlock(d->loop);
}

void AudioCapture::stop() {
    if (!d->loop) return;
    pw_thread_loop_lock(d->loop);
    for (auto &s : d->streams) {
        if (s.stream) {
            pw_stream_disconnect(s.stream);
            pw_stream_destroy(s.stream);
            s.stream = nullptr;
        }
    }
    if (d->core) {
        pw_core_disconnect(d->core);
        d->core = nullptr;
    }
    pw_thread_loop_unlock(d->loop);
    pw_thread_loop_stop(d->loop);
    pw_context_destroy(d->context);
    d->context = nullptr;
    pw_thread_loop_destroy(d->loop);
    d->loop = nullptr;
    for (auto &r : m_running) r.store(false);
}

std::string AudioCapture::error() const {
    std::lock_guard<std::mutex> lock(d->errorMutex);
    return d->error;
}

bool AudioCapture::setFormat(AudioFormat format) {
    if (d->loop) return false;
    d->format = format.normalized();
    return true;
}
