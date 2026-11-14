#include "device_stream.h"
#include "core/hardware.h"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <sys/mman.h>
#include <time.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

int64_t monotonicNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000ll + ts.tv_nsec;
}

uint32_t spaFormat(VideoMode::Format f) {
    switch (f) {
    case VideoMode::Format::NV12: return SPA_VIDEO_FORMAT_NV12;
    case VideoMode::Format::I420: return SPA_VIDEO_FORMAT_I420;
    case VideoMode::Format::YUY2: return SPA_VIDEO_FORMAT_YUY2;
    case VideoMode::Format::P010: return SPA_VIDEO_FORMAT_P010_10LE;
    case VideoMode::Format::I010: return SPA_VIDEO_FORMAT_I420_10LE;
    case VideoMode::Format::MJPEG: break;
    }
    return SPA_VIDEO_FORMAT_UNKNOWN;
}

void copyPlane(std::vector<uint8_t> &dst, int *dstStride, const uint8_t *src, int srcStride, int rowBytes, int rows) {
    *dstStride = rowBytes;
    dst.resize(size_t(rowBytes) * rows);
    for (int r = 0; r < rows; ++r) std::memcpy(&dst[size_t(r) * rowBytes], src + size_t(r) * srcStride, size_t(rowBytes));
}

}  // namespace

struct DeviceStream::Impl {
    DeviceStream *owner = nullptr;
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_stream *stream = nullptr;
    spa_hook listener{};
    VideoMode mode;
    int width = 0, height = 0;   // as negotiated
    int64_t durationNs = 0;
    bool interlaced = false;
    RangeDetector range;  // raw modes: what the camera really sends

    // The newest picture, and whether the consumer has had it.
    std::mutex readyMutex;
    Picture ready;
    bool fresh = false;

    // MJPEG: the newest compressed frame, decoded off PipeWire's thread by
    // whichever decoder is free. Each JPEG stands alone, so several decode at
    // once (one core manages 4K at about 40 a second); pictures are passed on
    // in the order they came, a later one already out dropping an earlier.
    std::mutex jpegMutex;
    std::condition_variable jpegWake;
    std::vector<uint8_t> jpeg;
    int64_t jpegTime = 0, jpegDuration = 0;
    uint64_t jpegSerial = 0, publishedSerial = 0;
    bool jpegPending = false;
    bool quit = false;
    std::vector<std::thread> decoders;

    std::string error;            // with errorMutex: written on PipeWire's and the decoder's threads
    mutable std::mutex errorMutex;
    void setError(const std::string &e) { std::lock_guard<std::mutex> lock(errorMutex); error = e; }

    // When the picture last changed, by a sample of its first plane.
    uint64_t signature = 0;
    std::deque<int64_t> changes;
    static uint64_t sample(const Picture &p) {
        const size_t size = p.plane[0].size();
        if (size == 0 || p.height <= 0) return 0;
        const size_t row = size / size_t(p.height);
        uint64_t h = 1469598103934665603ull;
        for (int y = 0; y < 18; ++y)
            for (int x = 0; x < 32; ++x) {
                const size_t at = size_t((2 * y + 1) * p.height / 36) * row + (2 * x + 1) * row / 64;
                h = (h ^ p.plane[0][std::min(at, size - 1)]) * 1099511628211ull;
            }
        return h;
    }

    void publish(Picture &p, uint64_t serial = 0) {
        const uint64_t seen = sample(p);
        std::lock_guard<std::mutex> lock(readyMutex);
        if (seen != signature) {
            signature = seen;
            changes.push_back(monotonicNs());
            while (changes.size() > 1 && changes.back() - changes.front() > 2'000'000'000) changes.pop_front();
        }
        if (serial) {
            if (serial <= publishedSerial) { owner->m_dropped.fetch_add(1); return; }
            publishedSerial = serial;
        }
        if (fresh) owner->m_dropped.fetch_add(1);
        p.discontinuity = p.discontinuity || owner->m_received.load() == 0 || (fresh && ready.discontinuity);
        std::swap(ready, p);
        fresh = true;
        owner->m_received.fetch_add(1);
        owner->m_width.store(ready.width);
        owner->m_height.store(ready.height);
    }

    void onProcess() {
        pw_buffer *b = pw_stream_dequeue_buffer(stream);
        if (!b) return;
        spa_buffer *buf = b->buffer;
        const spa_data &d0 = buf->datas[0];
        // A DMA-BUF PipeWire did not map: map it here for this read.
        void *mapped = nullptr;
        size_t mappedSize = 0;
        const uint8_t *data = static_cast<const uint8_t *>(d0.data);
        if (!data && d0.type == SPA_DATA_DmaBuf && d0.fd >= 0 && d0.maxsize > 0) {
            mappedSize = d0.maxsize + d0.mapoffset;
            mapped = mmap(nullptr, mappedSize, PROT_READ, MAP_SHARED, int(d0.fd), 0);
            if (mapped == MAP_FAILED) mapped = nullptr;
            else data = static_cast<const uint8_t *>(mapped) + d0.mapoffset;
        }
        if (data && d0.chunk && d0.chunk->size > 0 && d0.chunk->offset < d0.maxsize && width > 0 && height > 0) {
            const uint8_t *base = data + d0.chunk->offset;
            const int stride0 = d0.chunk->stride > 0 ? d0.chunk->stride : 0;
            // What may be read from base: never past the buffer.
            const size_t available = d0.maxsize - d0.chunk->offset;
            if (mode.format == VideoMode::Format::MJPEG) {
                std::lock_guard<std::mutex> lock(jpegMutex);
                jpeg.assign(base, base + std::min<size_t>(d0.chunk->size, available));
                jpegTime = monotonicNs();
                jpegDuration = durationNs;
                ++jpegSerial;
                jpegPending = true;
                jpegWake.notify_one();
            } else {
                copyRaw(buf, base, stride0, available);
            }
        }
        if (mapped) munmap(mapped, mappedSize);
        pw_stream_queue_buffer(stream, b);
    }

    // Planes may come as separate datas or one block; both are handled. A
    // frame whose planes do not fit what the buffer holds is dropped.
    void copyRaw(spa_buffer *buf, const uint8_t *base, int stride0, size_t available) {
        Picture p;
        p.width = width;
        p.height = height;
        p.timeNs = monotonicNs();
        p.durationNs = durationNs; p.interlaced = interlaced;
        // UVC convention: HD is BT.709, standard definition BT.601; limited
        // range unless the picture shows otherwise (see RangeDetector).
        p.matrix = height >= 720 ? engine::YuvMatrix::BT709 : engine::YuvMatrix::BT601;
        const int cw = (width + 1) / 2, ch = (height + 1) / 2;
        // Bytes `rows` rows of `bytes` each take at this stride.
        const auto span = [](int stride, int rows, int bytes) { return rows > 0 ? size_t(stride) * size_t(rows - 1) + size_t(bytes) : 0; };
        bool fits = true;  // every plane below inside its buffer
        auto plane = [&](uint32_t i, size_t offsetInBlock, int fallbackStride, int rows, int bytes, const uint8_t **ptr, int *stride) {
            if (buf->n_datas > i && buf->datas[i].chunk && (buf->datas[i].data || buf->datas[i].type == SPA_DATA_DmaBuf)) {
                const spa_data &d = buf->datas[i];
                // A plane of its own that PipeWire did not map (a multi-planar
                // DMA-BUF): not inside plane 0's memory, so not read at all.
                if (!d.data || d.chunk->offset >= d.maxsize) { fits = false; *ptr = base; *stride = fallbackStride; return; }
                *ptr = static_cast<const uint8_t *>(d.data) + d.chunk->offset;
                *stride = d.chunk->stride > 0 ? d.chunk->stride : fallbackStride;
                fits = fits && span(*stride, rows, bytes) <= d.maxsize - d.chunk->offset;
            } else {
                *ptr = base + offsetInBlock;
                *stride = fallbackStride;
                fits = fits && offsetInBlock + span(*stride, rows, bytes) <= available;
            }
        };
        switch (mode.format) {
        case VideoMode::Format::YUY2: {
            const int s = stride0 > 0 ? stride0 : cw * 4;
            p.layout = engine::YuvLayout::YUY2;
            if (span(s, height, cw * 4) > available) return;
            copyPlane(p.plane[0], &p.stride[0], base, s, cw * 4, height);
            break;
        }
        case VideoMode::Format::NV12: {
            const int s = stride0 > 0 ? stride0 : width;
            const uint8_t *uv;
            int uvStride;
            plane(1, size_t(s) * height, s, ch, cw * 2, &uv, &uvStride);
            p.layout = engine::YuvLayout::NV12;
            if (!fits || span(s, height, width) > available) return;
            copyPlane(p.plane[0], &p.stride[0], base, s, width, height);
            copyPlane(p.plane[1], &p.stride[1], uv, uvStride, cw * 2, ch);
            break;
        }
        case VideoMode::Format::I420: {
            const int s = stride0 > 0 ? stride0 : width;
            const int cs = (s + 1) / 2;
            const uint8_t *u, *v;
            int us, vs;
            plane(1, size_t(s) * height, cs, ch, cw, &u, &us);
            plane(2, size_t(s) * height + size_t(cs) * ch, cs, ch, cw, &v, &vs);
            p.layout = engine::YuvLayout::I420;
            if (!fits || span(s, height, width) > available) return;
            copyPlane(p.plane[0], &p.stride[0], base, s, width, height);
            copyPlane(p.plane[1], &p.stride[1], u, us, cw, ch);
            copyPlane(p.plane[2], &p.stride[2], v, vs, cw, ch);
            break;
        }
        case VideoMode::Format::P010: {
            // As NV12, two bytes a sample, the code in the high ten bits.
            const int s = stride0 > 0 ? stride0 : width * 2;
            const uint8_t *uv;
            int uvStride;
            plane(1, size_t(s) * height, s, ch, cw * 4, &uv, &uvStride);
            p.layout = engine::YuvLayout::NV12;
            p.depth = 10;
            p.msb = true;
            if (!fits || span(s, height, width * 2) > available) return;
            copyPlane(p.plane[0], &p.stride[0], base, s, width * 2, height);
            copyPlane(p.plane[1], &p.stride[1], uv, uvStride, cw * 4, ch);
            break;
        }
        case VideoMode::Format::I010: {
            const int s = stride0 > 0 ? stride0 : width * 2;
            const int cs = ((s / 2 + 1) / 2) * 2;
            const uint8_t *u, *v;
            int us, vs;
            plane(1, size_t(s) * height, cs, ch, cw * 2, &u, &us);
            plane(2, size_t(s) * height + size_t(cs) * ch, cs, ch, cw * 2, &v, &vs);
            p.layout = engine::YuvLayout::I420;
            p.depth = 10;
            if (!fits || span(s, height, width * 2) > available) return;
            copyPlane(p.plane[0], &p.stride[0], base, s, width * 2, height);
            copyPlane(p.plane[1], &p.stride[1], u, us, cw * 2, ch);
            copyPlane(p.plane[2], &p.stride[2], v, vs, cw * 2, ch);
            break;
        }
        case VideoMode::Format::MJPEG: return;
        }
        range.add(p);
        p.fullRange = range.full();
        publish(p);
    }

    void decodeLoop() {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!ctx || avcodec_open2(ctx, codec, nullptr) < 0) {
            setError("no MJPEG decoder");
            if (ctx) avcodec_free_context(&ctx);
            return;
        }
        AVPacket *pkt = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        SwsContext *sws = nullptr;
        std::vector<uint8_t> work;
        for (;;) {
            int64_t when = 0, duration = 0;
            uint64_t serial = 0;
            {
                std::unique_lock<std::mutex> lock(jpegMutex);
                jpegWake.wait(lock, [&] { return quit || jpegPending; });
                if (quit) break;
                work.swap(jpeg);
                when = jpegTime; duration = jpegDuration; serial = jpegSerial;
                jpegPending = false;
            }
            pkt->data = work.data();
            pkt->size = int(work.size());
            if (avcodec_send_packet(ctx, pkt) < 0) continue;
            while (avcodec_receive_frame(ctx, frame) == 0) {
                Picture p;
                p.width = frame->width;
                p.height = frame->height;
                p.timeNs = when; p.durationNs = duration;
                p.interlaced = (frame->flags & AV_FRAME_FLAG_INTERLACED) != 0;
                p.firstField = p.interlaced ? ((frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) ? 0 : 1) : -1;
                // JPEG is BT.601 at full range unless the stream says otherwise.
                p.matrix = engine::YuvMatrix::BT601;
                p.fullRange = frame->color_range != AVCOL_RANGE_MPEG;
                const int cw = (frame->width + 1) / 2, ch = (frame->height + 1) / 2;
                const AVPixelFormat fmt = AVPixelFormat(frame->format);
                if (fmt == AV_PIX_FMT_YUVJ420P || fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ422P ||
                    fmt == AV_PIX_FMT_YUV422P) {
                    const bool full422 = fmt == AV_PIX_FMT_YUVJ422P || fmt == AV_PIX_FMT_YUV422P;
                    const int crows = full422 ? frame->height : ch;
                    p.layout = full422 ? engine::YuvLayout::I422 : engine::YuvLayout::I420;
                    if (fmt == AV_PIX_FMT_YUVJ420P || fmt == AV_PIX_FMT_YUVJ422P) p.fullRange = true;
                    copyPlane(p.plane[0], &p.stride[0], frame->data[0], frame->linesize[0], frame->width, frame->height);
                    copyPlane(p.plane[1], &p.stride[1], frame->data[1], frame->linesize[1], cw, crows);
                    copyPlane(p.plane[2], &p.stride[2], frame->data[2], frame->linesize[2], cw, crows);
                } else {
                    // Preserve chroma rows for interlaced fallback formats.
                    sws = sws_getCachedContext(sws, frame->width, frame->height, fmt, frame->width, frame->height,
                                               p.interlaced ? AV_PIX_FMT_YUV422P : AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    if (!sws) continue;
                    // The range as it comes: the picture says which it is.
                    const auto *coefficients = sws_getCoefficients(SWS_CS_ITU601);
                    sws_setColorspaceDetails(sws, coefficients, p.fullRange, coefficients, p.fullRange, 0, 1 << 16, 1 << 16);
                    p.layout = p.interlaced ? engine::YuvLayout::I422 : engine::YuvLayout::I420;
                    p.stride[0] = frame->width;
                    p.stride[1] = p.stride[2] = cw;
                    p.plane[0].resize(size_t(frame->width) * frame->height);
                    p.plane[1].resize(size_t(cw) * (p.interlaced ? frame->height : ch));
                    p.plane[2].resize(size_t(cw) * (p.interlaced ? frame->height : ch));
                    uint8_t *dst[4] = {p.plane[0].data(), p.plane[1].data(), p.plane[2].data(), nullptr};
                    int dstStride[4] = {p.stride[0], p.stride[1], p.stride[2], 0};
                    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
                }
                publish(p, serial);
            }
        }
        sws_freeContext(sws);
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&ctx);
    }

    static void process(void *data) { static_cast<Impl *>(data)->onProcess(); }

    static void paramChanged(void *data, uint32_t id, const spa_pod *param) {
        auto *impl = static_cast<Impl *>(data);
        if (id != SPA_PARAM_Format || !param) return;
        uint32_t mediaType = 0, mediaSubtype = 0;
        if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0) return;
        spa_rectangle size{0, 0};
        spa_fraction rate{uint32_t(std::max(0,impl->mode.fpsNum)), uint32_t(std::max(1,impl->mode.fpsDen))};
        impl->interlaced = false;
        impl->range.reset();
        if (mediaSubtype == SPA_MEDIA_SUBTYPE_raw) {
            spa_video_info_raw raw{};
            if (spa_format_video_raw_parse(param, &raw) >= 0) {
                size = raw.size;
                if (raw.framerate.num > 0) rate = raw.framerate;
                impl->interlaced = raw.interlace_mode == SPA_VIDEO_INTERLACE_MODE_INTERLEAVED;
            }
        } else if (mediaSubtype == SPA_MEDIA_SUBTYPE_mjpg) {
            spa_pod_parse_object(param, SPA_TYPE_OBJECT_Format, nullptr, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size));
        }
        impl->durationNs = rate.num > 0 ? 1'000'000'000ll * rate.denom / rate.num : 0;
        impl->width = int(size.width);
        impl->height = int(size.height);
        // Real V4L2 devices export their buffers as DMA-BUF; accept those (we
        // map them to read) as well as plain memory, or the link fails with
        // EINVAL when the buffers are handed over.
        uint8_t buffer[256];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
        const int types = (1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_DmaBuf);
        const spa_pod *params[1] = {static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
            SPA_POD_CHOICE_RANGE_Int(4, 2, 16), SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(types)))};
        pw_stream_update_params(impl->stream, params, 1);
    }

    static void stateChanged(void *data, pw_stream_state, pw_stream_state state, const char *err) {
        auto *impl = static_cast<Impl *>(data);
        impl->owner->m_running.store(state == PW_STREAM_STATE_STREAMING);
        if (state == PW_STREAM_STATE_ERROR && err) impl->setError(err);
    }
};

DeviceStream::DeviceStream() : d(new Impl) {
    d->owner = this;
    pw_init(nullptr, nullptr);
}

DeviceStream::~DeviceStream() {
    stop();
    delete d;
}

bool DeviceStream::start(const VideoDevice &device, const VideoMode &mode, std::string *error) {
    stop();
    m_mode = mode;
    d->mode = mode;
    d->setError({});
    d->width = mode.width;
    d->height = mode.height;
    m_received.store(0);
    m_dropped.store(0);
    {
        // PipeWire's thread goes on after the decoders have stopped, so the last
        // run can leave a picture here, and none of this is about the new one.
        std::lock_guard<std::mutex> lock(d->jpegMutex);
        d->jpegPending = false;
    }
    {
        std::lock_guard<std::mutex> lock(d->readyMutex);
        d->signature = 0;
        d->changes.clear();
    }
    if (mode.format == VideoMode::Format::MJPEG) {
        d->quit = false;
        d->jpegSerial = d->publishedSerial = 0;
        // Big pictures need more of them; a few cores at most.
        const int count = hardware::jpegDecoders(mode.width, mode.height);
        for (int i = 0; i < count; ++i) d->decoders.emplace_back([this] { d->decodeLoop(); });
    }
    d->loop = pw_thread_loop_new("omabroadcast-device", nullptr);
    d->context = pw_context_new(pw_thread_loop_get_loop(d->loop), nullptr, 0);
    if (pw_thread_loop_start(d->loop) < 0) {
        if (error) *error = "PipeWire loop did not start";
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
    pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                             PW_KEY_NODE_NAME, "omabroadcast-device",
                                             nullptr);
    // The session manager finds a target by serial or name, never by the
    // node's id. No node.dont-fallback: with it WirePlumber destroys the
    // stream on a re-scan even while the target is there. An unplugged device
    // is stopped by its owner as soon as VideoDevices loses it.
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, device.serial.empty() ? device.name.c_str() : device.serial.c_str());
    d->stream = pw_stream_new(d->core, "Capture device", props);
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
    spa_rectangle size{uint32_t(mode.width), uint32_t(mode.height)};
    spa_fraction rate{uint32_t(mode.fpsNum), uint32_t(mode.fpsDen)};
    const spa_pod *param;
    if (mode.format == VideoMode::Format::MJPEG) {
        param = static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_mjpg), SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate)));
    } else {
        param = static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
            SPA_POD_Id(spaFormat(mode.format)), SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate)));
    }
    const int r = pw_stream_connect(d->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                                    static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                                    &param, 1);
    pw_thread_loop_unlock(d->loop);
    if (r < 0) {
        if (error) *error = std::string("device stream: ") + spa_strerror(r);
        stop();
        return false;
    }
    return true;
}

void DeviceStream::stop() {
    if (!d->decoders.empty()) {
        {
            std::lock_guard<std::mutex> lock(d->jpegMutex);
            d->quit = true;
        }
        d->jpegWake.notify_all();
        for (std::thread &t : d->decoders) t.join();
        d->decoders.clear();
    }
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
    m_running.store(false);
    std::lock_guard<std::mutex> lock(d->readyMutex);
    d->fresh = false;
}

bool DeviceStream::acquire(Picture &out) {
    std::lock_guard<std::mutex> lock(d->readyMutex);
    if (!d->fresh) return false;
    std::swap(out, d->ready);
    d->fresh = false;
    return true;
}

double DeviceStream::pictureRate() const {
    std::lock_guard<std::mutex> lock(d->readyMutex);
    const int64_t now = monotonicNs();
    // Two seconds, still coming: older or stalled pictures say nothing about the rate.
    if (d->changes.size() < 2 || now - d->changes.back() > 500'000'000 || now - d->changes.front() < 1'500'000'000) return 0;
    return double(d->changes.size() - 1) * 1e9 / double(d->changes.back() - d->changes.front());
}

std::string DeviceStream::error() const {
    std::lock_guard<std::mutex> lock(d->errorMutex);
    return d->error;
}
