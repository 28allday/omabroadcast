#include "backend.h"

#include "core/audio_settings.h"
#include "core/drawn.h"
#include "core/transitions.h"
#include "core/video_filter_settings.h"

#include <QBuffer>
#include <QFile>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QImageWriter>
#include <QJsonArray>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScreen>

#include <algorithm>
#include <cmath>

// Remote control (see control/remote_control.h): the protocol's scenes are
// studios, its inputs are sources and the global sound channels, its scene
// items are items and its outputs are recording, streaming, the replay
// buffer and the virtual camera. Things are named by id ("uuid") or name.
// Names, kinds and states on the wire are the protocol's.

namespace {

using Reply = RemoteControl::Reply;

Reply ok(const QJsonObject &data = {}) { return {RemoteControl::Success, {}, data}; }
Reply refuse(int code, const QString &comment) { return {code, comment, {}}; }

// Kinds clients know, for ours that have one.
QString wireKind(const QString &kind) {
    static const QHash<QString, QString> kinds{
        {"media", "ffmpeg_source"}, {"image", "image_source"}, {"colour", "color_source_v3"},
        {"text", "text_ft2_source_v2"}, {"slideshow", "image_slideshow"}, {"device", "v4l2_input"},
        {"audio-input", "pulse_input_capture"}, {"audio-output", "pulse_output_capture"},
        {"display", "pipewire-screen-capture-source"}, {"window", "pipewire-window-capture-source"},
        {"browser", "browser_source"}};
    return kinds.value(kind, kind);
}

QString unversioned(const QString &kind) {
    static const QRegularExpression version(QStringLiteral("_v\\d+$"));
    return QString(kind).remove(version);
}

double toDb(double mul) { return mul > 0 ? 20 * std::log10(mul) : -100; }

QString outputState(bool active) { return active ? QStringLiteral("OBS_WEBSOCKET_OUTPUT_STARTED") : QStringLiteral("OBS_WEBSOCKET_OUTPUT_STOPPED"); }

QString mediaState(const QVariantMap &playback) {
    const QString state = playback.value("state").toString();
    if (playback.isEmpty() || state.isEmpty()) return QStringLiteral("OBS_MEDIA_STATE_NONE");
    if (state == QLatin1String("playing")) return QStringLiteral("OBS_MEDIA_STATE_PLAYING");
    if (state == QLatin1String("paused")) return QStringLiteral("OBS_MEDIA_STATE_PAUSED");
    if (state == QLatin1String("opening")) return QStringLiteral("OBS_MEDIA_STATE_OPENING");
    if (state == QLatin1String("ended")) return QStringLiteral("OBS_MEDIA_STATE_ENDED");
    if (state == QLatin1String("inactive")) return QStringLiteral("OBS_MEDIA_STATE_STOPPED");
    if (state.startsWith(QLatin1String("failed"))) return QStringLiteral("OBS_MEDIA_STATE_ERROR");
    return QStringLiteral("OBS_MEDIA_STATE_NONE");
}

QString timecode(qint64 ms) {
    const qint64 s = ms / 1000;
    return QStringLiteral("%1:%2:%3.%4").arg(s / 3600, 2, 10, QLatin1Char('0')).arg(s / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(s % 60, 2, 10, QLatin1Char('0')).arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

int alignmentNumber(Transform::Alignment a) {
    // Left 1, right 2, top 4, bottom 8; 0 is the centre.
    static const int bits[] = {5, 4, 6, 1, 0, 2, 9, 8, 10};
    return bits[int(a)];
}

QString boundsType(Transform::Bounds b) {
    static const char *names[] = {"OBS_BOUNDS_NONE", "OBS_BOUNDS_STRETCH", "OBS_BOUNDS_SCALE_INNER", "OBS_BOUNDS_SCALE_OUTER",
                                  "OBS_BOUNDS_SCALE_OUTER", "OBS_BOUNDS_SCALE_TO_WIDTH", "OBS_BOUNDS_SCALE_TO_HEIGHT", "OBS_BOUNDS_MAX_ONLY"};
    return QString::fromLatin1(names[int(b)]);
}

QString filterLabel(const QString &type) {
    for (const QVariant &d : VideoFilterSettings::catalog())
        if (d.toMap().value("type").toString() == type) return d.toMap().value("label").toString();
    return type;
}

// A filter's name: its own, or its kind's label.
QString filterName(const QVariantMap &filter) {
    const QString own = filter.value("name").toString();
    return own.isEmpty() ? filterLabel(filter.value("type").toString()) : own;
}


// A source's settings as a client may see them: a phone source's passphrase is
// for the people in front of the app.
QJsonObject settingsToHandOut(const SourceDef &source) {
    QJsonObject out = QJsonObject::fromVariantMap(source.settings);
    out.remove(QLatin1String(kPassphraseKey));
    return out;
}
}  // namespace

QStringList Backend::remoteRequests() {
    return {"GetStats", "GetHotkeyList", "TriggerHotkeyByName",
            "GetSceneCollectionList", "SetCurrentSceneCollection", "GetProfileList", "SetCurrentProfile",
            "GetVideoSettings", "GetRecordDirectory",
            "GetSourceActive", "GetSourceScreenshot",
            "GetSceneList", "GetGroupList", "GetCurrentProgramScene", "SetCurrentProgramScene", "GetCurrentPreviewScene",
            "SetCurrentPreviewScene", "CreateScene", "RemoveScene", "SetSceneName", "GetSceneSceneTransitionOverride",
            "SetSceneSceneTransitionOverride",
            "GetInputList", "GetInputKindList", "GetSpecialInputs", "SetInputName", "GetInputSettings", "SetInputSettings",
            "PressInputPropertiesButton",
            "GetInputMute", "SetInputMute", "ToggleInputMute", "GetInputVolume", "SetInputVolume",
            "GetTransitionKindList", "GetSceneTransitionList", "GetCurrentSceneTransition", "SetCurrentSceneTransition",
            "SetCurrentSceneTransitionDuration", "GetCurrentSceneTransitionCursor", "TriggerStudioModeTransition", "SetTBarPosition",
            "GetSourceFilterList", "GetSourceFilter", "SetSourceFilterEnabled",
            "GetSceneItemList", "GetGroupSceneItemList", "GetSceneItemId", "GetSceneItemSource", "GetSceneItemEnabled",
            "SetSceneItemEnabled", "GetSceneItemLocked", "SetSceneItemLocked", "GetSceneItemIndex", "GetSceneItemTransform",
            "GetVirtualCamStatus", "ToggleVirtualCam", "StartVirtualCam", "StopVirtualCam",
            "GetReplayBufferStatus", "ToggleReplayBuffer", "StartReplayBuffer", "StopReplayBuffer", "SaveReplayBuffer",
            "GetLastReplayBufferReplay", "GetOutputList",
            "GetStreamStatus", "ToggleStream", "StartStream", "StopStream",
            "GetRecordStatus", "ToggleRecord", "StartRecord", "StopRecord", "ToggleRecordPause", "PauseRecord", "ResumeRecord",
            "SplitRecordFile", "CreateRecordChapter",
            "GetMediaInputStatus", "SetMediaInputCursor", "OffsetMediaInputCursor", "TriggerMediaInputAction",
            "GetStudioModeEnabled", "SetStudioModeEnabled", "GetMonitorList", "OpenVideoMixProjector", "OpenSourceProjector"};
}

RemoteControl::Settings Backend::remoteSettings() const {
    const QVariantMap p = preference(QStringLiteral("remoteControl"), QVariantMap()).toMap();
    RemoteControl::Settings s;
    s.enabled = p.value("enabled", false).toBool() && !safeMode();
    s.port = quint16(std::clamp(p.value("port", 4455).toInt(), 1024, 65535));
    s.network = p.value("network", true).toBool();
    s.password = p.value("auth", true).toBool() ? p.value("password").toString() : QString();
    return s;
}

QVariantMap Backend::remoteControl() const {
    const QVariantMap p = preference(QStringLiteral("remoteControl"), QVariantMap()).toMap();
    QStringList addresses;
    for (const QHostAddress &a : QNetworkInterface::allAddresses())
        if (!a.isLoopback() && a.protocol() == QAbstractSocket::IPv4Protocol) addresses << a.toString();
    return {{"enabled", p.value("enabled", false).toBool()}, {"port", remoteSettings().port},
            {"network", p.value("network", true).toBool()}, {"auth", p.value("auth", true).toBool()},
            {"password", p.value("password").toString()}, {"listening", m_remote.listening()},
            {"error", m_remoteError}, {"clients", m_remote.clients()}, {"addresses", addresses}};
}

void Backend::setRemoteOption(const QString &key, const QVariant &value) {
    QVariantMap p = preference(QStringLiteral("remoteControl"), QVariantMap()).toMap();
    if (key == QLatin1String("port")) p.insert(key, std::clamp(value.toInt(), 1024, 65535));
    else if (key == QLatin1String("password")) p.insert(key, value.toString().trimmed());
    else if (key == QLatin1String("enabled") || key == QLatin1String("network") || key == QLatin1String("auth")) p.insert(key, value.toBool());
    else return;
    // A password from the start, so turning it on is never left open; one
    // cleared is made again (no password is the "auth" switch, not this).
    if (p.value("password").toString().isEmpty()) p.insert("password", newRemotePassword());
    setPreference(QStringLiteral("remoteControl"), p);
    applyRemote();
}

QString Backend::newRemotePassword() const {
    static const char letters[] = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    QString out;
    for (int i = 0; i < 16; ++i) out += QLatin1Char(letters[QRandomGenerator::system()->bounded(int(sizeof letters - 1))]);
    return out;
}

void Backend::applyRemote() {
    // Settings saved before they held a password may still be readable by others.
    const QString settings = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/global.json");
    if (QFile::exists(settings)) QFile::setPermissions(settings, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    // Settings from before a cleared password was made again would listen with
    // no sign-in while the sheet says there is one. "auth" off is how to have none.
    if (const QVariantMap p = preference(QStringLiteral("remoteControl"), QVariantMap()).toMap();
        remoteSettings().enabled && p.value("auth", true).toBool() && p.value("password").toString().isEmpty()) {
        QVariantMap withPassword = p;
        withPassword.insert("password", newRemotePassword());
        setPreference(QStringLiteral("remoteControl"), withPassword);
    }
    QString why;
    m_remoteError = m_remote.apply(remoteSettings(), &why) ? QString() : tr("Remote control could not listen on port %1: %2").arg(remoteSettings().port).arg(why);
    if (!m_remoteError.isEmpty()) qInfo("remote control: %s", qPrintable(m_remoteError));
    m_remoteSeen = remoteSnapshot();
    emit remoteControlChanged();
}

// ---- What things are called on the wire

int Backend::remoteItemNumber(const QString &itemId) {
    // Item ids are "i" and a number, unique in the collection: that number.
    bool numeric = false;
    const int n = itemId.mid(1).toInt(&numeric);
    if (itemId.startsWith(QLatin1Char('i')) && numeric && n > 0 && n < 1000000) return n;
    auto it = m_remoteItemNumbers.find(itemId);
    if (it == m_remoteItemNumbers.end()) it = m_remoteItemNumbers.insert(itemId, 1000000 + int(m_remoteItemNumbers.size()));
    return *it;
}

const SceneItem *Backend::remoteItem(const Scene &scene, int number, int *index) {
    for (int i = 0; i < scene.items.size(); ++i)
        if (remoteItemNumber(scene.items[i].id) == number) {
            if (index) *index = i;
            return &scene.items[i];
        }
    return nullptr;
}

QList<Backend::RemoteInput> Backend::remoteInputs() const {
    QList<RemoteInput> out;
    if (desktopAdded()) out.append({QStringLiteral("desktop"), tr("Desktop audio"), QStringLiteral("pulse_output_capture"), true});
    if (microphoneAdded()) out.append({QStringLiteral("microphone"), tr("Microphone"), QStringLiteral("pulse_input_capture"), true});
    for (const SourceDef &d : m_session.profile().globalAudioDevices) out.append({d.id, d.name, wireKind(d.kind), true});
    for (const SourceDef &s : m_production.collection().sources)
        if (s.kind != QLatin1String("studio") && s.kind != QLatin1String("group")) out.append({s.id, s.name, wireKind(s.kind), s.hasAudio()});
    return out;
}

std::optional<Backend::RemoteInput> Backend::remoteInput(const QJsonObject &d, Reply *why) const {
    const QString uuid = d.value("inputUuid").toString(), name = d.value("inputName").toString();
    if (uuid.isEmpty() && name.isEmpty()) { *why = refuse(RemoteControl::MissingRequestField, QStringLiteral("inputName or inputUuid is needed.")); return {}; }
    for (const RemoteInput &i : remoteInputs())
        if ((!uuid.isEmpty() && i.id == uuid) || (uuid.isEmpty() && i.name == name)) return i;
    *why = refuse(RemoteControl::ResourceNotFound, QStringLiteral("No input called %1.").arg(uuid.isEmpty() ? name : uuid));
    return {};
}

const Scene *Backend::remoteScene(const QJsonObject &d, Reply *why, bool groups) const {
    const QString uuid = d.value("sceneUuid").toString(), name = d.value("sceneName").toString();
    if (uuid.isEmpty() && name.isEmpty()) { *why = refuse(RemoteControl::MissingRequestField, QStringLiteral("sceneName or sceneUuid is needed.")); return nullptr; }
    const Collection &c = m_production.collection();
    for (const Scene &s : c.scenes) {
        const QString shown = s.group ? remoteGroupName(s) : s.name;
        if ((!groups && s.group) || !(uuid.isEmpty() ? shown == name : s.id == uuid)) continue;
        return &s;
    }
    *why = refuse(RemoteControl::ResourceNotFound, QStringLiteral("No scene called %1.").arg(uuid.isEmpty() ? name : uuid));
    return nullptr;
}

QString Backend::remoteGroupName(const Scene &group) const {
    for (const SourceDef &s : m_production.collection().sources)
        if (m_production.collection().shownStudio(s) == group.id) return s.name;
    return group.name;
}

// A source by sourceName/sourceUuid: a studio first, then an input.
bool Backend::remoteSource(const QJsonObject &d, QString *shows, QString *target, Reply *why) const {
    const QString uuid = d.value("sourceUuid").toString(), name = d.value("sourceName").toString();
    if (uuid.isEmpty() && name.isEmpty()) { *why = refuse(RemoteControl::MissingRequestField, QStringLiteral("sourceName or sourceUuid is needed.")); return false; }
    for (const Scene &s : m_production.collection().scenes)
        if (!s.group && (uuid.isEmpty() ? s.name == name : s.id == uuid)) { *shows = QStringLiteral("studio"); *target = s.id; return true; }
    for (const SourceDef &s : m_production.collection().sources)
        if (uuid.isEmpty() ? s.name == name : s.id == uuid) { *shows = QStringLiteral("source"); *target = s.id; return true; }
    *why = refuse(RemoteControl::ResourceNotFound, QStringLiteral("No source called %1.").arg(uuid.isEmpty() ? name : uuid));
    return false;
}

QJsonObject Backend::remoteItemJson(const Scene &scene, int index) {
    const SceneItem &item = scene.items[index];
    const SourceDef *source = m_production.collection().source(item.source);
    const QString shown = source ? m_production.collection().shownStudio(*source) : QString();
    const bool group = source && source->kind == QLatin1String("group");
    return {{"sceneItemId", remoteItemNumber(item.id)}, {"sceneItemIndex", index}, {"sceneItemEnabled", item.visible},
            {"sceneItemLocked", item.locked}, {"sceneItemBlendMode", QStringLiteral("OBS_BLEND_NORMAL")},
            {"sourceName", source ? source->name : QString()}, {"sourceUuid", source ? source->id : QString()},
            {"sourceType", shown.isEmpty() ? QStringLiteral("OBS_SOURCE_TYPE_INPUT") : QStringLiteral("OBS_SOURCE_TYPE_SCENE")},
            {"inputKind", shown.isEmpty() && source ? QJsonValue(wireKind(source->kind)) : QJsonValue()},
            {"isGroup", shown.isEmpty() ? QJsonValue() : QJsonValue(group)},
            {"sceneItemTransform", remoteTransform(item)}};
}

QJsonObject Backend::remoteTransform(const SceneItem &item) const {
    const Transform &t = item.transform;
    const QSize size = sourceSize(item.source);
    const double w = std::max(0, size.width() - t.cropLeft - t.cropRight), h = std::max(0, size.height() - t.cropTop - t.cropBottom);
    const bool bounded = t.bounds != Transform::Bounds::None;
    return {{"positionX", t.x}, {"positionY", t.y}, {"rotation", t.rotation}, {"scaleX", t.scaleX}, {"scaleY", t.scaleY},
            {"sourceWidth", size.width()}, {"sourceHeight", size.height()},
            {"width", bounded ? t.boundsWidth : w * t.scaleX}, {"height", bounded ? t.boundsHeight : h * t.scaleY},
            {"alignment", alignmentNumber(t.positionAlignment)}, {"boundsType", boundsType(t.bounds)},
            {"boundsAlignment", alignmentNumber(t.boundsAlignment)}, {"boundsWidth", t.boundsWidth},
            {"boundsHeight", t.boundsHeight}, {"cropLeft", t.cropLeft}, {"cropTop", t.cropTop}, {"cropRight", t.cropRight},
            {"cropBottom", t.cropBottom}, {"cropToBounds", t.clipToBounds}};
}

bool Backend::remoteMuted(const RemoteInput &input) const {
    if (input.id == QLatin1String("desktop")) return desktopMuted();
    if (input.id == QLatin1String("microphone")) return microphoneMuted();
    for (const SourceDef &d : m_session.profile().globalAudioDevices)
        if (d.id == input.id) return d.settings.value("muted", false).toBool();
    const SourceDef *s = m_production.collection().source(input.id);
    return s && s->settings.value("muted", false).toBool();
}

double Backend::remoteVolume(const RemoteInput &input) const {
    if (input.id == QLatin1String("desktop")) return desktopGain();
    if (input.id == QLatin1String("microphone")) return microphoneGain();
    for (const SourceDef &d : m_session.profile().globalAudioDevices)
        if (d.id == input.id) return AudioSettings::gain(d.settings.value("volume", 1.0).toDouble());
    const SourceDef *s = m_production.collection().source(input.id);
    return s ? AudioSettings::gain(s->settings.value("volume", 1.0).toDouble()) : 1.0;
}

void Backend::setRemoteAudio(const RemoteInput &input, const QString &key, const QVariant &value) {
    const bool mute = key == QLatin1String("muted");
    if (input.id == QLatin1String("desktop")) { if (mute) setDesktopMuted(value.toBool()); else setDesktopGain(value.toDouble()); return; }
    if (input.id == QLatin1String("microphone")) { if (mute) setMicrophoneMuted(value.toBool()); else setMicrophoneGain(value.toDouble()); return; }
    for (const SourceDef &d : m_session.profile().globalAudioDevices)
        if (d.id == input.id) { setGlobalAudioDeviceSetting(d.id, key, value); return; }
    setSourceSetting(input.id, key, value);
}

// ---- Requests

void Backend::remoteRequest(const QString &type, const QJsonObject &d, const RemoteControl::Done &done) {
    const Collection &c = m_production.collection();
    Reply why;
    auto need = [&](const char *field, bool (QJsonValue::*is)() const) {
        const QJsonValue v = d.value(QLatin1String(field));
        if (v.isUndefined() || v.isNull()) { why = refuse(RemoteControl::MissingRequestField, QStringLiteral("%1 is needed.").arg(field)); return false; }
        if (!(v.*is)()) { why = refuse(RemoteControl::InvalidRequestFieldType, QStringLiteral("%1 has the wrong type.").arg(field)); return false; }
        return true;
    };
    auto studioJson = [&](const QString &id) {
        const Scene *s = c.scene(id);
        return QJsonObject{{"sceneName", s ? QJsonValue(s->name) : QJsonValue()}, {"sceneUuid", s ? QJsonValue(id) : QJsonValue()}};
    };
    const QString preview = studioMode() ? currentScene() : QString();

    // General
    if (type == QLatin1String("GetStats")) {
        refreshSystemStats();
        const QVariantMap sys = systemStats();
        const Session::Stats s = m_session.stats();
        quint64 frames = 0, dropped = 0;
        for (const Session::OutputStats &o : m_session.outputStats()) { frames += o.frames; dropped += o.dropped; }
        done(ok({{"cpuUsage", std::max(0.0, sys.value("processCpu").toDouble())},
                 {"memoryUsage", std::max(0.0, sys.value("residentBytes").toDouble() / (1024 * 1024))},
                 {"availableDiskSpace", std::max(0.0, sys.value("freeBytes").toDouble() / (1024 * 1024))},
                 {"activeFps", s.fps}, {"averageFrameRenderTime", s.renderMs},
                 {"renderSkippedFrames", double(s.lateTicks)}, {"renderTotalFrames", std::round(m_remoteUptime.elapsed() / 1000.0 * fps())},
                 {"outputSkippedFrames", double(dropped)}, {"outputTotalFrames", double(frames)}}));
    } else if (type == QLatin1String("GetHotkeyList")) {
        QJsonArray names;
        for (const QVariant &a : actions()) {
            const QVariantMap m = a.toMap();
            names.append(m.value("argument").toString().isEmpty() ? m.value("id").toString() : m.value("id").toString() + QLatin1Char('|') + m.value("argument").toString());
        }
        done(ok({{"hotkeys", names}}));
    } else if (type == QLatin1String("TriggerHotkeyByName")) {
        if (!need("hotkeyName", &QJsonValue::isString)) return done(why);
        const QString name = d.value("hotkeyName").toString();
        const int bar = name.indexOf(QLatin1Char('|'));
        const QString error = runAction(bar < 0 ? name : name.left(bar), bar < 0 ? d.value("contextName").toString() : name.mid(bar + 1));
        done(error.isEmpty() ? ok() : refuse(RemoteControl::ResourceNotFound, error));

    // Config
    } else if (type == QLatin1String("GetSceneCollectionList")) {
        QJsonArray names;
        for (const QVariant &v : collections()) names.append(v.toMap().value("name").toString());
        done(ok({{"currentSceneCollectionName", collectionName()}, {"sceneCollections", names}}));
    } else if (type == QLatin1String("SetCurrentSceneCollection")) {
        if (!need("sceneCollectionName", &QJsonValue::isString)) return done(why);
        for (const QVariant &v : collections())
            if (v.toMap().value("name").toString() == d.value("sceneCollectionName").toString()) {
                if (!v.toMap().value("current").toBool()) switchCollection(v.toMap().value("file").toString());
                return done(ok());
            }
        done(refuse(RemoteControl::ResourceNotFound, QStringLiteral("No scene collection by that name.")));
    } else if (type == QLatin1String("GetProfileList")) {
        QJsonArray names;
        for (const QVariant &v : profiles()) names.append(v.toMap().value("name").toString());
        done(ok({{"currentProfileName", profileName()}, {"profiles", names}}));
    } else if (type == QLatin1String("SetCurrentProfile")) {
        if (!need("profileName", &QJsonValue::isString)) return done(why);
        for (const QVariant &v : profiles())
            if (v.toMap().value("name").toString() == d.value("profileName").toString()) {
                if (v.toMap().value("current").toBool() || switchProfile(v.toMap().value("id").toString())) return done(ok());
                return done(refuse(RemoteControl::CannotAct, QStringLiteral("The profile cannot change now.")));
            }
        done(refuse(RemoteControl::ResourceNotFound, QStringLiteral("No profile by that name.")));
    } else if (type == QLatin1String("GetVideoSettings")) {
        const Profile &p = m_session.profile();
        done(ok({{"fpsNumerator", p.fpsNum}, {"fpsDenominator", p.fpsDen}, {"baseWidth", canvasWidth()}, {"baseHeight", canvasHeight()},
                 {"outputWidth", canvasWidth()}, {"outputHeight", canvasHeight()}}));
    } else if (type == QLatin1String("GetRecordDirectory")) {
        done(ok({{"recordDirectory", folder()}}));

    // Sources
    } else if (type == QLatin1String("GetSourceActive")) {
        QString shows, target;
        if (!remoteSource(d, &shows, &target, &why)) return done(why);
        // A studio inside the one shown, or a visible item of the source in any of them.
        auto in = [&](const QString &top) {
            if (top.isEmpty()) return false;
            if (shows == QLatin1String("studio")) return c.shows(top, target);
            for (const Scene &s : c.scenes)
                if (c.shows(top, s.id))
                    for (const SceneItem &i : s.items) if (i.source == target && i.visible) return true;
            return false;
        };
        const bool live = in(programScene()), shown = live || in(preview);
        done(ok({{"videoActive", live}, {"videoShowing", shown}}));
    } else if (type == QLatin1String("GetSourceScreenshot")) {
        QString shows, target;
        if (!remoteSource(d, &shows, &target, &why)) return done(why);
        if (!need("imageFormat", &QJsonValue::isString)) return done(why);
        const QByteArray format = d.value("imageFormat").toString().toLower().toLatin1();
        if (!QImageWriter::supportedImageFormats().contains(format)) return done(refuse(RemoteControl::InvalidRequestField, QStringLiteral("Unsupported imageFormat.")));
        const int w = d.value("imageWidth").toInt(0), h = d.value("imageHeight").toInt(0), quality = d.value("imageCompressionQuality").toInt(-1);
        if ((d.contains("imageWidth") && (w < 8 || w > 4096)) || (d.contains("imageHeight") && (h < 8 || h > 4096)) || quality < -1 || quality > 100)
            return done(refuse(RemoteControl::RequestFieldOutOfRange, QStringLiteral("A size or the quality is out of range.")));
        // Each one waits for a picture at full size: a few at a time.
        if (m_remoteShots.size() >= 8) return done(refuse(RemoteControl::RequestProcessingFailed, QStringLiteral("Too many screenshots at once; try again.")));
        const QString id = QStringLiteral("remote-%1").arg(++m_screenshotSerial);
        m_remoteShots.insert(id, {done, format, w, h, quality});
        m_session.requestSnapshot(id, shows, target);

    // Scenes
    } else if (type == QLatin1String("GetSceneList")) {
        QJsonArray scenes;
        QList<const Scene *> studios;
        for (const Scene &s : c.scenes) if (!s.group) studios.prepend(&s);
        // The bottom of the list first.
        for (int i = 0; i < studios.size(); ++i) scenes.append(QJsonObject{{"sceneIndex", i}, {"sceneName", studios[i]->name}, {"sceneUuid", studios[i]->id}});
        const QJsonObject program = studioJson(programScene()), previewed = studioJson(preview);
        done(ok({{"currentProgramSceneName", program.value("sceneName")}, {"currentProgramSceneUuid", program.value("sceneUuid")},
                 {"currentPreviewSceneName", previewed.value("sceneName")}, {"currentPreviewSceneUuid", previewed.value("sceneUuid")},
                 {"scenes", scenes}}));
    } else if (type == QLatin1String("GetGroupList")) {
        QJsonArray groups;
        for (const Scene &s : c.scenes) if (s.group) groups.append(remoteGroupName(s));
        done(ok({{"groups", groups}}));
    } else if (type == QLatin1String("GetCurrentProgramScene") || type == QLatin1String("GetCurrentPreviewScene")) {
        const bool program = type == QLatin1String("GetCurrentProgramScene");
        if (!program && !studioMode()) return done(refuse(RemoteControl::StudioModeNotActive, QStringLiteral("Studio mode is off.")));
        const QJsonObject s = studioJson(program ? programScene() : preview);
        const QString old = program ? QStringLiteral("currentProgramScene") : QStringLiteral("currentPreviewScene");
        done(ok({{"sceneName", s.value("sceneName")}, {"sceneUuid", s.value("sceneUuid")},
                 {old + QStringLiteral("Name"), s.value("sceneName")}, {old + QStringLiteral("Uuid"), s.value("sceneUuid")}}));
    } else if (type == QLatin1String("SetCurrentProgramScene")) {
        const Scene *s = remoteScene(d, &why);
        if (!s) return done(why);
        if (studioMode()) m_session.takeProgram(s->id, takeCopy());
        else setCurrentScene(s->id);
        done(ok());
    } else if (type == QLatin1String("SetCurrentPreviewScene")) {
        if (!studioMode()) return done(refuse(RemoteControl::StudioModeNotActive, QStringLiteral("Studio mode is off.")));
        const Scene *s = remoteScene(d, &why);
        if (!s) return done(why);
        setCurrentScene(s->id);
        done(ok());
    } else if (type == QLatin1String("CreateScene")) {
        if (!need("sceneName", &QJsonValue::isString)) return done(why);
        const QString name = d.value("sceneName").toString().trimmed();
        if (name.isEmpty()) return done(refuse(RemoteControl::RequestFieldEmpty, QStringLiteral("sceneName is empty.")));
        for (const Scene &s : c.scenes) if (!s.group && s.name == name) return done(refuse(RemoteControl::ResourceAlreadyExists, QStringLiteral("A scene has that name.")));
        // Made, not shown: outside studio mode the current studio is on air.
        done(ok({{"sceneUuid", m_production.addScene(name, false)}}));
    } else if (type == QLatin1String("RemoveScene")) {
        const Scene *s = remoteScene(d, &why);
        if (!s) return done(why);
        int studios = 0;
        for (const Scene &each : c.scenes) studios += !each.group;
        if (studios < 2) return done(refuse(RemoteControl::NotEnoughResources, QStringLiteral("The last scene stays.")));
        removeScene(s->id);
        done(ok());
    } else if (type == QLatin1String("SetSceneName")) {
        const Scene *s = remoteScene(d, &why);
        if (!s) return done(why);
        if (!need("newSceneName", &QJsonValue::isString)) return done(why);
        const QString name = d.value("newSceneName").toString().trimmed();
        if (name.isEmpty()) return done(refuse(RemoteControl::RequestFieldEmpty, QStringLiteral("newSceneName is empty.")));
        for (const Scene &each : c.scenes) if (!each.group && each.id != s->id && each.name == name) return done(refuse(RemoteControl::ResourceAlreadyExists, QStringLiteral("A scene has that name.")));
        renameScene(s->id, name);
        done(ok());
    } else if (type == QLatin1String("GetSceneSceneTransitionOverride")) {
        const Scene *s = remoteScene(d, &why);
        if (!s) return done(why);
        QJsonValue name;
        for (const QVariant &t : Transitions::all(c))
            if (t.toMap().value("id").toString() == s->transition) name = t.toMap().value("name").toString();
        done(ok({{"transitionName", name}, {"transitionDuration", s->transitionMs > 0 ? QJsonValue(s->transitionMs) : QJsonValue()}}));
    } else if (type == QLatin1String("SetSceneSceneTransitionOverride")) {
        const Scene *s = remoteScene(d, &why);
        if (!s) return done(why);
        QString transition = s->transition;
        int ms = s->transitionMs;
        if (d.contains("transitionName")) {
            transition.clear();
            if (!d.value("transitionName").isNull()) {
                for (const QVariant &t : Transitions::all(c))
                    if (t.toMap().value("name").toString() == d.value("transitionName").toString()) transition = t.toMap().value("id").toString();
                if (transition.isEmpty()) return done(refuse(RemoteControl::ResourceNotFound, QStringLiteral("No transition by that name.")));
            }
        }
        if (d.contains("transitionDuration")) {
            ms = d.value("transitionDuration").isNull() ? 0 : d.value("transitionDuration").toInt();
            if (ms && (ms < 50 || ms > 20000)) return done(refuse(RemoteControl::RequestFieldOutOfRange, QStringLiteral("transitionDuration is 50 to 20000.")));
        }
        setSceneTransition(s->id, transition, ms);
        done(ok());

    // Inputs
    } else if (type == QLatin1String("GetInputList")) {
        QJsonArray inputs;
        const QString kind = d.value("inputKind").toString();
        for (const RemoteInput &i : remoteInputs())
            if (kind.isEmpty() || i.kind == kind)
                inputs.append(QJsonObject{{"inputName", i.name}, {"inputUuid", i.id}, {"inputKind", i.kind},
                                          {"unversionedInputKind", unversioned(i.kind)}, {"inputKindCaps", i.audio ? 3 : 1}});
        done(ok({{"inputs", inputs}}));
    } else if (type == QLatin1String("GetInputKindList")) {
        QStringList kinds;
        for (const char *k : {"display", "window", "region", "device", "image", "colour", "text", "media", "slideshow", "audio-input", "audio-output", "audio-application", "browser"})
            if (QLatin1String(k) != QLatin1String("browser") || BrowserPage::available())
                kinds << (d.value("unversioned").toBool() ? unversioned(wireKind(k)) : wireKind(k));
        kinds.removeDuplicates();
        done(ok({{"inputKinds", QJsonArray::fromStringList(kinds)}}));
    } else if (type == QLatin1String("GetSpecialInputs")) {
        done(ok({{"desktop1", desktopAdded() ? QJsonValue(tr("Desktop audio")) : QJsonValue()}, {"desktop2", QJsonValue()}, {"mic1", microphoneAdded() ? QJsonValue(tr("Microphone")) : QJsonValue()},
                 {"mic2", QJsonValue()}, {"mic3", QJsonValue()}, {"mic4", QJsonValue()}}));
    } else if (type == QLatin1String("SetInputName")) {
        const auto input = remoteInput(d, &why);
        if (!input) return done(why);
        if (!need("newInputName", &QJsonValue::isString)) return done(why);
        const QString name = d.value("newInputName").toString().trimmed();
        if (name.isEmpty()) return done(refuse(RemoteControl::RequestFieldEmpty, QStringLiteral("newInputName is empty.")));
        if (!c.source(input->id)) return done(refuse(RemoteControl::ResourceNotConfigurable, QStringLiteral("This input keeps its name.")));
        for (const RemoteInput &i : remoteInputs()) if (i.id != input->id && i.name == name) return done(refuse(RemoteControl::ResourceAlreadyExists, QStringLiteral("An input has that name.")));
        renameSource(input->id, name);
        done(ok());
    } else if (type == QLatin1String("GetInputSettings")) {
        const auto input = remoteInput(d, &why);
        if (!input) return done(why);
        const SourceDef *s = c.source(input->id);
        done(ok({{"inputSettings", s ? settingsToHandOut(*s) : QJsonObject()}, {"inputKind", input->kind}}));
    } else if (type == QLatin1String("PressInputPropertiesButton")) {
        // A browser source's one button: load the page again, past the cache.
        const auto input = remoteInput(d, &why);
        if (!input) return done(why);
        if (!need("propertyName", &QJsonValue::isString)) return done(why);
        const SourceDef *s = c.source(input->id);
        if (!s || s->kind != QLatin1String("browser") || d.value("propertyName").toString() != QLatin1String("refreshnocache"))
            return done(refuse(RemoteControl::InvalidResourceState, QStringLiteral("This input has no button called %1.").arg(d.value("propertyName").toString())));
        if (!m_session.reloadBrowser(input->id)) return done(refuse(RemoteControl::ResourceActionFailed, QStringLiteral("The page is not running.")));
        done(ok());
    } else if (type == QLatin1String("SetInputSettings")) {
        const auto input = remoteInput(d, &why);
        if (!input) return done(why);
        if (!need("inputSettings", &QJsonValue::isObject)) return done(why);
        const SourceDef *s = c.source(input->id);
        if (!s) return done(refuse(RemoteControl::ResourceNotConfigurable, QStringLiteral("This input has no settings here.")));
        // Settings it has already, so nothing unknown slips in.
        const QJsonObject settings = d.value("inputSettings").toObject();
        // Whatever the value and whether the source has one, so it says nothing about what is set.
        if (settings.contains(QLatin1String(kPassphraseKey)))
            return done(refuse(RemoteControl::CannotAct, QStringLiteral("A passphrase is set in the app.")));
        QStringList unknown, mistyped, files;
        // Files are chosen here, in the app: a remote client could otherwise
        // show (and read back in a screenshot) any file this account can read.
        // A presentation's deck and picture folder are files too: the deck is
        // read when it is loaded again.
        static const QStringList kFileKeys{"path", "file", "files", "folder", "lut", "matte", "input",
                                           "presentation", "presentationFolder"};
        // A page by web address only; rich text loads what it names by itself.
        static const QRegularExpression kLoads(QStringLiteral("<[^>]*(src|background)|url\\s*\\("), QRegularExpression::CaseInsensitiveOption);
        const auto localPage = [](const QString &address) {
            const QString scheme = QUrl::fromUserInput(address.trimmed()).scheme().toLower();
            return !address.trimmed().isEmpty() && scheme != QLatin1String("http") && scheme != QLatin1String("https");
        };
        const auto filterFiles = [](const QJsonArray &filters) {
            QStringList out;
            for (const QJsonValue &f : filters)
                if (const QString file = f.toObject().value(QStringLiteral("file")).toString(); !file.isEmpty()) out << file;
            return out;
        };
        const QJsonObject current = QJsonObject::fromVariantMap(s->settings);
        for (auto it = settings.begin(); it != settings.end(); ++it) {
            if (!current.contains(it.key())) unknown << it.key();
            else if (current.value(it.key()).type() != it.value().type()) mistyped << it.key();
            else if (current.value(it.key()) != it.value() &&
                     (kFileKeys.contains(it.key()) ||
                      (it.key() == QLatin1String("url") && localPage(it.value().toString())) ||
                      (it.key() == QLatin1String("text") && it.value().toString().contains(kLoads)) ||
                      (it.key() == QLatin1String("videoFilters") &&
                       !std::ranges::all_of(filterFiles(it.value().toArray()), [&](const QString &file) {
                           return filterFiles(current.value(it.key()).toArray()).contains(file);
                       })) ||
                      // A slideshow's list, read as the slideshow reads it: in another
                      // order or with fewer is fine, anything new was not chosen here.
                      (it.key() == QLatin1String("entries") &&
                       !std::ranges::all_of(slideshowEntries({{it.key(), it.value().toVariant()}}), [&](const QVariant &entry) {
                           return slideshowEntries(s->settings).contains(entry);
                       }))))
                files << it.key();
        }
        if (!unknown.isEmpty()) return done(refuse(RemoteControl::InvalidRequestField, QStringLiteral("Unknown settings: %1").arg(unknown.join(", "))));
        if (!mistyped.isEmpty()) return done(refuse(RemoteControl::InvalidRequestFieldType, QStringLiteral("Settings of another type: %1").arg(mistyped.join(", "))));
        if (!files.isEmpty()) return done(refuse(RemoteControl::CannotAct, QStringLiteral("Files are chosen in the app: %1").arg(files.join(", "))));
        const QString id = input->id;
        // One request, one undo step, however many settings it changes.
        m_settingMergeKey = QStringLiteral("remote:%1:%2").arg(id).arg(++m_remoteEditSerial);
        for (auto it = settings.begin(); it != settings.end(); ++it) {
            // Filters go through their own checks, as from the app.
            if (it.key() == QLatin1String("videoFilters")) setVideoFilters(id, it.value().toArray().toVariantList());
            else setSourceSetting(id, it.key(), it.value().toVariant());
        }
        m_settingMergeKey.clear();
        m_production.endGesture();
        done(ok());
    } else if (type == QLatin1String("GetInputMute") || type == QLatin1String("SetInputMute") || type == QLatin1String("ToggleInputMute") ||
               type == QLatin1String("GetInputVolume") || type == QLatin1String("SetInputVolume")) {
        const auto input = remoteInput(d, &why);
        if (!input) return done(why);
        if (!input->audio) return done(refuse(RemoteControl::InvalidResourceState, QStringLiteral("This input has no sound.")));
        if (type == QLatin1String("GetInputMute")) return done(ok({{"inputMuted", remoteMuted(*input)}}));
        if (type == QLatin1String("SetInputMute")) {
            if (!need("inputMuted", &QJsonValue::isBool)) return done(why);
            setRemoteAudio(*input, QStringLiteral("muted"), d.value("inputMuted").toBool());
            return done(ok());
        }
        if (type == QLatin1String("ToggleInputMute")) {
            const bool muted = !remoteMuted(*input);
            setRemoteAudio(*input, QStringLiteral("muted"), muted);
            return done(ok({{"inputMuted", muted}}));
        }
        if (type == QLatin1String("GetInputVolume")) {
            const double mul = remoteVolume(*input);
            return done(ok({{"inputVolumeMul", mul}, {"inputVolumeDb", toDb(mul)}}));
        }
        double mul;
        if (d.value("inputVolumeMul").isDouble()) {
            mul = d.value("inputVolumeMul").toDouble();
            if (mul < 0 || mul > 20) return done(refuse(RemoteControl::RequestFieldOutOfRange, QStringLiteral("inputVolumeMul is 0 to 20.")));
        } else if (d.value("inputVolumeDb").isDouble()) {
            const double db = d.value("inputVolumeDb").toDouble();
            if (db < -100 || db > 26) return done(refuse(RemoteControl::RequestFieldOutOfRange, QStringLiteral("inputVolumeDb is -100 to 26.")));
            mul = db <= -100 ? 0 : std::pow(10.0, db / 20);
        } else {
            return done(refuse(RemoteControl::MissingRequestField, QStringLiteral("inputVolumeMul or inputVolumeDb is needed.")));
        }
        setRemoteAudio(*input, QStringLiteral("volume"), std::min(mul, 20.0));
        done(ok());

    // Transitions
    } else if (type == QLatin1String("GetTransitionKindList")) {
        QJsonArray kinds;
        for (const QVariant &k : Transitions::kinds()) kinds.append(k.toMap().value("id").toString());
        done(ok({{"transitionKinds", kinds}}));
    } else if (type == QLatin1String("GetSceneTransitionList") || type == QLatin1String("GetCurrentSceneTransition")) {
        QJsonArray list;
        QJsonObject current;
        for (const QVariant &t : Transitions::all(c)) {
            const QVariantMap m = t.toMap();
            const QString kind = m.value("settings").toMap().value("kind").toString();
            const bool fixed = kind == QLatin1String("cut") || kind == QLatin1String("stinger");
            const QJsonObject entry{{"transitionName", m.value("name").toString()}, {"transitionUuid", m.value("id").toString()},
                                    {"transitionKind", kind}, {"transitionFixed", fixed}, {"transitionConfigurable", !m.value("builtIn").toBool()}};
            list.append(entry);
            if (m.value("id").toString() == c.currentTransition) {
                current = entry;
                current.insert("transitionDuration", fixed ? QJsonValue() : QJsonValue(c.transitionMs));
                current.insert("transitionSettings", m.value("builtIn").toBool() ? QJsonValue() : QJsonValue(QJsonObject::fromVariantMap(m.value("settings").toMap())));
            }
        }
        if (type == QLatin1String("GetCurrentSceneTransition")) return done(ok(current));
        done(ok({{"currentSceneTransitionName", current.value("transitionName")}, {"currentSceneTransitionUuid", current.value("transitionUuid")},
                 {"currentSceneTransitionKind", current.value("transitionKind")}, {"transitions", list}}));
    } else if (type == QLatin1String("SetCurrentSceneTransition")) {
        if (!need("transitionName", &QJsonValue::isString)) return done(why);
        for (const QVariant &t : Transitions::all(c))
            if (t.toMap().value("name").toString() == d.value("transitionName").toString()) {
                setCurrentTransition(t.toMap().value("id").toString());
                return done(ok());
            }
        done(refuse(RemoteControl::ResourceNotFound, QStringLiteral("No transition by that name.")));
    } else if (type == QLatin1String("SetCurrentSceneTransitionDuration")) {
        if (!need("transitionDuration", &QJsonValue::isDouble)) return done(why);
        const int ms = d.value("transitionDuration").toInt();
        if (ms < 50 || ms > 20000) return done(refuse(RemoteControl::RequestFieldOutOfRange, QStringLiteral("transitionDuration is 50 to 20000.")));
        setTransitionDuration(ms);
        done(ok());
    } else if (type == QLatin1String("GetCurrentSceneTransitionCursor")) {
        done(ok({{"transitionCursor", transitioning() ? m_session.transitionProgress() : 1.0}}));
    } else if (type == QLatin1String("TriggerStudioModeTransition")) {
        if (!studioMode()) return done(refuse(RemoteControl::StudioModeNotActive, QStringLiteral("Studio mode is off.")));
        take();
        done(ok());
    } else if (type == QLatin1String("SetTBarPosition")) {
        if (!studioMode()) return done(refuse(RemoteControl::StudioModeNotActive, QStringLiteral("Studio mode is off.")));
        if (!need("position", &QJsonValue::isDouble)) return done(why);
        const double position = d.value("position").toDouble();
        if (position < 0 || position > 1) return done(refuse(RemoteControl::RequestFieldOutOfRange, QStringLiteral("position is 0 to 1.")));
        setTBar(position);
        if (d.value("release").toBool(true)) releaseTBar();
        done(ok());

    // Filters
    } else if (type == QLatin1String("GetSourceFilterList") || type == QLatin1String("GetSourceFilter") || type == QLatin1String("SetSourceFilterEnabled")) {
        QString shows, target;
        if (!remoteSource(d, &shows, &target, &why)) return done(why);
        const SourceDef *s = c.source(target);
        const QVariantList filters = s ? s->settings.value(QStringLiteral("videoFilters")).toList() : QVariantList();
        auto filterJson = [&](int i) {
            const QVariantMap f = filters[i].toMap();
            QVariantMap settings = f;
            for (const char *k : {"id", "type", "enabled", "name"}) settings.remove(QLatin1String(k));
            return QJsonObject{{"filterName", filterName(f)}, {"filterKind", f.value("type").toString()}, {"filterIndex", i},
                               {"filterEnabled", f.value("enabled", true).toBool()}, {"filterSettings", QJsonObject::fromVariantMap(settings)}};
        };
        if (type == QLatin1String("GetSourceFilterList")) {
            QJsonArray list;
            for (int i = 0; i < filters.size(); ++i) list.append(filterJson(i));
            return done(ok({{"filters", list}}));
        }
        if (!need("filterName", &QJsonValue::isString)) return done(why);
        int at = -1;
        for (int i = 0; i < filters.size() && at < 0; ++i) if (filterName(filters[i].toMap()) == d.value("filterName").toString()) at = i;
        if (at < 0) return done(refuse(RemoteControl::ResourceNotFound, QStringLiteral("No filter by that name.")));
        if (type == QLatin1String("GetSourceFilter")) {
            QJsonObject f = filterJson(at);
            f.remove("filterName");
            return done(ok(f));
        }
        if (!need("filterEnabled", &QJsonValue::isBool)) return done(why);
        QVariantList changed = filters;
        QVariantMap f = changed[at].toMap();
        f.insert(QStringLiteral("enabled"), d.value("filterEnabled").toBool());
        changed[at] = f;
        setVideoFilters(target, changed);
        done(ok());

    // Scene items
    } else if (type == QLatin1String("GetSceneItemList") || type == QLatin1String("GetGroupSceneItemList")) {
        const bool group = type == QLatin1String("GetGroupSceneItemList");
        const Scene *s = remoteScene(d, &why, group);
        if (!s) return done(why);
        if (group && !s->group) return done(refuse(RemoteControl::InvalidResourceType, QStringLiteral("That is not a group.")));
        QJsonArray items;
        for (int i = 0; i < s->items.size(); ++i) items.append(remoteItemJson(*s, i));
        done(ok({{"sceneItems", items}}));
    } else if (type == QLatin1String("GetSceneItemId")) {
        const Scene *s = remoteScene(d, &why, true);
        if (!s) return done(why);
        if (!need("sourceName", &QJsonValue::isString)) return done(why);
        const int offset = d.value("searchOffset").toInt(0);
        QList<int> matches;  // top first
        for (int i = s->items.size() - 1; i >= 0; --i) {
            const SourceDef *source = c.source(s->items[i].source);
            if (source && source->name == d.value("sourceName").toString()) matches.append(i);
        }
        const int pick = offset == -1 ? int(matches.size()) - 1 : offset;
        if (offset < -1 || pick < 0 || pick >= matches.size()) return done(refuse(RemoteControl::ResourceNotFound, QStringLiteral("No such item in that scene.")));
        done(ok({{"sceneItemId", remoteItemNumber(s->items[matches[pick]].id)}}));
    } else if (type.startsWith(QLatin1String("GetSceneItem")) || type == QLatin1String("SetSceneItemEnabled") || type == QLatin1String("SetSceneItemLocked")) {
        const Scene *s = remoteScene(d, &why, true);
        if (!s) return done(why);
        if (!need("sceneItemId", &QJsonValue::isDouble)) return done(why);
        int index = -1;
        const SceneItem *item = remoteItem(*s, d.value("sceneItemId").toInt(), &index);
        if (!item) return done(refuse(RemoteControl::ResourceNotFound, QStringLiteral("No item with that id in that scene.")));
        const QJsonObject json = remoteItemJson(*s, index);
        if (type == QLatin1String("GetSceneItemSource")) return done(ok({{"sourceName", json.value("sourceName")}, {"sourceUuid", json.value("sourceUuid")}}));
        if (type == QLatin1String("GetSceneItemEnabled")) return done(ok({{"sceneItemEnabled", item->visible}}));
        if (type == QLatin1String("GetSceneItemLocked")) return done(ok({{"sceneItemLocked", item->locked}}));
        if (type == QLatin1String("GetSceneItemIndex")) return done(ok({{"sceneItemIndex", index}}));
        if (type == QLatin1String("GetSceneItemTransform")) return done(ok({{"sceneItemTransform", json.value("sceneItemTransform")}}));
        if (type == QLatin1String("SetSceneItemEnabled")) {
            if (!need("sceneItemEnabled", &QJsonValue::isBool)) return done(why);
            setItemVisible(item->id, d.value("sceneItemEnabled").toBool());
            return done(ok());
        }
        if (type == QLatin1String("SetSceneItemLocked")) {
            if (!need("sceneItemLocked", &QJsonValue::isBool)) return done(why);
            setItemLocked(item->id, d.value("sceneItemLocked").toBool());
            return done(ok());
        }
        done(refuse(RemoteControl::UnknownRequestType, QStringLiteral("Unknown request type: %1").arg(type)));

    // Outputs
    } else if (type == QLatin1String("GetVirtualCamStatus") || type == QLatin1String("GetReplayBufferStatus")) {
        done(ok({{"outputActive", type == QLatin1String("GetVirtualCamStatus") ? virtualCamera() : replayActive()}}));
    } else if (type == QLatin1String("ToggleVirtualCam") || type == QLatin1String("StartVirtualCam") || type == QLatin1String("StopVirtualCam")) {
        const bool on = virtualCamera();
        if (type == QLatin1String("StartVirtualCam") && on) return done(refuse(RemoteControl::OutputRunning, QStringLiteral("The virtual camera is on.")));
        if (type == QLatin1String("StopVirtualCam") && !on) return done(refuse(RemoteControl::OutputNotRunning, QStringLiteral("The virtual camera is off.")));
        toggleVirtualCamera();
        // A start that did not happen says why, not success.
        if (!on && !virtualCamera()) return done(refuse(RemoteControl::ResourceActionFailed, status()));
        done(type == QLatin1String("ToggleVirtualCam") ? ok({{"outputActive", virtualCamera()}}) : ok());
    } else if (type == QLatin1String("ToggleReplayBuffer") || type == QLatin1String("StartReplayBuffer") || type == QLatin1String("StopReplayBuffer")) {
        const bool on = replayActive();
        if (type == QLatin1String("StartReplayBuffer") && on) return done(refuse(RemoteControl::OutputRunning, QStringLiteral("The replay buffer is running.")));
        if (type == QLatin1String("StopReplayBuffer") && !on) return done(refuse(RemoteControl::OutputNotRunning, QStringLiteral("The replay buffer is not running.")));
        toggleReplay();
        if (!on && !replayActive()) return done(refuse(RemoteControl::ResourceActionFailed, status()));
        done(type == QLatin1String("ToggleReplayBuffer") ? ok({{"outputActive", replayActive()}}) : ok());
    } else if (type == QLatin1String("SaveReplayBuffer")) {
        if (!replayActive()) return done(refuse(RemoteControl::OutputNotRunning, QStringLiteral("The replay buffer is not running.")));
        saveReplay();
        done(ok());
    } else if (type == QLatin1String("GetLastReplayBufferReplay")) {
        if (!replayActive()) return done(refuse(RemoteControl::OutputNotRunning, QStringLiteral("The replay buffer is not running.")));
        done(ok({{"savedReplayPath", m_session.lastReplayPath()}}));
    } else if (type == QLatin1String("GetOutputList")) {
        auto output = [](const char *name, const char *kind, bool active) {
            return QJsonObject{{"outputName", name}, {"outputKind", kind}, {"outputActive", active}, {"outputWidth", 0}, {"outputHeight", 0}, {"outputFlags", QJsonObject()}};
        };
        done(ok({{"outputs", QJsonArray{output("simple_stream", "rtmp_output", streaming()), output("simple_file_output", "ffmpeg_muxer", recording()),
                                        output("Replay Buffer", "replay_buffer", replayActive()), output("virtualcam_output", "virtualcam_output", virtualCamera())}}}));
    } else if (type == QLatin1String("GetStreamStatus")) {
        bool reconnecting = false;
        quint64 sent = 0, dropped = 0;
        double kbps = 0;
        for (const Session::StreamStatus &s : m_session.streamStatus()) {
            reconnecting |= s.state == QLatin1String("reconnecting");
            sent += s.framesSent; dropped += s.framesDropped; kbps += s.kbps;
        }
        const qint64 ms = streaming() && m_streamClock.isValid() ? m_streamClock.elapsed() : 0;
        done(ok({{"outputActive", streaming()}, {"outputReconnecting", reconnecting}, {"outputTimecode", timecode(ms)},
                 {"outputDuration", double(ms)}, {"outputCongestion", 0}, {"outputBytes", kbps * 125 * (ms / 1000.0)},
                 {"outputSkippedFrames", double(dropped)}, {"outputTotalFrames", double(sent + dropped)}}));
    } else if (type == QLatin1String("ToggleStream") || type == QLatin1String("StartStream") || type == QLatin1String("StopStream")) {
        const bool on = streaming();
        if (type == QLatin1String("StartStream") && on) return done(refuse(RemoteControl::OutputRunning, QStringLiteral("Already live.")));
        if (type == QLatin1String("StopStream") && !on) return done(refuse(RemoteControl::OutputNotRunning, QStringLiteral("Not live.")));
        const auto answer = [this, type, done] {
            done(type == QLatin1String("ToggleStream") ? ok({{"outputActive", streaming() && !m_session.streamingStopping()}}) : ok());
        };
        if (on) {
            toggleStreaming();
            return answer();
        }
        // Answered once it has gone live or failed: the keyring may be waiting for its prompt.
        goLive([this, answer, done] {
            if (!streaming()) return done(refuse(RemoteControl::ResourceActionFailed, status()));
            answer();
        });
    } else if (type == QLatin1String("GetRecordStatus")) {
        const Session::Stats s = m_session.stats();
        const qint64 ms = recording() ? s.recorder.durationNs / 1000000 : 0;
        done(ok({{"outputActive", recording()}, {"outputPaused", recording() && m_session.recordingPaused()}, {"outputTimecode", timecode(ms)},
                 {"outputDuration", double(ms)}, {"outputBytes", double(recording() ? s.recorder.bytesWritten : 0)}}));
    } else if (type == QLatin1String("ToggleRecord") || type == QLatin1String("StartRecord") || type == QLatin1String("StopRecord")) {
        const bool on = recording();
        if (type == QLatin1String("StartRecord") && on) return done(refuse(RemoteControl::OutputRunning, QStringLiteral("Already recording.")));
        if (type == QLatin1String("StopRecord") && !on) return done(refuse(RemoteControl::OutputNotRunning, QStringLiteral("Not recording.")));
        const QString path = m_session.stats().recordingPath;
        toggleRecording();
        if (!on && !recording()) return done(refuse(RemoteControl::ResourceActionFailed, status()));
        if (type == QLatin1String("ToggleRecord")) return done(ok({{"outputActive", recording() && !recordingStopping()}}));
        done(type == QLatin1String("StopRecord") ? ok({{"outputPath", path}}) : ok());
    } else if (type == QLatin1String("ToggleRecordPause") || type == QLatin1String("PauseRecord") || type == QLatin1String("ResumeRecord")) {
        if (!recording()) return done(refuse(RemoteControl::OutputNotRunning, QStringLiteral("Not recording.")));
        const bool paused = m_session.recordingPaused();
        if (type == QLatin1String("PauseRecord") && paused) return done(refuse(RemoteControl::OutputPaused, QStringLiteral("Already paused.")));
        if (type == QLatin1String("ResumeRecord") && !paused) return done(refuse(RemoteControl::OutputNotPaused, QStringLiteral("Not paused.")));
        done(setRecordingPaused(!paused) ? ok() : refuse(RemoteControl::CannotAct, QStringLiteral("This recording cannot pause.")));
    } else if (type == QLatin1String("SplitRecordFile") || type == QLatin1String("CreateRecordChapter")) {
        if (!recording()) return done(refuse(RemoteControl::OutputNotRunning, QStringLiteral("Not recording.")));
        if (type == QLatin1String("SplitRecordFile")) splitRecording();
        else addChapter(d.value("chapterName").toString());
        done(ok());

    // Media
    } else if (type == QLatin1String("GetMediaInputStatus") || type == QLatin1String("SetMediaInputCursor") ||
               type == QLatin1String("OffsetMediaInputCursor") || type == QLatin1String("TriggerMediaInputAction")) {
        const auto input = remoteInput(d, &why);
        if (!input) return done(why);
        const SourceDef *s = c.source(input->id);
        const bool media = s && s->kind == QLatin1String("media"), slides = s && s->kind == QLatin1String("slideshow");
        if (!media && !(slides && type == QLatin1String("TriggerMediaInputAction")))
            return done(refuse(RemoteControl::InvalidInputKind, QStringLiteral("That input is not a media input.")));
        const QVariantMap playback = media ? m_session.mediaPlayback(s->id) : QVariantMap();
        const bool playing = !playback.isEmpty() && playback.value("opened").toBool();
        if (type == QLatin1String("GetMediaInputStatus"))
            return done(ok({{"mediaState", mediaState(playback)},
                            {"mediaDuration", playing ? QJsonValue(playback.value("duration").toDouble() * 1000) : QJsonValue()},
                            {"mediaCursor", playing ? QJsonValue(playback.value("position").toDouble() * 1000) : QJsonValue()}}));
        if (type == QLatin1String("TriggerMediaInputAction")) {
            if (!need("mediaAction", &QJsonValue::isString)) return done(why);
            const QString action = d.value("mediaAction").toString().section(QLatin1Char('_'), -1);
            static const QHash<QString, QPair<QString, double>> mediaActions{
                {"PLAY", {"pause", 0}}, {"PAUSE", {"pause", 1}}, {"STOP", {"stop", 0}}, {"RESTART", {"restart", 0}}};
            if (action == QLatin1String("NONE")) return done(ok());
            if (slides) {
                if (action == QLatin1String("NEXT") || action == QLatin1String("PREVIOUS")) controlSlideshow(s->id, action.toLower());
                else if (action == QLatin1String("PLAY") || action == QLatin1String("PAUSE")) controlSlideshow(s->id, QStringLiteral("pause"), action == QLatin1String("PAUSE"));
                else if (action == QLatin1String("RESTART")) controlSlideshow(s->id, QStringLiteral("restart"));
                else return done(refuse(RemoteControl::InvalidRequestField, QStringLiteral("Unknown mediaAction.")));
                return done(ok());
            }
            if (!mediaActions.contains(action)) return done(refuse(RemoteControl::InvalidRequestField, QStringLiteral("Unknown mediaAction.")));
            controlMedia(s->id, mediaActions[action].first, mediaActions[action].second);
            emitRemoteEvent(QStringLiteral("MediaInputActionTriggered"), RemoteControl::MediaInputs,
                            {{"inputName", s->name}, {"inputUuid", s->id}, {"mediaAction", d.value("mediaAction").toString()}});
            return done(ok());
        }
        if (!playing) return done(refuse(RemoteControl::InvalidResourceState, QStringLiteral("The media is not playing.")));
        const bool absolute = type == QLatin1String("SetMediaInputCursor");
        const char *field = absolute ? "mediaCursor" : "mediaCursorOffset";
        if (!need(field, &QJsonValue::isDouble)) return done(why);
        const double ms = d.value(QLatin1String(field)).toDouble();
        if (absolute && ms < 0) return done(refuse(RemoteControl::RequestFieldOutOfRange, QStringLiteral("mediaCursor is 0 or more.")));
        const double at = absolute ? ms / 1000 : playback.value("position").toDouble() + ms / 1000;
        controlMedia(s->id, QStringLiteral("seek"), std::clamp(at, 0.0, playback.value("duration").toDouble()));
        done(ok());

    // Ui
    } else if (type == QLatin1String("GetStudioModeEnabled")) {
        done(ok({{"studioModeEnabled", studioMode()}}));
    } else if (type == QLatin1String("SetStudioModeEnabled")) {
        if (!need("studioModeEnabled", &QJsonValue::isBool)) return done(why);
        setStudioMode(d.value("studioModeEnabled").toBool());
        done(ok());
    } else if (type == QLatin1String("GetMonitorList")) {
        QJsonArray monitors;
        const auto screens = QGuiApplication::screens();
        for (int i = 0; i < screens.size(); ++i) {
            const QRect g = screens[i]->geometry();
            monitors.append(QJsonObject{{"monitorName", screens[i]->name()}, {"monitorIndex", i}, {"monitorWidth", g.width()},
                                        {"monitorHeight", g.height()}, {"monitorPositionX", g.x()}, {"monitorPositionY", g.y()}});
        }
        done(ok({{"monitors", monitors}}));
    } else if (type == QLatin1String("OpenVideoMixProjector") || type == QLatin1String("OpenSourceProjector")) {
        const int screen = d.value("monitorIndex").toInt(-1);
        if (screen >= QGuiApplication::screens().size()) return done(refuse(RemoteControl::RequestFieldOutOfRange, QStringLiteral("No monitor with that index.")));
        if (type == QLatin1String("OpenVideoMixProjector")) {
            if (!need("videoMixType", &QJsonValue::isString)) return done(why);
            const QString mix = d.value("videoMixType").toString().section(QLatin1Char('_'), -1).toLower();
            if (mix != QLatin1String("preview") && mix != QLatin1String("program") && mix != QLatin1String("multiview"))
                return done(refuse(RemoteControl::InvalidRequestField, QStringLiteral("Unknown videoMixType.")));
            openProjector(mix, {}, screen);
        } else {
            QString shows, target;
            if (!remoteSource(d, &shows, &target, &why)) return done(why);
            openProjector(shows, target, screen);
        }
        done(ok());
    } else {
        done(refuse(RemoteControl::UnknownRequestType, QStringLiteral("Unknown request type: %1").arg(type)));
    }
}

void Backend::remoteScreenshot(const QString &id, const QImage &image) {
    const RemoteShot shot = m_remoteShots.take(id);
    if (image.isNull()) { shot.done(refuse(RemoteControl::RequestProcessingFailed, QStringLiteral("There is no picture yet."))); return; }
    QImage scaled = image;
    if (shot.width > 0 && shot.height > 0) scaled = image.scaled(shot.width, shot.height, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    else if (shot.width > 0) scaled = image.scaledToWidth(shot.width, Qt::SmoothTransformation);
    else if (shot.height > 0) scaled = image.scaledToHeight(shot.height, Qt::SmoothTransformation);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    const bool opaque = shot.format == "jpg" || shot.format == "jpeg" || shot.format == "bmp";
    if (!(opaque ? scaled.convertToFormat(QImage::Format_RGB32) : scaled).save(&buffer, shot.format.constData(), shot.quality)) {
        shot.done(refuse(RemoteControl::RequestProcessingFailed, QStringLiteral("The picture could not be encoded.")));
        return;
    }
    const QString mime = shot.format == "jpg" ? QStringLiteral("jpeg") : QString::fromLatin1(shot.format);
    shot.done(ok({{"imageData", QStringLiteral("data:image/%1;base64,").arg(mime) + QString::fromLatin1(bytes.toBase64())}}));
}

// ---- Events

void Backend::emitRemoteEvent(const QString &type, int intent, const QJsonObject &data) {
    m_remote.emitEvent(type, intent, data);
}

Backend::RemoteSeen Backend::remoteSnapshot() {
    const Collection &c = m_production.collection();
    RemoteSeen s;
    s.collectionFile = m_session.profile().collection;
    s.collection = collectionName();
    s.profile = profileName();
    // The names of the others are in their files, which are read only when
    // the lists have changed: this runs twice a second with a client signed in.
    if (m_remoteNamesStale || s.collection != m_remoteSeen.collection || s.profile != m_remoteSeen.profile) {
        m_remoteCollectionNames.clear();
        m_remoteProfileNames.clear();
        for (const QVariant &v : collections()) m_remoteCollectionNames << v.toMap().value("name").toString();
        for (const QVariant &v : profiles()) m_remoteProfileNames << v.toMap().value("name").toString();
        m_remoteNamesStale = false;
    }
    s.collections = m_remoteCollectionNames;
    s.profiles = m_remoteProfileNames;
    for (const Scene &scene : c.scenes) {
        if (!scene.group) s.scenes.append({scene.id, scene.name});
        for (int i = 0; i < scene.items.size(); ++i) {
            const SceneItem &item = scene.items[i];
            s.items.insert(item.id, {{"scene", scene.id}, {"source", item.source}, {"visible", item.visible}, {"locked", item.locked}, {"index", i}});
        }
    }
    for (const RemoteInput &i : remoteInputs()) {
        s.inputs.insert(i.id, {{"name", i.name}, {"kind", i.kind}, {"audio", i.audio}, {"muted", i.audio && remoteMuted(i)}, {"volume", i.audio ? remoteVolume(i) : 1.0}});
        const SourceDef *source = c.source(i.id);
        if (!source) continue;
        const QVariantList filters = source->settings.value(QStringLiteral("videoFilters")).toList();
        for (const QVariant &f : filters) {
            const QVariantMap filter = f.toMap();
            s.filters.insert(source->id + QLatin1Char('/') + filter.value("id").toString(),
                             {{"source", source->name}, {"name", filterName(filter)}, {"enabled", filter.value("enabled", true).toBool()}});
        }
        if (source->kind == QLatin1String("media")) s.media.insert(source->id, m_session.mediaPlayback(source->id).value("state").toString());
    }
    s.program = programScene();
    s.studioMode = studioMode();
    s.preview = s.studioMode ? currentScene() : QString();
    s.transitioning = transitioning();
    s.transitionTarget = m_session.transitionTarget();
    s.transition = c.currentTransition;
    s.transitionMs = c.transitionMs;
    s.recording = recording();
    s.paused = s.recording && m_session.recordingPaused();
    s.recordingPath = s.recording ? m_session.stats().recordingPath : m_remoteSeen.recordingPath;
    s.streaming = streaming();
    s.replay = replayActive();
    s.camera = virtualCamera();
    return s;
}

void Backend::remoteDiff() {
    if (!m_remote.wants(~0)) return;
    RemoteSeen now = remoteSnapshot();
    const RemoteSeen was = std::exchange(m_remoteSeen, now);
    const Collection &c = m_production.collection();
    using R = RemoteControl;
    auto send = [this](const char *type, int intent, const QJsonObject &data = {}) { m_remote.emitEvent(QLatin1String(type), intent, data); };
    auto sceneName = [&](const QString &id) { const Scene *s = c.scene(id); return s ? s->name : QString(); };

    // Another collection: everything in it is new, so only that is said.
    const bool otherCollection = now.collectionFile != was.collectionFile;
    if (otherCollection) {
        send("CurrentSceneCollectionChanging", R::Config, {{"sceneCollectionName", was.collection}});
        send("CurrentSceneCollectionChanged", R::Config, {{"sceneCollectionName", now.collection}});
    }
    if (now.collections != was.collections) send("SceneCollectionListChanged", R::Config, {{"sceneCollections", QJsonArray::fromStringList(now.collections)}});
    if (now.profile != was.profile) {
        send("CurrentProfileChanging", R::Config, {{"profileName", was.profile}});
        send("CurrentProfileChanged", R::Config, {{"profileName", now.profile}});
    }
    if (now.profiles != was.profiles) send("ProfileListChanged", R::Config, {{"profiles", QJsonArray::fromStringList(now.profiles)}});

    if (!otherCollection) {
        QHash<QString, QString> wasScenes, nowScenes;
        for (const auto &[id, name] : was.scenes) wasScenes.insert(id, name);
        for (const auto &[id, name] : now.scenes) nowScenes.insert(id, name);
        for (const auto &[id, name] : was.scenes)
            if (!nowScenes.contains(id)) send("SceneRemoved", R::Scenes, {{"sceneName", name}, {"sceneUuid", id}, {"isGroup", false}});
        for (const auto &[id, name] : now.scenes) {
            if (!wasScenes.contains(id)) send("SceneCreated", R::Scenes, {{"sceneName", name}, {"sceneUuid", id}, {"isGroup", false}});
            else if (wasScenes[id] != name) send("SceneNameChanged", R::Scenes, {{"sceneUuid", id}, {"oldSceneName", wasScenes[id]}, {"sceneName", name}});
        }
        // Inputs
        for (auto it = was.inputs.begin(); it != was.inputs.end(); ++it)
            if (!now.inputs.contains(it.key())) send("InputRemoved", R::Inputs, {{"inputName", it->value("name").toString()}, {"inputUuid", it.key()}});
        for (auto it = now.inputs.begin(); it != now.inputs.end(); ++it) {
            const QString name = it->value("name").toString();
            const QJsonObject who{{"inputName", name}, {"inputUuid", it.key()}};
            if (!was.inputs.contains(it.key())) {
                const QString kind = it->value("kind").toString();
                const SourceDef *s = c.source(it.key());
                send("InputCreated", R::Inputs, {{"inputName", name}, {"inputUuid", it.key()}, {"inputKind", kind}, {"unversionedInputKind", unversioned(kind)},
                                                 {"inputKindCaps", it->value("audio").toBool() ? 3 : 1},
                                                 {"inputSettings", s ? settingsToHandOut(*s) : QJsonObject()}, {"defaultInputSettings", QJsonObject()}});
                continue;
            }
            const QVariantMap old = was.inputs.value(it.key());
            if (old.value("name") != name) send("InputNameChanged", R::Inputs, {{"inputUuid", it.key()}, {"oldInputName", old.value("name").toString()}, {"inputName", name}});
            if (old.value("muted") != it->value("muted")) {
                QJsonObject e = who; e.insert("inputMuted", it->value("muted").toBool());
                send("InputMuteStateChanged", R::Inputs, e);
            }
            if (!qFuzzyCompare(old.value("volume").toDouble() + 1, it->value("volume").toDouble() + 1)) {
                QJsonObject e = who;
                e.insert("inputVolumeMul", it->value("volume").toDouble());
                e.insert("inputVolumeDb", toDb(it->value("volume").toDouble()));
                send("InputVolumeChanged", R::Inputs, e);
            }
            const QString wasMedia = was.media.value(it.key()), nowMedia = now.media.value(it.key());
            if (wasMedia != nowMedia && nowMedia == QLatin1String("playing") && wasMedia != QLatin1String("paused")) send("MediaInputPlaybackStarted", R::MediaInputs, who);
            if (wasMedia != nowMedia && nowMedia == QLatin1String("ended")) send("MediaInputPlaybackEnded", R::MediaInputs, who);
        }
        // Filters
        for (auto it = now.filters.begin(); it != now.filters.end(); ++it) {
            const auto old = was.filters.find(it.key());
            if (old == was.filters.end()) continue;
            if (old->value("enabled") != it->value("enabled"))
                send("SourceFilterEnableStateChanged", R::Filters, {{"sourceName", it->value("source").toString()}, {"filterName", it->value("name").toString()},
                                                                    {"filterEnabled", it->value("enabled").toBool()}});
            if (old->value("name") != it->value("name"))
                send("SourceFilterNameChanged", R::Filters, {{"sourceName", it->value("source").toString()}, {"oldFilterName", old->value("name").toString()},
                                                             {"filterName", it->value("name").toString()}});
        }
        // Scene items
        QSet<QString> reindexed;
        auto itemEvent = [&](const QString &id, const QVariantMap &item) {
            const QString scene = item.value("scene").toString();
            const SourceDef *s = c.source(item.value("source").toString());
            return QJsonObject{{"sceneName", sceneName(scene)}, {"sceneUuid", scene}, {"sceneItemId", remoteItemNumber(id)},
                               {"sourceName", s ? s->name : QString()}, {"sourceUuid", item.value("source").toString()}};
        };
        for (auto it = was.items.begin(); it != was.items.end(); ++it)
            if (!now.items.contains(it.key())) {
                if (!c.scene(it->value("scene").toString())) continue;  // went with its scene
                send("SceneItemRemoved", R::SceneItems, itemEvent(it.key(), *it));
            }
        for (auto it = now.items.begin(); it != now.items.end(); ++it) {
            QJsonObject e = itemEvent(it.key(), *it);
            const auto old = was.items.find(it.key());
            if (old == was.items.end()) {
                e.insert("sceneItemIndex", it->value("index").toInt());
                send("SceneItemCreated", R::SceneItems, e);
                continue;
            }
            e.remove("sourceName");
            e.remove("sourceUuid");
            if (old->value("visible") != it->value("visible")) { QJsonObject v = e; v.insert("sceneItemEnabled", it->value("visible").toBool()); send("SceneItemEnableStateChanged", R::SceneItems, v); }
            if (old->value("locked") != it->value("locked")) { QJsonObject v = e; v.insert("sceneItemLocked", it->value("locked").toBool()); send("SceneItemLockStateChanged", R::SceneItems, v); }
            if (old->value("index") != it->value("index") && old->value("scene") == it->value("scene")) reindexed.insert(it->value("scene").toString());
        }
        for (const QString &id : reindexed) {
            const Scene *scene = c.scene(id);
            if (!scene) continue;
            QJsonArray items;
            for (int i = 0; i < scene->items.size(); ++i) items.append(QJsonObject{{"sceneItemId", remoteItemNumber(scene->items[i].id)}, {"sceneItemIndex", i}});
            send("SceneItemListReindexed", R::SceneItems, {{"sceneName", scene->name}, {"sceneUuid", id}, {"sceneItems", items}});
        }
    }
    QList<QPair<QString, QString>> wasOrder = was.scenes, nowOrder = now.scenes;
    if (otherCollection || wasOrder != nowOrder) {
        QJsonArray scenes;
        for (int i = 0; i < now.scenes.size(); ++i) {
            const auto &[id, name] = now.scenes[now.scenes.size() - 1 - i];
            scenes.append(QJsonObject{{"sceneIndex", i}, {"sceneName", name}, {"sceneUuid", id}});
        }
        send("SceneListChanged", R::Scenes, {{"scenes", scenes}});
    }
    if (now.studioMode != was.studioMode) send("StudioModeStateChanged", R::Ui, {{"studioModeEnabled", now.studioMode}});
    if (now.program != was.program) send("CurrentProgramSceneChanged", R::Scenes, {{"sceneName", sceneName(now.program)}, {"sceneUuid", now.program}});
    if (now.preview != was.preview && !now.preview.isEmpty()) send("CurrentPreviewSceneChanged", R::Scenes, {{"sceneName", sceneName(now.preview)}, {"sceneUuid", now.preview}});

    QString transitionName;
    for (const QVariant &t : Transitions::all(c)) if (t.toMap().value("id").toString() == now.transition) transitionName = t.toMap().value("name").toString();
    const QJsonObject transition{{"transitionName", transitionName}, {"transitionUuid", now.transition}};
    if (now.transition != was.transition) send("CurrentSceneTransitionChanged", R::Transitions, transition);
    if (now.transitionMs != was.transitionMs) send("CurrentSceneTransitionDurationChanged", R::Transitions, {{"transitionDuration", now.transitionMs}});
    // One interrupted by another starts again, towards the new studio.
    if (now.transitioning && (!was.transitioning || now.transitionTarget != was.transitionTarget)) send("SceneTransitionStarted", R::Transitions, transition);
    if (!now.transitioning && was.transitioning) {
        send("SceneTransitionEnded", R::Transitions, transition);
        send("SceneTransitionVideoEnded", R::Transitions, transition);
    }

    if (now.streaming != was.streaming) send("StreamStateChanged", R::Outputs, {{"outputActive", now.streaming}, {"outputState", outputState(now.streaming)}});
    if (now.recording != was.recording)
        send("RecordStateChanged", R::Outputs, {{"outputActive", now.recording}, {"outputState", outputState(now.recording)},
                                                {"outputPath", now.recording ? QJsonValue() : QJsonValue(was.recordingPath)}});
    else if (now.recording && now.paused != was.paused)
        send("RecordStateChanged", R::Outputs, {{"outputActive", true}, {"outputPath", QJsonValue()},
                                                {"outputState", now.paused ? QStringLiteral("OBS_WEBSOCKET_OUTPUT_PAUSED") : QStringLiteral("OBS_WEBSOCKET_OUTPUT_RESUMED")}});
    else if (now.recording && now.recordingPath != was.recordingPath && !was.recordingPath.isEmpty())
        send("RecordFileChanged", R::Outputs, {{"newOutputPath", now.recordingPath}});
    if (now.replay != was.replay) send("ReplayBufferStateChanged", R::Outputs, {{"outputActive", now.replay}, {"outputState", outputState(now.replay)}});
    if (now.camera != was.camera) send("VirtualcamStateChanged", R::Outputs, {{"outputActive", now.camera}, {"outputState", outputState(now.camera)}});
}

void Backend::remoteMeters() {
    if (!m_remote.wants(RemoteControl::InputVolumeMeters)) return;
    QJsonArray inputs;
    auto levels = [](const PeakMeter::Reading &r) {
        QJsonArray channels;
        for (float l : r.levels) channels.append(QJsonArray{l, l, l});
        return channels;
    };
    for (const RemoteInput &i : remoteInputs()) {
        if (!i.audio) continue;
        PeakMeter::Reading r;
        if (i.id == QLatin1String("desktop")) r = m_session.globalAudioMeter(0);
        else if (i.id == QLatin1String("microphone")) r = m_session.globalAudioMeter(1);
        else if (m_production.collection().source(i.id)) r = m_session.sourceAudioMeter(i.id);
        else r = m_session.globalAudioDeviceMeter(i.id);
        inputs.append(QJsonObject{{"inputName", i.name}, {"inputUuid", i.id}, {"inputLevelsMul", levels(r)}});
    }
    m_remote.emitEvent(QStringLiteral("InputVolumeMeters"), RemoteControl::InputVolumeMeters, {{"inputs", inputs}});
}

void Backend::setUpRemote() {
    m_remote.setProduct(QStringLiteral("OmaBroadcast"));
    m_remoteUptime.start();
    m_remoteDiff.setSingleShot(true);
    m_remoteDiff.setInterval(0);
    connect(&m_remoteDiff, &QTimer::timeout, this, &Backend::remoteDiff);
    // Only with someone signed in to tell: a snapshot reads every collection
    // and profile file. One who signs in starts from a fresh one.
    const auto later = [this] { if (m_remote.listening() && m_remote.wants(~0)) m_remoteDiff.start(); };
    connect(&m_production, &Production::changed, this, later);
    connect(&m_session, &Session::transitionChanged, this, later);
    connect(&m_session, &Session::studioModeChanged, this, later);
    for (auto signal : {&Backend::recordingChanged, &Backend::streamingChanged, &Backend::replayChanged, &Backend::virtualCameraChanged,
                        &Backend::settingsChanged, &Backend::globalAudioDevicesChanged, &Backend::profilesChanged,
                        &Backend::collectionsChanged, &Backend::mediaPlaybackChanged, &Backend::scenesChanged, &Backend::itemsChanged,
                        &Backend::statsChanged})
        connect(this, signal, this, later);
    connect(this, &Backend::collectionsChanged, this, [this] { m_remoteNamesStale = true; });
    connect(this, &Backend::profilesChanged, this, [this] { m_remoteNamesStale = true; });
    connect(&m_remote, &RemoteControl::clientsChanged, this, [this] {
        m_remoteNamesStale = true;
        m_remoteSeen = remoteSnapshot();
        emit remoteControlChanged();
    });
    connect(&m_session, &Session::replaySaved, this, [this](const QString &path) {
        m_remote.emitEvent(QStringLiteral("ReplayBufferSaved"), RemoteControl::Outputs, {{"savedReplayPath", path}});
    });
    connect(this, &Backend::screenshotSaved, this, [this](const QString &path) {
        m_remote.emitEvent(QStringLiteral("ScreenshotSaved"), RemoteControl::Ui, {{"savedScreenshotPath", path}});
    });
    connect(&m_session, &Session::snapshotReady, this, [this](const QString &id, const QImage &image) {
        if (m_remoteShots.contains(id)) remoteScreenshot(id, image);
    });
    connect(this, &Backend::levelsChanged, this, &Backend::remoteMeters);
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { m_remote.emitEvent(QStringLiteral("ExitStarted"), RemoteControl::General); });
}
