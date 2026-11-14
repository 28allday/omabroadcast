#include "profile.h"
#include "audio_settings.h"
#include "meter_settings.h"
#include "output/recorder.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSet>
#include <QUuid>

const SourceDef *Profile::globalAudioDevice(const QString &id) const {
    for (const auto &device : globalAudioDevices) if (device.id == id) return &device;
    return nullptr;
}

QJsonObject EncodingChoice::toJson() const {
    return QJsonObject{{QStringLiteral("rateControl"), rateControl}, {QStringLiteral("maxKbps"), maxKbps},
                       {QStringLiteral("bufferKbps"), bufferKbps}, {QStringLiteral("keyframeSeconds"), keyframeSeconds},
                       {QStringLiteral("bFrames"), bFrames}, {QStringLiteral("lookahead"), lookahead},
                       {QStringLiteral("preset"), preset}, {QStringLiteral("profile"), profile},
                       {QStringLiteral("tune"), tune}, {QStringLiteral("options"), options},
                       {QStringLiteral("audioCodec"), audioCodec}, {QStringLiteral("splitEncode"), splitEncode}};
}

void EncodingChoice::read(const QJsonObject &json, int bitrateKbps) {
    static const QStringList modes{QStringLiteral("quality"), QStringLiteral("cbr"), QStringLiteral("vbr"),
                                   QStringLiteral("cqp"), QStringLiteral("lossless")};
    static const QStringList codecs{QStringLiteral("aac"), QStringLiteral("opus"), QStringLiteral("flac"), QStringLiteral("alac"),
                                    QStringLiteral("pcm16"), QStringLiteral("pcm24"), QStringLiteral("pcm32f")};
    rateControl = json.value(QStringLiteral("rateControl")).toString(bitrateKbps > 0 ? QStringLiteral("cbr") : rateControl);
    if (!modes.contains(rateControl)) rateControl = QStringLiteral("quality");
    maxKbps = std::max(0, json.value(QStringLiteral("maxKbps")).toInt(maxKbps));
    bufferKbps = std::max(0, json.value(QStringLiteral("bufferKbps")).toInt(bufferKbps));
    keyframeSeconds = std::clamp(json.value(QStringLiteral("keyframeSeconds")).toInt(keyframeSeconds), 0, 20);
    bFrames = std::clamp(json.value(QStringLiteral("bFrames")).toInt(bFrames), 0, 4);
    lookahead = std::clamp(json.value(QStringLiteral("lookahead")).toInt(lookahead), 0, 60);
    preset = json.value(QStringLiteral("preset")).toString(preset);
    profile = json.value(QStringLiteral("profile")).toString(profile);
    tune = json.value(QStringLiteral("tune")).toString(tune);
    options = json.value(QStringLiteral("options")).toString(options);
    splitEncode = json.value(QStringLiteral("splitEncode")).toString(splitEncode);
    audioCodec = json.value(QStringLiteral("audioCodec")).toString(audioCodec);
    if (!codecs.contains(audioCodec)) audioCodec = QStringLiteral("aac");
}

QJsonObject StreamTarget::toJson() const {
    QJsonObject o{{QStringLiteral("id"), id}, {QStringLiteral("name"), name}, {QStringLiteral("service"), service},
                  {QStringLiteral("server"), server}, {QStringLiteral("enabled"), enabled}, {QStringLiteral("encoder"), encoder},
                  {QStringLiteral("bitrateKbps"), bitrateKbps}, {QStringLiteral("audioBitrateKbps"), audioBitrateKbps},
                  {QStringLiteral("track"), track}, {QStringLiteral("encoding"), choice.toJson()},
                  {QStringLiteral("delaySeconds"), delaySeconds}, {QStringLiteral("reconnect"), reconnect},
                  {QStringLiteral("retryDelaySeconds"), retryDelaySeconds}, {QStringLiteral("maxRetries"), maxRetries},
                  {QStringLiteral("dynamicBitrate"), dynamicBitrate}, {QStringLiteral("bindAddress"), bindAddress},
                  {QStringLiteral("outputHeight"), outputHeight}, {QStringLiteral("preserveDelay"), preserveDelay},
                  {QStringLiteral("ipFamily"), ipFamily}, {QStringLiteral("lowLatency"), lowLatency},
                  {QStringLiteral("username"), username}, {QStringLiteral("hasPassword"), hasPassword},
                  {QStringLiteral("enforce"), enforce}};
    if (!muxer.isEmpty()) o.insert(QStringLiteral("muxer"), muxer);
    if (!muxerOptions.isEmpty()) o.insert(QStringLiteral("muxerOptions"), muxerOptions);
    if (!key.isEmpty()) o.insert(QStringLiteral("key"), key);
    if (!password.isEmpty()) o.insert(QStringLiteral("password"), password);
    return o;
}

StreamTarget StreamTarget::fromJson(const QJsonObject &json) {
    StreamTarget t;
    t.id = json.value(QStringLiteral("id")).toString();
    if (t.id.isEmpty()) t.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    t.name = json.value(QStringLiteral("name")).toString().trimmed();
    t.service = json.value(QStringLiteral("service")).toString(t.service);
    t.server = json.value(QStringLiteral("server")).toString().trimmed();
    t.key = json.value(QStringLiteral("key")).toString();
    t.enabled = json.value(QStringLiteral("enabled")).toBool(true);
    t.encoder = json.value(QStringLiteral("encoder")).toString();
    t.bitrateKbps = std::clamp(json.value(QStringLiteral("bitrateKbps")).toInt(t.bitrateKbps), 100, 200000);
    t.audioBitrateKbps = std::clamp(json.value(QStringLiteral("audioBitrateKbps")).toInt(t.audioBitrateKbps), 32, 512);
    t.track = std::clamp(json.value(QStringLiteral("track")).toInt(0), 0, 5);
    t.choice.read(json.value(QStringLiteral("encoding")).toObject(), 0);
    if (!json.value(QStringLiteral("encoding")).toObject().contains(QStringLiteral("rateControl"))) t.choice.rateControl = QStringLiteral("cbr");
    t.delaySeconds = std::clamp(json.value(QStringLiteral("delaySeconds")).toInt(0), 0, 3600);
    t.outputHeight = std::clamp(json.value(QStringLiteral("outputHeight")).toInt(0), 0, 4320) & ~1;
    t.reconnect = json.value(QStringLiteral("reconnect")).toBool(true);
    t.retryDelaySeconds = std::clamp(json.value(QStringLiteral("retryDelaySeconds")).toInt(2), 1, 60);
    t.maxRetries = std::clamp(json.value(QStringLiteral("maxRetries")).toInt(20), 0, 10000);
    t.dynamicBitrate = json.value(QStringLiteral("dynamicBitrate")).toBool();
    t.bindAddress = json.value(QStringLiteral("bindAddress")).toString().trimmed();
    t.preserveDelay = json.value(QStringLiteral("preserveDelay")).toBool();
    t.ipFamily = json.value(QStringLiteral("ipFamily")).toInt();
    if (t.ipFamily != 4 && t.ipFamily != 6) t.ipFamily = 0;
    t.lowLatency = json.value(QStringLiteral("lowLatency")).toBool();
    t.username = json.value(QStringLiteral("username")).toString();
    t.password = json.value(QStringLiteral("password")).toString();
    t.hasPassword = json.value(QStringLiteral("hasPassword")).toBool() || !t.password.isEmpty();
    t.enforce = json.value(QStringLiteral("enforce")).toBool(true);
    t.muxer = json.value(QStringLiteral("muxer")).toString().trimmed();
    t.muxerOptions = json.value(QStringLiteral("muxerOptions")).toString().trimmed();
    return t;
}

QJsonObject Profile::toJson() const {
    QJsonArray devices;
    for (const auto &device : globalAudioDevices)
        devices.append(QJsonObject{{"id", device.id}, {"kind", device.kind}, {"name", device.name},
                                   {"settings", QJsonObject::fromVariantMap(device.settings)}});
    QJsonObject video{{QStringLiteral("width"), canvasWidth},
                      {QStringLiteral("height"), canvasHeight},
                      {QStringLiteral("fpsNum"), fpsNum},
                      {QStringLiteral("fpsDen"), fpsDen},
                      {QStringLiteral("colourFormat"), colourFormat},
                      {QStringLiteral("colourSpace"), colourSpace},
                      {QStringLiteral("colourRange"), colourFullRange ? QStringLiteral("full") : QStringLiteral("limited")},
                      {QStringLiteral("sdrWhite"), sdrWhite},
                      {QStringLiteral("hdrPeak"), hdrPeak},
                      {QStringLiteral("downscaleFilter"), downscaleFilter}};
    QJsonObject recording{{QStringLiteral("folder"), folder},
                          {QStringLiteral("filenameTemplate"), filenameTemplate},
                          {QStringLiteral("filenameNoSpaces"), filenameNoSpaces},
                          {QStringLiteral("container"), container},
                          {QStringLiteral("customMuxer"), customMuxer},
                          {QStringLiteral("containerOptions"), containerOptions},
                          {QStringLiteral("encoder"), encoder},
                          {QStringLiteral("quality"), quality},
                          {QStringLiteral("bitrateKbps"), bitrateKbps},
                          {QStringLiteral("audioBitrateKbps"), audioBitrateKbps},
                          {QStringLiteral("audioTracks"), int(recordingTracks & 63)},
                          {QStringLiteral("outputHeight"), recordingHeight},
                          {QStringLiteral("encoding"), recordingChoice.toJson()},
                          {QStringLiteral("splitMinutes"), splitMinutes},
                          {QStringLiteral("splitMegabytes"), splitMegabytes},
                          {QStringLiteral("remuxToMp4"), remuxToMp4},
                          {QStringLiteral("replay"), QJsonObject{{QStringLiteral("seconds"), replaySeconds},
                                                                 {QStringLiteral("megabytes"), replayMegabytes},
                                                                 {QStringLiteral("prefix"), replayPrefix},
                                                                 {QStringLiteral("suffix"), replaySuffix}}}};
    QJsonObject audio{{QStringLiteral("devices"), devices}, {QStringLiteral("sampleRate"), audioFormat.normalized().rate},
                      {QStringLiteral("channels"), audioFormat.normalized().channels},
                      {QStringLiteral("monitorOutput"), monitorOutput},
                      {QStringLiteral("activeAudioOnly"), activeAudioOnly},
                      {QStringLiteral("lowLatency"), lowLatencyAudio},
                      {QStringLiteral("globalAudioHotkeys"), globalHotkeys},
                      {QStringLiteral("meter"), QJsonObject::fromVariantMap(MeterSettings::normalized(meter))},
                      {QStringLiteral("desktopAudio"), QJsonObject::fromVariantMap(AudioSettings::normalized(desktopAudio))},
                      {QStringLiteral("microphoneAudio"), QJsonObject::fromVariantMap(AudioSettings::normalized(microphoneAudio))},
                      {QStringLiteral("desktopGain"), desktopGain},
                      {QStringLiteral("microphoneGain"), microphoneGain},
                      {QStringLiteral("desktopMuted"), desktopMuted},
                      {QStringLiteral("microphoneMuted"), microphoneMuted},
                      {QStringLiteral("desktopAdded"), desktopAdded},
                      {QStringLiteral("microphoneAdded"), microphoneAdded},
                      {QStringLiteral("microphone"), microphone}};
    QJsonArray streamList;
    for (const StreamTarget &t : streams) streamList.append(t.toJson());
    QJsonObject streaming{{QStringLiteral("targets"), streamList},
                          {QStringLiteral("recordWhileStreaming"), recordWhileStreaming},
                          {QStringLiteral("stopRecordingWithStream"), stopRecordingWithStream},
                          {QStringLiteral("replayWhileStreaming"), replayWhileStreaming}};
    return QJsonObject{{QStringLiteral("version"), 3},
                       {QStringLiteral("streaming"), streaming},
                       {QStringLiteral("virtualCamera"), QJsonObject{{QStringLiteral("shows"), cameraShows},
                                                                     {QStringLiteral("target"), cameraTarget}}},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("video"), video},
                       {QStringLiteral("recording"), recording},
                       {QStringLiteral("audio"), audio},
                       {QStringLiteral("restoreTokens"), QJsonObject::fromVariantMap(restoreTokens)},
                       {QStringLiteral("hotkeys"), QJsonObject::fromVariantMap(hotkeys)},
                       {QStringLiteral("collection"), collection}};
}

bool Profile::fixedColourFormat(engine::ColourFormat *format) const {
    static const char *names[] = {"nv12", "i420", "i444", "p010", "i010", "p216", "p416", "bgra", "i210"};
    static_assert(sizeof names / sizeof names[0] == engine::kColourFormats);
    for (int i = 0; i < engine::kColourFormats; ++i)
        if (colourFormat == QLatin1String(names[i])) { *format = engine::ColourFormat(i); return true; }
    return false;
}

engine::OutputColour Profile::outputColour(engine::ColourFormat format) const {
    using S = engine::ColourSpace;
    engine::OutputColour c;
    c.format = format;
    c.space = colourSpace == QLatin1String("srgb") ? S::Srgb : colourSpace == QLatin1String("601") ? S::Bt601
            : colourSpace == QLatin1String("pq") ? S::Bt2100Pq : colourSpace == QLatin1String("hlg") ? S::Bt2100Hlg : S::Bt709;
    c.fullRange = colourFullRange || engine::formatIsRgb(format);
    c.sdrWhite = float(sdrWhite);
    c.hdrPeak = float(hdrPeak);
    c.downscale = downscaleFilter == QLatin1String("bilinear") ? engine::ScaleFilter::Bilinear
                : downscaleFilter == QLatin1String("bicubic")  ? engine::ScaleFilter::Bicubic
                : downscaleFilter == QLatin1String("lanczos")  ? engine::ScaleFilter::Lanczos : engine::ScaleFilter::Area;
    return c;
}

Profile Profile::fromJson(const QJsonObject &json) {
    Profile p;
    p.name = json.value(QStringLiteral("name")).toString(p.name);
    const QJsonObject video = json.value(QStringLiteral("video")).toObject();
    p.canvasWidth = video.value(QStringLiteral("width")).toInt(p.canvasWidth);
    p.canvasHeight = video.value(QStringLiteral("height")).toInt(p.canvasHeight);
    // Version 1 saved the startup screen guess, which was wrong under
    // fractional scaling and never chosen by anyone: back to automatic.
    if (json.value(QStringLiteral("version")).toInt() < 2) p.canvasWidth = p.canvasHeight = 0;
    // Zero is automatic; a file says anything, the canvas keeps what the rest can draw.
    if (p.canvasWidth > 0) p.canvasWidth = std::clamp(p.canvasWidth, 16, 8192);
    if (p.canvasHeight > 0) p.canvasHeight = std::clamp(p.canvasHeight, 16, 8192);
    p.fpsNum = video.value(QStringLiteral("fpsNum")).toInt(p.fpsNum);
    p.fpsDen = video.value(QStringLiteral("fpsDen")).toInt(p.fpsDen);
    if (p.fpsNum <= 0 || p.fpsDen <= 0) { p.fpsNum = 60; p.fpsDen = 1; }
    // Unknown names keep the defaults, so an older or hand-edited profile still records.
    engine::ColourFormat format;
    const QString colourFormat = video.value(QStringLiteral("colourFormat")).toString(p.colourFormat).toLower();
    Profile probe; probe.colourFormat = colourFormat;
    if (colourFormat == QLatin1String("auto") || probe.fixedColourFormat(&format)) p.colourFormat = colourFormat;
    const QString space = video.value(QStringLiteral("colourSpace")).toString(p.colourSpace).toLower();
    if (QStringList{"srgb", "601", "709", "pq", "hlg"}.contains(space)) p.colourSpace = space;
    p.colourFullRange = video.value(QStringLiteral("colourRange")).toString() == QLatin1String("full");
    p.sdrWhite = std::clamp(video.value(QStringLiteral("sdrWhite")).toInt(p.sdrWhite), 80, 1000);
    p.hdrPeak = std::clamp(video.value(QStringLiteral("hdrPeak")).toInt(p.hdrPeak), 400, 10000);
    const QString downscale = video.value(QStringLiteral("downscaleFilter")).toString(p.downscaleFilter).toLower();
    if (QStringList{"area", "bilinear", "bicubic", "lanczos"}.contains(downscale)) p.downscaleFilter = downscale;
    const QJsonObject rec = json.value(QStringLiteral("recording")).toObject();
    p.folder = rec.value(QStringLiteral("folder")).toString();
    p.filenameTemplate = rec.value(QStringLiteral("filenameTemplate")).toString(p.filenameTemplate);
    p.filenameNoSpaces = rec.value(QStringLiteral("filenameNoSpaces")).toBool();
    p.container = rec.value(QStringLiteral("container")).toString(p.container);
    p.customMuxer = rec.value(QStringLiteral("customMuxer")).toString().trimmed();
    p.containerOptions = rec.value(QStringLiteral("containerOptions")).toString().trimmed();
    p.encoder = rec.value(QStringLiteral("encoder")).toString();
    p.quality = rec.value(QStringLiteral("quality")).toInt(p.quality);
    p.bitrateKbps = rec.value(QStringLiteral("bitrateKbps")).toInt(p.bitrateKbps);
    p.recordingTracks = unsigned(rec.value(QStringLiteral("audioTracks")).toInt(1)) & 63;
    p.recordingHeight = std::clamp(rec.value(QStringLiteral("outputHeight")).toInt(0), 0, 4320) & ~1;
    p.audioBitrateKbps = rec.value(QStringLiteral("audioBitrateKbps")).toInt(p.audioBitrateKbps);
    p.recordingChoice.read(rec.value(QStringLiteral("encoding")).toObject(), p.bitrateKbps);
    // ProRes made way for APV (2026-10-24): the graphics-card choice moves to
    // APV on the same card at the matching tier; the processor one, which APV
    // has no counterpart for, goes back to the best available.
    if (p.encoder.startsWith(QLatin1String("prores_ks"))) {
        const QString was = p.recordingChoice.profile.toLower();
        p.recordingChoice.profile = was == QLatin1String("standard") ? QStringLiteral("standard")
                                  : was == QLatin1String("hq") || was.startsWith(QLatin1String("4444")) ? QStringLiteral("high")
                                                                                                        : QString();
        if (p.encoder.startsWith(QLatin1String("prores_ks_vulkan")))
            p.encoder.replace(0, int(qstrlen("prores_ks_vulkan")), QStringLiteral("apv_vulkan"));
        else
            p.encoder.clear(), p.recordingChoice.profile.clear();
    }
    p.splitMinutes = std::max(0, rec.value(QStringLiteral("splitMinutes")).toInt());
    p.splitMegabytes = std::max(0, rec.value(QStringLiteral("splitMegabytes")).toInt());
    p.remuxToMp4 = rec.value(QStringLiteral("remuxToMp4")).toBool();
    const QJsonObject replay = rec.value(QStringLiteral("replay")).toObject();
    p.replaySeconds = std::clamp(replay.value(QStringLiteral("seconds")).toInt(p.replaySeconds), 0, 6 * 3600);
    p.replayMegabytes = std::clamp(replay.value(QStringLiteral("megabytes")).toInt(p.replayMegabytes), 0, 64 * 1024);
    if (p.replaySeconds == 0 && p.replayMegabytes == 0) p.replaySeconds = 20;
    p.replayPrefix = replay.value(QStringLiteral("prefix")).toString(p.replayPrefix);
    p.replaySuffix = replay.value(QStringLiteral("suffix")).toString();
    const QJsonObject audio = json.value(QStringLiteral("audio")).toObject();
    p.audioFormat = AudioFormat{audio.value(QStringLiteral("sampleRate")).toInt(48000), audio.value(QStringLiteral("channels")).toInt(2)}.normalized();
    p.monitorOutput = audio.value(QStringLiteral("monitorOutput")).toString(p.monitorOutput);
    p.globalHotkeys = audio.value(QStringLiteral("globalAudioHotkeys")).toBool();
    p.lowLatencyAudio = audio.value(QStringLiteral("lowLatency")).toBool();
    p.meter = MeterSettings::normalized(audio.value(QStringLiteral("meter")).toObject().toVariantMap());
    p.activeAudioOnly = audio.value(QStringLiteral("activeAudioOnly")).toBool();
    p.desktopAudio = AudioSettings::normalized(audio.value(QStringLiteral("desktopAudio")).toObject().toVariantMap());
    p.microphoneAudio = AudioSettings::normalized(audio.value(QStringLiteral("microphoneAudio")).toObject().toVariantMap());
    p.desktopGain = float(AudioSettings::gain(audio.value(QStringLiteral("desktopGain")).toDouble(p.desktopGain)));
    p.microphoneGain = float(AudioSettings::gain(audio.value(QStringLiteral("microphoneGain")).toDouble(p.microphoneGain)));
    p.desktopMuted = audio.value(QStringLiteral("desktopMuted")).toBool(p.desktopMuted);
    p.microphoneMuted = audio.value(QStringLiteral("microphoneMuted")).toBool(p.microphoneMuted);
    // A profile saved before they had to be added already records them: it keeps them.
    p.desktopAdded = audio.value(QStringLiteral("desktopAdded")).toBool(true);
    p.microphoneAdded = audio.value(QStringLiteral("microphoneAdded")).toBool(true);
    p.microphone = audio.value(QStringLiteral("microphone")).toString();
    QSet<QString> deviceIds;
    for (const auto &value : audio.value("devices").toArray()) {
        const auto object = value.toObject();
        SourceDef device{object.value("id").toString(), object.value("kind").toString(), object.value("name").toString().trimmed(), {}};
        if (device.kind != "audio-input" && device.kind != "audio-output") continue;
        if (device.id.isEmpty() || deviceIds.contains(device.id)) device.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        deviceIds.insert(device.id);
        if (device.name.isEmpty()) device.name = device.kind == "audio-input" ? QStringLiteral("Audio input") : QStringLiteral("Audio output");
        const auto raw = object.value("settings").toObject().toVariantMap();
        device.settings = AudioSettings::normalized(raw);
        device.settings["volume"] = AudioSettings::gain(raw.value("volume", 1.).toDouble());
        device.settings["muted"] = raw.value("muted", false).toBool();
        device.settings["audioNode"] = raw.value("audioNode").toString();
        device.settings["audioLabel"] = raw.value("audioLabel").toString();
        p.globalAudioDevices.append(device);
    }
    for (const QJsonValue &v : json.value(QStringLiteral("sources")).toArray()) {
        const QJsonObject o = v.toObject();
        SourceEntry s;
        s.name = o.value(QStringLiteral("name")).toString();
        s.types = uint(o.value(QStringLiteral("types")).toInt(1));
        s.cursor = o.value(QStringLiteral("cursor")).toBool(true);
        s.restoreToken = o.value(QStringLiteral("restoreToken")).toString();
        p.legacySources.append(s);
    }
    const QJsonObject streaming = json.value(QStringLiteral("streaming")).toObject();
    QSet<QString> streamIds;
    for (const QJsonValue &v : streaming.value(QStringLiteral("targets")).toArray()) {
        StreamTarget t = StreamTarget::fromJson(v.toObject());
        if (streamIds.contains(t.id)) t.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        streamIds.insert(t.id);
        p.streams.append(t);
    }
    p.recordWhileStreaming = streaming.value(QStringLiteral("recordWhileStreaming")).toBool();
    p.stopRecordingWithStream = streaming.value(QStringLiteral("stopRecordingWithStream")).toBool();
    p.replayWhileStreaming = streaming.value(QStringLiteral("replayWhileStreaming")).toBool();
    const QJsonObject camera = json.value(QStringLiteral("virtualCamera")).toObject();
    p.cameraShows = camera.value(QStringLiteral("shows")).toString(p.cameraShows);
    if (p.cameraShows != QLatin1String("studio") && p.cameraShows != QLatin1String("source") && p.cameraShows != QLatin1String("preview"))
        p.cameraShows = QStringLiteral("program");
    p.cameraTarget = camera.value(QStringLiteral("target")).toString();
    p.restoreTokens = json.value(QStringLiteral("restoreTokens")).toObject().toVariantMap();
    p.hotkeys = json.value(QStringLiteral("hotkeys")).toObject().toVariantMap();
    p.collection = json.value(QStringLiteral("collection")).toString(p.collection);
    if (p.collection.isEmpty() || p.collection.contains(QLatin1Char('/'))) p.collection = QStringLiteral("Default");
    return p;
}

QString Profile::pathFor(const QString &name) {
    if (name.isEmpty() || name == "." || name == ".." || name.contains('/') || name.contains('\\') || name.contains(QChar(0))) return {};
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return base + QStringLiteral("/profiles/") + name + QStringLiteral("/profile.json");
}

Profile Profile::load(const QString &name) {
    QFile f(pathFor(name));
    if (!f.open(QIODevice::ReadOnly)) return Profile{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return Profile{};
    return fromJson(doc.object());
}

bool Profile::save(const QString &name) const {
    const QString path = pathFor(name);
    if (path.isEmpty()) return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    // Only this user reads it: with no keyring it holds stream keys and passwords.
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
    return f.commit();
}

QString Profile::defaultFolder() {
    QString videos = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (videos.isEmpty()) videos = QDir::homePath();
    return videos + QStringLiteral("/OmaBroadcast");
}

// A folder from an imported or older profile may not be a full path, and the
// folder the program happened to start in is nowhere a person would look.
QString Profile::resolvedFolder() const {
    if (folder.isEmpty()) return defaultFolder();
    if (folder == QLatin1String("~") || folder.startsWith(QLatin1String("~/"))) return QDir::homePath() + folder.mid(1);
    return QDir(QDir::homePath()).absoluteFilePath(folder);
}

QString expandFileName(const QString &pattern, qint64 unixSeconds, const QMap<QString, QString> &fields, bool noSpaces) {
    QString named;
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        const qsizetype close = pattern[i] == QLatin1Char('{') ? pattern.indexOf(QLatin1Char('}'), i) : -1;
        const QString key = close > i ? pattern.mid(i + 1, close - i - 1) : QString();
        if (close > i && fields.contains(key)) {
            // A field's own "%" stays literal through the date codes.
            named += QString(fields.value(key)).replace(QLatin1Char('%'), QStringLiteral("%%"));
            i = close;
        } else {
            named += pattern[i];
        }
    }
    QString name = QString::fromStdString(Recorder::expandTemplate(named.toStdString(), unixSeconds));
    static const QString refused = QStringLiteral("/\\:*?\"<>|");
    for (QChar &c : name)
        if (c.unicode() < 32 || c.unicode() == 127 || refused.contains(c)) c = QLatin1Char('-');
    if (noSpaces) name.replace(QLatin1Char(' '), QLatin1Char('_'));
    while (name.startsWith(QLatin1Char('.')) || name.startsWith(QLatin1Char(' '))) name.remove(0, 1);
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) name.chop(1);
    // Room for " (n).mkv" inside the usual 255-byte limit.
    // Whole characters only: never half of a pair (an emoji, say).
    while (name.toUtf8().size() > 230) name.chop(name.size() > 1 && name.back().isLowSurrogate() ? 2 : 1);
    if (name.isEmpty()) return expandFileName(QStringLiteral("%Y-%m-%d %H-%M-%S"), unixSeconds, {}, noSpaces);
    return name;
}
