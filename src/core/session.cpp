#include "session.h"
#include "core/transitions.h"
#include "video_filter_settings.h"
#include "audio_settings.h"

#include "output/encoder_helper.h"
#include "core/machine_survey.h"

#include <QDBusArgument>
#include <QDateTime>
#include <QCoreApplication>
#include <QDir>
#include <QFontMetrics>
#include <QPainter>
#include <QProcess>
#include <QScopeGuard>
#include <QPointer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QLocale>
#include <QStorageInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <time.h>
#include <unistd.h>
#include <drm_fourcc.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <new>
#include <utility>

namespace {
// A chain of one step that draws nothing, for a source that must not be seen.
engine::VideoFilter drawsNothing() {
    engine::VideoFilter nothing;
    nothing.opacity = 0.f;
    nothing.multiply = {0.f, 0.f, 0.f, 1.f};
    return nothing;
}
QString cutOutFailureText(engine::FilterFailure why) {
    const char *reason = why == engine::FilterFailure::Model
        ? QT_TRANSLATE_NOOP("VideoFilters", "Remove background (AI): this source is hidden, as the filter's model is missing from this install.")
        : why == engine::FilterFailure::Device
        ? QT_TRANSLATE_NOOP("VideoFilters", "Remove background (AI): this source is hidden, as this graphics card cannot run the filter.")
        : QT_TRANSLATE_NOOP("VideoFilters", "Remove background (AI): this source is hidden, as the graphics card has no memory left for the filter.");
    return QCoreApplication::translate("VideoFilters", reason);
}
int64_t monotonicNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000ll + ts.tv_nsec;
}
}  // namespace

namespace {
// Disk space: a recording needs this much to start; one running stops while
// a little is left (so the file can be closed properly) and warns earlier.
constexpr qint64 kMinFreeToStart = 512ll * 1024 * 1024;
constexpr qint64 kStopFloor = 256ll * 1024 * 1024;
constexpr qint64 kWarnFloor = 2048ll * 1024 * 1024;
constexpr double kStopSeconds = 15, kWarnSeconds = 300;
constexpr double kMissingWorthSaying = 0.01;  // of a recording's pictures

QString sizeText(qint64 bytes) { return QLocale().formattedDataSize(bytes, 0); }
}  // namespace

Session::Session(QObject *parent, std::string waylandSocket) : QObject(parent), m_directCaptures(std::move(waylandSocket)) {
    connect(this, &Session::recordingChanged, this, &Session::resumeFit);
    connect(this, &Session::streamingChanged, this, &Session::resumeFit);
    connect(this, &Session::replayChanged, this, &Session::resumeFit);
    // A snapshot's picture went out: it is no longer planned.
    connect(this, &Session::snapshotReady, this, [this](const QString &id) {
        const auto before = m_snapshotViews.size();
        std::erase_if(m_snapshotViews, [&](const View &v) { return v.id == id; });
        if (m_snapshotViews.size() != before) rebuildPlan();
    }, Qt::QueuedConnection);
    // A picture on its way has arrived: the frame thread may send the next.
    // (A view's picture lets its own go: see drawViews.)
    connect(this, &Session::previewReady, this, [this] { m_previewOnItsWay.store(false); }, Qt::QueuedConnection);
    connect(this, &Session::programReady, this, [this] { m_programOnItsWay.store(false); }, Qt::QueuedConnection);
    m_transitionEnd.setSingleShot(true);
    connect(&m_transitionEnd, &QTimer::timeout, this, &Session::endTransition);
    m_itemTransitionEnd.setSingleShot(true);
    connect(&m_itemTransitionEnd, &QTimer::timeout, this, &Session::rebuildPlan);
    m_diskGuard.setInterval(1000);
    connect(&m_diskGuard, &QTimer::timeout, this, &Session::checkDiskSpace);
    m_directStatus.setInterval(100);
    connect(&m_directStatus, &QTimer::timeout, this, [this] {
        if (!m_running.load()) return;
        std::vector<std::shared_ptr<Feed>> feeds;
        { std::lock_guard lock(m_sourcesMutex); for (const auto &[id, feed] : m_feeds) if (feed->capture() || feed->isDevice() || feed->isMedia()) feeds.push_back(feed); }
        bool changed=false;
        for (const auto &feed : feeds) if (feed->isMedia()) {
            // Its worker opens the file or waits for the phone, and nothing tells
            // this thread when that is done: the list is shown what it has come to.
            if (!feed->live.load()) continue;
            const QString shown = QStringLiteral("%1 %2x%3 %4 %5 %6").arg(QString::fromStdString(feed->media.state())).arg(feed->media.width()).arg(feed->media.height())
                                      .arg(QString::fromStdString(feed->media.codec())).arg(feed->media.hardware()).arg(QString::fromStdString(feed->media.refusedSound()));
            if (feed->mediaShown != shown) { feed->mediaShown = shown; changed=true; }
        } else if (auto direct=feed->direct.load()) {
            const auto state=direct->state();
            if (state==DirectCapture::State::Unsupported && feed->portalFallbackAllowed) { startCapture(feed, true); changed=true; continue; }
            const QString text=state==DirectCapture::State::Live ? tr("live (direct)") : QString::fromStdString(direct->error());
            if (feed->state!=text) { feed->state=text; changed=true; }
        } else if (feed->live.load()) {
            // Started is not live: a camera another app holds, a format that does
            // not negotiate, come to nothing. With an error the old picture goes
            // too, so it cannot pass for a camera that has stopped moving.
            const std::string error = feed->isDevice() ? feed->device.error() : feed->stream.error();
            const uint64_t frames = feed->isDevice() ? feed->device.framesReceived() : feed->stream.framesReceived();
            const bool running = feed->isDevice() ? feed->device.running() : feed->stream.streaming();
            if (!error.empty()) feed->live.store(false);
            const QString text = !error.empty() ? tr("failed: %1").arg(QString::fromStdString(error))
                               : frames == 0 ? tr("waiting for a picture")
                               : running ? tr("live") : tr("not sending pictures");
            if (feed->state!=text) { feed->state=text; changed=true; }
        }
        if (changed) emit sourcesChanged();
    });
    m_directStatus.start();
    m_pipeWireWatch.setInterval(1000);
    connect(&m_pipeWireWatch, &QTimer::timeout, this, &Session::watchPipeWire);
    m_pipeWireWatch.start();
    // A studio shown in another is drawn at the canvas size.
    connect(this, &Session::canvasChanged, this, &Session::rebuildPlan);
    // Text from a file follows the file (a "now playing" line, a chat log).
    m_textFiles.setInterval(1000);
    connect(&m_textFiles, &QTimer::timeout, this, [this] {
        if (!m_production || !m_running.load()) return;
        // Each check has a thread of its own (see drawFeed), and one that has not
        // come back is not asked again. A file as it was is not read (readDrawn).
        for (const auto &[id, feed] : m_feeds) {
            const SourceDef *def = m_production->collection().source(id);
            if (!def || def->kind != QLatin1String("text") || feed->drawing) continue;
            if (def->settings.value(QStringLiteral("file")).toString().isEmpty()) continue;
            SourceDef drawn = *def;
            drawn.settings.remove(QStringLiteral("linearAlpha"));
            drawFeed(feed, drawn, true);
        }
        for (const auto &[key, copy] : m_copyFeeds) {
            if (copy.def.kind != QLatin1String("text") || copy.feed->drawing) continue;
            if (copy.def.settings.value(QStringLiteral("file")).toString().isEmpty()) continue;
            SourceDef drawn = copy.def;
            drawn.settings.remove(QStringLiteral("linearAlpha"));
            drawFeed(copy.feed, drawn, true);
        }
    });
    m_textFiles.start();
    m_mixer.setSourceCount(AudioCapture::Count);
    m_encodings.onFrameDone = [this](engine::YuvFrame &&frame) {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        if (m_framePool.size() < 8) m_framePool.push_back(std::move(frame));
    };
    m_encodings.onError = [this](const std::weak_ptr<EncodedStream> &weak, const std::string &message) {
        QMetaObject::invokeMethod(this, [this, weak, message] {
            // Gone already: its outputs have finished and said so.
            const std::shared_ptr<EncodedStream> stream = weak.lock();
            if (!stream) return;
            const QString why = QString::fromStdString(message);
            for (auto &o : m_streams) if (o->stream == stream) failStream(*o, why);
            if (m_replay && m_replay->stream == stream && !m_replay->stopping) {
                emit replayFailed(why);
                stopReplay(false);
            }
            if (!m_recording || m_recording->stream != stream || m_recording->stopping) return;
            emit recordingFailed(why);
            stopRecording(false);
        }, Qt::QueuedConnection);
    };
    wireRemux();
}

Session::~Session() {
    stop();
    if (m_probeThread.joinable()) m_probeThread.join();
    // A second for all of them: what is left after that is held by the system.
    const auto started = std::chrono::steady_clock::now();
    FileWork::settle(1000);
    MediaStream::settle(1000 - int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()));
}

LeaveAroundHeldWork::~LeaveAroundHeldWork() {
    if (FileWork::settle(0) + MediaStream::settle(0) == 0) return;
    std::fflush(nullptr);
    _exit(code);
}

QProcess *Session::runHelper(const QStringList &arguments, const QByteArray &input,
                             std::function<void(const QByteArray &)> done, int timeoutMs,
                             std::function<void(const QByteArray &)> line) {
    const QString program = encoderhelper::program();
    m_helperNeverStarted = false;
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->setStandardErrorFile(QProcess::nullDevice());
    // Everything it wrote, kept as it comes when its lines are wanted as they come.
    auto said = std::make_shared<QByteArray>();
    if (line)
        connect(process, &QProcess::readyReadStandardOutput, this, [process, said, line] {
            const qsizetype from = said->size();
            said->append(process->readAllStandardOutput());
            qsizetype start = from > 0 ? said->lastIndexOf('\n', from - 1) + 1 : 0, end;
            while ((end = said->indexOf('\n', std::max(start, from))) >= 0) {
                line(said->mid(start, end - start));
                start = end + 1;
            }
        });
    connect(process, &QProcess::finished, this, [process, done, said] {
        said->append(process->readAllStandardOutput());
        done(*said);
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, done](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;  // the others end in finished()
        m_helperNeverStarted = true;
        done({});
        process->deleteLater();
    });
    process->start(program, arguments);
    if (!input.isEmpty()) process->write(input);
    process->closeWriteChannel();
    // Killed, it ends in finished() with what it said so far.
    QTimer::singleShot(timeoutMs, process, [process] { process->kill(); });
    return process;
}

QString Session::stillChecking() const { return tr("Still checking which encoders work; try again in a moment."); }
QString Session::probeNow() const { return m_probeNow; }
std::vector<EncoderInfo> Session::probeFound() const { return m_probeFound; }

void Session::probe(int attempt) {
    m_probeNow.clear();
    m_probeFound.clear();
    auto trying = std::make_shared<std::string>();
    // Each encoder as it is tried, and whether it opened, for the settings to show as it goes.
    auto progress = [this, trying](const QByteArray &line) {
        EncoderInfo e;
        bool done = false;
        if (!encoderhelper::readProgress(line, &e, &done)) return;
        *trying = done ? std::string() : encoderKey(e);
        if (!done) m_probeNow = QString::fromStdString(e.label);
        else if (e.available) m_probeFound.push_back(e);
        emit encodersChanged();
    };
    runHelper(encoderhelper::probeArguments(m_probeSkip), {}, [this, attempt, trying](const QByteArray &output) {
        std::vector<EncoderInfo> list;
        if (encoderhelper::readProbe(output, &list)) { encodersFound(list); return; }
        const bool started = !m_helperNeverStarted;
        if (started && attempt == 0) { probe(1); return; }
        // It crashed or hung twice, which is what the helper is there to take:
        // the same work in this process would take the program down with it, on
        // every launch. The encoder it was trying is left out and the rest
        // checked again; with nothing to blame, only the processor's are safe.
        const QString blamed = QString::fromStdString(*trying);
        if (started && !blamed.isEmpty() && !m_probeSkip.contains(blamed) && m_probeSkip.size() < kMaxProbeSkips) {
            qWarning("The encoder check stopped while trying %s, so it is left out.", qPrintable(blamed));
            m_probeSkip << blamed;
            probe(1);
            return;
        }
        if (started) qWarning("The encoder check keeps stopping, so only encoders on the processor are offered.");
        m_probeNow.clear();
        // Not on this thread: opening every encoder can stall for seconds.
        const uint64_t run = m_runSerial.load();
        if (m_probeThread.joinable()) m_probeThread.join();
        m_probeThread = std::thread([this, run, started] {
            std::vector<EncoderInfo> found = started ? probeEncoders({}, {}, [](const EncoderInfo &e) { return e.hardware; })
                                                     : probeEncoders(engine::listGpus());
            QMetaObject::invokeMethod(this, [this, run, found = std::move(found)] {
                if (run == m_runSerial.load()) encodersFound(found);
            }, Qt::QueuedConnection);
        });
    }, 60000, progress);
}

void Session::encodersFound(const std::vector<EncoderInfo> &list) {
    m_encoders = list;
    m_probeNow.clear();
    m_probeFound.clear();
    m_probing.store(false);
    emit encodersChanged();
    warmRecordingEncoder();
    refitEncoders();
}

void Session::setProfile(const Profile &p) {
    const auto previous = m_profile.audioFormat;
    m_profile = p;
    applyColourLevels();
    const auto wanted = p.audioFormat.normalized();
    m_profile.audioFormat = previous;
    setAudioFormat(wanted);
    applyAudioSettings();
}

bool Session::setAudioFormat(AudioFormat format) {
    // Open encoders take the sound in the format they opened with.
    if (outputsBusy()) return false;
    format = format.normalized();
    if (format == m_profile.audioFormat && format.rate == m_mixer.sampleRate() && format.channels == m_mixer.channels()) return true;
    // Quiesce every producer before changing frame strides or clock units.
    // Video capture and the live studio remain open.
    m_audioQuit.store(true);
    if (m_audioThread.joinable()) m_audioThread.join();
    m_audio.stop(); m_monitor.stop(); m_monitorSerial.clear();
    for (auto *feeds : {&m_feeds, &m_globalAudioFeeds}) for (auto &[id, feed] : *feeds) {
        for (auto &[serial, tap] : feed->audioCaptures) {
            tap.capture.reset();
            m_mixer.removeInput(feed->audioChannel, tap.input);
        }
        feed->audioCaptures.clear();
        if (feed->isMedia()) { feed->media.stop(); feed->mediaSettings.clear(); }
    }
    // Stingers too: syncStingers makes them again in the new format.
    for (const auto &[id, feed] : m_stingers) { feed->media.stop(); feed->mediaSettings.clear(); }
    m_mixer.stop();
    m_mixer.setFormat(format.rate, format.channels);
    m_profile.audioFormat = format;
    m_audio.setFormat(format); m_monitor.setFormat(format);
    m_mixer.setLatencyMs(m_lowLatencyAudio ? 4 : 80);
    if (running()) {
        m_mixer.start(monotonicNs());
        applyAudioSettings();
        connectDesktopAudio();
        syncFeeds();
        m_audioQuit.store(false);
        m_audioThread = std::thread(&Session::audioLoop, this);
    }
    emit sourcesChanged();
    return true;
}

void Session::applyAudioSettings() {
    m_mixer.setTruePeakMeter(m_profile.meter.value(QStringLiteral("truePeak"), false).toBool());
    // Not under an open encoder: its sound is timed by counting samples, and
    // the mixer's jump would leave them all early for the rest of the output.
    if (m_lowLatencyAudio != m_profile.lowLatencyAudio && !outputsBusy()) {
        m_lowLatencyAudio = m_profile.lowLatencyAudio;
        {
            std::lock_guard<std::mutex> lock(m_recordMutex);
            m_mixer.setLatencyMs(m_lowLatencyAudio ? 4 : 80);
        }
        const int quantum = m_lowLatencyAudio ? 128 : 480;
        m_audio.setLatencyFrames(quantum);
        for (auto *feeds : {&m_feeds, &m_globalAudioFeeds}) for (const auto &[id, feed] : *feeds)
            for (auto &[serial, tap] : feed->audioCaptures) tap.capture->setLatencyFrames(quantum);
        m_monitor.stop();
        m_monitorSerial.clear();
        m_monitor.setLatencyFrames(quantum);
    }
    m_mixer.setPushSettings(AudioCapture::Desktop, AudioSettings::push(m_profile.desktopAudio));
    m_mixer.setPushSettings(AudioCapture::Microphone, AudioSettings::push(m_profile.microphoneAudio));
    m_mixer.setRouting(AudioCapture::Desktop, AudioSettings::routing(m_profile.desktopAudio));
    m_mixer.setRouting(AudioCapture::Microphone, AudioSettings::routing(m_profile.microphoneAudio));
    m_mixer.setFilters(AudioCapture::Desktop, AudioFilterSettings::parse(m_profile.desktopAudio.value(QStringLiteral("audioFilters")).toList()));
    m_mixer.setFilters(AudioCapture::Microphone, AudioFilterSettings::parse(m_profile.microphoneAudio.value(QStringLiteral("audioFilters")).toList()));
    m_mixer.setProcessing(AudioCapture::Desktop, AudioSettings::processing(m_profile.desktopAudio));
    m_mixer.setProcessing(AudioCapture::Microphone, AudioSettings::processing(m_profile.microphoneAudio));
    m_mixer.setGain(AudioCapture::Desktop, m_profile.desktopGain);
    m_mixer.setGain(AudioCapture::Microphone, m_profile.microphoneGain);
    m_mixer.setMuted(AudioCapture::Desktop, m_profile.desktopMuted || !m_profile.desktopAdded);
    m_mixer.setMuted(AudioCapture::Microphone, m_profile.microphoneMuted || !m_profile.microphoneAdded);
    m_audio.setDesktop(m_profile.desktopAdded);
    syncGlobalAudioDevices();
    refreshMonitoring();
}

void Session::start() {
    if (m_running.load()) return;
    ++m_runSerial;
    // Qt cannot report the display's pixel size under fractional scaling (it
    // rounds 1.67 up to 2), so an automatic canvas starts at a placeholder and
    // takes the first display's real size as soon as its frames arrive.
    // Until the first capture settles it, an automatic canvas is a placeholder:
    // the size the open collection's layout is at, which then need not move
    // unless the display says otherwise.
    const bool automatic = m_profile.canvasWidth <= 0 || m_profile.canvasHeight <= 0;
    const Collection *open = m_production ? &m_production->collection() : nullptr;
    const bool sized = open && open->canvasWidth > 0 && open->canvasHeight > 0;
    m_canvasAuto.store(automatic);
    m_canvasWidth.store((automatic ? (sized ? open->canvasWidth : 1920) : m_profile.canvasWidth) & ~1);
    m_canvasHeight.store((automatic ? (sized ? open->canvasHeight : 1080) : m_profile.canvasHeight) & ~1);
    // The rate this run keeps to: a new one in the profile waits for the next.
    m_runFpsNum = std::max(1, m_profile.fpsNum);
    m_runFpsDen = std::max(1, m_profile.fpsDen);

    // Opening every encoder takes a couple of seconds, and in this process it
    // would stall the frame loop on driver locks; a helper process does it.
    // Recording waits for it, nothing else does.
    m_probing.store(true);
    probe(0);
    m_monitor.onStateChanged = [this] {
        QMetaObject::invokeMethod(this, [this] { emit monitoringChanged(); }, Qt::QueuedConnection);
    };
    m_mixer.start(monotonicNs());
    applyAudioSettings();
    // The microphone stays unconnected until the inputs are known. With no
    // PipeWire yet, watchPipeWire says so and keeps trying.
    m_pipeWireLost = false;
    m_pipeWireTries = m_pipeWireWait = 0;
    connectPipeWire();
    connectDesktopAudio();

    m_quit.store(false);
    m_audioQuit.store(false);
    m_running.store(true);
    {
        std::lock_guard<std::mutex> lock(m_engineFormatsMutex);
        m_engineMade = false;
        m_engineFormats.clear();
    }
    m_thread = std::thread(&Session::frameLoop, this);
    m_audioThread = std::thread(&Session::audioLoop, this);

    syncGlobalAudioDevices();
    refreshMonitoring();
    m_launching = true;
    syncFeeds();
    m_launching = false;
}

bool Session::connectPipeWire() {
    std::string err;
    const bool audio = m_devices.start([this] {
        QMetaObject::invokeMethod(this, &Session::refreshMicrophone, Qt::QueuedConnection);
    }, &err);
    const bool video = m_videoDevices.start([this] {
        QMetaObject::invokeMethod(this, &Session::refreshDevices, Qt::QueuedConnection);
    }, &err);
    return audio && video;
}

void Session::connectDesktopAudio() {
    std::string err;
    const bool ok = m_audio.start([this](int source, int64_t timeNs, const float *data, int frames) {
        m_mixer.push(source, timeNs, data, frames, 0, monotonicNs());
    }, &err, m_profile.desktopAdded);
    // With PipeWire itself gone, watchPipeWire says so instead, and comes back here.
    if (!ok && m_devices.connected())
        emit pipeWireChanged(tr("Desktop sound did not connect: %1").arg(QString::fromStdString(err)));
}

// PipeWire restarted or crashed, or was not there at the start. Each connection
// to it is dead for good, and nothing says so but the device lists, which were
// emptied as it went: that alone has taken down what follows them (the
// microphone, sources' sound, monitoring, devices), and their coming back brings
// those back. What is left is everything with a connection of its own.
void Session::watchPipeWire() {
    if (!m_running.load() || (m_devices.connected() && m_videoDevices.connected())) return;
    if (!m_pipeWireLost) {
        m_pipeWireLost = true;
        m_pipeWireTries = m_pipeWireWait = 0;
        emit pipeWireChanged(outputsBusy() ? tr("PipeWire is not running: no sound or capture devices, and what is going out carries on without them. Trying to reconnect…")
                                           : tr("PipeWire is not running: no sound or capture devices. Trying to reconnect…"));
    }
    if (m_pipeWireWait-- > 0) return;
    // A little longer each time, up to half a minute.
    if (!connectPipeWire()) {
        m_pipeWireWait = std::min(30, 1 << std::min(m_pipeWireTries++, 5));
        return;
    }
    m_pipeWireLost = false;
    emit pipeWireChanged(tr("PipeWire is back: sound and capture devices reconnected."));
    connectDesktopAudio();
    const std::map<QString, std::shared_ptr<Feed>> feeds = m_feeds;
    for (const auto &[id, feed] : feeds) {
        const SourceDef *def = m_production ? m_production->collection().source(id) : nullptr;
        // A page plays into a sink that has gone, through a connection that has
        // too, so it starts again with new ones.
        if (def && feed->isBrowser() && feed->browser && !feed->browser->soundSink().isEmpty()) {
            feed->browser.reset();
            syncFeedAudio(*feed, *def);
            syncBrowser(*feed, *def, false);
        }
        // A screen's stream ended with the connection. Asking again is quiet with a
        // saved choice; with none, a picker out of nowhere is worse than the
        // source saying it failed, which it does.
        if (feed->capture() && feed->captureSettings.value(QStringLiteral("captureMethod")) != QLatin1String("direct") && !m_safeMode &&
            opensQuietly(id))
            startCapture(feed);
    }
    if (m_camera.running()) {
        stopVirtualCamera();
        QString error;
        if (!startVirtualCamera(&error)) emit pipeWireChanged(error);
    }
    emit sourcesChanged();
}

void Session::stop() {
    if (!m_running.load()) { m_directCaptures.stop(); return; }
    stopRecording();
    stopReplay();
    stopVirtualCamera();
    stopStreaming(true);
    m_encodings.waitClosed();  // their sound still comes from the audio worker
    m_quit.store(true);
    if (m_thread.joinable()) m_thread.join();
    if (m_audioThread.joinable()) m_audioThread.join();
    m_audioFeeds.store(std::make_shared<const AudioFeeds>());
    m_running.store(false);
    m_directCaptures.stop();
    ++m_runSerial;
    for (QProcess *p : findChildren<QProcess *>()) {
        p->disconnect(this);
        p->kill();
        p->waitForFinished(1000);
        p->deleteLater();
    }
    if (m_probeThread.joinable()) m_probeThread.join();
    m_fitting.store(false);
    m_devices.stop();
    m_videoDevices.stop();
    m_audio.stop();
    m_monitor.stop();
    m_monitorSerial.clear();
    m_mixer.stop();
    m_captureQueue.clear();
    m_captureInFlight.clear();
    std::map<QString, std::shared_ptr<Feed>> feeds;
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        feeds.swap(m_feeds);
        m_capturePreviewRequest.clear();
        for (const auto &[id, feed] : m_globalAudioFeeds) {
            feed->audioCaptures.clear();
            m_mixer.removeSource(feed->audioChannel);
        }
        m_globalAudioFeeds.clear();
        for (const auto &[id, feed] : m_stingers) { feed->media.stop(); m_mixer.removeSource(feed->audioChannel); }
        m_stingers.clear();
        for (auto &[key, copy] : m_copyFeeds) { delete copy.feed->animator; copy.feed->animator = nullptr; ++copy.feed->drawSerial; }
        m_copyFeeds.clear();
        m_retiredFeeds.clear();
    }
    // Nothing at all, as a plan left here would draw old feeds in the next engine.
    m_stagedTransition.reset();
    publish(std::make_shared<const Published>());
    m_transitionEnd.stop();
    const bool wasTransitioning = m_transition.has_value();
    m_transition.reset();
    m_nextTransition.reset();
    m_onAir = {};
    m_studioProgram = {};
    for (const View &shot : std::exchange(m_snapshotViews, {})) emit snapshotReady(shot.id, {});
    m_manualProgress.store(-1.f);
    if (wasTransitioning) emit transitionChanged();
    for (auto &[id, f] : feeds) {
        if (auto direct=f->direct.exchange({})) direct->stop();
        f->stream.stop();
        f->device.stop();
        f->media.stop();
        f->browser.reset();
        f->audioCaptures.clear();
        m_mixer.removeSource(f->audioChannel);
        delete f->animator;
        f->animator = nullptr;
        ++f->drawSerial;
        if (f->portal) {
            f->portal->stop();
            f->portal->deleteLater();
        }
    }
}

bool Session::sourceAudioActive(const QString &sourceId) const {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    const auto it = m_feeds.find(sourceId);
    return it != m_feeds.end() && m_mixer.sourceActive(it->second->audioChannel);
}

QString Session::monitoringStatus() const {
    if (!m_monitorReason.isEmpty()) return m_monitorReason;
    const auto error = m_monitor.error();
    if (!error.empty()) return tr("Monitoring failed: %1").arg(QString::fromStdString(error));
    return m_monitor.running() ? tr("Monitoring on %1").arg(m_monitorLabel) : tr("Connecting to %1…").arg(m_monitorLabel);
}

void Session::refreshMonitoring() {
    // Do not send monitor playback into any sink whose mix is being captured.
    // A different output (e.g. headphones) or muting that capture resolves it.
    const auto nodes = m_devices.nodes();
    const std::string choice = m_profile.monitorOutput.isEmpty() ? m_devices.defaultOutput() : m_profile.monitorOutput.toStdString();
    const auto output = std::find_if(nodes.begin(), nodes.end(), [&](const AudioNode &n) {
        return n.kind == AudioNode::Output && n.name == choice;
    });
    auto wantsMonitor = [](const QVariantMap &settings) {
        const auto routing = AudioSettings::routing(settings);
        return routing.solo || routing.monitoring != AudioMixer::Monitoring::Off;
    };
    bool wanted = (m_profile.desktopAdded && !m_profile.desktopMuted && m_profile.desktopGain > 0 && wantsMonitor(m_profile.desktopAudio)) ||
                  (m_profile.microphoneAdded && !m_profile.microphoneMuted && m_profile.microphoneGain > 0 && wantsMonitor(m_profile.microphoneAudio));
    QString conflict;
    if (m_profile.desktopAdded && !m_profile.desktopMuted && m_profile.desktopGain > 0 &&
        (m_devices.defaultOutput().empty() || choice == m_devices.defaultOutput())) conflict = tr("Desktop audio");
    if (m_production) {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        for (const auto &[id, feed] : m_feeds) {
            const auto *source = m_production->collection().source(id);
            if (!source || feed->audioChannel < 0 || !m_mixer.sourceActive(feed->audioChannel)) continue;
            if (source->settings.value(QStringLiteral("muted"), false).toBool() ||
                AudioSettings::gain(source->settings.value(QStringLiteral("volume"), 1.0).toDouble()) == 0) continue;
            wanted |= wantsMonitor(source->settings);
            if (source->kind == QLatin1String("audio-output") && source->settings.value(QStringLiteral("audioNode")).toString().toStdString() == choice)
                conflict = source->name;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        for (const auto &source : m_profile.globalAudioDevices) {
            const auto it = m_globalAudioFeeds.find(source.id);
            if (it == m_globalAudioFeeds.end() || !m_mixer.sourceActive(it->second->audioChannel)) continue;
            if (source.settings.value("muted").toBool() || AudioSettings::gain(source.settings.value("volume", 1.).toDouble()) == 0) continue;
            wanted |= wantsMonitor(source.settings);
            if (source.kind == "audio-output" && source.settings.value("audioNode").toString().toStdString() == choice)
                conflict = source.name;
        }
    }
    m_monitorReason.clear();
    if (!m_running.load() || m_profile.monitorOutput == QLatin1String("none")) m_monitorReason = tr("Monitoring output is off");
    else if (!wanted) m_monitorReason = tr("No channels selected for monitoring");
    else if (output == nodes.end()) m_monitorReason = tr("Monitoring output is not connected");
    else if (!conflict.isEmpty()) m_monitorReason = tr("Monitoring paused: %1 captures this output. Mute it or choose another monitoring device.").arg(conflict);
    if (!m_monitorReason.isEmpty()) {
        if (!m_monitorSerial.empty()) m_monitor.stop();
        m_monitorSerial.clear();
    } else {
        m_monitorLabel = QString::fromStdString(output->description);
        if (m_monitorSerial != output->serial) {
            m_monitorSerial = output->serial;
            std::string error;
            if (!m_monitor.start(m_monitorSerial, &error)) m_monitorReason = QString::fromStdString(error);
        }
    }
    emit monitoringChanged();
}

// -------------------------------------------------------------- microphone

void Session::setMicrophoneChoice(const QString &choice) {
    m_profile.microphone = choice;
    refreshMicrophone();
}

void Session::refreshMicrophone() {
    const MicrophonePick pick = pickMicrophone(m_devices.inputs(), m_profile.microphoneAdded ? m_profile.microphone.toStdString() : "none",
                                               m_devices.defaultInput());
    if (pick.name != m_microphone.name) {
        m_audio.setMicrophone(pick.name);
        if (const std::string error = m_audio.error(); !error.empty() && !pick.name.empty() && m_devices.connected())
            emit pipeWireChanged(tr("The microphone did not connect: %1").arg(QString::fromStdString(error)));
    }
    m_microphone = pick;
    if (m_production)
        for (auto &[id, feed] : m_feeds)
            if (const SourceDef *def = m_production->collection().source(id); def && (feed->isDevice() || feed->isAudio()))
                syncFeedAudio(*feed, *def);
    syncGlobalAudioDevices();
    refreshMonitoring();
    emit microphonesChanged();
}

// ------------------------------------------------------------------ feeds

void Session::setProduction(Production *production) {
    if (m_production) m_production->disconnect(this);
    m_production = production;
    if (m_production) connect(m_production, &Production::changed, this, &Session::syncFeeds);
    if (m_running.load()) syncFeeds();
}

void Session::syncFeeds() {
    if (!m_production || !m_running.load()) return;
    const Collection &c = m_production->collection();
    std::vector<std::shared_ptr<Feed>> started, gone;
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        for (auto it = m_feeds.begin(); it != m_feeds.end();) {
            // Gone, or (another collection) the same id for another kind.
            const SourceDef *def = c.source(it->first);
            if (m_restartFeeds || !def || def->kind != it->second->kind) {
                gone.push_back(it->second);
                it = m_feeds.erase(it);
            } else {
                ++it;
            }
        }
        m_restartFeeds = false;
        for (const SourceDef &def : c.sources) {
            auto it = m_feeds.find(def.id);
            if (it == m_feeds.end()) {
                auto feed = std::make_shared<Feed>();
                feed->sourceId = def.id;
                feed->kind = def.kind;
                if (def.hasAudio()) {
                    feed->audioChannel = m_mixer.addSource(feed->isMedia());
                    m_mixer.setActive(feed->audioChannel, false);
                }
                // Hidden until its filters have been read, so that its first picture
                // cannot go out uncut.
                if (VideoFilterSettings::cutsOut(def.settings.value(QStringLiteral("videoFilters")).toList()))
                    feed->videoFilters = {drawsNothing()};
                m_feeds.emplace(def.id, feed);
                started.push_back(feed);
            }
        }
    }
    for (const auto &feed : gone) {
        std::erase(m_captureQueue, feed->sourceId);
        if (m_captureInFlight == feed->sourceId) m_captureInFlight.clear();
        feed->live.store(false);
        if (auto direct=feed->direct.exchange({})) direct->stop();
        feed->stream.stop();
        feed->device.stop();
        feed->media.stop();
        feed->browser.reset();
        feed->audioCaptures.clear();
        m_mixer.removeSource(feed->audioChannel);
        delete feed->animator;
        feed->animator = nullptr;
        ++feed->drawSerial;
        ++feed->filtersAsked;
        if (feed->portal) {
            feed->portal->stop();
            feed->portal->deleteLater();
            feed->portal = nullptr;
        }
        // Its engine ids are the frame thread's: it lets them go (see tick).
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        m_retiredFeeds.push_back(feed);
    }
    auto captureSettings = [](const QVariantMap &settings) {
        QVariantMap out;
        for (const auto &key : {"captureMethod", "cursor", "directKey", "directOutput", "directApp", "directTitle", "followFocus"})
            if (settings.contains(key)) out[key]=settings[key];
        return out;
    };
    {
        std::lock_guard lock(m_sourcesMutex);
        for (const auto &[id, feed] : m_feeds) if (feed->capture()) {
            const auto settings=captureSettings(c.source(id)->settings);
            if (feed->captureSettings!=settings && std::find(started.begin(),started.end(),feed)==started.end()) started.push_back(feed);
            feed->captureSettings=settings;
        }
    }
    for (const auto &feed : started) {
        if (feed->isDevice()) continue;  // below, with every device feed
        if (!feed->capture()) continue;
        // At launch, a capture with no saved choice waits to be asked for
        // rather than opening the picker unbidden; in safe mode none is restored.
        if (m_launching && m_safeMode)
            feed->state = tr("not restored in safe mode");
        else if (m_launching && feed->captureSettings.value("captureMethod")!="direct" && !opensQuietly(feed->sourceId))
            feed->state = tr("not chosen yet");
        else
            startCapture(feed);
    }
    // Capture/device settings apply independently of scene activity.
    // Without the lock: sound streams open and close here, and devices start
    // and stop, and the frame thread must not wait for those.
    for (auto &[id, feed] : m_feeds) {
        const SourceDef *def = c.source(id);
        if (!def) continue;
        syncFeedAudio(*feed, *def);
        if (feed->isAudio()) continue;
        syncVideoFilters(feed, *def);
        if (feed->isDevice()) {
            syncDevice(*feed, *def);
            continue;
        }
        if (feed->isMedia()) continue; // lifecycle follows the full visible plan below
        if (feed->capture()) {
            syncColour(*feed, *def);  // noise reduction applies to screen captures too
            continue;
        }
        if (feed->isNested()) {
            feed->live.store(true);
            feed->state = tr("live");
            continue;
        }
    }
    syncStingers();  // also gathers the feeds whose sound is pumped
    rebuildPlan();
    emit sourcesChanged();
}

void Session::planScene(const Scene &scene, std::vector<PlanItem> &items, std::vector<NestedRender> &nested,
                        QStringList &planned, int depth, const Shown *shown) {
    const Collection &c = m_production->collection();
    for (const SceneItem &item : scene.items) {
        // Being shown, or hidden but not gone yet.
        std::shared_ptr<ItemTransition> change;
        if (auto t = m_itemTransitions.find(item.id); t != m_itemTransitions.end() && t->second->showing == item.visible) change = t->second;
        if (!item.visible && !change) continue;
        if (shown && shown->sources && shown->sources->count(item.source)) {
            if (auto own = m_copyFeeds.find(copyKey(shown->serial, item.source)); own != m_copyFeeds.end())
                items.push_back({own->second.feed, item.transform, change});
            continue;
        }
        auto it = m_feeds.find(item.source);
        if (it == m_feeds.end()) continue;
        if (it->second->isNested()) {
            const SourceDef *def = c.source(item.source);
            const Scene *inner = def ? c.scene(c.shownStudio(*def)) : nullptr;
            // The model refuses loops; the depth limit is the backstop.
            if (!inner || depth >= 8) continue;
            const int w = inner->group ? inner->width : m_canvasWidth.load();
            const int h = inner->group ? inner->height : m_canvasHeight.load();
            it->second->nestedWidth.store(w);
            it->second->nestedHeight.store(h);
            if (!planned.contains(item.source)) {
                planned.append(item.source);
                NestedRender r{it->second, w, h, {}};
                planScene(*inner, r.items, nested, planned, depth + 1);
                nested.push_back(std::move(r));  // after what it shows
            }
        }
        items.push_back({it->second, item.transform, change});
    }
}

void Session::syncStingers() {
    std::map<QString, QString> wanted;
    if (m_production && m_running.load())
        for (const TransitionDef &t : m_production->collection().transitions) {
            const QVariantMap settings = Transitions::normalized(t.settings);
            if (!Transitions::isStinger(settings) || settings.value("file").toString().isEmpty()) continue;
            wanted[t.id] = settings.value("file").toString();
            if (settings.value("matte").toString() == QLatin1String("separate") && !settings.value("matteFile").toString().isEmpty())
                wanted[matteKey(t.id)] = settings.value("matteFile").toString();
        }
    std::vector<std::shared_ptr<Feed>> gone;
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        for (auto it = m_stingers.begin(); it != m_stingers.end();) {
            const auto want = wanted.find(it->first);
            if (want != wanted.end() && want->second == it->second->mediaSettings.value("input").toString()) { ++it; continue; }
            gone.push_back(it->second);
            it = m_stingers.erase(it);
        }
    }
    for (const auto &[id, file] : wanted) {
        if (m_stingers.count(id)) continue;
        auto feed = std::make_shared<Feed>();
        feed->sourceId = QStringLiteral("stinger:") + id;
        feed->kind = QStringLiteral("media");
        feed->audioChannel = m_mixer.addSource(true);
        m_mixer.setActive(feed->audioChannel, false);
        feed->mediaSettings = {{QStringLiteral("input"), file}};
        {
            std::lock_guard<std::mutex> transport(feed->mediaControlMutex);
            feed->media.start({file.toStdString(), false, 1.f, m_profile.audioFormat, 1.0, false});
            feed->media.setPaused(true);
        }
        feed->live.store(true);
        feed->state = tr("ready");
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        m_stingers.emplace(id, feed);
    }
    for (const auto &feed : gone) {
        feed->live.store(false);
        feed->media.stop();
        m_mixer.removeSource(feed->audioChannel);
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        m_retiredFeeds.push_back(feed);
    }
    auto audioFeeds = std::make_shared<AudioFeeds>();
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        for (const auto &[id, feed] : m_feeds) if (feed->isMedia()) audioFeeds->push_back(feed);
        for (const auto &[id, feed] : m_stingers) audioFeeds->push_back(feed);
    }
    m_audioFeeds.store(std::move(audioFeeds));
}

int Session::stingerMs(const QString &transition) const {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    const auto it = m_stingers.find(transition);
    // Known once the file is open; a paused file is still open.
    return it == m_stingers.end() ? 0 : int(it->second->media.playback().duration * 1000);
}

void Session::parkStinger(const std::shared_ptr<Feed> &feed) {
    if (!feed) return;
    {
        std::lock_guard<std::mutex> transport(feed->mediaControlMutex);
        feed->media.restart();
        feed->media.setPaused(true);
    }
    m_mixer.setActive(feed->audioChannel, false);
    m_mixer.clearSource(feed->audioChannel);
}

void Session::setNextTransition(const QString &transition, int durationMs) {
    m_nextTransition = std::make_pair(transition, durationMs);
}

namespace {
// Every source a studio shows, through the studios and groups it shows. One
// seen before is not walked again: a ladder of studios each showing the next
// twice would otherwise double the work at every step.
void showsSources(const Collection &c, const Scene &scene, std::set<QString> &out, int depth = 0) {
    if (depth > 32) return;
    for (const SceneItem &item : scene.items) {
        if (!item.visible) continue;
        if (!out.insert(item.source).second) continue;
        if (const SourceDef *def = c.source(item.source))
            if (const Scene *inner = c.scene(c.shownStudio(*def))) showsSources(c, *inner, out, depth + 1);
    }
}
}  // namespace

void Session::setViews(const std::vector<View> &views) {
    if (views == m_views) return;
    m_views = views;
    rebuildPlan();
}

void Session::requestSnapshot(const QString &id, const QString &shows, const QString &target) {
    if (!m_running.load() || !m_production) { emit snapshotReady(id, {}); return; }
    // Planned like a view, drawn once at full size.
    View shot{id, shows, target, 0, 0, 60};
    if (shows == QLatin1String("preview"))
        shot = m_studioMode ? View{id, QStringLiteral("studio"), m_production->collection().currentScene, 0, 0, 60}
                            : View{id, QStringLiteral("program"), {}, 0, 0, 60};
    // sourceInput: a source before its filters (where a key's backdrop is sampled).
    else if (shows != QLatin1String("studio") && shows != QLatin1String("source") && shows != QLatin1String("sourceInput")) shot.shows = QStringLiteral("program");
    const Collection &c = m_production->collection();
    const bool there = shot.shows == QLatin1String("program") || (shot.shows == QLatin1String("studio") && c.scene(shot.target) && !c.scene(shot.target)->group)
                       || ((shot.shows == QLatin1String("source") || shot.shows == QLatin1String("sourceInput")) && c.source(shot.target));
    if (!there) {
        QMetaObject::invokeMethod(this, [this, id] { emit snapshotReady(id, {}); }, Qt::QueuedConnection);
        return;
    }
    m_snapshotViews.push_back(shot);
    rebuildPlan();
}

void Session::setManualAir(float progress) {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    for (const auto &[id, feed] : m_feeds) {
        if (feed->audioChannel < 0) continue;
        if (m_airIn.count(id)) m_mixer.setAirGain(feed->audioChannel, progress, 30'000'000);
        else if (m_airOut.count(id)) m_mixer.setAirGain(feed->audioChannel, 1.f - progress, 30'000'000);
    }
}

const Scene *Session::Shown::in(const Collection &c) const {
    return copy ? copy.get() : c.scene(scene);
}

Session::Shown Session::shownFor(const QString &sceneId, bool copy) {
    const Scene *scene = m_production ? m_production->collection().scene(sceneId) : nullptr;
    Shown shown{sceneId, copy && scene ? std::make_shared<const Scene>(*scene) : nullptr, 0};
    if (shown.copy) shown.serial = ++m_copySerial;
    if (shown.copy && m_copySources) {
        auto sources = std::make_shared<std::map<QString, SourceDef>>();
        for (const SceneItem &item : scene->items)
            if (const SourceDef *def = m_production->collection().source(item.source); def && copiesSource(def->kind))
                sources->emplace(def->id, *def);
        if (!sources->empty()) shown.sources = std::move(sources);
    }
    return shown;
}

bool Session::copiesSource(const QString &kind) {
    return kind == QLatin1String("colour") || kind == QLatin1String("image") || kind == QLatin1String("text") || kind == QLatin1String("slideshow");
}

void Session::syncCopyFeeds(const std::vector<const Shown *> &shown) {
    std::map<QString, const SourceDef *> wanted;
    if (m_running.load())
        for (const Shown *s : shown)
            if (s && s->sources) for (const auto &[id, def] : *s->sources) wanted[copyKey(s->serial, id)] = &def;
    std::vector<std::shared_ptr<Feed>> made;
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        for (auto it = m_copyFeeds.begin(); it != m_copyFeeds.end();) {
            if (wanted.count(it->first)) { ++it; continue; }
            const auto &feed = it->second.feed;
            feed->live.store(false);
            delete feed->animator;
            feed->animator = nullptr;
            ++feed->drawSerial;
            ++feed->filtersAsked;
            m_retiredFeeds.push_back(feed);
            it = m_copyFeeds.erase(it);
        }
        for (const auto &[key, def] : wanted) {
            if (m_copyFeeds.count(key)) continue;
            auto feed = std::make_shared<Feed>();
            feed->sourceId = key;
            feed->kind = def->kind;
            SourceDef own = *def;
            own.id = key;
            m_copyFeeds.emplace(key, CopyFeed{feed, own});
            made.push_back(feed);
        }
    }
    for (const auto &feed : made) syncVideoFilters(feed, m_copyFeeds.at(feed->sourceId).def);
}

void Session::setStudioMode(bool on) {
    if (on == m_studioMode) return;
    m_studioMode = on;
    m_studioRender.store(on);
    if (on) {
        m_studioProgram = m_onAir.scene.isEmpty() || !m_running.load()
                              ? shownFor(m_production ? m_production->collection().currentScene : QString(), false) : m_onAir;
    } else {
        // What is on air stays; editing follows it, as its own layout.
        const QString scene = m_onAir.scene;
        if (m_transition) endTransition();
        m_onAir = shownFor(scene, false);
        m_studioProgram = {};
        if (m_production && m_production->collection().scene(scene)) m_production->setCurrentScene(scene);
    }
    rebuildPlan();
    emit studioModeChanged();
}

QString Session::programScene() const {
    if (m_transition) return m_transition->to.scene;
    if (m_studioMode) return m_studioProgram.scene;
    return m_production ? m_production->collection().currentScene : QString();
}

void Session::takeProgram(const QString &sceneId, bool copy) {
    if (!m_production) return;
    const Scene *scene = m_production->collection().scene(sceneId);
    if (!scene || scene->group) return;
    if (!m_studioMode) {
        m_production->setCurrentScene(sceneId);
        return;
    }
    m_studioProgram = shownFor(sceneId, copy);
    // A T-bar parked part way: the take starts from the mix showing, to
    // whichever studio it names, the one the bar was leaving included.
    if (m_running.load() && manualTransition() && m_production->generation() == m_programGeneration)
        changeProgram(m_production->collection(), m_studioProgram);
    rebuildPlan();
    emit studioModeChanged();
}

void Session::setManualTransition(const QString &sceneId, double progress, bool copy) {
    if (!m_production || !m_running.load()) return;
    const Collection &c = m_production->collection();
    if (progress < 0) {
        if (m_transition && m_transition->manual) endTransition();
        return;
    }
    const Scene *to = c.scene(sceneId);
    if (!to || to->group) return;
    if (!m_transition || !m_transition->manual || m_transition->to.scene != sceneId) {
        if (m_onAir.scene == sceneId && !copy && !m_onAir.copy) return;
        beginTransition(m_onAir, shownFor(sceneId, copy), Transitions::choiceFor(c, sceneId).transition, 0, true,
                        std::clamp(progress, 0.0, 1.0));
        rebuildPlan();
        if (!m_transition) return;  // its studios were gone: dropped there
    }
    m_manualProgress.store(float(std::clamp(progress, 0.0, 1.0)));
    setManualAir(float(std::clamp(progress, 0.0, 1.0)));
    if (progress >= 1.0) {
        // Settled first, so the change on air is not one to animate.
        m_onAir = m_transition->to;
        m_transition.reset();
        unplanTransition();
        if (m_studioMode) m_studioProgram = m_onAir;
        else m_production->setCurrentScene(sceneId);
        rebuildPlan();
        emit transitionChanged();
        emit studioModeChanged();
    }
}

double Session::transitionProgress() const {
    if (!m_transition) return 0;
    if (m_transition->manual) return std::max(0.f, m_manualProgress.load());
    if (m_transition->durationNs <= 0) return 1;
    return std::clamp(double(monotonicNs() - m_transition->startNs) / double(m_transition->durationNs), 0.0, 1.0);
}

void Session::beginTransition(const Shown &from, const Shown &to, const QString &transition, int durationMs, bool manual,
                              double manualAt) {
    const Collection &c = m_production->collection();
    const bool interrupting = m_transition.has_value();
    if (m_transition) { parkStinger(m_transition->stinger); parkStinger(m_transition->stingerMatte); }
    m_transition = ProgramTransition{interrupting ? Shown{} : from, to, manual, monotonicNs(), int64_t(durationMs) * 1'000'000};
    QVariantMap settings = Transitions::settings(c, transition);
    engine::TransitionStyle style = Transitions::style(settings);
    // A stinger by hand is a fade; timed, its video plays over a cut.
    if (Transitions::isStinger(settings) && manual) style.type = engine::Transition::Fade;
    // A cut has nothing between its two studios, so by hand (the T-bar) it
    // fades: the bar mixes whatever is chosen. Take and the cut button still cut.
    if (style.type == engine::Transition::Cut && manual) style.type = engine::Transition::Fade;
    if (Transitions::isStinger(settings) && !manual) {
        std::shared_ptr<Feed> feed, matte;
        {
            std::lock_guard<std::mutex> lock(m_sourcesMutex);
            if (auto it = m_stingers.find(transition); it != m_stingers.end()) feed = it->second;
            if (auto it = m_stingers.find(matteKey(transition)); it != m_stingers.end()
                && settings.value("matte").toString() == QLatin1String("separate")) matte = it->second;
        }
        // A separate matte starts with the video; it is never heard.
        if (feed && matte) {
            m_transition->stingerMatte = matte;
            std::lock_guard<std::mutex> transport(matte->mediaControlMutex);
            matte->media.restart();
            matte->media.setPaused(false);
            matte->mediaPausedAt.store(0);
        }
        if (feed) {
            m_transition->stinger = feed;
            // In frames, by the video's own rate.
            const double fps = feed->media.frameRate();
            const int64_t cutNs = settings.value("cutUnit").toString() == QLatin1String("frames") && fps > 0
                                      ? int64_t(settings.value("cutFrames").toInt() / fps * 1e9)
                                      : int64_t(settings.value("cutMs").toInt()) * 1'000'000;
            m_transition->cutNs = std::min<int64_t>(cutNs, m_transition->durationNs);
            m_transition->outIn = settings.value("audioFade").toString() == QLatin1String("outIn");
            const QString monitor = settings.value("monitor").toString();
            AudioMixer::Routing routing;
            routing.monitoring = monitor == QLatin1String("only") ? AudioMixer::Monitoring::Only
                               : monitor == QLatin1String("both") ? AudioMixer::Monitoring::AndOutput : AudioMixer::Monitoring::Off;
            m_mixer.setRouting(feed->audioChannel, routing);
            m_mixer.clearSource(feed->audioChannel);
            m_mixer.setActive(feed->audioChannel, true);
            std::lock_guard<std::mutex> transport(feed->mediaControlMutex);
            feed->media.restart();
            feed->media.setPaused(false);
            feed->mediaPausedAt.store(0);
        }
    }
    {
        // Published by rebuildPlan with its plans; every caller rebuilds next.
        PlannedTransition next;
        next.on = true;
        next.serial = ++m_transitionSerials;
        next.fromShowing = interrupting;
        next.manual = manual;
        next.style = style;
        next.startNs = m_transition->startNs;
        next.durationNs = m_transition->durationNs;
        next.stinger = m_transition->stinger;
        next.cutAt = m_transition->durationNs > 0 ? float(double(m_transition->cutNs) / double(m_transition->durationNs)) : 0.f;
        const QString matte = settings.value("matte").toString();
        next.matte = !m_transition->stinger ? 0 : matte == QLatin1String("horizontal") ? 1 : matte == QLatin1String("vertical") ? 2
                   : matte == QLatin1String("mask") ? 3 : matte == QLatin1String("separate") && m_transition->stingerMatte ? 4 : 0;
        next.stingerMatte = m_transition->stingerMatte;
        next.matteInvert = settings.value("matteInvert").toBool();
        m_stagedTransition = std::move(next);
    }
    // A timed one leaves the progress alone: one by hand may still be drawing
    // from it until the plans change (rebuildPlan resets it with them).
    if (manual) m_manualProgress.store(float(std::clamp(manualAt, 0.0, 1.0)));
    if (manual) m_transitionEnd.stop();
    else m_transitionEnd.start(std::max(1, durationMs));
    emit transitionChanged();
}

void Session::dropTransition() {
    if (!m_transition) return;
    parkStinger(m_transition->stinger);
    parkStinger(m_transition->stingerMatte);
    m_transition.reset();
    m_transitionEnd.stop();
    unplanTransition();
    QMetaObject::invokeMethod(this, &Session::transitionChanged, Qt::QueuedConnection);
}

void Session::unplanTransition() {
    // One by hand is drawn with the studio it was going to as the plan and the
    // one on air as the outgoing plan. Were it switched off ahead of the plans,
    // a frame in between would show the studio it was going to, whole. So its
    // end waits for rebuildPlan, which every caller runs next.
    if (m_published->transition.on && m_published->transition.manual) {
        m_stagedTransition = PlannedTransition{};
        return;
    }
    // Any other already has the studio now on air as its plan: over at once,
    // the same plans published again without it.
    m_stagedTransition.reset();
    if (!m_published->transition.on) return;
    auto next = std::make_shared<Published>(*m_published);
    next->transition.on = false;
    publish(std::move(next));
}

void Session::publish(std::shared_ptr<const Published> next) {
    const bool byHand = next->transition.on && next->transition.manual;
    {
        std::lock_guard<std::mutex> lock(m_planMutex);
        m_published.swap(next);
        // Here, with the plans: a frame takes both under this lock.
        if (!byHand) m_manualProgress.store(-1.f);
    }
    // What was published before goes here, outside the lock (or with the
    // frame still drawing from it).
}

void Session::changeProgram(const Collection &c, const Shown &wanted) {
    auto choice = Transitions::choiceFor(c, wanted.scene);
    if (m_nextTransition && Transitions::exists(c, m_nextTransition->first)) {
        choice = {m_nextTransition->first, m_nextTransition->second};
        if (Transitions::settings(c, choice.transition).value("kind").toString() == QLatin1String("cut")) choice.durationMs = 0;
    }
    m_nextTransition.reset();
    // A stinger lasts as long as its video; before it opens, it cuts.
    if (Transitions::isStinger(Transitions::settings(c, choice.transition))) choice.durationMs = stingerMs(choice.transition);
    const Shown from = m_onAir;
    m_onAir = wanted;
    if (choice.durationMs > 0 && from.in(c) && wanted.in(c)) beginTransition(from, wanted, choice.transition, choice.durationMs, false);
    else dropTransition();
}

void Session::endTransition() {
    m_transitionEnd.stop();
    if (!m_transition) return;
    parkStinger(m_transition->stinger);
    parkStinger(m_transition->stingerMatte);
    // A hand-driven one given up leaves what was on air.
    m_transition.reset();
    unplanTransition();
    rebuildPlan();
    emit transitionChanged();
}

void Session::followItemVisibility() {
    const int64_t now = monotonicNs();
    const bool fresh = !m_production || !m_running.load() || m_production->generation() != m_itemGeneration;
    if (m_production) m_itemGeneration = m_production->generation();
    // Another collection, or a start: what is there simply shows.
    if (fresh) m_itemTransitions.clear();
    std::erase_if(m_itemTransitions, [&](const auto &t) { return now >= t.second->startNs + t.second->durationNs; });
    std::map<QString, bool> shown;
    if (m_production && m_running.load()) {
        const Collection &c = m_production->collection();
        for (const Scene &scene : c.scenes)
            for (const SceneItem &item : scene.items) {
                shown[item.id] = item.visible;
                const auto was = m_itemShown.find(item.id);
                if (fresh || was == m_itemShown.end() || was->second == item.visible) continue;
                const QString &id = item.visible ? item.showTransition : item.hideTransition;
                const int ms = item.visible ? item.showMs : item.hideMs;
                const QVariantMap settings = id.isEmpty() ? QVariantMap() : Transitions::settings(c, id);
                const auto old = m_itemTransitions.find(item.id);
                if (settings.isEmpty() || ms <= 0 || settings.value(QStringLiteral("kind")).toString() == QLatin1String("cut")) {
                    if (old != m_itemTransitions.end()) m_itemTransitions.erase(old);
                    continue;
                }
                auto t = std::make_shared<ItemTransition>();
                t->style = Transitions::style(settings);
                if (Transitions::isStinger(settings)) t->style.type = engine::Transition::Fade;  // no video over one source
                t->showing = item.visible;
                t->durationNs = int64_t(ms) * 1'000'000;
                t->startNs = now;
                // Turned back part way: it goes back from where it got to.
                if (old != m_itemTransitions.end()) {
                    const double done = std::clamp(double(now - old->second->startNs) / double(old->second->durationNs), 0.0, 1.0);
                    t->startNs = now - int64_t((1.0 - done) * double(t->durationNs));
                }
                m_itemTransitions[item.id] = std::move(t);
            }
    }
    m_itemShown.swap(shown);
    std::erase_if(m_itemTransitions, [&](const auto &t) { return !m_itemShown.count(t.first); });
    int64_t next = 0;
    for (const auto &[id, t] : m_itemTransitions) {
        const int64_t end = t->startNs + t->durationNs;
        if (!next || end < next) next = end;
    }
    if (next) m_itemTransitionEnd.start(int(std::max<int64_t>(1, (next - now) / 1'000'000 + 1)));
    else m_itemTransitionEnd.stop();
}

void Session::rebuildPlan() {
    std::vector<PlanItem> plan, cameraPlan, outgoing, editPlan;
    std::vector<NestedRender> nested;
    followItemVisibility();
    const Collection *collection = m_production ? &m_production->collection() : nullptr;
    // What the program should show: the current studio, or in studio mode
    // the one taken. A change goes through a transition; another collection
    // (or the first plan) simply shows.
    if (collection) {
        const Collection &c = *collection;
        if (m_studioMode && !m_studioProgram.in(c)) m_studioProgram = shownFor(c.currentScene, false);
        const Shown wanted = m_studioMode ? m_studioProgram : Shown{c.currentScene, nullptr, 0};
        if (m_production->generation() != m_programGeneration || !m_running.load()) {
            m_programGeneration = m_production->generation();
            m_onAir = wanted;
            m_nextTransition.reset();
            dropTransition();
        } else if (!wanted.same(m_onAir) && !(m_transition && m_transition->manual)) {
            changeProgram(c, wanted);
        }
        // Studios removed under a transition end it.
        if (m_transition && (!m_transition->to.in(c) || (!m_transition->from.scene.isEmpty() && !m_transition->from.in(c)))) {
            dropTransition();
            // The studio it was leaving is gone too (a T-bar parked part way): a cut.
            if (!m_onAir.in(c)) m_onAir = wanted;
        }
    }
    const Scene *live = nullptr, *outgoingScene = nullptr, *editScene = nullptr;
    const Shown *liveShown = nullptr, *outgoingShown = nullptr;
    if (collection) {
        const Collection &c = *collection;
        if (m_transition && m_transition->manual) {
            outgoingShown = &m_onAir;
            liveShown = &m_transition->to;
        } else {
            liveShown = &m_onAir;
            if (m_transition && !m_transition->from.scene.isEmpty()) outgoingShown = &m_transition->from;
        }
        live = liveShown->in(c);
        outgoingScene = outgoingShown ? outgoingShown->in(c) : nullptr;
        // Studio mode's preview: the studio being edited, unless the program shows it as it is.
        if (m_studioMode && (c.current() != live || (m_transition && m_transition->manual))) editScene = c.current();
        if (m_studioMode && m_onAir.copy && !m_transition) editScene = c.current();
    }
    // The virtual camera's own studio, or one source fitted to the canvas,
    // while it runs; planned alongside so nothing is drawn twice.
    const Scene *cameraScene = nullptr;
    Scene single;
    if (m_production && m_camera.running()) {
        const Collection &c = m_production->collection();
        if (m_profile.cameraShows == QLatin1String("preview")) {
            // Studio mode's preview; otherwise the program.
            if (m_studioMode && c.current() && c.current() != live) cameraScene = c.current();
        } else if (m_profile.cameraShows == QLatin1String("studio")) {
            const Scene *shown = c.scene(m_profile.cameraTarget);
            if (shown && !shown->group && shown != live) cameraScene = shown;
        } else if (m_profile.cameraShows == QLatin1String("source") && c.source(m_profile.cameraTarget)) {
            SceneItem item;
            item.source = m_profile.cameraTarget;
            item.transform.bounds = Transform::Bounds::Fit;
            item.transform.boundsWidth = m_canvasWidth.load();
            item.transform.boundsHeight = m_canvasHeight.load();
            single.items.append(item);
            cameraScene = &single;
        }
    }
    // Which sources' sound fades, worked out once as a transition starts.
    std::set<QString> liveSources, outgoingSources, cameraSources;
    bool freshAir = false;
    if (collection) {
        if (live) showsSources(*collection, *live, liveSources);
        if (outgoingScene) showsSources(*collection, *outgoingScene, outgoingSources);
        if (cameraScene) showsSources(*collection, *cameraScene, cameraSources);
    }
    const uint64_t plannedSerial = m_stagedTransition ? m_stagedTransition->serial : m_published->transition.serial;
    if (m_transition && plannedSerial != m_airSerial) {
        m_airSerial = plannedSerial;
        freshAir = true;
        const std::set<QString> &before = outgoingScene ? outgoingSources : m_airLive;
        m_airOut.clear(); m_airIn.clear();
        for (const QString &id : before) if (!liveSources.count(id) && !cameraSources.count(id)) m_airOut.insert(id);
        for (const QString &id : liveSources) if (!before.count(id)) m_airIn.insert(id);
    } else if (!m_transition) {
        m_airOut.clear(); m_airIn.clear();
    }
    m_airLive = liveSources;
    const bool manualAir = m_transition && m_transition->manual;
    const float airProgress = std::max(0.f, m_manualProgress.load());
    const int64_t airNs = m_transition && !manualAir ? m_transition->durationNs : 0;
    // Out to a stinger's cut, then in over the rest, when it asks.
    const bool outIn = m_transition && m_transition->outIn;
    const int64_t outNs = outIn ? m_transition->cutNs : airNs, inDelayNs = outIn ? m_transition->cutNs : 0;
    const int64_t inNs = outIn ? airNs - m_transition->cutNs : airNs;
    size_t nestedOnAir = 0;
    std::vector<PlannedView> viewPlans;
    syncCopyFeeds({liveShown, outgoingShown, &m_studioProgram});
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        QStringList planned;
        if (live) planScene(*live, plan, nested, planned, 0, liveShown);
        if (outgoingScene) planScene(*outgoingScene, outgoing, nested, planned, 0, outgoingShown);
        if (cameraScene) planScene(*cameraScene, cameraPlan, nested, planned, 0);
        nestedOnAir = nested.size();
        if (editScene) planScene(*editScene, editPlan, nested, planned, 0);
        std::vector<View> wanted = m_views;
        wanted.insert(wanted.end(), m_snapshotViews.begin(), m_snapshotViews.end());
        for (size_t i = 0; i < wanted.size(); ++i) {
            const View &view = wanted[i];
            PlannedView planned_{view, {}, nullptr, i >= m_views.size()};
            // Nothing to show: a snapshot says so with no picture.
            auto missing = [&] {
                if (planned_.snapshot)
                    QMetaObject::invokeMethod(this, [this, id = view.id] { emit snapshotReady(id, {}); }, Qt::QueuedConnection);
            };
            if (view.shows == QLatin1String("studio")) {
                const Scene *scene = collection ? collection->scene(view.target) : nullptr;
                if (!scene || scene->group) { missing(); continue; }
                planScene(*scene, planned_.items, nested, planned, 0);
            } else if (view.shows == QLatin1String("source") || view.shows == QLatin1String("sourceInput")) {
                const auto it = m_feeds.find(view.target);
                if (it == m_feeds.end()) { missing(); continue; }
                planned_.feed = it->second;
            }
            viewPlans.push_back(std::move(planned_));
        }
    }
    // Activity is the union of visible references through all nested studios
    // and groups. A source shared by two live layouts stays active on a switch.
    // Studio mode's preview shows its sources without putting their sound on air.
    {
        std::set<QString> active, onAir;
        for (const auto *list : {&plan, &outgoing, &cameraPlan})
            for (const PlanItem &item : *list) onAir.insert(item.feed->sourceId);
        for (size_t i = 0; i < nestedOnAir; ++i)
            for (const PlanItem &item : nested[i].items) onAir.insert(item.feed->sourceId);
        active = onAir;
        for (const PlanItem &item : editPlan) active.insert(item.feed->sourceId);
        for (const PlannedView &view : viewPlans) {
            for (const PlanItem &item : view.items) active.insert(item.feed->sourceId);
            if (view.feed) active.insert(view.feed->sourceId);
        }
        for (size_t i = nestedOnAir; i < nested.size(); ++i)
            for (const PlanItem &item : nested[i].items) active.insert(item.feed->sourceId);
        // Without the lock: this opens and closes media and draws sources, and
        // the frame thread must not wait for those. A browser's page can call
        // back here, so a copy of the list.
        std::vector<std::pair<QString, std::shared_ptr<Feed>>> feeds(m_feeds.begin(), m_feeds.end());
        for (const auto &[id, feed] : feeds) {
            const bool wasActive = feed->active;
            feed->active = active.count(id);
            if (feed->audioChannel >= 0) {
                m_mixer.setActive(feed->audioChannel,onAir.count(id) > 0);
                const int ch = feed->audioChannel;
                if (freshAir && m_airIn.count(id)) {
                    m_mixer.setAirGain(ch, 0.f);
                    m_mixer.setAirGain(ch, manualAir ? airProgress : 1.f, inNs, inDelayNs);
                } else if (freshAir && m_airOut.count(id)) {
                    m_mixer.setAirGain(ch, manualAir ? 1.f - airProgress : 0.f, outNs);
                } else if (freshAir || !m_transition) {
                    m_mixer.setAirGain(ch, 1.f);
                }
            }
            const SourceDef *def = m_production ? m_production->collection().source(id) : nullptr;
            if (!def) continue;
            feed->linearAlpha.store((def->kind == "image" || feed->isMedia()) ? def->settings.value("linearAlpha",true).toBool() : true);
            if (feed->isMedia()) {
                syncMedia(*feed,*def,feed->active && !wasActive);
            } else if (feed->isBrowser()) {
                syncBrowser(*feed, *def, feed->active && !wasActive);
            } else if (!feed->capture() && !feed->isDevice() && !feed->isAudio() && !feed->isNested()) {
                // Alpha is applied during upload. Editing it must not reopen
                // an image file or restart its animation.
                SourceDef drawn = *def;
                drawn.settings.remove("linearAlpha");
                const bool unload = def->kind == QLatin1String("image") && !feed->active && def->settings.value("unloadInactive",false).toBool();
                if (unload) {
                    ++feed->drawSerial;
                    feed->drawing = false;
                    if (feed->live.exchange(false) || feed->animator) {
                        delete feed->animator; feed->animator = nullptr;
                        std::lock_guard<std::mutex> pixels(feed->pixelsMutex);
                        feed->image = {};
                        feed->width = feed->height = 0;
                        feed->dirty.store(false); feed->releasePicture.store(true);
                    }
                    feed->state = tr("inactive");
                } else if (drawn.settings != feed->settings || !feed->live.load() ||
                           (!feed->uploaded.load() && !feed->dirty.load())) {
                    drawFeed(feed,drawn);
                }
                if (auto *show = qobject_cast<SlideshowAnimator *>(feed->animator)) show->setActive(feed->active);
            }
        }
        // Copies' own sources: drawn once from the settings they were taken with.
        for (auto &[key, copy] : m_copyFeeds) {
            Feed &feed = *copy.feed;
            feed.active = active.count(key);
            feed.linearAlpha.store(copy.def.kind == QLatin1String("image") ? copy.def.settings.value("linearAlpha", true).toBool() : true);
            SourceDef drawn = copy.def;
            drawn.settings.remove("linearAlpha");
            if (drawn.settings != feed.settings || !feed.live.load()) drawFeed(copy.feed, drawn);
            if (auto *show = qobject_cast<SlideshowAnimator *>(feed.animator)) show->setActive(feed.active);
        }
    }
    {
        // All of it at once: the plans, and the transition that goes with them
        // (one staged since the last, or the one already out).
        auto next = std::make_shared<Published>();
        next->plan = std::move(plan);
        next->outgoing = std::move(outgoing);
        next->transition = m_stagedTransition ? std::move(*m_stagedTransition) : m_published->transition;
        m_stagedTransition.reset();
        next->edit = std::move(editPlan);
        next->editShown = editScene != nullptr;
        next->views = std::move(viewPlans);
        next->nested = std::move(nested);
        next->camera = std::move(cameraPlan);
        next->cameraOwn = cameraScene != nullptr;
        publish(std::move(next));
    }
    refreshMonitoring();
}

void Session::startCapture(const std::shared_ptr<Feed> &feed, bool portalFallback) {
    Feed *raw = feed.get();
    if (raw->portal) {
        raw->portal->stop();
        raw->portal->deleteLater();
        raw->portal = nullptr;
    }
    // Its own earlier request is gone with that portal; it will not answer.
    if (m_captureInFlight == raw->sourceId) m_captureInFlight.clear();
    raw->live.store(false);
    if (auto direct=raw->direct.exchange({})) direct->stop();
    raw->stream.stop();
    std::erase(m_captureQueue, raw->sourceId);
    if (!portalFallback && raw->captureSettings.value("captureMethod")=="direct") {
        DirectCapture::Settings settings;
        settings.window=raw->kind=="window"; settings.focused=raw->captureSettings.value("followFocus").toBool();
        settings.cursor=raw->captureSettings.value("cursor",true).toBool();
        settings.key=raw->captureSettings.value("directKey").toString().toStdString();
        settings.output=raw->captureSettings.value("directOutput").toString().toStdString();
        settings.appId=raw->captureSettings.value("directApp").toString().toStdString();
        settings.title=raw->captureSettings.value("directTitle").toString().toStdString();
        settings.fps=std::max(1, int(std::ceil(double(runFpsNum())/std::max(1,runFpsDen()))));
        raw->portalFallbackAllowed=(settings.focused || !settings.key.empty() || !settings.output.empty() || !settings.appId.empty()) &&
            (!m_launching || opensQuietly(raw->sourceId));
        raw->direct.store(m_directCaptures.start(settings)); raw->live.store(true); raw->state=tr("waiting for direct capture");
        pumpCaptures();
        return;
    }
    raw->dmabufFailed = false;
    raw->state = tr("waiting");
    if (std::find(m_captureQueue.begin(), m_captureQueue.end(), raw->sourceId) == m_captureQueue.end() &&
        m_captureInFlight != raw->sourceId)
        m_captureQueue.push_back(raw->sourceId);
    pumpCaptures();
}

void Session::captureAnswered(const QString &sourceId) {
    if (m_captureInFlight != sourceId) return;
    m_captureInFlight.clear();
    QMetaObject::invokeMethod(this, &Session::pumpCaptures, Qt::QueuedConnection);
}

void Session::pumpCaptures() {
    if (!m_captureInFlight.isEmpty() || !m_running.load()) return;
    {
        // Once the engine is there: a stream offers what it can import when it
        // connects, and nothing renegotiates later. The frame thread calls again.
        std::lock_guard<std::mutex> lock(m_engineFormatsMutex);
        if (!m_engineMade) return;
    }
    std::shared_ptr<Feed> feed;
    while (!feed && !m_captureQueue.empty()) {
        const QString id = m_captureQueue.front();
        m_captureQueue.pop_front();
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        auto it = m_feeds.find(id);
        if (it != m_feeds.end()) feed = it->second;
    }
    if (!feed) return;
    Feed *raw = feed.get();
    const QString id = raw->sourceId;
    m_captureInFlight = id;
    raw->state = tr("waiting for the picker");
    emit sourcesChanged();
    raw->portal = new ScreenCastPortal(this);
    connect(raw->portal, &ScreenCastPortal::started, this,
            [this, raw, id](uint nodeId, int fd, const QString &token, const QVariantMap &props) {
                captureAnswered(id);
                // A display's place and size in the layout, (ii) each, where the portal gives them.
                const auto pair = [&](const char *key) {
                    int a = 0, b = 0;
                    const QVariant v = props.value(QLatin1String(key));
                    if (v.canConvert<QDBusArgument>()) {
                        const QDBusArgument arg = v.value<QDBusArgument>();
                        arg.beginStructure();
                        arg >> a >> b;
                        arg.endStructure();
                    }
                    return QPoint(a, b);
                };
                const QPoint at = pair("position"), size = pair("size");
                {
                    std::lock_guard<std::mutex> lock(m_sourcesMutex);
                    raw->layoutArea = size.x() > 0 && size.y() > 0 ? QRect(at, QSize(size.x(), size.y())) : QRect();
                }
                onFeedStarted(raw, nodeId, fd, token);
            });
    connect(raw->portal, &ScreenCastPortal::failed, this, [this, raw, id](const QString &message) {
        captureAnswered(id);
        setFeedState(raw, message == QLatin1String("Cancelled") ? tr("not chosen yet") : tr("failed: %1").arg(message));
    });
    connect(raw->portal, &ScreenCastPortal::closed, this, [this, raw] {
        raw->live.store(false);
        raw->stream.stop();
        setFeedState(raw, tr("closed by the compositor"));
    });
    const SourceDef *def = m_production ? m_production->collection().source(raw->sourceId) : nullptr;
    const bool cursor = def ? def->settings.value(QStringLiteral("cursor"), true).toBool() : true;
    const uint types = raw->kind == QLatin1String("window") ? ScreenCastPortal::Window : ScreenCastPortal::Monitor;
    raw->portal->start(types, cursor ? ScreenCastPortal::Embedded : ScreenCastPortal::Hidden,
                       m_profile.restoreTokens.value(tokenKey(raw->sourceId)).toString());
}

bool Session::opensQuietly(const QString &sourceId) {
    const QString token = m_profile.restoreTokens.value(tokenKey(sourceId)).toString();
    if (token.isEmpty()) return false;
    switch (ScreenCastPortal::grant(token)) {
    case ScreenCastPortal::Grant::Held: return true;
    case ScreenCastPortal::Grant::Unknown: return false;  // kept: it may answer next time
    case ScreenCastPortal::Grant::Gone: break;
    }
    qInfo("The saved choice for %s is no longer known to the portal: choose it again.", qPrintable(sourceId));
    m_profile.restoreTokens.remove(tokenKey(sourceId));
    emit restoreTokenChanged(sourceId, {});
    return false;
}

void Session::reopenFeed(const QString &sourceId) {
    std::shared_ptr<Feed> feed;
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        auto it = m_feeds.find(sourceId);
        if (it != m_feeds.end()) feed = it->second;
    }
    if (!feed || !feed->capture()) return;
    // A fresh pick: the old token named what the person no longer wants.
    m_profile.restoreTokens.remove(tokenKey(sourceId));
    emit restoreTokenChanged(sourceId, {});
    startCapture(feed);
    emit sourcesChanged();
}

QVariantMap Session::slideshowPlayback(const QString &sourceId) const {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    const auto it = m_feeds.find(sourceId);
    auto *show = it == m_feeds.end() ? nullptr : qobject_cast<SlideshowAnimator *>(it->second->animator);
    return show ? show->playback() : QVariantMap{};
}

bool Session::controlSlideshow(const QString &sourceId, const QString &action, double value) {
    SlideshowAnimator *show = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        const auto it = m_feeds.find(sourceId);
        if (it != m_feeds.end()) show = qobject_cast<SlideshowAnimator *>(it->second->animator);
    }
    return show && show->control(action, value);
}

QVariantMap Session::mediaPlayback(const QString &sourceId) const {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    const auto it = m_feeds.find(sourceId);
    if (it == m_feeds.end() || !it->second->isMedia()) return {};
    const auto &media = it->second->media;
    const auto p = media.playback();
    return {{"position",p.position},{"duration",p.duration},{"speed",p.speed},{"paused",p.paused},
            {"seekable",p.seekable && !media.closed()},{"network",media.network()},{"closed",media.closed()},
            {"opened",media.opened()},{"state",it->second->live.load() ? QString::fromStdString(media.state()) : it->second->state}};
}
bool Session::controlMedia(const QString &sourceId,const QString &action,double value) {
    const auto it = m_feeds.find(sourceId);
    if (it == m_feeds.end() || !it->second->isMedia() || !it->second->live.load() || it->second->media.network()) return false;
    auto &feed = *it->second;
    std::lock_guard<std::mutex> transport(feed.mediaControlMutex);
    bool changed = false;
    if (action == "pause") changed = feed.media.setPaused(value != 0);
    else if (action == "seek") changed = feed.media.seek(value);
    else if (action == "restart") { feed.media.restart(); changed = true; }
    else if (action == "stop") { changed = feed.media.setPaused(true); feed.media.seek(0); }
    if (changed) {
        feed.mediaPausedAt.store(feed.media.playback().paused ? monotonicNs() : 0);
        m_mixer.clearSource(feed.audioChannel);
    }
    return changed;
}

void Session::onFeedStarted(Feed *f, uint nodeId, int fd, const QString &token) {
    std::string err;
    // As the frame thread published them, not asked of the engine from here: it
    // can be replaced (a lost device) at any moment.
    VideoStream::ModifierLookup lookup;
    {
        std::lock_guard<std::mutex> lock(m_engineFormatsMutex);
        if (!m_engineFormats.empty())
            lookup = [modifiers = m_engineFormats](uint32_t fourcc) {
                const auto it = modifiers.find(fourcc);
                return it == modifiers.end() ? std::vector<uint64_t>{} : it->second;
            };
    }
    if (!f->stream.start(fd, nodeId, lookup, &err)) {
        setFeedState(f, tr("failed: %1").arg(QString::fromStdString(err)));
        return;
    }
    f->live.store(true);
    setFeedState(f, tr("waiting for a picture"));
    // Remember the token so the next run skips the picker.
    if (!token.isEmpty() && m_profile.restoreTokens.value(tokenKey(f->sourceId)).toString() != token) {
        m_profile.restoreTokens.insert(tokenKey(f->sourceId), token);
        emit restoreTokenChanged(f->sourceId, token);
    }
}

QMap<QString, QString> Session::fileNameFields() const {
    const Scene *studio = m_production ? m_production->currentScene() : nullptr;
    const double fps = runFpsDen() > 0 ? double(runFpsNum()) / runFpsDen() : 0;
    // Before the canvas settles, the profile's size (0 = still to follow a display).
    const int width = canvasWidth() > 0 ? canvasWidth() : m_profile.canvasWidth;
    const int height = canvasHeight() > 0 ? canvasHeight() : m_profile.canvasHeight;
    return {{QStringLiteral("studio"), studio ? studio->name : QString()},
            {QStringLiteral("profile"), m_profile.name},
            {QStringLiteral("collection"), m_profile.collection},
            {QStringLiteral("width"), QString::number(width)},
            {QStringLiteral("height"), QString::number(height)},
            {QStringLiteral("fps"), QString::number(fps, 'g', 4)}};
}

bool Session::automaticGpuChanged(QString *name, bool *onBattery) const {
    if (!m_running.load() || m_safeMode || m_gpuChoice != QLatin1String("auto") || qEnvironmentVariableIsSet("OMABROADCAST_GPU"))
        return false;
    std::lock_guard<std::mutex> lock(m_encoderMutex);
    if (m_gpusAtStart.empty() || m_gpuPickIndex < 0) return false;
    const bool battery = machinesurvey::readPower().onBattery;
    const engine::GpuPick pick = engine::chooseGpu(m_gpusAtStart, {}, battery);
    if (pick.index < 0 || pick.index == m_gpuPickIndex) return false;
    if (name) *name = QString::fromStdString(m_gpusAtStart[size_t(pick.index)].name);
    if (onBattery) *onBattery = battery;
    return true;
}

void Session::setFeedState(Feed *f, const QString &state) {
    f->state = state;
    emit sourcesChanged();
}

void Session::syncDevice(Feed &feed, const SourceDef &def) {
    const QString node = def.settings.value(QStringLiteral("node")).toString();
    const QString wanted = def.settings.value(QStringLiteral("mode")).toString();
    const VideoDevice *device = nullptr;
    const std::vector<VideoDevice> devices = m_videoDevices.devices();
    for (const VideoDevice &d : devices)
        if (QString::fromStdString(d.name) == node && !d.modes.empty()) device = &d;
    if (!device) {
        if (feed.device.running() || feed.live.load() || feed.state.isEmpty()) {
            feed.device.stop();
            feed.live.store(false);
            feed.deviceNode.clear();
            feed.deviceMode.clear();
            feed.state = tr("not connected");
        }
        return;
    }
    syncColour(feed, def);
    const VideoMode mode = pickVideoMode(device->modes, wanted.toStdString(), double(runFpsNum()) / std::max(1, runFpsDen()));
    const QString key = QString::fromStdString(mode.key());
    const QVariantMap controls = def.settings.value(QStringLiteral("controls")).toMap();
    auto applyControls = [&] {
        std::map<uint32_t, int> values;
        for (auto it = controls.begin(); it != controls.end(); ++it) values[it.key().toUInt()] = it.value().toInt();
        if (!values.empty()) applyDeviceControls(device->path, values);
        feed.appliedControls = controls;
    };
    if (feed.deviceNode == node && feed.deviceMode == key && feed.live.load()) {
        if (controls != feed.appliedControls) applyControls();  // a change or an undo, no restart
        return;
    }
    std::string err;
    if (!feed.device.start(*device, mode, &err)) {
        feed.state = tr("failed: %1").arg(QString::fromStdString(err));
        feed.live.store(false);
        return;
    }
    feed.deviceNode = node;
    feed.deviceMode = key;
    feed.settings = def.settings;
    feed.state = tr("waiting for a picture");
    feed.live.store(true);
    // Devices forget their controls when they are unplugged or start again.
    applyControls();
}

// When the list changes. LUT and mask files are read off the main thread too,
// and until they come the chain as it was goes on, unless it would show what
// the new one is there to keep off air.
void Session::syncVideoFilters(const std::shared_ptr<Feed> &shared, const SourceDef &def) {
    Feed &feed = *shared;
    const QVariantList settings = def.settings.value(QStringLiteral("videoFilters")).toList();
    if (settings == feed.videoFilterSettings) return;
    feed.videoFilterSettings = settings;
    const quint64 asked = ++feed.filtersAsked;
    const auto apply = [](Feed &f, std::vector<engine::VideoFilter> &&chain, const QStringList &problems) {
        f.videoFilterProblems = problems;
        std::lock_guard<std::mutex> lock(f.colourMutex);
        f.videoFilters = std::move(chain);
        ++f.videoFiltersSerial;
    };
    if (m_safeMode || !VideoFilterSettings::readsFiles(settings)) {
        QStringList problems;
        auto chain = VideoFilterSettings::parse(settings, &problems, !m_safeMode);
        apply(feed, std::move(chain), problems);
        return;
    }
    if (VideoFilterSettings::cutsOut(settings)) {
        std::lock_guard<std::mutex> lock(feed.colourMutex);
        if (!engine::chainCutsOut(feed.videoFilters)) {
            feed.videoFilters = {drawsNothing()};
            ++feed.videoFiltersSerial;
        }
    }
    m_fileWork.run([this, weak = std::weak_ptr<Feed>(shared), asked, settings, apply](const FileWork::Token &) -> FileWork::Answer {
        auto problems = std::make_shared<QStringList>();
        auto chain = std::make_shared<std::vector<engine::VideoFilter>>(VideoFilterSettings::parse(settings, problems.get()));
        return [this, weak, asked, chain, problems, apply] {
            const auto feed = weak.lock();
            if (!feed || asked != feed->filtersAsked) return;
            apply(*feed, std::move(*chain), *problems);
            emit sourcesChanged();  // what the filters have to say, and any new size
        };
    });
}

QStringList Session::videoFilterProblems(const QString &sourceId) const {
    QStringList problems;
    bool findsPeople = false;
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        const auto it = m_feeds.find(sourceId);
        if (it == m_feeds.end()) return {};
        problems = it->second->videoFilterProblems;
        std::lock_guard<std::mutex> colour(it->second->colourMutex);
        for (const engine::VideoFilter &f : it->second->videoFilters) findsPeople |= f.type == engine::VideoFilterType::RemoveBackground;
    }
    if (findsPeople && stats().gpu) {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        const auto it = m_feeds.find(sourceId);
        if (it != m_feeds.end() && it->second->cutOutFailure.load() != engine::FilterFailure::None)
            problems.append(cutOutFailureText(it->second->cutOutFailure.load()));
    }
    if (findsPeople && !stats().gpu)
        problems.append(QCoreApplication::translate("VideoFilters", "Remove background (AI): this source is hidden, as the filter needs a graphics card and this computer is drawing without one"));
    return problems;
}

void Session::syncColour(Feed &feed, const SourceDef &def) {
    // Noise reduction: Off, Low, Medium, High.
    static const float kStrength[] = {0.f, 0.33f, 0.66f, 1.f};
    feed.denoise.store(kStrength[std::clamp(def.settings.value(QStringLiteral("denoise"), 0).toInt(), 0, 3)]);
    std::lock_guard<std::mutex> lock(feed.colourMutex);
    feed.region = def.kind == "region" ? CaptureRegion::fromSettings(def.settings) : CaptureRegion{};
    feed.deinterlace = (feed.capture() || feed.isDevice() || feed.isMedia()) ? DeinterlaceSettings::fromSettings(def.settings) : DeinterlaceSettings{};
    feed.colourRange = def.settings.value(QStringLiteral("range"), QStringLiteral("auto")).toString().toStdString();
    feed.colourMatrix = def.settings.value(QStringLiteral("matrix"), QStringLiteral("auto")).toString().toStdString();
}

QString Session::devicePath(const QString &node) const {
    for (const VideoDevice &d : m_videoDevices.devices())
        if (QString::fromStdString(d.name) == node) return QString::fromStdString(d.path);
    return {};
}

std::vector<DeviceControl> Session::deviceControls(const QString &sourceId) const {
    const SourceDef *def = m_production ? m_production->collection().source(sourceId) : nullptr;
    if (!def || def->kind != QLatin1String("device")) return {};
    const QString path = devicePath(def->settings.value(QStringLiteral("node")).toString());
    return path.isEmpty() ? std::vector<DeviceControl>{} : listDeviceControls(path.toStdString());
}

float Session::sourceAudioPeak(const QString &sourceId) const {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    auto it = m_feeds.find(sourceId);
    return it == m_feeds.end() ? 0.f : m_mixer.sourcePeak(it->second->audioChannel);
}

PeakMeter::Reading Session::sourceAudioMeter(const QString &sourceId) const {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    auto it = m_feeds.find(sourceId);
    return it == m_feeds.end() ? PeakMeter::Reading{std::vector<float>(m_mixer.channels(), 0.f), 0.f} : m_mixer.sourceMeter(it->second->audioChannel);
}

void Session::resetAudioMeterMaximum(int globalChannel, const QString &sourceId) {
    if (globalChannel == 0 || globalChannel == 1) { m_mixer.resetMeterMaximum(globalChannel); return; }
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    if (auto it = m_feeds.find(sourceId); it != m_feeds.end()) m_mixer.resetMeterMaximum(it->second->audioChannel);
}

Session::Feed *Session::audioFeed(const QString &target) const {
    const bool global = target.startsWith("global:");
    const auto &feeds = global ? m_globalAudioFeeds : m_feeds;
    const QString id = global || target.startsWith("source:") ? target.mid(7) : target;
    const auto it = feeds.find(id);
    return it == feeds.end() ? nullptr : it->second.get();
}

PeakMeter::Reading Session::globalAudioDeviceMeter(const QString &id) const {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    const auto *feed = audioFeed("global:" + id);
    return feed ? m_mixer.sourceMeter(feed->audioChannel) : PeakMeter::Reading{std::vector<float>(m_mixer.channels(), 0.f), 0.f};
}
void Session::resetGlobalAudioDeviceMeter(const QString &id) {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    if (const auto *feed = audioFeed("global:" + id)) m_mixer.resetMeterMaximum(feed->audioChannel);
}
QString Session::globalAudioDeviceStatus(const QString &id) const {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    const auto *feed = audioFeed("global:" + id);
    if (!feed) return {};
    if (!feed->audioError.isEmpty()) return feed->audioError;
    for (const auto &[serial, tap] : feed->audioCaptures)
        if (const auto error = tap.capture->error(); !error.empty()) return QString::fromStdString(error);
    return feed->audioNode.isEmpty() ? tr("Off") : feed->audioCaptures.empty() ? tr("Not connected") : QString();
}

bool Session::audioPushHeld(const QString &target, bool talk) const {
    if (target == "desktop" || target == "microphone") return m_mixer.pushHeld(target == "desktop" ? 0 : 1, talk);
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    const auto *feed = audioFeed(target);
    return feed && m_mixer.pushHeld(feed->audioChannel, talk);
}
void Session::holdAudioPush(const QString &target, bool talk, bool held, unsigned origin) {
    if (target == "desktop" || target == "microphone") {
        m_mixer.holdPush(target == "desktop" ? 0 : 1, talk, held, origin); return;
    }
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    if (const auto *feed = audioFeed(target)) m_mixer.holdPush(feed->audioChannel, talk, held, origin);
}
void Session::cancelAudioPush(const QString &target, unsigned origins) {
    if (target.isEmpty()) { m_mixer.cancelPush(-1, origins); return; }
    if (target == "desktop" || target == "microphone") { m_mixer.cancelPush(target == "desktop" ? 0 : 1, origins); return; }
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    if (const auto *feed = audioFeed(target)) m_mixer.cancelPush(feed->audioChannel, origins);
}

QString Session::sourceAudioError(const QString &sourceId) const {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    auto it = m_feeds.find(sourceId);
    if (it == m_feeds.end()) return {};
    if (!it->second->audioError.isEmpty()) return it->second->audioError;
    for (const auto &[serial, capture] : it->second->audioCaptures)
        if (const std::string error = capture.capture->error(); !error.empty()) return QString::fromStdString(error);
    return {};
}

void Session::syncGlobalAudioDevices() {
    if (!running()) return;
    // Only this thread looks at these: no lock while sound streams open and close.
    for (auto it = m_globalAudioFeeds.begin(); it != m_globalAudioFeeds.end();) {
        const auto *def = m_profile.globalAudioDevice(it->first);
        if (!def || def->kind != it->second->kind) {
            it->second->audioCaptures.clear();
            m_mixer.removeSource(it->second->audioChannel);
            std::lock_guard<std::mutex> lock(m_sourcesMutex);
            it = m_globalAudioFeeds.erase(it);
        } else ++it;
    }
    for (const auto &def : m_profile.globalAudioDevices) {
        if (def.kind != "audio-input" && def.kind != "audio-output") continue;
        auto found = m_globalAudioFeeds.find(def.id);
        if (found == m_globalAudioFeeds.end()) {
            auto feed = std::make_shared<Feed>();
            feed->sourceId = def.id; feed->kind = def.kind;
            feed->audioChannel = m_mixer.addSource(false);
            std::lock_guard<std::mutex> lock(m_sourcesMutex);
            found = m_globalAudioFeeds.emplace(def.id, std::move(feed)).first;
        }
        syncFeedAudio(*found->second, def);
        m_mixer.setActive(found->second->audioChannel, !def.settings.value("audioNode").toString().isEmpty());
    }
}

void Session::syncFeedAudio(Feed &feed, const SourceDef &def) {
    if (feed.audioChannel < 0) return;
    m_mixer.setPushSettings(feed.audioChannel, AudioSettings::push(def.settings));
    m_mixer.setRouting(feed.audioChannel, AudioSettings::routing(def.settings));
    m_mixer.setFilters(feed.audioChannel, AudioFilterSettings::parse(def.settings.value(QStringLiteral("audioFilters")).toList()));
    m_mixer.setProcessing(feed.audioChannel, AudioSettings::processing(def.settings));
    m_mixer.setGain(feed.audioChannel, def.settings.value(QStringLiteral("volume"), 1.0).toFloat());
    m_mixer.setMuted(feed.audioChannel, def.settings.value(QStringLiteral("muted"), false).toBool());
    if (feed.isBrowser()) {
        // The page plays into a sink of its own: its monitor is this source's
        // sound. Nothing while the page is stopped or muted.
        const QString sink = feed.browser ? feed.browser->soundSink() : QString();
        if (sink != feed.audioNode) {
            for (auto &[key, tap] : feed.audioCaptures) {
                tap.capture.reset();
                m_mixer.removeInput(feed.audioChannel, tap.input);
            }
            feed.audioCaptures.clear();
            m_mixer.clearSource(feed.audioChannel);
            feed.audioNode = sink;
        }
        if (!sink.isEmpty() && feed.audioCaptures.empty()) tapAudio(feed, sink.toStdString(), sink.toStdString(), true);
        return;
    }
    if (!feed.isDevice() && !feed.isAudio()) return;
    const QString target = def.settings.value(QStringLiteral("audioNode")).toString();
    const AudioNode::Kind kind = def.kind == QLatin1String("audio-output") ? AudioNode::Output
                               : def.kind == QLatin1String("audio-application") ? AudioNode::Application : AudioNode::Input;
    std::map<std::string, AudioNode> wanted;
    for (const AudioNode &node : m_devices.nodes()) {
        const std::string key = kind == AudioNode::Application ? node.application : node.name;
        if (!target.isEmpty() && node.kind == kind && QString::fromStdString(key) == target)
            wanted.emplace(node.serial, node);
    }
    if (target != feed.audioNode) {
        for (auto &[serial, tap] : feed.audioCaptures) {
            tap.capture.reset();
            m_mixer.removeInput(feed.audioChannel, tap.input);
        }
        feed.audioCaptures.clear();
        m_mixer.clearSource(feed.audioChannel);
        feed.audioNode = target;
    }
    for (auto it = feed.audioCaptures.begin(); it != feed.audioCaptures.end();) {
        if (!wanted.count(it->first)) {
            it->second.capture.reset();
            m_mixer.removeInput(feed.audioChannel, it->second.input);
            it = feed.audioCaptures.erase(it);
        }
        else ++it;
    }
    feed.audioError.clear();
    for (const auto &[serial, node] : wanted)
        if (!feed.audioCaptures.count(serial)) tapAudio(feed, serial, serial, kind == AudioNode::Output);
    if (feed.isAudio()) {
        feed.live.store(!feed.audioCaptures.empty());
        feed.state = !feed.audioError.isEmpty() ? feed.audioError : target.isEmpty() ? tr("off")
                   : feed.audioCaptures.empty() ? tr("not connected") : tr("waiting for audio");
    }
}

void Session::tapAudio(Feed &feed, const std::string &key, const std::string &target, bool sinkMonitor) {
    auto capture = std::make_unique<AudioCapture>();
    capture->setFormat(m_profile.audioFormat);
    capture->setLatencyFrames(m_lowLatencyAudio ? 128 : 480);
    capture->onStateChanged = [this] {
        QMetaObject::invokeMethod(this, &Session::sourcesChanged, Qt::QueuedConnection);
    };
    capture->setTarget(target, sinkMonitor);
    std::string error;
    const int input = m_mixer.addInput(feed.audioChannel);
    if (capture->start([this, channel = feed.audioChannel, input](int, int64_t time, const float *data, int frames) {
            m_mixer.push(channel, time, data, frames, input, monotonicNs());
        }, &error, false)) feed.audioCaptures.emplace(key, Feed::AudioTap{std::move(capture), input});
    else {
        m_mixer.removeInput(feed.audioChannel, input);
        feed.audioError = tr("Audio capture failed: %1").arg(QString::fromStdString(error));
    }
}

QString Session::browserUrl(const QVariantMap &settings) {
    if (settings.value(QStringLiteral("local")).toBool()) {
        const QString path = settings.value(QStringLiteral("path")).toString();
        return path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString();
    }
    const QString url = settings.value(QStringLiteral("url")).toString().trimmed();
    // "example.com/overlay" is a web page, "/home/…" a file, as people type them.
    return url.isEmpty() ? QString() : QUrl::fromUserInput(url).toString();
}

void Session::syncBrowser(Feed &feed, const SourceDef &def, bool activated) {
    const QVariantMap &s = def.settings;
    if (!BrowserPage::available()) {
        feed.state = tr("needs Qt WebEngine (the qt6-webengine package)");
        return;
    }
    // Shut down while no live studio shows it, when asked: back from the start when shown.
    if (!feed.active && s.value(QStringLiteral("shutdownHidden"), false).toBool()) {
        if (feed.browser) {
            feed.browser.reset();
            {
                std::lock_guard<std::mutex> lock(feed.pixelsMutex);
                feed.image = {};
                feed.width = feed.height = 0;
            }
            feed.dirty.store(false);
            feed.live.store(false);
            feed.releasePicture.store(true);
            syncFeedAudio(feed, def);  // its sound goes with it
        }
        feed.state = tr("inactive");
        return;
    }
    BrowserPage::Settings want;
    want.url = browserUrl(s);
    want.width = std::clamp(s.value(QStringLiteral("width"), 800).toInt(), 1, 8192);
    want.height = std::clamp(s.value(QStringLiteral("height"), 600).toInt(), 1, 8192);
    want.fps = std::clamp(s.value(QStringLiteral("fps"), 30).toInt(), 1, 120);
    want.css = s.value(QStringLiteral("css")).toString();
    const bool fresh = !feed.browser;
    if (fresh) {
        // Its own site data, kept per collection like screen choices.
        QString key = tokenKey(def.id);
        key.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
        const QString storage = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/browser/") + key;
        feed.browser = std::make_unique<BrowserPage>(storage, def.name, m_profile.audioFormat.normalized().channels);
        BrowserPage *page = feed.browser.get();
        Feed *f = &feed;  // the page goes before its feed, on this thread
        page->onFrame = [this, f](const uchar *pixels, int w, int h, int stride) {
            bool resized;
            {
                std::lock_guard<std::mutex> lock(f->pixelsMutex);
                resized = f->width != w || f->height != h;
                f->width = w;
                f->height = h;
                // Into the picture it has, which is its own again once the frame
                // thread has put it back; a new one while an upload has it out.
                if (f->image.width() != w || f->image.height() != h || f->image.format() != QImage::Format_ARGB32_Premultiplied)
                    f->image = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
                if (!f->image.isNull())
                    for (int y = 0; y < h; ++y) std::memcpy(f->image.scanLine(y), pixels + size_t(y) * stride, size_t(w) * 4);
                f->pixelsPremultiplied = true;
            }
            f->live.store(true);
            f->dirty.store(true);
            if (resized) emit sourcesChanged();
            if (m_browserWatched.count(f->sourceId))
                emit browserFrameReady(f->sourceId, QImage(pixels, w, h, stride, QImage::Format_ARGB32_Premultiplied).copy());
        };
        connect(page, &BrowserPage::stateChanged, this, [this, f, page, id = def.id] {
            const SourceDef *d = m_production ? m_production->collection().source(id) : nullptr;
            f->state = d ? browserState(*page, d->settings) : page->state();
            // Queued: a first load can start inside rebuildPlan, under m_sourcesMutex.
            QMetaObject::invokeMethod(this, &Session::sourcesChanged, Qt::QueuedConnection);
        });
        connect(page, &BrowserPage::soundChanged, this, [this, id = def.id] {
            const auto it = m_feeds.find(id);
            const SourceDef *d = m_production ? m_production->collection().source(id) : nullptr;
            if (it != m_feeds.end() && d) syncFeedAudio(*it->second, *d);
        });
    }
    feed.browser->setSettings(want);
    if (activated && !fresh && s.value(QStringLiteral("refreshOnActive"), false).toBool()) feed.browser->reload();
    feed.state = browserState(*feed.browser, s);
}

// A page with nothing to show says what it is waiting for, not "live".
QString Session::browserState(const BrowserPage &page, const QVariantMap &settings) {
    if (page.state() != tr("live") || !browserUrl(settings).isEmpty()) return page.state();
    return settings.value(QStringLiteral("local")).toBool() ? tr("no file chosen") : tr("no web address yet");
}

bool Session::reloadBrowser(const QString &sourceId) {
    const auto it = m_feeds.find(sourceId);
    if (it == m_feeds.end() || !it->second->browser) return false;
    it->second->browser->reload();
    return true;
}

void Session::browserInput(const QString &sourceId, const QVariantMap &event) {
    if (const auto it = m_feeds.find(sourceId); it != m_feeds.end() && it->second->browser) it->second->browser->input(event);
}

void Session::setBrowserWatched(const QString &sourceId, bool watched) {
    if (!watched) { m_browserWatched.erase(sourceId); return; }
    m_browserWatched.insert(sourceId);
    // The picture it has now, so a window opened on a still page shows it.
    QImage now;
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        const auto it = m_feeds.find(sourceId);
        if (it == m_feeds.end() || !it->second->isBrowser()) return;
        const Feed &f = *it->second;
        std::lock_guard<std::mutex> pixels(f.pixelsMutex);
        if (f.width > 0 && f.height > 0 && !f.image.isNull()) now = f.image.copy();
    }
    if (!now.isNull()) emit browserFrameReady(sourceId, now);
}

QVariantMap Session::browserStatus(const QString &sourceId) const {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    const auto it = m_feeds.find(sourceId);
    if (it == m_feeds.end() || !it->second->browser) return {};
    const BrowserPage &page = *it->second->browser;
    return {{QStringLiteral("state"), it->second->state}, {QStringLiteral("title"), page.title()},
            {QStringLiteral("starts"), page.starts()}, {QStringLiteral("pid"), page.helperPid()},
            {QStringLiteral("sound"), page.soundSink()}};
}

void Session::syncMedia(Feed &feed, const SourceDef &def, bool activated) {
    syncColour(feed, def);
    // The stream restarts only when what it plays changed, not for every edit.
    // Volume belongs to the mixer and never restarts playback.
    const QVariantMap wanted{{QStringLiteral("input"), def.settings.value(QStringLiteral("input"), QString())},
                             {QStringLiteral("loop"), def.settings.value(QStringLiteral("loop"), false)},
                             {QLatin1String(kPassphraseKey), def.settings.value(QLatin1String(kPassphraseKey), QString())}};
    const double speed = std::clamp(def.settings.value("speed",1.).toDouble(),.25,4.);
    const bool closed = !feed.active && def.settings.value("closeInactive",false).toBool();
    if (closed) feed.live.store(false);
    if (wanted == feed.mediaSettings && !feed.state.isEmpty()) {
        std::lock_guard<std::mutex> transport(feed.mediaControlMutex);
        const bool wasClosed = feed.media.closed();
        feed.media.setClosed(closed);
        const bool restart = activated && !wasClosed && !closed && !feed.media.network() &&
                             def.settings.value("restart",true).toBool();
        if (restart) feed.media.restart();
        if (wasClosed != closed || restart) {
            m_mixer.clearSource(feed.audioChannel);
            feed.mediaPausedAt.store(0);
            feed.releasePicture.store(true);
        }
        if (!feed.media.network() && feed.media.playback().speed != speed && feed.media.setSpeed(speed))
            m_mixer.clearSource(feed.audioChannel);
        feed.live.store(!closed && !wanted.value("input").toString().isEmpty());
        feed.state = wanted.value("input").toString().isEmpty() ? tr("no file chosen") :
                     closed ? tr("inactive") : QString::fromStdString(feed.media.state());
        return;
    }
    feed.live.store(false);
    // Closing its decoders takes a moment, and the frame and audio threads go on
    // without it: outside mediaControlMutex, which the audio worker takes.
    feed.media.stop();
    std::lock_guard<std::mutex> transport(feed.mediaControlMutex);
    m_mixer.clearSource(feed.audioChannel);
    feed.mediaPausedAt.store(0);
    feed.mediaSettings = wanted;
    feed.releasePicture.store(true);
    const QString input = wanted.value(QStringLiteral("input")).toString();
    if (input.isEmpty()) {
        feed.live.store(false);
        feed.state = tr("no file chosen");
        return;
    }
    feed.media.start({input.toStdString(), wanted.value(QStringLiteral("loop")).toBool(),
                      1.f, m_profile.audioFormat, speed, closed, wanted.value(QLatin1String(kPassphraseKey)).toString().toStdString()});
    feed.state = closed ? tr("inactive") : tr("opening");
    feed.live.store(!closed);
}

void Session::refreshDevices() {
    if (m_production)
        for (auto &[id, feed] : m_feeds)
            if (const SourceDef *def = m_production->collection().source(id); def && feed->isDevice())
                syncDevice(*feed, *def);
    emit videoDevicesChanged();
    emit sourcesChanged();
}

void Session::setPixels(Feed &feed, QImage image) {
    // ARGB32 is straight alpha stored B, G, R, A on little-endian machines.
    // The picture itself goes over, not a copy of its pixels: each one that
    // comes here owns them (decoded, drawn or filled) and is not written to
    // again, so the frame thread can read them for as long as it holds it.
    image = image.convertToFormat(QImage::Format_ARGB32);
    const int width = image.width(), height = image.height();
    {
        std::lock_guard<std::mutex> lock(feed.pixelsMutex);
        feed.width = width;
        feed.height = height;
        feed.image.swap(image);
    }
    // The one before goes here, outside the lock.
    feed.live.store(true);
    feed.dirty.store(true);
}

void Session::animate(const std::shared_ptr<Feed> &shared, const SourceDef &def, const QByteArray &movie) {
    Feed &feed = *shared;
    if (feed.animator && feed.animator->settings == def.settings) return;  // already running as asked
    delete feed.animator;
    feed.animator = nullptr;
    if (def.kind == QLatin1String("slideshow"))
        feed.animator = new SlideshowAnimator(def.settings, m_canvasWidth.load(), m_canvasHeight.load(), this);
    else
        feed.animator = new MovieAnimator(movie, this);
    feed.animator->settings = def.settings;
    feed.settings = def.settings;
    feed.state = tr("live");
    // The feed may go while a frame is on its way.
    const std::weak_ptr<Feed> weak = shared;
    const QPointer<Animator> animator(feed.animator);
    if (auto *show = qobject_cast<SlideshowAnimator *>(feed.animator)) {
        connect(show, &SlideshowAnimator::presentationChanged, this, [this, weak, animator, show, serial = quint64(0), status = QVariantMap{}]() mutable {
            auto f = weak.lock();
            if (!f || !animator || f->animator != animator.data()) return;
            auto view = show->presentation();
            if (view.to.isNull()) return;
            bool resized;
            {
                std::lock_guard<std::mutex> lock(f->pixelsMutex);
                resized = f->width != view.to.width() || f->height != view.to.height();
                f->width = view.to.width(); f->height = view.to.height();
                if (serial != view.serial) { serial = view.serial; ++f->slideCounter; }
                view.serial = f->slideCounter; f->slideshow = std::move(view);
            }
            f->state = tr("live"); f->live.store(true); f->dirty.store(true);
            if (resized) QMetaObject::invokeMethod(this, &Session::sourcesChanged, Qt::QueuedConnection);
            const auto now = show->playback();
            if (now != status) {
                status = now;
                QMetaObject::invokeMethod(this, &Session::slideshowChanged, Qt::QueuedConnection);
            }
        });
    } else connect(feed.animator, &Animator::frame, this, [this, weak, animator](const QImage &image) {
        auto f = weak.lock();
        if (!f || !animator || f->animator != animator.data()) return;
        bool resized;
        {
            std::lock_guard<std::mutex> lock(f->pixelsMutex);
            resized = f->width != image.width() || f->height != image.height();
        }
        setPixels(*f, image);
        if (resized) emit sourcesChanged();  // the list shows its size
    });
    connect(feed.animator, &Animator::failed, this, [this, weak, animator](const QString &why) {
        if (auto f = weak.lock(); f && animator && f->animator == animator.data()) {
            f->state = tr("failed: %1").arg(why);
            if (f->kind == "slideshow") {
                QImage clear(1, 1, QImage::Format_ARGB32); clear.fill(Qt::transparent);
                { std::lock_guard<std::mutex> lock(f->pixelsMutex);
                  f->slideshow = {clear, clear, engine::Transition::Cut, 1.f, ++f->slideCounter, false};
                  f->width = f->height = 1; }
                f->live.store(true); f->dirty.store(true);
            }
            emit sourcesChanged();
        }
    });
}

// What a drawn source's file came to, found off the main thread.
struct Session::DrawnFile {
    QImage image;
    QByteArray movie;        // an animated picture's whole file
    QString text;            // a text source's file
    FileStamp stamp;         // that file as it was just before it was read
    bool unchanged = false;  // that text is as it was: nothing to draw
    bool missing = false, notAFile = false, tooBig = false;
};

namespace {
// An animated picture is kept in memory to play. Bigger than this it is refused.
constexpr qint64 kMaxMovieBytes = 256ll * 1024 * 1024;
}  // namespace

Session::DrawnFile Session::readDrawn(const SourceDef &def, const QString &fallbackText, const QString &was, const FileStamp &wasStamp,
                                      bool onlyIfChanged, const FileWork::Token &token) {
    DrawnFile out;
    const QVariantMap &set = def.settings;
    const QString path = set.value(def.kind == QLatin1String("image") ? QStringLiteral("path") : QStringLiteral("file")).toString();
    if (!path.isEmpty()) {
        const QFileInfo info(path);
        out.missing = !info.exists();
        out.notAFile = info.exists() && !info.isFile();
    }
    if (def.kind == QLatin1String("image")) {
        if (path.isEmpty() || out.missing || out.notAFile) return out;
        if (MovieAnimator::isAnimated(path)) {
            QFile file(path);
            if (file.size() > kMaxMovieBytes) out.tooBig = true;
            else if (file.open(QIODevice::ReadOnly)) out.movie = file.readAll();
            else out.missing = true;
            return out;
        }
        out.image = QImage(path).convertToFormat(QImage::Format_ARGB32);
        out.missing = out.image.isNull();
        return out;
    }
    if (!path.isEmpty()) {
        // Checked again: the same file, as big and last written when it was, is
        // not read. Taken before the read, so a write during it shows next time.
        out.stamp = fileStamp(path);
        if (onlyIfChanged && out.stamp.unchangedSince(wasStamp)) { out.unchanged = true; return out; }
        out.text = readTextFile(path, set.value(QStringLiteral("lines")).toInt());
        if (onlyIfChanged && out.text == was) { out.unchanged = true; return out; }
    }
    // Waited long enough to find the program closing: its fonts may be gone.
    if (!token.wanted()) return out;
    out.image = renderText(set, path.isEmpty() ? fallbackText : out.text).convertToFormat(QImage::Format_ARGB32);
    return out;
}

void Session::drawAnswered(const std::shared_ptr<Feed> &shared, quint64 serial, const DrawnFile &answer, const SourceDef &def) {
    if (!shared || serial != shared->drawSerial) return;  // asked again since, or gone
    Feed &feed = *shared;
    feed.drawing = false;
    const bool again = std::exchange(feed.drawAgain, false);
    feed.fileStamp = answer.stamp;
    if (!answer.unchanged) {
        const QString path = def.settings.value(def.kind == QLatin1String("image") ? QStringLiteral("path") : QStringLiteral("file")).toString();
        feed.fileText = answer.text;
        if (!answer.movie.isEmpty()) {
            animate(shared, def, answer.movie);
        } else {
            delete feed.animator;  // was animated, is not now
            feed.animator = nullptr;
            feed.settings = def.settings;
            feed.state = answer.tooBig ? tr("failed: %1 is too big to animate").arg(path)
                       : answer.notAFile ? tr("not an ordinary file: %1").arg(path)
                       : answer.missing ? tr("missing: %1").arg(path)
                       : path.isEmpty() && def.kind == QLatin1String("image") ? tr("no file chosen") : tr("live");
            QImage image = answer.image;
            if (image.isNull()) {
                image = QImage(1, 1, QImage::Format_ARGB32);
                image.fill(Qt::transparent);
            }
            setPixels(feed, image);
        }
        emit sourcesChanged();  // its size and how it is doing
    }
    if (again) rebuildPlan();  // settings that changed while this was on its way
}

void Session::drawFeed(const std::shared_ptr<Feed> &shared, const SourceDef &def, bool onlyIfChanged) {
    Feed &feed = *shared;
    const QVariantMap &set = def.settings;
    if (def.kind == QLatin1String("slideshow")) {
        ++feed.drawSerial;
        feed.drawing = false;
        animate(shared, def);
        return;
    }
    const bool image = def.kind == QLatin1String("image"), text = def.kind == QLatin1String("text");
    const QString path = set.value(image ? QStringLiteral("path") : QStringLiteral("file")).toString();
    // Anything that opens a file does it off this thread, where a file that never
    // answers costs only that thread (a rich text's pictures open as it is laid out).
    if (image || (text && (!path.isEmpty() || set.value(QStringLiteral("richText")).toBool()))) {
        if (feed.drawing && feed.drawingSettings == set) return;
        if (feed.drawing && feed.drawingFile == path && !path.isEmpty()) {
            // The same file on its way, however slowly: another thread would wait
            // just the same. Its answer asks again for these newer settings.
            if (!onlyIfChanged) feed.drawAgain = true;
            return;
        }
        const quint64 serial = ++feed.drawSerial;
        feed.drawing = true;
        feed.drawingFile = path;
        feed.drawingSettings = set;
        feed.drawAgain = false;
        if (feed.state.isEmpty()) feed.state = tr("opening");
        // The file as it was when drawn from these settings; none for any other.
        m_fileWork.run([this, weak = std::weak_ptr<Feed>(shared), serial, def, onlyIfChanged, was = feed.fileText,
                        wasStamp = feed.settings == set ? feed.fileStamp : FileStamp(),
                        fallback = text ? set.value(QStringLiteral("text"), tr("Text")).toString() : QString()](const FileWork::Token &token) -> FileWork::Answer {
            auto answer = std::make_shared<const DrawnFile>(readDrawn(def, fallback, was, wasStamp, onlyIfChanged, token));
            return [this, weak, serial, def, answer] { drawAnswered(weak.lock(), serial, *answer, def); };
        });
        return;
    }
    ++feed.drawSerial;
    feed.drawing = false;
    if (feed.animator) {  // was animated, is not now
        delete feed.animator;
        feed.animator = nullptr;
    }
    QImage picture;
    QString state = tr("live");
    if (def.kind == QLatin1String("colour")) {
        // As big as a canvas can be, at most.
        const int w = std::clamp(set.value(QStringLiteral("width"), m_canvasWidth.load()).toInt(), 1, 8192);
        const int h = std::clamp(set.value(QStringLiteral("height"), m_canvasHeight.load()).toInt(), 1, 8192);
        picture = QImage(w, h, QImage::Format_ARGB32);
        picture.fill(QColor(set.value(QStringLiteral("colour"), QStringLiteral("#000000")).toString()));
    } else if (text) {
        feed.fileText.clear();
        feed.fileStamp = {};
        picture = renderText(set, set.value(QStringLiteral("text"), tr("Text")).toString());
    } else {
        picture = QImage(1, 1, QImage::Format_ARGB32);
        picture.fill(Qt::transparent);
        state = tr("unknown kind %1").arg(def.kind);
    }
    feed.settings = set;
    feed.state = state;
    setPixels(feed, picture);
}

std::vector<Session::FeedInfo> Session::feeds() const {
    std::vector<FeedInfo> out;
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    for (const auto &[id, f] : m_feeds) {
        FeedInfo i;
        i.sourceId = id;
        i.kind = f->kind;
        i.state = f->state;
        i.live = f->live.load();
        i.resident = f->uploaded.load();
        if (f->isAudio()) {
            i.live = false;
            for (const auto &[serial, capture] : f->audioCaptures) {
                i.live = i.live || capture.capture->running(AudioCapture::Microphone);
                if (const std::string error = capture.capture->error(); !error.empty()) i.state = QString::fromStdString(error);
            }
            if (i.live) i.state = tr("live");
        } else if (f->capture()) {
            const auto area = f->captureBounds(f->captureWidth(), f->captureHeight());
            i.width = area.width(); i.height = area.height();
            auto direct=f->direct.load();
            i.dmabuf = !direct && f->stream.usingDmaBuf();
            i.frames = direct ? direct->received() : f->captureFrames();
            i.dropped = direct ? direct->dropped() : f->stream.framesDropped();
            if (direct) { i.live=direct->hasPicture(); i.state=i.live ? tr("live (direct)") : QString::fromStdString(direct->error()); }
        } else if (f->isMedia()) {
            i.width = f->media.width();
            i.height = f->media.height();
            i.frames = f->media.framesDecoded();
            i.codec = QString::fromStdString(f->media.codec());
            i.hardwareDecode = f->media.hardware();
            i.network = f->media.network();
            i.refusedSound = QString::fromStdString(f->media.refusedSound());
            if (f->live.load()) i.state = QString::fromStdString(f->media.state());
        } else if (f->isDevice()) {
            i.width = f->device.width();
            i.height = f->device.height();
            i.frames = f->device.framesReceived();
            i.dropped = f->device.framesDropped();
            i.mode = QString::fromStdString(f->device.mode().label());
            i.pictureRate = f->device.pictureRate();
        } else {
            std::lock_guard<std::mutex> pixels(f->pixelsMutex);
            i.width = f->width;
            i.height = f->height;
        }
        out.push_back(i);
    }
    return out;
}

void Session::requestCapturePreview(const QString &sourceId) {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    m_capturePreviewRequest = sourceId;
}

QSize Session::captureSize(const QString &sourceId) const {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    const auto it = m_feeds.find(sourceId);
    if (it == m_feeds.end() || !it->second->capture() || !it->second->live.load() || it->second->captureFrames() == 0) return {};
    return {it->second->captureWidth(), it->second->captureHeight()};
}

QRect Session::captureLayoutArea(const QString &sourceId) const {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    const auto it = m_feeds.find(sourceId);
    return it == m_feeds.end() ? QRect() : it->second->layoutArea;
}

bool Session::feedSize(const QString &sourceId, int *width, int *height) const {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    auto it = m_feeds.find(sourceId);
    if (it == m_feeds.end()) return false;
    const Feed &f = *it->second;
    int w = 0, h = 0;
    if (f.capture()) {
        if (f.captureFrames() == 0) return false;
        w = f.captureWidth(); h = f.captureHeight();
    } else if (f.isDevice()) {
        if (f.device.framesReceived() == 0) return false;
        w = f.device.width();
        h = f.device.height();
    } else if (f.isMedia()) {
        if (f.media.framesDecoded() == 0) return false;
        w = f.media.width();
        h = f.media.height();
    } else if (f.isNested()) {
        w = f.nestedWidth.load();
        h = f.nestedHeight.load();
    } else {
        std::lock_guard<std::mutex> pixels(f.pixelsMutex);
        w = f.width;
        h = f.height;
    }
    if (w <= 0 || h <= 0) return false;
    // In the same order as the picture is made: filters, then the region.
    if (f.kind == QLatin1String("region")) {
        const CaptureRegion region = f.regionAfterFilters(&w, &h);
        const QRect area = region.bounds(w, h);
        w = area.width(); h = area.height();
    } else f.filteredSize(&w, &h);
    *width = w;
    *height = h;
    return true;
}

QSize Session::outputSize(int height) const {
    const int cw = m_canvasWidth.load(), ch = m_canvasHeight.load();
    if (height <= 0 || ch <= 0 || height == ch) return {cw, ch};
    // The canvas's shape at that height, both sides even.
    const int w = std::max(2, int(std::lround(double(cw) * height / ch)) & ~1);
    return {w, std::max(2, height & ~1)};
}

void Session::setCanvasSize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    m_canvasAuto.store(false);
    m_canvasWidth.store(width & ~1);
    m_canvasHeight.store(height & ~1);
    m_canvasDirty.store(true);
    emit canvasChanged();
    refitEncoders();
}

// --------------------------------------------------------------- recording

void Session::applyColourLevels() {
    m_sdrWhite.store(float(m_profile.sdrWhite));
    m_hdrPeak.store(float(m_profile.hdrPeak));
}

QString Session::colourProblem(const EncoderInfo &encoder, engine::ColourFormat *format, const Profile *profile) const {
    using F = engine::ColourFormat;
    const Profile &p = profile ? *profile : m_profile;
    const engine::OutputColour wanted = p.outputColour(F::NV12);
    const bool hdr = engine::isHdr(wanted.space);
    F chosen;
    if (p.fixedColourFormat(&chosen)) {
        if (hdr && engine::formatBits(chosen) < 10)
            return tr("HDR needs 10 or more bits: choose P010, I010, I210, P216 or P416 as the colour format, or an SDR colour space.");
        if (!encoderTakes(encoder, chosen)) {
            return tr("Can't use %1. Set Colour format to Automatic in Settings → Video.").arg(QString::fromLatin1(colourFormatName(chosen)));
        }
    } else if (!hdr) {
        if (!colourFormatOf(encoder.swFormat, &chosen)) chosen = F::NV12;
    } else {
        // Its usual layout's 10-bit kin first: P010 beside NV12, I010 beside I420.
        const bool planar = encoder.swFormat == AV_PIX_FMT_YUV420P;
        const F order[] = {planar ? F::I010 : F::P010, planar ? F::P010 : F::I010, F::I210, F::P216, F::P416};
        bool found = false;
        for (F f : order)
            if (encoderTakes(encoder, f)) { chosen = f; found = true; break; }
        if (!found) return tr("Can't make the 10-bit pictures HDR needs. Choose an HEVC or AV1 encoder, or an SDR colour space.");
    }
    if (format) *format = chosen;
    return {};
}

EncodedStream::Settings Session::recordingEncoding(const EncoderInfo &encoder) const {
    const EncodingChoice &c = m_profile.recordingChoice;
    EncodedStream::Settings s;
    s.encoder = encoder;
    engine::ColourFormat format = engine::ColourFormat::NV12;
    colourProblem(encoder, &format);
    s.encoder.swFormat = pixelFormatOf(format);
    s.colour = m_profile.outputColour(format);
    const QSize size = outputSize(m_profile.recordingHeight);
    s.width = size.width();
    s.height = size.height();
    s.fpsNum = runFpsNum();
    s.fpsDen = runFpsDen();
    using M = VideoRate::Mode;
    s.video.mode = c.rateControl == QLatin1String("cbr")   ? M::Cbr
                 : c.rateControl == QLatin1String("vbr")   ? M::Vbr
                 : c.rateControl == QLatin1String("cqp")   ? M::Cqp
                 : c.rateControl == QLatin1String("lossless") ? M::Lossless
                                                           : M::Quality;
    s.video.quality = m_profile.quality;
    s.video.bitrateKbps = m_profile.bitrateKbps > 0 ? m_profile.bitrateKbps : 6000;
    s.video.maxKbps = c.maxKbps;
    s.video.bufferKbps = c.bufferKbps;
    s.video.keyframeSeconds = c.keyframeSeconds;
    s.video.bFrames = c.bFrames;
    s.video.lookahead = c.lookahead;
    s.video.splitEncode = c.splitEncode.toStdString();
    s.video.preset = c.preset.toStdString();
    s.video.profile = c.profile.toStdString();
    s.video.tune = c.tune.toStdString();
    s.video.options = c.options.toStdString();
    s.audioTracks = m_profile.recordingTracks & 63;
    s.audioCodec = audioCodecFromName(c.audioCodec.toStdString());
    s.audioBitrateKbps = m_profile.audioBitrateKbps;
    s.sampleRate = m_mixer.sampleRate();
    s.channels = m_mixer.channels();
    return s;
}

namespace {
// A path in the folder that is not taken: a taken name gets a number.
QString freePathIn(const QString &folder, const QString &base, const QString &extension, bool noSpaces) {
    const QString number = noSpaces ? QStringLiteral("_(%1).") : QStringLiteral(" (%1).");
    QString path = folder + QLatin1Char('/') + base + QLatin1Char('.') + extension;
    for (int n = 2; QFileInfo::exists(path); ++n) path = folder + QLatin1Char('/') + base + number.arg(n) + extension;
    return path;
}
}  // namespace

QString Session::containerRefusal(const EncoderInfo &encoder, const std::string &container) {
    // The codec as people know it: the part of the label before its device.
    QString codec = QString::fromStdString(encoder.label).section(QStringLiteral(" \u00b7 "), 0, 0).trimmed();
    if (codec.isEmpty()) codec = QString::fromStdString(encoder.codec).toUpper();
    QStringList fits;
    for (const char *id : {"mkv", "mp4", "mov"})
        if (id != container && containerCarries(id, encoder)) fits << QString::fromUtf8(fileFormat(id).label);
    const QString format = QString::fromUtf8(fileFormat(container).label);
    if (fits.isEmpty())
        return tr("%1 cannot be recorded in a %2 file. Choose another encoder in Settings \u2192 Recording.").arg(codec, format);
    const QString choices = fits.size() == 1 ? fits.front()
        : tr("%1 or %2").arg(fits.mid(0, fits.size() - 1).join(QStringLiteral(", ")), fits.back());
    return tr("%1 cannot be recorded in a %2 file. Choose %3 for Format in Settings \u2192 Recording, or choose another encoder.")
        .arg(codec, format, choices);
}

std::shared_ptr<EncodedStream> Session::acquireRecordingEncoder(bool *shared, QString *error) {
    if (m_probing.load()) {
        if (error) *error = stillChecking();
        return nullptr;
    }
    // A chosen encoder is tried alone; "best available" walks the ranking,
    // which already leaves out encoders proven unable to take this canvas.
    std::vector<EncoderInfo> tries;
    const EncoderInfo chosen = m_profile.encoder.isEmpty()
                                   ? EncoderInfo{}
                                   : findEncoder(m_encoders, m_profile.encoder.toStdString());
    if (!chosen.name.empty()) {
        // A chosen encoder whose codec the format cannot hold says so plainly,
        // before anything starts (the muxer would only say "Invalid argument").
        const std::string container = m_profile.recordingFormat();
        if (!containerCarries(container, chosen)) {
            if (error) *error = containerRefusal(chosen, container);
            return nullptr;
        }
        tries.push_back(chosen);
    } else {
        for (const RankedEncoder &r : recordingEncoders()) tries.push_back(r.encoder);
    }
    if (tries.empty()) {
        if (error) *error = engine::isHdr(m_profile.outputColour(engine::ColourFormat::NV12).space)
            ? tr("No video encoder here takes HDR's 10-bit pictures at this canvas size and container.")
            : tr("No video encoder works here at this canvas size, container and colour format.");
        return nullptr;
    }
    const auto opened = opening();
    // The fit check may not have run for this canvas yet, so an encoder can
    // still refuse here: fall through to the next.
    QStringList refusals;
    for (const EncoderInfo &e : tries) {
        if (const QString why = colourProblem(e); !why.isEmpty()) {
            // A chosen encoder that cannot take the colour says only why.
            if (tries.size() == 1) { if (error) *error = why; return nullptr; }
            refusals << QStringLiteral("%1: %2").arg(QString::fromStdString(e.label), why);
            continue;
        }
        std::string err;
        if (auto stream = m_encodings.acquire(recordingEncoding(e), &err, shared)) return stream;
        refusals << QStringLiteral("%1: %2").arg(QString::fromStdString(e.label), QString::fromStdString(err));
    }
    const QSize size = outputSize(m_profile.recordingHeight);
    if (error)
        *error = tr("No encoder would record at %1 × %2 (%3)")
                     .arg(size.width()).arg(size.height()).arg(refusals.join(QStringLiteral("; ")));
    return nullptr;
}

void Session::pauseFit() {
    if (!m_fitting.load() || !m_fitProcess) return;
    m_refitPending = true;
    // Its end comes back at once, inside the wait below, before the output
    // exists: it must not start the next fit then.
    m_refitAgain = false;
    m_fitProcess->kill();
    m_fitProcess->waitForFinished(1000);  // its GPU work over before the output opens its encoder
}

void Session::resumeFit() {
    if (!m_refitPending || recording() || streaming() || m_replay) return;
    m_refitPending = false;
    refitEncoders();
}

bool Session::startRecording(QString *error) {
    pauseFit();
    const auto resume = qScopeGuard([this] { resumeFit(); });  // only runs it if nothing is live after all
    if (m_recording) {
        if (!m_recording->stopping) return true;
        if (error) *error = tr("Finishing the previous recording.");
        return false;
    }
    const QString folder = m_profile.resolvedFolder();
    if (!QDir().mkpath(folder)) {
        if (error) *error = tr("Cannot create %1").arg(folder);
        return false;
    }
    if (const qint64 free = freeSpace(folder); free >= 0 && free < kMinFreeToStart) {
        if (error) *error = tr("Only %1 free in %2: make some room or choose another folder.").arg(sizeText(free), folder);
        return false;
    }
    if (const QString why = customMuxerProblem(); !why.isEmpty()) {
        if (error) *error = why;
        return false;
    }
    const FileFormat format = fileFormat(m_profile.recordingFormat());
    const QString extension = QString::fromLatin1(format.extension);
    const QString name = expandFileName(m_profile.filenameTemplate, QDateTime::currentSecsSinceEpoch(),
                                        fileNameFields(), m_profile.filenameNoSpaces);
    // Never overwrite: a taken name gets a number.
    const auto freePath = [folder, extension, noSpaces = m_profile.filenameNoSpaces](const QString &base) {
        return freePathIn(folder, base, extension, noSpaces);
    };
    const QString path = freePath(name);

    bool shared = false;
    std::shared_ptr<EncodedStream> stream = acquireRecordingEncoder(&shared, error);
    if (!stream) return false;

    FileSink::Settings fs;
    fs.path = path.toStdString();
    fs.format = format.id;
    fs.options = m_profile.containerOptions.toStdString();
    fs.splitSeconds = int64_t(m_profile.splitMinutes) * 60;
    fs.splitBytes = int64_t(m_profile.splitMegabytes) * 1024 * 1024;
    // Split files share the first one's name and count on from it.
    const QString splitBase = QFileInfo(path).completeBaseName();
    fs.nextPath = [freePath, splitBase, noSpaces = m_profile.filenameNoSpaces](int n) {
        return freePath(splitBase + (noSpaces ? QStringLiteral("_part%1") : QStringLiteral(" part %1")).arg(n)).toStdString();
    };
    // Joining a running encoder: from its next keyframe on.
    fs.startNs = shared ? monotonicNs() - stream->startTimeNs() : 0;
    auto file = std::make_shared<FileSink>();
    auto recording = std::make_unique<Recording>();
    const quint64 serial = recording->serial = ++m_outputSerial;
    recording->lateAtStart = recording->lateAtStop = lateTicks();
    recording->droppedAtStart = stream->stats().framesDropped;
    std::weak_ptr<EncodedStream> weak = stream;
    file->wantKeyframe = [weak](int64_t at) { if (auto s = weak.lock()) s->requestKeyframe(at); };
    file->onError = [this, serial](const std::string &message) {
        QMetaObject::invokeMethod(this, [this, serial, message] {
            if (!m_recording || m_recording->serial != serial) return;
            m_recording->failed = true;
            // While it closes too (the disk full at the very end): said, not "Saved".
            emit recordingFailed(QString::fromStdString(message));
            if (!m_recording->stopping) stopRecording(false);
        }, Qt::QueuedConnection);
    };
    file->onWarning = [this](const std::string &message) {
        const QString text = tr("The recording carries on in the same file: %1").arg(QString::fromStdString(message));
        QMetaObject::invokeMethod(this, [this, text] { emit recordingWarning(text); }, Qt::QueuedConnection);
    };
    file->onFileClosed = [this, serial](const std::string &closed) {
        QMetaObject::invokeMethod(this, [this, serial, closed] {
            // Once finished, the list is the file's own.
            if (m_recording && m_recording->serial == serial) m_recordingFiles << QString::fromStdString(closed);
        }, Qt::QueuedConnection);
    };
    file->onFinished = [this, serial] {
        QMetaObject::invokeMethod(this, [this, serial] { finishRecording(serial); }, Qt::QueuedConnection);
    };
    std::string err;
    if (!file->start(fs, stream->layout(), &err)) {
        m_encodings.release(stream);
        if (error) *error = QString::fromStdString(err);
        return false;
    }
    stream->attach(file, fs.startNs);
    recording->stream = stream;
    recording->file = file;
    m_recording = std::move(recording);
    m_recordingEncoder = QString::fromStdString(stream->settings().encoder.label);
    m_recordingPath = path;
    m_recordingFiles.clear();
    m_lastRecordingStats = {};
    m_diskWarned = false;
    m_diskGuard.start();
    emit recordingChanged();
    return true;
}

void Session::stopRecording(bool wait) {
    if (!m_recording) return;
    if (!m_recording->stopping) {
        m_recording->stopping = true;
        m_recording->lateAtStop = lateTicks();
        // The pictures handed over so far, the sound to the end of the last.
        const int64_t end = m_encodings.pictureEnd(m_recording->stream) - m_recording->stream->startTimeNs();
        m_recording->file->finishAt(end);
        m_encodings.release(m_recording->stream);
        emit recordingChanged();
    }
    if (wait) {
        m_recording->file->join();
        finishRecording(m_recording->serial);
    }
}

uint64_t Session::lateTicks() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats.lateTicks;
}

void Session::finishRecording(quint64 serial) {
    if (!m_recording || m_recording->serial != serial) return;
    m_recording->file->join();
    m_recording->stream->detach(m_recording->file.get());
    // Ended without being stopped: its encoder gave up (or the file did, and
    // said so first). Its encoder is still held.
    if (!m_recording->stopping) {
        m_recording->stopping = true;
        m_recording->failed = true;
        m_recording->lateAtStop = lateTicks();
        m_encodings.release(m_recording->stream);
        const std::string why = m_recording->stream->failure();
        emit recordingFailed(why.empty() ? tr("The recording stopped by itself.") : QString::fromStdString(why));
    }
    const FileSink::Stats f = m_recording->file->stats();
    const EncodedStream::Stats e = m_recording->stream->stats();
    m_lastRecordingStats = {e.framesEncoded, e.framesDropped, f.bytes, f.durationNs, e.encodeMs, e.gpuFrames};
    // Pictures the file should hold and does not: never drawn in time (the
    // processor or the graphics card busy, often with another program), or
    // drawn and refused by an encoder behind.
    const double wanted = double(f.durationNs) * m_runFpsNum / (m_runFpsDen * 1e9);
    // Counted as a tick ends, so a very slow one begun before the recording adds to it.
    const uint64_t late = std::min<uint64_t>(m_recording->lateAtStop - m_recording->lateAtStart, uint64_t(wanted));
    const uint64_t dropped = e.framesDropped - m_recording->droppedAtStart;
    const bool failed = m_recording->failed;
    QStringList files;
    for (const std::string &closed : m_recording->file->closedFiles()) files << QString::fromStdString(closed);
    m_recordingFiles = files;
    const bool noFile = files.isEmpty();
    if (noFile) m_recordingPath.clear();
    // An MP4 copy of each file, unless it is one already, or holds an editing
    // codec, which editors expect in MOV.
    const QString container = m_profile.container == QLatin1String("custom") ? m_profile.customMuxer : m_profile.container;
    if (m_profile.remuxToMp4 && !isMp4Family(container) && container != QLatin1String("hls") &&
        !isEditingCodec(m_recording->stream->settings().encoder.codec))
        for (const QString &f : files) m_remux.add(f.toStdString());
    // With nothing else on it the encoder is closing already; otherwise it
    // carries on for the outputs still using it.
    m_recording.reset();
    m_diskGuard.stop();
    const QString saved = QFileInfo(m_recordingPath).fileName();  // the signal may start another
    emit recordingChanged();
    if (noFile) {
        emit recordingWarning(tr("Stopped before the first picture, so there is no file."));
        return;
    }
    if (late + dropped == 0) return;
    qWarning("%s: %llu pictures late, %llu dropped by the encoder, of about %.0f.", qPrintable(saved),
             (unsigned long long)late, (unsigned long long)dropped, wanted);
    // A stray late frame is in the log and Stats; more than that is told, unless
    // something worse already has been.
    if (!failed && double(late + dropped) > wanted * kMissingWorthSaying)
        emit recordingWarning(tr("Saved %1, short of %2 pictures of %3: this computer could not keep up. See Stats.")
                                  .arg(saved).arg(late + dropped).arg(qRound64(wanted)));
}

bool Session::setRecordingPaused(bool paused) {
    if (!m_recording || m_recording->stopping) return false;
    const int64_t now = monotonicNs() - m_recording->stream->startTimeNs();
    if (paused) m_recording->file->pause(now);
    else m_recording->file->resume(now);
    emit recordingChanged();
    return true;
}

bool Session::splitRecording() {
    if (!m_recording || m_recording->stopping) return false;
    m_recording->file->split();
    return true;
}

bool Session::addChapter(const QString &name) {
    if (!m_recording || m_recording->stopping || !fileFormat(m_profile.recordingFormat()).chapters) return false;
    m_recording->file->chapter(name.toStdString(), monotonicNs() - m_recording->stream->startTimeNs());
    return true;
}

QStringList Session::recordingFiles() const { return m_recordingFiles; }

// ------------------------------------------------------------ replay buffer

bool Session::startReplay(QString *error, bool byStream) {
    pauseFit();
    const auto resume = qScopeGuard([this] { resumeFit(); });
    if (m_replay) {
        if (!m_replay->stopping) return true;
        if (error) *error = tr("Finishing the previous replay buffer.");
        return false;
    }
    // The recording's encoding, so the two share one encoder.
    bool shared = false;
    std::shared_ptr<EncodedStream> stream = acquireRecordingEncoder(&shared, error);
    if (!stream) return false;
    ReplaySink::Settings rs;
    rs.seconds = m_profile.replaySeconds;
    rs.bytes = int64_t(m_profile.replayMegabytes) * 1024 * 1024;
    rs.startNs = shared ? monotonicNs() - stream->startTimeNs() : 0;
    auto sink = std::make_shared<ReplaySink>();
    auto replay = std::make_unique<Replay>();
    const quint64 serial = replay->serial = ++m_outputSerial;
    std::weak_ptr<EncodedStream> weak = stream;
    sink->wantKeyframe = [weak](int64_t at) { if (auto s = weak.lock()) s->requestKeyframe(at); };
    sink->onFinished = [this, serial] {
        QMetaObject::invokeMethod(this, [this, serial] { finishReplay(serial); }, Qt::QueuedConnection);
    };
    sink->start(rs, stream->layout());
    stream->attach(sink, rs.startNs);
    replay->stream = stream;
    replay->sink = sink;
    replay->startedByStream = byStream;
    m_replay = std::move(replay);
    emit replayChanged();
    return true;
}

void Session::stopReplay(bool wait) {
    if (!m_replay) return;
    if (!m_replay->stopping) {
        m_replay->stopping = true;
        // Detached at once: nothing it holds is wanted past this instant, and
        // saves under way end with what they have.
        m_replay->stream->detach(m_replay->sink.get());
        m_replay->sink->end();
        m_encodings.release(m_replay->stream);
        emit replayChanged();
    }
    if (wait) {
        m_replay->sink->join();
        finishReplay(m_replay->serial);
    }
}

void Session::finishReplay(quint64 serial) {
    if (!m_replay || m_replay->serial != serial) return;
    m_replay->sink->join();
    m_replay->stream->detach(m_replay->sink.get());
    // Given up rather than stopped: its encoder is still held.
    if (!m_replay->stopping) {
        m_encodings.release(m_replay->stream);
        const std::string why = m_replay->stream->failure();
        emit replayFailed(why.empty() ? tr("The replay buffer stopped by itself.") : QString::fromStdString(why));
    }
    m_replay.reset();
    emit replayChanged();
}

bool Session::saveReplay(QString *error) {
    if (!m_replay || m_replay->stopping) {
        if (error) *error = tr("The replay buffer is not running.");
        return false;
    }
    const QString folder = m_profile.resolvedFolder();
    if (!QDir().mkpath(folder)) {
        if (error) *error = tr("Cannot create %1").arg(folder);
        return false;
    }
    const qint64 needed = qint64(m_replay->sink->stats().heldBytes) + 64ll * 1024 * 1024;
    if (const qint64 free = freeSpace(folder); free >= 0 && free < needed) {
        if (error) *error = tr("Only %1 free in %2; the replay needs about %3.").arg(sizeText(free), folder, sizeText(needed));
        return false;
    }
    if (const QString why = customMuxerProblem(); !why.isEmpty()) {
        if (error) *error = why;
        return false;
    }
    const FileFormat format = fileFormat(m_profile.recordingFormat());
    const QString name = expandFileName(m_profile.replayPrefix + m_profile.filenameTemplate + m_profile.replaySuffix,
                                        QDateTime::currentSecsSinceEpoch(), fileNameFields(), m_profile.filenameNoSpaces);
    FileSink::Settings fs;
    fs.path = freePathIn(folder, name, QString::fromLatin1(format.extension), m_profile.filenameNoSpaces).toStdString();
    fs.format = format.id;
    fs.options = m_profile.containerOptions.toStdString();
    // Up to the end of the last picture handed over: what was on screen when asked.
    const int64_t at = m_encodings.pictureEnd(m_replay->stream) - m_replay->stream->startTimeNs();
    m_replay->sink->save(fs, at, [this](const std::string &path, const std::string &why) {
        QMetaObject::invokeMethod(this, [this, path, why] {
            if (why.empty()) {
                m_lastReplayPath = QString::fromStdString(path);
                emit replaySaved(m_lastReplayPath);
            } else {
                emit replayFailed(QString::fromStdString(why));
            }
            emit replayChanged();
        }, Qt::QueuedConnection);
    });
    emit replayChanged();
    return true;
}

// --------------------------------------------------------------- disk space

qint64 Session::freeSpace(const QString &folder) const {
    if (m_freeSpaceProbe) return m_freeSpaceProbe(folder);
    QStorageInfo disk(folder);
    disk.refresh();
    return disk.isValid() && disk.isReady() ? disk.bytesAvailable() : -1;
}

void Session::setFreeSpaceProbe(std::function<qint64(const QString &)> probe) { m_freeSpaceProbe = std::move(probe); }

// While recording: stop while the file can still be closed properly, and
// say so well before. "Soon" is measured in the recording's own rate.
void Session::checkDiskSpace() {
    if (!m_recording || m_recording->stopping) return;
    // The disk it is writing to: the folder in Settings can change meanwhile.
    const QString folder = QFileInfo(m_recordingPath).absolutePath();
    const qint64 free = freeSpace(folder);
    if (free < 0) return;
    const FileSink::Stats f = m_recording->file->stats();
    const double rate = f.durationNs > 2'000'000'000ll ? f.bytes / (f.durationNs / 1e9) : 0;
    const qint64 stopAt = std::max(kStopFloor, qint64(rate * kStopSeconds));
    const qint64 warnAt = std::max(kWarnFloor, qint64(rate * kWarnSeconds));
    if (free < stopAt) {
        emit recordingFailed(tr("The disk is nearly full (%1 left in %2), so the recording was stopped and saved.").arg(sizeText(free), folder));
        stopRecording(false);
    } else if (free < warnAt && !m_diskWarned) {
        m_diskWarned = true;
        const QString left = rate > 0 ? tr("about %n min of recording", nullptr, std::max(1, int(free / rate / 60.0)))
                                      : sizeText(free);
        emit diskSpaceLow(tr("Disk space is running low: %1 left in %2.").arg(left, folder));
    }
}

// -------------------------------------------------------------------- remux

QString Session::customMuxerProblem() const {
    if (m_profile.container != QLatin1String("custom")) return {};
    if (m_profile.customMuxer.isEmpty()) return tr("Name the FFmpeg container to record into.");
    return QString::fromStdString(muxerProblem(m_profile.customMuxer.toStdString()));
}

bool Session::isMp4Family(const QString &format) {
    static const QStringList mp4{QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("fmp4"), QStringLiteral("fmov"),
                                 QStringLiteral("hmp4"), QStringLiteral("hmov")};
    return mp4.contains(format);
}

void Session::wireRemux() {
    // Progress comes often; the window hears at most once per event-loop turn.
    m_remux.onChanged = [this](const RemuxQueue::Job &job) {
        if (job.state == RemuxQueue::State::Done || job.state == RemuxQueue::State::Failed) {
            const QString in = QString::fromStdString(job.in), out = QString::fromStdString(job.out),
                          error = QString::fromStdString(job.error), note = QString::fromStdString(job.note);
            QMetaObject::invokeMethod(this, [this, in, out, error, note] { emit remuxFinished(in, out, error, note); }, Qt::QueuedConnection);
        }
        if (m_remuxNotifyPending.exchange(true)) return;
        QMetaObject::invokeMethod(this, [this] {
            m_remuxNotifyPending.store(false);
            emit remuxChanged();
        }, Qt::QueuedConnection);
    };
}

// ----------------------------------------------------------- virtual camera

bool Session::startVirtualCamera(QString *error) {
    if (m_camera.running()) return true;
    VirtualCamera::Settings cs;
    cs.width = m_canvasWidth.load();
    cs.height = m_canvasHeight.load();
    cs.fpsNum = runFpsNum();
    cs.fpsDen = runFpsDen();
    m_camera.onStateChanged = [this] {
        QMetaObject::invokeMethod(this, [this] { emit virtualCameraChanged(); }, Qt::QueuedConnection);
    };
    std::string err;
    if (!m_camera.start(cs, &err)) {
        if (error) *error = tr("The virtual camera did not start: %1").arg(QString::fromStdString(err));
        return false;
    }
    rebuildPlan();
    emit virtualCameraChanged();
    return true;
}

void Session::stopVirtualCamera() {
    if (!m_camera.running()) return;
    m_camera.stop();
    rebuildPlan();
    emit virtualCameraChanged();
}

void Session::setVirtualCameraShows(const QString &shows, const QString &target) {
    const bool own = shows == QLatin1String("studio") || shows == QLatin1String("source");
    m_profile.cameraShows = own || shows == QLatin1String("preview") ? shows : QStringLiteral("program");
    m_profile.cameraTarget = own ? target : QString();
    rebuildPlan();
    emit virtualCameraChanged();
}

bool Session::virtualCameraShowsOwn() const {
    std::lock_guard<std::mutex> lock(m_planMutex);
    return m_published->cameraOwn;
}

Session::ReplayStatus Session::replayStatus() const {
    ReplayStatus s;
    if (!m_replay) return s;
    const ReplaySink::Stats st = m_replay->sink->stats();
    s.heldNs = st.heldNs;
    s.heldBytes = st.heldBytes;
    s.saving = st.saving;
    s.encoder = QString::fromStdString(m_replay->stream->settings().encoder.label);
    return s;
}

// ---------------------------------------------------------------- streaming

namespace {
VideoRate::Mode rateMode(const QString &name) {
    using M = VideoRate::Mode;
    return name == QLatin1String("quality") ? M::Quality : name == QLatin1String("vbr") ? M::Vbr
         : name == QLatin1String("cqp") ? M::Cqp : name == QLatin1String("lossless") ? M::Lossless : M::Cbr;
}

QString stateName(StreamSink::State state) {
    switch (state) {
    case StreamSink::State::Connecting: return QStringLiteral("connecting");
    case StreamSink::State::Live: return QStringLiteral("live");
    case StreamSink::State::Reconnecting: return QStringLiteral("reconnecting");
    case StreamSink::State::Stopped: return QStringLiteral("stopped");
    case StreamSink::State::Failed: return QStringLiteral("failed");
    }
    return {};
}

QString targetName(const StreamTarget &t) {
    if (!t.name.isEmpty()) return t.name;
    if (const StreamService *s = streamService(t.service.toStdString())) return QString::fromStdString(s->name);
    return Session::tr("Custom server");
}

std::string targetServer(const StreamTarget &t) {
    if (!t.server.isEmpty()) return t.server.toStdString();
    const StreamService *s = streamService(t.service.toStdString());
    return s && !s->servers.empty() ? s->servers.front().url : std::string();
}
}  // namespace

std::vector<RankedEncoder> Session::streamEncoders(const StreamTarget &target) const {
    if (m_probing.load()) return {};
    StreamSink::Protocol protocol = StreamSink::Protocol::Rtmp;
    if (!target.muxer.isEmpty()) protocol = StreamSink::Protocol::Custom;
    else StreamSink::protocolFor(targetServer(target), &protocol);
    const std::string muxer = target.muxer.toStdString();
    const StreamService *service = streamService(target.service.toStdString());
    std::vector<RankedEncoder> ranked;
    {
        std::lock_guard<std::mutex> lock(m_encoderMutex);
        ranked = rankEncoders(m_encoders, Purpose::Stream, {}, m_engineDevice, m_unfit);
    }
    std::erase_if(ranked, [&](const RankedEncoder &r) {
        const AVCodecID video = r.encoder.codec == "av1" ? AV_CODEC_ID_AV1 : r.encoder.codec == "hevc" ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
        if (!StreamSink::carries(protocol, video, AV_CODEC_ID_NONE, nullptr, muxer)) return true;
        if (!colourProblem(r.encoder).isEmpty()) return true;
        return service && !service->videoCodecs.empty() &&
               std::find(service->videoCodecs.begin(), service->videoCodecs.end(), r.encoder.codec) == service->videoCodecs.end();
    });
    return ranked;
}

StreamTarget Session::keptToService(const StreamTarget &target) const {
    StreamTarget t = target;
    const StreamService *service = streamService(t.service.toStdString());
    if (!t.enforce || !service) return t;
    if (service->maxBitrateKbps > 0) t.bitrateKbps = std::min(t.bitrateKbps, service->maxBitrateKbps);
    if (service->keyframeSeconds > 0 && (t.choice.keyframeSeconds <= 0 || t.choice.keyframeSeconds > service->keyframeSeconds))
        t.choice.keyframeSeconds = service->keyframeSeconds;
    if (service->maxWidth > 0 && service->maxHeight > 0) {
        // The tallest size in the canvas's shape that fits.
        const QSize size = outputSize(t.outputHeight);
        if (size.width() > service->maxWidth || size.height() > service->maxHeight) {
            int h = std::min(size.height(), service->maxHeight) & ~1;
            while (h > 2 && outputSize(h).width() > service->maxWidth) h -= 2;
            t.outputHeight = h;
        }
    }
    return t;
}

EncodedStream::Settings Session::streamEncoding(const StreamTarget &target, const EncoderInfo &encoder,
                                                StreamSink::Protocol protocol) const {
    const EncodingChoice &c = target.choice;
    EncodedStream::Settings s;
    s.encoder = encoder;
    engine::ColourFormat format = engine::ColourFormat::NV12;
    colourProblem(encoder, &format);
    s.encoder.swFormat = pixelFormatOf(format);
    s.colour = m_profile.outputColour(format);
    const QSize size = outputSize(target.outputHeight);
    s.width = size.width();
    s.height = size.height();
    s.fpsNum = runFpsNum();
    s.fpsDen = runFpsDen();
    s.video.mode = rateMode(c.rateControl);
    s.video.quality = m_profile.quality;
    s.video.bitrateKbps = target.bitrateKbps;
    s.video.maxKbps = c.maxKbps;
    s.video.bufferKbps = c.bufferKbps;
    s.video.keyframeSeconds = c.keyframeSeconds;
    s.video.bFrames = c.bFrames;
    s.video.lookahead = c.lookahead;
    s.video.splitEncode = c.splitEncode.toStdString();
    s.video.preset = c.preset.toStdString();
    s.video.profile = c.profile.toStdString();
    s.video.tune = c.tune.toStdString();
    s.video.options = c.options.toStdString();
    s.audioTracks = 1u << std::clamp(target.track, 0, 5);
    s.audioBitrateKbps = target.audioBitrateKbps;
    s.sampleRate = m_mixer.sampleRate();
    // Services take stereo: a surround mix is folded down for them.
    s.channels = std::min(m_mixer.channels(), 2);
    s.sourceChannels = m_mixer.channels();
    // What the protocol takes: RTMP AAC, WebRTC Opus without B-frames,
    // MPEG-TS either.
    const AudioCodec chosen = audioCodecFromName(c.audioCodec.toStdString());
    if (protocol == StreamSink::Protocol::Whip) {
        s.audioCodec = AudioCodec::Opus;
        s.video.bFrames = 0;
        if (s.video.profile.empty()) s.video.profile = encoder.name == "libx264" || encoder.name.find("nvenc") != std::string::npos
                                                           ? "baseline" : "constrained_baseline";
    } else if (protocol == StreamSink::Protocol::Rtmp) {
        s.audioCodec = AudioCodec::Aac;
    } else if (protocol == StreamSink::Protocol::Custom) {
        // The one asked for where the container takes it, else the other.
        const std::string format = "custom:" + target.muxer.toStdString();
        s.audioCodec = chosen == AudioCodec::Opus ? AudioCodec::Opus : AudioCodec::Aac;
        if (!formatCarries(format, s.audioCodec == AudioCodec::Opus ? AV_CODEC_ID_OPUS : AV_CODEC_ID_AAC))
            s.audioCodec = s.audioCodec == AudioCodec::Opus ? AudioCodec::Aac : AudioCodec::Opus;
    } else {
        s.audioCodec = chosen == AudioCodec::Opus ? AudioCodec::Opus : AudioCodec::Aac;
    }
    return s;
}

bool Session::startStream(const StreamTarget &given, const QString &key, const QString &password, QString *error) {
    pauseFit();
    const auto resume = qScopeGuard([this] { resumeFit(); });
    const StreamTarget target = keptToService(given);
    const std::string server = targetServer(target);
    if (server.empty()) {
        if (error) *error = tr("no server is set");
        return false;
    }
    // Sent without one, the service only refuses the connection, which reads as
    // a network problem. A custom server may well take none.
    if (key.isEmpty() && target.muxer.isEmpty() && streamService(target.service.toStdString())) {
        if (error) *error = tr("it has no stream key: add one in Settings, or start the keyring if it is kept there");
        return false;
    }
    if (target.hasPassword && password.isEmpty()) {
        if (error) *error = tr("its password could not be read: enter it again in Settings, or start the keyring if it is kept there");
        return false;
    }
    StreamSink::Protocol protocol = StreamSink::Protocol::Custom;
    if (!target.muxer.isEmpty()) {
        if (const std::string why = muxerProblem(target.muxer.toStdString()); !why.empty()) {
            if (error) *error = QString::fromStdString(why);
            return false;
        }
    } else if (!StreamSink::protocolFor(server, &protocol)) {
        if (error) *error = tr("the server should start with rtmp://, rtmps://, srt:// or https://, "
                               "or name an FFmpeg container to send in");
        return false;
    }
    // The delay is held in memory: one that could not be is refused now,
    // rather than failing an hour into the show.
    if (target.delaySeconds > 0) {
        const double kbps = std::max(target.bitrateKbps, 1000) + 512;  // the picture, the sound and some room
        const double bytes = kbps * 125.0 * target.delaySeconds;
        if (bytes > 0.8 * double(QueuedSink::kMaxQueuedBytes)) {
            if (error) *error = tr("a %1 s delay at %2 kb/s needs more memory than it may hold; shorten the delay")
                                    .arg(target.delaySeconds).arg(target.bitrateKbps);
            return false;
        }
    }
    std::vector<EncoderInfo> tries;
    EncoderInfo chosen = target.encoder.isEmpty() ? EncoderInfo{} : findEncoder(m_encoders, target.encoder.toStdString());
    // Keeping to the service: one it does not take gives way to the best it does.
    if (const StreamService *service = streamService(target.service.toStdString());
        target.enforce && service && !chosen.name.empty() && !service->videoCodecs.empty() &&
        std::find(service->videoCodecs.begin(), service->videoCodecs.end(), chosen.codec) == service->videoCodecs.end())
        chosen = EncoderInfo{};
    if (!chosen.name.empty()) tries.push_back(chosen);
    else for (const RankedEncoder &r : streamEncoders(target)) tries.push_back(r.encoder);
    if (tries.empty()) {
        if (error) *error = tr("no encoder here makes what it takes");
        return false;
    }
    const auto opened = opening();
    QStringList refusals;
    std::shared_ptr<EncodedStream> stream;
    bool shared = false;
    for (const EncoderInfo &e : tries) {
        if (const QString why = colourProblem(e); !why.isEmpty()) { refusals << QStringLiteral("%1: %2").arg(QString::fromStdString(e.label), why); continue; }
        std::string err;
        stream = m_encodings.acquire(streamEncoding(target, e, protocol), &err, &shared);
        if (stream) break;
        refusals << QStringLiteral("%1: %2").arg(QString::fromStdString(e.label), QString::fromStdString(err));
    }
    if (!stream) {
        if (error) *error = tr("no encoder would start (%1)").arg(refusals.join(QStringLiteral("; ")));
        return false;
    }
    StreamSink::Settings ss;
    ss.url = protocol == StreamSink::Protocol::Whip ? server : streamUrl(server, key.toStdString());
    if (!target.username.isEmpty() || !password.isEmpty()) {
        std::string why;
        ss.url = withLogin(ss.url, target.username.toStdString(), password.toStdString(), &why);
        if (ss.url.empty()) {
            m_encodings.release(stream);
            if (error) *error = tr("the sign-in cannot be used: %1").arg(QString::fromStdString(why));
            return false;
        }
    }
    if (protocol == StreamSink::Protocol::Whip) ss.bearerToken = key.toStdString();
    ss.protocolGiven = true;
    ss.protocol = protocol;
    ss.muxer = target.muxer.toStdString();
    ss.muxerOptions = target.muxerOptions.toStdString();
    ss.bindAddress = target.bindAddress.toStdString();
    ss.clockZeroNs = stream->startTimeNs();
    ss.startNs = shared ? monotonicNs() - stream->startTimeNs() : 0;
    ss.delaySeconds = target.delaySeconds;
    ss.preserveDelay = target.preserveDelay;
    ss.ipFamily = target.ipFamily;
    ss.lowLatency = target.lowLatency;
    ss.reconnect = target.reconnect;
    ss.retryDelaySeconds = target.retryDelaySeconds;
    ss.maxRetries = target.maxRetries;
    ss.dynamicBitrate = target.dynamicBitrate;
    ss.bitrateKbps = target.bitrateKbps;
    auto sink = std::make_shared<StreamSink>();
    auto output = std::make_unique<StreamOutput>();
    const quint64 serial = output->serial = ++m_outputSerial;
    std::weak_ptr<EncodedStream> weak = stream;
    sink->wantKeyframe = [weak](int64_t at) { if (auto s = weak.lock()) s->requestKeyframe(at); };
    sink->onBitrate = [weak](int kbps) { const auto s = weak.lock(); return s && s->setBitrate(kbps); };
    sink->onState = [this, serial](StreamSink::State state) {
        QMetaObject::invokeMethod(this, [this, serial, state] {
            const auto it = std::find_if(m_streams.begin(), m_streams.end(), [&](const auto &o) { return o->serial == serial; });
            if (it == m_streams.end()) return;
            if (state == StreamSink::State::Failed)
                emit streamFailed(targetName((*it)->target), QString::fromStdString((*it)->sink->stats().message));
            emit streamingChanged();
        }, Qt::QueuedConnection);
    };
    sink->onFinished = [this, serial] {
        QMetaObject::invokeMethod(this, [this, serial] { finishStream(serial); }, Qt::QueuedConnection);
    };
    std::string err;
    if (!sink->start(ss, stream->layout(), &err)) {
        m_encodings.release(stream);
        if (error) *error = QString::fromStdString(err);
        return false;
    }
    stream->attach(sink, ss.startNs);
    output->target = target;
    output->stream = stream;
    output->sink = sink;
    m_streams.push_back(std::move(output));
    return true;
}

bool Session::startStreaming(const QMap<QString, QString> &keys, QString *error, const QMap<QString, QString> &passwords) {
    if (!m_streams.empty()) {
        if (!streamingStopping()) return true;
        if (error) *error = tr("Finishing the previous stream.");
        return false;
    }
    if (m_probing.load()) {
        if (error) *error = stillChecking();
        return false;
    }
    QStringList problems;
    int started = 0, wanted = 0;
    for (const StreamTarget &target : m_profile.streams) {
        if (!target.enabled) continue;
        ++wanted;
        QString why;
        if (startStream(target, keys.value(target.id, target.key), passwords.value(target.id, target.password), &why)) ++started;
        else problems << QStringLiteral("%1: %2").arg(targetName(target), why);
    }
    if (error) *error = wanted == 0 ? tr("No stream is set up: add one in Settings.") : problems.join(QStringLiteral("\n"));
    if (started == 0) return false;
    emit streamingChanged();
    if (m_profile.recordWhileStreaming && !m_recording) {
        QString why;
        if (!startRecording(&why)) emit recordingFailed(why);
    }
    if (m_profile.replayWhileStreaming && !m_replay && !m_safeMode) {
        QString why;
        if (!startReplay(&why, true)) emit replayFailed(why);
    }
    return true;
}

void Session::stopStreaming(bool wait) {
    bool changed = false;
    for (auto &o : m_streams) {
        if (o->stopping) continue;
        o->stopping = true;
        changed = true;
        o->sink->finishAt(m_encodings.pictureEnd(o->stream) - o->stream->startTimeNs());
        m_encodings.release(o->stream);
    }
    if (changed) {
        emit streamingChanged();
        if (m_profile.stopRecordingWithStream && m_recording) stopRecording(false);
        if (m_replay && m_replay->startedByStream) stopReplay(false);
    }
    if (wait) {
        while (!m_streams.empty()) {
            m_streams.front()->sink->join();
            finishStream(m_streams.front()->serial);
        }
    }
}

bool Session::streamingStopping() const {
    return !m_streams.empty() && std::all_of(m_streams.begin(), m_streams.end(), [](const auto &o) { return o->stopping; });
}

void Session::failStream(StreamOutput &o, const QString &message) {
    if (o.stopping) return;
    o.stopping = true;
    m_encodings.release(o.stream);
    emit streamFailed(targetName(o.target), message);
    emit streamingChanged();
}

void Session::finishStream(quint64 serial) {
    const auto it = std::find_if(m_streams.begin(), m_streams.end(), [&](const auto &o) { return o->serial == serial; });
    if (it == m_streams.end()) return;
    StreamOutput &o = **it;
    o.sink->join();
    o.stream->detach(o.sink.get());
    // Given up rather than stopped: its encoder is still held. A sink that
    // failed has said so; one its encoder ended has not.
    if (!o.stopping) {
        m_encodings.release(o.stream);
        if (o.sink->stats().state != StreamSink::State::Failed) {
            const std::string why = o.stream->failure();
            emit streamFailed(targetName(o.target), why.empty() ? tr("the stream stopped by itself")
                                                                : QString::fromStdString(why));
        }
    }
    m_streams.erase(it);
    emit streamingChanged();
    // The last one gone, however it went: a buffer started for it goes with it.
    if (m_streams.empty() && m_replay && m_replay->startedByStream) stopReplay(false);
}

std::vector<Session::StreamStatus> Session::streamStatus() const {
    std::vector<StreamStatus> out;
    for (const auto &o : m_streams) {
        const StreamSink::Stats st = o->sink->stats();
        StreamStatus s;
        s.id = o->target.id;
        s.name = targetName(o->target);
        s.state = o->stopping && st.state != StreamSink::State::Stopped ? QStringLiteral("stopping") : stateName(st.state);
        s.message = QString::fromStdString(st.message);
        s.encoder = QString::fromStdString(o->stream->settings().encoder.label);
        s.protocol = QString::fromLatin1(StreamSink::protocolName(o->sink->protocol()));
        s.kbps = st.kbps;
        s.framesSent = st.framesSent;
        s.framesDropped = st.framesDropped + o->stream->stats().framesDropped;
        s.reconnects = st.reconnects;
        s.lagMs = st.lagMs;
        s.delayMs = st.delayMs;
        s.heldBytes = st.heldBytes;
        s.bitrateKbps = st.bitrateKbps;
        s.bitrateHeld = st.bitrateHeld;
        out.push_back(s);
    }
    return out;
}

// ---------------------------------------------------------------- encoders

void Session::warmRecordingEncoder() {
    if (m_probing.load()) return;
    const EncoderInfo chosen = m_profile.encoder.isEmpty() ? EncoderInfo{} : findEncoder(m_encoders, m_profile.encoder.toStdString());
    if (!chosen.name.empty()) {
        warmHardwareDevice(chosen);
        return;
    }
    if (const std::vector<RankedEncoder> ranked = recordingEncoders(); !ranked.empty()) warmHardwareDevice(ranked.front().encoder);
}

std::vector<RankedEncoder> Session::recordingEncoders(bool evenSlow) const {
    if (m_probing.load()) return {};
    std::vector<RankedEncoder> ranked;
    {
        std::lock_guard<std::mutex> lock(m_encoderMutex);
        std::set<std::string> skip = m_unfit;
        if (!evenSlow) skip.insert(m_slow.begin(), m_slow.end());
        ranked = rankEncoders(m_encoders, Purpose::Record, m_profile.recordingFormat(), m_engineDevice, skip);
    }
    // Only those that take the profile's colour.
    std::erase_if(ranked, [this](const RankedEncoder &r) { return !colourProblem(r.encoder).isEmpty(); });
    // Every graphics card encoder slow: the fastest stays first. What is left
    // would be on the processor, never timed, and at a size that defeats the
    // hardware it is slower still.
    const auto onCard = [](const RankedEncoder &r) { return r.encoder.hardware && !isEditingCodec(r.encoder.codec); };
    if (!evenSlow && std::none_of(ranked.begin(), ranked.end(), onCard)) {
        std::optional<RankedEncoder> fastest;
        for (const RankedEncoder &r : recordingEncoders(true))
            if (onCard(r) && (!fastest || encoderSpeed(encoderKey(r.encoder)) > encoderSpeed(encoderKey(fastest->encoder)))) fastest = r;
        if (fastest) ranked.insert(ranked.begin(), *fastest);
    }
    return ranked;
}

std::set<std::string> Session::unfitEncoders() const {
    std::lock_guard<std::mutex> lock(m_encoderMutex);
    std::set<std::string> all = m_unfit;
    all.insert(m_slow.begin(), m_slow.end());
    return all;
}

std::set<std::string> Session::slowEncoders() const {
    std::lock_guard<std::mutex> lock(m_encoderMutex);
    return m_slow;
}

double Session::encoderSpeed(const std::string &key) const {
    std::lock_guard<std::mutex> lock(m_encoderMutex);
    const auto it = m_encoderFps.find(key);
    return it == m_encoderFps.end() ? 0.0 : it->second;
}

void Session::refitEncoders() {
    if (m_probing.load() || m_encoders.empty()) return;
    // Opening and timing hardware encoders beside a live recording or stream
    // would compete with it; the output already has its encoder. Later, then.
    if (recording() || streaming() || m_replay) {
        m_refitPending = true;
        return;
    }
    if (m_fitting.load()) {
        m_refitAgain = true;
        return;
    }
    m_fitting.store(true);
    // Measured at the size recordings are made; streams at other sizes fall
    // through to the next encoder if one refuses.
    const QSize size = outputSize(m_profile.recordingHeight);
    const int w = size.width(), h = size.height();
    const int fps = std::max(1, m_profile.fpsNum / std::max(1, m_profile.fpsDen));
    m_fitProcess = runHelper(encoderhelper::fitArguments(w, h, fps), encoderhelper::fitInput(m_encoders),
              [this](const QByteArray &output) {
        // No answer from the helper: leave the fit unknown rather than open
        // every encoder here; recording still falls through on refusal.
        encoderhelper::Fit fit;
        if (encoderhelper::readFit(output, &fit)) {
            std::lock_guard<std::mutex> lock(m_encoderMutex);
            m_unfit = fit.unfit;
            m_slow = fit.slow;
            m_encoderFps = fit.fps;
        }
        m_fitting.store(false);
        emit encodersChanged();
        warmRecordingEncoder();
        if (m_refitAgain) {
            m_refitAgain = false;
            refitEncoders();
        }
    });
}

// -------------------------------------------------------------- frame loop

void Session::setPreviewSize(int w, int h) {
    m_previewWidth.store(std::max(16, w));
    m_previewHeight.store(std::max(9, h));
}

double Session::filterCostMs(const QString &sourceId) const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    const auto it = m_filterCosts.find(sourceId);
    return it == m_filterCosts.end() ? -1 : it->second;
}

Session::Stats Session::stats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    Stats s = m_stats;
    s.audioLateFrames = m_mixer.lateFrames();
    s.monitorUnderruns = m_monitor.underrunFrames();
    s.monitorQueuedFrames = m_monitor.queuedFrames();
    s.monitorQuantumFrames = m_monitor.quantumFrames();
    if (m_recording) {
        const FileSink::Stats f = m_recording->file->stats();
        const EncodedStream::Stats e = m_recording->stream->stats();
        s.recorder = {e.framesEncoded, e.framesDropped, f.bytes, f.durationNs, e.encodeMs, e.gpuFrames};
    } else {
        s.recorder = m_lastRecordingStats;
    }
    s.recording = recording();
    s.recordingPath = m_recordingPath;
    for (int c = 0; c < 2; ++c) {
        s.desktopPeak[c] = m_mixer.sourcePeak(AudioCapture::Desktop);
        s.microphonePeak[c] = m_mixer.sourcePeak(AudioCapture::Microphone);
    }
    return s;
}

void Session::audioLoop() {
    auto next = std::chrono::steady_clock::now();
    while (!m_quit.load() && !m_audioQuit.load()) {
        const int64_t now = monotonicNs();
        const auto feeds = m_audioFeeds.load();
        for (const auto &feed : *feeds) {
            if (!feed->live.load()) continue;
            std::lock_guard<std::mutex> transport(feed->mediaControlMutex);
            feed->media.pumpAudio(now, [this, channel = feed->audioChannel](int64_t due, const float *data, int frames) {
                m_mixer.push(channel, due, data, frames, 0, monotonicNs());
            });
        }
        {
            std::lock_guard<std::mutex> lock(m_recordMutex);
            pumpAudio();
        }
        next += std::chrono::milliseconds(2);
        const auto after = std::chrono::steady_clock::now();
        if (next < after) next = after;
        std::this_thread::sleep_until(next);
    }
}

void Session::pumpAudio() {
    m_mixer.advanceTo(monotonicNs());
    float buf[6][96 * 8], monitor[96 * 8];
    AudioMixer::Buses buses;
    // Track 1 always mixes: the monitor and the meters follow it.
    const unsigned wanted = m_encodings.tracksWanted() | 1u;
    for (int t = 0; t < 6; ++t) if (wanted & (1u << t)) buses.tracks[t] = buf[t];
    buses.monitor = monitor;
    int64_t when = 0;
    while (int n = m_mixer.pull(buses, 96, &when)) {
        m_monitor.push(monitor, n);
        m_lastMixedEndNs = when + int64_t(n) * 1'000'000'000 / m_mixer.sampleRate();
        m_encodings.deliverAudio(buses.tracks.data(), n, when, m_mixer.sampleRate(), m_mixer.channels());
        m_encodings.audioReached(m_lastMixedEndNs);
    }
    m_encodings.audioReached(m_lastMixedEndNs);
}

void Session::frameLoop() {
    std::string why;
    m_cameraTarget = m_programFrom = m_programTo = m_programOut = m_programMatte = m_editTarget = m_clearSource = -1;
    for (const auto &t : std::exchange(m_itemTransitionsDrawn, {})) t->target = -1;
    m_viewTargets.clear(); m_viewDrawn.clear();
    m_drawnTransition = 0; m_fromStill = m_outDrawn = false;
    // Which GPU composites: Settings → General → Graphics, read as the loop starts.
    QString engineWhy;
    engine::GpuPick pick;
    if (m_safeMode || m_gpuChoice == QLatin1String("cpu") || qgetenv("OMABROADCAST_GPU") == "cpu") {
        if (!m_safeMode && m_gpuChoice == QLatin1String("cpu")) engineWhy = tr("chosen in Settings");
    } else {
        const std::vector<engine::GpuDevice> gpus = engine::listGpus();
        pick = engine::chooseGpu(gpus, m_gpuChoice == QLatin1String("auto") ? std::string() : m_gpuChoice.toStdString(),
                                 machinesurvey::readPower().onBattery);
        {
            std::lock_guard<std::mutex> lock(m_encoderMutex);
            m_gpusAtStart = gpus;
            m_gpuPickIndex = pick.index;
        }
        if (pick.index >= 0) {
            // On FFmpeg's own device for that GPU (kept, so its Vulkan encoders
            // share it) unless told not to; on a device of its own otherwise.
            AVBufferRef *shared = nullptr;
            if (!qEnvironmentVariableIsSet("OMABROADCAST_NO_ZERO_COPY")) {
                int result = 0;
                if (pick.index < int(gpus.size()))
                    shared = keptHardwareDevice(AV_HWDEVICE_TYPE_VULKAN, gpus[size_t(pick.index)].name, "VK_EXT_queue_family_foreign", &result);
            }
            m_engine = engine::createVulkanEngine(pick.index, &why, shared);
            if (!m_engine && shared) m_engine = engine::createVulkanEngine(pick.index, &why);
            if (shared) av_buffer_unref(&shared);
        }
        switch (pick.why) {
        case engine::GpuPick::Why::Chosen: engineWhy = tr("chosen in Settings"); break;
        case engine::GpuPick::Why::OnBattery: engineWhy = tr("integrated, picked because this machine is on battery"); break;
        case engine::GpuPick::Why::ChosenMissing: engineWhy = tr("the GPU chosen in Settings is not here; picked automatically"); break;
        default: break;
        }
    }
    // A GPU was picked and did not start: the processor draws instead, and the
    // person is told so and why, not that the choice in Settings was followed.
    const bool gpuFailed = pick.index >= 0 && !m_engine;
    if (!m_engine) m_engine = engine::createCpuEngine(gpuFailed ? why : std::string());
    if (gpuFailed) {
        QString gpu;
        {
            std::lock_guard<std::mutex> lock(m_encoderMutex);
            if (pick.index < int(m_gpusAtStart.size())) gpu = QString::fromStdString(m_gpusAtStart[size_t(pick.index)].name);
        }
        if (gpu.isEmpty()) gpu = tr("The GPU");
        const QString reason = QString::fromStdString(m_engine->note()).trimmed();
        engineWhy = reason.isEmpty() ? tr("%1 did not start").arg(gpu) : tr("%1 did not start: %2").arg(gpu, reason);
        qWarning("%s did not start (%s): drawing on the processor.", qPrintable(gpu), why.c_str());
        emit drawingChanged(reason.isEmpty()
                                ? tr("%1 did not start: drawing on the processor, which is slower.").arg(gpu)
                                : tr("%1 did not start (%2): drawing on the processor, which is slower.").arg(gpu, reason));
    }
    // Drawing on another GPU than last time: the FFmpeg devices kept for the
    // others go, so a card left behind can power down. What encoders on it
    // need is made again when they open.
    if (m_engine->isGpu() && m_lastEngineGpu != m_engine->deviceName()) {
        if (!m_lastEngineGpu.empty()) {
            const std::string name = m_engine->deviceName(), node = m_engine->renderNode();
            releaseHardwareDevices([&](AVHWDeviceType type, const std::string &device) {
                return (type == AV_HWDEVICE_TYPE_VULKAN && device == name) || (type == AV_HWDEVICE_TYPE_VAAPI && !node.empty() && device == node);
            });
        }
        m_lastEngineGpu = m_engine->deviceName();
    }
    m_encodings.setGpuDevice(m_engine->sharedDevice());
    m_engine->setCanvasSize(m_canvasWidth.load(), m_canvasHeight.load());
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.engine = QString::fromStdString(m_engine->deviceName());
        m_stats.engineWhy = engineWhy;
        m_stats.engineOnBattery = !gpuFailed && pick.why == engine::GpuPick::Why::OnBattery;
        m_stats.gpu = m_engine->isGpu();
    }
    {
        std::lock_guard<std::mutex> lock(m_encoderMutex);
        m_engineDevice = m_engine->isGpu() ? EngineDevice{m_engine->deviceName(), m_engine->renderNode(),
                                                          pick.why == engine::GpuPick::Why::Chosen ||
                                                              pick.why == engine::GpuPick::Why::OnBattery,
                                                          pick.index >= 0 && pick.index < int(m_gpusAtStart.size())
                                                              ? m_gpusAtStart[size_t(pick.index)].vendorId : 0u}
                                           : EngineDevice{};
    }
    publishEngineFormats();

    const int64_t period = int64_t(1'000'000'000ll) * m_runFpsDen / m_runFpsNum;
    int64_t next = monotonicNs();
    int64_t windowStart = next;
    int ticksInWindow = 0;
    bool shortOfMemory = false;
    int64_t renderNs = 0, renderWorstNs = 0;
    while (!m_quit.load()) {
        const int64_t now = monotonicNs();
        if (now < next) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(next - now));
            continue;
        }
        // No memory for this picture: the last one stands, and whatever is
        // recording stays open, rather than the program ending with it.
        try {
            tick(next);
            shortOfMemory = false;
        } catch (const std::bad_alloc &) {
            if (!std::exchange(shortOfMemory, true)) qWarning("No memory to draw the picture; the last one stays up.");
        }
        ++ticksInWindow;
        next += period;
        const int64_t after = monotonicNs();
        renderNs += after - now;
        renderWorstNs = std::max(renderWorstNs, after - now);
        if (after > next + period) {
            // More than a frame behind: skip ahead rather than pile up. Whole
            // frames, keeping the beat, so the count is the pictures never made.
            const int64_t skipped = (after - next) / period;
            next += skipped * period;
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.lateTicks += uint64_t(skipped);
        }
        if (after - windowStart >= 1'000'000'000ll) {
            std::map<QString, double> costs;
            {
                std::lock_guard<std::mutex> lock(m_sourcesMutex);
                for (const auto &[id, f] : m_feeds)
                    if (f->engineId >= 0) { const double ms = m_engine->filterMs(f->engineId); if (ms >= 0) costs[id] = ms; }
            }
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_filterCosts = std::move(costs);
            m_stats.fps = ticksInWindow * 1e9 / double(after - windowStart);
            m_stats.renderMs = ticksInWindow ? renderNs / 1e6 / ticksInWindow : 0;
            m_stats.renderWorstMs = renderWorstNs / 1e6;
            ticksInWindow = 0;
            renderNs = renderWorstNs = 0;
            windowStart = after;
            QMetaObject::invokeMethod(this, [this] { emit statsChanged(); }, Qt::QueuedConnection);
        }
    }
    // Drop the engine on this thread, where it was made.
    std::vector<std::shared_ptr<Feed>> feeds;
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        for (const auto &[id, f] : m_feeds) feeds.push_back(f);
        for (const auto &[id, f] : m_stingers) feeds.push_back(f);
        for (const auto &[key, copy] : m_copyFeeds) feeds.push_back(copy.feed);
    }
    for (auto &f : feeds) {
        for (int id : {f->engineId, f->slideFrom, f->slideTo, f->slideMix}) if (id >= 0) m_engine->destroySource(id);
        f->engineId = f->slideFrom = f->slideTo = f->slideMix = -1; f->uploadedSlide = 0; f->configuredDirect.reset();
        f->uploaded.store(false); f->hiddenWithoutGpu = false; f->cutOutFailure.store(engine::FilterFailure::None);
        if (!f->capture()) f->dirty.store(true);
    }
    m_encodings.setGpuDevice(nullptr);
    {
        std::lock_guard<std::mutex> lock(m_engineFormatsMutex);
        m_engineMade = false;
        m_engineFormats.clear();
    }
    m_engine.reset();  // waits for the GPU
    dropLastTick();
    m_cameraTarget = m_programFrom = m_programTo = m_programOut = m_programMatte = m_editTarget = m_clearSource = -1;
    for (const auto &t : std::exchange(m_itemTransitionsDrawn, {})) t->target = -1;
    m_viewTargets.clear(); m_viewDrawn.clear();
}

namespace {
// OMABROADCAST_TIMING=1: once a second, where the frame loop's time went.
struct TickTimer {
    static constexpr int kPhases = 6;
    static constexpr const char *kNames[kPhases] = {"sources", "composite", "yuv", "audio", "preview", "finish"};
    bool on = qEnvironmentVariableIsSet("OMABROADCAST_TIMING");
    int64_t mark = 0;
    double sum[kPhases] = {}, worst[kPhases] = {}, total = 0, worstTotal = 0;
    int ticks = 0;
    int64_t windowStart = 0;
    void start(int64_t now) { mark = now; }
    void lap(int phase, double *tickTotal) {
        if (!on) return;
        const int64_t now = monotonicNs();
        const double ms = (now - mark) / 1e6;
        mark = now;
        sum[phase] += ms;
        worst[phase] = std::max(worst[phase], ms);
        *tickTotal += ms;
    }
    void endTick(double tickTotal, uint64_t late) {
        if (!on) return;
        ++ticks;
        total += tickTotal;
        worstTotal = std::max(worstTotal, tickTotal);
        const int64_t now = monotonicNs();
        if (windowStart == 0) windowStart = now;
        if (now - windowStart < 1'000'000'000ll) return;
        QString line = QStringLiteral("tick x%1 avg %2 ms worst %3 ms late %4 |")
                           .arg(ticks).arg(total / ticks, 0, 'f', 2).arg(worstTotal, 0, 'f', 2).arg(late);
        for (int i = 0; i < kPhases; ++i)
            line += QStringLiteral(" %1 %2/%3").arg(QLatin1String(kNames[i])).arg(sum[i] / ticks, 0, 'f', 2)
                        .arg(worst[i], 0, 'f', 2);
        qInfo().noquote() << line;
        *this = TickTimer{};
        windowStart = now;
    }
};
}  // namespace

void Session::publishEngineFormats() {
    std::map<uint32_t, std::vector<uint64_t>> formats;
    if (m_engine->isGpu())
        for (const uint32_t fourcc : VideoStream::fourccs()) formats[fourcc] = m_engine->importModifiers(fourcc);
    {
        std::lock_guard<std::mutex> lock(m_engineFormatsMutex);
        m_engineMade = true;
        m_engineFormats = std::move(formats);
    }
    QMetaObject::invokeMethod(this, &Session::pumpCaptures, Qt::QueuedConnection);
}

void Session::overrideColour(Feed &f) {
    std::lock_guard<std::mutex> lock(f.colourMutex);
    f.picture.overrideColour(f.colourRange, f.colourMatrix);
}

int Session::itemTransitionTarget(const std::shared_ptr<ItemTransition> &t, int source, int width, int height) {
    engine::Engine &eng = *m_engine;
    if (m_clearSource < 0) {
        m_clearSource = eng.createSource();
        static const uint8_t clear[4] = {0, 0, 0, 0};
        eng.updateSource(m_clearSource, engine::CpuFrame{1, 1, 4, engine::PixelFormat::BGRA, clear});
    }
    if (t->target < 0) {
        t->target = eng.createSource();
        m_itemTransitionsDrawn.push_back(t);
    }
    // Once a frame, however many pictures show the item.
    if (t->drawnTick != m_tickSerial) {
        t->drawnTick = m_tickSerial;
        const float progress = float(std::clamp(double(m_tickNs - t->startNs) / double(t->durationNs), 0.0, 1.0));
        if (t->showing) eng.transitionInto(t->target, width, height, m_clearSource, source, t->style, progress);
        else eng.transitionInto(t->target, width, height, source, m_clearSource, t->style, progress);
    }
    return t->target;
}

std::vector<engine::Layer> Session::layersFor(const std::vector<PlanItem> &items) {
    std::vector<engine::Layer> layers;
    for (const PlanItem &item : items) {
        Feed &f = *item.feed;
        if (!f.live.load() || f.releasePicture.load() || f.engineId < 0) continue;
        int w = 0, h = 0;
        if (f.capture()) {
            if (f.captureFrames() == 0) continue;
            if (auto direct=f.direct.load(); direct && direct->generation()!=f.directGeneration) continue;
            // Geometry follows the uploaded picture while either transport
            // negotiates or publishes a new size.
            w = f.pictureWidth;
            h = f.pictureHeight;
        } else if (f.isDevice() || f.isMedia()) {
            w = f.pictureWidth;
            h = f.pictureHeight;
        } else if (f.isNested()) {
            w = f.nestedWidth.load();
            h = f.nestedHeight.load();
        } else {
            if (!f.uploaded.load()) continue;
            std::lock_guard<std::mutex> lock(f.pixelsMutex);
            w = f.width;
            h = f.height;
        }
        if (w <= 0 || h <= 0) continue;
        engine::Layer layer;
        if (f.kind == "region") {
            const CaptureRegion region = f.regionAfterFilters(&w, &h);
            layer = region.layer(item.transform, w, h);
        } else {
            f.filteredSize(&w, &h);
            layer = itemLayer(item.transform, w, h);
        }
        layer.source = item.transition ? itemTransitionTarget(item.transition, f.engineId, w, h) : f.engineId;
        layers.push_back(layer);
    }
    return layers;
}

void Session::drawViews(engine::Engine &eng, int64_t nowNs, const std::vector<PlannedView> &views,
                        const std::vector<PlanItem> &editPlan, bool editShown) {
    const int cw = eng.canvasWidth(), ch = eng.canvasHeight();
    if (cw <= 0 || ch <= 0) return;
    // A studio drawn at a size: its layers scaled from the canvas.
    auto drawStudio = [&](const QString &id, const std::vector<PlanItem> &items, int w, int h, engine::Bgra8Frame &out) {
        auto target = m_viewTargets.find(id);
        if (target == m_viewTargets.end()) target = m_viewTargets.emplace(id, eng.createSource()).first;
        std::vector<engine::Layer> layers = layersFor(items);
        const float sx = float(w) / cw, sy = float(h) / ch;
        for (engine::Layer &l : layers) { l.x *= sx; l.y *= sy; l.w *= sx; l.h *= sy; }
        eng.compositeInto(target->second, w, h, layers, {0.f, 0.f, 0.f, 1.f});
        return eng.readSourcePreview(target->second, w, h, out);
    };
    auto sourceSize = [](Feed &f, int *w, int *h) {
        *w = f.isNested() ? f.nestedWidth.load() : f.pictureWidth;
        *h = f.isNested() ? f.nestedHeight.load() : f.pictureHeight;
        if ((*w <= 0 || *h <= 0) && !f.capture() && !f.isDevice() && !f.isMedia() && !f.isNested()) {
            std::lock_guard<std::mutex> lock(f.pixelsMutex);
            *w = f.width; *h = f.height;
        }
        return *w > 0 && *h > 0 && f.engineId >= 0 && f.live.load();
    };
    std::set<QString> keep;
    for (const PlannedView &pv : views) {
        const View &v = pv.view;
        keep.insert(v.id);
        if (pv.snapshot) {
            if (m_viewDrawn.count(v.id)) continue;  // sent; the main thread drops it
        } else {
            const auto last = m_viewDrawn.find(v.id);
            if (last != m_viewDrawn.end() && nowNs - last->second.ns < 1'000'000'000ll / std::clamp(v.fps, 1, 60)) continue;
            if (last != m_viewDrawn.end() && last->second.onItsWay->load()) continue;
        }
        ViewSent &sent = m_viewDrawn[v.id];
        sent.ns = nowNs;
        // Fitted into the view, keeping the picture's shape; a snapshot at full size.
        int pw = cw, ph = ch;
        if (pv.feed && !sourceSize(*pv.feed, &pw, &ph)) {
            if (pv.snapshot) emit snapshotReady(v.id, {});
            continue;
        }
        // A region shows its region here too: the engine holds the whole display,
        // and whoever chose a region may be keeping the rest of it private.
        const bool region = pv.feed && pv.feed->kind == QLatin1String("region");
        const bool input = v.shows == QLatin1String("sourceInput");
        const int fullW = pw, fullH = ph;
        int filteredW = pw, filteredH = ph;
        CaptureRegion shownRegion;
        QRect area(0, 0, pw, ph);
        if (region && input) area = pv.feed->captureBounds(pw, ph);
        else if (region) { shownRegion = pv.feed->regionAfterFilters(&filteredW, &filteredH); area = shownRegion.bounds(filteredW, filteredH); }
        if (area.isEmpty()) {
            if (pv.snapshot) emit snapshotReady(v.id, {});
            continue;
        }
        pw = area.width(); ph = area.height();
        const double fit = pv.snapshot ? 1.0 : std::min(double(std::max(2, v.width)) / pw, double(std::max(2, v.height)) / ph);
        const int w = std::max(2, int(pw * fit) & ~1), h = std::max(2, int(ph * fit) & ~1);
        engine::Bgra8Frame out;
        bool ok = false;
        if (region && input) {
            // Read whole, as only the engine's own picture can be, and cut down.
            const int inW = std::max(2, int(fullW * fit)), inH = std::max(2, int(fullH * fit));
            engine::Bgra8Frame whole;
            if (eng.readSourceInput(pv.feed->engineId, inW, inH, whole)) {
                const QImage cut = QImage(whole.data.data(), whole.width, whole.height, whole.width * 4, QImage::Format_ARGB32)
                                       .copy(int(area.x() * fit), int(area.y() * fit), w, h);
                out.width = w; out.height = h;
                out.data.resize(size_t(w) * h * 4);
                for (int y = 0; y < h; ++y) std::memcpy(&out.data[size_t(y) * w * 4], cut.constScanLine(y), size_t(w) * 4);
                ok = true;
            }
        } else if (region) {
            // One layer: the region, filling the picture.
            auto target = m_viewTargets.find(v.id);
            if (target == m_viewTargets.end()) target = m_viewTargets.emplace(v.id, eng.createSource()).first;
            engine::Layer layer = shownRegion.layer(Transform{}, filteredW, filteredH);
            layer.source = pv.feed->engineId;
            layer.x = 0; layer.y = 0; layer.w = float(w); layer.h = float(h);
            eng.compositeInto(target->second, w, h, {layer}, {0.f, 0.f, 0.f, 0.f});
            ok = eng.readSourcePreview(target->second, w, h, out);
        } else if (pv.feed) ok = input ? eng.readSourceInput(pv.feed->engineId, w, h, out)
                                       : eng.readSourcePreview(pv.feed->engineId, w, h, out);
        else if (v.shows == QLatin1String("studio")) ok = drawStudio(v.id, pv.items, w, h, out);
        else if (v.shows == QLatin1String("preview") && editShown) ok = drawStudio(v.id, editPlan, w, h, out);
        else ok = eng.readPreview(w, h, out);
        QImage picture = ok ? QImage(out.data.data(), out.width, out.height, out.width * 4, QImage::Format_ARGB32).copy() : QImage();
        if (pv.snapshot) emit snapshotReady(v.id, picture);
        else if (ok && claimPicture(*sent.onItsWay)) {
            emit viewReady(v.id, picture);
            // Behind the picture in the main thread's queue: it has come when this runs.
            QMetaObject::invokeMethod(this, [arrived = sent.onItsWay] { arrived->store(false); }, Qt::QueuedConnection);
        }
    }
    for (auto it = m_viewTargets.begin(); it != m_viewTargets.end();) {
        if (keep.count(it->first)) { ++it; continue; }
        eng.destroySource(it->second);
        it = m_viewTargets.erase(it);
    }
    std::erase_if(m_viewDrawn, [&](const auto &entry) { return !keep.count(entry.first); });
}

void Session::finishLastTick(engine::Engine &eng) {
    if (m_pendingOutputs.empty() && !m_pendingPreview.request && m_heldFrames.empty() && m_heldDirect.empty()) return;
    eng.collect();
    for (const PendingOutput &p : std::exchange(m_pendingOutputs, {})) {
        engine::YuvFrame yuv;
        {
            std::lock_guard<std::mutex> lock(m_poolMutex);
            if (!m_framePool.empty()) { yuv = std::move(m_framePool.back()); m_framePool.pop_back(); }
        }
        const bool taken = eng.takeOutput(p.request, yuv);
        if (taken && p.forCamera) m_camera.push(yuv);
        if (taken && p.forEncoders) {
            m_encodings.deliverPicture(std::move(yuv), p.layout.colour, p.timeNs, p.durationNs);
        } else {
            std::lock_guard<std::mutex> lock(m_poolMutex);
            if (m_framePool.size() < 8) m_framePool.push_back(std::move(yuv));
        }
    }
    if (const PendingPreview p = std::exchange(m_pendingPreview, {}); p.request) {
        engine::Bgra8Frame preview;
        if (eng.takePreview(p.request, preview)) {
            const bool program = p.studio && claimPicture(m_programOnItsWay);
            const bool shown = !p.editShown && claimPicture(m_previewOnItsWay);
            if (program || shown) {
                const QImage image = QImage(preview.data.data(), p.width, p.height, p.width * 4, QImage::Format_ARGB32).copy();
                if (program) emit programReady(image);
                if (shown) emit previewReady(image);
            }
        }
    }
    for (HeldFrame &h : m_heldFrames) h.feed->stream.release(h.frame);
    m_heldFrames.clear();
    m_heldDirect.clear();
}

void Session::dropLastTick() {
    m_pendingOutputs.clear();
    m_pendingPreview = {};
    for (HeldFrame &h : m_heldFrames) h.feed->stream.release(h.frame);
    m_heldFrames.clear();
    m_heldDirect.clear();
}

void Session::tick(int64_t nowNs) {
    static TickTimer timer;
    double tickTotal = 0;
    if (timer.on) timer.start(monotonicNs());
    engine::Engine &eng = *m_engine;
    finishLastTick(eng);
    ++m_tickSerial;
    m_tickNs = nowNs;
    // Items no plan shows any more give back their pictures.
    std::erase_if(m_itemTransitionsDrawn, [&](const std::shared_ptr<ItemTransition> &t) {
        if (t.use_count() > 1) return false;
        eng.destroySource(t->target);
        return true;
    });
    std::vector<std::shared_ptr<Feed>> feeds;
    std::vector<std::shared_ptr<Feed>> retired;
    QString capturePreview;
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        for (const auto &[id, f] : m_feeds) feeds.push_back(f);
        for (const auto &[id, f] : m_stingers) feeds.push_back(f);
        for (const auto &[key, copy] : m_copyFeeds) feeds.push_back(copy.feed);
        retired.swap(m_retiredFeeds);
        capturePreview.swap(m_capturePreviewRequest);
    }
    // Feeds gone since the last tick: their pictures out of the engine. Their
    // ids are only ever read and written here, on this thread.
    for (const auto &f : retired) {
        for (int id : {f->engineId, f->slideFrom, f->slideTo, f->slideMix}) if (id >= 0) eng.destroySource(id);
        f->engineId = f->slideFrom = f->slideTo = f->slideMix = -1;
    }
    if (m_canvasDirty.exchange(false)) eng.setCanvasSize(m_canvasWidth.load(), m_canvasHeight.load());
    eng.setHdrLevels({m_sdrWhite.load(), m_hdrPeak.load()});

    std::vector<HeldFrame> held;
    std::vector<std::shared_ptr<const DirectCapture::Frame>> directFrames;
    std::vector<engine::Layer> layers;

    // An automatic canvas settles once, on the first capture with pictures (a
    // display before a window), never with an encoder open or opening, of
    // whichever kind: each was opened at the size it is.
    if (m_canvasAuto.load()) {
        Feed *pick = nullptr;
        for (auto &f : feeds) {
            if (!f->capture() || !f->live.load() || f->captureFrames() == 0) continue;
            if (!pick || (f->kind == QLatin1String("display") && pick->kind != QLatin1String("display"))) pick = f.get();
        }
        const auto area = pick ? pick->captureBounds(pick->captureWidth(), pick->captureHeight()) : QRect();
        const int sw = area.isEmpty() ? 0 : std::max(2, area.width() & ~1);
        const int sh = area.isEmpty() ? 0 : std::max(2, area.height() & ~1);
        bool settle = sw > 0 && sh > 0;
        if (settle) {
            std::lock_guard<std::mutex> lock(m_settleMutex);
            settle = !m_outputsOpening && !m_encodings.busy();
            if (settle) {
                m_canvasWidth.store(sw);
                m_canvasHeight.store(sh);
                m_canvasAuto.store(false);
            }
        }
        if (settle) {
            const int fromW = eng.canvasWidth(), fromH = eng.canvasHeight();
            eng.setCanvasSize(sw, sh);
            const auto run = m_runSerial.load();
            QMetaObject::invokeMethod(this, [this, run, fromW, fromH, sw, sh] {
                // A profile may have restarted the session before this UI
                // notification arrives. Its old canvas must not resize the
                // current collection or overwrite the newly selected setup.
                if (!m_running.load() || run != m_runSerial.load()) return;
                emit canvasSettled(fromW, fromH, sw, sh);
                emit canvasChanged();
                refitEncoders();
            }, Qt::QueuedConnection);
        }
    }

    bool captureResized = false;
    QStringList hiddenNow;
    // A cut-out that cannot run has its source hidden by the engine, and is told once.
    const auto noteCutOut = [&](const std::shared_ptr<Feed> &f) {
        // One hidden for want of a graphics card has been said already, and the
        // stand-in that hides it is not a cut-out that failed.
        const engine::FilterFailure now = f->hiddenWithoutGpu ? engine::FilterFailure::None : eng.sourceFilterFailure(f->engineId);
        if (f->cutOutFailure.exchange(now) == now) return;
        QMetaObject::invokeMethod(this, [this, id = f->sourceId, now] {
            const SourceDef *def = m_production ? m_production->collection().source(id) : nullptr;
            if (def) {
                const QString text = now == engine::FilterFailure::None
                    ? tr("Remove background (AI) is running again on %1.").arg(def->name)
                    : tr("%1: %2").arg(def->name, cutOutFailureText(now));
                qWarning("%s", qPrintable(text));
                emit drawingChanged(text);
            }
            emit sourcesChanged();  // the Filters window says so too
        }, Qt::QueuedConnection);
    };
    for (auto &f : feeds) {
        if (f->releasePicture.exchange(false)) {
            if (f->engineId >= 0) eng.destroySource(f->engineId);
            f->engineId = -1; f->picture = {}; f->pictureWidth = f->pictureHeight = 0;
            f->fieldClock = {}; f->uploaded.store(false);
        }
        if (!f->live.load() || f->isAudio()) continue;
        if (f->engineId < 0) {
            f->engineId = eng.createSource();
            f->denoiseApplied = -1.f;
            f->videoFiltersApplied = 0;
        }
        // Taken out of the lock first: the engine can wait a long time on the
        // card, and the main thread asks for a source's size under this lock.
        std::optional<std::vector<engine::VideoFilter>> newFilters;
        uint64_t filtersSerial = 0;
        {
            std::lock_guard<std::mutex> lock(f->colourMutex);
            if (f->videoFiltersApplied != f->videoFiltersSerial) {
                newFilters = f->videoFilters;
                filtersSerial = f->videoFiltersSerial;
            }
        }
        if (newFilters) {
            // Finding people takes seconds a picture without a graphics
            // card, and left out it would put on air what it is there to
            // keep off: the source draws nothing instead, wherever it is
            // shown (videoFilterProblems and the status line say so).
            const bool hide = !eng.isGpu() && std::any_of(newFilters->begin(), newFilters->end(), [](const engine::VideoFilter &v) {
                return v.type == engine::VideoFilterType::RemoveBackground; });
            if (hide) {
                eng.setSourceFilters(f->engineId, {drawsNothing()});
                if (!f->hiddenWithoutGpu) hiddenNow.append(f->sourceId);
            } else {
                eng.setSourceFilters(f->engineId, *newFilters);
            }
            f->hiddenWithoutGpu = hide;
            f->videoFiltersApplied = filtersSerial;
        }
        if (const float want = f->denoise.load(); want != f->denoiseApplied) {
            eng.setSourceDenoise(f->engineId, want);
            f->denoiseApplied = want;
        }
        DeinterlaceSettings deinterlace;
        { std::lock_guard<std::mutex> lock(f->colourMutex); deinterlace = f->deinterlace; }
        eng.setSourceDeinterlace(f->engineId, deinterlace.mode);
        if (deinterlace.firstField != f->deinterlaceApplied.firstField || deinterlace.doubleRate != f->deinterlaceApplied.doubleRate)
            eng.resetSourceHistory(f->engineId);
        f->deinterlaceApplied = deinterlace;
        const bool linearAlpha = f->linearAlpha.load();
        const bool alphaChanged = linearAlpha != f->linearAlphaApplied;
        if (alphaChanged) eng.resetSourceHistory(f->engineId);
        auto nextClock = [&](int64_t time, int64_t duration, int first, bool discontinuity) {
            auto clock = f->fieldClock;
            const bool reset = clock.push(time,duration,first);
            if (reset || discontinuity) eng.resetSourceHistory(f->engineId);
            return clock;
        };
        if (f->isMedia() || f->isDevice()) {
            const bool got = f->isMedia() ? f->media.acquire(f->picture, nowNs) : f->device.acquire(f->picture);
            if (got || (alphaChanged && f->uploaded.load())) {
                auto clock = f->fieldClock;
                if (got) {
                    overrideColour(*f);
                    clock = nextClock(f->isMedia() ? f->picture.timeNs : std::min(f->picture.timeNs,nowNs),
                        f->picture.durationNs,f->picture.firstField,f->picture.discontinuity);
                }
                if (f->picture.rgba ? eng.updateSource(f->engineId, f->picture.rgbInput(linearAlpha))
                                    : eng.updateSource(f->engineId, f->picture.input(linearAlpha))) {
                    f->linearAlphaApplied = linearAlpha;
                    f->uploaded.store(true);
                    f->pictureWidth = f->picture.width;
                    f->pictureHeight = f->picture.height;
                    f->fieldClock = clock;
                }
            }
        } else if (f->kind == "slideshow") {
            if (f->dirty.exchange(false)) {
                SlideshowAnimator::Presentation view;
                { std::lock_guard<std::mutex> lock(f->pixelsMutex); view = f->slideshow; }
                if (!view.to.isNull()) {
                    const int w = view.to.width(), h = view.to.height();
                    if (f->slideFrom < 0) { f->slideFrom = eng.createSource(); f->slideTo = eng.createSource(); f->slideMix = eng.createSource(); }
                    auto upload = [&](int id, const QImage &image) {
                        const auto pixels = image.convertToFormat(QImage::Format_ARGB32);
                        return eng.updateSource(id, engine::CpuFrame{pixels.width(), pixels.height(), int(pixels.bytesPerLine()),
                            engine::PixelFormat::BGRA, pixels.constBits()});
                    };
                    if (view.serial != f->uploadedSlide) {
                        // Interrupted: start from the mix as it was drawn, before the
                        // slideshow's own filters (the source's picture has had them).
                        if (view.usePrevious && f->uploadedSlide != 0 && f->uploaded.load()) {
                            eng.transitionInto(f->slideMix, w, h, f->slideFrom, f->slideTo, f->slideShown.type, f->slideShown.progress);
                            std::swap(f->slideFrom, f->slideMix);
                        } else upload(f->slideFrom, view.from);
                        upload(f->slideTo, view.to); f->uploadedSlide = view.serial;
                    }
                    eng.transitionInto(f->engineId, w, h, f->slideFrom, f->slideTo, view.type, view.progress);
                    f->slideShown.type = view.type; f->slideShown.progress = view.progress;
                    f->uploaded.store(true); f->pictureWidth = w; f->pictureHeight = h;
                }
            }
        } else if (!f->capture()) {
            if (f->dirty.exchange(false) || alphaChanged) {
                // The picture is taken out for the upload: the engine can wait a
                // long time on the card, and the main thread takes this lock for
                // each new picture and to ask a source's size. A new picture
                // meanwhile brings its own, and a new upload with it. Its pixels
                // are read where they are, and stay while it is held here.
                QImage image;
                int width, height;
                bool premultiplied;
                {
                    std::lock_guard<std::mutex> lock(f->pixelsMutex);
                    image.swap(f->image);
                    width = f->width; height = f->height;
                    premultiplied = f->pixelsPremultiplied;
                }
                if (eng.updateSource(f->engineId, drawnFrame(image, premultiplied, linearAlpha))) {
                    f->linearAlphaApplied = linearAlpha;
                    f->uploaded.store(true);
                }
                std::lock_guard<std::mutex> lock(f->pixelsMutex);
                if (f->image.isNull() && f->width == width && f->height == height) f->image.swap(image);
            }
        } else if (auto direct=f->direct.load()) {
            if (f->configuredDirect!=direct) {
                std::map<uint32_t,std::vector<uint64_t>> modifiers;
                if(eng.isGpu()) for(uint32_t format:{DRM_FORMAT_ARGB8888,DRM_FORMAT_XRGB8888,DRM_FORMAT_ABGR8888,DRM_FORMAT_XBGR8888})
                    modifiers[format]=eng.importModifiers(format);
                direct->setDmaBufDevice(eng.renderNode(),modifiers);
                f->configuredDirect=direct;
            }
            if (auto frame=direct->acquire()) {
                const bool changed=frame->generation!=f->directGeneration;
                const auto clock=nextClock(std::min(frame->timeNs,nowNs),0,-1,changed);
                const bool ok=frame->dmabuf ? eng.updateSource(f->engineId,frame->dma) : eng.updateSource(f->engineId,frame->input());
                if (!ok && frame->dmabuf) direct->disableDmaBuf();
                if (ok) {
                    captureResized |= f->pictureWidth!=frame->width || f->pictureHeight!=frame->height;
                    f->pictureWidth=frame->width; f->pictureHeight=frame->height;
                    f->directGeneration=frame->generation; f->fieldClock=clock;
                }
                directFrames.push_back(std::move(frame));
            }
        } else {
            VideoStream::Frame frame;
            if (f->stream.acquire(frame)) {
                const auto clock = nextClock(std::min(frame.timeNs,nowNs),frame.durationNs,-1,frame.discontinuity);
                bool ok = false;
                if (frame.dmabuf) {
                    ok = eng.updateSource(f->engineId, frame.dma);
                    if (!ok && !f->dmabufFailed) {
                        f->dmabufFailed = true;
                        f->stream.disableDmaBuf();
                    }
                } else ok = eng.updateSource(f->engineId, frame.cpu);
                if (ok) {
                    const int width = frame.dmabuf ? frame.dma.width : frame.cpu.width;
                    const int height = frame.dmabuf ? frame.dma.height : frame.cpu.height;
                    captureResized |= f->pictureWidth != width || f->pictureHeight != height;
                    f->pictureWidth = width; f->pictureHeight = height;
                    f->fieldClock = clock;
                }
                held.push_back({f, frame});
            }
        }
        const auto pausedAt = f->isMedia() ? f->mediaPausedAt.load() : 0;
        const auto fieldTime = pausedAt ? pausedAt : nowNs;
        eng.renderSourceField(f->engineId, f->fieldClock.field(deinterlace,fieldTime), !f->fieldClock.second(deinterlace,fieldTime));
        if (!f->isNested()) {
            eng.renderSourceFilters(f->engineId, double(nowNs) / 1e9);  // nested: once drawn, below
            noteCutOut(f);
        }
    }

    if (captureResized) QMetaObject::invokeMethod(this, &Session::sourcesChanged, Qt::QueuedConnection);
    if (!hiddenNow.isEmpty()) QMetaObject::invokeMethod(this, [this, hiddenNow] {
        QStringList names;
        for (const QString &id : hiddenNow)
            if (const SourceDef *def = m_production ? m_production->collection().source(id) : nullptr) names.append(def->name);
        if (!names.isEmpty()) {
            const QString text = tr("Drawing without a graphics card, so Remove background (AI) cannot run. Hidden rather than shown with its background: %1.").arg(names.join(QStringLiteral(", ")));
            qWarning("%s", qPrintable(text));
            emit drawingChanged(text);
        }
        emit sourcesChanged();  // the Filters window says so too
    }, Qt::QueuedConnection);

    if (!capturePreview.isEmpty()) {
        QImage image;
        int width = 0, height = 0;
        for (const auto &feed : feeds) {
            if (feed->sourceId != capturePreview || !feed->capture() || !feed->live.load() || feed->engineId < 0) continue;
            width = feed->pictureWidth; height = feed->pictureHeight;
            if (width <= 0 || height <= 0) break;
            const double scale = std::min({1., 1280. / width, 720. / height});
            const int pw = std::max(1, int(width * scale)), ph = std::max(1, int(height * scale));
            engine::Bgra8Frame pixels;
            if (eng.readSourcePreview(feed->engineId, pw, ph, pixels))
                image = QImage(pixels.data.data(), pw, ph, pw * 4, QImage::Format_ARGB32).copy();
            break;
        }
        emit capturePreviewReady(capturePreview, image, width, height);
    }

    // Studios shown inside others and groups first, deepest first; then the
    // live studio, bottom to top.
    // All from the one thing published, held until this frame is drawn: the
    // plans and their transition are those that were made together.
    std::shared_ptr<const Published> published;
    float manualAt = 0.f;
    {
        std::lock_guard<std::mutex> lock(m_planMutex);
        published = m_published;
        manualAt = m_manualProgress.load();
    }
    const std::vector<PlanItem> &plan = published->plan, &cameraPlan = published->camera, &outgoing = published->outgoing,
                                &editPlan = published->edit;
    const std::vector<NestedRender> &nested = published->nested;
    const std::vector<PlannedView> &views = published->views;
    const bool cameraOwnPlan = published->cameraOwn, editShown = published->editShown;
    const PlannedTransition &transition = published->transition;
    for (const NestedRender &r : nested) {
        // Gone since it was planned: its picture went above, and a new one here
        // would never be retired.
        if (!r.feed->live.load()) continue;
        if (r.feed->engineId < 0) r.feed->engineId = eng.createSource();
        eng.compositeInto(r.feed->engineId, r.width, r.height, layersFor(r.items), {0.f, 0.f, 0.f, 0.f});
        eng.renderSourceFilters(r.feed->engineId, double(nowNs) / 1e9);
        noteCutOut(r.feed);
    }
    layers = layersFor(plan);
    if (transition.on) {
        // Both studios drawn whole, then mixed into the program's picture.
        const int w = eng.canvasWidth(), h = eng.canvasHeight();
        for (int *id : {&m_programFrom, &m_programTo, &m_programOut}) if (*id < 0) *id = eng.createSource();
        if (transition.serial != m_drawnTransition) {
            m_drawnTransition = transition.serial;
            // An interruption holds what was showing; with nothing showing yet it starts from black.
            m_fromStill = transition.fromShowing;
            if (m_fromStill) eng.compositeInto(m_programFrom, w, h, m_outDrawn ? std::vector<engine::Layer>{{m_programOut, 0, 0, float(w), float(h), 1.f}}
                                                                                : std::vector<engine::Layer>{}, {0.f, 0.f, 0.f, 1.f});
        }
        if (!m_fromStill) eng.compositeInto(m_programFrom, w, h, layersFor(outgoing), {0.f, 0.f, 0.f, 1.f});
        eng.compositeInto(m_programTo, w, h, layers, {0.f, 0.f, 0.f, 1.f});
        // By hand: as far as the bar was when these plans were taken.
        const float progress = transition.manual ? std::clamp(manualAt, 0.f, 1.f) : transition.durationNs <= 0 ? 1.f
                             : float(double(nowNs - transition.startNs) / double(transition.durationNs));
        // A stinger cuts at its point under its video; with a track matte,
        // the matte's bright part shows the next studio instead.
        const Feed *stinger = transition.stinger.get();
        const bool ready = stinger && stinger->engineId >= 0 && stinger->pictureWidth > 0 && stinger->pictureHeight > 0;
        const Feed *matteFeed = transition.stingerMatte.get();
        const bool matteReady = matteFeed && matteFeed->engineId >= 0 && matteFeed->pictureWidth > 0 && matteFeed->pictureHeight > 0;
        // A separate matte not drawn yet: a cut, for now.
        const int matte = !ready || (transition.matte == 4 && !matteReady) ? 0 : transition.matte;
        // The colour part, and the matte's, of the stinger's picture (u0, v0, u1, v1).
        float colourPart[4] = {0.f, 0.f, 1.f, 1.f}, mattePart[4] = {0.f, 0.f, 1.f, 1.f};
        if (matte == 1) { colourPart[2] = .5f; mattePart[0] = .5f; }
        if (matte == 2) { colourPart[3] = .5f; mattePart[1] = .5f; }
        if (matte) {
            if (m_programMatte < 0) m_programMatte = eng.createSource();
            engine::TransitionStyle keep;
            keep.type = engine::Transition::Matte;
            keep.invert = transition.matteInvert;
            std::copy(std::begin(mattePart), std::end(mattePart), keep.matteRect);
            eng.transitionInto(m_programMatte, w, h, m_programTo, matte == 4 ? matteFeed->engineId : stinger->engineId, keep, 0.f);
            engine::Layer under{m_programFrom, 0, 0, float(w), float(h), 1.f}, over{m_programMatte, 0, 0, float(w), float(h), 1.f};
            under.filter = over.filter = engine::ScaleFilter::Point;
            eng.compositeInto(m_programOut, w, h, {under, over}, {0.f, 0.f, 0.f, 1.f});
        } else {
            eng.transitionInto(m_programOut, w, h, m_programFrom, m_programTo, transition.style,
                               stinger ? (progress >= transition.cutAt ? 1.f : 0.f) : progress);
        }
        m_outDrawn = true;
        engine::Layer whole{m_programOut, 0, 0, float(w), float(h), 1.f};
        whole.filter = engine::ScaleFilter::Point;
        layers = {whole};
        if (ready && matte != 3) {
            const float pw = stinger->pictureWidth * (colourPart[2] - colourPart[0]), ph = stinger->pictureHeight * (colourPart[3] - colourPart[1]);
            const float fit = std::min(float(w) / pw, float(h) / ph), sw = pw * fit, sh = ph * fit;
            engine::Layer video{stinger->engineId, (w - sw) / 2, (h - sh) / 2, sw, sh, 1.f};
            video.u0 = colourPart[0]; video.v0 = colourPart[1]; video.u1 = colourPart[2]; video.v1 = colourPart[3];
            layers.push_back(video);
        }
    } else if (m_programOut >= 0) {
        for (int *id : {&m_programFrom, &m_programTo, &m_programOut, &m_programMatte}) { if (*id >= 0) eng.destroySource(*id); *id = -1; }
        m_outDrawn = false;
    }

    timer.lap(0, &tickTotal);
    eng.composite(layers, {0.f, 0.f, 0.f, 1.f});
    timer.lap(1, &tickTotal);

    // One readback per layout the encoders and the virtual camera take. A
    // picture whose encoder closed during readback goes back to the pool.
    // Each layout and size once; outputs at another size than the canvas
    // get it scaled in the readback.
    // The camera shares one when it shows the program; its own studio or
    // source is drawn into a picture of the canvas's size and read from that.
    std::vector<Encodings::Layout> layouts = m_encodings.wanted().layouts;
    const size_t forEncodersCount = layouts.size();
    // The camera takes 8-bit SDR whatever the outputs carry.
    const Encodings::Layout cameraLayout{engine::Engine::sdr420(m_camera.wantsNv12()), eng.canvasWidth(), eng.canvasHeight()};
    const bool cameraOwn = m_camera.inUse() && cameraOwnPlan;
    const bool cameraTakes = m_camera.inUse() && !cameraOwn;
    if (cameraOwn) {
        if (m_cameraTarget < 0) m_cameraTarget = eng.createSource();
        eng.compositeInto(m_cameraTarget, cameraLayout.width, cameraLayout.height, layersFor(cameraPlan), {0.f, 0.f, 0.f, 1.f});
        engine::YuvFrame yuv;
        {
            std::lock_guard<std::mutex> lock(m_poolMutex);
            if (!m_framePool.empty()) { yuv = std::move(m_framePool.back()); m_framePool.pop_back(); }
        }
        if (eng.readSourceOutput(m_cameraTarget, cameraLayout.colour, yuv, 0, 0)) m_camera.push(yuv);
        std::lock_guard<std::mutex> lock(m_poolMutex);
        if (m_framePool.size() < 8) m_framePool.push_back(std::move(yuv));
    } else if (m_cameraTarget >= 0) {
        eng.destroySource(m_cameraTarget);
        m_cameraTarget = -1;
    }
    if (cameraTakes && std::find(layouts.begin(), layouts.end(), cameraLayout) == layouts.end()) layouts.push_back(cameraLayout);
    for (size_t li = 0; li < layouts.size(); ++li) {
        const Encodings::Layout &layout = layouts[li];
        // Taken at the start of the next tick, with this tick's time.
        if (const int request = eng.requestOutput(layout.colour, layout.width, layout.height))
            m_pendingOutputs.push_back({request, layout, cameraTakes && layout == cameraLayout, li < forEncodersCount, nowNs,
                                        int64_t(1'000'000'000) * m_runFpsDen / m_runFpsNum});
    }
    // Encoders on the engine's own device: the canvas is converted once per
    // layout and copied into each one's frame on the GPU, handed over once
    // submitted (each frame's semaphore holds the encoder until it is drawn).
    // One the engine cannot draw for takes read-back pictures from the next tick.
    struct GpuFrame {
        std::shared_ptr<EncodedStream> stream;
        AVFrame *frame;
    };
    std::vector<GpuFrame> gpuFrames;
    {
        std::vector<std::pair<Encodings::Layout, std::vector<GpuFrame>>> byLayout;
        for (const Encodings::GpuTarget &t : m_encodings.gpuTargets()) {
            AVFrame *frame = t.stream->newGpuFrame();
            if (!frame) continue;  // none free in its pool: this picture is dropped, as a full queue would
            auto group = std::find_if(byLayout.begin(), byLayout.end(), [&](const auto &g) { return g.first == t.layout; });
            if (group == byLayout.end()) group = byLayout.insert(byLayout.end(), {t.layout, {}});
            group->second.push_back({t.stream, frame});
        }
        for (auto &[layout, group] : byLayout) {
            std::vector<AVFrame *> frames;
            for (const GpuFrame &g : group) frames.push_back(g.frame);
            if (eng.writeVulkanFrames(layout.colour, layout.width, layout.height, frames)) {
                gpuFrames.insert(gpuFrames.end(), group.begin(), group.end());
            } else {
                for (GpuFrame &g : group) {
                    m_encodings.refuseGpu(g.stream);
                    av_frame_free(&g.frame);
                }
            }
        }
    }
    timer.lap(2, &tickTotal);
    timer.lap(3, &tickTotal);

    // Studio mode previews the studio being edited, drawn only for that,
    // and shows the program beside it.
    const bool studio = m_studioRender.load();
    if (!(studio && editShown && m_previewEnabled.load()) && m_editTarget >= 0) { eng.destroySource(m_editTarget); m_editTarget = -1; }
    if (m_previewEnabled.load()) {
        static int64_t lastPreview = 0;
        if (nowNs - lastPreview >= 1'000'000'000ll / 30) {
            lastPreview = nowNs;
            const int pw = m_previewWidth.load() & ~1, ph = m_previewHeight.load() & ~1;
            // The program's preview comes with the next tick (finishLastTick);
            // studio mode's edited studio is drawn and read now.
            if (const int request = eng.requestPreview(pw, ph)) {
                m_pendingPreview = {request, pw, ph, studio, studio && editShown};
                if (studio && editShown) {
                    engine::Bgra8Frame preview;
                    if (m_editTarget < 0) m_editTarget = eng.createSource();
                    eng.compositeInto(m_editTarget, eng.canvasWidth(), eng.canvasHeight(), layersFor(editPlan), {0.f, 0.f, 0.f, 1.f});
                    if (eng.readSourcePreview(m_editTarget, pw, ph, preview) && claimPicture(m_previewOnItsWay))
                        emit previewReady(QImage(preview.data.data(), pw, ph, pw * 4, QImage::Format_ARGB32).copy());
                }
            }
        }
    }

    drawViews(eng, nowNs, views, editPlan, editShown);

    timer.lap(4, &tickTotal);
    // To the GPU without waiting: it draws and reads back while this thread
    // waits for the next tick, which takes the pictures and gives back what
    // the GPU read from.
    eng.submit();
    for (GpuFrame &g : gpuFrames)
        m_encodings.deliverGpuFrame(g.stream, g.frame, nowNs, int64_t(1'000'000'000) * m_runFpsDen / m_runFpsNum);
    m_heldFrames = std::move(held);
    m_heldDirect = std::move(directFrames);
    timer.lap(5, &tickTotal);
    if (timer.on) {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        timer.endTick(tickTotal, m_stats.lateTicks);
    }

    if (eng.lost()) {
        // Device gone: carry on with the CPU twin, and say so.
        const QString gone = QString::fromStdString(eng.deviceName());
        qWarning("%s stopped answering: drawing on the processor from here on.", qPrintable(gone));
        QMetaObject::invokeMethod(this, [this, gone] {
            emit drawingChanged(tr("%1 stopped answering: drawing on the processor from here on, which is slower.").arg(gone));
        }, Qt::QueuedConnection);
        for (auto &f : feeds) {
            f->engineId = f->slideFrom = f->slideTo = f->slideMix = -1; f->uploadedSlide = 0; f->configuredDirect.reset();
            f->uploaded.store(false); f->fieldClock = {};  // nothing of it is in the new engine yet
            if (!f->capture()) f->dirty.store(true);  // re-upload to the new engine
        }
        {
            // Ids of the old engine: never to be destroyed in the new one.
            std::lock_guard<std::mutex> lock(m_sourcesMutex);
            m_retiredFeeds.clear();
        }
        m_engine = engine::createCpuEngine();
        m_encodings.setGpuDevice(nullptr);
        publishEngineFormats();
        dropLastTick();
        m_cameraTarget = m_programFrom = m_programTo = m_programOut = m_programMatte = m_editTarget = m_clearSource = -1;
        for (const auto &t : std::exchange(m_itemTransitionsDrawn, {})) t->target = -1;
        m_viewTargets.clear(); m_viewDrawn.clear();
        m_drawnTransition = 0; m_fromStill = m_outDrawn = false;
        m_engine->setCanvasSize(m_canvasWidth.load(), m_canvasHeight.load());
        std::lock_guard<std::mutex> lock(m_statsMutex);
        {
            std::lock_guard<std::mutex> encoderLock(m_encoderMutex);
            m_engineDevice = EngineDevice{};
        }
        m_stats.engine = QStringLiteral("CPU (after device loss)");
        m_stats.engineWhy.clear();
        m_stats.gpu = false;
    }
}
