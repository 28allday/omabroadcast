#include "remux.h"
#include "file_sink.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace {
std::string avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

std::string trackName(const AVStream *s, int number) {
    return std::string(s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? "picture" : "sound track ") +
           (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? "" : std::to_string(number));
}
}  // namespace

std::string remuxTarget(const std::string &in, const std::string &extension) {
    const std::filesystem::path p(in);
    const std::string base = (p.parent_path() / p.stem()).string();
    std::string out = base + "." + extension;
    for (int n = 2; std::filesystem::exists(out) || out == in; ++n) out = base + " (" + std::to_string(n) + ")." + extension;
    return out;
}

bool remuxFile(const std::string &in, const std::string &out, std::string *error, const std::function<bool(double)> &progress,
               std::string *note) {
    AVFormatContext *ic = nullptr, *oc = nullptr;
    bool opened = false, ok = false;
    auto fail = [&](const std::string &message) {
        if (error) *error = message;
        return false;
    };
    auto run = [&]() -> bool {
        int r = avformat_open_input(&ic, in.c_str(), nullptr, nullptr);
        if (r < 0) return fail("cannot open " + in + ": " + avErr(r));
        if ((r = avformat_find_stream_info(ic, nullptr)) < 0) return fail("cannot read " + in + ": " + avErr(r));
        r = avformat_alloc_output_context2(&oc, nullptr, nullptr, out.c_str());
        if (r < 0 || !oc) return fail("no container for " + out);
        const std::string container = oc->oformat->long_name ? oc->oformat->long_name : oc->oformat->name;
        std::vector<int> map(ic->nb_streams, -1);
        int outputs = 0, soundTracks = 0;
        std::string codecs;   // for people, when the container turns them down
        for (unsigned i = 0; i < ic->nb_streams; ++i) {
            const AVStream *s = ic->streams[i];
            const AVMediaType type = s->codecpar->codec_type;
            if ((type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) || (s->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;
            if (type == AVMEDIA_TYPE_AUDIO) ++soundTracks;
            // 0 is a definite no; a muxer with no table answers "don't know"
            // (below 0), and its header says.
            if (avformat_query_codec(oc->oformat, s->codecpar->codec_id, FF_COMPLIANCE_NORMAL) == 0)
                return fail(container + " can't hold " + avcodec_get_name(s->codecpar->codec_id) + " (" +
                            trackName(s, soundTracks) + "); choose another container or record in a codec it takes");
            AVStream *o = avformat_new_stream(oc, nullptr);
            if (!o) return fail("out of memory");
            avcodec_parameters_copy(o->codecpar, s->codecpar);
            o->codecpar->codec_tag = keptCodecTag(oc->oformat, o->codecpar);
            // Matroska keeps no layout for PCM; MP4's PCM box needs one.
            if (type == AVMEDIA_TYPE_AUDIO && o->codecpar->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
                const int channels = o->codecpar->ch_layout.nb_channels;
                av_channel_layout_uninit(&o->codecpar->ch_layout);
                av_channel_layout_default(&o->codecpar->ch_layout, channels);
            }
            o->time_base = s->time_base;
            o->avg_frame_rate = s->avg_frame_rate;
            o->disposition = s->disposition;
            av_dict_copy(&o->metadata, s->metadata, 0);
            map[i] = outputs++;
            codecs += std::string(codecs.empty() ? "" : ", ") + avcodec_get_name(s->codecpar->codec_id) + " (" + trackName(s, soundTracks) + ")";
        }
        if (outputs == 0) return fail("nothing to copy in " + in);
        av_dict_copy(&oc->metadata, ic->metadata, 0);
        const bool mp4Family = !std::strcmp(oc->oformat->name, "mp4") || !std::strcmp(oc->oformat->name, "mov");
        auto addChapter = [&](int64_t id, AVRational tb, int64_t start, int64_t end, const AVDictionary *metadata, const char *title) {
            AVChapter *copy = static_cast<AVChapter *>(av_mallocz(sizeof(AVChapter)));
            if (!copy) return;
            copy->id = id;
            copy->time_base = tb;
            copy->start = start;
            copy->end = end;
            av_dict_copy(&copy->metadata, metadata, 0);
            if (title) av_dict_set(&copy->metadata, "title", title, 0);
            void *grown = av_realloc_array(oc->chapters, oc->nb_chapters + 1, sizeof(AVChapter *));
            if (!grown) { av_dict_free(&copy->metadata); av_free(copy); return; }
            oc->chapters = static_cast<AVChapter **>(grown);
            oc->chapters[oc->nb_chapters++] = copy;
        };
        // MP4 and QuickTime chapters run end to end from the start, as the
        // recorder writes them: a "Start" one first, so the rest keep their times.
        if (mp4Family && ic->nb_chapters > 0 && ic->chapters[0]->start > 0)
            addChapter(0, ic->chapters[0]->time_base, 0, ic->chapters[0]->start, nullptr, "Start");
        for (unsigned i = 0; i < ic->nb_chapters; ++i) {
            const AVChapter *c = ic->chapters[i];
            addChapter(c->id, c->time_base, c->start, c->end, c->metadata, nullptr);
        }
        if (!(oc->oformat->flags & AVFMT_NOFILE)) {
            if ((r = avio_open(&oc->pb, out.c_str(), AVIO_FLAG_WRITE)) < 0) return fail("cannot write " + out + ": " + avErr(r));
        }
        opened = true;
        AVDictionary *opts = nullptr;
        if (mp4Family) av_dict_set(&opts, "movflags", "+faststart", 0);
        r = avformat_write_header(oc, &opts);
        av_dict_free(&opts);
        if (r < 0) return fail(container + " would not take " + codecs + ": " + avErr(r));
        const double duration = ic->duration > 0 ? double(ic->duration) / AV_TIME_BASE : 0;
        AVPacket *pkt = av_packet_alloc();
        int count = 0;
        int written = 0, dropped = 0;
        double fraction = 0;  // how far, as last known
        bool stopped = false;
        while ((r = av_read_frame(ic, pkt)) >= 0) {
            const int index = pkt->stream_index;
            if (index < 0 || unsigned(index) >= map.size() || map[size_t(index)] < 0) { av_packet_unref(pkt); continue; }
            const AVRational from = ic->streams[index]->time_base;
            // Asked every 64 packets whether to go on, with how far it is when it
            // can tell (no duration or time stamp: the last fraction again).
            if (progress && ++count % 64 == 0) {
                if (duration > 0 && pkt->pts != AV_NOPTS_VALUE) {
                    const double at = (pkt->pts * av_q2d(from)) - (ic->start_time != AV_NOPTS_VALUE ? double(ic->start_time) / AV_TIME_BASE : 0);
                    fraction = std::clamp(at / duration, 0.0, 1.0);
                }
                if (!progress(fraction)) { stopped = true; av_packet_unref(pkt); break; }
            }
            pkt->stream_index = map[size_t(index)];
            av_packet_rescale_ts(pkt, from, oc->streams[pkt->stream_index]->time_base);
            pkt->pos = -1;
            if ((r = av_interleaved_write_frame(oc, pkt)) < 0) {
                // A time that runs backwards in a damaged file: that packet goes,
                // and the rest is still good. Anything else is the disk.
                if (r == AVERROR(EINVAL)) { ++dropped; continue; }
                av_packet_free(&pkt);
                return fail("writing " + out + ": " + avErr(r));
            }
            ++written;
        }
        av_packet_free(&pkt);
        if (stopped) return fail("stopped");
        // Cut short: what came before it is worth keeping, and the source is
        // still there as it was. With nothing out, it is simply unreadable.
        if (r != AVERROR_EOF && !written) return fail("reading " + in + ": " + avErr(r));
        if (!written) return fail("nothing could be copied from " + in);
        if (note) {
            if (r != AVERROR_EOF) *note = "the source is damaged (" + avErr(r) + "), so only the part before that was copied";
            else if (dropped) *note = "the source is damaged: " + std::to_string(dropped) + " packets with bad times were left out";
        }
        if ((r = av_write_trailer(oc)) < 0) return fail("finishing " + out + " (" + codecs + "): " + avErr(r));
        if (progress) progress(1.0);
        return true;
    };
    ok = run();
    if (oc) {
        if (oc->pb) avio_closep(&oc->pb);
        avformat_free_context(oc);
    }
    if (ic) avformat_close_input(&ic);
    if (!ok && opened) {
        std::error_code ignored;
        std::filesystem::remove(out, ignored);
    }
    return ok;
}

RemuxQueue::~RemuxQueue() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
        for (Job &j : m_jobs)
            if (j.state == State::Running) m_cancelId = j.id;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

int RemuxQueue::add(const std::string &in, const std::string &out) {
    int id;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        id = m_nextId++;
        Job job;
        job.id = id;
        job.in = in;
        job.out = out;
        m_jobs.push_back(job);
        if (!m_thread.joinable()) m_thread = std::thread([this] { run(); });
    }
    m_wake.notify_all();
    return id;
}

void RemuxQueue::cancel(int id) {
    Job changed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (Job &j : m_jobs) {
            if (j.id != id) continue;
            if (j.state == State::Running) m_cancelId = id;
            if (j.state != State::Waiting) return;
            j.state = State::Cancelled;
            changed = j;
        }
    }
    if (changed.id && onChanged) onChanged(changed);
}

void RemuxQueue::cancelAll() {
    for (const Job &j : jobs()) cancel(j.id);
}

void RemuxQueue::clearFinished() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::erase_if(m_jobs, [](const Job &j) { return j.state != State::Waiting && j.state != State::Running; });
}

std::vector<RemuxQueue::Job> RemuxQueue::jobs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_jobs;
}

bool RemuxQueue::busy() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::any_of(m_jobs.begin(), m_jobs.end(), [](const Job &j) { return j.state == State::Waiting || j.state == State::Running; });
}

void RemuxQueue::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            const auto next = [&] {
                return std::find_if(m_jobs.begin(), m_jobs.end(), [](const Job &j) { return j.state == State::Waiting; });
            };
            m_wake.wait(lock, [&] { return m_quit || next() != m_jobs.end(); });
            if (m_quit) return;
            Job &j = *next();
            if (j.out.empty()) j.out = remuxTarget(j.in);
            j.state = State::Running;
            m_cancelId = 0;
            job = j;
        }
        auto update = [&](auto change) {
            Job now;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (Job &j : m_jobs)
                    if (j.id == job.id) { change(j); now = j; }
            }
            if (now.id && onChanged) onChanged(now);
        };
        update([](Job &) {});
        std::string error, note;
        const bool ok = remuxFile(job.in, job.out, &error, [&](double p) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_cancelId == job.id) return false;
            }
            update([p](Job &j) { j.progress = p; });
            return true;
        }, &note);
        bool cancelled;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cancelled = m_cancelId == job.id;
        }
        update([&](Job &j) {
            j.state = ok ? State::Done : cancelled ? State::Cancelled : State::Failed;
            j.error = ok || cancelled ? std::string() : error;
            j.note = note;
            if (ok) j.progress = 1;
        });
    }
}
