#include "media_stream.h"
#include "audio/tempo.h"
#include "engine/engine.h"
#include "core/hardware.h"
#include "core/scene.h"
#include "output/encoders.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/cpu.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <time.h>

#include <condition_variable>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <memory>
#include <thread>

namespace {

int64_t monotonicNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000ll + ts.tv_nsec;
}

constexpr int kMaxVideoAhead = 6;                 // decoded frames waiting
constexpr int64_t kMaxAudioAheadNs = 500'000'000; // decoded audio waiting
constexpr int64_t kFilePrerollNs = 50'000'000;    // a file's first frame, a moment after start
constexpr int64_t kAudioEarlyNs = 20'000'000;     // released this far before it is due
constexpr int kMaxFailedReads = 10;               // pictures in a row the card will not hand back
// A paused file is left open this long for the next step or seek, then let go.
constexpr auto kIdleOpen = std::chrono::seconds(30);
// Read while looking for a stream's first picture, at most.
constexpr int kMaxHeldPackets = 2000;
constexpr int64_t kMaxHeldBytes = 64 * 1024 * 1024;
// A jump in a file's times, in seconds, between one packet and the next: where
// the container is one that can have them, and where it is not.
constexpr double kTimeJump = 5, kTimeJumpLong = 60;
// A sender says its own size: no bigger than the canvas can be.
constexpr int64_t kMaxSenderPixels = int64_t(8192) * 8192;
// What a stranger's data may reach, on a listener: what phones and encoders send.
const char *const kListenerFormats = "mpegts,flv,live_flv,matroska,webm";
const char *const kListenerDecoders = "h264,hevc,av1,libdav1d,vp8,vp9,libvpx,libvpx-vp9,mjpeg,"
                                      "aac,aac_latm,opus,libopus,mp3float,mp3,mp2float,mp2,ac3,eac3";

void copyPlane(std::vector<uint8_t> &dst, int *stride, const uint8_t *src, int srcStride, int rowBytes, int rows) {
    *stride = rowBytes;
    dst.resize(size_t(rowBytes) * rows);
    for (int r = 0; r < rows; ++r) std::memcpy(&dst[size_t(r) * rowBytes], src + ptrdiff_t(r) * srcStride, size_t(rowBytes));
}

engine::YuvMatrix matrixOf(const AVFrame *f) {
    switch (f->colorspace) {
    case AVCOL_SPC_BT709: return engine::YuvMatrix::BT709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return engine::YuvMatrix::BT601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return engine::YuvMatrix::BT2020;
    default: break;
    }
    // Unlabelled: the convention by size.
    return f->height >= 720 ? engine::YuvMatrix::BT709 : engine::YuvMatrix::BT601;
}

// Several decoders on their own threads, for MJPEG, whose every picture
// stands alone and whose decoder cannot share one picture's work between
// cores itself. Each packet goes to the next decoder free; the pictures come
// back in the order the packets were sent, every one of them.
class IntraDecoders {
public:
    // `first` (the input's first picture) is decoded by every decoder before
    // its first job, so tables a file gives only once reach them all.
    // `maxPixels` is the main decoder's limit (0: FFmpeg's own); `finished`
    // is called, with no lock held, each time a picture is done.
    IntraDecoders(const AVCodec *dec, const AVCodecParameters *par, int count, const AVPacket *first, int64_t maxPixels,
                  std::function<void()> finished)
        : finished(std::move(finished)) {
        for (int i = 0; i < count; ++i) {
            AVCodecContext *c = avcodec_alloc_context3(dec);
            if (!c) continue;
            c->thread_count = 1;
            if (maxPixels > 0) c->max_pixels = maxPixels;
            if (avcodec_parameters_to_context(c, par) < 0 || avcodec_open2(c, dec, nullptr) < 0) {
                avcodec_free_context(&c);
                continue;
            }
            threads.emplace_back([this, c, prime = first ? av_packet_clone(first) : nullptr] { work(c, prime); });
        }
    }
    ~IntraDecoders() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            quit = true;
        }
        wake.notify_all();
        for (std::thread &t : threads) t.join();
        clearLocked();
    }
    int count() const { return int(threads.size()); }
    // A picture that took more than its own packet (a field to a packet): these
    // decoders cannot share such a stream.
    bool unsuitable() const {
        std::lock_guard<std::mutex> lock(mutex);
        return split;
    }
    // One on the way for every decoder, and one more waiting for the first free.
    bool full() const {
        std::lock_guard<std::mutex> lock(mutex);
        return sent - collected > uint64_t(threads.size());
    }
    // The next picture in order is done.
    bool ready() const {
        std::lock_guard<std::mutex> lock(mutex);
        return collected != sent && done.count(collected);
    }
    void send(const AVPacket *pkt) {
        AVPacket *copy = av_packet_clone(pkt);
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++sent;
            jobs.push_back(copy);  // a failed clone decodes to nothing, in its turn
        }
        wake.notify_one();
    }
    // The next picture in order: true with it in `out`; false when it is not
    // ready (and `wait` is false) or when nothing is on the way.
    bool receive(AVFrame *out, bool wait) {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            if (collected == sent) return false;
            const auto it = done.find(collected);
            if (it == done.end()) {
                if (!wait) return false;
                arrived.wait(lock);
                continue;
            }
            AVFrame *f = it->second;
            done.erase(it);
            ++collected;
            if (!f) continue;  // that packet held no picture
            av_frame_unref(out);
            av_frame_move_ref(out, f);
            av_frame_free(&f);
            return true;
        }
    }
    // Everything on the way forgotten (a seek, a loop); pictures still being
    // decoded are dropped when they are done.
    void reset() {
        std::lock_guard<std::mutex> lock(mutex);
        clearLocked();
        sent = taken = collected = 0;
        ++generation;
        arrived.notify_all();
    }

private:
    void clearLocked() {
        for (AVPacket *p : jobs) av_packet_free(&p);
        jobs.clear();
        for (auto &[ticket, f] : done) av_frame_free(&f);
        done.clear();
    }
    void work(AVCodecContext *c, AVPacket *prime) {
        AVFrame *f = av_frame_alloc();
        if (prime && f && avcodec_send_packet(c, prime) >= 0 && avcodec_receive_frame(c, f) == AVERROR(EAGAIN)) {
            std::lock_guard<std::mutex> lock(mutex);
            split = true;
        }
        av_frame_free(&f);
        av_packet_free(&prime);
        for (;;) {
            AVPacket *pkt = nullptr;
            uint64_t ticket = 0, round = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return quit || !jobs.empty(); });
                if (quit) break;
                pkt = jobs.front();
                jobs.pop_front();
                ticket = taken++;
                round = generation;
            }
            f = av_frame_alloc();
            int r = !pkt || !f ? AVERROR(ENOMEM) : avcodec_send_packet(c, pkt);
            if (r >= 0) r = avcodec_receive_frame(c, f);
            if (r < 0) av_frame_free(&f);
            av_packet_free(&pkt);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (r == AVERROR(EAGAIN)) split = true;
                if (round == generation) done[ticket] = f;
                else av_frame_free(&f);
            }
            arrived.notify_all();
            if (finished) finished();
        }
        avcodec_free_context(&c);  // pictures still held keep their own buffers
    }

    const std::function<void()> finished;
    mutable std::mutex mutex;
    std::condition_variable wake, arrived;
    std::deque<AVPacket *> jobs;
    std::map<uint64_t, AVFrame *> done;  // by ticket, the order sent
    uint64_t sent = 0, taken = 0, collected = 0, generation = 0;
    bool quit = false, split = false;
    std::vector<std::thread> threads;
};

// Workers running, for MediaStream::settle. Never freed: one the system is
// holding can end after everything static has gone.
struct Workers {
    std::mutex mutex;
    std::condition_variable done;
    int running = 0;
};
Workers &workers() {
    static auto *all = new Workers;
    return *all;
}

// For the tests: every opening of an input, and going on in an open one refused.
std::atomic<uint64_t> inputOpenings{0};
std::atomic<bool> noSeeksInPlace{false};

}  // namespace

struct MediaStream::Impl {
    Options opts;
    std::thread worker;
    std::atomic<bool> quit{false};
    std::atomic<bool> restartWanted{false};
    bool finished = false;              // queueMutex: the worker has let go of everything
    bool waiting = false;               // queueMutex: the worker has done all it was asked and waits for more
    std::condition_variable workerDone;

    // What the program can read as it plays. Here rather than in the stream, as
    // a worker left behind (see stop) goes on writing them for as long as it takes.
    bool m_network = false;
    bool m_listener = false;
    std::atomic<bool> m_hardware{false};
    std::atomic<int> m_width{0}, m_height{0}, m_decoders{0};
    std::atomic<uint64_t> m_decoded{0};
    std::atomic<double> m_duration{0}, m_frameRate{0};
    std::atomic<bool> m_seekable{false}, m_hasVideo{false};
    std::atomic<bool> wrongKey{false};   // this RTMP sender named another application
    std::atomic<bool> m_opened{false};

    mutable std::mutex stateMutex;
    std::string state = "opening";
    std::string codec;
    std::string refusedSound;

    // Decoded, waiting to be due.
    mutable std::mutex queueMutex;
    std::condition_variable space;
    std::deque<Picture> video;
    int64_t overdueUntil = 0;  // the picture the audio thread dropped older ones for
    struct Chunk {
        int64_t due;
        int64_t mediaTimeNs = 0;
        int frames = 0;
        int64_t duration = 0;
        std::vector<float> samples;
    };
    std::deque<Chunk> audio;
    int64_t audioQueuedNs = 0;
    // queueMutex also owns transport requests and the presented file clock.
    uint64_t serial = 0, workSerial = 0;
    double requestedPosition = 0, requestedSpeed = 1;
    bool paused = false, previewRequested = false, clockValid = false;
    bool closed = false;
    int64_t clockMedia = 0, clockDue = 0;
    double workSpeed = 1, seekTarget = 0, origin = 0;
    bool workPaused = false;
    int64_t finishedDue = 0;
    AudioTempo tempo;
    int64_t tempoDue = 0, audioInputEnd = 0, tempoLimit = 0;
    std::vector<float> tempoPending;

    double positionLocked(int64_t now) const {
        double position = requestedPosition;
        if (clockValid) position = clockMedia/1e9 + (paused ? 0 : std::max<int64_t>(0,now-clockDue)/1e9*requestedSpeed);
        const double duration = m_duration.load();
        return duration > 0 ? std::clamp(position,0.,duration) : std::max(0.,position);
    }
    void commandLocked(double position, bool hold, double speed, bool preview) {
        requestedPosition = position; paused = hold; requestedSpeed = speed; previewRequested = preview;
        clockValid = false; ++serial; finishedDue = 0;
        video.clear(); audio.clear(); audioQueuedNs = 0;
        restartWanted.store(true); space.notify_all();
    }
    void clockLocked(int64_t media, int64_t due) {
        clockMedia = media; clockDue = due; clockValid = true;
    }


    // The clock: a frame at time t is due at base + (t - t0) / speed.
    bool anchored = false;
    bool firstVideo = true;
    int64_t base = 0;
    double t0 = 0;
    int64_t lastAudioEnd = 0;
    int64_t nextBase = 0;      // where a loop picks up
    double lastVideoT = 0, frameDuration = 1.0 / 30;
    int networkFailures = 0;       // tries in a row that failed at once

    // Decoding, worker thread only.
    AVFormatContext *fmt = nullptr;
    AVCodecContext *vctx = nullptr, *actx = nullptr;
    int vstream = -1, astream = -1;
    AVPixelFormat hwFormat = AV_PIX_FMT_NONE;
    // A big MJPEG picture decodes on one core: this many decoders share the
    // pictures once decoding is in software (see videoPacket).
    int intraDecoders = 0;
    bool intraOnly = false;
    const AVCodec *videoCodec = nullptr;  // in software
    // Hardware decoders to try, in turn; a decoder can turn a stream down only
    // at its first picture (NVIDIA's takes no 4:2:2 JPEG), hence hwRefused,
    // findDecoder() and reopenDecoder().
    struct HwTry {
        const AVCodec *codec;  // not always the software one: AV1 has two
        AVHWDeviceType type;
        std::string device;  // a render node, or FFmpeg's default
        AVPixelFormat format;
    };
    std::vector<HwTry> hwTries;
    size_t hwNext = 0;
    bool hwRefused = false;
    // Hardware decoders that turned this input's video down, and devices that
    // would not open, so a loop or a seek goes straight to the one that takes it.
    std::set<std::string> refused;
    std::string refusalKey;  // the decoder in use, as `refused` names it
    std::string accepted;    // the hardware decoder that took it
    int failedReads = 0;     // pictures in a row the card did not hand back
    int frameThreads = 0;    // a software decoder's threads, a picture each; 0 with none
    // Hardware devices opened for this input, by type and device, kept over
    // loops and seeks; deviceKey is the one in use.
    std::map<std::string, AVBufferRef *> devices;
    std::string deviceKey;
    // Read while finding the decoder, before play() reads on.
    std::deque<AVPacket *> held;
    // The parallel decoders, kept over loops and seeks while the input's
    // video stays as intraKey says; pooling while this opening uses them.
    std::unique_ptr<IntraDecoders> intra;
    std::string intraKey;
    bool pooling = false;
    bool intraRefused = false;  // its pictures cannot be shared (a field to a packet)
    // A file's own times can jump (see keepTimesContinuous).
    double timeShift = 0, timeEnd = 0;
    bool timeSeen = false;
    // How far past its stated length this input has been read (see keepTimesContinuous).
    double reached = 0;
    SwsContext *sws = nullptr;
    SwrContext *swr = nullptr;
    AVChannelLayout swrLayout{};   // what swr takes
    AVSampleFormat swrFormat = AV_SAMPLE_FMT_NONE;
    int swrRate = 0;
    AVFrame *frame = nullptr, *sw = nullptr;
    AVPacket *pkt = nullptr;

    void setState(const std::string &s) {
        std::lock_guard<std::mutex> lock(stateMutex);
        state = s;
    }
    bool stopping() const { return quit.load() || restartWanted.load(); }
    std::string stateText() const {
        std::string now;
        { std::lock_guard<std::mutex> lock(stateMutex); now = state; }
        std::lock_guard<std::mutex> lock(queueMutex);
        if (closed) return "inactive";
        if (now.rfind("failed",0)==0) return now;
        if (paused) return "paused";
        if (now=="ended" && monotonicNs()<finishedDue) return "playing";
        return now;
    }

    // A file is not stopped in the middle of a read for a new command: it
    // stays open for it (see resumeInput), and a read of it is soon back.
    static int interrupt(void *opaque) {
        const auto *d = static_cast<Impl *>(opaque);
        return d->quit.load() || d->wrongKey.load() || (d->m_network && d->restartWanted.load()) ? 1 : 0;
    }

    static AVPixelFormat pickFormat(AVCodecContext *ctx, const AVPixelFormat *list) {
        auto *impl = static_cast<Impl *>(ctx->opaque);
        for (const AVPixelFormat *p = list; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == impl->hwFormat) return *p;
        // The hardware path is not on offer for this stream: software it is,
        // for this picture at least (see findDecoder).
        impl->hwRefused = impl->hwRefused || impl->hwFormat != AV_PIX_FMT_NONE;
        impl->hwFormat = AV_PIX_FMT_NONE;
        impl->m_hardware.store(false);
        for (const AVPixelFormat *p = list; *p != AV_PIX_FMT_NONE; ++p)
            if (!(av_pix_fmt_desc_get(*p)->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
        return list[0];
    }

    // The decoder that runs on a graphics card for what `dec` decodes: itself, or
    // for AV1, whose preferred decoder is a library that runs only on the
    // processor, FFmpeg's own, which does the reverse.
    static const AVCodec *hardwareDecoder(const AVCodec *dec) {
        if (avcodec_get_hw_config(dec, 0)) return dec;
        void *at = nullptr;
        while (const AVCodec *c = av_codec_iterate(&at))
            // A wrapper around one vendor's decoder has ways of its own.
            if (c->id == dec->id && av_codec_is_decoder(c) && !c->wrapper_name && avcodec_get_hw_config(c, 0)) return c;
        return dec;
    }

    // A video decoder, on the first hardware device that opens for this codec
    // (found, not assumed: every way the decoder offers, VA-API on each
    // graphics card), else in software.
    bool openVideo(const AVStream *st, bool tryHardware) {
        const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
        const auto *desc = av_pix_fmt_desc_get(AVPixelFormat(st->codecpar->format));
        const auto *alphaTag = av_dict_get(st->metadata, "alpha_mode", nullptr, 0);
        const bool alpha = (desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA)) ||
            st->codecpar->alpha_mode != AVALPHA_MODE_UNSPECIFIED || (alphaTag && std::strcmp(alphaTag->value,"1") == 0);
        // WebM stores alpha in a second bitstream. The libvpx decoders expose
        // it; native/hardware VP8/VP9 decoding can return only the colour stream.
        if (alpha && (st->codecpar->codec_id == AV_CODEC_ID_VP8 || st->codecpar->codec_id == AV_CODEC_ID_VP9))
            dec = avcodec_find_decoder_by_name(st->codecpar->codec_id == AV_CODEC_ID_VP9 ? "libvpx-vp9" : "libvpx");
        if (alpha) tryHardware = false;
        if (!dec) return false;
        hwTries.clear();
        hwNext = 0;
        const AVCodec *hw = hardwareDecoder(dec);
        for (int i = 0; tryHardware; ++i) {
            const AVCodecHWConfig *c = avcodec_get_hw_config(hw, i);
            if (!c) break;
            if (!(c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) continue;
            // Vulkan FFV1 currently drops per-frame interlace/order flags.
            // The container can disagree with the bitstream, so its order
            // cannot safely replace them. Decode these streams in software;
            // source processing and compositing still use the chosen GPU.
            if (dec->id == AV_CODEC_ID_FFV1 && c->device_type == AV_HWDEVICE_TYPE_VULKAN &&
                st->codecpar->field_order > AV_FIELD_PROGRESSIVE) continue;
            if (c->device_type == AV_HWDEVICE_TYPE_VAAPI)
                for (const std::string &node : hardware::renderNodes()) hwTries.push_back({hw, c->device_type, node, c->pix_fmt});
            else
                hwTries.push_back({hw, c->device_type, {}, c->pix_fmt});
        }
        const AVCodecDescriptor *about = avcodec_descriptor_get(dec->id);
        intraOnly = about && (about->props & AV_CODEC_PROP_INTRA_ONLY);
        // MJPEG, which decodes a packet to a picture on one core: above 1080p,
        // a few decoders at once (as a camera's). Not other intra-only codecs,
        // which need not keep to a packet a picture.
        intraDecoders = dec->id == AV_CODEC_ID_MJPEG && !intraRefused &&
                !(dec->capabilities & (AV_CODEC_CAP_SLICE_THREADS | AV_CODEC_CAP_FRAME_THREADS))
            ? hardware::jpegDecoders(st->codecpar->width, st->codecpar->height) : 0;
        if (intraDecoders < 2) intraDecoders = 0;
        const std::string key = std::string(dec->name) + ' ' + std::to_string(st->codecpar->format) + ' ' +
            std::to_string(st->codecpar->width) + 'x' + std::to_string(st->codecpar->height) + ' ' + std::to_string(intraDecoders);
        if (intra && (!intraDecoders || key != intraKey)) intra.reset();
        intraKey = key;
        videoCodec = dec;
        return openDecoder(st);
    }

    // The next of hwTries that opens, else software.
    bool openDecoder(const AVStream *st) {
        for (;;) {
            hwFormat = AV_PIX_FMT_NONE;
            hwRefused = false;
            frameThreads = 0;
            refusalKey.clear();
            deviceKey.clear();
            m_hardware.store(false);
            const AVCodec *dec = videoCodec;
            AVBufferRef *dev = nullptr;
            while (hwNext < hwTries.size()) {
                const HwTry &t = hwTries[hwNext++];
                const std::string key = std::to_string(int(t.type)) + ' ' + t.device + ' ' + t.codec->name + ' ' +
                    std::to_string(st->codecpar->format) + ' ' + std::to_string(st->codecpar->width) + 'x' +
                    std::to_string(st->codecpar->height);
                if (refused.count(key)) continue;
                const std::string where = std::to_string(int(t.type)) + ' ' + t.device;
                if (refused.count(where)) continue;
                if (const auto kept = devices.find(where); kept != devices.end()) dev = av_buffer_ref(kept->second);
                else {
                    std::lock_guard<std::recursive_mutex> lock(engine::loaderMutex());
                    // Not there is not there on the next loop of the clip either.
                    if (av_hwdevice_ctx_create(&dev, t.type, t.device.empty() ? nullptr : t.device.c_str(), nullptr, 0) < 0) { refused.insert(where); continue; }
                    devices[where] = av_buffer_ref(dev);
                }
                dec = t.codec;
                hwFormat = t.format;
                refusalKey = key;
                deviceKey = where;
                m_hardware.store(true);
                break;
            }
            vctx = avcodec_alloc_context3(dec);
            avcodec_parameters_to_context(vctx, st->codecpar);
            vctx->opaque = this;
            if (m_network) vctx->max_pixels = kMaxSenderPixels;
            if (m_listener) vctx->codec_whitelist = av_strdup(kListenerDecoders);
            if (hwFormat != AV_PIX_FMT_NONE) {
                vctx->hw_device_ctx = dev;
                vctx->get_format = pickFormat;
            } else {
                softwareThreads(dec, st);
            }
            if (avcodec_open2(vctx, dec, nullptr) >= 0) break;
            {
                std::lock_guard<std::recursive_mutex> lock(engine::loaderMutex());  // may drop the hw device
                avcodec_free_context(&vctx);
            }
            if (hwFormat == AV_PIX_FMT_NONE) return false;  // not even in software
        }
        std::lock_guard<std::mutex> lock(stateMutex);
        codec = vctx->codec->name;
        return true;
    }

    // Every core the decoder can use. Sharing one picture's work goes only as far
    // as its slices do, and most files have one slice a picture, so a file gets a
    // thread a picture too. Each of those holds a picture back, and the reader
    // stays within half a second of the clock: too many, and every picture comes
    // out late, so no more than a quarter of a second's worth. A sender is shown
    // as it arrives, and a paused picture wants only itself: no holding back.
    void softwareThreads(const AVCodec *dec, const AVStream *st) {
        vctx->thread_type = FF_THREAD_SLICE;
        vctx->thread_count = (dec->capabilities & AV_CODEC_CAP_SLICE_THREADS) ? 0 : 1;
        if (dec->capabilities & AV_CODEC_CAP_OTHER_THREADS) {
            // Threads of its own (dav1d, which keeps its delay to a few pictures).
            vctx->thread_count = 0;
            if (m_network) vctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
            return;
        }
        frameThreads = frameThreadsFor(dec, st);
        if (!frameThreads) return;
        vctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        vctx->thread_count = frameThreads;
    }

    // The threads, a picture each, a software decoder gets as things stand
    // (paused or not, the speed); 0 for none.
    int frameThreadsFor(const AVCodec *dec, const AVStream *st) const {
        if (dec->capabilities & AV_CODEC_CAP_OTHER_THREADS) return 0;
        if (m_network || workPaused || !(dec->capabilities & AV_CODEC_CAP_FRAME_THREADS)) return 0;
        const AVRational rate = av_guess_frame_rate(fmt, const_cast<AVStream *>(st), nullptr);
        const double fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 30;
        const int count = std::min({int(fps * workSpeed / 4), av_cpu_count(), 16});
        return count < 2 ? 0 : count;
    }

    // The decoder in use turned the stream down: on to the next that might take
    // it, or to software, which a context opened for hardware has no threads for.
    bool reopenDecoder() {
        refused.insert(refusalKey);
        forgetDevice();
        {
            std::lock_guard<std::recursive_mutex> lock(engine::loaderMutex());  // may drop the hw device
            avcodec_free_context(&vctx);
        }
        if (openDecoder(fmt->streams[vstream])) return true;
        vstream = -1;
        m_hasVideo.store(false);
        m_frameRate.store(0);
        if (astream < 0) setState("failed: the video decoder could not be opened");
        return false;
    }

    // A hardware decoder takes or turns down a stream only at its first
    // picture. Where every picture stands alone, that picture is tried before
    // playing, on each hardware decoder in turn, so the clock never waits for
    // a second try; what was read on the way is kept for play(). One that
    // took the stream before (a loop, a seek) is not tried again.
    bool findDecoder() {
        if (!intraOnly || hwFormat == AV_PIX_FMT_NONE || refusalKey == accepted) return true;
        AVPacket *p = nullptr;
        int64_t heldBytes = 0;
        while (!stopping()) {
            p = av_packet_alloc();
            if (!p || av_read_frame(fmt, p) < 0) { av_packet_free(&p); return true; }
            held.push_back(p);
            if (p->stream_index == vstream) break;
            heldBytes += p->size;
            p = nullptr;
            // No picture in all this (a sender can go on for ever without one):
            // no more is kept, and the decoder as it is finds out when one comes.
            if (int(held.size()) >= kMaxHeldPackets || heldBytes >= kMaxHeldBytes) return true;
        }
        if (!p) return !stopping();
        for (;;) {
            bool decoded = false, slow = false;
            int r = avcodec_send_packet(vctx, p);
            while (r >= 0 && (r = avcodec_receive_frame(vctx, frame)) == 0) {
                decoded = true;
                slow = slow || !readsBackInTime();
            }
            av_frame_unref(frame);
            // Taken at the format but no picture out of it, or none back in
            // time to play it: as bad as turned down.
            const bool failed = slow || (!decoded && hwFormat != AV_PIX_FMT_NONE && r != AVERROR(EAGAIN));
            if (!hwRefused && !failed) {
                if (hwFormat != AV_PIX_FMT_NONE) accepted = refusalKey;
                return true;
            }
            refused.insert(refusalKey);
            forgetDevice();
            {
                std::lock_guard<std::recursive_mutex> lock(engine::loaderMutex());  // may drop the hw device
                avcodec_free_context(&vctx);
            }
            if (!openDecoder(fmt->streams[vstream])) return false;
            if (hwFormat == AV_PIX_FMT_NONE) return true;  // software: nothing left to try
        }
    }

    // Whether `frame` comes back from the graphics card within a picture's time.
    // A card can decode far faster than its memory can be read: an Arc B580 makes
    // 215 pictures a second of 4K 4:2:2 JPEG and hands back 7. The better of two
    // reads, as the first pays for setting up.
    bool readsBackInTime() {
        if (hwFormat == AV_PIX_FMT_NONE || frame->format != hwFormat) return true;
        double best = std::numeric_limits<double>::max();
        for (int i = 0; i < 2; ++i) {
            av_frame_unref(sw);
            const auto start = std::chrono::steady_clock::now();
            if (av_hwframe_transfer_data(sw, frame, 0) < 0) { av_frame_unref(sw); return false; }
            best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        }
        av_frame_unref(sw);
        return best < frameDuration / workSpeed;
    }

    // The device in use turned this input down: not kept for it.
    void forgetDevice() {
        const auto it = devices.find(deviceKey);
        if (it == devices.end()) return;
        std::lock_guard<std::recursive_mutex> lock(engine::loaderMutex());
        av_buffer_unref(&it->second);
        devices.erase(it);
    }

    bool openAudio(const AVStream *st) {
        const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) return false;
        actx = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(actx, st->codecpar);
        if (m_listener) actx->codec_whitelist = av_strdup(kListenerDecoders);
        if (avcodec_open2(actx, dec, nullptr) < 0) {
            avcodec_free_context(&actx);
            return false;
        }
        if (!configureSwr(actx->ch_layout, actx->sample_fmt, actx->sample_rate)) {
            avcodec_free_context(&actx);
            return false;
        }
        return true;
    }

    // The converter for sound in this layout, format and rate. A live sender
    // can change them between frames, so each frame is checked against it.
    bool configureSwr(const AVChannelLayout &in, AVSampleFormat format, int rate) {
        swr_free(&swr);
        av_channel_layout_uninit(&swrLayout);
        AVChannelLayout layout{};
        av_channel_layout_from_string(&layout, AudioFormat::layoutName(opts.audioFormat.channels));
        const bool ok = rate > 0 &&
            swr_alloc_set_opts2(&swr, &layout, AV_SAMPLE_FMT_FLT, opts.audioFormat.rate, &in, format, rate, 0, nullptr) >= 0 &&
            swr_init(swr) >= 0;
        av_channel_layout_uninit(&layout);
        if (!ok) { swr_free(&swr); return false; }
        av_channel_layout_copy(&swrLayout, &in);
        swrFormat = format;
        swrRate = rate;
        return true;
    }

    bool openInput() {
        wrongKey.store(false);
        std::string address = opts.input;
        const bool keyed = m_listener && !opts.passphrase.empty();
        const bool srt = opts.input.rfind("srt", 0) == 0, rtmp = opts.input.rfind("rtmp", 0) == 0;
        if (keyed && !srt && !rtmp) {
            // It would listen open, to someone who thinks it does not.
            setState("failed: this address cannot take a passphrase");
            return false;
        }
        if (keyed && rtmp) {
            // RTMP has no passphrase of its own: it is the application name, which
            // every sender says first and cannot leave out (see AvLogListener). The
            // stream name is no use for it: one that never publishes never says one.
            const size_t path = opts.input.find('/', opts.input.find("://") + 3);
            address = opts.input.substr(0, path) + "/" + opts.passphrase + "/" + kRtmpStreamName;
        }
        fmt = avformat_alloc_context();
        fmt->interrupt_callback = {interrupt, this};
        AVDictionary *o = nullptr;
        if (m_network) {
            // A sender is a camera: start showing it quickly, keep latency low.
            av_dict_set(&o, "probesize", "1000000", 0);
            av_dict_set(&o, "analyzeduration", "1000000", 0);
            av_dict_set(&o, "fflags", "nobuffer", 0);
            // A link that goes quiet (a phone out of range) gives up after
            // this, and listens again, rather than waiting for ever.
            av_dict_set(&o, "rw_timeout", "10000000", 0);
            if (opts.input.rfind("rtmp", 0) == 0) av_dict_set(&o, "listen", "1", 0);
            // Anyone who can reach the port can send here, or anyone who has its
            // passphrase, which proves no more than that. An address someone
            // typed to read from is theirs to choose, and can be anything.
            if (m_listener) {
                av_dict_set(&o, "format_whitelist", kListenerFormats, 0);
                av_dict_set(&o, "codec_whitelist", kListenerDecoders, 0);
            }
            // SRT refuses a sender with another passphrase or none, before it
            // is ever accepted, and listens on. As an option, so never in the URL.
            if (keyed && srt) {
                av_dict_set(&o, "passphrase", opts.passphrase.c_str(), 0);
                av_dict_set(&o, "enforced_encryption", "1", 0);
            }
        }
        inputOpenings.fetch_add(1);
        const int r = avformat_open_input(&fmt, address.c_str(), nullptr, &o);
        av_dict_free(&o);
        if (r < 0) {
            fmt = nullptr;  // freed by avformat_open_input on failure
            if (!stopping() && !wrongKey.load()) {
                char msg[256];
                av_strerror(r, msg, sizeof msg);
                setState(std::string("failed: ") + msg);
            }
            return false;
        }
        // Its decoders too allocate for what a sender claims: the streams known so far.
        std::vector<AVDictionary *> findOptions(fmt->nb_streams, nullptr);
        if (m_network)
            for (AVDictionary *&d : findOptions) av_dict_set_int(&d, "max_pixels", kMaxSenderPixels, 0);
        const int found = avformat_find_stream_info(fmt, m_network && !findOptions.empty() ? findOptions.data() : nullptr);
        for (AVDictionary *&d : findOptions) av_dict_free(&d);
        if (found < 0) {
            if (!stopping()) {
                char msg[256];
                av_strerror(found, msg, sizeof msg);
                setState(std::string("failed: ") + msg);
            }
            return false;
        }
        vstream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vstream, nullptr, 0);
        // OMABROADCAST_NO_HW_DECODE=1: every video in software (a hardware
        // decoder that misbehaves).
        const char *noHardware = std::getenv("OMABROADCAST_NO_HW_DECODE");
        if (vstream >= 0 && !openVideo(fmt->streams[vstream], !(noHardware && std::strcmp(noHardware, "1") == 0))) vstream = -1;
        const AVStream *sound = astream >= 0 ? fmt->streams[astream] : nullptr;
        if (astream >= 0 && !openAudio(fmt->streams[astream])) astream = -1;
        // Otherwise a sender's sound would just not be there, with nothing to say
        // why. One whose decoder was refused while looking may not even be found,
        // as only a decoder could say how many channels it has.
        if (astream < 0 && m_listener) {
            for (unsigned i = 0; !sound && i < fmt->nb_streams; ++i)
                if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && fmt->streams[i]->codecpar->codec_id != AV_CODEC_ID_NONE) sound = fmt->streams[i];
            if (sound) {
                std::lock_guard<std::mutex> lock(stateMutex);
                refusedSound = avcodec_get_name(sound->codecpar->codec_id);
            }
        }
        if (vstream < 0 && astream < 0) {
            setState("failed: nothing playable in it");
            return false;
        }
        if (vstream >= 0) {
            const AVRational r = av_guess_frame_rate(fmt, fmt->streams[vstream], nullptr);
            if (r.num > 0 && r.den > 0) frameDuration = av_q2d(av_inv_q(r));
            m_frameRate.store(r.num > 0 && r.den > 0 ? av_q2d(r) : 0);
        }
        origin = fmt->start_time == AV_NOPTS_VALUE ? 0 : double(fmt->start_time)/AV_TIME_BASE;
        // A length found past the stated one, at an earlier opening, still holds.
        m_duration.store(fmt->duration > 0 ? std::max(double(fmt->duration)/AV_TIME_BASE, reached) : 0);
        m_seekable.store(!m_network && fmt->pb && (fmt->pb->seekable & AVIO_SEEKABLE_NORMAL));
        m_hasVideo.store(vstream >= 0);
        if (!m_network && !seekInput(true)) {
            setState("failed: this file cannot seek to that position"); return false;
        }
        return prepare();
    }

    // A file to the place asked for: one just opened (`fresh`), which is at
    // its start already, or one still open from the last command.
    bool seekInput(bool fresh) {
        const double duration = m_duration.load();
        if (duration > 0) seekTarget = std::min(seekTarget,std::max(0.,duration-0.000001));
        if ((seekTarget > 0 || !fresh) && av_seek_frame(fmt,-1,int64_t((origin+seekTarget)*AV_TIME_BASE),AVSEEK_FLAG_BACKWARD)<0) return false;
        // Preserve offsets between streams, including when seeking into a GOP.
        anchored = false; t0 = origin+seekTarget;
        return true;
    }

    // The last of making an input ready, new or still open: the tempo for
    // this speed, and the decoder its first picture takes.
    bool prepare() {
        if (actx && workSpeed != 1 && !tempo.open(opts.audioFormat.rate,opts.audioFormat.channels,workSpeed)) {
            setState("failed: could not initialize audio tempo conversion"); return false;
        }
        if (vstream >= 0 && !findDecoder()) {
            if (stopping()) return false;
            // The sound plays on without the picture.
            {
                std::lock_guard<std::recursive_mutex> lock(engine::loaderMutex());  // may drop the hw device
                avcodec_free_context(&vctx);
            }
            vstream = -1;
            m_hasVideo.store(false);
            m_frameRate.store(0);
            if (astream < 0) {
                setState("failed: the video decoder could not be opened");
                return false;
            }
        }
        return true;
    }

    // On from a new place in a file still open from the last command (a seek,
    // a step, a pause, a resume, another speed): the container and its
    // decoders stay, emptied of what they held, so nothing is opened or probed
    // again. False when it has to be opened afresh after all: an input that
    // cannot seek or will not go there, a decoder that cannot be had again.
    bool resumeInput() {
        if (m_network || !m_seekable.load() || hwRefused || noSeeksInPlace.load()) return false;
        // To play from the very start (a loop, a restart) it is opened afresh, as
        // it always was: sound can begin before a file's start, and only reading
        // from the top gives a loop those first packets. A picture alone has no
        // such need, but where times can jump the start is not found by its time.
        if (seekTarget <= 0 && (!workPaused || (fmt->iformat->flags & AVFMT_TS_DISCONT))) return false;
        tempo.close();
        av_frame_unref(frame);
        av_frame_unref(sw);
        av_packet_unref(pkt);
        for (AVPacket *p : held) av_packet_free(&p);
        held.clear();
        if (intra) intra->reset();
        if (vstream >= 0) {
            const AVStream *st = fmt->streams[vstream];
            // Turned down while playing (see present), or opened with the threads
            // for a pause and now to play, or the other way: the decoder alone again.
            const bool turnedDown = !refusalKey.empty() && refused.count(refusalKey);
            const bool otherThreads = hwFormat == AV_PIX_FMT_NONE && frameThreadsFor(vctx->codec, st) != frameThreads;
            if (turnedDown || otherThreads) {
                {
                    std::lock_guard<std::recursive_mutex> lock(engine::loaderMutex());  // may drop the hw device
                    avcodec_free_context(&vctx);
                }
                hwNext = 0;
                if (!openDecoder(st)) return false;
            } else avcodec_flush_buffers(vctx);
        }
        if (actx) {
            avcodec_flush_buffers(actx);
            // What the converter still held was from the old place.
            if (swr) {
                swr_close(swr);
                if (swr_init(swr) < 0) swr_free(&swr);  // made again at the next sound (see decodeAudio)
            }
        }
        return seekInput(false) && prepare();
    }

    // `reopening`: closed only to be opened again at once, so still counted as open.
    void closeInput(bool reopening = false) {
        tempo.close();
        std::vector<float>().swap(tempoPending);
        av_frame_unref(frame);
        av_frame_unref(sw);
        av_packet_unref(pkt);
        for (AVPacket *p : held) av_packet_free(&p);
        held.clear();
        if (intra) intra->reset();
        pooling = false;
        intraDecoders = 0;
        m_decoders.store(0);
        if (hwRefused) forgetDevice();  // turned the stream down while playing
        {
            std::lock_guard<std::recursive_mutex> lock(engine::loaderMutex());  // may drop the hw device
            avcodec_free_context(&vctx);
        }
        avcodec_free_context(&actx);
        swr_free(&swr);
        av_channel_layout_uninit(&swrLayout);
        swrFormat = AV_SAMPLE_FMT_NONE;
        swrRate = 0;
        if (fmt) avformat_close_input(&fmt);
        sws_freeContext(sws); sws = nullptr;
        vstream = astream = -1;
        if (!reopening) m_opened.store(false);
    }

    // When something with time t (seconds) is due. Files keep their own pace;
    // a network sender is shown as it arrives, re-anchored when it drifts.
    int64_t dueFor(double t) {
        const int64_t now = monotonicNs();
        if (!anchored) {
            anchored = true;
            t0 = m_network ? t : origin+seekTarget;
            base = !m_network && nextBase > now ? nextBase : now + (m_network || workPaused ? 0 : kFilePrerollNs);
        }
        int64_t due = base + int64_t((t - t0) * 1e9 / workSpeed);
        // Live, the newest decoded frame is due now: never hold a sender back
        // (reading ahead to find its streams would otherwise add a second of
        // latency), and catch up after a stall. Audio shares the clock.
        if (m_network && (due < now - 300'000'000 || due > now + 50'000'000)) {
            t0 = t;
            base = now;
            due = now;
        }
        return due;
    }

    // Waits while enough is decoded ahead; false when it should stop.
    bool waitForRoom() {
        // A live sender never waits: the newest frame wins (see trimLive).
        if (m_network) return !stopping();
        std::unique_lock<std::mutex> lock(queueMutex);
        // Woken as frames are taken, and as the parallel decoders finish a
        // picture; the timeout re-checks for a stop request.
        while (!stopping() && !(int(video.size()) < kMaxVideoAhead && audioQueuedNs < kMaxAudioAheadNs)) {
            // The wait can be for the sound with pictures short: what the
            // parallel decoders have done goes on the queue meanwhile.
            if (pooling && intra->ready()) {
                lock.unlock();
                collectPool(false);
                lock.lock();
                continue;
            }
            space.wait_for(lock, std::chrono::milliseconds(200));
        }
        return !stopping();
    }

    bool toPicture(const AVFrame *src, Picture &p) {
        p.width = src->width;
        p.height = src->height;
        p.matrix = matrixOf(src);
        p.depth = 8;
        p.msb = false;
        // HDR as the file labels it; anything else is read as SDR, as always.
        p.transfer = src->color_trc == AVCOL_TRC_SMPTE2084 ? engine::Transfer::Pq
                   : src->color_trc == AVCOL_TRC_ARIB_STD_B67 ? engine::Transfer::Hlg : engine::Transfer::Sdr;
        p.wideGamut = src->color_primaries == AVCOL_PRI_BT2020;
        p.fullRange = src->color_range == AVCOL_RANGE_JPEG;
        p.interlaced = (src->flags & AV_FRAME_FLAG_INTERLACED) != 0;
        p.firstField = p.interlaced ? ((src->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) ? 0 : 1) : -1;
        const int cw = (src->width + 1) / 2, ch = (src->height + 1) / 2;
        const AVPixelFormat f = AVPixelFormat(src->format);
        const auto *desc = av_pix_fmt_desc_get(f);
        if (!desc) return false;
        p.premultiplied = src->alpha_mode == AVALPHA_MODE_PREMULTIPLIED;
        if (desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) {
            p.rgba = true;
            p.fullRange = src->color_range != AVCOL_RANGE_MPEG;
            if (f == AV_PIX_FMT_RGBA) {
                copyPlane(p.plane[0], &p.stride[0], src->data[0], src->linesize[0], src->width * 4, src->height);
                return true;
            }
            sws = sws_getCachedContext(sws, src->width, src->height, f, src->width, src->height,
                AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            p.stride[0] = src->width * 4; p.plane[0].resize(size_t(p.stride[0]) * src->height);
            uint8_t *dst[4] = {p.plane[0].data(), nullptr, nullptr, nullptr};
            int strides[4] = {p.stride[0], 0, 0, 0};
            return sws && sws_scale(sws, src->data, src->linesize, 0, src->height, dst, strides) == src->height;
        }
        const bool alpha = desc->flags & AV_PIX_FMT_FLAG_ALPHA;
        // Above 8 bits the samples stay as they are (or are widened to 16), so
        // 10-bit and HDR pictures reach the canvas at their own precision.
        const int bits = desc->comp[0].depth;
        if (bits > 8) {
            const bool big = desc->flags & AV_PIX_FMT_FLAG_BE;
            const bool semi = desc->nb_components >= 3 && desc->comp[1].plane == desc->comp[2].plane;
            const bool p420 = desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1, p422 = desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0;
            const bool p444 = desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0;
            if (!big && !alpha && semi && p420 && desc->comp[0].step == 2 && desc->comp[1].step == 4) {
                // P010, P012, P016: interleaved chroma, the code in the high bits.
                p.layout = engine::YuvLayout::NV12;
                p.depth = bits;
                p.msb = desc->comp[0].shift > 0;
                copyPlane(p.plane[0], &p.stride[0], src->data[0], src->linesize[0], src->width * 2, src->height);
                copyPlane(p.plane[1], &p.stride[1], src->data[1], src->linesize[1], cw * 4, ch);
                p.plane[2].clear();
                return true;
            }
            // Three planes at least: grey (gray10, gray12, gray16) has none of
            // chroma and goes through the conversion below.
            if (!big && !semi && desc->nb_components >= 3 && desc->comp[0].shift == 0 && desc->comp[0].step == 2 &&
                bits <= 16 && (p420 || p422 || p444)) {
                // yuv420p10, yuv422p12, yuv444p16 and kin: planes as they are.
                p.layout = p444 ? engine::YuvLayout::I444 : p422 ? engine::YuvLayout::I422 : engine::YuvLayout::I420;
                p.depth = bits;
                const int rows = p420 ? ch : src->height, cols = p444 ? src->width : cw;
                copyPlane(p.plane[0], &p.stride[0], src->data[0], src->linesize[0], src->width * 2, src->height);
                copyPlane(p.plane[1], &p.stride[1], src->data[1], src->linesize[1], cols * 2, rows);
                copyPlane(p.plane[2], &p.stride[2], src->data[2], src->linesize[2], cols * 2, rows);
                if (alpha) copyPlane(p.alpha, &p.alphaStride, src->data[3], src->linesize[3], src->width * 2, src->height);
                return true;
            }
            if (!(desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL))) {
                // Any other deep Y'CbCr: widened to 16-bit planes of its own chroma grid.
                const bool fullChroma = desc->log2_chroma_w == 0;
                const bool everyRow = fullChroma || desc->log2_chroma_h == 0 || p.interlaced;
                const AVPixelFormat target = alpha ? (fullChroma ? AV_PIX_FMT_YUVA444P16LE : everyRow ? AV_PIX_FMT_YUVA422P16LE : AV_PIX_FMT_YUVA420P16LE)
                    : fullChroma ? AV_PIX_FMT_YUV444P16LE : everyRow ? AV_PIX_FMT_YUV422P16LE : AV_PIX_FMT_YUV420P16LE;
                sws = sws_getCachedContext(sws, src->width, src->height, f, src->width, src->height, target,
                                           SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws) return false;
                const int matrix = p.matrix == engine::YuvMatrix::BT709 ? SWS_CS_ITU709 : p.matrix == engine::YuvMatrix::BT2020 ? SWS_CS_BT2020 : SWS_CS_ITU601;
                const auto *coefficients = sws_getCoefficients(matrix);
                sws_setColorspaceDetails(sws, coefficients, p.fullRange, coefficients, p.fullRange, 0, 1 << 16, 1 << 16);
                const int cols = fullChroma ? src->width : cw, rows = everyRow ? src->height : ch;
                p.layout = fullChroma ? engine::YuvLayout::I444 : everyRow ? engine::YuvLayout::I422 : engine::YuvLayout::I420;
                p.depth = 16;
                p.stride[0] = src->width * 2;
                p.stride[1] = p.stride[2] = cols * 2;
                p.plane[0].resize(size_t(p.stride[0]) * src->height);
                p.plane[1].resize(size_t(p.stride[1]) * rows);
                p.plane[2].resize(size_t(p.stride[2]) * rows);
                if (alpha) { p.alphaStride = src->width * 2; p.alpha.resize(size_t(p.alphaStride) * src->height); }
                uint8_t *dst[4] = {p.plane[0].data(), p.plane[1].data(), p.plane[2].data(), alpha ? p.alpha.data() : nullptr};
                int strides[4] = {p.stride[0], p.stride[1], p.stride[2], p.alphaStride};
                return sws_scale(sws, src->data, src->linesize, 0, src->height, dst, strides) == src->height;
            }
        }
        if (f == AV_PIX_FMT_NV12) {
            p.layout = engine::YuvLayout::NV12;
            copyPlane(p.plane[0], &p.stride[0], src->data[0], src->linesize[0], src->width, src->height);
            copyPlane(p.plane[1], &p.stride[1], src->data[1], src->linesize[1], cw * 2, ch);
            p.plane[2].clear();
            return true;
        }
        const bool p420 = f == AV_PIX_FMT_YUV420P || f == AV_PIX_FMT_YUVJ420P || f == AV_PIX_FMT_YUVA420P;
        const bool p422 = f == AV_PIX_FMT_YUV422P || f == AV_PIX_FMT_YUVJ422P || f == AV_PIX_FMT_YUVA422P;
        const bool p444 = f == AV_PIX_FMT_YUV444P || f == AV_PIX_FMT_YUVJ444P || f == AV_PIX_FMT_YUVA444P;
        if (p420 || p422 || p444) {
            if (f == AV_PIX_FMT_YUVJ420P || f == AV_PIX_FMT_YUVJ422P || f == AV_PIX_FMT_YUVJ444P) p.fullRange = true;
            p.layout = p444 ? engine::YuvLayout::I444 : p422 ? engine::YuvLayout::I422 : engine::YuvLayout::I420;
            const int rows = p420 ? ch : src->height, cols = p444 ? src->width : cw;
            copyPlane(p.plane[0], &p.stride[0], src->data[0], src->linesize[0], src->width, src->height);
            copyPlane(p.plane[1], &p.stride[1], src->data[1], src->linesize[1], cols, rows);
            copyPlane(p.plane[2], &p.stride[2], src->data[2], src->linesize[2], cols, rows);
            if (alpha) copyPlane(p.alpha, &p.alphaStride, src->data[3], src->linesize[3], src->width, src->height);
            return true;
        }
        // Keep every chroma row for interlaced fallback formats; reducing them
        // together to 4:2:0 would blend colour between different field times.
        const bool fullChroma = desc->log2_chroma_w == 0;
        // For high-bit-depth alpha formats retain the decoder's chroma grid.
        // In particular, let the engine interpolate interlaced 4:2:0 by field.
        const bool everyRow = fullChroma || (alpha ? desc->log2_chroma_h == 0 : p.interlaced);
        const AVPixelFormat target = alpha ? (fullChroma ? AV_PIX_FMT_YUVA444P : everyRow ? AV_PIX_FMT_YUVA422P : AV_PIX_FMT_YUVA420P)
            : fullChroma ? AV_PIX_FMT_YUV444P : everyRow ? AV_PIX_FMT_YUV422P : AV_PIX_FMT_YUV420P;
        sws = sws_getCachedContext(sws, src->width, src->height, f, src->width, src->height, target,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) return false;
        const int matrix = p.matrix == engine::YuvMatrix::BT709 ? SWS_CS_ITU709 : p.matrix == engine::YuvMatrix::BT2020 ? SWS_CS_BT2020 : SWS_CS_ITU601;
        const auto *coefficients = sws_getCoefficients(matrix);
        sws_setColorspaceDetails(sws, coefficients, p.fullRange, coefficients, p.fullRange, 0, 1 << 16, 1 << 16);
        const int cols = fullChroma ? src->width : cw, rows = everyRow ? src->height : ch;
        p.layout = fullChroma ? engine::YuvLayout::I444 : everyRow ? engine::YuvLayout::I422 : engine::YuvLayout::I420;
        p.stride[0] = src->width;
        p.stride[1] = p.stride[2] = cols;
        p.plane[0].resize(size_t(src->width) * src->height);
        p.plane[1].resize(size_t(cols) * rows);
        p.plane[2].resize(size_t(cols) * rows);
        if (alpha) { p.alphaStride = src->width; p.alpha.resize(size_t(src->width) * src->height); }
        uint8_t *dst[4] = {p.plane[0].data(), p.plane[1].data(), p.plane[2].data(), alpha ? p.alpha.data() : nullptr};
        int strides[4] = {p.stride[0], p.stride[1], p.stride[2], p.alphaStride};
        return sws_scale(sws, src->data, src->linesize, 0, src->height, dst, strides) == src->height;
    }

    double frameTime(const AVFrame *f, const AVStream *st, double fallback) {
        const int64_t ts = f->best_effort_timestamp;
        return ts == AV_NOPTS_VALUE ? fallback : ts * av_q2d(st->time_base);
    }

    // A video packet to its decoder. Once decoding is in software (from the
    // start, or after a hardware decoder turned the stream down) a codec that
    // qualifies goes to several decoders at once.
    void videoPacket(const AVPacket *p) {
        if (pooling && intra->unsuitable()) stopPooling();
        if (!pooling && intraDecoders > 1 && hwFormat == AV_PIX_FMT_NONE) startPooling(p);
        if (!pooling) {
            int sent = avcodec_send_packet(vctx, p);
            // Turned down, at its first picture: this packet again on the next
            // decoder, so no picture is lost.
            while (hwRefused) {
                if (!reopenDecoder()) return;
                sent = avcodec_send_packet(vctx, p);
            }
            if (sent >= 0) collectDecoder();
            return;
        }
        // Full: the oldest picture first, so pictures never pile up unseen.
        collectPool(intra->full());
        if (stopping() || (workPaused && !firstVideo)) return;
        intra->send(p);
        collectPool(false);
    }

    void startPooling(const AVPacket *first) {
        const int count = intraDecoders;
        intraDecoders = 0;  // tried once an opening
        // What the one decoder still holds comes first. It stays open, for
        // a stream that turns out not to share (see stopPooling).
        avcodec_send_packet(vctx, nullptr);
        collectDecoder();
        avcodec_flush_buffers(vctx);
        if (!intra) {
            intra = std::make_unique<IntraDecoders>(videoCodec, fmt->streams[vstream]->codecpar, count, first, vctx->max_pixels,
                                                    [this] {
                                                        { std::lock_guard<std::mutex> lock(queueMutex); }
                                                        space.notify_all();  // see waitForRoom
                                                    });
            if (intra->count() < 2) intra.reset();
        }
        pooling = intra != nullptr;
        m_decoders.store(pooling ? intra->count() : 0);
    }

    // Back to the one decoder for this input, for good: the pictures the
    // others still had are lost.
    void stopPooling() {
        intra.reset();
        pooling = false;
        intraRefused = true;
        m_decoders.store(0);
    }

    // Every picture the one decoder has ready.
    void collectDecoder() {
        while (avcodec_receive_frame(vctx, frame) == 0)
            if (!present()) return;
    }

    // The pictures the parallel decoders have done, in order: with
    // `waitOldest` after waiting for the next, with `waitAll` every one on the way.
    void collectPool(bool waitOldest, bool waitAll = false) {
        for (bool wait = waitOldest || waitAll; intra->receive(frame, wait); wait = waitAll)
            if (!present()) return;
    }

    // `frame` on to the queue; false when no more should be taken now.
    bool present() {
        const double t = frameTime(frame, fmt->streams[vstream], lastVideoT + frameDuration);
        lastVideoT = t;
        const double duration = frame->duration > 0 ? frame->duration * av_q2d(fmt->streams[vstream]->time_base) : frameDuration;
        // Before the place asked for: not shown, so not read back from the card either.
        if (!m_network && t+duration <= origin+seekTarget+1e-8) return true;
        const AVFrame *src = frame;
        if (frame->format == hwFormat && hwFormat != AV_PIX_FMT_NONE) {
            av_frame_unref(sw);
            if (av_hwframe_transfer_data(sw, frame, 0) < 0) {
                if (++failedReads < kMaxFailedReads) return true;
                // A card that has stopped handing pictures back: from here on another
                // decoder, or software, rather than a clip frozen and "playing".
                failedReads = 0;
                refused.insert(refusalKey);
                forgetDevice();
                std::lock_guard<std::mutex> lock(queueMutex);
                commandLocked(positionLocked(monotonicNs()), paused, requestedSpeed, paused);
                return false;
            }
            failedReads = 0;
            av_frame_copy_props(sw, frame);
            src = sw;
        }
        if (stopping()) return false;
        Picture p;
        if (!toPicture(src, p)) { setState("failed: unsupported decoded picture format"); return true; }
        const auto presented = m_network ? t : std::max(t,origin+seekTarget);
        p.timeNs = dueFor(presented);
        p.mediaTimeNs = int64_t(std::max(0.,presented-origin)*1e9);
        p.discontinuity = firstVideo; firstVideo = false;
        p.durationNs = std::max<int64_t>(1, int64_t(duration * 1e9 / workSpeed));
        m_width.store(p.width);
        m_height.store(p.height);
        m_decoded.fetch_add(1);
        std::lock_guard<std::mutex> lock(queueMutex);
        if (workSerial != serial) return false;
        video.push_back(std::move(p));
        trimLive();
        return !workPaused;
    }

    // Live: drop the oldest rather than fall behind the sender. Caller locks.
    void trimLive() {
        if (!m_network) return;
        while (int(video.size()) > kMaxVideoAhead) {
            video[1].discontinuity |= video.front().discontinuity;
            video.pop_front();
        }
        while (audioQueuedNs > kMaxAudioAheadNs && !audio.empty()) {
            audioQueuedNs -= audio.front().duration;
            audio.pop_front();
        }
    }

    void queueAudio(Chunk c) {
        c.duration = int64_t(c.frames)*1'000'000'000ll/opts.audioFormat.rate;
        c.mediaTimeNs = int64_t((t0-origin)*1e9 + (c.due-base)*workSpeed);
        lastAudioEnd = c.due+c.duration;
        std::lock_guard<std::mutex> lock(queueMutex);
        if (workSerial != serial || workPaused) return;
        audioQueuedNs += c.duration; audio.push_back(std::move(c)); trimLive();
    }
    void drainTempo() {
        std::vector<float> samples;
        while (tempo.take(samples)>0) tempoPending.insert(tempoPending.end(),samples.begin(),samples.end());
        // Hold any overlap/add surplus until the corresponding input time is
        // known. At EOF this also prevents a short clip from running long.
        const auto limit = tempoLimit ? tempoLimit : audioInputEnd;
        const int frames = std::min<int>(tempoPending.size()/opts.audioFormat.channels,
            int(std::max(0.,std::round((limit-tempoDue)*opts.audioFormat.rate/1e9))));
        if (!frames) return;
        Chunk c; c.frames=frames; c.due=tempoDue;
        const auto end=tempoPending.begin()+size_t(frames)*opts.audioFormat.channels;
        c.samples.assign(tempoPending.begin(),end); tempoPending.erase(tempoPending.begin(),end);
        tempoDue += int64_t(frames)*1'000'000'000ll/opts.audioFormat.rate;
        queueAudio(std::move(c));
    }
    void flushTempo(int64_t end) {
        // Supply lookahead for the final overlap/add windows, then retain
        // exactly the requested duration. Otherwise short clips at slow speeds
        // lose a portion of their tail and each loop advances the audio clock.
        tempoLimit = end;
        std::vector<float> silence(size_t(opts.audioFormat.rate/4)*opts.audioFormat.channels,0.f);
        tempo.push(silence.data(),opts.audioFormat.rate/4); drainTempo();
        tempo.push(nullptr,0); drainTempo();
        tempoLimit = 0; tempoPending.clear();
    }
    void convertedAudio(std::vector<float> samples, int frames, double time) {
        if (workPaused || stopping()) return;
        if (!m_network) {
            const int skip = int(std::clamp(std::ceil((origin+seekTarget-time)*opts.audioFormat.rate-1e-5),0.,double(frames)));
            if (skip == frames) return;
            samples.erase(samples.begin(),samples.begin()+size_t(skip)*opts.audioFormat.channels);
            frames -= skip; time += double(skip)/opts.audioFormat.rate;
        }
        for (float &sample : samples) sample *= opts.volume;
        int64_t due = dueFor(time);
        const int64_t tickNs = av_rescale_q_rnd(1,fmt->streams[astream]->time_base,AVRational{1,1'000'000'000},AV_ROUND_UP);
        const int64_t tolerance = std::max<int64_t>(1'000'000'000ll/opts.audioFormat.rate+1,std::min<int64_t>(2'000'000,int64_t(tickNs/workSpeed)+1));
        const bool continuous = audioInputEnd && std::abs(due-audioInputEnd) <= tolerance;
        if (continuous) due = audioInputEnd;
        const auto previousEnd = audioInputEnd;
        audioInputEnd = due+int64_t(frames*1e9/opts.audioFormat.rate/workSpeed);
        if (workSpeed == 1) {
            Chunk c; c.due=due; c.frames=frames; c.samples=std::move(samples); queueAudio(std::move(c));
        } else {
            if (!tempoDue || !continuous) {
                if (tempoDue) { flushTempo(previousEnd); tempo.open(opts.audioFormat.rate,opts.audioFormat.channels,workSpeed); }
                tempoDue = due;
            }
            if (tempo.push(samples.data(),frames)) drainTempo();
        }
    }
    void decodeAudio() {
        while (avcodec_receive_frame(actx,frame) == 0) {
            const double t = frameTime(frame,fmt->streams[astream],origin+seekTarget);
            // A decoder that leaves any of these unset means the stream's own.
            const AVChannelLayout &inLayout = frame->ch_layout.nb_channels > 0 ? frame->ch_layout : actx->ch_layout;
            const AVSampleFormat inFormat = frame->format >= 0 ? AVSampleFormat(frame->format) : actx->sample_fmt;
            const int inRate = frame->sample_rate > 0 ? frame->sample_rate : actx->sample_rate;
            if (!swr || inRate != swrRate || inFormat != swrFormat || av_channel_layout_compare(&inLayout, &swrLayout) != 0) {
                // What the converter still held was in the old format: dropped.
                if (!configureSwr(inLayout, inFormat, inRate)) continue;
            }
            const int out = swr_get_out_samples(swr,frame->nb_samples);
            if (out <= 0) continue;
            std::vector<float> samples(size_t(out)*opts.audioFormat.channels);
            uint8_t *dst = reinterpret_cast<uint8_t *>(samples.data());
            const int64_t delay = swr_get_delay(swr,swrRate);
            const int n = swr_convert(swr,&dst,out,const_cast<const uint8_t **>(frame->extended_data),frame->nb_samples);
            if (n <= 0) continue;
            samples.resize(size_t(n)*opts.audioFormat.channels);
            convertedAudio(std::move(samples),n,t-double(delay)/actx->sample_rate);
        }
    }

    // A file's own times can jump: MPEG-TS files joined, a camera's split clips,
    // damage. Forward, the next picture and sound would be due that much later and
    // the clip would freeze; back, everything would be due already and the rest
    // would be decoded flat out. Such a jump is closed up here, for every stream
    // at once, so they stay together. Packets from all the streams are compared,
    // so a gap in one stream alone, which is no jump, passes untouched. Containers
    // with one timeline can have long gaps with nothing in them: there it takes
    // a leap to past the file's own end, or far backwards. Such a file can also
    // simply run on past the length it states (one still being written when it
    // was opened): that is no jump, and its length grows with what is read.
    void keepTimesContinuous(AVPacket *p) {
        if (m_network || (p->stream_index != vstream && p->stream_index != astream)) return;
        const int64_t stamp = p->dts != AV_NOPTS_VALUE ? p->dts : p->pts;
        if (stamp == AV_NOPTS_VALUE) return;
        const double unit = av_q2d(fmt->streams[p->stream_index]->time_base);
        double t = stamp * unit + timeShift;
        const bool expected = fmt->iformat->flags & AVFMT_TS_DISCONT;
        if (timeSeen) {
            const double step = t - timeEnd, duration = m_duration.load();
            const bool jumped = expected ? std::abs(step) > kTimeJump
                                         : step < -kTimeJumpLong ||
                                           (duration > 0 && step > kTimeJump && t > origin + duration + kTimeJumpLong);
            if (jumped) {
                timeShift -= step;
                t -= step;
            }
        }
        const double end = t + std::max<int64_t>(p->duration, 0) * unit;
        // Where the jumps are expected, as read; where not, packets of one stream
        // can run seconds ahead of another's and the latest is what counts.
        timeEnd = expected || !timeSeen ? end : std::max(timeEnd, end);
        timeSeen = true;
        if (!expected) {
            // By when it is shown, where the packet says: the last picture's end is the file's.
            const double shown = p->pts != AV_NOPTS_VALUE ? std::max(end, p->pts * unit + timeShift + std::max<int64_t>(p->duration, 0) * unit) : end;
            const double stated = m_duration.load();
            if (stated > 0 && shown - origin > stated) {
                reached = shown - origin;
                m_duration.store(reached);
            }
        }
        if (timeShift == 0) return;
        const int64_t shift = std::llrint(timeShift / unit);
        if (p->pts != AV_NOPTS_VALUE) p->pts += shift;
        if (p->dts != AV_NOPTS_VALUE) p->dts += shift;
    }

    // Plays one opening of the input; returns at its end, on a fault, or when
    // asked to stop or restart.
    void play() {
        while (!stopping() && !(workPaused && !firstVideo)) {
            if (pooling) collectPool(false);  // what the decoders finished meanwhile
            if (!waitForRoom()) return;
            int r = 0;
            if (!held.empty()) {
                av_packet_move_ref(pkt, held.front());
                av_packet_free(&held.front());
                held.pop_front();
            } else r = av_read_frame(fmt, pkt);
            if (r < 0) break;  // the end, or the sender went away
            keepTimesContinuous(pkt);
            if (pkt->stream_index == vstream) videoPacket(pkt);
            else if (pkt->stream_index == astream && avcodec_send_packet(actx, pkt) >= 0) decodeAudio();
            av_packet_unref(pkt);
        }
        if (stopping() || (workPaused && !firstVideo)) return;
        // Drain what the decoders still hold.
        // A paused seek near EOF may need a delayed final picture (B-frames).
        // Drain video until that held picture is available, without audio.
        if (pooling) collectPool(false, true);
        else if (vctx) {
            avcodec_send_packet(vctx, nullptr);
            collectDecoder();
        }
        if (workPaused) return;
        if (actx) {
            avcodec_send_packet(actx, nullptr);
            decodeAudio();
            // Resampling needs future samples. Flush its final lookahead so a
            // changed sample rate cannot truncate the end of a clip.
            while (swr) {
                const int capacity = swr_get_out_samples(swr, 0);
                if (capacity <= 0) break;
                std::vector<float> samples(size_t(capacity)*opts.audioFormat.channels);
                uint8_t *dst = reinterpret_cast<uint8_t *>(samples.data());
                const int frames = swr_convert(swr,&dst,capacity,nullptr,0);
                if (frames <= 0) break;
                samples.resize(size_t(frames)*opts.audioFormat.channels);
                const double time = t0 + (audioInputEnd-base)/1e9*workSpeed;
                convertedAudio(std::move(samples),frames,time);
            }
            if (workSpeed != 1) flushTempo(audioInputEnd);
        }
        // A loop carries on one frame after the last.
        nextBase = std::max(lastAudioEnd, base + int64_t((lastVideoT - t0 + frameDuration) * 1e9/workSpeed));
    }

    // What is kept over loops and seeks, let go while nothing plays. Never
    // with queueMutex held: a decoder finishing takes it.
    void releaseKept() {
        intra.reset();
        intraKey.clear();
        std::lock_guard<std::recursive_mutex> lock(engine::loaderMutex());
        for (auto &[where, dev] : devices) av_buffer_unref(&dev);
        devices.clear();
    }

    // A pass starts: a command, a loop, or an opening tried again.
    void beginPass() {
        tempoPending.clear();
        anchored = false; firstVideo = true; lastAudioEnd = audioInputEnd = tempoDue = 0; lastVideoT = 0;
        timeShift = timeEnd = 0; timeSeen = false; failedReads = 0;
    }

    // Waits for the next command, with everything asked for done. A file left
    // open (paused on a picture, for the next step or seek) is let go when
    // none comes for a while; with nothing open, so is what was kept.
    void waitForCommand(std::unique_lock<std::mutex> &lock) {
        const auto asked = [&] { return quit.load() || restartWanted.load(); };
        waiting = true;
        if (!fmt || !space.wait_for(lock, kIdleOpen, asked)) {
            lock.unlock();
            closeInput();
            releaseKept();
            lock.lock();
        }
        space.wait(lock, asked);
        waiting = false;
    }

    void run() {
        const bool keyed = m_listener && !opts.passphrase.empty();
        const AvLogListener watching(keyed ? opts.passphrase : std::string(), keyed ? &wrongKey : nullptr);
        // What earlier inputs taught does not hold for this one.
        refused.clear();
        accepted.clear();
        intraRefused = false;
        frame = av_frame_alloc();
        sw = av_frame_alloc();
        pkt = av_packet_alloc();
        bool initial = true;
        while (!quit.load()) {
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                const bool requested = restartWanted.exchange(false);
                if (initial || requested) {
                    initial = false; workSerial = serial; workSpeed = requestedSpeed;
                    workPaused = paused; seekTarget = requestedPosition;
                    nextBase = 0;
                } else seekTarget = 0; // natural loop
                if (closed) {
                    setState("inactive");
                    lock.unlock(); closeInput(); lock.lock();
                    waitForCommand(lock);
                    continue;
                }
                if (workPaused && !previewRequested) {
                    // The picture showing stays; the file, if open, waits for what comes next.
                    setState("paused");
                    waitForCommand(lock);
                    continue;
                }
                previewRequested = false;
                // Publish the opening attempt under the same lock as closing.
                // Once setClosed() returns, a false opened flag must not race
                // with an attempt that already passed the closed check.
                m_opened.store(true);
            }
            beginPass();
            int64_t openedAt = monotonicNs();
            // Still open from the last command: on from the new place in it.
            bool reused = fmt && resumeInput();
            bool played = reused;
            for (;;) {
                if (!reused) {
                    closeInput(true);
                    played = false;
                    if (stopping()) break;
                    setState(m_network ? "listening" : "opening");
                    {
                        std::lock_guard<std::mutex> lock(stateMutex);
                        refusedSound.clear();
                    }
                    openedAt = monotonicNs();
                    played = openInput();
                }
                if (played) {
                    setState(workPaused ? "paused" : "playing");
                    if (!workPaused || vstream >= 0) play();
                }
                // Nothing came of going on in the open file (an index that
                // lies, a seek that only said it worked): once more from a new
                // opening, which is what a command always had.
                if (!reused || stopping() || !firstVideo || lastAudioEnd > 0 || (workPaused && vstream < 0)) break;
                reused = false;
                beginPass();
            }
            // A file stays open for the next command or loop; a sender is listened for anew.
            if (!played || m_network || quit.load()) closeInput();
            if (quit.load() || restartWanted.load()) continue;
            if (m_network) {
                std::unique_lock<std::mutex> lock(queueMutex);
                // What did arrive is still heard: a short file from a fast server
                // is all on the queue by the time the input ends. Nobody may be
                // taking it (paused, no sink), so not for ever.
                space.wait_for(lock, std::chrono::nanoseconds(audioQueuedNs) + std::chrono::milliseconds(500),
                               [&] { return quit.load() || restartWanted.load() || audio.empty(); });
                video.clear(); audio.clear(); audioQueuedNs = 0;
                // Failing at once (the port taken, the server down) waits a
                // little longer each time before the next try.
                // Nor is a sender turned away a failure: it must not keep the right one out.
                if (wrongKey.exchange(false) || (played && monotonicNs() - openedAt > 2'000'000'000ll)) networkFailures = 0;
                else ++networkFailures;
                if (networkFailures > 0) {
                    const auto wait = std::chrono::milliseconds(std::min(5000, 250 << std::min(networkFailures - 1, 5)));
                    space.wait_for(lock, wait, [&] { return quit.load() || restartWanted.load(); });
                }
                continue;
            }
            if (!workPaused && opts.loop && (!firstVideo || lastAudioEnd > 0)) continue;
            // Played to its end: let go, with what was kept. Paused on a
            // picture it stays open: scrubbing goes on in it at once. Before
            // it says "ended": closing a decoder on many threads takes a
            // moment, and the last pictures are still waiting to be shown.
            if (!workPaused) closeInput();
            const bool failed = stateText().rfind("failed",0) == 0;
            std::unique_lock<std::mutex> lock(queueMutex);
            finishedDue = nextBase;  // "playing" until then (see stateText), so set first
            if (!failed) setState(workPaused ? "paused" : "ended");
            waitForCommand(lock);
        }
        closeInput();
        releaseKept();
        sws_freeContext(sws);
        sws = nullptr;
        av_frame_free(&frame);
        av_frame_free(&sw);
        av_packet_free(&pkt);
    }
};

MediaStream::MediaStream() : m_d(std::make_shared<Impl>()) {}

MediaStream::~MediaStream() { stop(); }

bool MediaStream::isNetwork(const std::string &input) {
    const size_t at = input.find("://");
    return at != std::string::npos && input.compare(0, at, "file") != 0;
}

bool MediaStream::isListener(const std::string &input) {
    if (!isNetwork(input)) return false;
    const size_t at = input.find("://");
    const std::string scheme = input.substr(0, at);
    // RTMP is only ever told to listen here (see openInput), and reading UDP takes
    // from whoever writes to the port, whatever address it names.
    if (scheme.rfind("rtmp", 0) == 0 || scheme == "udp") return true;
    // What follows a web address is for the server; "listen" in it asks nothing here.
    if (scheme == "http" || scheme == "https") return false;
    // The rest say so in their query, where FFmpeg reads it.
    for (size_t from = input.find('?', at); from != std::string::npos;) {
        const size_t end = input.find('&', ++from);
        const std::string param = input.substr(from, end == std::string::npos ? end : end - from);
        if (param == "mode=listener" || param == "listen" || (param.rfind("listen=", 0) == 0 && param != "listen=0")) return true;
        from = end;
    }
    return false;
}

uint64_t MediaStream::openings() { return inputOpenings.load(); }
void MediaStream::refuseSeeksInPlace(bool refuse) { noSeeksInPlace.store(refuse); }

int MediaStream::settle(int milliseconds) {
    Workers &all = workers();
    std::unique_lock<std::mutex> lock(all.mutex);
    all.done.wait_for(lock, std::chrono::milliseconds(milliseconds), [&] { return all.running == 0; });
    return all.running;
}

bool MediaStream::start(const Options &options) {
    stop();
    if (!options.passphrase.empty()) keepSecretsOutOfAvLog();
    const std::shared_ptr<Impl> d = m_d.load();
    d->opts = options;
    d->wrongKey.store(false);
    d->opts.audioFormat = options.audioFormat.normalized();
    {
        std::lock_guard<std::mutex> lock(d->queueMutex);
        d->requestedPosition = 0; d->requestedSpeed = isNetwork(options.input) ? 1 : std::clamp(std::isfinite(options.speed) ? options.speed : 1.,.25,4.);
        d->paused = false; d->previewRequested = false; d->clockValid = false; d->finishedDue = 0; ++d->serial;
        d->closed = options.closed;
        d->finished = false;
        d->waiting = false;
    }
    d->reached = 0;
    d->m_duration.store(0); d->m_seekable.store(false); d->m_hasVideo.store(false);
    d->m_network = isNetwork(options.input);
    d->m_listener = isListener(options.input);
    d->m_decoded.store(0);
    d->quit.store(false);
    d->restartWanted.store(false);
    {
        std::lock_guard<std::mutex> lock(workers().mutex);
        ++workers().running;
    }
    d->worker = std::thread([d = d]() mutable {
        d->run();
        {
            std::lock_guard<std::mutex> lock(d->queueMutex);
            d->finished = true;
            d->workerDone.notify_all();
        }
        // Left behind, this is the last of its state: gone before it counts as gone.
        d.reset();
        std::lock_guard<std::mutex> lock(workers().mutex);
        --workers().running;
        workers().done.notify_all();
    });
    return true;
}

void MediaStream::stop() {
    const std::shared_ptr<Impl> d = m_d.load();
    if (!d->worker.joinable()) return;
    std::unique_lock<std::mutex> lock(d->queueMutex);
    // Under the lock the worker's waits check it with, or the wake-up can go unheard.
    d->quit.store(true);
    d->space.notify_all();
    // It closes its decoders and goes, in moments. But FFmpeg cannot interrupt
    // an open or a read the system is holding (a FIFO no one writes to, a mount
    // that has gone away), and nothing should wait for those: the worker is left
    // to end by itself if that ever returns, with its state, and this stream
    // goes on with new state.
    if (!d->workerDone.wait_for(lock, std::chrono::seconds(1), [&] { return d->finished; })) {
        lock.unlock();
        d->worker.detach();
        m_d.store(std::make_shared<Impl>());
        return;
    }
    lock.unlock();
    d->worker.join();
    lock.lock();
    d->video.clear();
    d->audio.clear();
    d->audioQueuedNs = 0;
}

void MediaStream::restart() {
    const auto d = m_d.load();
    if (d->m_network) return;
    std::lock_guard<std::mutex> lock(d->queueMutex);
    if (d->closed) return;
    d->commandLocked(0,false,d->requestedSpeed,true);
}
void MediaStream::setClosed(bool closed) {
    const auto d = m_d.load();
    std::lock_guard<std::mutex> lock(d->queueMutex);
    if (d->closed == closed) return;
    d->closed = closed;
    d->commandLocked(0,false,d->requestedSpeed,false);
}
bool MediaStream::closed() const {
    const auto d = m_d.load();
    std::lock_guard<std::mutex> lock(d->queueMutex);
    return d->closed;
}
MediaStream::Playback MediaStream::playback() const {
    const auto d = m_d.load();
    std::lock_guard<std::mutex> lock(d->queueMutex);
    return {d->positionLocked(monotonicNs()),d->m_duration.load(),d->requestedSpeed,d->paused,d->m_seekable.load()};
}
bool MediaStream::seek(double seconds) {
    const auto d = m_d.load();
    if (d->m_network || !d->m_seekable.load() || !std::isfinite(seconds)) return false;
    std::lock_guard<std::mutex> lock(d->queueMutex);
    if (d->closed) return false;
    const double duration = d->m_duration.load();
    d->commandLocked(duration > 0 ? std::clamp(seconds,0.,duration) : std::max(0.,seconds),d->paused,d->requestedSpeed,true);
    return true;
}
bool MediaStream::settled() const {
    const auto d = m_d.load();
    std::lock_guard<std::mutex> lock(d->queueMutex);
    return d->waiting && !d->restartWanted.load() && d->video.empty();
}
bool MediaStream::setPaused(bool paused) {
    const auto d = m_d.load();
    if (d->m_network) return false;
    std::lock_guard<std::mutex> lock(d->queueMutex);
    if (d->closed) return false;
    if (paused == d->paused) return true;
    double at = d->positionLocked(monotonicNs());
    if (!paused && d->m_duration.load()>0 && at >= d->m_duration.load()-.001) at=0;
    d->commandLocked(at,paused,d->requestedSpeed,paused && !d->clockValid); return true;
}
bool MediaStream::setSpeed(double speed) {
    const auto d = m_d.load();
    if (d->m_network || !std::isfinite(speed)) return false;
    speed = std::clamp(speed,.25,4.);
    std::lock_guard<std::mutex> lock(d->queueMutex);
    if (speed == d->requestedSpeed) return true;
    d->commandLocked(d->positionLocked(monotonicNs()),d->paused,speed,d->paused); return true;
}

bool MediaStream::acquire(Picture &out, int64_t nowNs) {
    const auto d = m_d.load();
    std::lock_guard<std::mutex> lock(d->queueMutex);
    bool got = false, discontinuity = false;
    // The frame thread's time was taken before it waited for the GPU's last
    // picture. What pumpAudio kept as due by now is due here too, or a GPU slower
    // than the clip's frames would only ever find pictures not due yet.
    const int64_t due = std::max(nowNs, d->overdueUntil);
    while (!d->video.empty() && d->video.front().timeNs <= due) {
        discontinuity |= d->video.front().discontinuity;
        std::swap(out, d->video.front());
        d->video.pop_front();
        got = true;
    }
    if (got) { out.discontinuity |= discontinuity; d->clockLocked(out.mediaTimeNs,out.timeNs); d->space.notify_all(); }
    return got;
}

void MediaStream::pumpAudio(int64_t nowNs, const AudioSink &sink) {
    const auto d = m_d.load();
    std::vector<Impl::Chunk> due;
    {
        std::lock_guard<std::mutex> lock(d->queueMutex);
        if (d->paused || d->restartWanted.load()) return;
        // A stalled/slow renderer must not fill the picture queue and stop
        // the demuxer feeding audio. Keep the newest due picture for acquire().
        while (d->video.size() > 1 && d->video[1].timeNs <= nowNs) {
            d->video[1].discontinuity |= d->video.front().discontinuity;
            d->video.pop_front();
            d->overdueUntil = d->video.front().timeNs;
        }
        while (!d->audio.empty() && d->audio.front().due <= nowNs + kAudioEarlyNs) {
            d->audioQueuedNs -= d->audio.front().duration;
            if (!d->m_hasVideo.load()) d->clockLocked(d->audio.front().mediaTimeNs,d->audio.front().due);
            due.push_back(std::move(d->audio.front()));
            d->audio.pop_front();
        }
    }
    d->space.notify_all();
    for (const Impl::Chunk &c : due) sink(c.due, c.samples.data(), c.frames);
}

std::string MediaStream::state() const { return m_d.load()->stateText(); }

bool MediaStream::opened() const { return m_d.load()->m_opened.load(); }
bool MediaStream::network() const { return m_d.load()->m_network; }
bool MediaStream::hardware() const { return m_d.load()->m_hardware.load(); }
int MediaStream::width() const { return m_d.load()->m_width.load(); }
int MediaStream::height() const { return m_d.load()->m_height.load(); }
double MediaStream::frameRate() const { return m_d.load()->m_frameRate.load(); }
uint64_t MediaStream::framesDecoded() const { return m_d.load()->m_decoded.load(); }
int MediaStream::parallelDecoders() const { return m_d.load()->m_decoders.load(); }

std::string MediaStream::refusedSound() const {
    const auto d = m_d.load();
    std::lock_guard<std::mutex> lock(d->stateMutex);
    return d->refusedSound;
}

std::string MediaStream::codec() const {
    const auto d = m_d.load();
    std::lock_guard<std::mutex> lock(d->stateMutex);
    return d->codec;
}
