#include "audio_playback.h"
#include "pipewire_format.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

struct AudioPlayback::Impl {
    static constexpr uint64_t Capacity = 4800; // at most 109 ms at 44.1 kHz
    AudioPlayback *owner = nullptr;
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_stream *stream = nullptr;
    spa_hook listener{};
    std::array<float, Capacity * 8> ring{};
    std::atomic<uint64_t> read{0}, write{0}, dropped{0}, underruns{0};
    std::atomic<int> quantum{0};
    std::atomic<bool> fed{false};
    std::mutex producerMutex; // only lifecycle and non-realtime producer
    mutable std::mutex errorMutex;
    std::string error;
    int latencyFrames = 480;
    AudioFormat format;
    std::string node = "omabroadcast-monitor", description = "OmaBroadcast monitoring";
    bool accepting = false; // producerMutex

    static void process(void *data) {
        auto *d = static_cast<Impl *>(data);
        const int channels = d->format.channels;
        pw_buffer *b = pw_stream_dequeue_buffer(d->stream);
        if (!b) return;
        if (b->buffer && b->buffer->n_datas) {
            spa_data &plane = b->buffer->datas[0];
            if (plane.data && plane.chunk) {
                const uint64_t capacity = plane.maxsize / (channels * sizeof(float));
                const uint64_t frames = b->requested ? std::min<uint64_t>(b->requested, capacity) : capacity;
                auto *out = static_cast<float *>(plane.data);
                const uint64_t read = d->read.load(std::memory_order_relaxed);
                const uint64_t count = std::min(frames, d->write.load(std::memory_order_acquire) - read);
                for (uint64_t i = 0; i < count; ++i)
                    for (int c = 0; c < channels; ++c) out[i * channels + c] = d->ring[((read + i) % Capacity) * channels + c];
                std::fill(out + count * channels, out + frames * channels, 0.f);
                d->quantum.store(int(frames));
                if (d->fed.load()) d->underruns.fetch_add(frames - count);
                d->read.store(read + count, std::memory_order_release);
                plane.chunk->offset = 0;
                plane.chunk->flags = 0;
                plane.chunk->stride = channels * sizeof(float);
                plane.chunk->size = uint32_t(frames * channels * sizeof(float));
                b->size = frames;
            }
        }
        pw_stream_queue_buffer(d->stream, b);
    }

    static void stateChanged(void *data, pw_stream_state, pw_stream_state state, const char *why) {
        auto *d = static_cast<Impl *>(data);
        d->owner->m_running.store(state == PW_STREAM_STATE_STREAMING);
        if (state == PW_STREAM_STATE_ERROR) {
            std::lock_guard<std::mutex> lock(d->errorMutex);
            d->error = why ? why : "Monitoring stream failed";
        }
        if (d->owner->onStateChanged) d->owner->onStateChanged();
    }
};

AudioPlayback::AudioPlayback() : d(new Impl) { d->owner = this; pw_init(nullptr, nullptr); }
AudioPlayback::~AudioPlayback() { stop(); delete d; }

bool AudioPlayback::start(const std::string &target, std::string *error) {
    stop();
    if (target.empty()) { if (error) *error = "No monitoring output selected"; return false; }
    { std::lock_guard<std::mutex> lock(d->errorMutex); d->error.clear(); }
    d->loop = pw_thread_loop_new("omabroadcast-monitor", nullptr);
    d->context = pw_context_new(pw_thread_loop_get_loop(d->loop), nullptr, 0);
    if (pw_thread_loop_start(d->loop) < 0) { if (error) *error = "Monitoring loop did not start"; stop(); return false; }
    pw_thread_loop_lock(d->loop);
    d->core = pw_context_connect(d->context, nullptr, 0);
    if (!d->core) {
        pw_thread_loop_unlock(d->loop);
        if (error) *error = "No PipeWire server";
        stop(); return false;
    }
    auto *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Production", PW_KEY_NODE_NAME, d->node.c_str(),
        PW_KEY_APP_ID, "org.omarchy.OmaBroadcast", PW_KEY_TARGET_OBJECT, target.c_str(),
        "state.restore-props", "false", "state.restore-target", "false",
        "node.dont-fallback", "true", "node.dont-reconnect", "true",
        PW_KEY_NODE_LATENCY, (std::to_string(d->latencyFrames) + "/" + std::to_string(d->format.rate)).c_str(), nullptr);
    d->stream = pw_stream_new(d->core, d->description.c_str(), props);
    static const pw_stream_events events = [] {
        pw_stream_events e{}; e.version = PW_VERSION_STREAM_EVENTS;
        e.process = Impl::process; e.state_changed = Impl::stateChanged; return e;
    }();
    pw_stream_add_listener(d->stream, &d->listener, &events, d);
    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
    auto info = pipeWireFormat(d->format);
    const spa_pod *params[]{spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
    const int result = pw_stream_connect(d->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1);
    pw_thread_loop_unlock(d->loop);
    if (result < 0) { if (error) *error = spa_strerror(result); stop(); return false; }
    { std::lock_guard<std::mutex> lock(d->producerMutex); d->accepting = true; }
    return true;
}

void AudioPlayback::setName(const std::string &node, const std::string &description) {
    d->node = node;
    d->description = description;
}

void AudioPlayback::stop() {
    std::lock_guard<std::mutex> lock(d->producerMutex);
    d->accepting = false;
    if (d->loop) {
        pw_thread_loop_lock(d->loop);
        if (d->stream) { pw_stream_disconnect(d->stream); pw_stream_destroy(d->stream); d->stream = nullptr; }
        if (d->core) { pw_core_disconnect(d->core); d->core = nullptr; }
        pw_thread_loop_unlock(d->loop);
        pw_thread_loop_stop(d->loop);
        pw_context_destroy(d->context); d->context = nullptr;
        pw_thread_loop_destroy(d->loop); d->loop = nullptr;
    }
    d->read.store(0); d->write.store(0); d->dropped.store(0); d->underruns.store(0); d->fed.store(false);
    m_running.store(false);
}

void AudioPlayback::setLatencyFrames(int frames) {
    if (d->loop) pw_thread_loop_lock(d->loop);
    d->latencyFrames = std::clamp(frames, 32, 4800);
    const std::string value = std::to_string(d->latencyFrames) + "/" + std::to_string(d->format.rate);
    const spa_dict_item item{PW_KEY_NODE_LATENCY, value.c_str()};
    const spa_dict props = SPA_DICT_INIT(&item, 1);
    if (d->stream) pw_stream_update_properties(d->stream, &props);
    if (d->loop) pw_thread_loop_unlock(d->loop);
}

void AudioPlayback::push(const float *interleaved, int frames) {
    if (!interleaved || frames <= 0) return;
    std::lock_guard<std::mutex> lock(d->producerMutex);
    if (!d->accepting || !m_running.load()) return;
    const int channels = d->format.channels;
    const uint64_t write = d->write.load(std::memory_order_relaxed);
    const uint64_t count = std::min<uint64_t>(frames, Impl::Capacity - (write - d->read.load(std::memory_order_acquire)));
    for (uint64_t i = 0; i < count; ++i)
        for (int c = 0; c < channels; ++c) {
            const float value = interleaved[i * channels + c];
            d->ring[((write + i) % Impl::Capacity) * channels + c] = std::isfinite(value) ? std::clamp(value, -1.f, 1.f) : 0.f;
        }
    if (count) d->fed.store(true);
    d->write.store(write + count, std::memory_order_release);
    d->dropped.fetch_add(uint64_t(frames) - count);
}

std::string AudioPlayback::error() const { std::lock_guard<std::mutex> lock(d->errorMutex); return d->error; }
uint64_t AudioPlayback::droppedFrames() const { return d->dropped.load(); }

uint64_t AudioPlayback::queuedFrames() const {
    // Read first: the producer can only advance write, so the difference is
    // nonnegative even while the callback is consuming frames.
    const auto read = d->read.load();
    const auto write = d->write.load();
    return write >= read ? write - read : 0;
}
uint64_t AudioPlayback::underrunFrames() const { return d->underruns.load(); }
int AudioPlayback::quantumFrames() const { return d->quantum.load(); }

bool AudioPlayback::setFormat(AudioFormat format) {
    std::lock_guard<std::mutex> lock(d->producerMutex);
    if (d->loop) return false;
    d->format = format.normalized();
    return true;
}
