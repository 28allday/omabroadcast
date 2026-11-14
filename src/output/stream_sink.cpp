#include "stream_sink.h"

extern "C" {
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

#include <time.h>

#include <algorithm>
#include <cstring>
#include <thread>

#include "file_sink.h"

namespace {
std::string avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

int64_t monotonicNow() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000ll + ts.tv_nsec;
}

bool startsWith(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

const char *muxerFor(StreamSink::Protocol p) {
    switch (p) {
    case StreamSink::Protocol::Rtmp: return "flv";
    case StreamSink::Protocol::Srt: return "mpegts";
    case StreamSink::Protocol::Whip: return "whip";
    case StreamSink::Protocol::Hls: return "hls";
    case StreamSink::Protocol::Custom: break;
    }
    return "flv";
}

constexpr AVRational kNs{1, 1'000'000'000};
}  // namespace

StreamSink::StreamSink() { idleMs = 100; }

StreamSink::~StreamSink() {
    m_abort.store(true);
    stopThread();
    disconnect(false);
}

bool StreamSink::protocolFor(const std::string &url, Protocol *protocol) {
    Protocol p;
    if (startsWith(url, "rtmp://") || startsWith(url, "rtmps://")) p = Protocol::Rtmp;
    else if (startsWith(url, "srt://")) p = Protocol::Srt;
    else if (startsWith(url, "http://") || startsWith(url, "https://"))
        // HLS ingest is a playlist upload; anything else over HTTP is WHIP.
        p = url.find(".m3u8") != std::string::npos || url.find("http_upload_hls") != std::string::npos ? Protocol::Hls : Protocol::Whip;
    else return false;
    if (protocol) *protocol = p;
    return true;
}

const char *StreamSink::protocolName(Protocol protocol) {
    switch (protocol) {
    case Protocol::Rtmp: return "RTMP";
    case Protocol::Srt: return "SRT";
    case Protocol::Whip: return "WHIP";
    case Protocol::Hls: return "HLS";
    case Protocol::Custom: return "FFmpeg";
    }
    return "RTMP";
}

bool StreamSink::protocolAvailable(Protocol protocol) {
    if (protocol == Protocol::Custom) return true;  // the address and muxer are checked at the start
    const char *scheme = protocol == Protocol::Srt ? "srt://x" : protocol == Protocol::Rtmp ? "rtmp://x" : "https://x";
    const char *name = avio_find_protocol_name(scheme);
    if (!name) return false;
    return av_guess_format(muxerFor(protocol), nullptr, nullptr) != nullptr;
}

bool StreamSink::carries(Protocol protocol, AVCodecID video, AVCodecID audio, std::string *why,
                         const std::string &muxer) {
    auto refuse = [&](const std::string &reason) { if (why) *why = reason; return false; };
    switch (protocol) {
    case Protocol::Custom:
        if (muxer.empty()) return true;
        if (const std::string problem = muxerProblem(muxer); !problem.empty()) return refuse(problem);
        if (!formatCarries("custom:" + muxer, video))
            return refuse(muxer + " does not take " + std::string(avcodec_get_name(video)) + " pictures");
        if (audio != AV_CODEC_ID_NONE && !formatCarries("custom:" + muxer, audio))
            return refuse(muxer + " does not take " + std::string(avcodec_get_name(audio)) + " sound");
        return true;
    case Protocol::Rtmp:
        // Enhanced RTMP carries HEVC and AV1 besides H.264; the sound is AAC
        // (Opus only where the server speaks enhanced RTMP too).
        if (video != AV_CODEC_ID_H264 && video != AV_CODEC_ID_HEVC && video != AV_CODEC_ID_AV1)
            return refuse("RTMP carries H.264, HEVC or AV1");
        if (audio != AV_CODEC_ID_NONE && audio != AV_CODEC_ID_AAC) return refuse("RTMP carries AAC sound");
        return true;
    case Protocol::Srt:
    case Protocol::Hls:
        if (video != AV_CODEC_ID_H264 && video != AV_CODEC_ID_HEVC && video != AV_CODEC_ID_AV1)
            return refuse("MPEG-TS streams carry H.264, HEVC or AV1");
        // As for recordings: FFmpeg writes AV1 into MPEG-TS but cannot read
        // it back, so a receiver built on it could not either.
        if (video == AV_CODEC_ID_AV1 && !formatCarries("ts", AV_CODEC_ID_AV1))
            return refuse("this FFmpeg cannot read AV1 back from MPEG-TS; use H.264 or HEVC");
        if (audio != AV_CODEC_ID_NONE && audio != AV_CODEC_ID_AAC && audio != AV_CODEC_ID_OPUS)
            return refuse("MPEG-TS streams carry AAC or Opus sound");
        return true;
    case Protocol::Whip:
        if (video != AV_CODEC_ID_H264) return refuse("WHIP (WebRTC) needs H.264");
        if (audio != AV_CODEC_ID_NONE && audio != AV_CODEC_ID_OPUS) return refuse("WHIP (WebRTC) needs Opus sound");
        return true;
    }
    return true;
}

bool StreamSink::start(const Settings &settings, std::shared_ptr<const EncodedLayout> layout, std::string *error) {
    m_settings = settings;
    m_layout = std::move(layout);
    if (!m_settings.protocolGiven && !protocolFor(settings.url, &m_settings.protocol)) {
        if (error) *error = "the server address should start with rtmp://, rtmps://, srt:// or https://";
        return false;
    }
    if (m_settings.protocol == Protocol::Custom && !avio_find_protocol_name(m_settings.url.c_str())) {
        // The scheme and host only: the rest can hold the key or a sign-in.
        std::string where = m_settings.url;
        const size_t scheme = where.find("://");
        if (scheme != std::string::npos) {
            const size_t end = where.find_first_of("/?#", scheme + 3);
            std::string host = where.substr(scheme + 3, end == std::string::npos ? std::string::npos : end - scheme - 3);
            if (const size_t at = host.rfind('@'); at != std::string::npos) host = host.substr(at + 1);
            where = where.substr(0, scheme + 3) + host;
        } else {
            where = "that address";
        }
        if (error) *error = "FFmpeg cannot write to " + where;
        return false;
    }
    if (!protocolAvailable(m_settings.protocol)) {
        if (error) *error = std::string("this FFmpeg cannot send ") + protocolName(m_settings.protocol);
        return false;
    }
    const AVCodecID audio = m_layout->audio.empty() ? AV_CODEC_ID_NONE : m_layout->audio.front().params->codec_id;
    std::string why;
    if (!carries(m_settings.protocol, m_layout->video->codec_id, audio, &why, m_settings.muxer)) {
        if (error) *error = why;
        return false;
    }
    m_bitrateKbps = settings.bitrateKbps;
    m_keyAfter = settings.startNs;
    m_waitingKey = true;
    m_nextAttemptNs = 0;
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats = Stats{};
        m_stats.bitrateKbps = m_bitrateKbps;
        m_stats.delayMs = settings.delaySeconds * 1000;
    }
    startThread();
    return true;
}

void StreamSink::finishAt(int64_t ns) {
    m_finishAt.store(ns);
    // Not live: nothing worth finishing, and a connect in progress gives up.
    if (stats().state != State::Live) m_abort.store(true);
    poke();
}

StreamSink::Stats StreamSink::stats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

int StreamSink::interrupted(void *opaque) { return static_cast<StreamSink *>(opaque)->m_abort.load() ? 1 : 0; }

void StreamSink::setState(State state, const std::string &message) {
    bool changed;
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        changed = m_stats.state != state;
        m_stats.state = state;
        if (!message.empty() || state == State::Live) m_stats.message = message;
    }
    if (changed && onState) onState(state);
}

bool StreamSink::connect(std::string *error) {
    const char *muxer = m_settings.protocol == Protocol::Custom ? m_settings.muxer.c_str() : muxerFor(m_settings.protocol);
    int r = avformat_alloc_output_context2(&m_fmt, nullptr, muxer, m_settings.url.c_str());
    if (r < 0 || !m_fmt) { *error = "container: " + avErr(r); return false; }
    m_fmt->interrupt_callback = {&StreamSink::interrupted, this};
    m_fmt->flush_packets = 1;  // live: out as soon as it is muxed
    const bool ts = !strcmp(muxer, "mpegts") || !strcmp(muxer, "hls");
    bool streamsMade = true;
    auto addStream = [&](const AVCodecParameters *params, AVRational timeBase, int index) {
        AVStream *st = avformat_new_stream(m_fmt, nullptr);
        if (!st) { streamsMade = false; return; }
        st->time_base = timeBase;
        AVBSFContext *bsf = nullptr;
        // Viewers join mid-stream: headers before every keyframe.
        if (ts && index == 0 && (params->codec_id == AV_CODEC_ID_H264 || params->codec_id == AV_CODEC_ID_HEVC)) {
            const AVBitStreamFilter *f = av_bsf_get_by_name("dump_extra");
            if (f && av_bsf_alloc(f, &bsf) >= 0) {
                avcodec_parameters_copy(bsf->par_in, params);
                bsf->time_base_in = timeBase;
                av_opt_set(bsf->priv_data, "freq", "keyframe", 0);
                if (av_bsf_init(bsf) < 0) av_bsf_free(&bsf);
            }
        }
        avcodec_parameters_copy(st->codecpar, bsf ? bsf->par_out : params);
        st->codecpar->codec_tag = keptCodecTag(m_fmt->oformat, st->codecpar);
        if (index == 0) st->avg_frame_rate = m_layout->frameRate;
        m_bsf.push_back(bsf);
    };
    addStream(m_layout->video, m_layout->videoTimeBase, 0);
    // Services take one sound track: the first.
    if (!m_layout->audio.empty()) addStream(m_layout->audio.front().params, m_layout->audio.front().timeBase, 1);
    if (!streamsMade) {
        *error = "out of memory for the container";
        disconnect(false);
        return false;
    }

    AVDictionary *opts = nullptr;
    const std::string timeout = std::to_string(int64_t(m_settings.timeoutSeconds) * 1'000'000);
    av_dict_set(&opts, "rw_timeout", timeout.c_str(), 0);
    // Bound to the any-address of one family, a socket of the other cannot
    // connect, so only the family asked for is tried.
    const std::string bind = !m_settings.bindAddress.empty() ? m_settings.bindAddress
                           : m_settings.ipFamily == 4 ? "0.0.0.0" : m_settings.ipFamily == 6 ? "::" : "";
    if (!bind.empty()) av_dict_set(&opts, "local_addr", bind.c_str(), 0);
    if (m_settings.protocol == Protocol::Rtmp) av_dict_set(&opts, "flvflags", "no_duration_filesize", 0);
    if (m_settings.lowLatency) av_dict_set(&opts, "tcp_nodelay", "1", 0);
    if (m_settings.protocol == Protocol::Whip && !m_settings.bearerToken.empty())
        av_dict_set(&opts, "authorization", m_settings.bearerToken.c_str(), 0);
    if (m_settings.protocol == Protocol::Hls) {
        av_dict_set(&opts, "method", "PUT", 0);
        av_dict_set(&opts, "hls_time", "2", 0);
        av_dict_set(&opts, "hls_list_size", "6", 0);
        av_dict_set(&opts, "http_persistent", "1", 0);
        // FFmpeg never reads the answer to an upload, so a refused sign-in
        // would pass for live: with one, each request waits for the go-ahead
        // (the muxer passes only its own options on to what it opens).
        const size_t host = m_settings.url.find("://");
        const size_t path = m_settings.url.find('/', host == std::string::npos ? 0 : host + 3);
        if (host != std::string::npos && m_settings.url.substr(host + 3, path - host - 3).find('@') != std::string::npos) {
            m_fmt->opaque = reinterpret_cast<void *>(m_fmt->io_open);
            m_fmt->io_open = [](AVFormatContext *s, AVIOContext **pb, const char *url, int flags, AVDictionary **options) {
                if ((flags & AVIO_FLAG_WRITE) && options) av_dict_set(options, "send_expect_100", "1", 0);
                using Open = int (*)(AVFormatContext *, AVIOContext **, const char *, int, AVDictionary **);
                return reinterpret_cast<Open>(s->opaque)(s, pb, url, flags, options);
            };
        }
    }
    const auto own = parseEncoderOptions(m_settings.muxerOptions);
    for (const auto &[key, value] : own) av_dict_set(&opts, key.c_str(), value.c_str(), 0);
    if (!(m_fmt->oformat->flags & AVFMT_NOFILE)) {
        r = avio_open2(&m_fmt->pb, m_settings.url.c_str(), AVIO_FLAG_WRITE, &m_fmt->interrupt_callback, &opts);
        if (r < 0) {
            av_dict_free(&opts);
            *error = avErr(r);
            disconnect(false);
            return false;
        }
    }
    r = avformat_write_header(m_fmt, &opts);
    // Taken by neither the connection nor the container: trying again
    // would not help.
    std::string unknown;
    for (const auto &kv : own)
        if (av_dict_get(opts, kv.first.c_str(), nullptr, 0)) unknown = kv.first;
    av_dict_free(&opts);
    if (r >= 0 && !unknown.empty()) {
        *error = "the " + std::string(m_fmt->oformat->name) + " container has no option \"" + unknown + "\"";
        m_hopeless = true;
        disconnect(false);
        return false;
    }
    if (r < 0) {
        *error = avErr(r);
        disconnect(false);
        return false;
    }
    return true;
}

void StreamSink::disconnect(bool cleanly) {
    if (m_fmt) {
        if (cleanly) av_write_trailer(m_fmt);
        if (m_fmt->pb && !(m_fmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&m_fmt->pb);
        avformat_free_context(m_fmt);
        m_fmt = nullptr;
    }
    for (AVBSFContext *&b : m_bsf) av_bsf_free(&b);
    m_bsf.clear();
}

void StreamSink::lost(const std::string &why) {
    disconnect(false);
    if (m_abort.load() || m_finishAt.load() != INT64_MAX) {
        setState(State::Stopped);
        markDone();
        return;
    }
    if (!m_settings.reconnect || m_hopeless || (m_settings.maxRetries > 0 && m_retries >= m_settings.maxRetries)) {
        setState(State::Failed, why);
        markDone();
        return;
    }
    // Waits grow from the first, to at most a minute.
    const int64_t wait = std::min<int64_t>(60, int64_t(m_settings.retryDelaySeconds * (1.0 + 0.5 * m_retries)));
    m_nextAttemptNs = monotonicNow() + std::max<int64_t>(1, wait) * 1'000'000'000;
    ++m_retries;
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.reconnects = m_retries;
    }
    setState(State::Reconnecting, why);
}

bool StreamSink::send(const EncodedPacket &p) {
    const int index = p.stream == 0 ? 0 : 1;
    if (p.stream > 1 || index >= int(m_fmt->nb_streams)) return true;  // tracks 2–6 stay home
    const AVRational tb = p.stream == 0 ? m_layout->videoTimeBase : m_layout->audio.front().timeBase;
    AVPacket *pkt = av_packet_clone(p.packet.get());
    if (!pkt) return true;
    const int64_t shift = av_rescale_q(m_zeroNs, kNs, tb);
    if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= shift;
    if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= shift;
    bool ok = true;
    auto out = [&](AVPacket *q) {
        av_packet_rescale_ts(q, tb, m_fmt->streams[index]->time_base);
        q->stream_index = index;
        const int size = q->size;
        const int r = av_interleaved_write_frame(m_fmt, q);
        if (r < 0) {
            ok = false;
            lost(m_abort.load() ? std::string("stopped") : avErr(r));
            return false;
        }
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.bytes += uint64_t(size);
        return true;
    };
    if (AVBSFContext *bsf = m_bsf[size_t(index)]) {
        if (av_bsf_send_packet(bsf, pkt) >= 0) {
            AVPacket *f = av_packet_alloc();
            while (ok && av_bsf_receive_packet(bsf, f) >= 0) out(f);
            av_packet_free(&f);
        }
    } else {
        out(pkt);
    }
    av_packet_free(&pkt);
    return ok;
}

void StreamSink::askForBitrate(int kbps) {
    // Refused, the encoder carries on at what it opened with, and that is the
    // target to show.
    m_bitrateHeld = !onBitrate(kbps);
    m_bitrateKbps = m_bitrateHeld ? m_settings.bitrateKbps : kbps;
}

void StreamSink::adjustBitrate(int lagMs, int64_t now) {
    if (!m_settings.dynamicBitrate || m_settings.bitrateKbps <= 0 || !onBitrate) return;
    const int64_t second = 1'000'000'000;
    if (lagMs > m_settings.dropAfterMs / 3) {
        m_clearSinceNs = 0;
        if (!m_congestedSinceNs) m_congestedSinceNs = now;
        // Behind for a second, or far enough to drop pictures: 15% less (at
        // most once a second), down to a quarter of the target.
        const bool dropping = lagMs > m_settings.dropAfterMs && now - m_loweredNs >= second;
        if (now - m_congestedSinceNs >= second || dropping) {
            m_loweredNs = now;
            const int lower = std::max(m_settings.bitrateKbps / 4, m_bitrateKbps * 85 / 100);
            if (lower < m_bitrateKbps) askForBitrate(lower);
            m_congestedSinceNs = now;
        }
    } else if (lagMs < 50) {
        m_congestedSinceNs = 0;
        if (!m_clearSinceNs) m_clearSinceNs = now;
        // Clear for ten seconds: back up by a tenth.
        if (m_bitrateKbps < m_settings.bitrateKbps && now - m_clearSinceNs >= 10 * second) {
            askForBitrate(std::min(m_settings.bitrateKbps, m_bitrateKbps * 110 / 100 + 1));
            m_clearSinceNs = now;
        }
    }
    std::lock_guard<std::mutex> lock(m_statsMutex);
    m_stats.bitrateKbps = m_bitrateKbps;
    m_stats.bitrateHeld = m_bitrateHeld;
}

void StreamSink::handle(const EncodedPacket &p) {
    if (m_abort.load()) {
        disconnect(false);
        setState(State::Stopped);
        markDone();
        return;
    }
    const int64_t finishAt = m_finishAt.load();
    // Past the stop but decoded before it: a picture with some before the stop
    // still to come that look back at it, so it goes out with them.
    const bool neededBeforeStop = p.stream == 0 && p.packet->dts != AV_NOPTS_VALUE &&
                                  av_rescale_q(p.packet->dts, m_layout->videoTimeBase, kNs) < finishAt;
    if (p.ptsNs >= finishAt && !neededBeforeStop) {
        // Everything before the stop is out: close cleanly once the picture is past it.
        if (p.stream == 0) {
            if (m_fmt) disconnect(true);
            setState(State::Stopped);
            markDone();
        }
        return;
    }
    const int64_t now = monotonicNow();
    if (!m_fmt) {
        // Kept from the cut on (only once it was live), else dropped.
        if (preserving() && m_cutNs >= 0) keep(p);
        if (now < m_nextAttemptNs) return;
        setState(m_retries ? State::Reconnecting : State::Connecting);
        std::string error;
        if (!connect(&error)) {
            lost(m_abort.load() ? std::string("stopped") : error);
            return;
        }
        m_waitingKey = true;
        if (preserving() && m_cutNs >= 0) {
            // Carry on from the first keyframe after the last picture sent.
            m_keyAfter = m_cutNs + 1;
            m_resuming = true;
        } else {
            // From a keyframe after now: what queued while connecting is stale.
            m_keyAfter = std::max(m_keyAfter, now - m_settings.clockZeroNs - int64_t(m_settings.delaySeconds) * 1'000'000'000);
            if (wantKeyframe) wantKeyframe(m_keyAfter);
            return;
        }
    } else if (!m_backlog.empty()) {
        keep(p);
    } else {
        if (!deliver(p) && preserving() && m_cutNs >= 0) keep(p, true);
        return;
    }
    // What was kept, in order, until it is out or the connection goes again.
    while (!m_backlog.empty() && m_fmt) {
        EncodedPacket q = std::move(m_backlog.front());
        m_backlog.pop_front();
        m_backlogBytes -= size_t(q.packet->size);
        if (!deliver(q)) {
            if (!m_fmt && m_cutNs >= 0) keep(q, true);
            break;
        }
    }
}

void StreamSink::keep(const EncodedPacket &p, bool front) {
    if (front) m_backlog.push_front(p);
    else m_backlog.push_back(p);
    m_backlogBytes += size_t(p.packet->size);
    // Over the limit: the oldest whole stretches go, so it still starts on a keyframe.
    while (m_backlogBytes > m_settings.keepBytes && !m_backlog.empty()) {
        do {
            m_backlogBytes -= size_t(m_backlog.front().packet->size);
            m_backlog.pop_front();
        } while (!m_backlog.empty() && !(m_backlog.front().stream == 0 && m_backlog.front().key));
    }
}

bool StreamSink::deliver(const EncodedPacket &p) {
    if (m_waitingKey) {
        if (p.stream != 0 || !p.key || p.ptsNs < m_keyAfter) return true;
        m_waitingKey = false;
        if (m_zeroNs < 0) m_zeroNs = p.ptsNs;
        m_retries = 0;
        if (m_resuming) {
            // Sent now rather than skipped: the delay grows by the outage.
            m_resuming = false;
            const int64_t due = m_settings.clockZeroNs + p.ptsNs + int64_t(m_settings.delaySeconds) * 1'000'000'000 + m_shiftNs;
            m_shiftNs += std::max<int64_t>(0, monotonicNow() - due);
        }
        setState(State::Live);
    }
    if (p.ptsNs < m_zeroNs) return true;
    const int64_t now = monotonicNow();

    // Held back by the delay, then sent; how late it goes out is the lag. From
    // when it reached the sink, if that is later: the time a look-ahead keeps
    // it in the encoder says nothing about the network, and no dropping or
    // lower bitrate would shorten it.
    const int64_t due = m_settings.clockZeroNs + p.ptsNs + int64_t(m_settings.delaySeconds) * 1'000'000'000 + m_shiftNs;
    while (monotonicNow() < due && !m_abort.load() && m_finishAt.load() == INT64_MAX)
        std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp<int64_t>((due - monotonicNow()) / 1'000'000, 1, 20)));
    const int lagMs = int(std::max<int64_t>(0, monotonicNow() - std::max(due, p.madeNs)) / 1'000'000);
    adjustBitrate(lagMs, now);

    // Too far behind: drop pictures to the next keyframe that is caught up;
    // the sound goes on unless it is twice as far behind.
    if (p.stream == 0) {
        if (lagMs > m_settings.dropAfterMs) m_dropping = true;
        else if (m_dropping && p.key && lagMs < m_settings.dropAfterMs / 2) m_dropping = false;
        if (m_dropping) {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.framesDropped++;
            m_stats.lagMs = lagMs;
            return true;
        }
    } else if (lagMs > 2 * m_settings.dropAfterMs) {
        return true;
    }
    if (!send(p)) return false;
    if (p.stream == 0) m_cutNs = p.ptsNs;
    const size_t queued = queuedBytes();
    std::lock_guard<std::mutex> lock(m_statsMutex);
    if (p.stream == 0) m_stats.framesSent++;
    m_stats.lagMs = lagMs;
    m_stats.delayMs = int((int64_t(m_settings.delaySeconds) * 1'000'000'000 + m_shiftNs) / 1'000'000);
    m_stats.heldBytes = queued + m_backlogBytes;
    // A couple of seconds of throughput.
    m_rate.emplace_back(now, m_stats.bytes);
    while (m_rate.size() > 2 && now - m_rate.front().first > 2'000'000'000) m_rate.erase(m_rate.begin());
    if (m_rate.size() > 1 && now > m_rate.front().first)
        m_stats.kbps = double(m_stats.bytes - m_rate.front().second) * 8.0 / (double(now - m_rate.front().first) / 1e9) / 1000.0;
    return true;
}

void StreamSink::idle() {
    // Packets stopped coming (the encoder finished): close at a stop request.
    if (m_finishAt.load() != INT64_MAX || m_abort.load()) {
        if (m_fmt) disconnect(!m_abort.load());
        setState(State::Stopped);
        markDone();
    }
}

void StreamSink::finish() {
    if (m_fmt) disconnect(!m_abort.load());
    if (stats().state != State::Failed) setState(State::Stopped);
}

void StreamSink::overflow() {
    setState(State::Failed, "too far behind to catch up (a slow connection, or a delay too long to hold)");
    markDone();
}
