#pragma once

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>
#include <QTimer>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include "audio/audio_capture.h"
#include "audio/audio_playback.h"
#include "audio/audio_devices.h"
#include "audio/mixer.h"
#include "capture/browser_page.h"
#include "capture/device_controls.h"
#include "capture/device_stream.h"
#include "capture/media_stream.h"
#include "capture/screencast_portal.h"
#include "capture/video_devices.h"
#include "capture/video_stream.h"
#include "capture/direct_capture.h"
#include "core/drawn.h"
#include "core/production.h"
#include "core/profile.h"
#include "core/region.h"
#include "core/deinterlace.h"
#include "output/encodings.h"
#include "output/file_sink.h"
#include "output/services.h"
#include "output/stream_sink.h"
#include "engine/engine.h"
#include "output/encoders.h"
#include "output/recorder.h"
#include "output/remux.h"
#include "output/replay_sink.h"
#include "output/virtual_camera.h"

// Declared just after the application. A job or a stream the system held past
// the session's wait can wake at any moment and go on using fonts and image
// plugins, which go with the application and the statics: while one is still
// running the program ends here with `code`, leaving them in place.
struct LeaveAroundHeldWork {
    ~LeaveAroundHeldWork();
    int code = 0;
};

// The running production: sources feeding the engine on its own thread at
// the canvas frame rate, audio on an independent clock, a recorder when asked.
// Lives on the main thread and owns separate frame and audio workers.
class Session : public QObject {
    Q_OBJECT

public:
    // A source as it is running: what it shows and how it is doing.
    struct FeedInfo {
        QString sourceId;
        QString kind;
        QString state;        // "waiting for the picker", "live", "cancelled", "failed: …", …
        int width = 0, height = 0;
        bool dmabuf = false;
        bool live = false;
        bool resident = false; // a picture has been uploaded and not released
        uint64_t frames = 0, dropped = 0;
        QString mode;         // a capture device's mode, for people
        double pictureRate = 0;  // a capture device's different pictures a second (DeviceStream::pictureRate)
        QString codec;        // a media source's video codec
        bool hardwareDecode = false;
        bool network = false;
        QString refusedSound; // the codec of a sound stream a listener would not take
    };
    struct Stats {
        double fps = 0;            // ticks actually completed per second
        uint64_t audioLateFrames = 0, monitorUnderruns = 0, monitorQueuedFrames = 0;
        int monitorQuantumFrames = 0;
        uint64_t lateTicks = 0;
        double renderMs = 0, renderWorstMs = 0;  // drawing one frame, over the last second
        QString engine;
        QString engineWhy;  // why that one, when it is not the plain automatic choice
        bool engineOnBattery = false;  // the integrated GPU, picked to save the battery
        bool gpu = false;
        float desktopPeak[2] = {0, 0};
        float microphonePeak[2] = {0, 0};
        Recorder::Stats recorder;
        bool recording = false;
        QString recordingPath;
    };

    explicit Session(QObject *parent = nullptr, std::string waylandSocket = {});
    ~Session() override;

    Profile &profile() { return m_profile; }
    const Profile &profile() const { return m_profile; }
    void setProfile(const Profile &p);
    // Source ids repeat between collections: saved screen choices are kept
    // per collection. Empty for the Default collection (as before there were others).
    void setTokenScope(const QString &scope) { m_tokenScope = scope; }
    // Drops every feed and starts afresh from the collection (after a switch).
    void restartFeeds() {
        m_restartFeeds = true;
        m_launching = true;
        syncFeeds();
        m_launching = false;
    }
    QString tokenKey(const QString &sourceId) const {
        return m_tokenScope.isEmpty() ? sourceId : m_tokenScope + QLatin1Char('/') + sourceId;
    }

    // Probes encoders (slow), starts the engine thread and audio capture.
    void start();
    void stop();
    bool running() const { return m_running.load(); }
    bool probing() const { return m_probing.load(); }
    // While probing: the encoder being tried now (its label) and those found to work so far.
    QString probeNow() const;
    std::vector<EncoderInfo> probeFound() const;
    DirectCapture::Catalog directCaptureCatalog() { return m_directCaptures.catalog(); }

    const std::vector<EncoderInfo> &encoders() const { return m_encoders; }
    // The device that composites; empty on the CPU engine.
    EngineDevice engineDevice() const {
        std::lock_guard<std::mutex> lock(m_encoderMutex);
        return m_engineDevice;
    }
    // Encoders for recording into the profile's container at the current
    // canvas, best first. Empty while probing or when nothing works.
    std::vector<RankedEncoder> recordingEncoders(bool evenSlow = false) const;
    // Makes the hardware device of the encoder a recording would use now, in
    // the background, so starting to record does not wait for it.
    void warmRecordingEncoder();
    // Encoders proven unable to take the current canvas (by encoderKey):
    // those that would not open at its size, and those that did but, timed,
    // did not keep up with its frame rate.
    std::set<std::string> unfitEncoders() const;
    // The timed ones that did not keep up, and what any timed one managed
    // (frames a second; 0 when it was not timed).
    std::set<std::string> slowEncoders() const;
    // Why an output cannot start while the encoders are being checked.
    QString stillChecking() const;
    double encoderSpeed(const std::string &key) const;
    // Re-tests every encoder at the canvas size, off the main thread.
    void refitEncoders();
    // The format an encoder takes the profile's colour in: the chosen one,
    // or with "auto" its usual one (a 10-bit one it opened with for HDR).
    // Empty when it fits; otherwise why not, in words for people.
    // `profile` checks another profile's colour (a choice not made yet).
    QString colourProblem(const EncoderInfo &encoder, engine::ColourFormat *format = nullptr, const Profile *profile = nullptr) const;
    // The profile's SDR white and HDR peak reach the engine from the next frame.
    void applyColourLevels();

    // The scene collection to show. Each of its sources becomes a feed that
    // keeps running unless its inactive-resource option is enabled. The live
    // scene's visible items, including nested studios, determine activity.
    void setProduction(Production *production);
    std::vector<FeedInfo> feeds() const;
    // Pixel size of a source's pictures; false until it has any.
    bool feedSize(const QString &sourceId, int *width, int *height) const;
    QSize captureSize(const QString &sourceId) const;
    // Where the display a capture shows sits in the compositor's layout
    // (logical points), as its portal stream said; empty when it did not.
    QRect captureLayoutArea(const QString &sourceId) const;
    // Why some of a source's video filters are left out (a file that cannot be used).
    QStringList videoFilterProblems(const QString &sourceId) const;
    // Asks the portal again for a capture source (after a cancel or a close).
    void reopenFeed(const QString &sourceId);
    QVariantMap slideshowPlayback(const QString &sourceId) const;
    bool controlSlideshow(const QString &sourceId, const QString &action, double value = 0);
    QVariantMap mediaPlayback(const QString &sourceId) const;
    bool controlMedia(const QString &sourceId, const QString &action, double value = 0);
    // Browser sources: load the page again; mouse, wheel and keys from the
    // interact window (see BrowserPage::input); sources whose pictures go
    // out by browserFrameReady as well, while an interact window shows them.
    bool reloadBrowser(const QString &sourceId);
    void browserInput(const QString &sourceId, const QVariantMap &event);
    void setBrowserWatched(const QString &sourceId, bool watched);
    // {state, title, starts, pid, sound (the sink it plays into, "" muted)}; empty when not running.
    QVariantMap browserStatus(const QString &sourceId) const;
    // The page's address as the helper loads it, from a source's settings.
    static QString browserUrl(const QVariantMap &settings);
    static QString browserState(const BrowserPage &page, const QVariantMap &settings);
    // Capture devices (cameras, capture cards) plugged in now, with their modes.
    std::vector<VideoDevice> videoDevices() const { return m_videoDevices.devices(); }
    // What a recording's {name} fields stand for right now (see expandFileName).
    QMap<QString, QString> fileNameFields() const;
    // A device source's picture controls as the device has them now.
    std::vector<DeviceControl> deviceControls(const QString &sourceId) const;

    bool startRecording(QString *error);
    // Nonblocking for the UI; shutdown can wait for the completed file.
    void stopRecording(bool wait = true);
    bool recording() const { return m_recording != nullptr; }
    bool recordingStopping() const { return m_recording && m_recording->stopping; }
    // Leaves out what happens until resumed; the file plays straight through.
    bool setRecordingPaused(bool paused);
    bool recordingPaused() const { return m_recording && m_recording->file->paused(); }
    // A new file from the next keyframe.
    bool splitRecording();
    // A chapter here, where the format keeps them; false where it cannot.
    bool addChapter(const QString &name);
    // Every file the current or last recording wrote, in order.
    QStringList recordingFiles() const;

    // Replay buffer: the last stretch of the recording's encoding kept in
    // memory. `byStream` ones stop with the stream.
    bool startReplay(QString *error, bool byStream = false);
    void stopReplay(bool wait = true);
    bool replayActive() const { return m_replay && !m_replay->stopping; }
    // Writes what is held up to now into the recording folder; replaySaved or
    // replayFailed tells how it went.
    bool saveReplay(QString *error);
    struct ReplayStatus {
        int64_t heldNs = 0;
        uint64_t heldBytes = 0;
        int saving = 0;
        QString encoder;
    };
    ReplayStatus replayStatus() const;
    QString lastReplayPath() const { return m_lastReplayPath; }

    // The program as a camera other apps can open (see VirtualCamera).
    bool startVirtualCamera(QString *error);
    void stopVirtualCamera();
    bool virtualCameraRunning() const { return m_camera.running(); }
    // An app is taking its pictures now.
    bool virtualCameraInUse() const { return m_camera.inUse(); }
    uint64_t virtualCameraFrames() const { return m_camera.framesSent(); }
    // Transitions between studios. While running, a change of live studio
    // goes through that studio's transition or the collection's; one change
    // can be given another instead (a quick transition). A change during a
    // transition starts from the picture showing then.
    void setNextTransition(const QString &transition, int durationMs);
    // A transition towards `sceneId` driven by hand (a T-bar), progress 0..1;
    // the live studio changes when it reaches 1. Negative gives it up.
    // `copy` as for takeProgram.
    void setManualTransition(const QString &sceneId, double progress, bool copy = false);
    bool transitioning() const { return m_transition.has_value(); }
    // One driven by hand is under way: begun by the T-bar and not yet taken
    // to its end, given up or taken over by a timed one.
    bool manualTransition() const { return m_transition && m_transition->manual; }
    // The studio a transition is going to, and how far it has gone.
    QString transitionTarget() const { return m_transition ? m_transition->to.scene : QString(); }
    double transitionProgress() const;

    // Studio mode: the program keeps its studio while another (the
    // collection's current one) is edited and previewed; takeProgram puts a
    // studio on air through its transition. With `copy` the program shows a
    // copy of the layout as it is, so later edits stay off air. Leaving
    // studio mode makes the studio on air the current one. With copySources
    // a copy also keeps its own colour, image, text and slideshow sources,
    // as they were set, so editing those stays off air too (captures, media
    // and sound are not opened twice, and studios inside follow edits).
    void setStudioMode(bool on);
    void setCopySources(bool on) { m_copySources = on; }
    bool studioMode() const { return m_studioMode; }
    void takeProgram(const QString &sceneId, bool copy = false);
    // What the program shows, or is going to.
    QString programScene() const;

    // Pictures for other windows (multiview tiles, projectors). Each view
    // shows the program, studio mode's preview, a studio or one source, fitted
    // into its size, about `fps` times a second, by viewReady. Studios a view
    // shows have their sources running, but not heard.
    struct View {
        QString id;
        QString shows;            // program, preview, studio, source
        QString target;           // the studio or source
        int width = 0, height = 0;
        int fps = 30;
        bool operator==(const View &o) const = default;
    };
    void setViews(const std::vector<View> &views);
    // A full-size picture of the program, a studio or a source (at its own
    // size), by snapshotReady with the same id: null when there is none.
    void requestSnapshot(const QString &id, const QString &shows, const QString &target);

    // What it shows: "program", or a studio or source by id (profile.cameraShows).
    // A studio or source that is gone, or the live studio, shows the program.
    void setVirtualCameraShows(const QString &shows, const QString &target);
    // Whether it shows something of its own now rather than the program.
    bool virtualCameraShowsOwn() const;

    // Copies into MP4 without encoding again: after each recording when the
    // profile asks, or any files by hand.
    RemuxQueue &remux() { return m_remux; }
    const RemuxQueue &remux() const { return m_remux; }
    // Whether a format is MP4 or QuickTime already (no copy needed).
    static bool isMp4Family(const QString &format);
    // Why the custom container cannot be recorded into; "" when it can (or
    // the format is not custom).
    QString customMuxerProblem() const;

    // Bytes free where recordings go; -1 when it can't tell. A recording
    // will not start short of space, warns while it runs low and stops,
    // saved, before it runs out.
    qint64 freeSpace(const QString &folder) const;
    void setFreeSpaceProbe(std::function<qint64(const QString &)> probe);

    // Streaming: every enabled target in the profile, each with its key
    // (looked up by the caller, by target id). Targets that cannot start say
    // why; the others go live. True when at least one started.
    // Keys and passwords by target id, from the keyring.
    bool startStreaming(const QMap<QString, QString> &keys, QString *error, const QMap<QString, QString> &passwords = {});
    void stopStreaming(bool wait = false);
    bool streaming() const { return !m_streams.empty(); }
    // Any output open or finishing: recording, streams, the replay buffer, the
    // virtual camera. The canvas, frame rate and sound format hold meanwhile.
    bool outputsBusy() const { return m_recording || !m_streams.empty() || m_replay || m_camera.running(); }
    bool streamingStopping() const;
    struct StreamStatus {
        QString id, name;
        QString state;          // connecting, live, reconnecting, stopping, stopped, failed
        QString message;
        QString encoder;
        QString protocol;
        double kbps = 0;
        uint64_t framesSent = 0, framesDropped = 0;
        int reconnects = 0, lagMs = 0, bitrateKbps = 0;
        bool bitrateHeld = false;  // the encoder cannot lower it: only pictures are dropped
        int delayMs = 0;        // grows after a reconnect that kept the delay
        uint64_t heldBytes = 0;
    };
    std::vector<StreamStatus> streamStatus() const;
    // Encoders for this stream target, best first: what its service and
    // protocol take come first.
    std::vector<RankedEncoder> streamEncoders(const StreamTarget &target) const;
    // The target as it goes out when it keeps to its service's limits: the
    // bitrate no higher, keyframes as often as asked, the size within bounds.
    StreamTarget keptToService(const StreamTarget &target) const;

    // The canvas in use. With no size in the profile it settles once, on the
    // first capture's real pixel size — what the compositor delivers after its
    // own scaling, so fractional scales need no guessing — and canvasSettled
    // tells the owner to rescale the layout and keep the size.
    int canvasWidth() const { return m_canvasWidth.load(); }
    int canvasHeight() const { return m_canvasHeight.load(); }
    bool canvasFollowsDisplay() const { return m_canvasAuto.load(); }
    // A chosen canvas size, from the next frame. The layout is the owner's.
    void setCanvasSize(int width, int height);
    // An output's size for a chosen height (0 = the canvas): the canvas's
    // shape, both sides even.
    QSize outputSize(int height) const;
    // Label of the encoder the current or last recording opened with.
    QString recordingEncoder() const { return m_recordingEncoder; }

    // Preview readbacks only happen while something displays them.
    void setPreviewEnabled(bool on) { m_previewEnabled.store(on); }
    void setPreviewSize(int w, int h);
    void requestCapturePreview(const QString &sourceId);

    Stats stats() const;
    // What a source's video filters cost per frame (ms, GPU time where the
    // engine can time it), as of the last second; -1 when not measured.
    double filterCostMs(const QString &sourceId) const;
    // Each output running now, for the stats dock: how long a frame takes to
    // encode, how far sending is behind, and what was dropped.
    struct OutputStats {
        QString kind;              // recording, stream, replay, camera
        QString name;
        double encodeMs = -1;      // -1 where nothing is encoded
        QString encoder;           // the encoder at work, as its label names it (NVENC, VA-API, x264…)
        bool hardware = false;
        int lagMs = -1;            // -1 where nothing is sent
        uint64_t frames = 0, dropped = 0;
    };
    std::vector<OutputStats> outputStats() const;
    // The compositing device's render node; "" on the CPU.
    QString engineRenderNode() const;
    // Safe mode, set before start(): the CPU engine, no capture restored at
    // launch, video filters without their files, nothing started with a stream.
    void setSafeMode(bool on) { m_safeMode = on; }
    // "auto", "cpu" or a GPU's id (engine::GpuDevice::id). Read when the
    // session starts; set it while stopped.
    void setGpuChoice(const QString &choice) { m_gpuChoice = choice.isEmpty() ? QStringLiteral("auto") : choice; }
    QString gpuChoice() const { return m_gpuChoice; }
    // On Automatic, whether the power has changed since the picture started
    // (plugged in or unplugged) so that another GPU should draw it now: true
    // with that GPU's name. The picture moves over on the next start.
    bool automaticGpuChanged(QString *name = nullptr, bool *onBattery = nullptr) const;
    bool safeMode() const { return m_safeMode; }
    void applyAudioSettings();
    bool setAudioFormat(AudioFormat format);
    QString monitoringStatus() const;
    bool sourceAudioActive(const QString &sourceId) const;
    float sourceAudioPeak(const QString &sourceId) const;
    PeakMeter::Reading sourceAudioMeter(const QString &sourceId) const;
    PeakMeter::Reading globalAudioMeter(int channel) const { return m_mixer.sourceMeter(channel); }
    void resetAudioMeterMaximum(int globalChannel, const QString &sourceId);
    void holdAudioPush(const QString &target, bool talk, bool held, unsigned origin);
    bool audioPushHeld(const QString &target, bool talk) const;
    void cancelAudioPush(const QString &target = {}, unsigned origins = ~0u);
    QString sourceAudioError(const QString &sourceId) const;
    PeakMeter::Reading globalAudioDeviceMeter(const QString &id) const;
    QString globalAudioDeviceStatus(const QString &id) const;
    void resetGlobalAudioDeviceMeter(const QString &id);

    // Audio inputs as discovered, and which one the microphone channel uses.
    std::vector<AudioInput> audioInputs() const { return m_devices.inputs(); }
    std::vector<AudioNode> audioNodes() const { return m_devices.nodes(); }
    std::string defaultAudioOutput() const { return m_devices.defaultOutput(); }
    MicrophonePick microphone() const { return m_microphone; }
    // "" automatic, "none" off, else an input's node.name.
    void setMicrophoneChoice(const QString &choice);

signals:
    void slideshowChanged();
    void monitoringChanged();
    void previewReady(const QImage &image);
    void capturePreviewReady(const QString &sourceId, const QImage &image, int fullWidth, int fullHeight);
    void sourcesChanged();
    void recordingChanged();
    void recordingFailed(const QString &message);
    void streamingChanged();
    void streamFailed(const QString &name, const QString &message);
    void replayChanged();
    void replaySaved(const QString &path);
    void replayFailed(const QString &message);
    void virtualCameraChanged();
    void remuxChanged();
    void diskSpaceLow(const QString &message);
    // The recording goes on, but something did not (a split).
    void recordingWarning(const QString &message);
    void remuxFinished(const QString &in, const QString &out, const QString &error, const QString &note);
    void statsChanged();
    void encodersChanged();
    void canvasChanged();
    void microphonesChanged();
    void restoreTokenChanged(const QString &sourceId, const QString &token);
    void canvasSettled(int fromWidth, int fromHeight, int toWidth, int toHeight);
    void videoDevicesChanged();
    // For the status line: gone, or back.
    void pipeWireChanged(const QString &message);
    // For the status line: the graphics card gone, and what that took with it.
    void drawingChanged(const QString &message);
    void transitionChanged();
    void viewReady(const QString &id, const QImage &image);
    void browserFrameReady(const QString &sourceId, const QImage &image);
    void snapshotReady(const QString &id, const QImage &image);
    void studioModeChanged();
    // Studio mode: the program while previewReady shows the studio being edited.
    void programReady(const QImage &image);

private:
    struct Feed {
        QString sourceId;
        QString kind;
        QVariantMap settings;          // what a drawn feed was last drawn from
        ScreenCastPortal *portal = nullptr;
        VideoStream stream;
        std::atomic<std::shared_ptr<DirectCapture::Stream>> direct;
        QVariantMap captureSettings; // main thread: only settings that reopen capture
        bool portalFallbackAllowed = true;
        QRect layoutArea; // main thread: where the portal says the captured display sits, in layout points, if it said
        std::shared_ptr<DirectCapture::Stream> configuredDirect; // render thread
        uint64_t directGeneration = 0; // render thread: last uploaded target
        int captureWidth() const { auto s=direct.load(); return s ? s->width() : stream.width(); }
        int captureHeight() const { auto s=direct.load(); return s ? s->height() : stream.height(); }
        uint64_t captureFrames() const { auto s=direct.load(); return s ? (s->hasPicture() ? s->received() : 0) : stream.framesReceived(); }
        bool dmabufFailed = false;
        QString state;
        std::atomic<bool> live{false};
        bool active = false;           // main thread: visible anywhere in the live plan
        std::atomic<bool> releasePicture{false}; // render thread releases GPU/held pixels
        int engineId = -1;             // frame thread only
        // Drawn kinds (colour, image, text): the picture the frame thread
        // uploads when dirty, straight-alpha BGRA (ARGB32), as it was drawn:
        // its pixels are never written again, only the picture replaced. A
        // browser's is its premultiplied twin, written into when no one else
        // holds it. Null while the frame thread has it out for an upload.
        mutable std::mutex pixelsMutex;
        QImage image;
        int width = 0, height = 0;
        std::atomic<bool> dirty{false};
        std::atomic<bool> uploaded{false};
        std::atomic<bool> linearAlpha{true}; // image/media source processing, independent of item blend space
        bool linearAlphaApplied = true; // render thread only
        // Capture devices (kind "device").
        DeviceStream device;
        QString deviceNode;            // node.name it streams from
        QString deviceMode;            // VideoMode::key it streams in
        DeviceStream::Picture picture; // frame thread only
        QVariantMap appliedControls;   // what the device was last told
        mutable std::mutex colourMutex; // guards colour overrides and region
        std::string colourRange, colourMatrix;
        CaptureRegion region; // also guarded by colourMutex
        DeinterlaceSettings deinterlace; // also guarded by colourMutex
        VideoFieldClock fieldClock; // frame thread only
        DeinterlaceSettings deinterlaceApplied; // frame thread only
        QRect captureBounds(int w, int h) const {
            std::lock_guard<std::mutex> lock(colourMutex);
            return region.bounds(w, h);
        }
        // Video filters, parsed on the main thread from `videoFilterSettings`;
        // the chain and its serial are guarded by colourMutex.
        QVariantList videoFilterSettings;
        QStringList videoFilterProblems;
        std::vector<engine::VideoFilter> videoFilters;
        uint64_t videoFiltersSerial = 1, videoFiltersApplied = 0; // applied: frame thread
        bool hiddenWithoutGpu = false;  // frame thread: Remove background with no GPU to run it
        std::atomic<engine::FilterFailure> cutOutFailure{engine::FilterFailure::None};  // set by the frame thread
        quint64 filtersAsked = 0; // main thread: the newest list, as its files come off the main thread
        void filteredSize(int *w, int *h) const {
            std::lock_guard<std::mutex> lock(colourMutex);
            engine::filteredSize(videoFilters, *w, *h, w, h);
        }
        // The region in the picture the engine holds, which has been through
        // the filters: *w and *h go in as the capture's size, out as that picture's.
        CaptureRegion regionAfterFilters(int *w, int *h) const {
            std::lock_guard<std::mutex> lock(colourMutex);
            const QRect area = region.bounds(*w, *h);
            if (area.isEmpty()) { engine::filteredSize(videoFilters, *w, *h, w, h); return {}; }
            double x = area.x(), y = area.y(), rw = area.width(), rh = area.height();
            engine::filteredRect(videoFilters, *w, *h, w, h, &x, &y, &rw, &rh);
            return {int(std::lround(x)), int(std::lround(y)), std::max(1, int(std::lround(rw))), std::max(1, int(std::lround(rh)))};
        }
        std::atomic<float> denoise{0.f};   // wanted noise reduction, 0..1
        float denoiseApplied = -1.f;       // frame thread: what the engine has
        int pictureWidth = 0, pictureHeight = 0;
        bool capture() const { return kind == QLatin1String("display") || kind == QLatin1String("window") || kind == QLatin1String("region"); }
        bool isAudio() const { return SourceDef::isAudioKind(kind); }
        bool isDevice() const { return kind == QLatin1String("device"); }
        // Media files and network streams (kind "media").
        int audioChannel = -1;          // stable mixer id, never reused
        struct AudioTap { std::unique_ptr<AudioCapture> capture; int input = -1; };
        std::map<std::string, AudioTap> audioCaptures;
        QString audioNode;
        QString audioError;
        MediaStream media;
        std::mutex mediaControlMutex;
        std::atomic<int64_t> mediaPausedAt{0};
        QVariantMap mediaSettings;     // what the stream was started with
        QString mediaShown;            // how it was doing when the list was last told
        bool isMedia() const { return kind == QLatin1String("media"); }
        // Another studio, or a group: drawn by the engine each frame.
        std::atomic<int> nestedWidth{0}, nestedHeight{0};
        bool isNested() const { return kind == QLatin1String("studio") || kind == QLatin1String("group"); }
        // Browser sources (main thread): the page, drawn by the helper; its
        // pictures arrive premultiplied.
        std::unique_ptr<BrowserPage> browser;
        bool isBrowser() const { return kind == QLatin1String("browser"); }
        bool pixelsPremultiplied = false; // pixelsMutex
        // Animated images and slideshows (main thread); text from a file.
        SlideshowAnimator::Presentation slideshow; // pixelsMutex
        quint64 slideCounter = 0; // main thread, distinguishes replacement animators
        quint64 uploadedSlide = 0; // render thread
        int slideFrom = -1, slideTo = -1, slideMix = -1; // render thread engine sources
        SlideshowAnimator::Presentation slideShown; // render thread: the last one drawn
        Animator *animator = nullptr;
        QString fileText;              // the file's text when last drawn
        FileStamp fileStamp;           // and the file as it was then: read again only when it differs
        // Main thread: the newest draw asked for, older answers being dropped;
        // what is on its way off the thread; asked again meanwhile.
        quint64 drawSerial = 0;
        bool drawing = false, drawAgain = false;
        QString drawingFile;
        QVariantMap drawingSettings;
    };
    // An item being shown or hidden through a transition: made on the main
    // thread, drawn into target by the render thread.
    struct ItemTransition {
        engine::TransitionStyle style;
        int64_t startNs = 0, durationNs = 0;
        bool showing = true;
        int target = -1;                       // render thread
        uint64_t drawnTick = 0;                // render thread
    };
    struct PlanItem {
        std::shared_ptr<Feed> feed;
        Transform transform;
        std::shared_ptr<ItemTransition> transition{};
    };
    // A studio or group drawn into its feed's picture before the frame.
    struct NestedRender {
        std::shared_ptr<Feed> feed;
        int width = 0, height = 0;
        std::vector<PlanItem> items;
    };
    // Plans `scene`'s visible items, first planning (deepest first) every
    // studio and group they show. Caller holds m_sourcesMutex.
    struct Shown;
    // shown: the program's choice the scene came from, whose own copies of
    // sources (if any) stand in for the collection's.
    void planScene(const Scene &scene, std::vector<PlanItem> &items, std::vector<NestedRender> &nested,
                   QStringList &planned, int depth, const Shown *shown = nullptr);
    std::vector<engine::Layer> layersFor(const std::vector<PlanItem> &items);
    int itemTransitionTarget(const std::shared_ptr<ItemTransition> &t, int source, int width, int height);
    // Main thread: starts a transition for each item shown or hidden since last time.
    void followItemVisibility();

    void frameLoop();
    void audioLoop();
    void tick(int64_t nowNs);
    static void overrideColour(Feed &feed);
    void pumpAudio();
    void syncFeeds();
    void rebuildPlan();
    // Captures ask the portal one at a time: each may show the desktop's
    // picker, and several at once stack pickers on top of each other.
    void startCapture(const std::shared_ptr<Feed> &feed, bool portalFallback = false);
    // Whether a capture nobody just asked for can start without showing the picker.
    bool opensQuietly(const QString &sourceId);
    void pumpCaptures();
    void captureAnswered(const QString &sourceId);
    // What opens a file is read off this thread and drawn when it comes.
    // `onlyIfChanged`: a text file checked again, drawn only if it differs.
    void drawFeed(const std::shared_ptr<Feed> &feed, const SourceDef &def, bool onlyIfChanged = false);
    struct DrawnFile;
    // On its own thread: nothing here may touch the session.
    static DrawnFile readDrawn(const SourceDef &def, const QString &fallbackText, const QString &was, const FileStamp &wasStamp,
                               bool onlyIfChanged, const FileWork::Token &token);
    void drawAnswered(const std::shared_ptr<Feed> &feed, quint64 serial, const DrawnFile &answer, const SourceDef &def);
    // Starts (or keeps) the animator a moving drawn source needs; `movie` is an animated picture's file.
    void animate(const std::shared_ptr<Feed> &feed, const SourceDef &def, const QByteArray &movie = {});
    // Hands a drawn picture to the frame thread.
    static void setPixels(Feed &feed, QImage image);
    // Starts, restarts or stops a capture device's stream to match its
    // settings and what is plugged in.
    void syncDevice(Feed &feed, const SourceDef &def);
    void refreshDevices();
    bool connectPipeWire();
    void connectDesktopAudio();
    void watchPipeWire();
    void syncMedia(Feed &feed, const SourceDef &def, bool activated = false);
    void syncBrowser(Feed &feed, const SourceDef &def, bool activated);
    // Starts capturing `target` (a node's name or serial) into the feed's channel, as audioCaptures[key].
    void tapAudio(Feed &feed, const std::string &key, const std::string &target, bool sinkMonitor);
    std::set<QString> m_browserWatched;
    void syncFeedAudio(Feed &feed, const SourceDef &def);
    void syncGlobalAudioDevices();
    // Caller holds m_sourcesMutex; typed targets keep profile and collection ids separate.
    Feed *audioFeed(const QString &target) const;
    static void syncColour(Feed &feed, const SourceDef &def);
    void syncVideoFilters(const std::shared_ptr<Feed> &feed, const SourceDef &def);
    QString devicePath(const QString &node) const;
    void onFeedStarted(Feed *feed, uint nodeId, int fd, const QString &token);
    void setFeedState(Feed *feed, const QString &state);
    void refreshMicrophone();
    void refreshMonitoring();
    // Runs this binary's encoder helper verb; `done` gets its stdout (empty
    // when it could not start or was stopped).
    // One that has not answered in `timeoutMs` is stopped (a hung driver).
    // `line`, when given, hears each line it writes as it comes.
    QProcess *runHelper(const QStringList &arguments, const QByteArray &input,
                        std::function<void(const QByteArray &)> done, int timeoutMs = 120000,
                        std::function<void(const QByteArray &)> line = {});
    // Which encoders work: in the helper, once more if it fails, and only
    // then in this process, on a thread of its own.
    void probe(int attempt);
    void encodersFound(const std::vector<EncoderInfo> &list);
    std::thread m_probeThread;
    // The encoder fit times every hardware encoder for a moment, which a live
    // recording or stream would share the GPU with: one starting stops it
    // (the last answer stands), and it runs again once none is live.
    void pauseFit();
    void resumeFit();

    Profile m_profile;
    std::atomic<bool> m_canvasAuto{false};      // until the first capture settles it
    std::atomic<int> m_canvasWidth{0}, m_canvasHeight{0};
    std::atomic<bool> m_canvasDirty{false};
    std::unique_ptr<engine::Engine> m_engine;   // owned by the frame thread once running
    // What the engine imports, by DRM format, published by the frame thread as
    // it makes an engine; none on the CPU. Captures ask once one is made.
    std::mutex m_engineFormatsMutex;
    bool m_engineMade = false;
    std::map<uint32_t, std::vector<uint64_t>> m_engineFormats;
    void publishEngineFormats();
    // An automatic canvas settling against an encoder opening at the old size.
    std::mutex m_settleMutex;
    int m_outputsOpening = 0;                   // main thread writes, with m_settleMutex
    // Until the output's encoder is in Encodings, where the frame thread sees it.
    auto opening() {
        { std::lock_guard<std::mutex> lock(m_settleMutex); ++m_outputsOpening; }
        return qScopeGuard([this] { std::lock_guard<std::mutex> lock(m_settleMutex); --m_outputsOpening; });
    }
    std::thread m_thread, m_audioThread;
    using AudioFeeds = std::vector<std::shared_ptr<Feed>>;
    std::atomic<std::shared_ptr<const AudioFeeds>> m_audioFeeds{std::make_shared<const AudioFeeds>()};
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_runSerial{0};
    std::atomic<bool> m_quit{false};
    std::atomic<bool> m_audioQuit{false};
    std::atomic<bool> m_previewEnabled{false};
    std::atomic<int> m_previewWidth{640}, m_previewHeight{360};

    Production *m_production = nullptr;
    // Guards the lists of feeds and the retired ones against the frame thread,
    // the only other thread that reads them. Only the main thread changes them,
    // so it reads them without this, and holds it just for the moment a list
    // changes or a getter looks up. Never across a file, a decoder or a device:
    // the frame thread takes this every tick, and would wait with them.
    mutable std::mutex m_sourcesMutex;
    std::map<QString, std::shared_ptr<Feed>> m_feeds, m_globalAudioFeeds;
    std::map<QString, double> m_filterCosts;  // with m_statsMutex: source id → ms
    // Feeds gone, whose engine ids the frame thread lets go at its next tick.
    std::vector<std::shared_ptr<Feed>> m_retiredFeeds;
    QString m_capturePreviewRequest;            // guarded by m_sourcesMutex
    std::deque<QString> m_captureQueue;        // main thread
    QString m_captureInFlight;
    QString m_tokenScope;
    bool m_restartFeeds = false;
    QTimer m_textFiles;                        // re-reads text sources' files
    FileWork m_fileWork{this};
    DirectCapture m_directCaptures;
    QTimer m_directStatus;
    QTimer m_pipeWireWatch;
    bool m_pipeWireLost = false;               // said so; connecting again
    int m_pipeWireTries = 0, m_pipeWireWait = 0;  // seconds until the next try
    bool m_launching = false;                  // starting, or opening a collection: no surprise pickers
    bool m_safeMode = false;
    QString m_gpuChoice = QStringLiteral("auto");
    std::vector<View> m_views;                 // main thread
    std::vector<View> m_snapshotViews;         // main thread: drawn once, then dropped
    struct PlannedView {
        View view;
        std::vector<PlanItem> items;           // a studio's
        std::shared_ptr<Feed> feed;            // a source's
        bool snapshot = false;
    };
    std::map<QString, int> m_viewTargets;      // render thread: engine pictures
    // Render thread: when each view was last sent, and whether that picture
    // is still on its way to the main thread (which says when it has come).
    struct ViewSent {
        int64_t ns = 0;
        std::shared_ptr<std::atomic<bool>> onItsWay = std::make_shared<std::atomic<bool>>(false);
    };
    std::map<QString, ViewSent> m_viewDrawn;
    void drawViews(engine::Engine &eng, int64_t nowNs, const std::vector<PlannedView> &views,
                   const std::vector<PlanItem> &editPlan, bool editShown);
    std::atomic<bool> m_studioRender{false};   // previews go out as preview and program
    int m_editTarget = -1;                     // render thread
    struct PlannedTransition {
        bool on = false;
        uint64_t serial = 0;                   // a new one each start
        bool fromShowing = false;              // starts from the picture showing (an interruption)
        bool manual = false;                   // driven by hand: m_manualProgress says how far
        engine::TransitionStyle style;
        int64_t startNs = 0, durationNs = 0;
        std::shared_ptr<Feed> stinger;         // its video over the program
        float cutAt = 0.f;                     // a stinger's cut, as progress
        int matte = 0;                         // its track matte: none, beside, below, the whole picture, another file
        std::shared_ptr<Feed> stingerMatte;    // that other file
        bool matteInvert = false;
    };
    // Everything a frame is drawn from, as one thing that is never changed
    // once it is out: the plans and the transition between them cannot reach
    // the render thread apart. rebuildPlan makes a new one each time and
    // publish() swaps it in; a frame takes the pointer and draws from that.
    struct Published {
        std::vector<PlanItem> plan;
        std::vector<NestedRender> nested;      // drawn in order, before plan
        std::vector<PlanItem> outgoing;        // the studio a transition leaves
        std::vector<PlanItem> edit;            // studio mode: the studio being edited, drawn for the preview
        bool editShown = false;
        std::vector<PlannedView> views;
        std::vector<PlanItem> camera;          // the virtual camera's studio or source
        bool cameraOwn = false;                // it shows `camera`, not the program
        PlannedTransition transition;
    };
    // The main thread, the only one to change it, reads it as it is; the
    // render thread takes it under m_planMutex. Never null.
    mutable std::mutex m_planMutex;
    std::shared_ptr<const Published> m_published = std::make_shared<const Published>();
    void publish(std::shared_ptr<const Published> next);
    // A transition just begun, or one by hand just ended (main thread).
    // rebuildPlan publishes it with the plans that go with it, so no frame
    // mixes a new one with the old plans (an empty outgoing studio, drawn
    // black) or draws alone the plan of one by hand given up (the studio it
    // was going to, whole).
    std::optional<PlannedTransition> m_stagedTransition;
    uint64_t m_transitionSerials = 0;          // main thread: the last serial given
    // A hand-driven transition's progress. The render thread reads it with
    // what is published, under m_planMutex; it goes back to -1 there too, as
    // plans without one by hand are published.
    std::atomic<float> m_manualProgress{-1.f};
    int m_programFrom = -1, m_programTo = -1, m_programOut = -1, m_programMatte = -1; // render thread
    uint64_t m_drawnTransition = 0;            // render thread: serial being drawn
    bool m_fromStill = false, m_outDrawn = false; // render thread
    int m_cameraTarget = -1;                   // render thread: its picture in the engine

    // Main thread: the studio the program settled on, and a transition under way.
    // What the program shows: a studio, or a copy of one taken in studio mode.
    struct Shown {
        QString scene;
        std::shared_ptr<const Scene> copy;
        quint64 serial = 0;                    // tells copies apart
        std::shared_ptr<const std::map<QString, SourceDef>> sources; // a copy's own sources, by the collection's id
        const Scene *in(const Collection &c) const;
        bool same(const Shown &o) const { return scene == o.scene && serial == o.serial; }
    };
    Shown shownFor(const QString &sceneId, bool copy);
    struct ProgramTransition {
        Shown from, to;                        // from is empty when it starts from the picture showing
        bool manual = false;
        int64_t startNs = 0, durationNs = 0;
        std::shared_ptr<Feed> stinger{}, stingerMatte{};
        int64_t cutNs = 0;                     // a stinger's cut
        bool outIn = false;                    // sound out to the cut, then in
    };
    // Stinger transitions' videos, open and paused at their start so one
    // begins at once; by transition id (a separate matte's by matteKey(id)),
    // guarded by m_sourcesMutex.
    std::map<QString, std::shared_ptr<Feed>> m_stingers;
    void syncStingers();
    static QString matteKey(const QString &transition) { return transition + QStringLiteral("#matte"); }
    // How long a stinger lasts: 0 while its file is not open yet (it cuts).
    int stingerMs(const QString &transition) const;
    void parkStinger(const std::shared_ptr<Feed> &feed);
    Shown m_onAir;
    bool m_studioMode = false;
    Shown m_studioProgram;                     // studio mode: what was taken
    quint64 m_copySerial = 0;
    bool m_copySources = false;
    // Feeds for copies' own sources, by copyKey; guarded by m_sourcesMutex.
    struct CopyFeed { std::shared_ptr<Feed> feed; SourceDef def; };
    std::map<QString, CopyFeed> m_copyFeeds;
    static QString copyKey(quint64 serial, const QString &source) { return QStringLiteral("copy%1/").arg(serial) + source; }
    static bool copiesSource(const QString &kind);
    void syncCopyFeeds(const std::vector<const Shown *> &shown);
    quint64 m_programGeneration = 0;
    std::optional<ProgramTransition> m_transition;
    std::optional<std::pair<QString, int>> m_nextTransition;
    QTimer m_transitionEnd;
    std::map<QString, std::shared_ptr<ItemTransition>> m_itemTransitions; // by item id
    std::map<QString, bool> m_itemShown;       // each item's visibility when last planned
    quint64 m_itemGeneration = 0;
    QTimer m_itemTransitionEnd;                // replans as the next one ends
    std::vector<std::shared_ptr<ItemTransition>> m_itemTransitionsDrawn; // render thread: those with a target
    int m_clearSource = -1;                    // render thread: one transparent pixel
    uint64_t m_tickSerial = 0;                 // render thread
    int64_t m_tickNs = 0;                      // render thread
    // `manualAt`: where one by hand starts, so nothing told of it sees it at 0 first.
    void beginTransition(const Shown &from, const Shown &to, const QString &transition, int durationMs, bool manual,
                         double manualAt = 0);
    void endTransition();
    void dropTransition();                     // without drawing it out
    // Tells the render thread a transition is over: at once (the same plans,
    // published again without it), or with the next plans.
    void unplanTransition();
    // Puts `wanted` on air through its transition, or the one set for this change.
    void changeProgram(const Collection &c, const Shown &wanted);
    // Sound follows the picture: sources only the outgoing studio shows fade
    // out, sources only the incoming one shows fade in (main thread).
    quint64 m_airSerial = 0;
    std::set<QString> m_airLive, m_airOut, m_airIn;
    void setManualAir(float progress);

    bool m_lowLatencyAudio = false; // main thread, applied profile choice
    AudioPlayback m_monitor;
    QString m_monitorReason, m_monitorLabel;
    std::string m_monitorSerial;
    AudioCapture m_audio;
    AudioDevices m_devices;
    VideoDevices m_videoDevices;
    MicrophonePick m_microphone;
    AudioMixer m_mixer;
    Encodings m_encodings;
    // The recording: its file and the encoder it takes from. Main thread.
    // The frame rate of this run, from the profile at start(): the profile's
    // may change meanwhile (the frame thread reads these unlocked).
    int m_runFpsNum = 30, m_runFpsDen = 1;
    // For the main thread: the running rate, or the profile's before a start.
    int runFpsNum() const { return m_running.load() ? m_runFpsNum : std::max(1, m_profile.fpsNum); }
    int runFpsDen() const { return m_running.load() ? m_runFpsDen : std::max(1, m_profile.fpsDen); }
    // Outputs are known by a serial, never by address: a late message from
    // one that is gone must not match the next, made where it was.
    quint64 m_outputSerial = 0;
    struct Recording {
        quint64 serial = 0;
        std::shared_ptr<EncodedStream> stream;
        std::shared_ptr<FileSink> file;
        bool stopping = false, failed = false;
        uint64_t lateAtStart = 0, lateAtStop = 0, droppedAtStart = 0;  // this recording's share of the running counts
    };
    std::unique_ptr<Recording> m_recording;
    Recorder::Stats m_lastRecordingStats;
    QStringList m_recordingFiles;
    EncodedStream::Settings recordingEncoding(const EncoderInfo &encoder) const;
    // The chosen recording encoder, or the best that opens.
    std::shared_ptr<EncodedStream> acquireRecordingEncoder(bool *shared, QString *error);
    // Why a chosen encoder cannot record into this format, and what can.
    QString containerRefusal(const EncoderInfo &encoder, const std::string &container);
    void finishRecording(quint64 serial);
    uint64_t lateTicks() const;
    struct Replay {
        quint64 serial = 0;
        std::shared_ptr<EncodedStream> stream;
        std::shared_ptr<ReplaySink> sink;
        bool stopping = false;
        bool startedByStream = false;
    };
    std::unique_ptr<Replay> m_replay;
    QString m_lastReplayPath;
    void finishReplay(quint64 serial);
    VirtualCamera m_camera;
    RemuxQueue m_remux;
    std::atomic<bool> m_remuxNotifyPending{false};
    QTimer m_diskGuard;
    bool m_diskWarned = false;
    std::function<qint64(const QString &)> m_freeSpaceProbe;
    void checkDiskSpace();
    void wireRemux();
    // A stream going out: its target, its encoder and what sends it. Main thread.
    struct StreamOutput {
        quint64 serial = 0;
        StreamTarget target;
        std::shared_ptr<EncodedStream> stream;
        std::shared_ptr<StreamSink> sink;
        bool stopping = false;
    };
    std::vector<std::unique_ptr<StreamOutput>> m_streams;
    EncodedStream::Settings streamEncoding(const StreamTarget &target, const EncoderInfo &encoder,
                                           StreamSink::Protocol protocol) const;
    bool startStream(const StreamTarget &given, const QString &key, const QString &password, QString *error);
    void finishStream(quint64 serial);
    // Stops one stream that gave up, saying why (once).
    void failStream(StreamOutput &o, const QString &message);
    std::vector<EncoderInfo> m_encoders;
    mutable std::mutex m_encoderMutex;        // guards the fit results and the device below
    std::set<std::string> m_unfit, m_slow;
    QStringList m_probeSkip;            // encoders that took the check down, by key
    bool m_helperNeverStarted = false;  // the last helper could not even be run
    static constexpr int kMaxProbeSkips = 4;
    std::map<std::string, double> m_encoderFps;
    EngineDevice m_engineDevice;
    std::vector<engine::GpuDevice> m_gpusAtStart;  // as the picture started; with m_encoderMutex
    int m_gpuPickIndex = -1;
    std::string m_lastEngineGpu;  // the GPU the picture was last drawn on (frame thread)
    std::atomic<bool> m_fitting{false};
    bool m_refitAgain = false;
    bool m_refitPending = false;     // wanted while an output was live
    QPointer<QProcess> m_fitProcess;
    std::atomic<bool> m_probing{false};
    QString m_probeNow;                     // GUI thread only, as m_probeFound
    std::vector<EncoderInfo> m_probeFound;
    int64_t m_lastMixedEndNs = 0;
    // Serializes mixing with changes to the mixer's latency.
    std::mutex m_recordMutex;
    QString m_recordingPath;
    QString m_recordingEncoder;

    mutable std::mutex m_statsMutex;
    Stats m_stats;
    std::vector<engine::YuvFrame> m_framePool;
    std::mutex m_poolMutex;
    std::atomic<float> m_sdrWhite{203.f}, m_hdrPeak{1000.f};

    // The last tick's work, still on the GPU (render thread): its readbacks,
    // taken at the start of the next tick and stamped with this one's time,
    // and the captured frames it reads, given back only then.
    struct PendingOutput {
        int request = 0;
        Encodings::Layout layout;
        bool forCamera = false, forEncoders = false;
        int64_t timeNs = 0, durationNs = 0;
    };
    struct PendingPreview {
        int request = 0;
        int width = 0, height = 0;
        bool studio = false, editShown = false;
    };
    struct HeldFrame {
        std::shared_ptr<Feed> feed;
        VideoStream::Frame frame;
    };
    std::vector<PendingOutput> m_pendingOutputs;
    PendingPreview m_pendingPreview;
    std::vector<HeldFrame> m_heldFrames;
    std::vector<std::shared_ptr<const DirectCapture::Frame>> m_heldDirect;
    // Pictures sent to the main thread and not there yet: one at a time for
    // the preview, the program and each view (ViewSent), or while it is busy
    // they would pile up at a hundred megabytes a second. The render thread
    // claims one as it sends; the main thread lets it go when the picture comes.
    std::atomic<bool> m_previewOnItsWay{false}, m_programOnItsWay{false};
    static bool claimPicture(std::atomic<bool> &onItsWay) { return !onItsWay.exchange(true); }
    // Takes the last tick's readbacks and gives back what it held.
    void finishLastTick(engine::Engine &eng);
    // The same when the engine goes: nothing it was asked for will come.
    void dropLastTick();
};
