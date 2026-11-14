#include "video_stream.h"

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/debug/types.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace {

int64_t monotonicNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000ll + ts.tv_nsec;
}

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
}

struct FormatMap {
    spa_video_format spa;
    uint32_t fourcc;
    engine::PixelFormat pixel;
};
const FormatMap kFormats[] = {
    {SPA_VIDEO_FORMAT_BGRx, fourcc('X', 'R', '2', '4'), engine::PixelFormat::BGRx},
    {SPA_VIDEO_FORMAT_BGRA, fourcc('A', 'R', '2', '4'), engine::PixelFormat::BGRA},
    {SPA_VIDEO_FORMAT_RGBx, fourcc('X', 'B', '2', '4'), engine::PixelFormat::RGBx},
    {SPA_VIDEO_FORMAT_RGBA, fourcc('A', 'B', '2', '4'), engine::PixelFormat::RGBA},
};

const FormatMap *mapFor(uint32_t spaFormat) {
    for (const FormatMap &m : kFormats)
        if (m.spa == spaFormat) return &m;
    return nullptr;
}

struct Mapping {
    void *ptr = nullptr;
    size_t size = 0;
    // A DMA-BUF's own descriptor, ours to keep for as long as the mapping is: the
    // consumer imports it later with no lock held, and PipeWire can close its copy
    // at any moment (see retired).
    int fd = -1;
    // Names the buffer to the engine's import cache: never reused, as a
    // pw_buffer's address is once the stream renegotiates or starts again.
    uintptr_t token = 0;
};

std::atomic<uintptr_t> gNextToken{1};

void unmap(Mapping *m) {
    if (m->ptr) munmap(m->ptr, m->size);
    if (m->fd >= 0) close(m->fd);
    delete m;
}

}  // namespace

struct VideoStream::Impl {
    VideoStream *owner;
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_stream *stream = nullptr;
    spa_hook listener{};
    ModifierLookup modifiers;
    bool allowDmaBuf = true;

    // Negotiated format.
    spa_video_info_raw info{};
    const FormatMap *format = nullptr;
    bool dmabuf = false;

    // The consumer's calls against stop(): never both at once.
    std::mutex consumer;
    std::mutex mutex;               // what follows, and the format above
    // Mappings of buffers that went while the consumer held one: unmapped
    // once it lets go, as it may still be reading them.
    std::vector<Mapping *> retired;
    pw_buffer *pending = nullptr;   // newest frame not yet acquired
    pw_buffer *held = nullptr;      // acquired by the consumer
    int64_t pendingTime = 0;
    // Where its picture lies in shared memory, as checked: the other side can
    // write the chunk at any time.
    uint32_t pendingOffset = 0;
    int pendingStride = 0;
    bool firstFrame = true;
    std::string error;

    static constexpr int kMaxParams = 16;

    // Build the EnumFormat pods: for each pixel format one entry with the
    // importable modifiers (DMA-BUF) and one without (memory).
    int buildFormats(spa_pod_builder *b, const spa_pod **params, int max, const FormatMap *only,
                     uint64_t fixedModifier, bool fixate) {
        // Named so their addresses can be taken (the SPA macros build temporaries).
        const spa_rectangle sizeDefault = SPA_RECTANGLE(1920, 1080);
        const spa_rectangle sizeMin = SPA_RECTANGLE(1, 1);
        const spa_rectangle sizeMax = SPA_RECTANGLE(16384, 16384);
        const spa_fraction rateDefault = SPA_FRACTION(60, 1);
        const spa_fraction rateMin = SPA_FRACTION(0, 1);
        const spa_fraction rateMax = SPA_FRACTION(1000, 1);
        int n = 0;
        for (const FormatMap &m : kFormats) {
            if (only && only != &m) continue;
            std::vector<uint64_t> mods;
            if (allowDmaBuf && modifiers) mods = modifiers(m.fourcc);
            if (fixate) mods = {fixedModifier};
            if (!mods.empty() && n < max) {
                spa_pod_frame f;
                spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
                spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                                    SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                                    SPA_FORMAT_VIDEO_format, SPA_POD_Id(m.spa), 0);
                if (fixate) {
                    spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
                    spa_pod_builder_long(b, int64_t(fixedModifier));
                } else {
                    spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier,
                                         SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
                    spa_pod_frame choice;
                    spa_pod_builder_push_choice(b, &choice, SPA_CHOICE_Enum, 0);
                    spa_pod_builder_long(b, int64_t(mods[0]));  // the default, then the alternatives
                    for (uint64_t mod : mods) spa_pod_builder_long(b, int64_t(mod));
                    spa_pod_builder_pop(b, &choice);
                }
                spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
                                    SPA_POD_CHOICE_RANGE_Rectangle(&sizeDefault, &sizeMin, &sizeMax),
                                    SPA_FORMAT_VIDEO_framerate,
                                    SPA_POD_CHOICE_RANGE_Fraction(&rateDefault, &rateMin, &rateMax), 0);
                params[n++] = static_cast<const spa_pod *>(spa_pod_builder_pop(b, &f));
            }
            if (!fixate && n < max) {
                params[n++] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
                    b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
                    SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                    SPA_FORMAT_VIDEO_format, SPA_POD_Id(m.spa), SPA_FORMAT_VIDEO_size,
                    SPA_POD_CHOICE_RANGE_Rectangle(&sizeDefault, &sizeMin, &sizeMax), SPA_FORMAT_VIDEO_framerate,
                    SPA_POD_CHOICE_RANGE_Fraction(&rateDefault, &rateMin, &rateMax)));
            }
        }
        return n;
    }

    void onParamChanged(uint32_t id, const spa_pod *param) {
        if (!param || id != SPA_PARAM_Format) return;
        uint32_t mediaType = 0, mediaSubtype = 0;
        if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 || mediaType != SPA_MEDIA_TYPE_video ||
            mediaSubtype != SPA_MEDIA_SUBTYPE_raw)
            return;
        spa_video_info_raw parsed{};
        if (spa_format_video_raw_parse(param, &parsed) < 0) return;
        const FormatMap *fm = mapFor(parsed.format);
        if (!fm) {
            setError("unsupported pixel format from the compositor");
            return;
        }

        uint8_t buffer[4096];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
        const spa_pod *params[kMaxParams];

        const spa_pod_prop *modProp = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier);
        if (modProp && (modProp->flags & SPA_POD_PROP_FLAG_DONT_FIXATE)) {
            // Several modifiers are acceptable to both sides; pin the first.
            const int n = buildFormats(&b, params, kMaxParams, fm, parsed.modifier, true);
            pw_stream_update_params(stream, params, n);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            info = parsed;
            format = fm;
            dmabuf = modProp != nullptr;
        }
        owner->m_width.store(int(info.size.width));
        owner->m_height.store(int(info.size.height));
        owner->m_dmabuf.store(dmabuf);

        const uint32_t stride = info.size.width * 4;
        const uint32_t size = stride * info.size.height;
        const int dataTypes = dmabuf ? (1 << SPA_DATA_DmaBuf) : ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr));
        int n = 0;
        params[n++] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
            SPA_POD_CHOICE_RANGE_Int(6, 2, 16), SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size,
            SPA_POD_Int(int(size)), SPA_PARAM_BUFFERS_stride, SPA_POD_Int(int(stride)), SPA_PARAM_BUFFERS_align,
            SPA_POD_Int(16), SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(dataTypes)));
        params[n++] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
            SPA_PARAM_META_size, SPA_POD_Int(int(sizeof(spa_meta_header)))));
        pw_stream_update_params(stream, params, n);
    }

    void onAddBuffer(pw_buffer *buf) {
        spa_data &d = buf->buffer->datas[0];
        auto *m = new Mapping;
        m->token = gNextToken.fetch_add(1);
        if (d.type == SPA_DATA_MemFd && d.fd >= 0) {
            m->size = d.maxsize + d.mapoffset;
            m->ptr = mmap(nullptr, m->size, PROT_READ, MAP_SHARED, int(d.fd), 0);
            if (m->ptr == MAP_FAILED) m->ptr = nullptr;
        } else if (d.type == SPA_DATA_DmaBuf && d.fd >= 0) {
            m->fd = fcntl(int(d.fd), F_DUPFD_CLOEXEC, 0);
        }
        buf->user_data = m;
    }

    void onRemoveBuffer(pw_buffer *buf) {
        auto *m = static_cast<Mapping *>(buf->user_data);
        buf->user_data = nullptr;
        std::lock_guard<std::mutex> lock(mutex);
        if (pending == buf) pending = nullptr;
        if (held == buf) {
            held = nullptr;
            if (m) retired.push_back(m);
        } else if (m) {
            unmap(m);
        }
    }
    // The consumer has let go of what it held (under `consumer`).
    void dropRetired() {
        std::vector<Mapping *> gone;
        {
            std::lock_guard<std::mutex> lock(mutex);
            gone.swap(retired);
        }
        for (Mapping *m : gone) unmap(m);
    }

    void onProcess() {
        pw_buffer *newest = nullptr;
        while (pw_buffer *b = pw_stream_dequeue_buffer(stream)) {
            if (newest) pw_stream_queue_buffer(stream, newest);
            newest = b;
        }
        if (!newest) return;
        const spa_data &d = newest->buffer->datas[0];
        if (!d.chunk || d.chunk->size == 0 || (d.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
            pw_stream_queue_buffer(stream, newest);
            return;
        }
        const uint32_t offset = d.chunk->offset;
        int rowStride = d.chunk->stride;
        // In shared memory the engines read every row from where this says. This
        // thread is the only one that writes the format.
        if (d.type != SPA_DATA_DmaBuf &&
            !VideoStream::pictureFits(offset, &rowStride, int(info.size.width), int(info.size.height), d.maxsize)) {
            pw_stream_queue_buffer(stream, newest);
            return;
        }
        owner->m_received.fetch_add(1);
        std::lock_guard<std::mutex> lock(mutex);
        if (pending) {
            pw_stream_queue_buffer(stream, pending);
            owner->m_dropped.fetch_add(1);
        }
        pending = newest;
        pendingTime = monotonicNs();
        pendingOffset = offset;
        pendingStride = rowStride;
    }

    void onStateChanged(pw_stream_state old, pw_stream_state state, const char *err) {
        (void)old;
        owner->m_streaming.store(state == PW_STREAM_STATE_STREAMING);
        if (state == PW_STREAM_STATE_ERROR) setError(err ? err : "stream error");
    }

    void setError(const std::string &e) {
        std::lock_guard<std::mutex> lock(mutex);
        error = e;
    }

    static void paramChanged(void *data, uint32_t id, const spa_pod *param) {
        static_cast<Impl *>(data)->onParamChanged(id, param);
    }
    static void addBuffer(void *data, pw_buffer *b) { static_cast<Impl *>(data)->onAddBuffer(b); }
    static void removeBuffer(void *data, pw_buffer *b) { static_cast<Impl *>(data)->onRemoveBuffer(b); }
    static void process(void *data) { static_cast<Impl *>(data)->onProcess(); }
    static void stateChanged(void *data, pw_stream_state old, pw_stream_state state, const char *err) {
        static_cast<Impl *>(data)->onStateChanged(old, state, err);
    }
};

namespace {
const pw_stream_events kEvents = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = VideoStream::Impl::stateChanged;
    e.param_changed = VideoStream::Impl::paramChanged;
    e.add_buffer = VideoStream::Impl::addBuffer;
    e.remove_buffer = VideoStream::Impl::removeBuffer;
    e.process = VideoStream::Impl::process;
    return e;
}();

std::once_flag gPwInit;
}  // namespace

bool VideoStream::pictureFits(uint32_t offset, int *stride, int width, int height, uint32_t size) {
    if (width <= 0 || height <= 0) return false;
    // None given: rows follow each other, as PipeWire's own default has it.
    if (*stride <= 0) *stride = width * 4;
    if (*stride < width * 4) return false;
    return uint64_t(offset) + uint64_t(*stride) * uint64_t(height - 1) + uint64_t(width) * 4 <= uint64_t(size);
}

std::vector<uint32_t> VideoStream::fourccs() {
    std::vector<uint32_t> out;
    for (const FormatMap &m : kFormats) out.push_back(m.fourcc);
    return out;
}

VideoStream::VideoStream() : d(new Impl) {
    d->owner = this;
    std::call_once(gPwInit, [] { pw_init(nullptr, nullptr); });
}

VideoStream::~VideoStream() {
    stop();
    d->dropRetired();  // nothing can hold a frame of a stream being destroyed
    delete d;
}

bool VideoStream::start(int fd, uint32_t nodeId, ModifierLookup modifiers, std::string *error) {
    stop();
    d->firstFrame = true;
    d->modifiers = std::move(modifiers);
    d->setError({});
    m_received.store(0);
    m_dropped.store(0);
    d->loop = pw_thread_loop_new("omabroadcast-video", nullptr);
    if (!d->loop) {
        if (error) *error = "PipeWire loop";
        if (fd >= 0) close(fd);
        return false;
    }
    d->context = pw_context_new(pw_thread_loop_get_loop(d->loop), nullptr, 0);
    if (pw_thread_loop_start(d->loop) < 0) {
        if (error) *error = "PipeWire loop did not start";
        if (fd >= 0) close(fd);
        stop();
        return false;
    }

    pw_thread_loop_lock(d->loop);
    d->core = fd >= 0 ? pw_context_connect_fd(d->context, fd, nullptr, 0)  // owns fd from here
                      : pw_context_connect(d->context, nullptr, 0);
    if (!d->core) {
        pw_thread_loop_unlock(d->loop);
        if (error) *error = "PipeWire would not take the portal's connection";
        stop();
        return false;
    }
    pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                             PW_KEY_MEDIA_ROLE, "Screen", nullptr);
    d->stream = pw_stream_new(d->core, "omabroadcast", props);
    pw_stream_add_listener(d->stream, &d->listener, &kEvents, d);

    uint8_t buffer[8192];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
    const spa_pod *params[Impl::kMaxParams];
    const int n = d->buildFormats(&b, params, Impl::kMaxParams, nullptr, 0, false);
    const int r = pw_stream_connect(d->stream, PW_DIRECTION_INPUT, nodeId,
                                    static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT), params, n);
    pw_thread_loop_unlock(d->loop);
    if (r < 0) {
        if (error) *error = std::string("stream connect: ") + spa_strerror(r);
        stop();
        return false;
    }
    return true;
}

void VideoStream::disableDmaBuf() {
    std::lock_guard<std::mutex> use(d->consumer);
    if (!d->stream || !d->allowDmaBuf) return;
    d->allowDmaBuf = false;
    pw_thread_loop_lock(d->loop);
    uint8_t buffer[8192];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
    const spa_pod *params[Impl::kMaxParams];
    const int n = d->buildFormats(&b, params, Impl::kMaxParams, nullptr, 0, false);
    pw_stream_update_params(d->stream, params, n);
    pw_thread_loop_unlock(d->loop);
}

void VideoStream::stop() {
    // A frame the consumer holds keeps its memory (see retired) until release.
    std::lock_guard<std::mutex> use(d->consumer);
    if (d->loop) {
        pw_thread_loop_lock(d->loop);
        if (d->stream) {
            {
                std::lock_guard<std::mutex> lock(d->mutex);
                if (d->pending) pw_stream_queue_buffer(d->stream, d->pending);
                if (d->held) {
                    pw_stream_queue_buffer(d->stream, d->held);
                    // Still being read, maybe: its memory stays until release.
                    if (auto *m = static_cast<Mapping *>(d->held->user_data)) d->retired.push_back(m);
                    d->held->user_data = nullptr;
                }
                d->pending = d->held = nullptr;
            }
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
        if (d->context) {
            pw_context_destroy(d->context);
            d->context = nullptr;
        }
        pw_thread_loop_destroy(d->loop);
        d->loop = nullptr;
    }
    d->allowDmaBuf = true;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->format = nullptr;
    }
    m_streaming.store(false);
}

bool VideoStream::acquire(Frame &out) {
    std::lock_guard<std::mutex> use(d->consumer);
    std::lock_guard<std::mutex> lock(d->mutex);
    if (!d->pending || !d->format) return false;
    pw_buffer *buf = d->pending;
    d->pending = nullptr;
    d->held = buf;

    const spa_data &data = buf->buffer->datas[0];
    out = Frame{};
    out.raw = buf;
    out.timeNs = d->pendingTime;
    const auto *header = static_cast<const spa_meta_header *>(spa_buffer_find_meta_data(buf->buffer,SPA_META_Header,sizeof(spa_meta_header)));
    out.discontinuity = d->firstFrame || (header && (header->flags & SPA_META_HEADER_FLAG_DISCONT));
    if (d->info.framerate.num > 0) out.durationNs = 1'000'000'000ll * d->info.framerate.denom / d->info.framerate.num;
    const int w = int(d->info.size.width), h = int(d->info.size.height);
    if (data.type == SPA_DATA_DmaBuf) {
        out.dmabuf = true;
        out.dma.width = w;
        out.dma.height = h;
        out.dma.fourcc = d->format->fourcc;
        out.dma.modifier = d->info.modifier;
        const auto *m = static_cast<const Mapping *>(buf->user_data);
        out.dma.planes = {{m && m->fd >= 0 ? m->fd : int(data.fd), data.chunk->offset, data.chunk->stride}};
        out.dma.token = m ? reinterpret_cast<const void *>(m->token) : nullptr;
    } else {
        const uint8_t *base = nullptr;
        if (data.type == SPA_DATA_MemPtr) {
            base = static_cast<const uint8_t *>(data.data);
        } else if (auto *m = static_cast<Mapping *>(buf->user_data); m && m->ptr) {
            base = static_cast<const uint8_t *>(m->ptr) + data.mapoffset;
        }
        if (!base) {
            d->held = nullptr;
            d->pending = buf;  // give the consumer another go later
            return false;
        }
        out.cpu = engine::CpuFrame{w, h, d->pendingStride, d->format->pixel, base + d->pendingOffset};
    }
    d->firstFrame = false;
    return true;
}

void VideoStream::release(Frame &frame) {
    if (!frame.raw) return;
    std::lock_guard<std::mutex> use(d->consumer);
    if (d->loop) {
        pw_thread_loop_lock(d->loop);
        {
            std::lock_guard<std::mutex> lock(d->mutex);
            if (d->held == frame.raw) {
                if (d->stream) pw_stream_queue_buffer(d->stream, frame.raw);
                d->held = nullptr;
            }
        }
        pw_thread_loop_unlock(d->loop);
    }
    // It held one frame at most: whatever went meanwhile is unread now.
    d->dropRetired();
    frame.raw = nullptr;
}

std::string VideoStream::error() const {
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->error;
}
