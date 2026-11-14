#pragma once

#include <QJsonObject>
#include <QVariantMap>
#include <QMap>
#include <QString>
#include <QVector>
#include <string>
#include "audio/format.h"
#include "engine/frame.h"
#include "scene.h"

// Everything that persists: the profile (output and audio settings) and the
// sources. JSON under $XDG_CONFIG_HOME/omabroadcast. No machine-specific
// values are baked in anywhere; every default is derived at runtime.
struct SourceEntry {
    QString name;
    uint types = 1;          // ScreenCastPortal source types bit set
    bool cursor = true;
    QString restoreToken;    // lets the portal re-open it without a prompt
};

// How an output is encoded beyond its encoder, quality and bitrate: the
// knobs the advanced settings show. Defaults are what the simple mode uses.
struct EncodingChoice {
    QString rateControl = QStringLiteral("quality"); // quality, cbr, vbr, cqp, lossless
    int maxKbps = 0;         // VBR peak; 0 = 1.5 × the bitrate
    int bufferKbps = 0;      // 0 = one second of the peak
    int keyframeSeconds = 2;
    int bFrames = 0;
    int lookahead = 0;       // frames
    QString splitEncode;     // the encoder's split_encode_mode value; empty = its choice
    QString preset, profile, tune;  // the encoder's own names; empty = ours
    QString options;         // "key=value …", passed to the encoder as typed
    QString audioCodec = QStringLiteral("aac");  // aac, opus, flac, alac, pcm16, pcm24, pcm32f

    QJsonObject toJson() const;
    // Reads what is there and keeps the rest; `bitrateKbps` is the old
    // profiles' only rate-control switch.
    void read(const QJsonObject &json, int bitrateKbps);
    bool operator==(const EncodingChoice &) const = default;
};

// Where a stream goes and how it is sent. Its key lives in the keyring under
// `id`; only with no keyring does it stay here, and Settings says so.
struct StreamTarget {
    QString id;                  // stable: the keyring finds the key by it
    QString name;                // for people; the service's name when empty
    QString service = QStringLiteral("custom");  // a streamServices() id
    QString server;              // URL; may hold {key}
    QString key;                 // only when no keyring was there to hold it
    QString username;            // a custom server's sign-in; its password is
    bool hasPassword = false;    // in the keyring beside the key
    QString password;            // only when no keyring was there to hold it
    bool enforce = true;         // keep to the service's published limits
    bool enabled = true;         // goes live with Start streaming
    QString encoder;             // encoderKey; empty = best for streaming
    int bitrateKbps = 6000;
    int audioBitrateKbps = 160;
    int track = 0;               // which mixer track it sends
    EncodingChoice choice = [] { EncodingChoice c; c.rateControl = QStringLiteral("cbr"); return c; }();
    int delaySeconds = 0;
    bool preserveDelay = false;  // keep the delay's pictures through a reconnect
    bool reconnect = true;
    int retryDelaySeconds = 2;
    int maxRetries = 20;         // 0 = for ever
    bool dynamicBitrate = false;
    QString bindAddress;         // send from this local address; empty = any
    int ipFamily = 0;            // 4 or 6 to use only that; 0 = either
    bool lowLatency = false;     // send each packet at once (RTMP)
    // Any FFmpeg container to any address FFmpeg writes to (udp://, tcp://,
    // a file…); empty = the one the address implies.
    QString muxer;
    QString muxerOptions;        // "key=value …", the container's own
    int outputHeight = 0;        // sent at this height, the canvas's shape; 0 = the canvas

    QJsonObject toJson() const;
    static StreamTarget fromJson(const QJsonObject &json);
};

struct Profile {
    QString name = QStringLiteral("Default");
    // Video
    int canvasWidth = 0;     // 0 = follow the first live display's pixel size
    int canvasHeight = 0;
    int fpsNum = 60;
    int fpsDen = 1;
    // Colour of every recording and stream: a format ("auto" lets each
    // encoder use its usual one, 10-bit for HDR), a space, the range and the
    // HDR levels. See OutputColour in engine/frame.h.
    QString colourFormat = QStringLiteral("auto");  // auto, nv12, i420, i444, p010, i010, p216, p416, bgra, i210
    QString colourSpace = QStringLiteral("709");    // srgb, 601, 709, pq, hlg
    bool colourFullRange = false;
    int sdrWhite = 203;      // nits
    int hdrPeak = 1000;      // nits
    // How the canvas is scaled to a smaller (or larger) recording or stream:
    // area, bilinear, bicubic or lanczos.
    QString downscaleFilter = QStringLiteral("area");
    // The chosen space, range and levels in `format`.
    engine::OutputColour outputColour(engine::ColourFormat format) const;
    // The chosen format; false for "auto".
    bool fixedColourFormat(engine::ColourFormat *format) const;

    // Recording
    QString folder;          // empty = $XDG_VIDEOS_DIR/OmaBroadcast
    QString filenameTemplate = QStringLiteral("%Y-%m-%d %H-%M-%S");
    bool filenameNoSpaces = false;
    QString container = QStringLiteral("mkv");  // a fileFormats() id, or "custom"
    QString customMuxer;     // with "custom": FFmpeg's name for the container
    QString containerOptions; // "key=value …", the container's own options
    QString encoder;         // FFmpeg encoder name; empty = best available
    int quality = 23;
    int bitrateKbps = 0;
    int audioBitrateKbps = 160;
    unsigned recordingTracks = 1;
    int recordingHeight = 0; // recorded at this height, the canvas's shape; 0 = the canvas
    EncodingChoice recordingChoice;
    int splitMinutes = 0;    // a new file every so often; 0 = never
    int splitMegabytes = 0;  // or every so big
    bool remuxToMp4 = false; // an MP4 copy beside each finished file

    // Replay buffer: the last stretch kept in memory, saved on demand into the
    // recording folder and format, named prefix + filename pattern + suffix.
    int replaySeconds = 20;      // at least this long; 0 = no time limit
    int replayMegabytes = 512;   // no more than this; 0 = no size limit
    QString replayPrefix = QStringLiteral("Replay ");
    QString replaySuffix;

    // Streaming
    QVector<StreamTarget> streams;
    bool recordWhileStreaming = false;   // start recording with the stream
    bool stopRecordingWithStream = false;
    bool replayWhileStreaming = false;   // a replay buffer from go-live to the end

    // Virtual camera: what it shows. "program", or "studio" / "source" with
    // the id of one in the open collection; one that is gone shows the program.
    QString cameraShows = QStringLiteral("program");
    QString cameraTarget;

    // Audio
    AudioFormat audioFormat;
    QString monitorOutput = QStringLiteral("none"); // empty follows default; otherwise node.name
    bool activeAudioOnly = false;
    bool lowLatencyAudio = false;
    bool globalHotkeys = false;  // the GlobalShortcuts portal was asked to bind them
    QVariantMap meter;
    QVector<SourceDef> globalAudioDevices; // additional input/output captures, active in every studio
    const SourceDef *globalAudioDevice(const QString &id) const;
    QVariantMap desktopAudio, microphoneAudio; // mono, balance, syncMs
    float desktopGain = 1.f;
    float microphoneGain = 1.f;
    bool desktopMuted = false;
    bool microphoneMuted = false;
    // In the mixer because someone added them; what is not added is not captured.
    bool desktopAdded = false;
    bool microphoneAdded = false;
    QString microphone;      // "" automatic, "none" off, else the input's node.name

    // Portal restore tokens by source id: machine state, not content, so they
    // live here and never in the scene collection or its undo history.
    QVariantMap restoreTokens;
    // Keys for actions that take no argument: "action" → key ("Ctrl+F1").
    QVariantMap hotkeys;
    // The scene collection open: the base name of its file in scenes/.
    QString collection = QStringLiteral("Default");
    // Sources as version 2 kept them, read once to move into a scene collection.
    QVector<SourceEntry> legacySources;

    QJsonObject toJson() const;
    static Profile fromJson(const QJsonObject &json);

    // $XDG_CONFIG_HOME/omabroadcast/profiles/<name>/profile.json
    static QString pathFor(const QString &name);
    static Profile load(const QString &name);
    bool save(const QString &name) const;

    static QString defaultFolder();
    QString resolvedFolder() const;
    // The FileSink format the recording is written in.
    std::string recordingFormat() const {
        return container == QLatin1String("custom") ? "custom:" + customMuxer.toStdString() : container.toStdString();
    }
};

// A recording's file name, without folder or extension: {name} fields (studio,
// profile, collection, width, height, fps; unknown ones stay as typed) and the
// date codes %Y %y %m %d %H %M %S %%. Characters other systems refuse become
// "-"; noSpaces turns spaces into "_". Never empty, never hidden.
QString expandFileName(const QString &pattern, qint64 unixSeconds, const QMap<QString, QString> &fields, bool noSpaces);
