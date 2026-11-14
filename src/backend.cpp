#include "backend.h"
#include "core/audio_settings.h"
#include "core/meter_settings.h"

#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStorageInfo>
#include <QUuid>

#include <cmath>
#include <numeric>
#include <set>

Backend::Backend(QObject *parent, std::string waylandSocket)
    : QObject(parent), m_session(nullptr, std::move(waylandSocket)), m_hyprland(QGuiApplication::desktopFileName()),
      m_control([this](const QJsonObject &request) { return controlRequest(request); }),
      m_scripts({[this](const QString &action, const QString &argument) { return runAction(action, argument); },
                 [this] { return state(); }, [this] { return actions(); },
                 [this](const QString &path) { return scriptValues(path); },
                 [this](const QString &path, const QVariantMap &form) { m_scriptForms.insert(path, form); emit scriptsChanged(); }}),
      m_remote([this](const QString &type, const QJsonObject &data, const RemoteControl::Done &done) { remoteRequest(type, data, done); },
               remoteRequests()) {
    connect(this,&Backend::selectionChanged,this,&Backend::mediaPlaybackChanged);
    connect(this,&Backend::selectionChanged,this,&Backend::slideshowPlaybackChanged);
    connect(&m_session,&Session::slideshowChanged,this,&Backend::slideshowPlaybackChanged);
    connect(&m_audioHotkeys, &Hotkeys::held, this, [this](const QString &target, bool talk, bool held, unsigned origin) {
        m_session.holdAudioPush(target, talk, held, origin); emit audioPushChanged();
    });
    connect(&m_audioHotkeys, &Hotkeys::triggered, this, &Backend::runHotkey);
    connect(&m_audioHotkeys, &Hotkeys::cancelled, this, [this](unsigned origins) { m_session.cancelAudioPush({}, origins); emit audioPushChanged(); });
    connect(&m_audioHotkeys, &Hotkeys::changed, this, [this] {
        if (!m_audioHotkeys.triggers().isEmpty() && !m_session.profile().globalHotkeys) {
            m_session.profile().globalHotkeys = true; saveProfile();
        }
        emit audioHotkeysChanged();
    });
    connect(this, &Backend::settingsChanged, this, &Backend::refreshHotkeys);
    connect(this, &Backend::settingsChanged, this, &Backend::globalAudioDevicesChanged);
    connect(&m_session, &Session::sourcesChanged, this, &Backend::globalAudioDevicesChanged);
    connect(&m_session, &Session::microphonesChanged, this, &Backend::globalAudioDevicesChanged);
    connect(&m_production, &Production::saveFailed, this, [this](const QString &path) {
        setStatus(tr("Could not save the studios to %1; the changes are kept until it can.").arg(path));
    });
    connect(&m_production, &Production::changed, this, &Backend::refreshHotkeys);
    connect(&m_production, &Production::changed, this, &Backend::hotkeysChanged);
    connect(&m_production, &Production::changed, this, [this] { syncSwitcher(); emit switcherChanged(); });
    connect(&m_focus, &FocusWatcher::focused, this, &Backend::windowFocused);
    m_outputTimer.setInterval(1000);
    connect(&m_outputTimer, &QTimer::timeout, this, &Backend::tickOutputTimer);
    m_outputTimer.start();
    connect(&m_scripts, &ScriptHost::logChanged, this, &Backend::scriptLogChanged);
    sendScriptEvents();
    connect(this, &Backend::settingsChanged, this, &Backend::hotkeysChanged);
    loadActiveProfile();
    refreshHotkeys();
    connect(&m_session, &Session::previewReady, this, &Backend::previewReady);
    connect(&m_session, &Session::programReady, this, &Backend::programReady);
    connect(&m_session, &Session::viewReady, this, &Backend::viewReady);
    connect(&m_session, &Session::browserFrameReady, this, &Backend::browserFrame);
    m_viewsChanged.setSingleShot(true);
    m_viewsChanged.setInterval(0);
    connect(&m_viewsChanged, &QTimer::timeout, this, [this] {
        std::vector<Session::View> views;
        for (const auto &[id, view] : m_views) views.push_back(view);
        m_session.setViews(views);
    });
    connect(qGuiApp, &QGuiApplication::screenAdded, this, &Backend::screensChanged);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &Backend::screensChanged);
    // Names change and studios go: projectors follow, without reopening the rest.
    connect(&m_production, &Production::changed, this, [this] {
        const QVariantList now = projectors();
        if (now != m_projectorsSeen) { m_projectorsSeen = now; emit projectorsChanged(); }
    });
    connect(&m_session, &Session::snapshotReady, this, &Backend::keyBackdropFound);
    connect(&m_session, &Session::snapshotReady, this, [this](const QString &id, const QImage &image) {
        if (!m_screenshots.contains(id)) return;
        const bool source = m_screenshots.take(id) == QLatin1String("source");
        if (image.isNull()) { emit failed(tr("There is no picture to take a screenshot of yet.")); return; }
        QDir().mkpath(folder());
        const QString base = QDir(folder()).filePath(QStringLiteral("Screenshot %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh-mm-ss"))));
        QString path = base + QStringLiteral(".png");
        for (int n = 2; QFileInfo::exists(path); ++n) path = QStringLiteral("%1 (%2).png").arg(base).arg(n);
        // A source keeps its transparency (a keyed one shows where it is clear).
        if (!(source ? image : image.convertToFormat(QImage::Format_RGB32)).save(path)) { emit failed(tr("The screenshot could not be saved in %1.").arg(folder())); return; }
        setStatus(tr("Saved a screenshot: %1").arg(QFileInfo(path).fileName()));
        emit screenshotSaved(path);
    });
    applyProjectorRules();
    connect(&m_session, &Session::studioModeChanged, this, &Backend::transitionStateChanged);
    connect(&m_session, &Session::transitionChanged, this, [this] {
        transitionEnded();
        emit transitionStateChanged();
    });
    connect(&m_regionDrawer, &RegionDrawer::drawn, this, [this](const QString &id, const QRect &r) {
        // Drawn for a source of another collection: this one's of that id is not it.
        if (m_regionGeneration != m_production.generation()) { setStatus(tr("The region is as it was: the collection changed.")); return; }
        setCaptureRegion(id, r.x(), r.y(), r.width(), r.height());
        setStatus(tr("Region: %1 × %2 pixels from %3, %4").arg(r.width()).arg(r.height()).arg(r.x()).arg(r.y()));
    });
    connect(&m_regionDrawer, &RegionDrawer::failed, this, [this](const QString &, const QString &message) {
        if (message.isEmpty()) setStatus(tr("The region is as it was."));
        else emit failed(message);
    });
    connect(&m_regionDrawer, &RegionDrawer::windowsMoved, this, &Backend::failed);
    connect(&m_session, &Session::capturePreviewReady, this, [this](const QString &id, const QImage &image, int w, int h) {
        const auto *source = m_production.collection().source(id);
        if (!source || source->kind != "region") return;
        if (image.isNull()) { emit failed(tr("The display has no picture yet. Try again when capture is live.")); return; }
        emit captureRegionPreview(id, image, w, h);
    });
    m_session.setProduction(&m_production);
    connect(&m_session, &Session::monitoringChanged, this, &Backend::monitoringChanged);
    connect(&m_session, &Session::sourcesChanged, this, &Backend::itemsChanged);
    connect(&m_session, &Session::sourcesChanged, this, &Backend::selectionChanged);
    connect(&m_session, &Session::videoDevicesChanged, this, &Backend::videoDevicesChanged);
    connect(&m_session, &Session::restoreTokenChanged, this, [this] { saveProfile(); });
    // The first capture settles an automatic canvas: the layout follows it, from
    // the size it is at, and the size is kept from then on.
    connect(&m_session, &Session::canvasSettled, this, [this](int fromW, int fromH, int toW, int toH) {
        // One that does not say what size it is at was drawn at the placeholder.
        if (m_production.collection().canvasWidth <= 0) m_production.followCanvas(fromW, fromH);
        m_production.followCanvas(toW, toH);
        m_session.profile().canvasWidth = toW;
        m_session.profile().canvasHeight = toH;
        saveProfile();
    });
    connect(&m_production, &Production::changed, this, [this] {
        if (!m_slideshowTarget.isEmpty() && (m_slideshowGeneration != m_production.generation() || !hasSlideshow(m_slideshowTarget)))
            m_slideshowPicker.cancel();
        emit scenesChanged();
        emit itemsChanged();
        emit selectionChanged();
        emit virtualCameraChanged();  // what it shows may be gone
    });
    connect(&m_production, &Production::selectionChanged, this, &Backend::selectionChanged);
    connect(&m_production, &Production::historyChanged, this, &Backend::historyChanged);
    connect(&m_production, &Production::clipboardChanged, this, &Backend::sourceClipboardChanged);
    connect(&m_mediaPicker, &PortalFileChooser::selected, this, [this](const QUrl &url) {
        const QString path = url.toLocalFile();
        if (!m_mediaTarget.isEmpty()) {
            // Not in another collection's source of the same id.
            if (m_pickGeneration == m_production.generation()) setSourceSetting(m_mediaTarget, QStringLiteral("input"), path);
            m_mediaTarget.clear();
            return;
        }
        addMediaFile(path);
    });
    auto slideshowChosen = [this](const QList<QUrl> &urls) {
        auto callback = std::move(m_slideshowChosen); m_slideshowChosen = nullptr; m_slideshowTarget.clear();
        if (callback) callback(urls);
    };
    connect(&m_slideshowPicker, &PortalFileChooser::filesSelected, this, slideshowChosen);
    connect(&m_slideshowPicker, &PortalFileChooser::selected, this, [slideshowChosen](const QUrl &url) { slideshowChosen({url}); });
    connect(&m_slideshowPicker, &PortalFileChooser::canceled, this, [this] { m_slideshowChosen = nullptr; m_slideshowTarget.clear(); });
    connect(&m_slideshowPicker, &PortalFileChooser::failed, this, [this](const QString &error) { m_slideshowChosen = nullptr; m_slideshowTarget.clear(); setStatus(error); });
    connect(&m_otherPicker, &PortalFileChooser::selected, this, [this](const QUrl &url) {
        if (m_otherChosen) m_otherChosen(url.toLocalFile());
        m_otherChosen = nullptr;
    });
    connect(&m_otherPicker, &PortalFileChooser::canceled, this, [this] { m_otherChosen = nullptr; });
    // No portal, or none that can show a file picker: the action did nothing.
    const auto pickerFailed = [this](const QString &error) {
        const QString text = tr("The file picker did not open: %1").arg(error);
        setStatus(text);
        emit failed(text);
    };
    connect(&m_otherPicker, &PortalFileChooser::failed, this, [this, pickerFailed](const QString &error) { m_otherChosen = nullptr; pickerFailed(error); });
    connect(&m_mediaPicker, &PortalFileChooser::failed, this, [this, pickerFailed](const QString &error) { m_mediaTarget.clear(); pickerFailed(error); });
    connect(&m_imagePicker, &PortalFileChooser::failed, this, [this, pickerFailed](const QString &error) { m_imageTarget.clear(); pickerFailed(error); });
    connect(&m_imagePicker, &PortalFileChooser::selected, this, [this](const QUrl &url) {
        const QString path = url.toLocalFile();
        if (!m_imageTarget.isEmpty()) {
            if (m_pickGeneration == m_production.generation()) setSourceSetting(m_imageTarget, QStringLiteral("path"), path);
            m_imageTarget.clear();
            return;
        }
        addImageFile(path);
    });
    connect(&m_session, &Session::recordingChanged, this, [this] {
        // A recording ended, even between two of the timer's ticks: the next counts from nothing.
        if (!m_session.recording() || m_session.recordingStopping()) m_recordElapsed = -1;
        emit recordingChanged();
        refreshStats();
        if (m_session.recordingStopping() && !m_recordingFailed) setStatus(tr("Finishing recording…"));
        // One that failed keeps saying so.
        if (!m_session.recording() && !m_session.stats().recordingPath.isEmpty() && !std::exchange(m_recordingFailed, false)) {
            setStatus(tr("Saved %1").arg(QFileInfo(m_session.stats().recordingPath).fileName()));
            // The playback window moves to it, unless something there is playing.
            m_lastRecording = m_session.stats().recordingPath;
            if (m_playbackOpen && m_player.shuttle() == 0) m_player.open(m_lastRecording);
            emit playbackChanged();
        }
        m_player.refreshSound();
    });
    m_player.soundTarget = [this](QString *why) { return playbackSoundTarget(why); };
    // Whether the playback window may sound is chosen again whenever what could
    // take its sound in changes: the replay buffer, Desktop sound and the
    // outputs captured (added, muted, another device), monitoring's output and
    // the desktop's own. Asking again changes nothing when the answer is the same.
    const auto playbackSoundAgain = [this] { if (m_playbackOpen) m_player.refreshSound(); };
    connect(&m_session, &Session::replayChanged, this, playbackSoundAgain);
    connect(this, &Backend::settingsChanged, this, playbackSoundAgain);
    connect(&m_production, &Production::changed, this, playbackSoundAgain);
    connect(&m_session, &Session::microphonesChanged, this, playbackSoundAgain);
    connect(&m_session, &Session::monitoringChanged, this, playbackSoundAgain);
    connect(&m_session, &Session::streamingChanged, this, [this] {
        if (!m_session.streaming() || m_session.streamingStopping()) m_streamElapsed = -1;
        emit streamingChanged();
        refreshStats();
        m_player.refreshSound();
        if (!m_session.streaming()) setStatus(tr("Stream ended"));
        else if (m_session.streamingStopping()) setStatus(tr("Ending the stream…"));
    });
    connect(&m_session, &Session::streamFailed, this, [this](const QString &name, const QString &message) {
        const QString text = tr("%1 stopped: %2").arg(name, message);
        setStatus(text);
        emit failed(text);
    });
    connect(&m_session, &Session::recordingFailed, this, [this](const QString &message) {
        m_recordingFailed = m_session.recording();  // the one now finishing, if any
        setStatus(tr("Recording stopped: %1").arg(message));
        emit failed(message);
    });
    connect(&m_session, &Session::replayChanged, this, &Backend::replayChanged);
    connect(&m_session, &Session::remuxChanged, this, &Backend::remuxChanged);
    connect(&m_session, &Session::recordingWarning, this, &Backend::setStatus);
    connect(&m_session, &Session::pipeWireChanged, this, &Backend::setStatus);
    connect(&m_session, &Session::drawingChanged, this, &Backend::setStatus);
    connect(&m_session, &Session::diskSpaceLow, this, [this](const QString &message) {
        setStatus(message);
        emit failed(message);
    });
    connect(&m_session, &Session::remuxFinished, this, [this](const QString &in, const QString &out, const QString &error, const QString &note) {
        if (error.isEmpty()) {
            setStatus(note.isEmpty() ? tr("MP4 copy made: %1").arg(QFileInfo(out).fileName())
                                     : tr("MP4 copy made: %1, but %2").arg(QFileInfo(out).fileName(), note));
        } else {
            const QString text = tr("No MP4 copy of %1: %2").arg(QFileInfo(in).fileName(), error);
            setStatus(text);
            emit failed(text);
        }
    });
    auto remuxChosen = [this](const QList<QUrl> &urls) {
        QStringList paths;
        for (const QUrl &u : urls) paths << u.toLocalFile();
        remuxFiles(paths);
    };
    connect(&m_remuxPicker, &PortalFileChooser::filesSelected, this, remuxChosen);
    connect(&m_remuxPicker, &PortalFileChooser::selected, this, [remuxChosen](const QUrl &url) { remuxChosen({url}); });
    connect(&m_remuxPicker, &PortalFileChooser::failed, this, [this](const QString &error) { setStatus(error); });
    connect(&m_session, &Session::virtualCameraChanged, this, [this] {
        const bool wasInUse = m_cameraInUse;
        m_cameraInUse = m_session.virtualCameraInUse();
        emit virtualCameraChanged();
        if (m_cameraInUse && !wasInUse) setStatus(tr("An app is using the virtual camera"));
        else if (!m_cameraInUse && wasInUse && m_session.virtualCameraRunning()) setStatus(tr("No app is using the virtual camera"));
    });
    connect(&m_session, &Session::replaySaved, this, [this](const QString &path) {
        setStatus(tr("Replay saved: %1").arg(QFileInfo(path).fileName()));
    });
    connect(&m_session, &Session::replayFailed, this, [this](const QString &message) {
        setStatus(tr("Replay: %1").arg(message));
        emit failed(message);
    });
    connect(&m_session, &Session::statsChanged, this, &Backend::refreshStats);
    connect(&m_session, &Session::statsChanged, this, &Backend::followPowerAndPendingRestart);
    connect(&m_session, &Session::videoDevicesChanged, this, &Backend::frameRateNoteChanged);
    connect(&m_production, &Production::changed, this, &Backend::frameRateNoteChanged);
    connect(&m_session, &Session::canvasChanged, this, &Backend::settingsChanged);
    connect(&m_session, &Session::microphonesChanged, this, &Backend::microphonesChanged);
    connect(this, &Backend::settingsChanged, this, &Backend::noSoundChanged);
    connect(this, &Backend::itemsChanged, this, &Backend::noSoundChanged);
    connect(&m_session, &Session::microphonesChanged, this, &Backend::itemsChanged);
    connect(&m_session, &Session::encodersChanged, this, [this] {
        emit settingsChanged();
        if (!m_session.probing() && m_session.recordingEncoders().empty())
            setStatus(tr("No video encoder works here; recording is unavailable."));
        else if (!m_session.probing() && m_status == tr("Checking encoders…"))  // progress along the way leaves it
            setStatus(m_production.collection().sources.isEmpty() ? tr("Add a display or a window to begin.")
                                                                  : QString());
    });
    m_statsTimer.setInterval(500);
    connect(&m_statsTimer, &QTimer::timeout, this, &Backend::refreshStats);
    m_levelTimer.setInterval(50);
    m_meterElapsed.start();
    connect(&m_levelTimer, &QTimer::timeout, this, [this] {
        const double seconds = m_meterElapsed.restart() / 1000.0;
        const double rate = MeterSettings::decayRate(m_session.profile().meter);
        const auto display = [&](const PeakMeter::Reading &reading, const QVariantMap &previous) {
            QVariantList db, levels;
            const auto oldDb = previous.value("db").toList();
            for (size_t c = 0; c < reading.levels.size(); ++c) {
                const double measured = reading.levels[c] > 0 ? std::max(-60.0, 20.0*std::log10(reading.levels[c])) : -60;
                const double value = PeakMeter::decay(c < size_t(oldDb.size()) ? oldDb[int(c)].toDouble() : -60, measured, seconds, rate);
                db.append(value); levels.append(std::clamp((value+60)/60, 0.0, 1.0));
            }
            return QVariantMap{{"db", db}, {"levels", levels}, {"maximumDb", reading.maximum > 0 ? 20*std::log10(reading.maximum) : -120},
                               {"clipped", reading.maximum >= 1.f}};
        };
        m_desktopMeter = display(m_session.globalAudioMeter(0), m_desktopMeter);
        m_microphoneMeter = display(m_session.globalAudioMeter(1), m_microphoneMeter);
        QVariantMap meters, levels;
        const auto maximumLevel = [](const QVariantMap &meter) {
            double peak = 0; for (const auto &value : meter.value("levels").toList()) peak = std::max(peak, value.toDouble());
            return peak;
        };
        m_desktopLevel = maximumLevel(m_desktopMeter);
        m_microphoneLevel = maximumLevel(m_microphoneMeter);
        for (const SourceDef &source : m_production.collection().sources) {
            if (!source.hasAudio()) continue;
            const auto meter = display(m_session.sourceAudioMeter(source.id), m_sourceAudioMeters.value(source.id).toMap());
            meters[source.id] = meter; levels[source.id] = maximumLevel(meter);
        }
        m_sourceAudioMeters = meters; m_sourceAudioLevels = levels;
        QVariantMap globalMeters;
        for (const auto &device : m_session.profile().globalAudioDevices)
            globalMeters[device.id] = display(m_session.globalAudioDeviceMeter(device.id), m_globalAudioDeviceMeters.value(device.id).toMap());
        m_globalAudioDeviceMeters = globalMeters;
        emit levelsChanged();
    });
    setUpTools();
    setUpRemote();
}

Backend::~Backend() {
    // No more answers from here: a copy started now waits for this one to finish
    // rather than asking it to come forward.
    m_control.close();
    // Then the scripts: stopping one answers its waiting calls, and an action it
    // asked for needs everything here still standing.
    m_scripts.setScripts({});
    // A presentation still being drawn has nowhere to go now: its pictures are
    // swept with the rest that nothing uses.
    if (m_presentationJob) {
        m_presentationJob->disconnect(this);
        m_presentationJob->kill();
        m_presentationJob->waitForFinished(2000);
        m_presentationLoading.clear();
    }
    m_session.stop();
    m_production.save();
    saveProfile();
    sweepPresentationFolders();
    // The members go one by one from here: nothing they still say may reach this.
    disconnect(&m_session, nullptr, this, nullptr);
    disconnect(&m_production, nullptr, this, nullptr);
}

bool Backend::listenForControl(QString *why) { return m_control.listen(why); }

// The icon goes in the bar once, the first time. After that the bar's own
// settings say where it is, or that the person took it out.
void Backend::startOmarchyBar() {
    if (!m_omarchyBar.available() || safeMode()) return;
    connect(&m_omarchyBar, &OmarchyBar::activeChanged, this, [this] {
        if (m_omarchyBar.active()) setPreference(QStringLiteral("omarchyBarPlaced"), true);
        emit omarchyBarChanged();
    });
    connect(&m_omarchyBar, &OmarchyBar::failed, this, [](const QString &why) {
        qWarning("Omarchy bar: %s", qPrintable(why));
    });
    if (QString why; !m_omarchyBar.install(&why)) { qWarning("Omarchy bar: %s", qPrintable(why)); return; }
    if (!m_omarchyBar.active() && !preference(QStringLiteral("omarchyBarPlaced"), false).toBool()) m_omarchyBar.show();
}

void Backend::setOmarchyBar(bool on) {
    if (on) m_omarchyBar.show();
    else m_omarchyBar.hide();
}

void Backend::start() {
    // The control socket: `omabroadcast ctl` and scripts. A second copy of
    // the app leaves it to the first.
    if (QString why; !m_control.listen(&why)) qInfo("control socket: %s", qPrintable(why));
    startOmarchyBar();
    applyRemote();
    // Safe mode starts nothing by itself: no scripts, no switcher.
    if (!safeMode()) m_scripts.setScripts(preference(QStringLiteral("scripts"), QVariantList()).toList());
    {
        const QString open = m_session.profile().collection;
        m_session.setTokenScope(open == QLatin1String("Default") ? QString() : open);
    }
    loadCollection();
    // Pictures of presentations no collection shows any more (a source removed,
    // a load given up when the app closed): nothing can still want them now.
    sweepPresentationFolders();
    dropGoneProjectors();
    fitLayoutToCanvas();
    // A file set aside on the way here says so first, and that stays up.
    if (m_status.isEmpty()) setStatus(tr("Checking encoders…"));
    m_session.setCopySources(takeCopySources());
    m_session.setStudioMode(preference(QStringLiteral("studioMode"), false).toBool());
    m_session.setGpuChoice(gpu());
    m_session.start();
    fitLayoutToCanvas();
    refreshHotkeys();
    // Safe mode asks the desktop for nothing.
    if (m_session.profile().globalHotkeys && !m_session.safeMode()) m_audioHotkeys.bindGlobal();
    m_statsTimer.start();
    m_levelTimer.start();
    refreshStats();
    emit settingsChanged();
    // A file named before there were studios to put it in.
    m_started = true;
    if (const QUrl wanted = std::exchange(m_openAfterCrash, {}); !wanted.isEmpty()) open(wanted);
}

QString Backend::recordingFile() const {
    return QFileInfo(m_session.stats().recordingPath).fileName();
}

QVariantList Backend::encoders() const {
    const std::vector<RankedEncoder> ranked = m_session.recordingEncoders();
    QVariantList out;
    // While the check runs: those found to work so far, as they are found.
    if (m_session.probing()) {
        out.append(QVariantMap{{QStringLiteral("name"), QString()}, {QStringLiteral("label"), tr("Best available (checking…)")}});
        for (const EncoderInfo &e : m_session.probeFound())
            out.append(QVariantMap{{QStringLiteral("name"), QString::fromStdString(encoderKey(e))},
                                   {QStringLiteral("label"), QString::fromStdString(e.label)}});
        return out;
    }
    out.append(QVariantMap{{QStringLiteral("name"), QString()},
                           {QStringLiteral("label"), ranked.empty()
                                ? tr("Best available")
                                : tr("Best available (%1)").arg(QString::fromStdString(ranked.front().encoder.label))}});
    // Best first; then what works here but not for this canvas or container,
    // still listed so the choice is visible, marked with why.
    std::set<std::string> listed;
    for (const RankedEncoder &r : ranked) {
        listed.insert(encoderKey(r.encoder));
        out.append(QVariantMap{{QStringLiteral("name"), QString::fromStdString(encoderKey(r.encoder))},
                               {QStringLiteral("label"), QString::fromStdString(r.encoder.label)}});
    }
    const std::set<std::string> unfit = m_session.unfitEncoders();
    const std::set<std::string> slow = m_session.slowEncoders();
    for (const EncoderInfo &e : m_session.encoders()) {
        if (!e.available || listed.count(encoderKey(e))) continue;
        const QString why = slow.count(encoderKey(e))
                                ? tr("only %1 of %2 frames a second at %3 × %4")
                                      .arg(int(m_session.encoderSpeed(encoderKey(e)) + 0.5)).arg(fps()).arg(canvasWidth()).arg(canvasHeight())
                                : unfit.count(encoderKey(e))
                                ? QStringLiteral("can't take %1 × %2").arg(canvasWidth()).arg(canvasHeight())
                                : !m_session.colourProblem(e).isEmpty() ? tr("change colour settings")
                                : QStringLiteral("not in %1").arg(containerName());
        out.append(QVariantMap{{QStringLiteral("name"), QString::fromStdString(encoderKey(e))},
                               {QStringLiteral("label"), QStringLiteral("%1 (%2)").arg(QString::fromStdString(e.label), why)}});
    }
    return out;
}

// While the check runs: what it is trying, and what works so far.
QString Backend::probeNote() const {
    const std::vector<EncoderInfo> found = m_session.probeFound();
    int hardware = 0;
    for (const EncoderInfo &e : found) hardware += e.hardware;
    QString note = m_session.probeNow().isEmpty() ? tr("Checking which encoders work…")
                                                   : tr("Checking which encoders work: trying %1…").arg(m_session.probeNow());
    if (!found.empty()) note += QLatin1Char(' ') + tr("%n work so far, %1 of them on a graphics card.", nullptr, int(found.size())).arg(hardware);
    return note;
}

QString Backend::encoderNote() const {
    if (m_session.probing()) return probeNote();
    const std::vector<RankedEncoder> ranked = m_session.recordingEncoders();
    // An encoder on another graphics card than the one drawing the picture
    // takes every frame copied across: say so, and where the card is chosen.
    const auto across = [this](const EncoderInfo &e) {
        if (!onAnotherGpu(e, m_session.engineDevice())) return QString();
        return QLatin1Char(' ') + tr("It is on another graphics card than the one drawing the picture (%1), so every frame is copied across to it, "
                                     "which is slower and at 4K may not keep up. Settings → General → Graphics chooses the card that draws.")
                                      .arg(m_engineName);
    };
    if (!encoder().isEmpty()) {
        for (const RankedEncoder &r : ranked)
            if (QString::fromStdString(encoderKey(r.encoder)) == encoder()) {
                const QString muxer = QString::fromStdString(fileFormat(m_session.profile().recordingFormat()).muxer);
                if (isEditingCodec(r.encoder.codec) && muxer != QLatin1String("mov"))
                    return tr("Your choice: %1. Editors expect APV in a QuickTime (MOV) file; this records %2.")
                        .arg(QString::fromStdString(r.reason), containerName()) + across(r.encoder);
                return tr("Your choice: %1.").arg(QString::fromStdString(r.reason)) + across(r.encoder);
            }
        const EncoderInfo chosen = findEncoder(m_session.encoders(), encoder().toStdString());
        if (!chosen.name.empty()) if (const QString why = m_session.colourProblem(chosen); !why.isEmpty()) return why;
        return tr("Your choice can't record this canvas into %1; recording will say why.")
            .arg(containerName());
    }
    if (ranked.empty()) return tr("Nothing here can record this canvas into %1.").arg(containerName());
    // The one that would have been picked had it kept up, if there is one.
    QString slower;
    const std::vector<RankedEncoder> wished = m_session.recordingEncoders(true);
    if (!wished.empty() && encoderKey(wished.front().encoder) != encoderKey(ranked.front().encoder))
        slower = QLatin1Char(' ') + tr("%1 only managed %2 of %3 frames a second at this size.")
                                        .arg(QString::fromStdString(wished.front().encoder.label))
                                        .arg(int(m_session.encoderSpeed(encoderKey(wished.front().encoder)) + 0.5)).arg(fps());
    if (const std::string front = encoderKey(ranked.front().encoder); ranked.front().encoder.hardware && m_session.slowEncoders().count(front))
        return tr("No hardware encoder keeps up with %1 × %2 at %3 frames a second with room to spare, so %4, the fastest: it managed %5.")
                   .arg(canvasWidth()).arg(canvasHeight()).arg(fps()).arg(QString::fromStdString(ranked.front().encoder.label))
                   .arg(int(m_session.encoderSpeed(front) + 0.5)) + across(ranked.front().encoder);
    bool hardwareHere = false;
    for (const EncoderInfo &e : m_session.encoders()) hardwareHere |= e.available && e.hardware;
    if (!ranked.front().encoder.hardware && hardwareHere)
        return (m_session.slowEncoders().empty()
                    ? tr("No hardware encoder takes %1 × %2 into %3, so %4: %5.")
                    : tr("No hardware encoder keeps up with %1 × %2 into %3, so %4: %5."))
                   .arg(canvasWidth()).arg(canvasHeight()).arg(containerName())
                   .arg(QString::fromStdString(ranked.front().encoder.label), QString::fromStdString(ranked.front().reason)) + slower;
    return tr("Uses %1: %2.").arg(QString::fromStdString(ranked.front().encoder.label),
                                               QString::fromStdString(ranked.front().reason)) + slower + across(ranked.front().encoder);
}

void Backend::setFps(int fps) { setFrameRate(fps, 1); }

void Backend::setFrameRate(int num, int den) { applyFrameRate(num, den, {}); }

namespace {
// In lowest terms, and within what a profile keeps: a device's interval in
// 10-millionths of a second is not, and a profile holding it would not read
// next time. Past those limits the closest fraction that is (a continued
// fraction's convergents).
void keepableFrameRate(int &num, int &den) {
    const int common = std::gcd(num, den);
    num /= common;
    den /= common;
    constexpr int64_t kMaxNum = 240000, kMaxDen = 10000;
    if (num <= kMaxNum && den <= kMaxDen) return;
    int64_t n0 = 0, d0 = 1, n1 = 1, d1 = 0, a = num, b = den;
    while (b != 0) {
        const int64_t q = a / b, n2 = q * n1 + n0, d2 = q * d1 + d0;
        if (n2 > kMaxNum || d2 > kMaxDen) break;
        n0 = n1; d0 = d1; n1 = n2; d1 = d2;
        const int64_t r = a % b;
        a = b;
        b = r;
    }
    num = int(n1);
    den = int(d1);
}
}  // namespace

// `match`: the capture device the rate was taken from, named in the status.
void Backend::applyFrameRate(int num, int den, const QString &match) {
    if (num <= 0 || den <= 0 || double(num) / den < 1 || double(num) / den > 240) return;
    keepableFrameRate(num, den);
    if (num == fpsNum() && den == fpsDen()) return;
    m_session.profile().fpsNum = num;
    m_session.profile().fpsDen = den;
    saveProfile();
    m_session.refitEncoders();
    emit settingsChanged();
    emit frameRateNoteChanged();
    const QString rate = QString::number(double(num) / den, 'g', 5);
    // A rate is the profile's, and undo is the collection's: undoing what brought
    // this one leaves it, so that is said here.
    restartWhenIdle(match.isEmpty() ? tr("Frame rate: %1 a second.").arg(rate)
                                    : tr("Frame rate: %1 a second, to match %2. Undo leaves the rate as it is now.").arg(rate, match),
                    match.isEmpty() ? tr("Frame rate: %1 a second once recording, streaming, the replay buffer and the virtual camera stop.").arg(rate)
                                    : tr("Frame rate: %1 a second, to match %2, once recording, streaming, the replay buffer and the virtual camera stop. Undo leaves the rate as it is now.").arg(rate, match));
}

bool Backend::outputsBusy() const { return m_session.outputsBusy(); }

void Backend::restartSession() {
    m_audioHotkeys.stopGlobal();
    m_session.cancelAudioPush({}, ~0u);
    m_session.stop();
    m_session.start();
    refreshHotkeys();
    if (m_session.profile().globalHotkeys && !m_session.safeMode()) m_audioHotkeys.bindGlobal();
}

// What the picture only takes as it starts (the frame rate, the GPU): it
// restarts now, or once nothing records, streams, buffers or feeds the
// virtual camera, so no output is cut short.
void Backend::restartWhenIdle(const QString &done, const QString &waiting) {
    if (!m_session.running()) { setStatus(done); return; }
    if (outputsBusy()) {
        if (m_pendingRestart != done) setStatus(waiting);
        m_pendingRestart = done;
        return;
    }
    m_pendingRestart.clear();
    restartSession();
    setStatus(done);
}

// Once a second with the stats: a restart that waited for the outputs, and on
// Automatic, the picture moving to the GPU the power now calls for.
void Backend::followPowerAndPendingRestart() {
    if (!m_session.running()) return;
    if (!m_pendingRestart.isEmpty() && !outputsBusy()) {
        const QString done = std::exchange(m_pendingRestart, {});
        restartSession();
        setStatus(done);
        return;
    }
    if (m_powerChecked.isValid() && m_powerChecked.elapsed() < 3000) return;
    m_powerChecked.start();
    QString gpu;
    bool battery = false;
    if (!m_session.automaticGpuChanged(&gpu, &battery)) return;
    restartWhenIdle(battery ? tr("On battery: the picture is now drawn on %1 to save power.").arg(gpu)
                            : tr("Plugged in: the picture is now drawn on %1.").arg(gpu),
                    battery ? tr("On battery: the picture moves to %1 once recording, streaming, the replay buffer and the virtual camera stop.").arg(gpu)
                            : tr("Plugged in: the picture moves to %1 once recording, streaming, the replay buffer and the virtual camera stop.").arg(gpu));
}

QString Backend::colourRefusal(const Profile &candidate) const {
    using F = engine::ColourFormat;
    F fixed;
    const bool hdr = engine::isHdr(candidate.outputColour(F::NV12).space);
    if (hdr && candidate.fixedColourFormat(&fixed) && engine::formatBits(fixed) < 10)
        return tr("HDR needs 10 or more bits: choose P010, I010, I210, P216 or P416 as the colour format, or an SDR colour space.");
    if (m_session.probing() || m_session.encoders().empty()) return {};  // recording will say, once it knows
    if (!candidate.encoder.isEmpty()) {
        const EncoderInfo chosen = findEncoder(m_session.encoders(), candidate.encoder.toStdString());
        if (!chosen.name.empty()) return m_session.colourProblem(chosen, nullptr, &candidate);
    }
    // Best available: something here has to take it.
    QString first;
    for (const EncoderInfo &e : m_session.encoders()) {
        if (!e.available) continue;
        const QString why = m_session.colourProblem(e, nullptr, &candidate);
        if (why.isEmpty()) return {};
        if (first.isEmpty()) first = why;
    }
    if (hdr) return tr("No encoder here can encode the 10-bit pictures HDR needs.");
    if (candidate.fixedColourFormat(&fixed))
        return tr("No encoder here takes %1 pictures.").arg(QString::fromLatin1(colourFormatName(fixed)));
    return first;
}

bool Backend::applyColour(const Profile &candidate) {
    if (const QString why = colourRefusal(candidate); !why.isEmpty()) {
        setStatus(why);
        emit settingsChanged();  // the controls go back to what is kept
        return false;
    }
    Profile &p = m_session.profile();
    p.colourFormat = candidate.colourFormat;
    p.colourSpace = candidate.colourSpace;
    p.colourFullRange = candidate.colourFullRange;
    p.sdrWhite = candidate.sdrWhite;
    p.hdrPeak = candidate.hdrPeak;
    m_session.applyColourLevels();
    saveProfile();
    emit settingsChanged();
    if (recording() || streaming()) setStatus(tr("The new colour applies to recordings and streams started from now."));
    return true;
}

void Backend::setColourFormat(const QString &format) {
    if (format == colourFormat()) return;
    Profile candidate = m_session.profile();
    candidate.colourFormat = format;
    engine::ColourFormat f;
    if (format != QLatin1String("auto") && !candidate.fixedColourFormat(&f)) return;
    applyColour(candidate);
}

void Backend::setColourSpace(const QString &space) {
    if (space == colourSpace() || !QStringList{"srgb", "601", "709", "pq", "hlg"}.contains(space)) return;
    Profile candidate = m_session.profile();
    candidate.colourSpace = space;
    applyColour(candidate);
}

void Backend::setColourFullRange(bool full) {
    if (full == colourFullRange()) return;
    Profile candidate = m_session.profile();
    candidate.colourFullRange = full;
    applyColour(candidate);
}

void Backend::setSdrWhite(int nits) {
    nits = std::clamp(nits, 80, 1000);
    if (nits == sdrWhite()) return;
    Profile candidate = m_session.profile();
    candidate.sdrWhite = nits;
    applyColour(candidate);
}

void Backend::setDownscaleFilter(const QString &filter) {
    if (filter == downscaleFilter() || !QStringList{"area", "bilinear", "bicubic", "lanczos"}.contains(filter)) return;
    m_session.profile().downscaleFilter = filter;
    saveProfile();
    emit settingsChanged();
    if (recording() || streaming()) setStatus(tr("The new scaling applies to recordings and streams started from now."));
}

void Backend::setHdrPeak(int nits) {
    nits = std::clamp(nits, 400, 10000);
    if (nits == hdrPeak()) return;
    Profile candidate = m_session.profile();
    candidate.hdrPeak = nits;
    applyColour(candidate);
}

QVariantList Backend::colourFormats() const {
    using F = engine::ColourFormat;
    const Profile &p = m_session.profile();
    QVariantList out;
    // What "automatic" comes to for the recording encoder.
    QString automatic;
    std::vector<EncoderInfo> takers;
    if (!p.encoder.isEmpty()) {
        const EncoderInfo chosen = findEncoder(m_session.encoders(), p.encoder.toStdString());
        if (!chosen.name.empty()) takers.push_back(chosen);
    } else {
        for (const EncoderInfo &e : m_session.encoders()) if (e.available) takers.push_back(e);
    }
    Profile asAuto = p;
    asAuto.colourFormat = QStringLiteral("auto");
    const std::vector<RankedEncoder> ranked = m_session.recordingEncoders();
    const EncoderInfo recorder = !p.encoder.isEmpty() && !takers.empty() ? takers.front()
                               : !ranked.empty() ? ranked.front().encoder : EncoderInfo{};
    F resolved;
    if (!recorder.name.empty() && m_session.colourProblem(recorder, &resolved, &asAuto).isEmpty())
        automatic = QString::fromLatin1(colourFormatName(resolved));
    out.append(QVariantMap{{"value", "auto"}, {"label", automatic.isEmpty() ? tr("Automatic") : tr("Automatic (%1)").arg(automatic)}});
    static const char *ids[] = {"nv12", "i420", "i444", "p010", "i010", "p216", "p416", "bgra", "i210"};
    const char *about[] = {QT_TR_NOOP("8-bit 4:2:0"), QT_TR_NOOP("8-bit 4:2:0, planar"), QT_TR_NOOP("8-bit 4:4:4"),
                           QT_TR_NOOP("10-bit 4:2:0"), QT_TR_NOOP("10-bit 4:2:0, planar"), QT_TR_NOOP("16-bit 4:2:2"),
                           QT_TR_NOOP("16-bit 4:4:4"), QT_TR_NOOP("8-bit RGB"), QT_TR_NOOP("10-bit 4:2:2, planar")};
    static_assert(sizeof ids / sizeof ids[0] == engine::kColourFormats);
    for (int i = 0; i < engine::kColourFormats; ++i) {
        const bool taken = m_session.probing() ||
            std::any_of(takers.begin(), takers.end(), [&](const EncoderInfo &e) { return encoderTakes(e, F(i)); });
        const bool current = p.colourFormat == QLatin1String(ids[i]);
        if (!taken && !current) continue;
        QString label = QStringLiteral("%1 (%2)").arg(QString::fromLatin1(colourFormatName(F(i))), tr(about[i]));
        if (!taken) label = tr("%1, not taken here").arg(label);
        out.append(QVariantMap{{"value", QString::fromLatin1(ids[i])}, {"label", label}});
    }
    return out;
}

QString Backend::colourNote() const {
    using F = engine::ColourFormat;
    if (m_session.probing()) return tr("Checking which encoders work…");
    const Profile &p = m_session.profile();
    const std::vector<RankedEncoder> ranked = m_session.recordingEncoders();
    EncoderInfo recorder = !p.encoder.isEmpty() ? findEncoder(m_session.encoders(), p.encoder.toStdString())
                         : !ranked.empty() ? ranked.front().encoder : EncoderInfo{};
    if (recorder.name.empty()) return colourRefusal(p);
    F format;
    if (const QString why = m_session.colourProblem(recorder, &format); !why.isEmpty()) return why;
    const engine::OutputColour c = p.outputColour(format);
    static const char *spaces[] = {QT_TR_NOOP("sRGB"), QT_TR_NOOP("Rec. 601"), QT_TR_NOOP("Rec. 709"),
                                   QT_TR_NOOP("Rec. 2100 PQ"), QT_TR_NOOP("Rec. 2100 HLG")};
    return tr("Recordings: %1, %2, %3, through %4.")
        .arg(QString::fromLatin1(colourFormatName(format)), tr(spaces[int(c.space)]),
             c.fullRange ? tr("full range") : tr("limited range"), QString::fromStdString(recorder.label));
}

void Backend::setEncoder(const QString &name) {
    if (name == encoder()) return;
    Profile &p = m_session.profile();
    // An editing tier (light, standard, high) means nothing to the next
    // encoder, and an H.264 profile nothing to APV, even where the word is
    // the same ("high"): each starts from its default.
    const bool wasEditing = isEditingCodec(findEncoder(m_session.encoders(), p.encoder.toStdString()).codec);
    const bool isEditing = isEditingCodec(findEncoder(m_session.encoders(), name.toStdString()).codec);
    if (wasEditing != isEditing) p.recordingChoice.profile.clear();
    p.encoder = name;
    saveProfile();
    m_session.warmRecordingEncoder();
    emit settingsChanged();
}

void Backend::setQuality(int quality) {
    quality = std::clamp(quality, 0, 51);
    if (quality == this->quality()) return;
    m_session.profile().quality = quality;
    saveProfile();
    emit settingsChanged();
}

void Backend::setFolder(const QString &folder) {
    QString clean = folder.trimmed();
    // As a shell would read it; otherwise it would make a folder called "~".
    if (clean == QLatin1String("~") || clean.startsWith(QLatin1String("~/"))) clean = QDir::homePath() + clean.mid(1);
    // Relative to where the program happened to start is nowhere a person could find.
    if (!clean.isEmpty() && QDir::isRelativePath(clean)) {
        setStatus(tr("Recordings need a full path, such as %1.").arg(Profile::defaultFolder()));
        emit settingsChanged();  // the field goes back to what is kept
        return;
    }
    if (clean == this->folder()) {
        if (clean != folder) emit settingsChanged();
        return;
    }
    m_session.profile().folder = clean == Profile::defaultFolder() ? QString() : clean;
    saveProfile();
    emit settingsChanged();
}

void Backend::chooseRecordingFolder() {
    m_otherChosen = [this](const QString &chosen) { if (!chosen.isEmpty()) setFolder(chosen); };
    m_otherPicker.openFolder(tr("Where recordings go"), folder());
}

void Backend::setFileNamePattern(const QString &pattern) {
    const QString clean = pattern.trimmed().isEmpty() ? Profile{}.filenameTemplate : pattern.trimmed();
    if (clean == fileNamePattern()) return;
    m_session.profile().filenameTemplate = clean;
    saveProfile();
    emit settingsChanged();
}

void Backend::setFileNameNoSpaces(bool noSpaces) {
    if (noSpaces == fileNameNoSpaces()) return;
    m_session.profile().filenameNoSpaces = noSpaces;
    saveProfile();
    emit settingsChanged();
}

QString Backend::fileNamePreview(const QString &pattern, bool noSpaces) const {
    return expandFileName(pattern, QDateTime::currentSecsSinceEpoch(), m_session.fileNameFields(), noSpaces) +
           QLatin1Char('.') + QString::fromLatin1(fileFormat(m_session.profile().recordingFormat()).extension);
}

void Backend::setRecordingTracks(int tracks) {
    tracks &= 63;
    if (m_session.recording() || tracks == recordingTracks()) return;
    m_session.profile().recordingTracks = unsigned(tracks);
    saveProfile(); emit settingsChanged();
}

void Backend::setContainer(const QString &container) {
    if (container == this->container()) return;
    m_session.profile().container = container;
    saveProfile();
    emit settingsChanged();
}

void Backend::setDesktopMuted(bool muted) {
    if (muted == desktopMuted()) return;
    m_session.profile().desktopMuted = muted;
    m_session.applyAudioSettings();
    saveProfile();
    emit settingsChanged();
}

void Backend::setMicrophoneMuted(bool muted) {
    if (muted == microphoneMuted()) return;
    m_session.profile().microphoneMuted = muted;
    m_session.applyAudioSettings();
    saveProfile();
    emit settingsChanged();
}

void Backend::setDesktopAdded(bool added) {
    if (added == desktopAdded()) return;
    m_session.profile().desktopAdded = added;
    m_session.applyAudioSettings();
    saveProfile();
    refreshHotkeys();
    emit settingsChanged();
}

void Backend::setMicrophoneAdded(bool added) {
    if (added == microphoneAdded()) return;
    m_session.profile().microphoneAdded = added;
    m_session.setMicrophoneChoice(m_session.profile().microphone);
    m_session.applyAudioSettings();
    saveProfile();
    refreshHotkeys();
    emit settingsChanged();
}

bool Backend::noSound() const {
    const Profile &p = m_session.profile();
    if (p.desktopAdded || (p.microphoneAdded && p.microphone != QLatin1String("none"))) return false;
    for (const SourceDef &device : p.globalAudioDevices)
        if (!device.settings.value(QStringLiteral("audioNode")).toString().isEmpty()) return false;
    for (const SourceDef &source : m_production.collection().sources) {
        if (!source.hasAudio()) continue;
        const bool picksInput = source.kind == QLatin1String("device") || SourceDef::isAudioKind(source.kind);
        if (!picksInput || !source.settings.value(QStringLiteral("audioNode")).toString().isEmpty()) return false;
    }
    return true;
}

// What is already taken, so it is not offered twice.
QStringList Backend::audioInputsInUse() const {
    QStringList used;
    const Profile &p = m_session.profile();
    if (p.microphoneAdded && !m_session.microphone().name.empty()) used << QString::fromStdString(m_session.microphone().name);
    for (const SourceDef &device : p.globalAudioDevices)
        if (device.kind == QLatin1String("audio-input")) used << device.settings.value(QStringLiteral("audioNode")).toString();
    for (const SourceDef &source : m_production.collection().sources)
        if (source.kind == QLatin1String("device") || source.kind == QLatin1String("audio-input")) used << source.settings.value(QStringLiteral("audioNode")).toString();
    used.removeAll(QString());
    used.removeDuplicates();
    return used;
}

QVariantList Backend::sourceAudioChannels() const {
    QVariantList channels;
    for (const SourceDef &source : m_production.collection().sources) {
        if (!source.hasAudio()) continue;
        channels.append(QVariantMap{{QStringLiteral("id"), source.id}, {QStringLiteral("name"), source.name},
                                   {QStringLiteral("device"), source.kind == QLatin1String("device") || SourceDef::isAudioKind(source.kind)},
                                   {QStringLiteral("camera"), source.kind == QLatin1String("device")},
                                   {QStringLiteral("audioLabel"), source.settings.value(QStringLiteral("audioLabel"), source.name)},
                                   {QStringLiteral("audioKind"), source.kind == QLatin1String("device") ? QStringLiteral("audio-input") : source.kind},
                                   {QStringLiteral("active"), m_session.sourceAudioActive(source.id)},
                                   {QStringLiteral("processing"), AudioSettings::normalized(source.settings)},
                                   {QStringLiteral("gain"), AudioSettings::gain(source.settings.value(QStringLiteral("volume"), 1.0).toDouble())},
                                   {QStringLiteral("error"), m_session.sourceAudioError(source.id)},
                                   {QStringLiteral("muted"), source.settings.value(QStringLiteral("muted"), false)},
                                   {QStringLiteral("audioNode"), source.settings.value(QStringLiteral("audioNode"), QString())}});
    }
    return channels;
}

QVariantList Backend::globalAudioDevices() const {
    QVariantList devices;
    for (const auto &device : m_session.profile().globalAudioDevices)
        devices.append(QVariantMap{{"id", device.id}, {"name", device.name}, {"role", "global"}, {"device", true},
            {"audioKind", device.kind}, {"audioNode", device.settings.value("audioNode")}, {"audioLabel", device.settings.value("audioLabel")},
            {"processing", AudioSettings::normalized(device.settings)}, {"gain", AudioSettings::gain(device.settings.value("volume", 1.).toDouble())},
            {"muted", device.settings.value("muted", false)}, {"error", m_session.globalAudioDeviceStatus(device.id)}});
    return devices;
}

QString Backend::addGlobalAudioDevice(const QString &kind) {
    if (kind != "audio-input" && kind != "audio-output") return {};
    auto &devices = m_session.profile().globalAudioDevices;
    const QString base = kind == "audio-input" ? tr("Global input") : tr("Global output");
    QString name = base;
    for (int i = 2; std::any_of(devices.begin(), devices.end(), [&](const auto &d) { return d.name == name; }); ++i)
        name = base + " " + QString::number(i);
    SourceDef device{QUuid::createUuid().toString(QUuid::WithoutBraces), kind, name, AudioSettings::normalized({})};
    device.settings.insert("audioNode", QString()); device.settings.insert("audioLabel", QString());
    device.settings.insert("volume", 1.); device.settings.insert("muted", false);
    devices.append(device);
    m_session.applyAudioSettings(); saveProfile(); emit settingsChanged();
    return device.id;
}
void Backend::removeGlobalAudioDevice(const QString &id) {
    auto &devices = m_session.profile().globalAudioDevices;
    if (!devices.removeIf([&](const auto &d) { return d.id == id; })) return;
    m_globalAudioDeviceMeters.remove(id);
    m_session.applyAudioSettings(); saveProfile(); emit settingsChanged(); emit levelsChanged();
}
void Backend::setGlobalAudioDeviceSetting(const QString &id, const QString &key, const QVariant &value) {
    auto &devices = m_session.profile().globalAudioDevices;
    auto it = std::find_if(devices.begin(), devices.end(), [&](const auto &d) { return d.id == id; });
    if (it == devices.end()) return;
    SourceDef next = *it;
    if (key == "name") {
        const auto name = value.toString().trimmed();
        if (name.isEmpty()) return;
        next.name = name;
    } else if (key == "volume") next.settings[key] = AudioSettings::gain(value.toDouble());
    else if (key == "muted") next.settings[key] = value.toBool();
    else if (AudioSettings::isKey(key)) {
        next.settings[key] = value;
        next.settings[key] = AudioSettings::normalized(next.settings).value(key);
    } else if (key == "audioNode") {
        next.settings[key] = value.toString(); next.settings["audioLabel"] = QString();
        for (const auto &entry : audioSourceTargets()) {
            const auto row = entry.toMap();
            if (row.value("kind") == next.kind && row.value("value") == value) next.settings["audioLabel"] = row.value("label");
        }
    } else return;
    if (next == *it) return;
    *it = next;
    m_session.applyAudioSettings(); saveProfile(); emit settingsChanged();
}
void Backend::resetGlobalAudioDevicePeak(const QString &id) {
    m_session.resetGlobalAudioDeviceMeter(id);
    auto meter = m_globalAudioDeviceMeters.value(id).toMap();
    meter["maximumDb"] = -120; meter["clipped"] = false; m_globalAudioDeviceMeters[id] = meter;
    emit levelsChanged();
}

QVariantList Backend::audioSourceTargets() const {
    QVariantList targets;
    QSet<QString> seen;
    for (const AudioNode &node : m_session.audioNodes()) {
        const QString kind = node.kind == AudioNode::Input ? QStringLiteral("audio-input")
                           : node.kind == AudioNode::Output ? QStringLiteral("audio-output") : QStringLiteral("audio-application");
        const QString value = QString::fromStdString(node.kind == AudioNode::Application ? node.application : node.name);
        const QString key = kind + value;
        if (seen.contains(key)) continue;
        seen.insert(key);
        targets.append(QVariantMap{{QStringLiteral("kind"), kind}, {QStringLiteral("value"), value},
                                   {QStringLiteral("label"), QString::fromStdString(node.kind == AudioNode::Application
                                                                               ? node.applicationName : node.description)}});
    }
    std::sort(targets.begin(), targets.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("label")).toString().localeAwareCompare(b.toMap().value(QStringLiteral("label")).toString()) < 0;
    });
    return targets;
}

void Backend::addAudioSource(const QString &kind, const QString &target) {
    if (!SourceDef::isAudioKind(kind)) return;
    QString label = kind == QLatin1String("audio-input") ? tr("Audio input")
                  : kind == QLatin1String("audio-output") ? tr("Audio output") : tr("Application audio");
    for (const QVariant &item : audioSourceTargets()) {
        const QVariantMap row = item.toMap();
        if (row.value(QStringLiteral("kind")) == kind && row.value(QStringLiteral("value")) == target) {
            label = row.value(QStringLiteral("label")).toString();
            break;
        }
    }
    m_production.addSource(kind, label, {{QStringLiteral("audioNode"), target}, {QStringLiteral("audioLabel"), label}}, {});
}

QVariantList Backend::sourceAudioInputs() const {
    QVariantList inputs{QVariantMap{{QStringLiteral("value"), QString()}, {QStringLiteral("label"), tr("Off")}}};
    QSet<QString> found;
    for (const AudioInput &input : m_session.audioInputs()) {
        const QString name = QString::fromStdString(input.name);
        found.insert(name);
        inputs.append(QVariantMap{{QStringLiteral("value"), name},
                                  {QStringLiteral("label"), QString::fromStdString(input.description)}});
    }
    for (const SourceDef &source : m_production.collection().sources) {
        const QString node = source.settings.value(QStringLiteral("audioNode")).toString();
        if (node.isEmpty() || found.contains(node)) continue;
        found.insert(node);
        inputs.append(QVariantMap{{QStringLiteral("value"), node},
                                  {QStringLiteral("label"), tr("%1 (not connected)").arg(node)}});
    }
    return inputs;
}

QVariantList Backend::microphones() const {
    const MicrophonePick pick = m_session.microphone();
    const std::vector<AudioInput> inputs = m_session.audioInputs();
    auto entry = [](const QString &value, const QString &label) {
        return QVariantMap{{QStringLiteral("value"), value}, {QStringLiteral("label"), label}};
    };
    QVariantList out;
    const bool automatic = microphone().isEmpty();
    out << entry(QString(), automatic && pick.state == MicrophonePick::Using
                                ? tr("Automatic (%1)").arg(QString::fromStdString(pick.description))
                                : QStringLiteral("Automatic"));
    for (const AudioInput &in : inputs)
        if (in.hardware) out << entry(QString::fromStdString(in.name), QString::fromStdString(in.description));
    // Virtual inputs (loopbacks, filters) can be chosen, never picked for you.
    for (const AudioInput &in : inputs)
        if (!in.hardware)
            out << entry(QString::fromStdString(in.name),
                         QStringLiteral("%1 (virtual)").arg(QString::fromStdString(in.description)));
    if (pick.state == MicrophonePick::Missing)
        out << entry(microphone(), QStringLiteral("%1 (not connected)").arg(microphone()));
    out << entry(QStringLiteral("none"), QStringLiteral("Off"));
    return out;
}

QString Backend::microphoneStatus() const {
    switch (m_session.microphone().state) {
    case MicrophonePick::Using: return QString();
    case MicrophonePick::NoneFound: return tr("No microphone found");
    case MicrophonePick::Off: return QStringLiteral("Off");
    case MicrophonePick::Missing: return tr("Not connected");
    }
    return QString();
}

void Backend::setMicrophone(const QString &choice) {
    if (choice == microphone()) return;
    m_session.setMicrophoneChoice(choice);
    saveProfile();
}

void Backend::setMonitorOutput(const QString &choice) {
    if (choice == monitorOutput()) return;
    m_session.profile().monitorOutput = choice;
    m_session.applyAudioSettings();
    saveProfile(); emit settingsChanged();
}

QVariantList Backend::monitorOutputs() const {
    QVariantList outputs{QVariantMap{{"label", tr("Off")}, {"value", "none"}},
                         QVariantMap{{"label", tr("Default output")}, {"value", ""}}};
    bool found = monitorOutput().isEmpty() || monitorOutput() == QLatin1String("none");
    for (const QVariant &entry : audioSourceTargets()) {
        const auto row = entry.toMap();
        if (row.value(QStringLiteral("kind")) != QLatin1String("audio-output")) continue;
        outputs.append(row);
        found |= row.value(QStringLiteral("value")).toString() == monitorOutput();
    }
    if (!found) outputs.append(QVariantMap{{"label", tr("%1 (not connected)").arg(monitorOutput())}, {"value", monitorOutput()}});
    return outputs;
}

QStringList Backend::audioChannelLabels() const {
    QStringList result;
    const auto labels = AudioFormat::labels(audioChannelCount());
    for (int c = 0; c < audioChannelCount(); ++c) result.append(QString::fromLatin1(labels[c]));
    return result;
}
void Backend::setAudioSampleRate(int rate) {
    if (rate == audioSampleRate()) return;
    if (outputsBusy()) { setStatus(tr("The sample rate can change once recording, streaming, the replay buffer and the virtual camera stop.")); return; }
    if (!m_session.setAudioFormat({rate, audioChannelCount()})) return;
    m_desktopMeter.clear(); m_microphoneMeter.clear(); m_sourceAudioMeters.clear(); m_globalAudioDeviceMeters.clear();
    saveProfile(); emit settingsChanged(); emit levelsChanged();
}
void Backend::setAudioChannelCount(int channels) {
    if (channels == audioChannelCount()) return;
    if (outputsBusy()) { setStatus(tr("The channels can change once recording, streaming, the replay buffer and the virtual camera stop.")); return; }
    if (!m_session.setAudioFormat({audioSampleRate(), channels})) return;
    m_desktopMeter.clear(); m_microphoneMeter.clear(); m_sourceAudioMeters.clear(); m_globalAudioDeviceMeters.clear();
    saveProfile(); emit settingsChanged(); emit levelsChanged();
}

QVariantMap Backend::audioPushStates() const {
    QVariantMap result;
    const auto append = [&](const QString &target) {
        result[target+":talk"] = m_session.audioPushHeld(target, true);
        result[target+":mute"] = m_session.audioPushHeld(target, false);
    };
    append("desktop"); append("microphone");
    for (const auto &device : m_session.profile().globalAudioDevices) append("global:"+device.id);
    for (const auto &source : m_production.collection().sources) if (source.hasAudio()) append("source:"+source.id);
    return result;
}
void Backend::refreshHotkeys() {
    QList<Hotkeys::Binding> bindings;
    const auto append = [&](const QString &target, const QString &name, const QVariantMap &settings) {
        for (bool talk : {true, false}) {
            if (!settings.value(talk ? "pushToTalk" : "pushToMute").toBool()) continue;
            const QString action = talk ? "talk" : "mute";
            bindings.append({target+":"+action, target, talk ? tr("%1 — hold to talk").arg(name) : tr("%1 — hold to mute").arg(name),
                talk, QKeySequence::fromString(settings.value(talk ? "talkShortcut" : "muteShortcut").toString(), QKeySequence::PortableText), {}, {}});
        }
    };
    if (desktopAdded()) append("desktop", tr("Desktop"), desktopAudioSettings());
    if (microphoneAdded()) append("microphone", tr("Microphone"), microphoneAudioSettings());
    for (const auto &device : m_session.profile().globalAudioDevices) append("global:"+device.id, device.name, device.settings);
    for (const auto &source : m_production.collection().sources) if (source.hasAudio()) append("source:"+source.id, source.name, source.settings);
    // Keys for actions that still have what they act on.
    const QVariantMap keys = hotkeys();
    for (const QVariant &entry : actions()) {
        const QVariantMap a = entry.toMap();
        const QString id = a.value("id").toString() + QLatin1Char('|') + a.value("argument").toString();
        const QKeySequence key = QKeySequence::fromString(keys.value(id).toString(), QKeySequence::PortableText);
        if (key.isEmpty()) continue;
        const Hotkeys::Binding binding{QStringLiteral("action:") + id, {}, a.value("name").toString(), true, key,
                                       a.value("id").toString(), a.value("argument").toString()};
        bindings.append(binding);
    }
    m_audioHotkeys.setBindings(bindings);
    emit audioPushChanged();
}
void Backend::setLocalHotkeysEnabled(bool enabled) { m_localHotkeysEnabled = enabled; m_audioHotkeys.setEnabled(enabled); }
void Backend::applyGlobalHotkeys() { refreshHotkeys(); m_audioHotkeys.bindGlobal(); }
void Backend::stopGlobalHotkeys() { m_session.profile().globalHotkeys = false; saveProfile(); m_audioHotkeys.stopGlobal(); }

void Backend::setLowLatencyAudio(bool low) {
    if (low == lowLatencyAudio()) return;
    if (outputsBusy()) { setStatus(tr("Low latency can change once recording, streaming, the replay buffer and the virtual camera stop.")); emit settingsChanged(); return; }
    m_session.profile().lowLatencyAudio = low;
    m_session.applyAudioSettings();
    saveProfile(); emit settingsChanged();
}

void Backend::setActiveAudioOnly(bool active) {
    if (active == activeAudioOnly()) return;
    m_session.profile().activeAudioOnly = active;
    saveProfile(); emit settingsChanged();
}

QVariantMap Backend::meterSettings() const { return MeterSettings::normalized(m_session.profile().meter); }

void Backend::setMeterSetting(const QString &key, const QVariant &value) {
    auto next = meterSettings();
    if (!next.contains(key)) return;
    next.insert(key, value); next = MeterSettings::normalized(next);
    if (next == meterSettings()) return;
    if (next.value("truePeak") != meterSettings().value("truePeak")) {
        m_desktopMeter.clear(); m_microphoneMeter.clear(); m_sourceAudioMeters.clear(); m_globalAudioDeviceMeters.clear();
    }
    m_session.profile().meter = next;
    m_session.applyAudioSettings();
    saveProfile(); emit settingsChanged();
}

void Backend::resetMeterPeak(int globalChannel, const QString &sourceId) {
    m_session.resetAudioMeterMaximum(globalChannel, sourceId);
    if (globalChannel == 0) { m_desktopMeter["maximumDb"] = -120; m_desktopMeter["clipped"] = false; }
    else if (globalChannel == 1) { m_microphoneMeter["maximumDb"] = -120; m_microphoneMeter["clipped"] = false; }
    else { auto meter = m_sourceAudioMeters.value(sourceId).toMap(); meter["maximumDb"] = -120; meter["clipped"] = false; m_sourceAudioMeters[sourceId] = meter; }
    emit levelsChanged();
}

QVariantList Backend::audioFilterTypes() const { return AudioFilterSettings::catalog(); }

QVariantMap Backend::desktopAudioSettings() const { return AudioSettings::normalized(m_session.profile().desktopAudio); }
QVariantMap Backend::microphoneAudioSettings() const { return AudioSettings::normalized(m_session.profile().microphoneAudio); }

void Backend::setGlobalAudioSetting(int channel, const QString &key, const QVariant &value) {
    if (channel != AudioCapture::Desktop && channel != AudioCapture::Microphone) return;
    if (!AudioSettings::isKey(key)) return;
    auto &settings = channel == AudioCapture::Desktop ? m_session.profile().desktopAudio : m_session.profile().microphoneAudio;
    QVariantMap next = settings;
    next.insert(key, value);
    next = AudioSettings::normalized(next);
    if (next == settings) return;
    settings = next;
    m_session.applyAudioSettings();
    saveProfile();
    emit settingsChanged();
}

void Backend::setDesktopGain(qreal gain) {
    gain = AudioSettings::gain(gain);
    if (qFuzzyCompare(gain, desktopGain())) return;
    m_session.profile().desktopGain = float(gain);
    m_session.applyAudioSettings();
    saveProfile();
    emit settingsChanged();
}

void Backend::setMicrophoneGain(qreal gain) {
    gain = AudioSettings::gain(gain);
    if (qFuzzyCompare(gain, microphoneGain())) return;
    m_session.profile().microphoneGain = float(gain);
    m_session.applyAudioSettings();
    saveProfile();
    emit settingsChanged();
}

QString Backend::formatDuration(qint64 ns) {
    const qint64 s = ns / 1'000'000'000ll;
    return QStringLiteral("%1:%2:%3")
        .arg(s / 3600, 2, 10, QLatin1Char('0'))
        .arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString Backend::formatBytes(quint64 bytes) {
    if (bytes < 1024ull * 1024) return QStringLiteral("%1 KB").arg(bytes / 1024);
    if (bytes < 1024ull * 1024 * 1024) return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 2);
}

qreal Backend::levelToMeter(float peak) {
    if (peak <= 0.f) return 0;
    const double db = 20.0 * std::log10(double(peak));
    return std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
}

void Backend::toggleRecording() {
    if (m_session.recordingStopping()) return;
    if (m_session.recording()) {
        m_session.stopRecording(false);
        return;
    }
    startRecording();
}

QString Backend::startRecording() {
    QString error;
    if (!m_session.startRecording(&error)) {
        setStatus(error);
        emit failed(error);
        emit recordingRefused(error);
        return error;
    }
    setStatus(tr("Recording to %1 with %2").arg(recordingFile(), m_session.recordingEncoder())
              + (noSound() ? QLatin1Char(' ') + tr("No sound: nothing in the Audio mixer is on.") : QString()));
    return {};
}

void Backend::setPreviewEnabled(bool on) { m_session.setPreviewEnabled(on); }
void Backend::setPreviewSize(int width, int height) { m_session.setPreviewSize(width, height); }

void Backend::addImageFile(const QString &path) {
    Transform t;
    QImageReader reader(path);
    const QSize size = reader.size();
    // Native size, scaled down to fit when it is bigger than the canvas.
    if (size.isValid() && (size.width() > canvasWidth() || size.height() > canvasHeight())) {
        const double s = std::min(double(canvasWidth()) / size.width(), double(canvasHeight()) / size.height());
        t.scaleX = t.scaleY = s;
    }
    m_production.addSource(QStringLiteral("image"), QFileInfo(path).completeBaseName(), {{QStringLiteral("path"), path}}, t);
}

void Backend::addMediaFile(const QString &path) {
    m_production.addSource(QStringLiteral("media"), QFileInfo(path).completeBaseName(),
                           {{QStringLiteral("input"), path}, {QStringLiteral("loop"), false},
                            {QStringLiteral("restart"), true}, {QStringLiteral("volume"), 1.0}},
                           canvasFit());
}

void Backend::open(const QUrl &url) {
    // "Open with", or a file named on the command line: an exported profile
    // or collection is imported; a picture or a video goes into the current
    // studio as a source. Before start() nothing is open yet (the launch card
    // or the crash sheet is up): it waits, and is opened as start() ends.
    if (m_crashOffered || !m_started) {
        m_openAfterCrash = url;
        return;
    }
    const QString path = url.toLocalFile();
    if (path.isEmpty() || !QFileInfo(path).isFile()) {
        setStatus(tr("%1 is not a file here.").arg(url.toDisplayString()));
        return;
    }
    if (path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive)) {
        QFile f(path);
        const QJsonObject o = f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
        if (o.value(QStringLiteral("scenes")).isArray()) importCollectionFrom(path);
        else importProfileFrom(path);  // says so when it is neither
        return;
    }
    // A still picture by its content; an animated one is an image source too.
    QImageReader reader(path);
    if (reader.canRead() && !QStringList{"mp4", "mkv", "mov", "webm", "avi"}.contains(QFileInfo(path).suffix().toLower()))
        addImageFile(path);
    else
        addMediaFile(path);
    setStatus(tr("Added %1 to the studio.").arg(QFileInfo(path).fileName()));
}

void Backend::copyText(const QString &text) { QGuiApplication::clipboard()->setText(text); }

void Backend::showRecordings() {
    const QString folder = m_session.profile().resolvedFolder();
    QDir().mkpath(folder);
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void Backend::setStatus(const QString &status) {
    if (m_status == status) return;
    m_status = status;
    emit statusChanged();
}

QVariantMap Backend::selectedSlideshow() const {
    const auto *item = m_production.collection().findItem(selectedItem());
    return item ? m_session.slideshowPlayback(item->source) : QVariantMap{};
}
void Backend::controlSlideshow(const QString &sourceId, const QString &action, double value) {
    if (m_session.controlSlideshow(sourceId, action, value)) emit slideshowPlaybackChanged();
}

QVariantMap Backend::selectedMedia() const {
    const auto *item = m_production.collection().findItem(selectedItem());
    return item ? m_session.mediaPlayback(item->source) : QVariantMap{};
}
void Backend::controlMedia(const QString &sourceId,const QString &action,double value) {
    if (m_session.controlMedia(sourceId,action,value)) emit mediaPlaybackChanged();
}

void Backend::refreshStats() {
    emit mediaPlaybackChanged();
    emit slideshowPlaybackChanged();
    // Late frames over the last five seconds (framesLateLately).
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        m_lateHistory.emplace_back(now, m_session.stats().lateTicks);
        while (m_lateHistory.size() > 1 && now - m_lateHistory.front().first > 5000) m_lateHistory.pop_front();
    }
    // The selected camera's picture rate settles over a few seconds: its settings say when it stops fitting the canvas.
    {
        const SceneItem *item = m_production.collection().findItem(selectedItem());
        const QVariantMap even = item ? evenRateFor(item->source) : QVariantMap();
        if (even != m_selectedEvenRate) { m_selectedEvenRate = even; emit selectionChanged(); }
    }
    const Session::Stats s = m_session.stats();
    m_engineName = s.engine;
    m_engineWhy = s.engineWhy;
    m_engineOnBattery = s.engineOnBattery;
    QStringList parts;
    parts << QStringLiteral("%1 fps").arg(s.fps, 0, 'f', 1);
    if (s.lateTicks) parts << QStringLiteral("%1 late").arg(s.lateTicks);
    if (s.recording) {
        parts << QStringLiteral("%1 encoded").arg(s.recorder.framesEncoded);
        if (s.recorder.framesDropped) parts << QStringLiteral("%1 dropped").arg(s.recorder.framesDropped);
        parts << QStringLiteral("%1 ms/frame").arg(s.recorder.encodeMs, 0, 'f', 1);
        parts << formatBytes(s.recorder.bytesWritten);
        const QStorageInfo disk(m_session.profile().resolvedFolder());
        if (disk.isValid() && s.recorder.durationNs > 2'000'000'000ll && s.recorder.bytesWritten > 0) {
            const double bytesPerSecond = s.recorder.bytesWritten / (s.recorder.durationNs / 1e9);
            const double secondsLeft = disk.bytesAvailable() / bytesPerSecond;
            parts << QStringLiteral("%1 of disk left").arg(formatDuration(qint64(secondsLeft * 1e9)));
        }
        m_recordingTime = formatDuration(s.recorder.durationNs);
    } else {
        m_recordingTime.clear();
    }
    // Each stream: how it is going, at a glance.
    for (const Session::StreamStatus &st : m_session.streamStatus()) {
        QString line = st.state == QLatin1String("live") ? QStringLiteral("%1 %2 kb/s").arg(st.name).arg(st.kbps, 0, 'f', 0)
                                                         : QStringLiteral("%1 %2").arg(st.name, st.state);
        if (st.framesDropped) line += QStringLiteral(" · %1 dropped").arg(st.framesDropped);
        if (st.delayMs > 0) line += tr(" · %1 s behind").arg(st.delayMs / 1000);
        parts << line;
    }
    if (m_session.replayActive()) parts << tr("Replay %1").arg(replayHeld());
    if (m_session.virtualCameraInUse()) parts << tr("Camera in use");
    m_streamingTime = m_session.streaming() && m_streamClock.isValid() ? formatDuration(m_streamClock.nsecsElapsed()) : QString();
    m_statsLine = parts.join(QStringLiteral("  ·  "));
    emit statsChanged();
}

bool Backend::saveProfile() {
    return m_session.profile().save(m_profileId);
}
