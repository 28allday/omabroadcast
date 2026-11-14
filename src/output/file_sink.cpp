#include "file_sink.h"

extern "C" {
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cstdio>
#include <climits>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

namespace {
std::string avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return buf;
}
constexpr AVRational kNs{1, 1'000'000'000};
constexpr size_t kMaxHeld = 2000;   // audio held while waiting for a keyframe
constexpr int64_t kSplitRetryNs = 30'000'000'000;

int64_t decodeNs(const EncodedPacket &p, AVRational timeBase) {
    return p.packet->dts == AV_NOPTS_VALUE ? p.ptsNs : av_rescale_q(p.packet->dts, timeBase, kNs);
}
}  // namespace

const std::vector<FileFormat> &fileFormats() {
    static const std::vector<FileFormat> formats = {
        {"mkv", "Matroska (MKV)", "mkv", "matroska", nullptr, true, true},
        {"mp4", "MP4", "mp4", "mp4", nullptr, true, false},
        {"mov", "QuickTime (MOV)", "mov", "mov", nullptr, true, false},
        {"fmp4", "Fragmented MP4", "mp4", "mp4", "frag_keyframe+empty_moov+default_base_moof", false, true},
        {"fmov", "Fragmented MOV", "mov", "mov", "frag_keyframe+empty_moov+default_base_moof", false, true},
        {"hmp4", "Hybrid MP4", "mp4", "mp4", "hybrid_fragmented", true, true},
        {"hmov", "Hybrid MOV", "mov", "mov", "hybrid_fragmented", true, true},
        {"ts", "MPEG-TS", "ts", "mpegts", nullptr, false, true},
        {"flv", "FLV", "flv", "flv", nullptr, false, true},
        {"hls", "HLS (playlist and segments)", "m3u8", "hls", nullptr, false, true},
    };
    return formats;
}

unsigned keptCodecTag(const AVOutputFormat *format, const AVCodecParameters *params) {
    if (!format || !params || !params->codec_tag || !format->codec_tag) return 0;
    for (const AVCodecTag *const *table = format->codec_tag; *table; ++table)
        if (av_codec_get_id(table, params->codec_tag) == params->codec_id) return params->codec_tag;
    return 0;
}

// Whether FFmpeg reads the muxer's picture stream back as what was written:
// it writes AV1 into MPEG-TS but reads it back as unknown data, a file no
// player here opens. Only a clear "something else" counts against it; a
// sample it cannot make or read says nothing.
static bool readsBackAs(const char *muxer, AVCodecID codec) {
    AVFormatContext *out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, muxer, nullptr) < 0 || !out) return true;
    AVStream *st = avformat_new_stream(out, nullptr);
    uint8_t *bytes = nullptr;
    int size = 0;
    if (st && avio_open_dyn_buf(&out->pb) >= 0) {
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id = codec;
        st->codecpar->width = st->codecpar->height = 64;
        st->time_base = {1, 30};
        if (avformat_write_header(out, nullptr) >= 0) {
            AVPacket *p = av_packet_alloc();
            if (p && av_new_packet(p, 16) >= 0) {
                memset(p->data, 0, 16);
                p->flags = AV_PKT_FLAG_KEY;
                p->pts = p->dts = 0;
                p->duration = 1;
                av_write_frame(out, p);
            }
            av_packet_free(&p);
            av_write_trailer(out);
        }
        size = avio_close_dyn_buf(out->pb, &bytes);
        out->pb = nullptr;
    }
    avformat_free_context(out);
    bool same = true;
    struct Reader { const uint8_t *data; int size, at; } reader{bytes, size, 0};
    uint8_t *buffer = size > 0 ? static_cast<uint8_t *>(av_malloc(4096)) : nullptr;
    AVIOContext *io = buffer ? avio_alloc_context(buffer, 4096, 0, &reader, [](void *o, uint8_t *to, int n) {
        auto *r = static_cast<Reader *>(o);
        n = std::min(n, r->size - r->at);
        if (n <= 0) return int(AVERROR_EOF);
        memcpy(to, r->data + r->at, size_t(n));
        r->at += n;
        return n;
    }, nullptr, nullptr) : nullptr;
    if (!io) av_free(buffer);
    AVFormatContext *in = io ? avformat_alloc_context() : nullptr;
    if (in) {
        in->pb = io;
        const AVInputFormat *format = nullptr;
        if (av_probe_input_buffer(io, &format, nullptr, nullptr, 0, 0) >= 0 &&
            avformat_open_input(&in, nullptr, format, nullptr) >= 0) {
            avformat_find_stream_info(in, nullptr);
            for (unsigned i = 0; i < in->nb_streams; ++i)
                if (in->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO && in->streams[i]->codecpar->codec_id != codec)
                    same = false;
            avformat_close_input(&in);
        } else {
            avformat_free_context(in);   // open_input frees it itself on failure; harmless when null
        }
    }
    if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    av_free(bytes);
    return same;
}

bool formatCarries(const std::string &format, AVCodecID codec) {
    // Asking the muxer to start, not its codec table: the table lists Opus
    // for MOV, but the MOV muxer refuses it when the file is opened.
    static std::mutex mutex;
    static std::map<std::pair<std::string, int>, bool> known;
    const std::pair<std::string, int> key{fileFormat(format).muxer, int(codec)};
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = known.find(key); it != known.end()) return it->second;
    }
    const AVCodecDescriptor *desc = avcodec_descriptor_get(codec);
    bool ok = false;
    AVFormatContext *ctx = nullptr;
    if (desc && avformat_alloc_output_context2(&ctx, nullptr, key.first.c_str(), nullptr) >= 0 && ctx &&
        avformat_query_codec(ctx->oformat, codec, FF_COMPLIANCE_NORMAL) != 0) {
        AVStream *st = avformat_new_stream(ctx, nullptr);
        if (st && avio_open_dyn_buf(&ctx->pb) >= 0) {
            st->codecpar->codec_type = desc->type;
            st->codecpar->codec_id = codec;
            if (desc->type == AVMEDIA_TYPE_AUDIO) {
                st->codecpar->sample_rate = 48000;
                av_channel_layout_default(&st->codecpar->ch_layout, 2);
                st->time_base = {1, 48000};
            } else {
                st->codecpar->width = st->codecpar->height = 64;
                st->time_base = {1, 30};
            }
            const int level = av_log_get_level();
            av_log_set_level(AV_LOG_QUIET);
            ok = avformat_init_output(ctx, nullptr) >= 0;
            uint8_t *sink = nullptr;
            avio_close_dyn_buf(ctx->pb, &sink);
            av_free(sink);
            ctx->pb = nullptr;
            // HLS writes its pictures into MPEG-TS segments.
            const bool hls = key.first == "hls";
            if (ok && desc->type == AVMEDIA_TYPE_VIDEO && (hls || !(ctx->oformat->flags & AVFMT_NOFILE)))
                ok = readsBackAs(hls ? "mpegts" : key.first.c_str(), codec);
            av_log_set_level(level);
        }
    }
    avformat_free_context(ctx);
    std::lock_guard<std::mutex> lock(mutex);
    known[key] = ok;
    return ok;
}

std::string muxerProblem(const std::string &muxer) {
    const AVOutputFormat *f = muxer.empty() ? nullptr : av_guess_format(muxer.c_str(), nullptr, nullptr);
    if (!f || muxer != f->name) return "FFmpeg has no container called \"" + muxer + "\"";
    if (f->video_codec == AV_CODEC_ID_NONE) return muxer + " holds no picture";
    return {};
}

std::string muxerLabel(const std::string &muxer) {
    const AVOutputFormat *f = muxer.empty() ? nullptr : av_guess_format(muxer.c_str(), nullptr, nullptr);
    return f && muxer == f->name && f->long_name ? f->long_name : std::string();
}

const FileFormat &fileFormat(const std::string &id) {
    for (const FileFormat &f : fileFormats()) if (id == f.id) return f;
    if (id.rfind("custom:", 0) == 0 && muxerProblem(id.substr(7)).empty()) {
        // Made once per muxer and kept, so the pointers stay good.
        struct Made { std::string id, label, extension, muxer; FileFormat format; };
        static std::mutex mutex;
        static std::map<std::string, std::unique_ptr<Made>> made;
        std::lock_guard<std::mutex> lock(mutex);
        auto &m = made[id];
        if (!m) {
            m = std::make_unique<Made>();
            m->id = id;
            m->muxer = id.substr(7);
            const AVOutputFormat *f = av_guess_format(m->muxer.c_str(), nullptr, nullptr);
            m->label = f->long_name ? f->long_name : m->muxer;
            const std::string extensions = f->extensions ? f->extensions : "";
            m->extension = extensions.empty() ? m->muxer : extensions.substr(0, extensions.find(','));
            // Those FFmpeg writes chapters into when the file closes (NUT and
            // Ogg take them only in the header, before any are marked).
            static const char *const withChapters[] = {"matroska", "webm", "mp4", "mov", "ipod", "ismv"};
            const bool chapters = std::any_of(std::begin(withChapters), std::end(withChapters),
                                              [&](const char *n) { return m->muxer == n; });
            m->format = {m->id.c_str(), m->label.c_str(), m->extension.c_str(), m->muxer.c_str(), nullptr, chapters, false};
        }
        return m->format;
    }
    return fileFormats().front();
}

// One file. Its spans are the stretches of the encoder's clock it holds —
// several when it was paused — laid end to end from its time zero.
struct FileSink::Part {
    AVFormatContext *fmt = nullptr;
    std::vector<AVBSFContext *> bsf;   // per stream, or null
    std::string path;
    struct Span {
        int64_t from = 0, to = INT64_MAX;
        int64_t base = 0;              // where it starts in the file
    };
    std::vector<Span> spans;
    std::vector<Marker> markers;
    int64_t writtenNs = 0;             // file time reached
    bool open = false;
    // The HLS muxer opens its own files, so its context has no size to read:
    // the segments are counted as it closes them, not its playlist, which it
    // writes afresh each time.
    int (*ownOpen)(AVFormatContext *, AVIOContext **, const char *, int, AVDictionary **) = nullptr;
    int (*ownClose)(AVFormatContext *, AVIOContext *) = nullptr;
    const AVIOContext *playlist = nullptr;
    uint64_t segmentBytes = 0;

    // The span holding this instant; audio from a fresh encoder's zero keeps
    // its priming frame, which starts before it.
    const Span *spanFor(const EncodedPacket &p) const {
        for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
            if (p.ptsNs >= it->to) return nullptr;
            if (p.ptsNs >= it->from || (it->from == 0 && it->base == 0 && p.stream != 0)) return &*it;
        }
        return nullptr;
    }
    int64_t endNs() const { return spans.empty() ? INT64_MAX : spans.back().to; }
};

FileSink::FileSink() = default;

FileSink::~FileSink() {
    stopThread();
    if (m_part) closePart(*m_part);
    if (m_next) closePart(*m_next);
}

bool FileSink::openPart(Part &part, const std::string &path, std::string *error) {
    const FileFormat &format = fileFormat(m_settings.format);
    part.path = path;
    int r = avformat_alloc_output_context2(&part.fmt, nullptr, format.muxer, path.c_str());
    if (r < 0 || !part.fmt) {
        if (error) *error = "container: " + avErr(r);
        return false;
    }
    auto addStream = [&](const AVCodecParameters *params, AVRational timeBase, int index, int track) -> bool {
        AVStream *st = avformat_new_stream(part.fmt, nullptr);
        if (!st) return false;
        st->time_base = timeBase;
        AVBSFContext *bsf = nullptr;
        // MPEG-TS players join mid-stream: headers before every keyframe.
        const bool inBand = !strcmp(format.muxer, "mpegts") || !strcmp(format.muxer, "hls");
        if (inBand && index == 0 && (params->codec_id == AV_CODEC_ID_H264 || params->codec_id == AV_CODEC_ID_HEVC)) {
            const AVBitStreamFilter *f = av_bsf_get_by_name("dump_extra");
            if (f && av_bsf_alloc(f, &bsf) >= 0) {
                avcodec_parameters_copy(bsf->par_in, params);
                bsf->time_base_in = timeBase;
                av_opt_set(bsf->priv_data, "freq", "keyframe", 0);
                if (av_bsf_init(bsf) < 0) av_bsf_free(&bsf);
            }
        }
        avcodec_parameters_copy(st->codecpar, bsf ? bsf->par_out : params);
        st->codecpar->codec_tag = keptCodecTag(part.fmt->oformat, st->codecpar);
        part.bsf.push_back(bsf);
        if (index == 0) {
            st->avg_frame_rate = m_layout->frameRate;
        } else {
            st->disposition = index == 1 ? AV_DISPOSITION_DEFAULT : 0;
            const std::string title = "Track " + std::to_string(track + 1);
            av_dict_set(&st->metadata, "title", title.c_str(), 0);
            av_dict_set(&st->metadata, "handler_name", title.c_str(), 0);
        }
        return true;
    };
    if (!addStream(m_layout->video, m_layout->videoTimeBase, 0, 0)) { if (error) *error = "video stream"; return false; }
    for (size_t i = 0; i < m_layout->audio.size(); ++i)
        if (!addStream(m_layout->audio[i].params, m_layout->audio[i].timeBase, int(i) + 1, m_layout->audio[i].track)) {
            if (error) *error = "audio stream";
            return false;
        }
    AVDictionary *opts = nullptr;
    if (format.movflags) av_dict_set(&opts, "movflags", format.movflags, 0);
    if (!strcmp(format.muxer, "hls")) {
        const std::filesystem::path p(path);
        const std::string segments = (p.parent_path() / (p.stem().string() + "_%05d.ts")).string();
        av_dict_set(&opts, "hls_segment_filename", segments.c_str(), 0);
        av_dict_set(&opts, "hls_list_size", "0", 0);
        av_dict_set(&opts, "hls_playlist_type", "event", 0);
    }
    if (!strcmp(format.muxer, "hls")) {
        part.ownOpen = part.fmt->io_open;
        part.ownClose = part.fmt->io_close2;
        part.fmt->opaque = &part;
        part.fmt->io_open = [](AVFormatContext *s, AVIOContext **pb, const char *url, int flags, AVDictionary **options) {
            Part *p = static_cast<Part *>(s->opaque);
            const int opened = p->ownOpen(s, pb, url, flags, options);
            if (opened >= 0 && strstr(url, ".m3u8")) p->playlist = *pb;
            return opened;
        };
        part.fmt->io_close2 = [](AVFormatContext *s, AVIOContext *pb) {
            Part *p = static_cast<Part *>(s->opaque);
            if (pb == p->playlist) p->playlist = nullptr;
            else if (pb) p->segmentBytes += uint64_t(avio_tell(pb));
            return p->ownClose(s, pb);
        };
    }
    const auto own = parseEncoderOptions(m_settings.options);
    for (const auto &[key, value] : own) av_dict_set(&opts, key.c_str(), value.c_str(), 0);
    if (!(part.fmt->oformat->flags & AVFMT_NOFILE)) {
        r = avio_open(&part.fmt->pb, path.c_str(), AVIO_FLAG_WRITE);
        if (r < 0) {
            av_dict_free(&opts);
            if (error) *error = "cannot write " + path + ": " + avErr(r);
            return false;
        }
    }
    r = avformat_write_header(part.fmt, &opts);
    // What is left was taken by nothing.
    std::string unknown;
    for (const auto &kv : own)
        if (av_dict_get(opts, kv.first.c_str(), nullptr, 0)) unknown = kv.first;
    av_dict_free(&opts);
    if (r < 0) {
        if (error) *error = "header: " + avErr(r);
        return false;
    }
    if (!unknown.empty()) {
        if (error) *error = "the " + std::string(part.fmt->oformat->name) + " container has no option \"" + unknown + "\"";
        return false;
    }
    part.open = true;
    return true;
}

void FileSink::closePart(Part &part) {
    // Stopped before its first picture: only a header, which no player opens.
    const bool nothing = part.open && part.spans.empty();
    if (part.open) {
        // Markers between the spans' file times; each runs to the next.
        std::vector<std::pair<int64_t, std::string>> marks;
        for (const Marker &m : part.markers) {
            int64_t at = -1;
            for (const Part::Span &s : part.spans)
                if (m.ns < s.to) { at = s.base + std::max<int64_t>(0, m.ns - s.from); break; }
            if (at >= 0 && at <= part.writtenNs) marks.emplace_back(at, m.name);
        }
        std::sort(marks.begin(), marks.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        // Unnamed ones are numbered in the order they were marked.
        for (size_t i = 0; i < marks.size(); ++i)
            if (marks[i].second.empty()) marks[i].second = "Chapter " + std::to_string(i + 1);
        // MP4 and QuickTime chapters run end to end from the start: one there,
        // so the first marker keeps its own time.
        const bool fromStart = !strcmp(part.fmt->oformat->name, "mp4") || !strcmp(part.fmt->oformat->name, "mov");
        if (fromStart && !marks.empty() && marks.front().first > 0) marks.insert(marks.begin(), {0, "Start"});
        if (fileFormat(m_settings.format).chapters) {
            for (size_t i = 0; i < marks.size(); ++i) {
                const int64_t start = av_rescale_q(marks[i].first, kNs, {1, 1000});
                const int64_t end = av_rescale_q(i + 1 < marks.size() ? marks[i + 1].first : part.writtenNs, kNs, {1, 1000});
                AVChapter *c = static_cast<AVChapter *>(av_mallocz(sizeof(AVChapter)));
                if (!c) break;
                c->id = int64_t(i) + 1;
                c->time_base = {1, 1000};
                c->start = start;
                c->end = std::max(end, start + 1);
                av_dict_set(&c->metadata, "title", marks[i].second.c_str(), 0);
                void *grown = av_realloc_array(part.fmt->chapters, part.fmt->nb_chapters + 1, sizeof(AVChapter *));
                if (!grown) { av_dict_free(&c->metadata); av_free(c); break; }
                part.fmt->chapters = static_cast<AVChapter **>(grown);
                part.fmt->chapters[part.fmt->nb_chapters++] = c;
            }
        }
        // What the filters still hold, then the index.
        for (size_t i = 0; i < part.bsf.size(); ++i) {
            if (!part.bsf[i]) continue;
            av_bsf_send_packet(part.bsf[i], nullptr);
            AVPacket *out = av_packet_alloc();
            while (av_bsf_receive_packet(part.bsf[i], out) >= 0) {
                out->stream_index = int(i);
                av_packet_rescale_ts(out, i == 0 ? m_layout->videoTimeBase : m_layout->audio[i - 1].timeBase,
                                     part.fmt->streams[i]->time_base);
                av_interleaved_write_frame(part.fmt, out);
            }
            av_packet_free(&out);
        }
        // HLS ends by writing out a segment, even one with nothing in it: with
        // no picture there is none to end, and none is left in the folder.
        const bool noSegment = nothing && !strcmp(part.fmt->oformat->name, "hls");
        const int r = noSegment ? 0 : av_write_trailer(part.fmt);
        if (r < 0 && onError && !m_failed) onError("finishing " + part.path + ": " + avErr(r));
        if (r < 0) {
            // Nothing more can be written, to this file or the next: the sink
            // ends here, whatever else its encoder goes on feeding.
            m_failed = true;
            markDone();
        }
    }
    if (!nothing) m_closedBytes += part.segmentBytes;
    if (part.fmt && part.fmt->pb) {
        if (!nothing) m_closedBytes += uint64_t(avio_tell(part.fmt->pb));
        // The last of it reaches the disk here: a full disk shows now.
        if (const int closed = avio_closep(&part.fmt->pb); closed < 0 && onError && !m_failed)
            onError("closing " + part.path + ": " + avErr(closed));
    }
    for (AVBSFContext *&b : part.bsf) av_bsf_free(&b);
    part.bsf.clear();
    if (part.fmt) avformat_free_context(part.fmt);
    part.fmt = nullptr;
    if (nothing) {
        part.open = false;
        std::error_code ignored;
        std::filesystem::remove(part.path, ignored);
    } else if (part.open) {
        part.open = false;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_closed.push_back(part.path);
        }
        if (onFileClosed) onFileClosed(part.path);
    }
}

bool FileSink::start(const Settings &settings, std::shared_ptr<const EncodedLayout> layout, std::string *error) {
    m_settings = settings;
    m_layout = std::move(layout);
    m_part = std::make_unique<Part>();
    if (!openPart(*m_part, settings.path, error)) {
        closePart(*m_part);
        m_part.reset();
        std::error_code ignored;
        std::filesystem::remove(settings.path, ignored);
        return false;
    }
    m_waiting = true;
    m_waitAfter = settings.startNs;
    m_stats = Stats{};
    m_stats.files = 1;
    m_stats.path = settings.path;
    startThread();
    return true;
}

void FileSink::finishAt(int64_t ns) {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_finishAt = std::min(m_finishAt, ns);
    }
    poke();
}

void FileSink::pause(int64_t ns) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_pauseAt == INT64_MAX && !m_resumeAsked) {
        m_pauseAt = ns;
        m_stats.paused = true;
    }
}

void FileSink::resume(int64_t ns) {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_pauseAt == INT64_MAX || m_resumeAsked) return;
        m_resumeAsked = true;
        m_resumeAfter = std::max(ns, m_pauseAt);
        ns = m_resumeAfter;
    }
    if (wantKeyframe) wantKeyframe(ns);
}

bool FileSink::paused() const {
    // Resumed as soon as it is asked; the file reopens at the next keyframe.
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_stats.paused && !m_resumeAsked;
}

void FileSink::split() {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_splitAsked = true;
    }
    if (wantKeyframe) wantKeyframe(0);
}

void FileSink::chapter(const std::string &name, int64_t ns) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_markers.push_back({name, ns});
}

FileSink::Stats FileSink::stats() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_stats;
}

std::vector<std::string> FileSink::closedFiles() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_closed;
}

void FileSink::write(Part &part, const EncodedPacket &p, bool pastEnd) {
    // Failed: what is left to write would only fail again, each with a message.
    if (m_failed) return;
    const Part::Span *span = pastEnd ? &part.spans.back() : part.spanFor(p);
    if (!span || !part.open) return;
    const AVRational tb = p.stream == 0 ? m_layout->videoTimeBase : m_layout->audio[size_t(p.stream - 1)].timeBase;
    const int64_t shift = av_rescale_q(span->from - span->base, kNs, tb);
    AVPacket *pkt = av_packet_clone(p.packet.get());
    if (!pkt) return;
    if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= shift;
    if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= shift;
    pkt->stream_index = p.stream;
    part.writtenNs = std::max(part.writtenNs, span->base + (p.endNs - span->from));
    auto send = [&](AVPacket *out) -> bool {
        av_packet_rescale_ts(out, tb, part.fmt->streams[p.stream]->time_base);
        out->stream_index = p.stream;
        const int r = av_interleaved_write_frame(part.fmt, out);
        if (r < 0) {
            m_failed = true;
            markDone();
            if (onError) onError("writing " + part.path + ": " + avErr(r));
            return false;
        }
        return true;
    };
    if (AVBSFContext *bsf = part.bsf[size_t(p.stream)]) {
        if (av_bsf_send_packet(bsf, pkt) >= 0) {
            AVPacket *out = av_packet_alloc();
            while (av_bsf_receive_packet(bsf, out) >= 0 && send(out)) {}
            av_packet_free(&out);
        }
    } else {
        send(pkt);
    }
    av_packet_free(&pkt);
}

// A span from a start or a resume: the sound from where it was asked for,
// the picture from its keyframe just after.
void FileSink::startWriting(int64_t keyNs) {
    Part &part = *m_part;
    Part::Span s;
    s.from = std::min(keyNs, m_waitAfter);
    if (!part.spans.empty()) {
        const Part::Span &last = part.spans.back();
        s.base = last.base + (last.to - last.from);
    }
    part.spans.push_back(s);
    m_waiting = false;
}

void FileSink::handle(const EncodedPacket &p) {
    if (m_failed) return;
    int64_t finishAt, pauseAt, resumeAfter;
    bool resumeAsked, splitAsked;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        finishAt = m_finishAt;
        pauseAt = m_pauseAt;
        resumeAsked = m_resumeAsked;
        resumeAfter = m_resumeAfter;
        splitAsked = m_splitAsked;
        for (const Marker &m : m_markers) (m_next && m.ns >= m_next->spans.front().from ? m_next : m_part)->markers.push_back(m);
        m_markers.clear();
    }
    Part &part = *m_part;
    // A stop or a pause bounds the open span; packets past it go nowhere.
    const int64_t bound = std::min(finishAt, pauseAt);
    if (!m_waiting && !part.spans.empty() && bound < part.spans.back().to) {
        part.spans.back().to = std::max(bound, part.spans.back().from);
        // A cut not reached before the stop or pause is no longer wanted.
        if (m_splitAt != INT64_MAX && bound <= m_splitAt) {
            m_splitAt = INT64_MAX;
            m_nextHeld.clear();
        }
    }
    if (m_next && bound < m_next->spans.back().to) m_next->spans.back().to = std::max(bound, m_next->spans.back().from);
    if (pauseAt != INT64_MAX && resumeAsked && !m_waiting && p.ptsNs >= pauseAt && finishAt == INT64_MAX) {
        // Resuming: the next keyframe from the resume reopens it.
        m_waiting = true;
        m_waitAfter = resumeAfter;
    }

    if (m_waiting) {
        // Even with a stop asked for, what comes before it still opens the file.
        const bool canOpen = (pauseAt == INT64_MAX || resumeAsked) && p.ptsNs < finishAt;
        if (resumeAsked) m_waitAfter = std::max(m_waitAfter, resumeAfter);
        if (canOpen && p.stream == 0 && p.key && p.ptsNs >= m_waitAfter) {
            if (pauseAt != INT64_MAX) {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_pauseAt = INT64_MAX;
                m_resumeAsked = false;
                m_stats.paused = false;
            }
            startWriting(p.ptsNs);
            if (finishAt != INT64_MAX) part.spans.back().to = std::max(finishAt, p.ptsNs);
            write(part, p);
            for (const EncodedPacket &held : m_held) write(part, held);
            m_held.clear();
        } else if (p.stream != 0) {
            // Before a pause, it still belongs; after the resume, it might.
            if (part.spanFor(p)) write(part, p);
            // Held by its end: a fresh encoder's priming frame starts before zero.
            else if (canOpen && p.endNs >= m_waitAfter && m_held.size() < kMaxHeld) m_held.push_back(p);
        }
    } else if (!m_waiting) {
        // A split: the cut is chosen ahead of everything seen so far (the
        // sound runs ahead of the pictures), with a keyframe asked for there.
        const Part::Span &open = part.spans.back();
        const int64_t inFile = open.base + (p.ptsNs - open.from);
        const uint64_t bytes = part.fmt && part.fmt->pb ? uint64_t(avio_tell(part.fmt->pb)) : 0;
        const bool due = splitAsked || (p.ptsNs >= m_splitRetryNs &&
                                        ((m_settings.splitSeconds > 0 && inFile >= m_settings.splitSeconds * 1'000'000'000) ||
                                         (m_settings.splitBytes > 0 && bytes >= uint64_t(m_settings.splitBytes))));
        const int64_t frameNs = av_rescale_q(1, av_inv_q(m_layout->frameRate), kNs);
        if (due && m_splitAt == INT64_MAX && !m_next && m_settings.nextPath && pauseAt == INT64_MAX && finishAt == INT64_MAX) {
            int64_t latest = p.ptsNs;
            for (int64_t seen : m_seen) latest = std::max(latest, seen);
            m_splitAt = latest + frameNs;
            part.spans.back().to = std::min(part.spans.back().to, m_splitAt);
            if (splitAsked) m_splitWarned = false;  // asked for by hand: it gets its answer
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_splitAsked = false;
            if (wantKeyframe) wantKeyframe(m_splitAt);
        }
        if (m_splitAt != INT64_MAX && p.ptsNs >= m_splitAt) {
            if (p.stream != 0) {
                m_nextHeld.push_back(p);         // the new file's, once it opens
            } else if (!p.key) {
                // Encoded before the keyframe was asked for: it stays in this
                // file, the cut moves past it, and so does the sound before it.
                m_splitAt = p.endNs;
                part.spans.back().to = m_splitAt;
                write(part, p);
                std::vector<EncodedPacket> later;
                for (const EncodedPacket &held : m_nextHeld) (held.ptsNs < m_splitAt ? write(part, held) : later.push_back(held));
                m_nextHeld.swap(later);
                if (wantKeyframe) wantKeyframe(m_splitAt);
            } else {
                auto next = std::make_unique<Part>();
                std::string error;
                if (openPart(*next, m_settings.nextPath(m_partNumber + 1), &error)) {
                    ++m_partNumber;
                    m_splitRetryNs = INT64_MIN;
                    m_splitWarned = false;
                    // The sound from the cut, the picture from its keyframe.
                    next->spans.push_back({m_splitAt, std::min(finishAt, pauseAt), 0});
                    m_next = std::move(next);
                    write(*m_next, p);
                    for (const EncodedPacket &held : m_nextHeld) write(*m_next, held);
                    std::lock_guard<std::mutex> lock(m_stateMutex);
                    m_stats.files = m_partNumber;
                    m_stats.path = m_next->path;
                } else {
                    // Carry on in this file; what the new one got is not kept.
                    closePart(*next);
                    if (!next->path.empty()) std::remove(next->path.c_str());
                    part.spans.back().to = INT64_MAX;
                    write(part, p);
                    for (const EncodedPacket &held : m_nextHeld) write(part, held);
                    m_splitRetryNs = p.ptsNs + kSplitRetryNs;
                    if (onWarning && !m_splitWarned) onWarning(error);
                    m_splitWarned = true;
                }
                m_nextHeld.clear();
                m_splitAt = INT64_MAX;
            }
        } else if (part.spanFor(p)) {
            write(part, p);
        } else if (m_next && m_next->spanFor(p)) {
            write(*m_next, p);
        } else if (p.stream == 0 && pauseAt == INT64_MAX && p.ptsNs >= finishAt && decodeNs(p, m_layout->videoTimeBase) < finishAt) {
            // Past the stop but decoded before it: pictures before the stop
            // still to come look back at it, and would not decode without it.
            write(m_next ? *m_next : part, p, true);
        }
    }

    // A file closes once every stream has gone past its end.
    m_seen.resize(m_layout->audio.size() + 1, INT64_MIN);
    m_seen[size_t(p.stream)] = std::max(m_seen[size_t(p.stream)], p.ptsNs);
    if (p.stream == 0) m_decodedNs = std::max(m_decodedNs, decodeNs(p, m_layout->videoTimeBase));
    checkClosing();
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        const auto bytesIn = [](const std::unique_ptr<Part> &part) {
            if (!part) return uint64_t(0);
            return part->segmentBytes + (part->fmt && part->fmt->pb ? uint64_t(avio_tell(part->fmt->pb)) : 0);
        };
        m_stats.bytes = m_closedBytes + bytesIn(m_part) + bytesIn(m_next);
        m_stats.durationNs = m_doneNs + (m_part ? m_part->writtenNs : 0) + (m_next ? m_next->writtenNs : 0);
    }
}

void FileSink::checkClosing() {
    for (;;) {
        if (!m_part) return;
        int64_t finishAt;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            finishAt = m_finishAt;
        }
        // Stopped before anything opened a span: done once everything is past the stop.
        const int64_t end = m_part->spans.empty() ? finishAt : m_part->endNs();
        // Pictures come in decoding order: only one decoded past the end says
        // none from before the end is still on its way.
        bool past = end != INT64_MAX && !m_seen.empty() && m_decodedNs >= end;
        for (size_t i = 1; i < m_seen.size(); ++i) past = past && m_seen[i] >= end;
        if (!past) return;
        // A paused file waits for its resume rather than closing.
        if (finishAt == INT64_MAX && !m_next) return;
        m_doneNs += m_part->writtenNs;
        closePart(*m_part);
        m_part = std::move(m_next);
        if (!m_part) {
            markDone();
            return;
        }
    }
}

void FileSink::finish() {
    if (m_part) { m_doneNs += m_part->writtenNs; closePart(*m_part); }
    if (m_next) { m_doneNs += m_next->writtenNs; closePart(*m_next); }
    m_part.reset();
    m_next.reset();
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_stats.bytes = m_closedBytes;
    m_stats.durationNs = m_doneNs;
}

void FileSink::overflow() {
    // Closed with what is written: the rest would only pile up in memory.
    m_failed = true;
    markDone();
    if (onError) onError("the disk is not keeping up with the recording, so it stopped here");
}
