#include "core/recording_player.h"

#include "audio/audio_playback.h"
#include "capture/media_stream.h"
#include "preview_view.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QQuickWindow>
#include <QImage>
#include <QLocale>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iterator>

namespace {

int64_t monotonicNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

QString clock(double seconds) {
    const qint64 ms = qRound64(std::max(0.0, seconds) * 1000);
    return QStringLiteral("%1:%2:%3.%4").arg(ms / 3600000).arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0')).arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

QString codecName(AVCodecID id) {
    const AVCodecDescriptor *d = avcodec_descriptor_get(id);
    return d ? QString::fromUtf8(d->name).toUpper() : QStringLiteral("?");
}

QString rate(int64_t bitsPerSecond) {
    if (bitsPerSecond <= 0) return {};
    return bitsPerSecond >= 10'000'000 ? QCoreApplication::translate("RecordingPlayer", "%1 Mb/s").arg(bitsPerSecond / 1e6, 0, 'f', 1)
                                       : QCoreApplication::translate("RecordingPlayer", "%1 kb/s").arg(qRound64(bitsPerSecond / 1e3));
}

constexpr int kBackStepMs = 125;  // backward shuttle: a picture this often, where they come that fast
// A seek's picture is waited for this long before another seek may take its place.
constexpr int kSeekWaitMs = 5000;
constexpr int kSoundLatencyFrames = 1024;  // asked of the sound output
constexpr auto kPumpEvery = std::chrono::milliseconds(4);

}  // namespace

// An HDR or wide-colour picture's samples to the screen's, by table: the
// engine's own curves (engine/frame.h), read once for a kind of picture.
struct RecordingPlayer::ScreenTables {
    static constexpr int kSize = 4096;
    engine::Transfer transfer = engine::Transfer::Sdr;
    bool wide = false;
    bool hdr = false, hlg = false;
    float top = 1;                            // the brightest a channel is taken to be, SDR white being 1
    float gamut[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<float, kSize> light{};         // a 12-bit sample to light (HLG: before its system gamma)
    std::array<float, kSize + 1> ootf{};      // HLG, by the root of the scene's luminance: what its light is multiplied by
    std::array<float, kSize + 1> gain{};      // by the root of the brightest channel over `top`: the tone map
    std::array<uint8_t, kSize + 1> code{};    // by the root of light 0..1: the screen's code

    ScreenTables(engine::Transfer curve, bool wideGamut) : transfer(curve), wide(wideGamut) {
        engine::YuvInput in;
        in.transfer = curve;
        in.wideGamut = wideGamut;
        const engine::HdrLevels levels{};
        const engine::InputParams q = engine::inputParams(in, levels);
        hdr = q.kind != 0;
        hlg = q.kind == 2;
        std::copy(std::begin(q.gamut), std::end(q.gamut), std::begin(gamut));
        for (int i = 0; i < kSize; ++i) {
            const float e = float(i) / (kSize - 1);
            light[size_t(i)] = q.kind == 0 ? engine::srgbToLinear(e) : q.kind == 1 ? engine::pqDecode(e) * q.scale : engine::hlgDecode(e);
        }
        // Brought into BT.709, a pure BT.2020 colour passes its own white.
        top = (hdr ? q.scale : 1.f) * (wide ? 1.7f : 1.f);
        // The Tone map filter's own curve at its own settings: the HDR peak down to SDR white.
        const float white = levels.sdrWhite > 1.f ? levels.sdrWhite : 1.f, peak = levels.hdrPeak > white ? levels.hdrPeak : white;
        float p[4];
        p[0] = white;
        p[1] = engine::pqEncode(peak / 10000.f);
        p[2] = engine::pqEncode(white / 10000.f) / p[1];
        p[3] = 1.5f * p[2] - 0.5f;
        const int mode = peak <= white ? 2 : 0;
        for (int i = 0; i <= kSize; ++i) {
            const float root = float(i) / kSize, part = root * root;
            ootf[size_t(i)] = part > 0.f ? std::pow(part, q.gamma1) * q.scale : 0.f;
            const float m = part * top;
            gain[size_t(i)] = !hdr ? 1.f : m > 0.f ? engine::toneMapValue(mode, p, m) / m : 0.f;
            code[size_t(i)] = uint8_t(std::lround(engine::linearToSrgb(part) * 255.f));
        }
    }

    // One pixel: three 16-bit samples to three of the screen's.
    void convert(const uint16_t *in, uint8_t *out) const {
        float c[3] = {light[in[0] >> 4], light[in[1] >> 4], light[in[2] >> 4]};
        if (hlg) {
            const float ys = std::clamp(0.2627f * c[0] + 0.678f * c[1] + 0.0593f * c[2], 0.f, 1.f);
            const float f = ootf[size_t(std::sqrt(ys) * kSize + 0.5f)];
            for (float &v : c) v *= f;
        }
        if (wide) {
            const float r = c[0], g = c[1], b = c[2];
            for (int k = 0; k < 3; ++k) c[k] = gamut[k * 3] * r + gamut[k * 3 + 1] * g + gamut[k * 3 + 2] * b;
        }
        if (hdr) {
            // Hue kept: the brightest channel follows the curve, the others in proportion.
            const float m = std::clamp(std::max({c[0], c[1], c[2]}), 0.f, top);
            const float k = gain[size_t(std::sqrt(m / top) * kSize + 0.5f)];
            for (float &v : c) v *= k;
        }
        for (int k = 0; k < 3; ++k) out[k] = code[size_t(std::sqrt(std::clamp(c[k], 0.f, 1.f)) * kSize + 0.5f)];
    }
};

RecordingPlayer::RecordingPlayer(QObject *parent) : QObject(parent) {
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(10);
    connect(&m_timer, &QTimer::timeout, this, &RecordingPlayer::tick);
    m_back.setTimerType(Qt::PreciseTimer);
    m_back.setSingleShot(true);
    connect(&m_back, &QTimer::timeout, this, &RecordingPlayer::backStep);
}

RecordingPlayer::~RecordingPlayer() {
    close();
    if (m_sws) sws_freeContext(m_sws);
}

QString RecordingPlayer::fileName() const { return QFileInfo(m_path).fileName(); }
QQuickItem *RecordingPlayer::view() const { return m_view; }

void RecordingPlayer::setView(QQuickItem *view) {
    auto *preview = qobject_cast<PreviewView *>(view);
    if (preview == m_view) return;
    m_view = preview;
    // A new view starts with what is on screen now.
    updateTarget();
    {
        std::lock_guard<std::mutex> lock(m_workMutex);
        m_redraw = m_view != nullptr;
    }
    m_workWake.notify_all();
    emit viewChanged();
}

// The size pictures are made at: the view's, in the screen's own pixels.
void RecordingPlayer::updateTarget() {
    if (!m_view) return;
    const qreal ratio = m_view->window() ? m_view->window()->effectiveDevicePixelRatio() : 1.0;
    m_targetWidth.store(int(m_view->width() * ratio));
    m_targetHeight.store(int(m_view->height() * ratio));
}

void RecordingPlayer::setTruePeak(bool on) {
    if (on == m_truePeak) return;
    m_truePeak = on;
    std::lock_guard<std::mutex> lock(m_peakMutex);
    m_peak.setTruePeak(on);
}

void RecordingPlayer::setProblem(const QString &problem) {
    if (problem == m_problem) return;
    m_problem = problem;
    emit problemChanged();
}

void RecordingPlayer::open(const QString &path) {
    close();
    if (path.isEmpty()) return;
    m_path = QFileInfo(path).absoluteFilePath();
    Facts facts;
    m_info = probe(m_path, &facts);
    if (m_info.isEmpty()) {
        setProblem(tr("%1 cannot be read.").arg(fileName()));
        emit fileChanged();
        return;
    }
    m_channels = AudioFormat::validChannels(facts.channels);
    {
        std::lock_guard<std::mutex> lock(m_peakMutex);
        m_peak = PeakMeter(m_channels);
        m_peak.setTruePeak(m_truePeak);
    }
    m_meter.clear();
    m_meterQuiet = true;
    m_stream = std::make_unique<MediaStream>();
    MediaStream::Options options;
    options.input = m_path.toStdString();
    options.audioFormat = {48000, m_channels};
    if (!m_stream->start(options)) {
        m_stream.reset();
        setProblem(tr("%1 would not open.").arg(fileName()));
        emit fileChanged();
        return;
    }
    // Waiting on its first picture.
    m_stream->setPaused(true);
    // From the file: the decoder opens it on its own thread, a moment later.
    m_width = facts.width;
    m_height = facts.height;
    m_fps = facts.fps > 0 ? facts.fps : 30;
    m_duration = m_stated = facts.duration;
    // No length in it (a recording that was never finished): read through for it.
    m_measured = facts.duration <= 0;
    m_position = 0;
    setProblem({});
    m_awaiting = m_width > 0;
    m_deferred = false;
    m_awaitClock.start();
    m_soundFrames.store(0);
    m_picturesShown = 0;
    startSound();
    startThreads();
    wake();
    emit fileChanged();
    emit durationChanged();
    emit positionChanged();
    emit meterChanged();
}

void RecordingPlayer::close() {
    setProblem({});
    m_timer.stop();
    m_back.stop();
    // The threads first: they use the decoder and the sound output.
    stopThreads();
    if (m_sound) m_sound->stop();
    if (m_stream) m_stream->stop();
    m_stream.reset();
    ++m_opening;
    const bool had = !m_path.isEmpty();
    m_path.clear();
    m_info.clear();
    m_width = m_height = 0;
    m_duration = m_position = m_stated = 0;
    m_measured = false;
    m_awaiting = m_deferred = false;
    m_picture = Picture{};
    setShuttle(0);
    if (had) {
        emit fileChanged();
        emit durationChanged();
    }
}

void RecordingPlayer::startThreads() {
    {
        std::lock_guard<std::mutex> lock(m_workMutex);
        m_workQuit = m_workRun = false;
    }
    m_measureQuit.store(false);
    m_soundThread = std::thread([this] { pumpSound(); });
    m_pictureThread = std::thread([this] { pumpPictures(); });
    if (m_measured) m_measureThread = std::thread([this, file = QFile::encodeName(m_path), opening = m_opening] { measure(file, opening); });
}

void RecordingPlayer::stopThreads() {
    {
        std::lock_guard<std::mutex> lock(m_workMutex);
        m_workQuit = true;
    }
    m_measureQuit.store(true);
    m_workWake.notify_all();
    if (m_soundThread.joinable()) m_soundThread.join();
    if (m_pictureThread.joinable()) m_pictureThread.join();
    if (m_measureThread.joinable()) m_measureThread.join();
    // A picture still on its way to the view was the old file's.
    std::lock_guard<std::mutex> lock(m_workMutex);
    m_workRun = false;
    m_ready = QImage();
    m_readyFresh = false;
}

// The timer and the threads run from here until tick() finds nothing moving.
void RecordingPlayer::wake() {
    if (!m_stream) return;
    if (!m_timer.isActive()) {
        m_meterClock.start();
        m_timer.start();
    }
    {
        std::lock_guard<std::mutex> lock(m_workMutex);
        m_workRun = true;
    }
    m_workWake.notify_all();
}

void RecordingPlayer::startSound() {
    if (!m_stream) return;
    QString why;
    const std::string target = soundTarget ? soundTarget(&why) : std::string();
    if (target == m_soundTarget && m_sound && m_sound->running() && !target.empty()) return;
    if (!m_sound) {
        m_sound = std::make_unique<AudioPlayback>();
        m_sound->setName("omabroadcast-playback", "OmaBroadcast playback");
    }
    m_sound->stop();
    m_soundTarget = target;
    QString note = why;
    if (!target.empty()) {
        std::string error;
        m_sound->setFormat({48000, m_channels});
        m_sound->setLatencyFrames(kSoundLatencyFrames);
        if (!m_sound->start(target, &error)) note = tr("No sound: %1").arg(QString::fromStdString(error));
    }
    if (note != m_soundNote) {
        m_soundNote = note;
        emit soundChanged();
    }
}

void RecordingPlayer::refreshSound() { startSound(); }

int RecordingPlayer::soundToDrop(uint64_t queued, int frames, int quantum, bool *catching) {
    if (frames <= 0) return 0;
    // What the output takes at once is what has to wait there; two more of
    // those over what was asked for, and the sound is late enough to hear.
    // Never above 4096, so it is caught before the output's own queue is full.
    const uint64_t step = uint64_t(std::max(quantum, kSoundLatencyFrames));
    const uint64_t high = std::max(step, std::min<uint64_t>(kSoundLatencyFrames + 2 * step, 4096));
    if (queued > high) *catching = true;
    else if (queued <= step) *catching = false;
    return *catching ? int(std::min<uint64_t>(uint64_t(frames), queued - step)) : 0;
}

// The sound, from the decoder to the meters and the output as it falls due.
void RecordingPlayer::pumpSound() {
    MediaStream *const stream = m_stream.get();
    AudioPlayback *const sound = m_sound.get();
    const int channels = m_channels;
    bool catching = false;
    std::unique_lock<std::mutex> lock(m_workMutex);
    while (!m_workQuit) {
        if (!m_workRun) {
            m_workWake.wait(lock);
            continue;
        }
        lock.unlock();
        const bool sounding = sound && sound->running();
        if (!sounding) catching = false;
        stream->pumpAudio(monotonicNs(), [&](int64_t due, const float *samples, int frames) {
            {
                std::lock_guard<std::mutex> peak(m_peakMutex);
                for (int i = 0; i < frames; ++i) m_peak.process(samples + size_t(i) * channels, due);
            }
            m_soundFrames.fetch_add(uint64_t(frames));
            if (!sounding) return;
            // Held up for a moment (a busy machine), the output played silence and
            // everything after would play that much late: the oldest goes instead.
            const int skip = soundToDrop(sound->queuedFrames(), frames, sound->quantumFrames(), &catching);
            if (skip < frames) sound->push(samples + size_t(skip) * channels, frames - skip);
        });
        lock.lock();
        if (!m_workQuit && m_workRun) m_workWake.wait_for(lock, kPumpEvery);
    }
}

// The pictures, from the decoder to the view's size and colour, handed to
// showPicture() on the window's thread: the newest only, when it falls behind.
void RecordingPlayer::pumpPictures() {
    MediaStream *const stream = m_stream.get();
    std::unique_lock<std::mutex> lock(m_workMutex);
    while (!m_workQuit) {
        if (!m_workRun && !m_redraw) {
            m_workWake.wait(lock);
            continue;
        }
        const bool run = m_workRun, redraw = m_redraw;
        m_redraw = false;
        lock.unlock();
        // Read before the picture is taken: one from before a seek never counts as that seek's.
        const uint64_t seek = m_seekSerial.load();
        const bool fresh = run && stream->acquire(m_picture, monotonicNs());
        QImage image;
        if (fresh || (redraw && m_picture.width > 0)) image = toImage(m_picture, m_targetWidth.load(), m_targetHeight.load());
        lock.lock();
        if (fresh || !image.isNull()) {
            if (!image.isNull()) m_ready = std::move(image);
            if (fresh) {
                m_readyFresh = true;
                m_readySeek = seek;
            }
            if (!m_posted) {
                m_posted = true;
                QMetaObject::invokeMethod(this, &RecordingPlayer::showPicture, Qt::QueuedConnection);
            }
        }
        if (!m_workQuit && m_workRun) m_workWake.wait_for(lock, kPumpEvery);
    }
}

void RecordingPlayer::showPicture() {
    QImage image;
    bool fresh = false;
    uint64_t seek = 0;
    {
        std::lock_guard<std::mutex> lock(m_workMutex);
        m_posted = false;
        image = m_ready;
        m_ready = QImage();
        fresh = m_readyFresh;
        m_readyFresh = false;
        seek = m_readySeek;
    }
    if (!m_stream) return;
    if (m_view && !image.isNull()) m_view->setImage(image);
    if (!fresh) return;
    ++m_picturesShown;
    if (m_awaiting && seek == m_seekSerial.load()) seekDone();
}

// Reads the file through for where it ends, saying how far it has got as it goes.
void RecordingPlayer::measure(QByteArray file, uint64_t opening) {
    AVFormatContext *ctx = avformat_alloc_context();
    if (!ctx) return;
    ctx->interrupt_callback.callback = [](void *quit) -> int { return static_cast<std::atomic<bool> *>(quit)->load() ? 1 : 0; };
    ctx->interrupt_callback.opaque = &m_measureQuit;
    if (avformat_open_input(&ctx, file.constData(), nullptr, nullptr) < 0) return;  // which frees it
    if (avformat_find_stream_info(ctx, nullptr) >= 0) {
        // The streams the decoder plays, from where it counts.
        const int picture = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        const int sound = av_find_best_stream(ctx, AVMEDIA_TYPE_AUDIO, -1, picture, nullptr, 0);
        const double origin = ctx->start_time == AV_NOPTS_VALUE ? 0 : double(ctx->start_time) / AV_TIME_BASE;
        double end = 0;
        const auto tell = [&] {
            QMetaObject::invokeMethod(this, [this, end, opening] { measured(end, opening); }, Qt::QueuedConnection);
        };
        auto told = std::chrono::steady_clock::now();
        AVPacket *packet = av_packet_alloc();
        while (packet && !m_measureQuit.load() && av_read_frame(ctx, packet) >= 0) {
            if (packet->stream_index == picture || packet->stream_index == sound) {
                const int64_t stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
                if (stamp != AV_NOPTS_VALUE)
                    end = std::max(end, double(stamp + std::max<int64_t>(packet->duration, 0)) *
                                            av_q2d(ctx->streams[packet->stream_index]->time_base) - origin);
            }
            av_packet_unref(packet);
            const auto now = std::chrono::steady_clock::now();
            if (now - told >= std::chrono::milliseconds(250)) {
                told = now;
                tell();
            }
        }
        av_packet_free(&packet);
        if (!m_measureQuit.load()) tell();
    }
    avformat_close_input(&ctx);
}

void RecordingPlayer::measured(double seconds, uint64_t opening) {
    if (opening != m_opening || !m_stream || !m_measured || seconds <= m_duration) return;
    m_duration = seconds;
    emit durationChanged();
}

void RecordingPlayer::setShuttle(int shuttle) {
    if (shuttle == m_shuttle) return;
    m_shuttle = shuttle;
    emit shuttleChanged();
}

void RecordingPlayer::togglePlay() {
    if (m_shuttle != 0) stop();
    else forward();
}

void RecordingPlayer::stop() {
    m_back.stop();
    if (m_stream) m_stream->setPaused(true);
    setShuttle(0);
}

void RecordingPlayer::forward() {
    if (!m_stream) return;
    m_back.stop();
    const int next = m_shuttle <= 0 ? 1 : std::min(4, m_shuttle * 2);
    m_stream->setSpeed(next);
    if (m_shuttle <= 0) {
        // From the end, again from the start.
        if (m_duration > 0 && m_position >= m_duration - 1.5 / m_fps) seek(0);
        // It plays from where it was last sent, without waiting for that picture.
        if (m_deferred) sendSeek(m_position);
        m_stream->setPaused(false);
    }
    setShuttle(next);
    wake();
}

void RecordingPlayer::backward() {
    if (!m_stream) return;
    if (m_shuttle >= 0) {
        m_stream->setPaused(true);
        setShuttle(-1);
        m_backClock.start();
        m_back.start(kBackStepMs);
        wake();
    } else {
        setShuttle(std::max(-4, m_shuttle * 2));
    }
}

// One step back: as far as the shuttle's rate goes in the time since the last
// step. The next is set off by this one's picture (see seekDone), so pictures
// that take long to find make longer steps, each of them seen.
void RecordingPlayer::backStep() {
    if (!m_stream || m_shuttle >= 0) return;
    const double seconds = double(std::max<qint64>(kBackStepMs, m_backClock.restart())) / 1000.0;
    const double to = std::max(0.0, m_position + m_shuttle * seconds);
    seek(to);
    if (to <= 0) stop();
}

void RecordingPlayer::stepFrames(int frames) {
    if (!m_stream) return;
    stop();
    seek(m_position + frames / m_fps);
}

void RecordingPlayer::seek(double seconds) {
    if (!m_stream) return;
    const double end = m_duration > 0 ? m_duration : seconds;
    const double to = std::clamp(seconds, 0.0, std::max(0.0, end));
    m_position = to;
    emit positionChanged();
    // The decoder drops what it was doing for a new seek. One whose picture has
    // not shown yet is left to show it, and this goes after: dragging the
    // slider or stepping back always shows pictures on the way.
    if (m_awaiting) m_deferred = true;
    else sendSeek(to);
    wake();
}

void RecordingPlayer::sendSeek(double to) {
    m_deferred = false;
    const bool sent = m_stream->seek(to);
    m_seekSerial.fetch_add(1);
    // A file with no picture has none to wait for.
    m_awaiting = sent && m_width > 0;
    m_awaitClock.start();
    if (!m_awaiting) seekDone();
}

// The last seek's picture has shown (or none is coming): on to what waited for it.
void RecordingPlayer::seekDone() {
    m_awaiting = false;
    if (m_deferred) sendSeek(m_position);
    else if (m_shuttle < 0) m_back.start(int(std::max<qint64>(0, kBackStepMs - m_backClock.elapsed())));
}

void RecordingPlayer::toEnd() {
    stop();
    seek(std::max(0.0, m_duration - 1.0 / m_fps));
}

void RecordingPlayer::resetPeak() {
    {
        std::lock_guard<std::mutex> lock(m_peakMutex);
        m_peak.resetMaximum();
    }
    m_meter.insert(QStringLiteral("maximumDb"), -120);
    m_meter.insert(QStringLiteral("clipped"), false);
    emit meterChanged();
}

double RecordingPlayer::pollPosition() {
    const MediaStream::Playback playback = m_stream->playback();
    // A file that runs past the length it states: the decoder's grows with it.
    if (playback.duration > 0 && std::abs(playback.duration - m_duration) > 0.0005) {
        m_duration = playback.duration;
        emit durationChanged();
    }
    return playback.position;
}

void RecordingPlayer::tick() {
    if (!m_stream) return;
    const int64_t now = monotonicNs();
    updateTarget();
    // Nothing more is coming from the decoder until it is asked again.
    const bool settled = m_stream->settled();
    if (m_awaiting && (settled || m_awaitClock.elapsed() > kSeekWaitMs)) seekDone();
    // Playing: the decoder's clock; held still, or sent somewhere it has not gone yet: where it was sent.
    if (m_shuttle > 0 && !m_deferred) {
        const double at = pollPosition();
        if (std::abs(at - m_position) > 0.0005) {
            m_position = at;
            emit positionChanged();
        }
        // No length stated: as long as it has played, at the least.
        if (m_measured && m_position > m_duration) {
            m_duration = m_position;
            emit durationChanged();
        }
        const std::string state = m_stream->state();
        // The end: the decoder has played everything out, or the clock is at the
        // length the file states. Not that length once the file has run past it
        // (one still being written): then only the decoder knows.
        const bool stated = m_stated > 0 && std::abs(m_duration - m_stated) < 0.001;
        if ((state == "ended" && settled) || (stated && m_position >= m_duration - 0.0005)) stop();
        else if (state.rfind("failed", 0) == 0) { setProblem(QString::fromStdString(state)); stop(); }
    }
    if (m_meterClock.elapsed() >= 50) {
        const double seconds = m_meterClock.restart() / 1000.0;
        PeakMeter::Reading reading;
        {
            std::lock_guard<std::mutex> lock(m_peakMutex);
            reading = m_peak.reading(now);
        }
        const QVariantList oldDb = m_meter.value(QStringLiteral("db")).toList();
        QVariantList db, levels;
        bool quiet = true;
        for (size_t c = 0; c < reading.levels.size(); ++c) {
            const double level = reading.levels[c] > 0 ? std::max(-60.0, 20.0 * std::log10(reading.levels[c])) : -60;
            const double value = PeakMeter::decay(c < size_t(oldDb.size()) ? oldDb[int(c)].toDouble() : -60, level, seconds, 12);
            db.append(value);
            levels.append(std::clamp((value + 60) / 60, 0.0, 1.0));
            quiet = quiet && value <= -60;
        }
        m_meterQuiet = quiet;
        const QVariantMap meter{{"db", db}, {"levels", levels},
                                {"maximumDb", reading.maximum > 0 ? 20 * std::log10(reading.maximum) : -120},
                                {"clipped", reading.maximum >= 1.f}};
        if (meter != m_meter) {
            m_meter = meter;
            emit meterChanged();
        }
    }
    // Nothing plays, no picture is on its way and the meters have fallen: all of it rests.
    if (m_shuttle == 0 && !m_awaiting && !m_deferred && settled && m_meterQuiet) {
        m_timer.stop();
        std::lock_guard<std::mutex> lock(m_workMutex);
        m_workRun = false;
    }
}

QImage RecordingPlayer::toImage(const Picture &p, int maxWidth, int maxHeight) {
    if (p.width <= 0 || p.height <= 0) return {};
    AVPixelFormat format = AV_PIX_FMT_NONE;
    if (p.rgba) {
        format = AV_PIX_FMT_RGBA;
    } else if (p.layout == engine::YuvLayout::YUY2) {
        format = AV_PIX_FMT_YUYV422;
    } else if (p.layout == engine::YuvLayout::NV12) {
        format = p.depth <= 8 ? AV_PIX_FMT_NV12 : p.depth >= 16 || !p.msb ? AV_PIX_FMT_P016LE : p.depth > 10 ? AV_PIX_FMT_P012LE : AV_PIX_FMT_P010LE;
    } else {
        const char *grid = p.layout == engine::YuvLayout::I444 ? "444" : p.layout == engine::YuvLayout::I422 ? "422" : "420";
        const QByteArray name = p.depth <= 8 ? QByteArray("yuv") + grid + "p" : QByteArray("yuv") + grid + "p" + QByteArray::number(p.depth) + "le";
        format = av_get_pix_fmt(name.constData());
    }
    if (format == AV_PIX_FMT_NONE) return {};
    // The picture's own shape, inside the view, never larger than it is.
    QSize size(p.width, p.height);
    if (maxWidth > 0 && maxHeight > 0) size = size.scaled(std::min(maxWidth, p.width), std::min(maxHeight, p.height), Qt::KeepAspectRatio);
    size = size.expandedTo(QSize(2, 2));
    // HDR light and BT.2020 colour are not the screen's: those go by way of
    // 16-bit samples and the tables, so the picture is not shown grey and flat.
    const bool deep = p.transfer != engine::Transfer::Sdr || p.wideGamut;
    std::lock_guard<std::mutex> lock(m_convertMutex);
    m_sws = sws_getCachedContext(m_sws, p.width, p.height, format, size.width(), size.height(),
                                 deep ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws) return {};
    if (!p.rgba) {
        const int matrix = p.matrix == engine::YuvMatrix::BT709 ? SWS_CS_ITU709 : p.matrix == engine::YuvMatrix::BT2020 ? SWS_CS_BT2020 : SWS_CS_ITU601;
        const int *coefficients = sws_getCoefficients(matrix);
        sws_setColorspaceDetails(m_sws, coefficients, p.fullRange, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    }
    QImage image(size, QImage::Format_RGBA8888);
    if (image.isNull()) return {};
    const uint8_t *src[4] = {p.plane[0].data(), p.plane[1].empty() ? nullptr : p.plane[1].data(),
                             p.plane[2].empty() ? nullptr : p.plane[2].data(), nullptr};
    const int strides[4] = {p.stride[0], p.stride[1], p.stride[2], 0};
    if (!deep) {
        uint8_t *dst[4] = {image.bits(), nullptr, nullptr, nullptr};
        const int dstStrides[4] = {int(image.bytesPerLine()), 0, 0, 0};
        if (sws_scale(m_sws, src, strides, 0, p.height, dst, dstStrides) <= 0) return {};
        return image;
    }
    const size_t width = size_t(size.width());
    m_deep.resize(width * size_t(size.height()) * 4 + 16);
    uint8_t *dst[4] = {reinterpret_cast<uint8_t *>(m_deep.data()), nullptr, nullptr, nullptr};
    const int dstStrides[4] = {int(width * 8), 0, 0, 0};
    if (sws_scale(m_sws, src, strides, 0, p.height, dst, dstStrides) <= 0) return {};
    if (!m_tables || m_tables->transfer != p.transfer || m_tables->wide != p.wideGamut)
        m_tables = std::make_unique<ScreenTables>(p.transfer, p.wideGamut);
    for (int y = 0; y < size.height(); ++y) {
        const uint16_t *in = m_deep.data() + size_t(y) * width * 4;
        uint8_t *out = image.scanLine(y);
        for (size_t x = 0; x < width; ++x, in += 4, out += 4) {
            m_tables->convert(in, out);
            out[3] = uint8_t(in[3] >> 8);
        }
    }
    return image;
}

QVariantList RecordingPlayer::probe(const QString &path, Facts *facts) {
    AVFormatContext *ctx = nullptr;
    const QByteArray file = QFile::encodeName(path);
    if (avformat_open_input(&ctx, file.constData(), nullptr, nullptr) < 0) return {};
    QVariantList rows;
    auto row = [&rows](const QString &label, const QString &value) { rows.append(QVariantMap{{"label", label}, {"value", value}}); };
    if (avformat_find_stream_info(ctx, nullptr) >= 0) {
        const QFileInfo info(path);
        row(tr("File"), info.fileName());
        row(tr("Size"), QLocale().formattedDataSize(info.size()));
        row(tr("Format"), QString::fromUtf8(ctx->iformat->long_name ? ctx->iformat->long_name : ctx->iformat->name));
        if (ctx->duration > 0) row(tr("Length"), clock(ctx->duration / double(AV_TIME_BASE)));
        if (facts && ctx->duration > 0) facts->duration = ctx->duration / double(AV_TIME_BASE);
        const int seen = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (ctx->bit_rate > 0) row(tr("Bit rate"), rate(ctx->bit_rate));
        int sound = 0, playing = 0;
        // The one the player hears, chosen as the decoder chooses it.
        const int heard = av_find_best_stream(ctx, AVMEDIA_TYPE_AUDIO, -1, seen, nullptr, 0);
        for (unsigned i = 0; i < ctx->nb_streams; ++i) {
            const AVStream *st = ctx->streams[i];
            const AVCodecParameters *par = st->codecpar;
            const QString title = QString::fromUtf8(av_dict_get(st->metadata, "title", nullptr, 0) ? av_dict_get(st->metadata, "title", nullptr, 0)->value : "");
            QStringList parts{codecName(par->codec_id)};
            if (par->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                if (const char *profile = avcodec_profile_name(par->codec_id, par->profile)) parts << QString::fromUtf8(profile);
                parts << QStringLiteral("%1 × %2").arg(par->width).arg(par->height);
                const AVRational fps = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
                if (fps.num > 0 && fps.den > 0) parts << tr("%1 fps").arg(QString::number(av_q2d(fps), 'g', 5));
                if (facts && int(i) == seen) {
                    facts->width = par->width;
                    facts->height = par->height;
                    if (fps.num > 0 && fps.den > 0) facts->fps = av_q2d(fps);
                }
                if (const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(AVPixelFormat(par->format))) {
                    const char *grid = d->nb_components < 3 ? "4:0:0" : d->log2_chroma_w == 0 ? "4:4:4" : d->log2_chroma_h == 0 ? "4:2:2" : "4:2:0";
                    parts << tr("%1-bit %2").arg(d->comp[0].depth).arg(QString::fromLatin1(grid));
                }
                QStringList colour;
                if (par->color_space != AVCOL_SPC_UNSPECIFIED) colour << QString::fromUtf8(av_color_space_name(par->color_space));
                if (par->color_range != AVCOL_RANGE_UNSPECIFIED) colour << (par->color_range == AVCOL_RANGE_JPEG ? tr("full range") : tr("limited range"));
                if (par->color_trc == AVCOL_TRC_SMPTE2084) colour << QStringLiteral("PQ");
                else if (par->color_trc == AVCOL_TRC_ARIB_STD_B67) colour << QStringLiteral("HLG");
                if (!colour.isEmpty()) parts << colour.join(QLatin1Char(' '));
                if (const QString r = rate(par->bit_rate); !r.isEmpty()) parts << r;
                row(title.isEmpty() ? tr("Picture") : tr("Picture: %1").arg(title), parts.join(QStringLiteral(" · ")));
            } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
                ++sound;
                if (int(i) == heard) {
                    playing = sound;
                    if (facts) facts->channels = par->ch_layout.nb_channels;
                }
                parts << tr("%1 kHz").arg(QString::number(par->sample_rate / 1000.0, 'g', 4));
                char layout[64] = {};
                if (av_channel_layout_describe(&par->ch_layout, layout, sizeof layout) > 0) parts << QString::fromUtf8(layout);
                if (const QString r = rate(par->bit_rate); !r.isEmpty()) parts << r;
                const QString label = tr("Sound %1").arg(sound);
                row(title.isEmpty() ? label : tr("%1: %2").arg(label, title), parts.join(QStringLiteral(" · ")));
            }
        }
        if (sound == 0) row(tr("Sound"), tr("none"));
        if (sound > 1 && playing > 0) row(tr("Heard"), tr("Sound %1; the others are in the file for editing").arg(playing));
        if (ctx->nb_chapters > 0) row(tr("Chapters"), QString::number(ctx->nb_chapters));
    }
    avformat_close_input(&ctx);
    return rows;
}
