#pragma once

#include <QImage>
#include <QDateTime>
#include <deque>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <functional>

#include "core/presets.h"
#include "core/production.h"
#include "core/session.h"
#include "core/app_log.h"
#include "core/system_stats.h"
#include "control/hotkeys.h"
#include "control/control_socket.h"
#include "control/omarchy_bar.h"
#include "control/focus_watcher.h"
#include "control/scripts.h"
#include "control/hyprland_rules.h"
#include "control/region_drawer.h"
#include "control/remote_control.h"
#include "control/secrets.h"
#include "filepicker.h"
#include "core/recording_player.h"

// The one object QML talks to. Wraps the session for the interface:
// properties for everything displayed, invokables for everything the user
// can do, signals for what the view must react to.
class Backend : public QObject {
    Q_OBJECT
    // Scenes: [{id, name}], in order; the live one.
    Q_PROPERTY(QVariantList scenes READ scenes NOTIFY scenesChanged)
    Q_PROPERTY(QString currentScene READ currentScene WRITE setCurrentScene NOTIFY scenesChanged)
    // The live scene's items, top first as a list shows them: {id, sourceId,
    // name, kind, visible, locked, live, state, detail, capture}.
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    // Each visible item's outline on the canvas, bottom first, in canvas
    // pixels: {id, x, y, width, height, rotation, locked}.
    Q_PROPERTY(QVariantList outlines READ outlines NOTIFY itemsChanged)
    Q_PROPERTY(QString selectedItem READ selectedItem WRITE setSelectedItem NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedMedia READ selectedMedia NOTIFY mediaPlaybackChanged)
    Q_PROPERTY(QVariantMap selectedSlideshow READ selectedSlideshow NOTIFY slideshowPlaybackChanged)
    // Every selected item, for groups and multi-select (Ctrl/Shift-click).
    Q_PROPERTY(QStringList selectedItems READ selectedItems WRITE setSelectedItems NOTIFY selectionChanged)
    Q_PROPERTY(bool canPastePlacement READ canPastePlacement NOTIFY placementCopied)
    Q_PROPERTY(bool canPasteSourceReference READ canPasteSourceReference NOTIFY sourceClipboardChanged)
    Q_PROPERTY(bool canPasteSourceDuplicate READ canPasteSourceDuplicate NOTIFY sourceClipboardChanged)
    // Scene collections: [{file, name, current}], by name.
    Q_PROPERTY(QVariantList collections READ collections NOTIFY collectionsChanged)
    Q_PROPERTY(QString collectionName READ collectionName NOTIFY collectionsChanged)
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY profilesChanged)
    Q_PROPERTY(QString profileId READ profileId NOTIFY profilesChanged)
    Q_PROPERTY(QString profileName READ profileName NOTIFY profilesChanged)
    // Sources whose file or folder is not where the collection says:
    // [{sourceId, name, kind, key, path, folder}].
    Q_PROPERTY(QVariantList missingFiles READ missingFiles NOTIFY itemsChanged)
    // The selected item's transform and its source: {x, y, width, height,
    // rotation, cropLeft, …, flipH, flipV, bounds, sourceWidth, sourceHeight,
    // kind, sourceId, name, settings}; empty with nothing selected.
    Q_PROPERTY(QVariantMap selected READ selected NOTIFY selectionChanged)
    // Capture devices plugged in now: [{name, description}].
    Q_PROPERTY(QVariantList videoDevices READ videoDevices NOTIFY videoDevicesChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY historyChanged)
    Q_PROPERTY(QString redoText READ redoText NOTIFY historyChanged)
    Q_PROPERTY(bool recordingStopping READ recordingStopping NOTIFY recordingChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(QString recordingTime READ recordingTime NOTIFY statsChanged)
    Q_PROPERTY(QString recordingFile READ recordingFile NOTIFY recordingChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString statsLine READ statsLine NOTIFY statsChanged)
    Q_PROPERTY(QString engineName READ engineName NOTIFY statsChanged)
    Q_PROPERTY(QString engineWhy READ engineWhy NOTIFY statsChanged)
    // The integrated GPU draws the picture because the machine is on battery.
    Q_PROPERTY(bool engineOnBattery READ engineOnBattery NOTIFY statsChanged)
    // Capture devices sending another frame rate than the canvas's; empty when none.
    Q_PROPERTY(QString frameRateNote READ frameRateNote NOTIFY frameRateNoteChanged)
    Q_PROPERTY(int canvasWidth READ canvasWidth NOTIFY settingsChanged)
    // The width a recording is encoded at (its chosen size, else the canvas);
    // the preview's "output pixels" zoom uses it.
    Q_PROPERTY(int outputWidth READ outputWidth NOTIFY settingsChanged)
    Q_PROPERTY(int canvasHeight READ canvasHeight NOTIFY settingsChanged)
    Q_PROPERTY(bool canvasFollowsDisplay READ canvasFollowsDisplay NOTIFY settingsChanged)
    Q_PROPERTY(int fps READ fps WRITE setFps NOTIFY settingsChanged)
    // The frame rate as a fraction (30000/1001 for 29.97); setFrameRate sets both.
    Q_PROPERTY(int fpsNum READ fpsNum NOTIFY settingsChanged)
    Q_PROPERTY(int fpsDen READ fpsDen NOTIFY settingsChanged)
    // The colour every recording and stream carries (Settings → Video): the
    // format ("auto", "nv12", "i420", "i444", "p010", "i010", "p216", "p416",
    // "bgra"), the space ("srgb", "601", "709", "pq", "hlg"), full range or
    // limited, and the HDR levels in nits. A setter that cannot be met (HDR
    // in an 8-bit format, a format the encoder does not take) refuses and
    // says why in the status line.
    Q_PROPERTY(QString colourFormat READ colourFormat WRITE setColourFormat NOTIFY settingsChanged)
    Q_PROPERTY(QString colourSpace READ colourSpace WRITE setColourSpace NOTIFY settingsChanged)
    Q_PROPERTY(bool colourFullRange READ colourFullRange WRITE setColourFullRange NOTIFY settingsChanged)
    Q_PROPERTY(int sdrWhite READ sdrWhite WRITE setSdrWhite NOTIFY settingsChanged)
    Q_PROPERTY(int hdrPeak READ hdrPeak WRITE setHdrPeak NOTIFY settingsChanged)
    Q_PROPERTY(QString downscaleFilter READ downscaleFilter WRITE setDownscaleFilter NOTIFY settingsChanged)
    // [{value, label}]: "auto" and the formats the recording encoder (or, at
    // best available, any encoder here) takes, plus the current one.
    Q_PROPERTY(QVariantList colourFormats READ colourFormats NOTIFY settingsChanged)
    // What recordings will carry, or why the colour cannot be recorded.
    Q_PROPERTY(QString colourNote READ colourNote NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList encoders READ encoders NOTIFY settingsChanged)
    Q_PROPERTY(QString encoder READ encoder WRITE setEncoder NOTIFY settingsChanged)
    // What recording will use and why, as the settings sheet shows it.
    Q_PROPERTY(QString encoderNote READ encoderNote NOTIFY settingsChanged)
    // [{value, label}] for splitting pictures across the recording encoder's
    // engines; empty when it cannot.
    Q_PROPERTY(QVariantList splitEncodeModes READ splitEncodeModes NOTIFY settingsChanged)
    // The recording encoder (chosen, or the best) makes an editing codec with
    // no rate control: the quality and bitrate rows have nothing to say.
    Q_PROPERTY(bool encoderIsForEditing READ encoderIsForEditing NOTIFY settingsChanged)
    Q_PROPERTY(int quality READ quality WRITE setQuality NOTIFY settingsChanged)
    Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY settingsChanged)
    Q_PROPERTY(QString fileNamePattern READ fileNamePattern WRITE setFileNamePattern NOTIFY settingsChanged)
    Q_PROPERTY(bool fileNameNoSpaces READ fileNameNoSpaces WRITE setFileNameNoSpaces NOTIFY settingsChanged)
    Q_PROPERTY(int recordingTracks READ recordingTracks WRITE setRecordingTracks NOTIFY settingsChanged)
    Q_PROPERTY(QString container READ container WRITE setContainer NOTIFY settingsChanged)
    // [{id, label, chapters, survivesCrash}] for the format picker.
    Q_PROPERTY(QVariantList fileFormats READ fileFormats CONSTANT)
    // With the "custom" format: FFmpeg's name for the container.
    Q_PROPERTY(QString customMuxer READ customMuxer WRITE setCustomMuxer NOTIFY settingsChanged)
    // "key=value …", the container's own options, for any format.
    Q_PROPERTY(QString containerOptions READ containerOptions WRITE setContainerOptions NOTIFY settingsChanged)
    // The rest of how a recording is encoded and split, {key: value}: rateControl,
    // bitrateKbps, maxKbps, bufferKbps, keyframeSeconds, bFrames, lookahead,
    // preset, profile, tune, options, audioCodec, splitMinutes, splitMegabytes.
    Q_PROPERTY(QVariantMap recordingOptions READ recordingOptions NOTIFY settingsChanged)
    Q_PROPERTY(bool recordingPaused READ recordingPaused NOTIFY recordingChanged)
    // Whether the chosen format keeps chapter markers.
    Q_PROPERTY(bool chaptersAvailable READ chaptersAvailable NOTIFY settingsChanged)
    Q_PROPERTY(bool streaming READ streaming NOTIFY streamingChanged)
    Q_PROPERTY(bool streamingStopping READ streamingStopping NOTIFY streamingChanged)
    Q_PROPERTY(QString streamingTime READ streamingTime NOTIFY statsChanged)
    // [{id, name, state, message, kbps, dropped, reconnects, …}] while live.
    Q_PROPERTY(QVariantList streamStatus READ streamStatus NOTIFY statsChanged)
    // The profile's stream targets, without their keys.
    Q_PROPERTY(QVariantList streamTargets READ streamTargets NOTIFY streamTargetsChanged)
    // [{id, name, servers: [{name, url}], keyLabel, codecs}] from the services list.
    Q_PROPERTY(QVariantList streamServices READ streamServices CONSTANT)
    Q_PROPERTY(bool keyringAvailable READ keyringAvailable CONSTANT)
    Q_PROPERTY(bool recordWhileStreaming READ recordWhileStreaming WRITE setRecordWhileStreaming NOTIFY settingsChanged)
    Q_PROPERTY(bool stopRecordingWithStream READ stopRecordingWithStream WRITE setStopRecordingWithStream NOTIFY settingsChanged)
    // Replay buffer: running, what it holds now ("18 s · 42 MB"), its
    // settings {seconds, megabytes, prefix, suffix} and roughly how much
    // memory those take ("" when the rate control gives no bitrate).
    Q_PROPERTY(bool replayActive READ replayActive NOTIFY replayChanged)
    Q_PROPERTY(QString replayHeld READ replayHeld NOTIFY statsChanged)
    Q_PROPERTY(QVariantMap replayOptions READ replayOptions NOTIFY settingsChanged)
    Q_PROPERTY(QString replayEstimate READ replayEstimate NOTIFY settingsChanged)
    Q_PROPERTY(bool replayWhileStreaming READ replayWhileStreaming WRITE setReplayWhileStreaming NOTIFY settingsChanged)
    // The program as a camera for other apps: on, and whether one is using it.
    Q_PROPERTY(bool virtualCamera READ virtualCamera NOTIFY virtualCameraChanged)
    Q_PROPERTY(bool virtualCameraInUse READ virtualCameraInUse NOTIFY virtualCameraChanged)
    // What the virtual camera shows: {shows, target, label, gone}; and the
    // program, studios and sources it can show: [{shows, target, label}].
    Q_PROPERTY(QVariantMap cameraShows READ cameraShows NOTIFY virtualCameraChanged)
    Q_PROPERTY(QVariantList cameraChoices READ cameraChoices NOTIFY scenesChanged)
    // MP4 copies: after each recording, and the Remux sheet's list of jobs
    // [{id, name, out, state, progress, error}].
    Q_PROPERTY(bool remuxToMp4 READ remuxToMp4 WRITE setRemuxToMp4 NOTIFY settingsChanged)
    // The height recordings are made at, the canvas's shape (0 = the canvas),
    // and the sizes to offer [{height, label}] for it and for streams.
    Q_PROPERTY(int recordingHeight READ recordingHeight WRITE setRecordingHeight NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList outputSizes READ outputSizes NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList remuxJobs READ remuxJobs NOTIFY remuxChanged)
    Q_PROPERTY(bool remuxBusy READ remuxBusy NOTIFY remuxChanged)
    Q_PROPERTY(QVariantMap meterSettings READ meterSettings NOTIFY settingsChanged)
    Q_PROPERTY(QVariantMap desktopMeter READ desktopMeter NOTIFY levelsChanged)
    Q_PROPERTY(QVariantMap microphoneMeter READ microphoneMeter NOTIFY levelsChanged)
    Q_PROPERTY(QVariantMap sourceAudioMeters READ sourceAudioMeters NOTIFY levelsChanged)
    Q_PROPERTY(qreal desktopLevel READ desktopLevel NOTIFY levelsChanged)
    Q_PROPERTY(QVariantList sourceAudioChannels READ sourceAudioChannels NOTIFY itemsChanged)
    Q_PROPERTY(QVariantList globalAudioDevices READ globalAudioDevices NOTIFY globalAudioDevicesChanged)
    Q_PROPERTY(QVariantMap globalAudioDeviceMeters READ globalAudioDeviceMeters NOTIFY levelsChanged)
    Q_PROPERTY(QVariantMap sourceAudioLevels READ sourceAudioLevels NOTIFY levelsChanged)
    Q_PROPERTY(QVariantList audioSourceTargets READ audioSourceTargets NOTIFY microphonesChanged)
    Q_PROPERTY(QVariantList sourceAudioInputs READ sourceAudioInputs NOTIFY itemsChanged)
    Q_PROPERTY(qreal microphoneLevel READ microphoneLevel NOTIFY levelsChanged)
    Q_PROPERTY(bool desktopMuted READ desktopMuted WRITE setDesktopMuted NOTIFY settingsChanged)
    Q_PROPERTY(bool microphoneMuted READ microphoneMuted WRITE setMicrophoneMuted NOTIFY settingsChanged)
    // [{value, label}] for the picker: Automatic, hardware, virtual, Off.
    Q_PROPERTY(bool desktopAdded READ desktopAdded WRITE setDesktopAdded NOTIFY settingsChanged)
    Q_PROPERTY(bool microphoneAdded READ microphoneAdded WRITE setMicrophoneAdded NOTIFY settingsChanged)
    Q_PROPERTY(bool noSound READ noSound NOTIFY noSoundChanged)
    Q_PROPERTY(QStringList audioInputsInUse READ audioInputsInUse NOTIFY noSoundChanged)
    Q_PROPERTY(QVariantList microphones READ microphones NOTIFY microphonesChanged)
    Q_PROPERTY(QString microphone READ microphone WRITE setMicrophone NOTIFY microphonesChanged)
    Q_PROPERTY(bool microphoneLive READ microphoneLive NOTIFY microphonesChanged)
    Q_PROPERTY(QString microphoneStatus READ microphoneStatus NOTIFY microphonesChanged)
    Q_PROPERTY(QString monitorOutput READ monitorOutput WRITE setMonitorOutput NOTIFY settingsChanged)
    Q_PROPERTY(QString monitoringStatus READ monitoringStatus NOTIFY monitoringChanged)
    Q_PROPERTY(QVariantList monitorOutputs READ monitorOutputs NOTIFY microphonesChanged)
    Q_PROPERTY(int audioSampleRate READ audioSampleRate WRITE setAudioSampleRate NOTIFY settingsChanged)
    Q_PROPERTY(int audioChannelCount READ audioChannelCount WRITE setAudioChannelCount NOTIFY settingsChanged)
    Q_PROPERTY(QStringList audioChannelLabels READ audioChannelLabels NOTIFY settingsChanged)
    Q_PROPERTY(QVariantMap audioPushStates READ audioPushStates NOTIFY audioPushChanged)
    Q_PROPERTY(bool localHotkeysEnabled READ localHotkeysEnabled WRITE setLocalHotkeysEnabled)
    Q_PROPERTY(QVariantMap hotkeys READ hotkeys NOTIFY hotkeysChanged)
    // Tools → Automatic studio switcher: the collection's rules, and the
    // window that has the keyboard as the compositor reports it.
    Q_PROPERTY(bool switcherAvailable READ switcherAvailable CONSTANT)
    Q_PROPERTY(bool switcherEnabled READ switcherEnabled WRITE setSwitcherEnabled NOTIFY switcherChanged)
    Q_PROPERTY(QVariantList switcherRules READ switcherRules WRITE setSwitcherRules NOTIFY switcherChanged)
    Q_PROPERTY(QString switcherOtherwise READ switcherOtherwise WRITE setSwitcherOtherwise NOTIFY switcherChanged)
    Q_PROPERTY(QString focusedWindow READ focusedWindow NOTIFY focusedWindowChanged)
    // Tools → Output timer: seconds after which a recording or a stream
    // stops (0: never), and the seconds each has left (-1: not counting).
    Q_PROPERTY(int recordTimerSeconds READ recordTimerSeconds WRITE setRecordTimerSeconds NOTIFY preferencesChanged)
    Q_PROPERTY(int streamTimerSeconds READ streamTimerSeconds WRITE setStreamTimerSeconds NOTIFY preferencesChanged)
    Q_PROPERTY(int recordTimerLeft READ recordTimerLeft NOTIFY timersChanged)
    Q_PROPERTY(int streamTimerLeft READ streamTimerLeft NOTIFY timersChanged)
    // Tools → Scripts: [{path, name, enabled, running}], and their log.
    Q_PROPERTY(QVariantList scripts READ scripts NOTIFY scriptsChanged)
    Q_PROPERTY(QStringList scriptLog READ scriptLog NOTIFY scriptLogChanged)
    Q_PROPERTY(QString audioHotkeyStatus READ audioHotkeyStatus NOTIFY audioHotkeysChanged)
    Q_PROPERTY(QVariantMap audioHotkeyTriggers READ audioHotkeyTriggers NOTIFY audioHotkeysChanged)
    Q_PROPERTY(bool audioHotkeysBusy READ audioHotkeysBusy NOTIFY audioHotkeysChanged)
    Q_PROPERTY(bool lowLatencyAudio READ lowLatencyAudio WRITE setLowLatencyAudio NOTIFY settingsChanged)
    // App-wide preferences (global.json, not the profile).
    Q_PROPERTY(bool hideFromCapture READ hideFromCapture WRITE setHideFromCapture NOTIFY preferencesChanged)
    // The GPU that composites: "auto", "cpu" or one of gpuChoices' values.
    Q_PROPERTY(QString gpu READ gpu WRITE setGpu NOTIFY preferencesChanged)
    Q_PROPERTY(QVariantList gpuChoices READ gpuChoices CONSTANT)
    Q_PROPERTY(bool captureHidingAvailable READ captureHidingAvailable CONSTANT)
    Q_PROPERTY(bool prompterOpen READ prompterOpen WRITE setPrompterOpen NOTIFY prompterChanged)
    // The playback window: the last recording, played back to check it.
    Q_PROPERTY(bool playbackOpen READ playbackOpen WRITE setPlaybackOpen NOTIFY playbackChanged)
    Q_PROPERTY(QString lastRecording READ lastRecording NOTIFY playbackChanged)
    Q_PROPERTY(QObject *player READ player CONSTANT)
    Q_PROPERTY(bool prompterPlaying READ prompterPlaying WRITE setPrompterPlaying NOTIFY prompterChanged)
    Q_PROPERTY(bool prompterHidden READ prompterHidden NOTIFY prompterChanged)
    Q_PROPERTY(QString prompterText READ prompterText NOTIFY prompterChanged)
    Q_PROPERTY(QString prompterProblem READ prompterProblem NOTIFY prompterChanged)
    Q_PROPERTY(int prompterSpeed READ prompterSpeed WRITE setPrompterSpeed NOTIFY preferencesChanged)
    Q_PROPERTY(int prompterSize READ prompterSize WRITE setPrompterSize NOTIFY preferencesChanged)
    Q_PROPERTY(int prompterWidth READ prompterWidth NOTIFY preferencesChanged)
    Q_PROPERTY(int prompterHeight READ prompterHeight NOTIFY preferencesChanged)
    Q_PROPERTY(bool previewLocked READ previewLocked WRITE setPreviewLocked NOTIFY preferencesChanged)
    Q_PROPERTY(bool showOverflow READ showOverflow WRITE setShowOverflow NOTIFY preferencesChanged)
    Q_PROPERTY(bool studioGrid READ studioGrid WRITE setStudioGrid NOTIFY preferencesChanged)
    // Transitions between studios: [{id, name, settings, builtIn}], the one
    // chosen and its duration, the quick ones [{transition, name, durationMs}],
    // and what each setting can be.
    Q_PROPERTY(QVariantList transitions READ transitions NOTIFY scenesChanged)
    Q_PROPERTY(QString currentTransition READ currentTransition WRITE setCurrentTransition NOTIFY scenesChanged)
    Q_PROPERTY(int transitionDuration READ transitionDuration WRITE setTransitionDuration NOTIFY scenesChanged)
    Q_PROPERTY(QVariantList quickTransitions READ quickTransitions NOTIFY scenesChanged)
    Q_PROPERTY(QVariantList transitionKinds READ transitionKinds CONSTANT)
    Q_PROPERTY(QVariantList transitionSides READ transitionSides CONSTANT)
    Q_PROPERTY(QVariantList transitionMasks READ transitionMasks CONSTANT)
    Q_PROPERTY(bool transitioning READ transitioning NOTIFY transitionStateChanged)
    // The T-bar held, or let go part way with the program waiting between two studios.
    Q_PROPERTY(bool tBarActive READ tBarActive NOTIFY transitionStateChanged)
    Q_PROPERTY(double transitionProgress READ transitionProgress NOTIFY transitionStateChanged)
    // Studio mode: preview (the current studio, edited) beside the program.
    Q_PROPERTY(bool studioMode READ studioMode WRITE setStudioMode NOTIFY transitionStateChanged)
    Q_PROPERTY(QString programScene READ programScene NOTIFY transitionStateChanged)
    // After a take, the preview shows what was on air.
    Q_PROPERTY(bool swapAfterTake READ swapAfterTake WRITE setSwapAfterTake NOTIFY preferencesChanged)
    // A take puts a copy of the layout on air, so edits stay off air until the next.
    Q_PROPERTY(bool takeCopy READ takeCopy WRITE setTakeCopy NOTIFY preferencesChanged)
    Q_PROPERTY(bool takeCopySources READ takeCopySources WRITE setTakeCopySources NOTIFY preferencesChanged)
    // Projector windows, kept open across runs: [{id, shows, target, label,
    // screen, fullscreen, onTop, hideCursor}]; shows is program, preview,
    // studio, source or multiview.
    Q_PROPERTY(QVariantList projectors READ projectors NOTIFY projectorsChanged)
    // The screens a projector can fill: [{index, name, width, height}].
    Q_PROPERTY(QVariantList screens READ screens NOTIFY screensChanged)
    // Multiview: layout (top, bottom, left, right, grid4, grid9, grid16,
    // grid25), names on tiles, safe-area guides, and whether a click on a
    // studio also takes it (studio mode) rather than only previewing it.
    Q_PROPERTY(QString multiviewLayout READ multiviewLayout WRITE setMultiviewLayout NOTIFY preferencesChanged)
    Q_PROPERTY(bool multiviewLabels READ multiviewLabels WRITE setMultiviewLabels NOTIFY preferencesChanged)
    Q_PROPERTY(bool multiviewSafeAreas READ multiviewSafeAreas WRITE setMultiviewSafeAreas NOTIFY preferencesChanged)
    Q_PROPERTY(bool multiviewClickTakes READ multiviewClickTakes WRITE setMultiviewClickTakes NOTIFY preferencesChanged)
    Q_PROPERTY(bool contextBar READ contextBar WRITE setContextBar NOTIFY preferencesChanged)
    // Docks: {"left": [ids], "right": [ids]} (ids not listed are closed),
    // their sizes {"left": w, "right": w, "<id>": h}, and whether they are locked.
    Q_PROPERTY(QVariantMap dockLayout READ dockLayout WRITE setDockLayout NOTIFY preferencesChanged)
    Q_PROPERTY(QVariantMap dockSizes READ dockSizes WRITE setDockSizes NOTIFY preferencesChanged)
    Q_PROPERTY(bool docksLocked READ docksLocked WRITE setDocksLocked NOTIFY preferencesChanged)
    Q_PROPERTY(bool omarchyBarAvailable READ omarchyBarAvailable CONSTANT)
    Q_PROPERTY(bool omarchyBarActive READ omarchyBarActive NOTIFY omarchyBarChanged)
    Q_PROPERTY(bool trayIcon READ trayIcon WRITE setTrayIcon NOTIFY preferencesChanged)
    Q_PROPERTY(bool closeToTray READ closeToTray WRITE setCloseToTray NOTIFY preferencesChanged)
    Q_PROPERTY(bool startInTray READ startInTray WRITE setStartInTray NOTIFY preferencesChanged)
    Q_PROPERTY(bool confirmStart READ confirmStart WRITE setConfirmStart NOTIFY preferencesChanged)
    Q_PROPERTY(bool confirmStop READ confirmStop WRITE setConfirmStop NOTIFY preferencesChanged)
    Q_PROPERTY(bool activeAudioOnly READ activeAudioOnly WRITE setActiveAudioOnly NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList audioFilterTypes READ audioFilterTypes CONSTANT)
    Q_PROPERTY(QVariantList videoFilterTypes READ videoFilterTypes CONSTANT)
    Q_PROPERTY(bool canPasteVideoFilters READ canPasteVideoFilters NOTIFY videoFilterClipboardChanged)
    Q_PROPERTY(QVariantMap desktopAudioSettings READ desktopAudioSettings NOTIFY settingsChanged)
    Q_PROPERTY(QVariantMap microphoneAudioSettings READ microphoneAudioSettings NOTIFY settingsChanged)
    Q_PROPERTY(qreal desktopGain READ desktopGain WRITE setDesktopGain NOTIFY settingsChanged)
    Q_PROPERTY(qreal microphoneGain READ microphoneGain WRITE setMicrophoneGain NOTIFY settingsChanged)
    // Tools (backendtools.cpp). The stats dock's numbers, read about once a
    // second while it is watched: open and not folded.
    Q_PROPERTY(QVariantMap systemStats READ systemStats NOTIFY systemStatsChanged)
    Q_PROPERTY(bool statsWatched READ statsWatched WRITE setStatsWatched NOTIFY systemStatsChanged)
    // The last run did not quit cleanly: the session waits for startAfterCrash().
    Q_PROPERTY(bool crashOffered READ crashOffered NOTIFY startStateChanged)
    // Tools → Set up for…: presets fitted to what this machine measures
    // (core/presets.h). Offered by itself once, on a fresh install.
    Q_PROPERTY(QVariantList setupScenarios READ setupScenarios CONSTANT)
    Q_PROPERTY(bool setupOffered READ setupOffered NOTIFY setupChanged)
    Q_PROPERTY(bool setupBusy READ setupBusy NOTIFY setupChanged)
    Q_PROPERTY(QString setupStep READ setupStep NOTIFY setupChanged)
    Q_PROPERTY(double setupProgress READ setupProgress NOTIFY setupChanged)
    // {ok, cancelled, name, reasons: [{what, value, why}], warnings: [..]}
    Q_PROPERTY(QVariantMap setupResult READ setupResult NOTIFY setupChanged)
    Q_PROPERTY(bool safeMode READ safeMode NOTIFY startStateChanged)
    Q_PROPERTY(bool started READ started NOTIFY startStateChanged)

public:
    // One enclosing group: source pixels to its parent, and whether the item
    // showing it is locked (see groupSteps).
    struct GroupStep { QTransform mapping; bool locked = false; };

    QVariantList audioFilterTypes() const;
    QVariantList videoFilterTypes() const;
    bool canPasteVideoFilters() const { return !m_videoFilterClipboard.isEmpty(); }
    explicit Backend(QObject *parent = nullptr, std::string waylandSocket = {});
    ~Backend() override;

    void start();
    // The control socket, as soon as there is a Backend: until then a second copy
    // cannot tell there is a first. start() does it for itself if not done.
    bool listenForControl(QString *why = nullptr);

    QVariantList scenes() const;
    QVariantList videoDevices() const;
    QString currentScene() const { return m_production.collection().currentScene; }
    void setCurrentScene(const QString &id) { m_production.setCurrentScene(id); }
    QVariantList items() const;
    QVariantList outlines() const;
    QString selectedItem() const { return m_production.selection().value(0); }
    void setSelectedItem(const QString &id) { m_production.select(id.isEmpty() ? QStringList() : QStringList{id}); }
    QStringList selectedItems() const { return m_production.selection(); }
    bool canPastePlacement() const { return m_hasPlacement; }
    bool canPasteSourceReference() const { return m_production.canPasteItems(false); }
    bool canPasteSourceDuplicate() const { return m_production.canPasteItems(true); }
    QVariantList collections() const;
    QString collectionName() const;
    QVariantList profiles() const;
    QString profileId() const { return m_profileId; }
    QString profileName() const { return m_session.profile().name; }
    Q_INVOKABLE bool switchProfile(const QString &id);
    Q_INVOKABLE bool newProfile(const QString &name);
    Q_INVOKABLE bool duplicateProfile();
    Q_INVOKABLE bool renameProfile(const QString &name);
    Q_INVOKABLE bool removeProfile(const QString &id);
    Q_INVOKABLE void importProfile();
    Q_INVOKABLE void exportProfile();
    bool importProfileFrom(const QString &path);
    bool exportProfileTo(const QString &path);
    QVariantList missingFiles() const;
    Q_INVOKABLE void switchCollection(const QString &file);
    Q_INVOKABLE void newCollection(const QString &name);
    Q_INVOKABLE void duplicateCollection();
    Q_INVOKABLE void renameCollection(const QString &name);
    Q_INVOKABLE void removeCollection(const QString &file);
    Q_INVOKABLE void importCollection();
    Q_INVOKABLE void exportCollection();
    // Point one missing source at its file (then its neighbours too), or
    // search a folder and its subfolders for all of them by name.
    Q_INVOKABLE void locateMissing(const QString &sourceId, int index = -1, const QString &key = {});
    int relinkMissingTo(const QString &sourceId, const QString &key, int index, const QString &expectedPath, const QString &replacement);
    Q_INVOKABLE void searchForMissing();
    // Relinks missing files found by name under `folder`; how many.
    int relinkFrom(const QString &folder, bool recursive);
    void setSelectedItems(const QStringList &ids) { m_production.select(ids); }
    QVariantMap selected() const;
    QVariantMap evenRateFor(const QString &sourceId) const;
    QString probeNote() const;
    Q_INVOKABLE void useEvenRate(const QString &sourceId);
    QVariantMap selectedSlideshow() const;
    Q_INVOKABLE void controlSlideshow(const QString &sourceId, const QString &action, double value = 0);
    QVariantMap selectedMedia() const;
    Q_INVOKABLE void controlMedia(const QString &sourceId, const QString &action, double value = 0);
    bool canUndo() const { return m_production.canUndo(); }
    bool canRedo() const { return m_production.canRedo(); }
    QString undoText() const { return m_production.undoText(); }
    QString redoText() const { return m_production.redoText(); }
    Production &production() { return m_production; }
    Session &session() { return m_session; }
    bool recordingStopping() const { return m_session.recordingStopping(); }
    bool recording() const { return m_session.recording(); }
    QString recordingTime() const { return m_recordingTime; }
    QString recordingFile() const;
    QString status() const { return m_status; }
    QString statsLine() const { return m_statsLine; }
    QString engineName() const { return m_engineName; }
    QString engineWhy() const { return m_engineWhy; }
    bool engineOnBattery() const { return m_engineOnBattery; }
    QString frameRateNote() const;
    int canvasWidth() const { return m_session.canvasWidth(); }
    int outputWidth() const { return m_session.outputSize(m_session.profile().recordingHeight).width(); }
    int canvasHeight() const { return m_session.canvasHeight(); }
    bool canvasFollowsDisplay() const { return m_session.canvasFollowsDisplay(); }
    int fps() const { return m_session.profile().fpsNum / m_session.profile().fpsDen; }
    int fpsNum() const { return m_session.profile().fpsNum; }
    int fpsDen() const { return m_session.profile().fpsDen; }
    QString colourFormat() const { return m_session.profile().colourFormat; }
    QString colourSpace() const { return m_session.profile().colourSpace; }
    bool colourFullRange() const { return m_session.profile().colourFullRange; }
    int sdrWhite() const { return m_session.profile().sdrWhite; }
    int hdrPeak() const { return m_session.profile().hdrPeak; }
    QString downscaleFilter() const { return m_session.profile().downscaleFilter; }
    QVariantList colourFormats() const;
    QString colourNote() const;
    QVariantList encoders() const;
    QString encoder() const { return m_session.profile().encoder; }
    QString encoderNote() const;
    QVariantList splitEncodeModes() const;
    bool encoderIsForEditing() const;
    int quality() const { return m_session.profile().quality; }
    QString folder() const { return m_session.profile().resolvedFolder(); }
    QString fileNamePattern() const { return m_session.profile().filenameTemplate; }
    bool fileNameNoSpaces() const { return m_session.profile().filenameNoSpaces; }
    // The name a recording started now would get (before any " (2)").
    Q_INVOKABLE QString fileNamePreview(const QString &pattern, bool noSpaces) const;
    QString container() const { return m_session.profile().container; }
    QVariantList fileFormats() const;
    QString customMuxer() const { return m_session.profile().customMuxer; }
    QString containerOptions() const { return m_session.profile().containerOptions; }
    // What the container recorded into is called, for people.
    QString containerName() const;
    // Why a profile's colour cannot be recorded here (empty when it can).
    QString colourRefusal(const Profile &candidate) const;
    bool applyColour(const Profile &candidate);
    QVariantMap recordingOptions() const;
    bool recordingPaused() const { return m_session.recordingPaused(); }
    bool chaptersAvailable() const;
    bool streaming() const { return m_session.streaming(); }
    bool streamingStopping() const { return m_session.streamingStopping(); }
    QString streamingTime() const { return m_streamingTime; }
    QVariantList streamStatus() const;
    QVariantList streamTargets() const;
    QVariantList streamServices() const;
    bool keyringAvailable() const { return m_secrets.available(); }
    bool recordWhileStreaming() const { return m_session.profile().recordWhileStreaming; }
    void setRecordWhileStreaming(bool on);
    bool stopRecordingWithStream() const { return m_session.profile().stopRecordingWithStream; }
    void setStopRecordingWithStream(bool on);
    bool replayActive() const { return m_session.replayActive(); }
    QString replayHeld() const;
    QVariantMap replayOptions() const;
    QString replayEstimate() const;
    bool replayWhileStreaming() const { return m_session.profile().replayWhileStreaming; }
    void setReplayWhileStreaming(bool on);
    bool virtualCamera() const { return m_session.virtualCameraRunning(); }
    bool virtualCameraInUse() const { return m_session.virtualCameraInUse(); }
    QVariantMap cameraShows() const;
    QVariantList cameraChoices() const;
    bool remuxToMp4() const { return m_session.profile().remuxToMp4; }
    int recordingHeight() const { return m_session.profile().recordingHeight; }
    void setRecordingHeight(int height);
    QVariantList outputSizes() const;
    void setRemuxToMp4(bool on);
    QVariantList remuxJobs() const;
    bool remuxBusy() const { return m_session.remux().busy(); }
    QVariantMap meterSettings() const;
    QVariantMap desktopMeter() const { return m_desktopMeter; }
    QVariantMap microphoneMeter() const { return m_microphoneMeter; }
    QVariantMap sourceAudioMeters() const { return m_sourceAudioMeters; }
    Q_INVOKABLE void setMeterSetting(const QString &key, const QVariant &value);
    Q_INVOKABLE void resetMeterPeak(int globalChannel, const QString &sourceId = QString());
    qreal desktopLevel() const { return m_desktopLevel; }
    qreal microphoneLevel() const { return m_microphoneLevel; }
    bool desktopMuted() const { return m_session.profile().desktopMuted; }
    bool desktopAdded() const { return m_session.profile().desktopAdded; }
    bool microphoneAdded() const { return m_session.profile().microphoneAdded; }
    void setDesktopAdded(bool added);
    void setMicrophoneAdded(bool added);
    // Nothing in the mixer can bring in any sound, so what goes out is silent.
    bool noSound() const;
    QStringList audioInputsInUse() const;
    bool microphoneMuted() const { return m_session.profile().microphoneMuted; }
    QVariantList microphones() const;
    QVariantList sourceAudioChannels() const;
    QVariantList globalAudioDevices() const;
    QVariantMap globalAudioDeviceMeters() const { return m_globalAudioDeviceMeters; }
    Q_INVOKABLE QString addGlobalAudioDevice(const QString &kind);
    Q_INVOKABLE void removeGlobalAudioDevice(const QString &id);
    Q_INVOKABLE void setGlobalAudioDeviceSetting(const QString &id, const QString &key, const QVariant &value);
    Q_INVOKABLE void resetGlobalAudioDevicePeak(const QString &id);
    QString monitorOutput() const { return m_session.profile().monitorOutput; }
    void setMonitorOutput(const QString &choice);
    QString monitoringStatus() const { return m_session.monitoringStatus(); }
    QVariantList monitorOutputs() const;
    QVariantMap audioPushStates() const;
    bool localHotkeysEnabled() const { return m_localHotkeysEnabled; }
    void setLocalHotkeysEnabled(bool enabled);
    QString audioHotkeyStatus() const { return m_audioHotkeys.status(); }
    QVariantMap audioHotkeyTriggers() const { return m_audioHotkeys.triggers(); }
    bool audioHotkeysBusy() const { return m_audioHotkeys.busy(); }
    Q_INVOKABLE void applyGlobalHotkeys();
    Q_INVOKABLE void stopGlobalHotkeys();
    Q_INVOKABLE void holdAudioPush(const QString &target, bool talk, bool held) { m_session.holdAudioPush(target, talk, held, 1); emit audioPushChanged(); }
    Q_INVOKABLE void cancelAudioPush(const QString &target) { m_session.cancelAudioPush(target, 1); emit audioPushChanged(); }
    bool lowLatencyAudio() const { return m_session.profile().lowLatencyAudio; }
    // Off unless chosen: Hyprland shows a hidden window black in every capture, screenshots included.
    bool hideFromCapture() const { return preference(QStringLiteral("hideFromCapture"), false).toBool(); }
    void setHideFromCapture(bool hide);
    QString gpu() const { return preference(QStringLiteral("gpu"), QStringLiteral("auto")).toString(); }
    void setGpu(const QString &choice);
    QVariantList gpuChoices() const;
    bool captureHidingAvailable() const { return HyprlandRules::available(); }
    bool prompterOpen() const { return m_prompterOpen; }
    void setPrompterOpen(bool open);
    bool prompterPlaying() const { return m_prompterPlaying; }
    void setPrompterPlaying(bool playing);
    // Whether the capture rule took: the window says so when it did not.
    bool prompterHidden() const { return m_prompterHidden; }
    QString prompterText() const { return m_prompterText; }
    QString prompterProblem() const { return m_prompterProblem; }
    QString prompterFile() const { return preference(QStringLiteral("prompterFile"), QString()).toString(); }
    int prompterSpeed() const { return preference(QStringLiteral("prompterSpeed"), 5).toInt(); }
    void setPrompterSpeed(int speed);
    int prompterSize() const { return preference(QStringLiteral("prompterSize"), 48).toInt(); }
    void setPrompterSize(int size);
    int prompterWidth() const { return preference(QStringLiteral("prompterWidth"), 800).toInt(); }
    int prompterHeight() const { return preference(QStringLiteral("prompterHeight"), 450).toInt(); }
    Q_INVOKABLE void setPrompterWindowSize(int width, int height);
    Q_INVOKABLE void restartPrompter();
    Q_INVOKABLE bool loadPrompter(const QString &path);
    Q_INVOKABLE void choosePrompterFile();
    bool previewLocked() const { return preference(QStringLiteral("previewLocked"), false).toBool(); }
    void setPreviewLocked(bool locked) { setPreference(QStringLiteral("previewLocked"), locked); }
    bool showOverflow() const { return preference(QStringLiteral("showOverflow"), false).toBool(); }
    void setShowOverflow(bool show) { setPreference(QStringLiteral("showOverflow"), show); }
    bool studioGrid() const { return preference(QStringLiteral("studioGrid"), false).toBool(); }
    QVariantList transitions() const;
    QString currentTransition() const { return m_production.collection().currentTransition; }
    void setCurrentTransition(const QString &id) { m_production.setCurrentTransition(id, transitionDuration()); }
    int transitionDuration() const { return m_production.collection().transitionMs; }
    void setTransitionDuration(int ms) { m_production.setCurrentTransition(currentTransition(), ms); }
    QVariantList quickTransitions() const;
    QVariantList transitionKinds() const;
    QVariantList transitionSides() const;
    QVariantList transitionMasks() const;
    bool transitioning() const { return m_session.transitioning(); }
    // A T-bar move is under way (held or parked part way): the session's
    // transition by hand, so there is one copy of that state.
    bool tBarActive() const { return m_session.manualTransition(); }
    double transitionProgress() const { return m_session.transitionProgress(); }
    bool studioMode() const { return m_session.studioMode(); }
    void setStudioMode(bool on);
    QString programScene() const { return m_session.programScene(); }
    bool swapAfterTake() const { return preference(QStringLiteral("swapAfterTake"), true).toBool(); }
    void setSwapAfterTake(bool swap) { setPreference(QStringLiteral("swapAfterTake"), swap); }
    bool takeCopy() const { return preference(QStringLiteral("takeCopy"), false).toBool(); }
    bool takeCopySources() const { return preference(QStringLiteral("takeCopySources"), false).toBool(); }
    QVariantList projectors() const;
    QVariantList screens() const;
    QString multiviewLayout() const { return preference(QStringLiteral("multiviewLayout"), QStringLiteral("top")).toString(); }
    void setMultiviewLayout(const QString &layout) { setPreference(QStringLiteral("multiviewLayout"), layout); }
    bool multiviewLabels() const { return preference(QStringLiteral("multiviewLabels"), true).toBool(); }
    void setMultiviewLabels(bool on) { setPreference(QStringLiteral("multiviewLabels"), on); }
    bool multiviewSafeAreas() const { return preference(QStringLiteral("multiviewSafeAreas"), false).toBool(); }
    void setMultiviewSafeAreas(bool on) { setPreference(QStringLiteral("multiviewSafeAreas"), on); }
    bool multiviewClickTakes() const { return preference(QStringLiteral("multiviewClickTakes"), false).toBool(); }
    void setMultiviewClickTakes(bool on) { setPreference(QStringLiteral("multiviewClickTakes"), on); }
    void setTakeCopy(bool copy) { setPreference(QStringLiteral("takeCopy"), copy); }
    void setTakeCopySources(bool copy) { setPreference(QStringLiteral("takeCopySources"), copy); m_session.setCopySources(copy); }
    void setStudioGrid(bool grid) { setPreference(QStringLiteral("studioGrid"), grid); }
    bool contextBar() const { return preference(QStringLiteral("contextBar"), true).toBool(); }
    QVariantMap dockLayout() const { return preference(QStringLiteral("docks"), QVariantMap()).toMap(); }
    void setDockLayout(const QVariantMap &layout) { setPreference(QStringLiteral("docks"), layout); }
    QVariantMap dockSizes() const { return preference(QStringLiteral("dockSizes"), QVariantMap()).toMap(); }
    void setDockSizes(const QVariantMap &sizes) { setPreference(QStringLiteral("dockSizes"), sizes); }
    bool docksLocked() const { return preference(QStringLiteral("docksLocked"), false).toBool(); }
    void setDocksLocked(bool locked) { setPreference(QStringLiteral("docksLocked"), locked); }
    void setContextBar(bool show) { setPreference(QStringLiteral("contextBar"), show); }
    bool trayIcon() const { return preference(QStringLiteral("trayIcon"), true).toBool(); }
    void setTrayIcon(bool on) { setPreference(QStringLiteral("trayIcon"), on); }
    bool omarchyBarAvailable() const { return m_omarchyBar.available(); }
    bool omarchyBarActive() const { return m_omarchyBar.active(); }
    Q_INVOKABLE void setOmarchyBar(bool on);
    void startOmarchyBar();
    bool closeToTray() const { return preference(QStringLiteral("closeToTray"), false).toBool(); }
    void setCloseToTray(bool on) { setPreference(QStringLiteral("closeToTray"), on); }
    bool startInTray() const { return preference(QStringLiteral("startInTray"), false).toBool(); }
    void setStartInTray(bool on) { setPreference(QStringLiteral("startInTray"), on); }
    bool confirmStart() const { return preference(QStringLiteral("confirmStart"), false).toBool(); }
    void setConfirmStart(bool on) { setPreference(QStringLiteral("confirmStart"), on); }
    bool confirmStop() const { return preference(QStringLiteral("confirmStop"), false).toBool(); }
    void setConfirmStop(bool on) { setPreference(QStringLiteral("confirmStop"), on); }
    // Before the first window maps: keeps our windows out of screen capture.
    void applyCaptureHiding();
    int audioSampleRate() const { return m_session.profile().audioFormat.rate; }
    int audioChannelCount() const { return m_session.profile().audioFormat.channels; }
    QStringList audioChannelLabels() const;
    void setAudioSampleRate(int rate);
    void setAudioChannelCount(int channels);
    void setLowLatencyAudio(bool low);
    bool activeAudioOnly() const { return m_session.profile().activeAudioOnly; }
    void setActiveAudioOnly(bool active);
    QVariantMap desktopAudioSettings() const;
    QVariantMap microphoneAudioSettings() const;
    Q_INVOKABLE void setGlobalAudioSetting(int channel, const QString &key, const QVariant &value);
    QVariantList sourceAudioInputs() const;
    QVariantList audioSourceTargets() const;
    QVariantMap sourceAudioLevels() const { return m_sourceAudioLevels; }
    QString microphone() const { return m_session.profile().microphone; }
    bool microphoneLive() const { return m_session.microphone().state == MicrophonePick::Using; }
    QString microphoneStatus() const;
    qreal desktopGain() const { return m_session.profile().desktopGain; }
    qreal microphoneGain() const { return m_session.profile().microphoneGain; }

    void setFps(int fps);
    Q_INVOKABLE void setFrameRate(int num, int den);
    void setColourFormat(const QString &format);
    void setColourSpace(const QString &space);
    void setColourFullRange(bool full);
    void setSdrWhite(int nits);
    void setHdrPeak(int nits);
    void setDownscaleFilter(const QString &filter);
    void setEncoder(const QString &name);
    void setQuality(int quality);
    void setFolder(const QString &folder);
    // The desktop's folder picker, opening in the current recordings folder.
    Q_INVOKABLE void chooseRecordingFolder();
    void setFileNamePattern(const QString &pattern);
    void setFileNameNoSpaces(bool noSpaces);
    void setContainer(const QString &container);
    void setCustomMuxer(const QString &muxer);
    void setContainerOptions(const QString &options);
    int recordingTracks() const { return int(m_session.profile().recordingTracks); }
    void setRecordingTracks(int tracks);
    void setDesktopMuted(bool muted);
    void setMicrophoneMuted(bool muted);
    void setMicrophone(const QString &choice);
    void setDesktopGain(qreal gain);
    void setMicrophoneGain(qreal gain);

    // Pure helpers.
    static QString formatDuration(qint64 ns);
    static QString formatBytes(quint64 bytes);
    static qreal levelToMeter(float peak);   // linear peak -> 0..1 on a dB scale
    // Where a phone should send a stream that listens on this port: one entry
    // per network link, labelled ("iPhone over USB", "Wi-Fi", "Tailscale").
    // With a passphrase each also has what to type both ways: "url" (and "streamKey"
    // on RTMP) for an app with separate fields, "inAddress" for one with a single field.
    static QVariantList sendAddresses(const QString &protocol, int port, const QString &passphrase = {});
    static QString listenUrl(const QString &protocol, int port, int latencyMs);
    // When the firewall (ufw) would turn the phone away: the command that
    // lets it in. Empty when nothing is in the way, or no firewall is known.
    static QString firewallHint(const QString &protocol, int port, const QString &ufwDir = QStringLiteral("/etc/ufw"));

    // Scenes
    Q_INVOKABLE void addScene();
    Q_INVOKABLE void removeScene(const QString &id);
    Q_INVOKABLE void renameScene(const QString &id, const QString &name);
    Q_INVOKABLE void duplicateScene(const QString &id);
    Q_INVOKABLE void moveScene(const QString &id, int toIndex);

    // Transitions. A new one takes its kind's name and becomes the choice;
    // settings change one key at a time (sliders pass a merge key).
    Q_INVOKABLE QString addTransition(const QString &kind);
    Q_INVOKABLE void removeTransition(const QString &id);
    Q_INVOKABLE void renameTransition(const QString &id, const QString &name);
    Q_INVOKABLE void setTransitionSetting(const QString &id, const QString &key, const QVariant &value, const QString &mergeKey = {});
    Q_INVOKABLE QVariantMap transitionSettings(const QString &id) const;
    // A stinger's video, through the desktop's file chooser.
    // key: file (the video) or matteFile (a separate track matte).
    Q_INVOKABLE void chooseStingerFile(const QString &id, const QString &key = QStringLiteral("file"));
    // A studio's own transition ("" follows the collection's) and duration (0 likewise).
    Q_INVOKABLE void setSceneTransition(const QString &sceneId, const QString &transition, int durationMs);
    Q_INVOKABLE void addQuickTransition(const QString &transition, int durationMs);
    Q_INVOKABLE void removeQuickTransition(int index);
    // Studio mode: puts the previewed studio on air, through the chosen
    // transition or a quick one (index); the T-bar drives one by hand, 0..1.
    // Pictures for other windows: a view is asked for by id until released;
    // viewReady carries its pictures. See Session::View.
    Q_INVOKABLE void requestView(const QString &id, const QString &shows, const QString &target, int width, int height, int fps = 30);
    Q_INVOKABLE void releaseView(const QString &id);
    // Projectors: opened on a screen (-1 windowed), changed, closed.
    Q_INVOKABLE QString openProjector(const QString &shows, const QString &target, int screen);
    Q_INVOKABLE void setProjectorOption(const QString &id, const QString &key, const QVariant &value);
    Q_INVOKABLE void closeProjector(const QString &id);
    // A full-size PNG of the program, studio mode's preview, a studio or a
    // source in the recording folder; screenshotSaved or failed says how it went.
    Q_INVOKABLE void screenshot(const QString &shows, const QString &target = {});
    // Every action that can be bound to a key or run from outside, as
    // [{id, argument, group, name, global}] for what exists now; runAction
    // does one (argument by id or name) and says why not ("" when done).
    Q_INVOKABLE QVariantList actions() const;
    Q_INVOKABLE QString runAction(const QString &action, const QString &argument = {});
    // What is running, for `omabroadcast ctl status` and remote control.
    Q_INVOKABLE QVariantMap state() const;
    // Remote control over the network (Tools → Remote control):
    // {enabled, port, network, auth, password, listening, error,
    // clients: [{address, since}], addresses: this machine's}. Options:
    // enabled, port, network (other machines too), auth, password.
    Q_PROPERTY(QVariantMap remoteControl READ remoteControl NOTIFY remoteControlChanged)
    QVariantMap remoteControl() const;
    Q_INVOKABLE void setRemoteOption(const QString &key, const QVariant &value);
    Q_INVOKABLE QString newRemotePassword() const;
    // The requests remote control answers, besides the protocol's own.
    static QStringList remoteRequests();
    // Keys for actions: {"action|argument": "Ctrl+F1"}. Keys for actions
    // without an argument are kept in the profile, the rest in the collection.
    QVariantMap hotkeys() const;
    bool switcherAvailable() const { return FocusWatcher::available(); }
    bool switcherEnabled() const { return m_production.collection().switcher.value("enabled").toBool(); }
    void setSwitcherEnabled(bool on);
    QVariantList switcherRules() const { return m_production.collection().switcher.value("rules").toList(); }
    void setSwitcherRules(const QVariantList &rules);
    QString switcherOtherwise() const { return m_production.collection().switcher.value("otherwise").toString(); }
    void setSwitcherOtherwise(const QString &studio);
    QString focusedWindow() const;
    // A window had the keyboard (the watcher's report).
    Q_INVOKABLE void windowFocused(const QString &windowClass, const QString &title);
    int recordTimerSeconds() const { return preference(QStringLiteral("recordTimerSeconds"), 0).toInt(); }
    void setRecordTimerSeconds(int seconds);
    int streamTimerSeconds() const { return preference(QStringLiteral("streamTimerSeconds"), 0).toInt(); }
    void setStreamTimerSeconds(int seconds);
    int recordTimerLeft() const;
    QVariantList scripts() const;
    QStringList scriptLog() const { return m_scripts.log(); }
    Q_INVOKABLE void addScript();  // through the desktop's file chooser
    Q_INVOKABLE void addScriptFile(const QString &path);
    Q_INVOKABLE void removeScript(const QString &path);
    Q_INVOKABLE void setScriptEnabled(const QString &path, bool enabled);
    Q_INVOKABLE void reloadScript(const QString &path) { m_scripts.reload(path); emit scriptsChanged(); }
    Q_INVOKABLE void clearScriptLog() { m_scripts.clearLog(); }
    // A script's declared form [{key, label, type, value}] and setting one value.
    Q_INVOKABLE QVariantList scriptForm(const QString &path) const;
    Q_INVOKABLE void setScriptSetting(const QString &path, const QString &key, const QVariant &value);
    ScriptHost &scriptHost() { return m_scripts; }
    int streamTimerLeft() const;
    // When keys run: always, only while this app has focus ("focused"), or
    // only while another app has it ("unfocused").
    Q_PROPERTY(QString hotkeyFocus READ hotkeyFocus WRITE setHotkeyFocus NOTIFY preferencesChanged)
    QString hotkeyFocus() const { return preference(QStringLiteral("hotkeyFocus"), QStringLiteral("always")).toString(); }
    void setHotkeyFocus(const QString &when) { setPreference(QStringLiteral("hotkeyFocus"), when); }
    // A key for an action went down, here (`local`) or through the desktop.
    void runHotkey(const QString &action, const QString &argument, bool local);
    // An empty key clears it.
    Q_INVOKABLE void setHotkey(const QString &action, const QString &argument, const QString &key);
    // The key as text from a key press, the way hotkeys are kept ("" for a lone modifier).
    Q_INVOKABLE QString keyText(int key, int modifiers) const;
    // What else uses this key ("" when nothing does).
    Q_INVOKABLE QString hotkeyClash(const QString &action, const QString &argument, const QString &key) const;
    Q_INVOKABLE void take();
    Q_INVOKABLE void quickTake(int index);
    Q_INVOKABLE void setTBar(double position);
    Q_INVOKABLE void releaseTBar();

    // Sources and items. Adding puts the new item in the live scene, selected.
    Q_INVOKABLE void addDisplay();
    Q_INVOKABLE void addWindow();
    Q_INVOKABLE void addRegion();
    Q_INVOKABLE void requestCaptureRegionPreview(const QString &sourceId);
    // Draw a region source's rectangle: on the screen itself where Hyprland
    // and slurp are there, else on a snapshot (requestCaptureRegionPreview).
    Q_INVOKABLE void drawCaptureRegion(const QString &sourceId);
    Q_INVOKABLE void setCaptureRegion(const QString &sourceId, double x, double y, double width, double height);
    Q_INVOKABLE void addColour();
    Q_INVOKABLE void addImage();
    Q_INVOKABLE void addText();
    // Images from a folder, one after another.
    Q_INVOKABLE void addSlideshow();
    Q_INVOKABLE QVariantList slideshowItems(const QString &sourceId) const;
    Q_INVOKABLE bool hasSlideshow(const QString &sourceId) const;
    Q_INVOKABLE bool setSlideshowEntries(const QString &sourceId, const QVariantList &entries);
    Q_INVOKABLE void pickSlideshowEntries(const QString &sourceId, bool folder, int replaceIndex = -1);
    Q_INVOKABLE void editSlideshow(const QString &sourceId) { emit slideshowEditRequested(sourceId); }
    // An OmaShow presentation: a slideshow stepped by hand, a picture for every
    // click, drawn by OmaShow's own export. Hidden where OmaShow is not installed.
    Q_PROPERTY(bool presentationAvailable READ presentationAvailable CONSTANT)
    static bool presentationAvailable();
    // A deck by its path, or "" to choose one.
    bool playbackOpen() const { return m_playbackOpen; }
    void setPlaybackOpen(bool open);
    // This run's last finished recording, or else the newest in the recordings folder.
    QString lastRecording() const;
    QObject *player() { return &m_player; }
    // Where the playback window's sound goes ("" for none) and why it is
    // silent, when it is.
    std::string playbackSoundTarget(QString *why) const;
    // Whether sound played into `output` would be taken back in by what is
    // going out now: a recording, a stream or the replay buffer, through
    // Desktop sound (which hears `defaultOutput`) or an output captured by name.
    bool playbackWouldGoOut(const std::string &output, const std::string &defaultOutput) const;
    Q_INVOKABLE void addPresentation(const QString &deck = QString());
    Q_INVOKABLE void reloadPresentation(const QString &sourceId);
    // Removes the picture folders of presentations that nothing uses any more:
    // not a source in any collection, a step Undo could bring back, or what
    // was copied. Run at start and at the end, never while one is loading into
    // it. How many went.
    int sweepPresentationFolders();
    Q_INVOKABLE quint64 collectionGeneration() const { return m_production.generation(); }
    // The text a text source shows comes from this file, kept up to date.
    Q_INVOKABLE void chooseTextFile(const QString &sourceId);
    // A source's video filters (settings key "videoFilters"), one undo step per change.
    Q_INVOKABLE QVariantList videoFilters(const QString &sourceId) const;
    // {name, kind, async}: async sources arrive on their own clock (video delay suits them); empty when gone.
    Q_INVOKABLE QVariantMap videoFilterSource(const QString &sourceId) const;
    // `gesture`: a drag's many changes are one undo step, closed by endGesture().
    Q_INVOKABLE void setVideoFilters(const QString &sourceId, const QVariantList &filters, bool gesture = false);
    Q_INVOKABLE void editVideoFilters(const QString &sourceId) { emit videoFiltersEditRequested(sourceId); }
    // Copies to paste onto other sources, after their own filters.
    Q_INVOKABLE void copyVideoFilters(const QString &sourceId, const QString &filterId = QString());
    Q_INVOKABLE void pasteVideoFilters(const QString &sourceId);
    // Picks the LUT or picture for one filter's file parameter.
    Q_INVOKABLE void chooseVideoFilterFile(const QString &sourceId, const QString &filterId, const QString &key);
    // A chroma key's backdrop colour from the source's own picture (before its
    // filters): at (x, y), 0..1 of the picture, or found by itself.
    Q_INVOKABLE void findKeyBackdrop(const QString &sourceId, const QString &filterId, double x = -1, double y = -1);
    // What a source's video filters cost against the frame there is:
    // {known, ms, budgetMs, frameMs, level: fine|note|warn, text}.
    Q_INVOKABLE QVariantMap videoFilterLoad(const QString &sourceId) const;
    Q_INVOKABLE QStringList videoFilterProblems(const QString &sourceId) const { return m_session.videoFilterProblems(sourceId); }
    // A camera or capture card, by its node.name.
    Q_INVOKABLE void addDevice(const QString &node);
    Q_INVOKABLE void addAudioSource(const QString &kind, const QString &target = QString());
    // A video file (picked), or a network stream a phone sends to.
    Q_INVOKABLE void addMedia();
    Q_INVOKABLE void addNetworkStream();
    Q_INVOKABLE void replaceMedia(const QString &sourceId);
    // Rebuilds a network stream's listening address from its parts.
    Q_INVOKABLE void setNetworkStream(const QString &sourceId, const QString &protocol, int port, int latencyMs);
    // Empty turns protection off. Answers what is wrong with it, or nothing when
    // it is set; a passphrase refused leaves the one there was.
    Q_INVOKABLE QString setNetworkPassphrase(const QString &sourceId, const QString &passphrase);
    Q_INVOKABLE void newNetworkPassphrase(const QString &sourceId);
    // A capture device's own picture control; saved with the source and
    // re-applied whenever the device starts.
    Q_INVOKABLE void setDeviceControl(const QString &sourceId, int controlId, int value, bool gesture = false);
    Q_INVOKABLE void resetDeviceControls(const QString &sourceId);
    // A fixed canvas size; every scene's layout scales with it.
    Q_INVOKABLE bool setCanvasSize(int width, int height);
    // Picks a new file for an image source.
    Q_INVOKABLE void replaceImage(const QString &sourceId);
    Q_INVOKABLE void removeItem(const QString &id);
    // Every selected source, as one step (Delete, the Edit menu).
    Q_INVOKABLE void removeSelectedItems();
    // Drag and drop in the list: `id` goes just above (or below) `targetId` as
    // the list shows them; both must be in the same studio or group.
    Q_INVOKABLE void dropItem(const QString &id, const QString &targetId, bool below);
    // Placement (position, size, rotation, crop, scaling) from one item onto others.
    Q_INVOKABLE void copyPlacement(const QString &id);
    Q_INVOKABLE void pastePlacement();
    Q_INVOKABLE void copySources();
    Q_INVOKABLE void pasteSources(bool duplicate = false);
    // Another studio as a source in this one.
    Q_INVOKABLE void addStudio(const QString &studioId);
    // The selection into a group (all from the same studio or group), and back.
    Q_INVOKABLE void groupSelected();
    Q_INVOKABLE void ungroupSelected();
    Q_INVOKABLE void setItemVisible(const QString &id, bool visible);
    Q_INVOKABLE void setItemLocked(const QString &id, bool locked);
    // show: how it appears, else how it goes; transition "" for at once.
    Q_INVOKABLE void setItemTransition(const QString &id, bool show, const QString &transition, int ms);
    // Steps an item up (towards the top) or down the stack.
    Q_INVOKABLE void raiseItem(const QString &id, int steps);
    Q_INVOKABLE void renameSource(const QString &sourceId, const QString &name);
    Q_INVOKABLE void reopenSource(const QString &sourceId);
    Q_INVOKABLE void addDirectCapture(bool window, bool focused = false);
    Q_INVOKABLE void chooseDirectCapture(const QString &sourceId);
    Q_INVOKABLE QVariantMap directCaptureCatalog();
    Q_INVOKABLE QString captureSourceKind(const QString &sourceId) const;
    Q_INVOKABLE bool setDirectCaptureTarget(const QString &sourceId, const QVariantMap &target, bool focused = false);
    Q_INVOKABLE void usePortalCapture(const QString &sourceId);

    // Browser sources and custom browser docks: Qt WebEngine is optional and
    // both are hidden without it (see BrowserPage::helper()).
    Q_PROPERTY(bool browserAvailable READ browserAvailable CONSTANT)
    Q_PROPERTY(bool webDocksAvailable READ webDocksAvailable CONSTANT)
    bool browserAvailable() const;
    // Docks load Qt WebEngine's QML in this process; the module must be there.
    static bool webDocksAvailable();
    // A page by its address ("" asks in the inspector), or a local HTML file.
    Q_INVOKABLE void addBrowser(const QString &url = QString());
    Q_INVOKABLE void chooseBrowserFile(const QString &sourceId);
    Q_INVOKABLE void reloadBrowser(const QString &sourceId);
    // The interact window: it shows the page and passes on what is done in it.
    Q_INVOKABLE void openInteract(const QString &sourceId) { emit interactRequested(sourceId); }
    Q_INVOKABLE void watchBrowser(const QString &sourceId, bool watched) { m_session.setBrowserWatched(sourceId, watched); }
    Q_INVOKABLE void browserInput(const QString &sourceId, const QVariantMap &event) { m_session.browserInput(sourceId, event); }
    // {name, state, title, starts, pid, sound}; empty when there is no such browser source.
    Q_INVOKABLE QVariantMap browserStatus(const QString &sourceId) const;
    // Custom browser docks, app-wide: [{id, name, url}]. Ids are "web-<n>".
    Q_PROPERTY(QVariantList browserDocks READ browserDocks NOTIFY preferencesChanged)
    QVariantList browserDocks() const { return preference(QStringLiteral("browserDocks"), QVariantList()).toList(); }
    Q_INVOKABLE QString addBrowserDock(const QString &name, const QString &url);
    Q_INVOKABLE bool setBrowserDock(const QString &id, const QString &name, const QString &url);
    Q_INVOKABLE void removeBrowserDock(const QString &id);
    // What a dock loads for what people typed.
    Q_INVOKABLE static QString browserAddress(const QString &typed);
    Q_INVOKABLE void setSourceSetting(const QString &sourceId, const QString &key, const QVariant &value,
                                      bool gesture = false);

    // Transforms. `gesture` folds a drag's many calls into one undo step,
    // closed by endGesture().
    Q_INVOKABLE void setItemBox(const QString &id, qreal x, qreal y, qreal width, qreal height, bool gesture = false);
    Q_INVOKABLE void setItemRotation(const QString &id, qreal degrees, bool gesture = false);
    // [{id, x, y, width, height}] in canvas coordinates, as one step.
    Q_INVOKABLE void setItemBoxes(const QVariantList &boxes, bool gesture = false);
    Q_INVOKABLE void setItemCrop(const QString &id, int left, int top, int right, int bottom, bool gesture = false);
    // Crop and move together, one step: cropping an unbounded item from a
    // handle keeps the picture where it is while the outline shrinks.
    Q_INVOKABLE void setItemCropAt(const QString &id, int left, int top, int right, int bottom, qreal x, qreal y,
                                   bool gesture = false);
    Q_INVOKABLE void setItemFlip(const QString &id, bool horizontal, bool vertical);
    Q_INVOKABLE void setItemBounds(const QString &id, const QString &bounds);
    Q_INVOKABLE void rotateItem(const QString &id, qreal degrees);
    Q_INVOKABLE void editTransform(const QString &id);
    Q_INVOKABLE void setItemSize(const QString &id, qreal width, qreal height);
    Q_INVOKABLE void setItemPosition(const QString &id, qreal x, qreal y);
    Q_INVOKABLE void setItemAlignment(const QString &id, const QString &kind, int alignment);
    Q_INVOKABLE void setItemClipToBounds(const QString &id, bool clip);
    Q_INVOKABLE void setItemScaleFilter(const QString &id, const QString &filter);
    Q_INVOKABLE void setItemOpacity(const QString &id, qreal opacity, bool gesture = false);
    // "normal", "multiply", "screen", "overlay", "darken", "lighten", "add", "subtract", "difference"
    Q_INVOKABLE void setItemBlend(const QString &id, const QString &blend);
    Q_INVOKABLE void setItemBlendSpace(const QString &id, const QString &space);
    Q_INVOKABLE void fitItem(const QString &id);       // fit inside the canvas, centred
    Q_INVOKABLE void stretchItem(const QString &id);   // fill the canvas exactly
    Q_INVOKABLE void centreItem(const QString &id);
    Q_INVOKABLE void resetItem(const QString &id);     // native size, no rotation, crop or flip
    Q_INVOKABLE void endGesture() { m_production.endGesture(); }
    Q_INVOKABLE void undo() { m_production.undo(); }
    Q_INVOKABLE void redo() { m_production.redo(); }
    Q_INVOKABLE void toggleRecording();
    // Starts one, saying why not in the status line; returns why not, for a script.
    QString startRecording();
    Q_INVOKABLE void toggleStreaming();
    Q_INVOKABLE void toggleReplay();
    Q_INVOKABLE void toggleVirtualCamera();
    Q_INVOKABLE void setCameraShows(const QString &shows, const QString &target);
    // Asks for files to copy into MP4, or takes these paths.
    Q_INVOKABLE void chooseRemuxFiles();
    Q_INVOKABLE void remuxFiles(const QStringList &paths);
    Q_INVOKABLE void cancelRemux(int id);
    Q_INVOKABLE void clearRemux();
    Q_INVOKABLE void saveReplay();
    Q_INVOKABLE void setReplayOption(const QString &key, const QVariant &value);
    // Whether a folding panel section is open, remembered app-wide by key.
    Q_INVOKABLE bool panelOpen(const QString &key, bool fallback) const {
        return preference(QStringLiteral("panels"), QVariantMap{}).toMap().value(key, fallback).toBool();
    }
    Q_INVOKABLE void setPanelOpen(const QString &key, bool open) {
        QVariantMap panels = preference(QStringLiteral("panels"), QVariantMap{}).toMap();
        panels.insert(key, open);
        setPreference(QStringLiteral("panels"), panels);
    }
    Q_INVOKABLE void setRecordingOption(const QString &key, const QVariant &value);
    Q_INVOKABLE bool setRecordingPaused(bool paused);
    Q_INVOKABLE void splitRecording();
    Q_INVOKABLE void addChapter(const QString &name = {});
    // Saves a stream target ({id, name, service, server, enabled, encoder,
    // bitrateKbps, …}); with keyGiven the key goes to the keyring. Returns its
    // id, or "" after failed().
    Q_INVOKABLE QString saveStreamTarget(const QVariantMap &target, const QString &key, bool keyGiven);
    Q_INVOKABLE void removeStreamTarget(const QString &id);
    Q_INVOKABLE bool streamTargetHasKey(const QString &id);
    // [{name, label}] encoders for a target as it stands in the editor.
    Q_INVOKABLE QVariantList streamEncoderChoices(const QVariantMap &target) const;
    // What the target's service would refuse or warn about, for people.
    Q_INVOKABLE QStringList streamWarnings(const QVariantMap &target) const;
    // Why lowering the bitrate would do nothing for this stream; "" when it works.
    Q_INVOKABLE QString dynamicBitrateNote(const QVariantMap &target) const;
    // What keeping to the service changes in this draft, for people.
    Q_INVOKABLE QStringList streamAdjustments(const QVariantMap &target) const;
    // About FFmpeg's container of this name: {label, extension, chapters,
    // problem}; problem is "" when it can be written.
    Q_INVOKABLE QVariantMap muxerInfo(const QString &muxer) const;
    Q_INVOKABLE void stopRecording() { m_session.stopRecording(false); }
    Q_INVOKABLE void setPreviewEnabled(bool on);
    Q_INVOKABLE void setPreviewSize(int width, int height);
    Q_INVOKABLE void open(const QUrl &url);
    void importCollectionFrom(const QString &path);
    void addImageFile(const QString &path);
    void addMediaFile(const QString &path);
    // Opens the recordings folder in the file manager.
    Q_INVOKABLE void showRecordings();
    Q_INVOKABLE void copyText(const QString &text);

    // Tools (backendtools.cpp).
    QVariantMap systemStats() const { return m_systemStatsMap; }
    bool statsWatched() const { return m_systemStatsTimer.isActive(); }
    void setStatsWatched(bool on);
    Q_INVOKABLE void refreshSystemStats();
    // The log: this run's lines or the run before's, at least `minimumLevel`
    // (AppLog::Level) and containing `search`: [{time, level, text}].
    Q_INVOKABLE QVariantList logLines(bool previous, int minimumLevel, const QString &search);
    Q_INVOKABLE double logSerial() const { return AppLog::instance() ? double(AppLog::instance()->serial()) : 0; }
    Q_INVOKABLE bool hasPreviousLog();
    Q_INVOKABLE void copyLog(bool previous, int minimumLevel, const QString &search);
    // Asks where, then writes the whole run's log there.
    Q_INVOKABLE void saveLog(bool previous);
    bool saveLogTo(bool previous, const QString &path);
    bool crashOffered() const { return m_crashOffered; }
    QVariantList setupScenarios() const;
    bool setupOffered() const { return m_setupOffered && !m_crashOffered && !safeMode(); }
    bool setupBusy() const { return m_setupBusy; }
    QString setupStep() const { return m_setupStep; }
    double setupProgress() const { return m_setupProgress; }
    QVariantMap setupResult() const { return m_setupResult; }
    // [{index, label}] for the set-up's display choice.
    Q_INVOKABLE QVariantList setupDisplays() const;
    // {canTest, why}: whether the upload can be measured for this service
    // without going live.
    Q_INVOKABLE QVariantMap setupUploadTest(const QString &service) const;
    // {scenario, display, camera, cameraName, microphone, service, server,
    //  key, fastMotion, uploadKbps}: surveys, measures and plans, then
    // setupResult says what it would set.
    Q_INVOKABLE void startSetupCheck(const QVariantMap &choices);
    Q_INVOKABLE void cancelSetupCheck();
    // Makes the planned profile (named so) and its studio collection, and
    // switches to both.
    Q_INVOKABLE bool applySetup(const QString &name);
    Q_INVOKABLE void dismissSetupOffer();
    bool safeMode() const { return m_session.safeMode(); }
    bool started() const { return m_session.running(); }
    // Before start(): the last run ended without quitting, so offer its log
    // and safe mode, and start once chosen.
    void setPreviousRunCrashed(bool crashed) { m_crashOffered = crashed; emit startStateChanged(); }
    // Before start().
    void setSafeMode(bool on) { m_session.setSafeMode(on); emit startStateChanged(); }
    Q_INVOKABLE void startAfterCrash(bool safe);
    // On quitting, start again without safe mode (main starts the new copy).
    Q_INVOKABLE void restartNormally(bool restart = true) { m_restartRequested = restart; }
    bool restartRequested() const { return m_restartRequested; }
    // Help → Repair. beginRepair() starts a new dated folder for what is set
    // aside; repairCheck() is {problems: [{kind, id, path, why}], collections,
    // profiles, staleGrants, folder}. Each action puts what it changes in that
    // folder first, and says how it went in the status line.
    Q_INVOKABLE void beginRepair();
    Q_INVOKABLE QVariantMap repairCheck() const;
    Q_INVOKABLE bool setAsideUnreadable();
    Q_INVOKABLE bool resetWindowPreferences();
    Q_INVOKABLE bool clearStaleGrants();
    // As the app reads profiles (backendprofiles.cpp).
    static bool profileFileReadable(const QString &path);

signals:
    void omarchyBarChanged();
    // Pressed on the bar icon: the window acts on it as on its tray menu.
    void barCommand(const QString &name);
    void prompterChanged();
    void playbackChanged();
    void prompterRestarted();
    void systemStatsChanged();
    void startStateChanged();
    void setupChanged();
    void mediaPlaybackChanged();
    void slideshowPlaybackChanged();
    void slideshowEditRequested(const QString &sourceId);
    void videoFiltersEditRequested(const QString &sourceId);
    void videoFilterClipboardChanged();
    void profilesChanged();
    void audioHotkeysChanged();
    void audioPushChanged();
    void monitoringChanged();
    void placementCopied();
    void sourceClipboardChanged();
    void transformEditRequested(const QString &itemId);
    void preferencesChanged();
    void directCaptureRequested(const QString &sourceId, bool window);
    void collectionsChanged();
    void scenesChanged();
    void itemsChanged();
    void selectionChanged();
    void historyChanged();
    void videoDevicesChanged();
    void recordingChanged();
    void streamingChanged();
    void replayChanged();
    void virtualCameraChanged();
    void remuxChanged();
    void streamTargetsChanged();
    void statusChanged();
    void statsChanged();
    void settingsChanged();
    void frameRateNoteChanged();
    void microphonesChanged();
    void noSoundChanged();
    void levelsChanged();
    void globalAudioDevicesChanged();
    void previewReady(const QImage &image);
    void programReady(const QImage &image);
    void viewReady(const QString &id, const QImage &image);
    void projectorsChanged();
    void screensChanged();
    void screenshotSaved(const QString &path);
    void transitionStateChanged();
    void hotkeysChanged();
    void switcherChanged();
    void focusedWindowChanged();
    void timersChanged();
    void scriptsChanged();
    void scriptLogChanged();
    void remoteControlChanged();
    void captureRegionPreview(const QString &sourceId, const QImage &image, int width, int height);
    void interactRequested(const QString &sourceId);
    void browserFrame(const QString &sourceId, const QImage &image);
    void failed(const QString &message);
    // Record was pressed and no recording started: why, in words.
    void recordingRefused(const QString &message);

private:
    void refreshHotkeys();
    void setStatus(const QString &status);
    void refreshStats();
    bool saveProfile();
    void applyFrameRate(int num, int den, const QString &match);
    bool outputsBusy() const;
    void restartSession();
    void restartWhenIdle(const QString &done, const QString &waiting);
    void followPowerAndPendingRestart();
    void loadActiveProfile();
    QVariant preference(const QString &key, const QVariant &fallback) const { return m_preferences.value(key, fallback); }
    void setPreference(const QString &key, const QVariant &value);
    bool profileChangeAllowed();
    bool activateProfile(const QString &id, Profile profile);
    bool createProfile(Profile profile, const QString &name);
    bool profileError(const QString &message);
    QString uniqueProfileName(const QString &name) const;
    void loadCollection();
    // The open collection's layout at the size of the canvas in use.
    void fitLayoutToCanvas();
    QString collectionFile(const QString &base) const;
    bool openCollection(const QString &base);
    // Writes it under a free file name and opens it.
    bool createCollection(const Collection &collection);
    // What the keyring calls a stream's key.
    static QString streamKeyLabel(const StreamTarget &target);
    // The transform of an item in the live scene, and its source's pixel size
    // (the canvas when the source has none yet, so a new capture can be placed).
    bool itemState(const QString &id, Transform *transform, int *sourceWidth, int *sourceHeight) const;
    // A source's pixel size: a group's box, the canvas for a studio, the feed's
    // pictures otherwise (the canvas until it has any).
    QSize sourceSize(const QString &sourceId) const;
    // Where items in a group sit on the canvas: one step per enclosing group,
    // innermost first. Empty for the live studio's own items.
    QList<GroupStep> groupSteps(const QString &itemId) const;
    Outline canvasOutline(const QString &itemId) const;
    // Locked itself or inside a locked group: its placement can't change.
    bool placementLocked(const QString &itemId) const;
    void changeTransform(const QString &id, bool gesture, const std::function<void(Transform &, int, int)> &change);
    // Places an item by its outline as it shows on the canvas; level turns it
    // upright on the canvas first.
    void placeOnCanvas(const QString &id, bool level, const std::function<Outline(const Outline &)> &target);
    Transform canvasFit() const;
    // Where an export goes: ".json" added if it was not typed. Empty (and said so)
    // when that makes a name the picker never asked about replacing.
    QString exportFile(const QString &picked);
    bool copySecret(const QString &from, const QString &to, const QString &label, QString *why);
    bool projectorShown(const QVariantMap &projector) const;
    void dropGoneProjectors();
    bool canTake() const;
    QString nextName(const QString &base) const;
    void openImagePicker();
    QVariantList deviceModes(const SourceDef *source) const;
    QVariantList deviceControlList(const QString &sourceId) const;

    // Before the session, so it is destroyed after it: the session keeps a
    // pointer to it.
    Production m_production;
    Session m_session;
    SecretStore m_secrets;
    QElapsedTimer m_streamClock;
    QString m_streamingTime;
    bool m_cameraInUse = false;
    QString m_profileId = QStringLiteral("default");
    Hotkeys m_audioHotkeys;
    QVariantMap m_preferences;
    HyprlandRules m_hyprland;
    RegionDrawer m_regionDrawer;
    quint64 m_regionGeneration = 0;  // the collection a region is being drawn for
    ControlSocket m_control;
    OmarchyBar m_omarchyBar{this};
    FocusWatcher m_focus;
    ScriptHost m_scripts;
    QString m_scriptStudio, m_scriptProgram;  // what scripts were last told
    QMap<QString, QVariantMap> m_scriptForms;  // as each running script declared it
    QVariantMap scriptValues(const QString &path) const;
    void sendScriptEvents();
    QString m_focusedClass, m_focusedTitle;  // the last window reported
    void syncSwitcher();
    QTimer m_outputTimer;    // counts the output timer's seconds
    int m_recordElapsed = -1, m_streamElapsed = -1;  // -1: not counting
    void tickOutputTimer();
    QJsonObject controlRequest(const QJsonObject &request);
    bool m_localHotkeysEnabled = false;
    PortalFileChooser m_imagePicker;
    PortalFileChooser m_mediaPicker;
    // Folders for slideshows, text files for text: what to do with the answer.
    PortalFileChooser m_otherPicker;
    bool m_prompterOpen = false, m_prompterPlaying = false, m_prompterHidden = false;
    QString m_prompterText, m_prompterProblem;
    PortalFileChooser m_remuxPicker;
    PortalFileChooser m_slideshowPicker;
    std::function<void(const QList<QUrl> &)> m_slideshowChosen;
    QString m_slideshowTarget;
    quint64 m_slideshowGeneration = 0;
    Transform m_placement;
    bool m_hasPlacement = false;
    std::function<void(const QString &path)> m_otherChosen;
    static QString omashowProgram();
    void loadPresentation(const QString &deck, const QString &sourceId);
    QProcess *m_presentationJob = nullptr;  // one at a time
    QString m_presentationLoading;          // the folder that one is drawing into, by its name
    RecordingPlayer m_player;
    bool m_playbackOpen = false;
    QString m_lastRecording;
    QVariantList m_videoFilterClipboard;
    QString m_mediaTarget;   // the media source being replaced; empty adds a new one
    QString m_swapTo;        // studio mode: the studio to preview once a take settles
    quint64 m_swapGeneration = 0;  // the collection that studio is in
    void swapAfter(const QString &onAir);
    void settleTake();
    void transitionEnded();
    std::map<QString, Session::View> m_views;
    QTimer m_viewsChanged;   // gathers view requests made together
    QMap<QString, QString> m_screenshots; // snapshot id → what it shows
    struct BackdropAsk { QString sourceId, filterId; double x = -1, y = -1; quint64 generation = 0; };
    QMap<QString, BackdropAsk> m_backdropAsks;  // snapshot id → the key it is for
    void keyBackdropFound(const QString &snapshotId, const QImage &picture);
    int m_backdropSerial = 0;
    QVariantList m_projectorsSeen;
    int m_screenshotSerial = 0;
    QString projectorLabel(const QVariantMap &projector) const;
    void applyProjectorRules();
    QString m_imageTarget;   // the image source being replaced; empty adds a new one
    quint64 m_pickGeneration = 0;  // the collection a target above is in
    QTimer m_statsTimer;
    QTimer m_levelTimer;
    QString m_status;
    mutable QVariantList m_lastMissing;  // missingFiles when a path does not answer
    bool m_recordingFailed = false;
    QString m_settingMergeKey;  // set: every source setting changed meanwhile is one undo step
    quint64 m_remoteEditSerial = 0;  // the recording finishing now failed: not "Saved"
    QString m_statsLine;
    QString m_engineName, m_engineWhy;
    QVariantMap m_selectedEvenRate;  // what the selected camera's settings last said about its rate
    std::deque<std::pair<qint64, uint64_t>> m_lateHistory;  // (ms, late frames so far), the last five seconds
    bool framesLateLately() const { return m_lateHistory.size() > 1 && m_lateHistory.back().second > m_lateHistory.front().second; }
    bool m_engineOnBattery = false;
    QString m_pendingRestart;      // the status once a restart that waits for the outputs is done
    QElapsedTimer m_powerChecked;
    mutable QVariantList m_gpuChoices;  // listed once: the GPUs do not change while running
    QString m_recordingTime;
    qreal m_desktopLevel = 0;
    qreal m_microphoneLevel = 0;
    QVariantMap m_sourceAudioLevels;
    QVariantMap m_desktopMeter, m_microphoneMeter, m_sourceAudioMeters, m_globalAudioDeviceMeters;
    QElapsedTimer m_meterElapsed;
    // Tools (backendtools.cpp).
    void setUpTools();
    SystemStats m_systemStats;
    QTimer m_systemStatsTimer;
    QVariantMap m_systemStatsMap;
    QList<AppLog::Line> logSelection(bool previous, int minimumLevel, const QString &search);
    QList<AppLog::Line> m_previousLog;
    bool m_previousLogRead = false;
    bool m_crashOffered = false;
    bool m_started = false;          // start() has run: there are studios to act on
    QUrl m_openAfterCrash;           // a file named before then (the launch card or the crash sheet is up)
    // The set-up wizard's state (backendsetup.cpp).
    bool m_setupOffered = false, m_setupBusy = false;
    QString m_setupStep;
    double m_setupProgress = 0;
    QVariantMap m_setupResult;
    QString m_setupResultNote;  // a measurement that did not work, said first in the summary
    presets::Survey m_setupSurvey;
    presets::Choices m_setupChoices;
    presets::Plan m_setupPlan;
    quint64 m_setupRun = 0;   // bumped by each start and cancel: late answers from an old run are dropped
    void setSetupStep(const QString &step, double progress);
    void setupMeasureUpload();
    void setupMeasureSpeeds();
    void setupFinish(bool cancelled = false);
    class BandwidthTest *m_setupUpload = nullptr;   // made when first needed
    class EncoderBench *m_setupBench = nullptr;
    bool m_restartRequested = false;
    QDateTime m_repairWhen;
    QString repairFolder() const;
    QStringList staleGrantKeys() const;
    // Remote control (backendremote.cpp).
    struct RemoteInput {
        QString id, name, kind;
        bool audio = false;
    };
    // What clients were last told about, to tell them what changed.
    struct RemoteSeen {
        QString collectionFile, collection, profile;
        QStringList collections, profiles;
        QList<QPair<QString, QString>> scenes;  // studios (id, name), as listed
        QMap<QString, QVariantMap> inputs, items, filters;
        QMap<QString, QString> media;           // media source → its state
        QString program, preview, transition, transitionTarget, recordingPath;
        int transitionMs = 0;
        bool studioMode = false, transitioning = false, recording = false, paused = false;
        bool streaming = false, replay = false, camera = false;
    };
    struct RemoteShot {
        RemoteControl::Done done;
        QByteArray format;
        int width = 0, height = 0, quality = -1;
    };
    void setUpRemote();
    void applyRemote();
    RemoteControl::Settings remoteSettings() const;
    void remoteRequest(const QString &type, const QJsonObject &data, const RemoteControl::Done &done);
    // Goes live, once the keyring has given what the targets keep in it.
    // `settled` runs when that has succeeded or failed.
    void goLive(const std::function<void()> &settled);
    void startStreams(const QMap<QString, QString> &keys, const QMap<QString, QString> &passwords);
    void streamsInKeyring(QStringList *keys, QStringList *passwords) const;
    bool m_askingKeyring = false;
    void remoteScreenshot(const QString &id, const QImage &image);
    void emitRemoteEvent(const QString &type, int intent, const QJsonObject &data);
    RemoteSeen remoteSnapshot();
    void remoteDiff();
    void remoteMeters();
    QList<RemoteInput> remoteInputs() const;
    std::optional<RemoteInput> remoteInput(const QJsonObject &data, RemoteControl::Reply *why) const;
    const Scene *remoteScene(const QJsonObject &data, RemoteControl::Reply *why, bool groups = false) const;
    QString remoteGroupName(const Scene &group) const;
    bool remoteSource(const QJsonObject &data, QString *shows, QString *target, RemoteControl::Reply *why) const;
    int remoteItemNumber(const QString &itemId);
    const SceneItem *remoteItem(const Scene &scene, int number, int *index);
    QJsonObject remoteItemJson(const Scene &scene, int index);
    QJsonObject remoteTransform(const SceneItem &item) const;
    bool remoteMuted(const RemoteInput &input) const;
    double remoteVolume(const RemoteInput &input) const;
    void setRemoteAudio(const RemoteInput &input, const QString &key, const QVariant &value);
    RemoteControl m_remote;
    QTimer m_remoteDiff;     // gathers changes made together into one round of events
    RemoteSeen m_remoteSeen;
    QHash<QString, int> m_remoteItemNumbers;  // items whose id is not "i" and a number
    QHash<QString, RemoteShot> m_remoteShots;
    QString m_remoteError;
    QElapsedTimer m_remoteUptime;
    // The last names read from the collection and profile files, and whether
    // those are out of date.
    QStringList m_remoteCollectionNames, m_remoteProfileNames;
    bool m_remoteNamesStale = true;
    // Last, so it goes first: an answer from the keyring then reaches nothing
    // that has gone, and a prompt being waited for is let go before the session ends.
    FileWork m_keyringWork{this};
};
