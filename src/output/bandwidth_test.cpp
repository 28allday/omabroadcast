#include "bandwidth_test.h"
#include "encoders.h"

#include <QMetaObject>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
}

#include <time.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <random>

#include "output/encoder_helper.h"
#include "output/services.h"
#include "output/stream_sink.h"

namespace {

constexpr int kWidth = 1280, kHeight = 720, kFps = 30;
constexpr int kStartKbps = 2500, kDefaultTopKbps = 20000;
constexpr int kAudioRate = 48000;
constexpr int64_t kSecond = 1'000'000'000;
constexpr int64_t kLeadNs = 150'000'000;        // fed this far ahead of the clock
constexpr int64_t kMinStepNs = 1'500'000'000;
constexpr int kPassLagMs = 150, kPassPeakLagMs = 350, kBlockedLagMs = 100;
constexpr int64_t kStallNs = 400'000'000;

int64_t monotonicNow() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * kSecond + ts.tv_nsec;
}

std::shared_ptr<AVPacket> own(AVPacket *p) {
    return std::shared_ptr<AVPacket>(p, [](AVPacket *q) { av_packet_free(&q); });
}

// One second of pictures at one bitrate, starting on a keyframe.
struct Burst {
    int kbps = 0;                    // what it really holds, per second
    std::vector<std::shared_ptr<AVPacket>> video;
};

struct Content {
    std::shared_ptr<EncodedLayout> layout;
    std::vector<Burst> bursts;
    std::vector<std::shared_ptr<AVPacket>> audio;   // pts from 0 within the loop
    int64_t audioSpan = 0;                          // samples in one loop
};

bool makeVideo(const std::vector<int> &rates, Content &c, std::string *error, const std::atomic<bool> &cancel) {
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        *error = BandwidthTest::tr("This FFmpeg has no software H.264 encoder to make the test pictures.").toStdString();
        return false;
    }
    std::vector<engine::YuvFrame> pictures;
    for (int i = 0; i < kFps && !cancel.load(); ++i)
        pictures.push_back(encoderhelper::busyPicture(kWidth, kHeight, engine::ColourFormat::I420, i));
    AVFrame *frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = kWidth;
    frame->height = kHeight;
    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        *error = "frame buffer";
        return false;
    }
    std::vector<uint8_t> extradata;
    bool ok = true;
    for (int rate : rates) {
        if (cancel.load()) { ok = false; break; }
        AVCodecContext *ctx = avcodec_alloc_context3(codec);
        ctx->width = kWidth;
        ctx->height = kHeight;
        ctx->time_base = {1, kFps};
        ctx->framerate = {kFps, 1};
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        ctx->gop_size = kFps;
        ctx->max_b_frames = 0;
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        // A high quality held to the step's rate by the buffer: the busy
        // picture wants more, so each second comes out at about the rate.
        // A half-second buffer that starts nearly empty, so the second is
        // not front-loaded beyond the rate.
        ctx->rc_max_rate = int64_t(rate) * 1000;
        ctx->rc_buffer_size = int(ctx->rc_max_rate / 2);
        ctx->rc_initial_buffer_occupancy = ctx->rc_buffer_size / 10;
        av_opt_set(ctx->priv_data, "crf", "8", 0);
        av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
        // A fixed level keeps the stream headers the same at every rate:
        // the server is told them once.
        av_opt_set(ctx->priv_data, "level", "5.1", 0);
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            *error = BandwidthTest::tr("The software H.264 encoder would not open for the test pictures.").toStdString();
            ok = false;
            break;
        }
        const std::vector<uint8_t> headers(ctx->extradata, ctx->extradata + ctx->extradata_size);
        if (c.bursts.empty()) {
            extradata = headers;
            c.layout->video = avcodec_parameters_alloc();
            avcodec_parameters_from_context(c.layout->video, ctx);
            c.layout->videoTimeBase = ctx->time_base;
            c.layout->frameRate = ctx->framerate;
        } else if (headers != extradata) {
            avcodec_free_context(&ctx);
            *error = "the test pictures' headers changed with the rate";
            ok = false;
            break;
        }
        Burst burst;
        uint64_t bytes = 0;
        auto drain = [&] {
            for (;;) {
                AVPacket *p = av_packet_alloc();
                if (avcodec_receive_packet(ctx, p) < 0) {
                    av_packet_free(&p);
                    return;
                }
                bytes += uint64_t(p->size);
                burst.video.push_back(own(p));
            }
        };
        for (int i = 0; i < kFps && ok; ++i) {
            av_frame_make_writable(frame);
            const engine::YuvFrame &y = pictures[size_t(i) % pictures.size()];
            const std::vector<uint8_t> *planes[3] = {&y.y, &y.u, &y.v};
            for (int p = 0; p < 3; ++p)
                for (int row = 0; row < y.rows(p); ++row)
                    std::memcpy(frame->data[p] + row * frame->linesize[p], planes[p]->data() + size_t(row) * y.rowBytes(p),
                                size_t(y.rowBytes(p)));
            frame->pts = i;
            frame->pict_type = i == 0 ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
            if (i == 0) frame->flags |= AV_FRAME_FLAG_KEY;
            else frame->flags &= ~AV_FRAME_FLAG_KEY;
            if (avcodec_send_frame(ctx, frame) < 0) ok = false;
            drain();
        }
        avcodec_send_frame(ctx, nullptr);
        drain();
        avcodec_free_context(&ctx);
        if (!ok || burst.video.empty() || !(burst.video.front()->flags & AV_PKT_FLAG_KEY)) {
            *error = BandwidthTest::tr("The test pictures could not be made.").toStdString();
            ok = false;
            break;
        }
        burst.kbps = int(double(bytes) * 8.0 / 1000.0 * kFps / double(burst.video.size()) + 0.5);
        c.bursts.push_back(std::move(burst));
    }
    av_frame_free(&frame);
    return ok;
}

// A second of quiet hiss: sound the protocol carries, at a real bitrate.
bool makeAudio(bool opus, Content &c, std::string *error) {
    const AVCodec *codec = opus ? avcodec_find_encoder_by_name("libopus") : avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        *error = BandwidthTest::tr("This FFmpeg has no encoder for the test sound.").toStdString();
        return false;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->sample_rate = kAudioRate;
    av_channel_layout_from_mask(&ctx->ch_layout, AV_CH_LAYOUT_STEREO);
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    const void *configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, &count) >= 0 && configs && count > 0) {
        const auto *formats = static_cast<const AVSampleFormat *>(configs);
        ctx->sample_fmt = formats[0];
        for (int i = 0; i < count; ++i)
            if (formats[i] == AV_SAMPLE_FMT_FLTP || formats[i] == AV_SAMPLE_FMT_FLT) { ctx->sample_fmt = formats[i]; break; }
    }
    ctx->bit_rate = 128000;
    ctx->time_base = {1, kAudioRate};
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(ctx, codec, nullptr) < 0 ||
        (ctx->sample_fmt != AV_SAMPLE_FMT_FLTP && ctx->sample_fmt != AV_SAMPLE_FMT_FLT)) {
        avcodec_free_context(&ctx);
        *error = BandwidthTest::tr("The encoder for the test sound would not open.").toStdString();
        return false;
    }
    const int frameSize = ctx->frame_size > 0 ? ctx->frame_size : 1024;
    const int frames = (kAudioRate + frameSize - 1) / frameSize;
    c.audioSpan = int64_t(frames) * frameSize;
    AVFrame *f = av_frame_alloc();
    f->format = ctx->sample_fmt;
    f->nb_samples = frameSize;
    f->sample_rate = kAudioRate;
    av_channel_layout_copy(&f->ch_layout, &ctx->ch_layout);
    av_frame_get_buffer(f, 0);
    std::mt19937 noise(7);
    std::uniform_real_distribution<float> hiss(-0.03f, 0.03f);
    std::vector<std::shared_ptr<AVPacket>> packets;
    auto drain = [&] {
        for (;;) {
            AVPacket *p = av_packet_alloc();
            if (avcodec_receive_packet(ctx, p) < 0) {
                av_packet_free(&p);
                return;
            }
            packets.push_back(own(p));
        }
    };
    for (int i = 0; i < frames; ++i) {
        av_frame_make_writable(f);
        if (ctx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < frameSize; ++s) reinterpret_cast<float *>(f->data[ch])[s] = hiss(noise);
        } else {
            for (int s = 0; s < frameSize * 2; ++s) reinterpret_cast<float *>(f->data[0])[s] = hiss(noise);
        }
        f->pts = int64_t(i) * frameSize;
        avcodec_send_frame(ctx, f);
        drain();
    }
    avcodec_send_frame(ctx, nullptr);
    drain();
    av_frame_free(&f);
    if (packets.empty()) {
        avcodec_free_context(&ctx);
        *error = BandwidthTest::tr("The test sound could not be made.").toStdString();
        return false;
    }
    // From the first packet, one loop's worth.
    const int64_t first = packets.front()->pts;
    for (auto &p : packets) {
        p->pts -= first;
        p->dts = p->pts;
        if (p->duration <= 0) p->duration = frameSize;
        if (p->pts < c.audioSpan) c.audio.push_back(p);
    }
    EncodedLayout::Audio a;
    a.track = 0;
    a.params = avcodec_parameters_alloc();
    avcodec_parameters_from_context(a.params, ctx);
    a.timeBase = ctx->time_base;
    c.layout->audio.push_back(a);
    avcodec_free_context(&ctx);
    return true;
}

// A copy of a packet at a new time.
std::shared_ptr<AVPacket> retimed(const std::shared_ptr<AVPacket> &from, int64_t pts, int64_t duration) {
    AVPacket *p = av_packet_clone(from.get());
    p->pts = p->dts = pts;
    p->duration = duration;
    return own(p);
}

bool startsWith(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

}  // namespace

BandwidthTest::BandwidthTest(QObject *parent) : QObject(parent) {}

BandwidthTest::~BandwidthTest() {
    m_cancel.store(true);
    join();
}

bool BandwidthTest::canTest(const std::string &serviceId, std::string *why) {
    if (serviceId.empty() || serviceId == "custom") return true;
    const StreamService *service = streamService(serviceId);
    if (!service) {
        if (why) *why = tr("This service is not one the test knows; type your upload speed instead.").toStdString();
        return false;
    }
    if (service->testKeySuffix.empty()) {
        if (why) *why = tr("A test to %1 would go live; type your upload speed instead.")
                            .arg(QString::fromStdString(service->name)).toStdString();
        return false;
    }
    return true;
}

std::string BandwidthTest::testUrl(const Settings &settings, std::string *why) {
    if (!canTest(settings.serviceId, why)) return {};
    const StreamService *service = streamService(settings.serviceId);
    std::string server = settings.server;
    if (server.empty() && service && !service->servers.empty()) server = service->servers.front().url;
    if (server.empty()) {
        if (why) *why = tr("Type the server address first.").toStdString();
        return {};
    }
    // A service's server typed in as a custom one is still that service: it
    // takes its test mode, or is not tested, as it would go live.
    if (!service) {
        service = serviceForServer(server);
        if (service && service->testKeySuffix.empty()) {
            if (why) *why = tr("That server is %1's: a test would go live; type your upload speed instead.")
                                .arg(QString::fromStdString(service->name)).toStdString();
            return {};
        }
    }
    StreamSink::Protocol protocol;
    if (!StreamSink::protocolFor(server, &protocol)) {
        if (why) *why = tr("The server address should start with rtmp://, rtmps://, srt:// or https://.").toStdString();
        return {};
    }
    std::string key = settings.key;
    if (service) {
        if (key.empty()) {
            if (why) *why = tr("Type your %1 first.").arg(QString::fromStdString(service->keyLabel).toLower()).toStdString();
            return {};
        }
        // The test mode once, however the key was pasted.
        const std::string &suffix = service->testKeySuffix;
        const std::string bare = suffix[0] == '?' || suffix[0] == '&' ? suffix.substr(1) : suffix;
        if (key.find(bare) == std::string::npos)
            key += suffix[0] == '?' && key.find('?') != std::string::npos ? "&" + bare : suffix;
    }
    // WHIP takes the key as a bearer token, not in the address.
    std::string url = protocol == StreamSink::Protocol::Whip ? server : streamUrl(server, key);
    if (!settings.user.empty() || !settings.password.empty()) {
        std::string problem;
        url = withLogin(url, settings.user, settings.password, &problem);
        if (url.empty()) {
            if (why) *why = tr("The sign-in cannot be used: %1").arg(QString::fromStdString(problem)).toStdString();
            return {};
        }
    }
    return url;
}

std::vector<int> BandwidthTest::steps(int maxKbps) {
    const int top = std::clamp(maxKbps > 0 ? maxKbps : kDefaultTopKbps, 500, 60000);
    std::vector<int> out;
    for (double r = kStartKbps; r < top; r *= 1.5) out.push_back(int(r + 0.5));
    // The top itself, standing in for a step just under it.
    if (!out.empty() && top < out.back() * 1.2) out.pop_back();
    out.push_back(top);
    return out;
}

void BandwidthTest::start(const Settings &settings) {
    if (m_running) {
        m_cancel.store(true);
        join();
    }
    m_cancel.store(false);
    m_result = Result{};
    const unsigned serial = ++m_serial;
    m_running = true;
    std::string why;
    const std::string url = testUrl(settings, &why);
    if (url.empty()) {
        Result r;
        r.error = why;
        QMetaObject::invokeMethod(this, [this, serial, r] { done(serial, r); }, Qt::QueuedConnection);
        return;
    }
    m_thread = std::thread(&BandwidthTest::run, this, settings, url, serial);
}

void BandwidthTest::cancel() {
    if (!m_running) return;
    m_cancel.store(true);
    join();
    Result r;
    r.error = tr("The test was stopped.").toStdString();
    done(m_serial, r);
}

void BandwidthTest::join() {
    if (m_thread.joinable()) m_thread.join();
}

void BandwidthTest::post(unsigned serial, double fraction, int kbps) {
    QMetaObject::invokeMethod(this, [this, serial, fraction, kbps] {
        if (serial == m_serial && m_running) emit progress(fraction, kbps);
    }, Qt::QueuedConnection);
}

void BandwidthTest::done(unsigned serial, const Result &result) {
    if (serial != m_serial || !m_running) return;
    join();
    m_running = false;
    m_result = result;
    emit finished(result);
}

void BandwidthTest::run(Settings settings, std::string url, unsigned serial) {
    Result result;
    auto finish = [&] {
        if (m_cancel.load()) return;  // cancel() tells
        QMetaObject::invokeMethod(this, [this, serial, result] { done(serial, result); }, Qt::QueuedConnection);
    };
    const StreamService *service = streamService(settings.serviceId);
    StreamSink::Protocol protocol = StreamSink::Protocol::Rtmp;
    StreamSink::protocolFor(url, &protocol);
    const std::vector<int> rates = steps(settings.maxKbps);

    // The content, made once.
    Content content;
    content.layout = std::make_shared<EncodedLayout>();
    std::string error;
    const bool opus = !StreamSink::carries(protocol, AV_CODEC_ID_H264, AV_CODEC_ID_AAC, nullptr) &&
                      StreamSink::carries(protocol, AV_CODEC_ID_H264, AV_CODEC_ID_OPUS, nullptr);
    bool made;
    {
        const QuietAvLog quiet;
        made = makeVideo(rates, content, &error, m_cancel) && makeAudio(opus, content, &error);
    }
    if (!made || m_cancel.load()) {
        result.error = error;
        finish();
        return;
    }

    // The sink as a stream has it, with the socket's buffer kept short where
    // the protocol takes that (a protocol that turns it down is tried without).
    const int bufferBytes = std::max(64 * 1024, rates.back() * 1000 / 8 / 10);
    bool shortBuffer = (startsWith(url, "rtmp://") || startsWith(url, "rtmps://")) &&
                       settings.muxerOptions.find("send_buffer_size") == std::string::npos;
    // Before the sink, so it outlives the sink's thread that sets it.
    std::atomic<int64_t> keyAt{INT64_MAX};
    std::shared_ptr<StreamSink> sink;
    int64_t zero = 0, connectedAt = 0;
    for (;;) {
        StreamSink::Settings ss;
        ss.url = url;
        ss.protocolGiven = true;
        ss.protocol = protocol;
        if (protocol == StreamSink::Protocol::Whip) ss.bearerToken = settings.key;
        ss.muxerOptions = settings.muxerOptions;
        if (shortBuffer) ss.muxerOptions += " send_buffer_size=" + std::to_string(bufferBytes);
        ss.bindAddress = settings.bindAddress;
        ss.ipFamily = settings.ipFamily;
        ss.reconnect = false;
        ss.timeoutSeconds = 10;
        ss.bitrateKbps = rates.back();
        zero = monotonicNow();
        ss.clockZeroNs = zero;
        sink = std::make_shared<StreamSink>();
        sink->wantKeyframe = [&keyAt](int64_t at) { keyAt.store(at); };
        if (!sink->start(ss, content.layout, &error)) {
            result.error = error;
            finish();
            return;
        }
        sink->begin(content.layout);

        // Feeds up to the clock plus the lead; the first packet connects.
        int64_t frame = 0, audioLoop = 0;
        size_t inBurst = 0, audioAt = 0, burst = 0;
        bool restart = false;
        const int64_t frameNs = kSecond / kFps;
        auto feed = [&](int64_t until) {
            while (frame * frameNs < until) {
                const int64_t ptsNs = frame * frameNs;
                if (ptsNs >= keyAt.load()) {
                    keyAt.store(INT64_MAX);
                    inBurst = 0;
                }
                if (restart) {
                    restart = false;
                    inBurst = 0;
                }
                const Burst &b = content.bursts[burst];
                EncodedPacket p;
                p.packet = retimed(b.video[inBurst], frame, 1);
                p.stream = 0;
                p.key = inBurst == 0;
                p.ptsNs = ptsNs;
                p.endNs = ptsNs + frameNs;
                sink->packet(p);
                ++frame;
                inBurst = (inBurst + 1) % b.video.size();
                // The sound up to the end of that picture.
                for (;;) {
                    const auto &a = content.audio[audioAt];
                    const int64_t pts = audioLoop * content.audioSpan + a->pts;
                    const int64_t aNs = pts * kSecond / kAudioRate;
                    if (aNs >= frame * frameNs) break;
                    EncodedPacket q;
                    q.packet = retimed(a, pts, a->duration);
                    q.stream = 1;
                    q.key = true;
                    q.ptsNs = aNs;
                    q.endNs = aNs + a->duration * kSecond / kAudioRate;
                    sink->packet(q);
                    if (++audioAt == content.audio.size()) {
                        audioAt = 0;
                        ++audioLoop;
                    }
                }
            }
        };

        // Connecting.
        const int64_t giveUp = zero + int64_t(ss.timeoutSeconds + 5) * kSecond;
        StreamSink::Stats st;
        while (!m_cancel.load()) {
            feed(monotonicNow() - zero + kLeadNs);
            st = sink->stats();
            if (st.state == StreamSink::State::Live || st.state == StreamSink::State::Failed || monotonicNow() > giveUp) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (m_cancel.load()) return;
        if (st.state == StreamSink::State::Failed && shortBuffer && st.message.find("send_buffer_size") != std::string::npos) {
            shortBuffer = false;
            sink.reset();
            continue;
        }
        if (st.state != StreamSink::State::Live) {
            result.error = st.state == StreamSink::State::Failed
                               ? tr("The server did not take the test: %1").arg(QString::fromStdString(st.message)).toStdString()
                               : tr("The server did not answer.").toStdString();
            sink.reset();
            finish();
            return;
        }
        connectedAt = monotonicNow();
        result.connectMs = int((connectedAt - zero) / 1'000'000);

        // The ramp.
        const int64_t stepNs = std::max<int64_t>(kMinStepNs, int64_t(std::max(1, settings.seconds)) * kSecond / int64_t(rates.size()));
        int best = 0, through = 0;
        bool failed = false, blockedThrough = false;
        int64_t lastPost = 0, lastMove = monotonicNow();
        uint64_t moved = st.framesSent + st.framesDropped;
        for (size_t step = 0; step < rates.size() && !failed && !m_cancel.load(); ++step) {
            burst = step;
            restart = true;
            const int64_t stepStart = monotonicNow();
            const StreamSink::Stats atStart = sink->stats();
            int64_t halfAt = 0, blockedAt = 0, firstBlockedAt = 0;
            uint64_t halfBytes = 0, blockedBytes = 0, firstBlockedBytes = 0;
            int peakLag = 0;
            for (;;) {
                const int64_t now = monotonicNow();
                feed(now - zero + kLeadNs);
                st = sink->stats();
                if (st.framesSent + st.framesDropped != moved) {
                    moved = st.framesSent + st.framesDropped;
                    lastMove = now;
                }
                const bool stalled = now - lastMove > kStallNs;
                if (!halfAt && now - stepStart >= stepNs / 2) {
                    halfAt = now;
                    halfBytes = st.bytes;
                }
                if (halfAt) peakLag = std::max(peakLag, st.lagMs);
                // Behind: the socket is full, and from here what it takes is
                // what the link carries.
                // Caught up again (a big picture held it up for a moment):
                // the stretch starts over.
                if (!blockedAt && st.lagMs >= kBlockedLagMs) {
                    blockedAt = now;
                    blockedBytes = st.bytes;
                    if (!firstBlockedAt) {
                        firstBlockedAt = now;
                        firstBlockedBytes = st.bytes;
                    }
                } else if (blockedAt && st.lagMs < kBlockedLagMs / 2) {
                    blockedAt = 0;
                }
                if (now - lastPost >= 250'000'000) {
                    lastPost = now;
                    const double fraction = double(now - connectedAt) / double(stepNs * int64_t(rates.size()));
                    post(serial, std::clamp(fraction, 0.0, 1.0), int(st.kbps + 0.5));
                }
                const bool gone = st.state != StreamSink::State::Live;
                const bool dropped = st.framesDropped > atStart.framesDropped;
                const bool over = now - stepStart >= stepNs;
                if (gone || dropped || stalled || st.lagMs > 700 ||
                    (over && (st.lagMs > kPassLagMs || peakLag > kPassPeakLagMs))) {
                    failed = true;
                    if (gone) {
                        result.error = tr("The connection was lost: %1").arg(QString::fromStdString(st.message)).toStdString();
                        break;
                    }
                    // What got through once the socket was full (since it last
                    // was, or since it first was when one big picture filled it
                    // at the end); failing that, through the second half, or
                    // the step so far.
                    const int64_t window = 300'000'000;
                    const bool last = blockedAt && now - blockedAt >= window;
                    blockedThrough = last || (firstBlockedAt && now - firstBlockedAt >= window);
                    const int64_t from = last ? blockedAt : blockedThrough ? firstBlockedAt : halfAt ? halfAt : stepStart;
                    const uint64_t fromBytes = last ? blockedBytes : blockedThrough ? firstBlockedBytes : halfAt ? halfBytes : atStart.bytes;
                    if (now - from >= 250'000'000)
                        through = int(double(st.bytes - fromBytes) * 8.0 / 1000.0 / (double(now - from) / 1e9));
                    through = std::min(through, content.bursts[step].kbps);
                    break;
                }
                if (over) {
                    best = content.bursts[step].kbps;
                    break;
                }
                if (m_cancel.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        if (m_cancel.load()) return;
        st = sink->stats();
        // Finishes cleanly: what is queued goes, then the server is told.
        sink->finishAt(frame * frameNs);
        for (int i = 0; i < 300 && !sink->done() && !m_cancel.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        sink.reset();
        if (m_cancel.load()) return;
        if (!result.error.empty()) {
            finish();
            return;
        }
        result.ok = true;
        // Measured while the link was the limit, it stands even below a
        // step that passed (whose buffer was still filling).
        result.measuredKbps = blockedThrough ? through : std::max(best, through);
        int recommended = result.measuredKbps * 3 / 4;
        if (service && service->maxBitrateKbps > 0) recommended = std::min(recommended, service->maxBitrateKbps);
        result.recommendedKbps = recommended;
        const uint64_t pictures = st.framesSent + st.framesDropped;
        result.droppedFraction = pictures ? double(st.framesDropped) / double(pictures) : 0.0;
        post(serial, 1.0, int(st.kbps + 0.5));
        finish();
        return;
    }
}
