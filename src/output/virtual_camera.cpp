#include "virtual_camera.h"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace {
VirtualCamera::Format fromSpa(uint32_t f) {
    switch (f) {
    case SPA_VIDEO_FORMAT_NV12: return VirtualCamera::Format::Nv12;
    case SPA_VIDEO_FORMAT_I420: return VirtualCamera::Format::I420;
    case SPA_VIDEO_FORMAT_YUY2: return VirtualCamera::Format::Yuy2;
    default: return VirtualCamera::Format::None;
    }
}

// Bytes in one picture, and the stride of its first plane.
size_t pictureBytes(VirtualCamera::Format f, int w, int h, int *stride) {
    const size_t luma = size_t(w) * h, chroma = size_t((w + 1) / 2) * ((h + 1) / 2);
    *stride = f == VirtualCamera::Format::Yuy2 ? ((w + 1) / 2) * 4 : w;
    return f == VirtualCamera::Format::Yuy2 ? size_t(*stride) * h : luma + 2 * chroma;
}

// Either readback layout into the format the app took, planes packed
// one after another.
void writePicture(const engine::YuvFrame &in, VirtualCamera::Format f, uint8_t *out) {
    const int w = in.width, h = in.height, cw = in.chromaWidth(), ch = in.chromaHeight();
    const auto u = [&](int x, int y) { return in.nv12 ? in.u[size_t(y) * cw * 2 + size_t(x) * 2] : in.u[size_t(y) * cw + x]; };
    const auto v = [&](int x, int y) { return in.nv12 ? in.u[size_t(y) * cw * 2 + size_t(x) * 2 + 1] : in.v[size_t(y) * cw + x]; };
    if (f == VirtualCamera::Format::Yuy2) {
        const int stride = cw * 4;
        for (int y = 0; y < h; ++y) {
            uint8_t *row = out + size_t(y) * stride;
            const uint8_t *luma = in.y.data() + size_t(y) * w;
            for (int x = 0; x < cw; ++x) {
                row[x * 4] = luma[x * 2];
                row[x * 4 + 1] = u(x, y / 2);
                row[x * 4 + 2] = x * 2 + 1 < w ? luma[x * 2 + 1] : luma[x * 2];
                row[x * 4 + 3] = v(x, y / 2);
            }
        }
        return;
    }
    std::memcpy(out, in.y.data(), size_t(w) * h);
    uint8_t *c = out + size_t(w) * h;
    if (f == VirtualCamera::Format::Nv12) {
        if (in.nv12) { std::memcpy(c, in.u.data(), size_t(cw) * ch * 2); return; }
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                c[(size_t(y) * cw + x) * 2] = u(x, y);
                c[(size_t(y) * cw + x) * 2 + 1] = v(x, y);
            }
        return;
    }
    uint8_t *cv = c + size_t(cw) * ch;
    if (!in.nv12) {
        std::memcpy(c, in.u.data(), size_t(cw) * ch);
        std::memcpy(cv, in.v.data(), size_t(cw) * ch);
        return;
    }
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            c[size_t(y) * cw + x] = u(x, y);
            cv[size_t(y) * cw + x] = v(x, y);
        }
}
}  // namespace

struct VirtualCamera::Impl {
    VirtualCamera *owner = nullptr;
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_stream *stream = nullptr;
    spa_hook listener{};
    Settings settings;
    int width = 0, height = 0;        // offered and negotiated; loop thread
    std::mutex lifeMutex;             // start and stop against push
    std::mutex frameMutex;
    engine::YuvFrame pending;
    bool havePending = false;
    mutable std::mutex errorMutex;
    std::string error;

    const spa_pod *formatParam(spa_pod_builder *b) const {
        spa_rectangle size{uint32_t(width), uint32_t(height)};
        spa_fraction rate{uint32_t(settings.fpsNum), uint32_t(settings.fpsDen)};
        return static_cast<const spa_pod *>(spa_pod_builder_add_object(
            b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(4, SPA_VIDEO_FORMAT_NV12, SPA_VIDEO_FORMAT_NV12, SPA_VIDEO_FORMAT_I420, SPA_VIDEO_FORMAT_YUY2),
            SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate),
            SPA_FORMAT_VIDEO_colorRange, SPA_POD_Id(SPA_VIDEO_COLOR_RANGE_16_235),
            SPA_FORMAT_VIDEO_colorMatrix, SPA_POD_Id(SPA_VIDEO_COLOR_MATRIX_BT709)));
    }

    static void process(void *data) {
        auto *d = static_cast<Impl *>(data);
        pw_buffer *b = pw_stream_dequeue_buffer(d->stream);
        if (!b) return;
        spa_data *plane = b->buffer && b->buffer->n_datas ? &b->buffer->datas[0] : nullptr;
        const Format format = d->owner->m_format.load();
        int stride = 0;
        const size_t bytes = pictureBytes(format, d->width, d->height, &stride);
        bool sent = false;
        if (plane && plane->data && plane->chunk && plane->maxsize >= bytes && format != Format::None) {
            std::lock_guard<std::mutex> lock(d->frameMutex);
            if (d->havePending && d->pending.width == d->width && d->pending.height == d->height) {
                writePicture(d->pending, format, static_cast<uint8_t *>(plane->data));
                d->havePending = false;
                sent = true;
            }
        }
        if (plane && plane->chunk) {
            plane->chunk->offset = 0;
            plane->chunk->stride = stride;
            plane->chunk->size = sent ? uint32_t(bytes) : 0;
            plane->chunk->flags = sent ? 0 : SPA_CHUNK_FLAG_CORRUPTED;
        }
        pw_stream_queue_buffer(d->stream, b);
        if (sent) d->owner->m_sent.fetch_add(1);
    }

    static void paramChanged(void *data, uint32_t id, const spa_pod *param) {
        auto *d = static_cast<Impl *>(data);
        if (id != SPA_PARAM_Format) return;
        if (!param) { d->owner->m_format.store(Format::None); return; }
        spa_video_info_raw raw{};
        if (spa_format_video_raw_parse(param, &raw) < 0) return;
        const Format format = fromSpa(raw.format);
        d->owner->m_format.store(format);
        if (format == Format::None) return;
        int stride = 0;
        const size_t bytes = pictureBytes(format, int(raw.size.width), int(raw.size.height), &stride);
        uint8_t buffer[512];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
        const spa_pod *params[1] = {static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size, SPA_POD_Int(int(bytes)),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))))};
        pw_stream_update_params(d->stream, params, 1);
    }

    static void stateChanged(void *data, pw_stream_state, pw_stream_state state, const char *why) {
        auto *d = static_cast<Impl *>(data);
        if (state == PW_STREAM_STATE_ERROR) {
            std::lock_guard<std::mutex> lock(d->errorMutex);
            d->error = why ? why : "The virtual camera stopped";
        }
        if (state >= PW_STREAM_STATE_PAUSED) d->owner->m_nodeId.store(pw_stream_get_node_id(d->stream));
        d->owner->m_streaming.store(state == PW_STREAM_STATE_STREAMING);
        if (d->owner->onStateChanged) d->owner->onStateChanged();
    }

    // On the loop: a new picture to send, or a new size to offer.
    static int trigger(spa_loop *, bool, uint32_t, const void *, size_t, void *data) {
        auto *d = static_cast<Impl *>(data);
        if (d->stream) pw_stream_trigger_process(d->stream);
        return 0;
    }
    static int resize(spa_loop *, bool, uint32_t, const void *message, size_t, void *data) {
        auto *d = static_cast<Impl *>(data);
        const int *size = static_cast<const int *>(message);
        if (!d->stream || (size[0] == d->width && size[1] == d->height)) return 0;
        d->width = size[0];
        d->height = size[1];
        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
        const spa_pod *params[1] = {d->formatParam(&b)};
        pw_stream_update_params(d->stream, params, 1);
        return 0;
    }
};

VirtualCamera::VirtualCamera() : d(new Impl) {
    d->owner = this;
    pw_init(nullptr, nullptr);
}

VirtualCamera::~VirtualCamera() {
    stop();
    delete d;
}

std::string VirtualCamera::error() const {
    std::lock_guard<std::mutex> lock(d->errorMutex);
    return d->error;
}

bool VirtualCamera::start(const Settings &settings, std::string *error) {
    stop();
    std::lock_guard<std::mutex> life(d->lifeMutex);
    {
        std::lock_guard<std::mutex> lock(d->errorMutex);
        d->error.clear();
    }
    d->settings = settings;
    d->width = settings.width & ~1;
    d->height = settings.height & ~1;
    if (d->width <= 0 || d->height <= 0 || settings.fpsNum <= 0 || settings.fpsDen <= 0) {
        if (error) *error = "The canvas has no size yet";
        return false;
    }
    d->loop = pw_thread_loop_new("omabroadcast-camera", nullptr);
    d->context = pw_context_new(pw_thread_loop_get_loop(d->loop), nullptr, 0);
    if (pw_thread_loop_start(d->loop) < 0) {
        if (error) *error = "The camera's loop did not start";
        teardown();
        return false;
    }
    pw_thread_loop_lock(d->loop);
    d->core = pw_context_connect(d->context, nullptr, 0);
    if (!d->core) {
        pw_thread_loop_unlock(d->loop);
        if (error) *error = "No PipeWire server";
        teardown();
        return false;
    }
    auto *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                    PW_KEY_MEDIA_ROLE, "Camera", PW_KEY_MEDIA_CLASS, "Video/Source",
                                    PW_KEY_NODE_NAME, settings.nodeName.c_str(),
                                    PW_KEY_NODE_DESCRIPTION, settings.description.c_str(),
                                    PW_KEY_APP_ID, "org.omarchy.OmaBroadcast", nullptr);
    d->stream = pw_stream_new(d->core, settings.description.c_str(), props);
    static const pw_stream_events events = [] {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.process = Impl::process;
        e.param_changed = Impl::paramChanged;
        e.state_changed = Impl::stateChanged;
        return e;
    }();
    pw_stream_add_listener(d->stream, &d->listener, &events, d);
    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
    const spa_pod *params[1] = {d->formatParam(&b)};
    const int result = pw_stream_connect(d->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                         static_cast<pw_stream_flags>(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS),
                                         params, 1);
    pw_thread_loop_unlock(d->loop);
    if (result < 0) {
        if (error) *error = spa_strerror(result);
        teardown();
        return false;
    }
    m_running.store(true);
    return true;
}

void VirtualCamera::stop() {
    std::lock_guard<std::mutex> life(d->lifeMutex);
    teardown();
}

void VirtualCamera::teardown() {
    m_running.store(false);
    if (d->loop) {
        pw_thread_loop_lock(d->loop);
        if (d->stream) {
            pw_stream_disconnect(d->stream);
            pw_stream_destroy(d->stream);
            d->stream = nullptr;
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
    }
    m_streaming.store(false);
    m_format.store(Format::None);
    m_nodeId.store(0);
    std::lock_guard<std::mutex> lock(d->frameMutex);
    d->havePending = false;
}

void VirtualCamera::push(const engine::YuvFrame &frame) {
    std::lock_guard<std::mutex> life(d->lifeMutex);
    if (!m_running.load() || !d->loop || frame.width <= 0 || frame.height <= 0) return;
    {
        std::lock_guard<std::mutex> lock(d->frameMutex);
        if (d->havePending && m_streaming.load()) m_dropped.fetch_add(1);
        d->pending.width = frame.width;
        d->pending.height = frame.height;
        d->pending.nv12 = frame.nv12;
        d->pending.y = frame.y;
        d->pending.u = frame.u;
        d->pending.v = frame.v;
        d->havePending = true;
    }
    const int size[2] = {frame.width & ~1, frame.height & ~1};
    pw_loop_invoke(pw_thread_loop_get_loop(d->loop), Impl::resize, 0, size, sizeof size, false, d);
    pw_loop_invoke(pw_thread_loop_get_loop(d->loop), Impl::trigger, 0, nullptr, 0, false, d);
}
