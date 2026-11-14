// Outputs as the window shows them: the recording's format, rate control and
// splitting; pause, split and chapters while recording; stream targets with
// their keys in the keyring, and going live.

#include "backend.h"

#include <QFileInfo>
#include <QUuid>

#include "output/file_sink.h"
#include "output/services.h"

namespace {
const QStringList kRateControls{QStringLiteral("quality"), QStringLiteral("cbr"), QStringLiteral("vbr"),
                                QStringLiteral("cqp"), QStringLiteral("lossless")};
const QStringList kAudioCodecs{QStringLiteral("aac"), QStringLiteral("opus"), QStringLiteral("flac"), QStringLiteral("alac"),
                               QStringLiteral("pcm16"), QStringLiteral("pcm24"), QStringLiteral("pcm32f")};

StreamTarget targetFrom(const QVariantMap &m, StreamTarget t) {
    auto text = [&](const char *key, QString &into) { if (m.contains(key)) into = m.value(key).toString().trimmed(); };
    auto number = [&](const char *key, int &into, int low, int high) {
        if (m.contains(key)) into = std::clamp(m.value(key).toInt(), low, high);
    };
    text("name", t.name);
    text("service", t.service);
    text("server", t.server);
    text("encoder", t.encoder);
    text("bindAddress", t.bindAddress);
    text("username", t.username);
    if (m.contains("enforce")) t.enforce = m.value("enforce").toBool();
    if (m.contains("enabled")) t.enabled = m.value("enabled").toBool();
    if (m.contains("reconnect")) t.reconnect = m.value("reconnect").toBool();
    if (m.contains("dynamicBitrate")) t.dynamicBitrate = m.value("dynamicBitrate").toBool();
    if (m.contains("preserveDelay")) t.preserveDelay = m.value("preserveDelay").toBool();
    if (m.contains("lowLatency")) t.lowLatency = m.value("lowLatency").toBool();
    if (m.contains("muxer")) t.muxer = m.value("muxer").toString().trimmed();
    if (m.contains("muxerOptions")) t.muxerOptions = m.value("muxerOptions").toString().trimmed();
    if (m.contains("ipFamily")) { const int f = m.value("ipFamily").toInt(); t.ipFamily = f == 4 || f == 6 ? f : 0; }
    number("bitrateKbps", t.bitrateKbps, 100, 200000);
    number("audioBitrateKbps", t.audioBitrateKbps, 32, 512);
    number("track", t.track, 0, 5);
    number("delaySeconds", t.delaySeconds, 0, 3600);
    number("retryDelaySeconds", t.retryDelaySeconds, 1, 60);
    number("maxRetries", t.maxRetries, 0, 10000);
    number("outputHeight", t.outputHeight, 0, 4320);
    t.outputHeight &= ~1;
    number("keyframeSeconds", t.choice.keyframeSeconds, 0, 20);
    if (m.contains("rateControl") && kRateControls.contains(m.value("rateControl").toString()))
        t.choice.rateControl = m.value("rateControl").toString();
    if (t.service != QLatin1String("custom") && !streamService(t.service.toStdString())) t.service = QStringLiteral("custom");
    return t;
}

QVariantMap targetMap(const StreamTarget &t) {
    return {{"id", t.id}, {"name", t.name}, {"service", t.service}, {"server", t.server}, {"enabled", t.enabled},
            {"encoder", t.encoder}, {"bitrateKbps", t.bitrateKbps}, {"audioBitrateKbps", t.audioBitrateKbps},
            {"track", t.track}, {"rateControl", t.choice.rateControl}, {"keyframeSeconds", t.choice.keyframeSeconds},
            {"delaySeconds", t.delaySeconds}, {"reconnect", t.reconnect}, {"retryDelaySeconds", t.retryDelaySeconds},
            {"maxRetries", t.maxRetries}, {"dynamicBitrate", t.dynamicBitrate}, {"bindAddress", t.bindAddress},
            {"outputHeight", t.outputHeight}, {"preserveDelay", t.preserveDelay}, {"ipFamily", t.ipFamily},
            {"lowLatency", t.lowLatency}, {"muxer", t.muxer}, {"muxerOptions", t.muxerOptions}, {"username", t.username}, {"hasPassword", t.hasPassword}, {"enforce", t.enforce},
            {"keyInProfile", !t.key.isEmpty()}};
}

QString keyLabel(const StreamTarget &t) {
    const StreamService *s = streamService(t.service.toStdString());
    const QString name = !t.name.isEmpty() ? t.name : s ? QString::fromStdString(s->name) : QStringLiteral("custom server");
    return QStringLiteral("OmaBroadcast stream key (%1)").arg(name);
}
}  // namespace

QString Backend::streamKeyLabel(const StreamTarget &target) { return keyLabel(target); }

QVariantList Backend::fileFormats() const {
    QVariantList out;
    for (const FileFormat &f : ::fileFormats())
        out.append(QVariantMap{{"id", QString::fromLatin1(f.id)}, {"label", QString::fromLatin1(f.label)},
                               {"chapters", f.chapters}, {"survivesCrash", f.survivesCrash}});
    out.append(QVariantMap{{"id", QStringLiteral("custom")}, {"label", tr("Custom (any FFmpeg container)")},
                           {"chapters", false}, {"survivesCrash", false}});
    return out;
}

bool Backend::encoderIsForEditing() const {
    if (!encoder().isEmpty()) return isEditingCodec(findEncoder(m_session.encoders(), encoder().toStdString()).codec);
    const auto ranked = m_session.recordingEncoders();
    return !ranked.empty() && isEditingCodec(ranked.front().encoder.codec);
}

QVariantList Backend::splitEncodeModes() const {
    std::string name = encoder().isEmpty() ? std::string() : findEncoder(m_session.encoders(), encoder().toStdString()).name;
    if (name.empty())
        if (const auto ranked = m_session.recordingEncoders(); !ranked.empty()) name = ranked.front().encoder.name;
    const std::vector<std::string> values = encoderOptionValues(name, "split_encode_mode");
    if (values.empty()) return {};
    QVariantList out{QVariantMap{{"value", QString()}, {"label", tr("Encoder's choice")}}};
    for (const std::string &v : values) {
        const QString value = QString::fromStdString(v);
        bool number = false;
        const int ways = value.toInt(&number);
        if (v == "auto") continue;  // what its choice is already
        const QString label = v == "disabled" ? tr("Off") : v == "forced" ? tr("On, as many ways as it likes")
                            : number ? tr("Across %1 engines (where it has them)").arg(ways) : value;
        out.append(QVariantMap{{"value", value}, {"label", label}});
    }
    return out;
}

QVariantMap Backend::muxerInfo(const QString &muxer) const {
    const std::string name = muxer.trimmed().toStdString();
    const std::string problem = muxerProblem(name);
    if (!problem.empty()) return {{"problem", QString::fromStdString(problem)}};
    const FileFormat &f = fileFormat("custom:" + name);
    return {{"problem", QString()}, {"label", QString::fromLatin1(f.label)},
            {"extension", QString::fromLatin1(f.extension)}, {"chapters", f.chapters}};
}

QString Backend::containerName() const {
    if (container() == QLatin1String("custom")) return customMuxer().isEmpty() ? tr("a custom container") : customMuxer();
    return container().toUpper();
}

void Backend::setCustomMuxer(const QString &muxer) {
    if (muxer.trimmed() == customMuxer()) return;
    m_session.profile().customMuxer = muxer.trimmed();
    saveProfile();
    emit settingsChanged();
}

void Backend::setContainerOptions(const QString &options) {
    if (options.trimmed() == containerOptions()) return;
    m_session.profile().containerOptions = options.trimmed();
    saveProfile();
    emit settingsChanged();
}

QVariantMap Backend::recordingOptions() const {
    const Profile &p = m_session.profile();
    const EncodingChoice &c = p.recordingChoice;
    return {{"rateControl", c.rateControl}, {"bitrateKbps", p.bitrateKbps > 0 ? p.bitrateKbps : 6000},
            {"maxKbps", c.maxKbps}, {"bufferKbps", c.bufferKbps}, {"keyframeSeconds", c.keyframeSeconds},
            {"bFrames", c.bFrames}, {"lookahead", c.lookahead}, {"preset", c.preset}, {"profile", c.profile},
            {"tune", c.tune}, {"options", c.options}, {"audioCodec", c.audioCodec}, {"splitEncode", c.splitEncode},
            {"splitMinutes", p.splitMinutes}, {"splitMegabytes", p.splitMegabytes}};
}

void Backend::setRecordingOption(const QString &key, const QVariant &value) {
    Profile &p = m_session.profile();
    EncodingChoice &c = p.recordingChoice;
    const int n = value.toInt();
    if (key == QLatin1String("rateControl") && kRateControls.contains(value.toString())) c.rateControl = value.toString();
    else if (key == QLatin1String("bitrateKbps")) p.bitrateKbps = std::clamp(n, 100, 500000);
    else if (key == QLatin1String("maxKbps")) c.maxKbps = std::clamp(n, 0, 500000);
    else if (key == QLatin1String("bufferKbps")) c.bufferKbps = std::clamp(n, 0, 500000);
    else if (key == QLatin1String("keyframeSeconds")) c.keyframeSeconds = std::clamp(n, 0, 20);
    else if (key == QLatin1String("bFrames")) c.bFrames = std::clamp(n, 0, 4);
    else if (key == QLatin1String("lookahead")) c.lookahead = std::clamp(n, 0, 60);
    else if (key == QLatin1String("preset")) c.preset = value.toString().trimmed();
    else if (key == QLatin1String("profile")) c.profile = value.toString().trimmed();
    else if (key == QLatin1String("tune")) c.tune = value.toString().trimmed();
    else if (key == QLatin1String("options")) c.options = value.toString().trimmed();
    else if (key == QLatin1String("splitEncode")) c.splitEncode = value.toString().trimmed();
    else if (key == QLatin1String("audioCodec") && kAudioCodecs.contains(value.toString())) c.audioCodec = value.toString();
    else if (key == QLatin1String("splitMinutes")) p.splitMinutes = std::clamp(n, 0, 24 * 60);
    else if (key == QLatin1String("splitMegabytes")) p.splitMegabytes = std::clamp(n, 0, 1024 * 1024);
    else return;
    saveProfile();
    emit settingsChanged();
}

bool Backend::chaptersAvailable() const { return fileFormat(m_session.profile().recordingFormat()).chapters; }

bool Backend::setRecordingPaused(bool paused) {
    if (!m_session.setRecordingPaused(paused)) return false;
    setStatus(paused ? tr("Recording paused") : tr("Recording resumed"));
    return true;
}

void Backend::splitRecording() {
    if (m_session.splitRecording()) setStatus(tr("Starting a new file at the next keyframe"));
}

void Backend::addChapter(const QString &name) {
    if (!m_session.recording()) return;
    if (m_session.addChapter(name)) setStatus(name.isEmpty() ? tr("Chapter marked") : tr("Chapter marked: %1").arg(name));
    else setStatus(tr("%1 files keep no chapters.").arg(fileFormat(m_session.profile().recordingFormat()).label));
}

QVariantList Backend::streamStatus() const {
    QVariantList out;
    for (const Session::StreamStatus &s : m_session.streamStatus())
        out.append(QVariantMap{{"id", s.id}, {"name", s.name}, {"state", s.state}, {"message", s.message},
                               {"encoder", s.encoder}, {"protocol", s.protocol}, {"kbps", s.kbps},
                               {"sent", qulonglong(s.framesSent)}, {"dropped", qulonglong(s.framesDropped)},
                               {"reconnects", s.reconnects}, {"lagMs", s.lagMs}, {"bitrateKbps", s.bitrateKbps}, {"bitrateHeld", s.bitrateHeld},
                               {"delayMs", s.delayMs}, {"heldBytes", qulonglong(s.heldBytes)}});
    return out;
}

QVariantList Backend::streamTargets() const {
    QVariantList out;
    for (const StreamTarget &t : m_session.profile().streams) out.append(targetMap(t));
    return out;
}

QVariantList Backend::streamServices() const {
    QVariantList out;
    out.append(QVariantMap{{"id", "custom"}, {"name", tr("Custom server")}, {"servers", QVariantList{}},
                           {"keyLabel", tr("Stream key")}, {"codecs", QStringList{}}});
    for (const StreamService &s : ::streamServices()) {
        QVariantList servers;
        for (const auto &server : s.servers)
            servers.append(QVariantMap{{"name", QString::fromStdString(server.name)}, {"url", QString::fromStdString(server.url)}});
        QStringList codecs;
        for (const auto &c : s.videoCodecs) codecs << QString::fromStdString(c);
        out.append(QVariantMap{{"id", QString::fromStdString(s.id)}, {"name", QString::fromStdString(s.name)},
                               {"servers", servers}, {"keyLabel", QString::fromStdString(s.keyLabel)}, {"codecs", codecs}});
    }
    return out;
}

void Backend::setRecordWhileStreaming(bool on) {
    if (on == recordWhileStreaming()) return;
    m_session.profile().recordWhileStreaming = on;
    saveProfile();
    emit settingsChanged();
}

void Backend::setStopRecordingWithStream(bool on) {
    if (on == stopRecordingWithStream()) return;
    m_session.profile().stopRecordingWithStream = on;
    saveProfile();
    emit settingsChanged();
}

QString Backend::saveStreamTarget(const QVariantMap &values, const QString &key, bool keyGiven) {
    if (m_session.streaming()) {
        emit failed(tr("Stop streaming before changing where it goes."));
        return {};
    }
    Profile &p = m_session.profile();
    const QString profileId = m_profileId;
    const QString id = values.value("id").toString();
    auto it = std::find_if(p.streams.begin(), p.streams.end(), [&](const StreamTarget &t) { return !id.isEmpty() && t.id == id; });
    StreamTarget target = targetFrom(values, it != p.streams.end() ? *it : StreamTarget{});
    if (target.id.isEmpty()) target.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (target.service == QLatin1String("custom") && target.server.isEmpty()) {
        emit failed(tr("A custom stream needs a server address."));
        return {};
    }
    if (keyGiven) {
        // The keyring holds it; only with no keyring does it go in the profile.
        if (m_secrets.available()) {
            QString why;
            if (key.isEmpty() ? !m_secrets.remove(target.id, &why) : !m_secrets.store(target.id, keyLabel(target), key.toUtf8(), &why)) {
                emit failed(tr("The keyring did not take the stream key: %1").arg(why));
                return {};
            }
            target.key.clear();
        } else {
            target.key = key;
        }
    }
    if (values.contains("password")) {
        // Beside the key, under its own name.
        const QString password = values.value("password").toString();
        if (m_secrets.available()) {
            QString why;
            const QString id = target.id + QStringLiteral("/password");
            const QString label = QStringLiteral("OmaBroadcast stream password (%1)").arg(target.name.isEmpty() ? target.server : target.name);
            if (password.isEmpty() ? !m_secrets.remove(id, &why) : !m_secrets.store(id, label, password.toUtf8(), &why)) {
                emit failed(tr("The keyring did not take the password: %1").arg(why));
                return {};
            }
            target.password.clear();
        } else {
            target.password = password;
        }
        target.hasPassword = !password.isEmpty();
    }
    // A locked keyring's prompt keeps everything else running while it waits,
    // so what was found before it may have changed.
    if (m_profileId != profileId || m_session.streaming()) {
        emit failed(tr("The stream was not saved: the profile changed or went live while the keyring was asked."));
        return {};
    }
    it = std::find_if(p.streams.begin(), p.streams.end(), [&](const StreamTarget &t) { return t.id == target.id; });
    if (it != p.streams.end()) *it = target;
    else p.streams.append(target);
    saveProfile();
    emit streamTargetsChanged();
    return target.id;
}

void Backend::removeStreamTarget(const QString &id) {
    if (m_session.streaming()) {
        emit failed(tr("Stop streaming before changing where it goes."));
        return;
    }
    Profile &p = m_session.profile();
    const auto before = p.streams.size();
    p.streams.removeIf([&](const StreamTarget &t) { return t.id == id; });
    if (p.streams.size() == before) return;
    if (m_secrets.available()) {
        m_secrets.remove(id);
        m_secrets.remove(id + QStringLiteral("/password"));
    }
    saveProfile();
    emit streamTargetsChanged();
}

bool Backend::streamTargetHasKey(const QString &id) {
    for (const StreamTarget &t : m_session.profile().streams)
        if (t.id == id) return !t.key.isEmpty() || (m_secrets.available() && m_secrets.has(id));
    return false;
}

QVariantList Backend::streamEncoderChoices(const QVariantMap &values) const {
    const StreamTarget target = targetFrom(values, StreamTarget{});
    const std::vector<RankedEncoder> ranked = m_session.streamEncoders(target);
    QVariantList out;
    out.append(QVariantMap{{"name", QString()},
                           {"label", ranked.empty() ? tr("Best available")
                                                    : tr("Best available (%1)").arg(QString::fromStdString(ranked.front().encoder.label))}});
    for (const RankedEncoder &r : ranked)
        out.append(QVariantMap{{"name", QString::fromStdString(encoderKey(r.encoder))}, {"label", QString::fromStdString(r.encoder.label)}});
    return out;
}

QStringList Backend::streamAdjustments(const QVariantMap &values) const {
    const StreamTarget asked = targetFrom(values, StreamTarget{});
    const StreamTarget sent = m_session.keptToService(asked);
    QStringList out;
    if (sent.bitrateKbps != asked.bitrateKbps) out << tr("Sent at %1 kb/s, the most it recommends.").arg(sent.bitrateKbps);
    if (sent.choice.keyframeSeconds != asked.choice.keyframeSeconds)
        out << tr("A keyframe every %1 seconds, as it asks.").arg(sent.choice.keyframeSeconds);
    if (sent.outputHeight != asked.outputHeight) {
        const QSize size = m_session.outputSize(sent.outputHeight);
        out << tr("Sent at %1 × %2, the largest it takes.").arg(size.width()).arg(size.height());
    }
    if (sent.enforce && !asked.encoder.isEmpty()) {
        const StreamService *service = streamService(asked.service.toStdString());
        const EncoderInfo e = findEncoder(m_session.encoders(), asked.encoder.toStdString());
        if (service && !e.name.empty() && !service->videoCodecs.empty() &&
            std::find(service->videoCodecs.begin(), service->videoCodecs.end(), e.codec) == service->videoCodecs.end())
            out << tr("Encoded with the best encoder it takes instead of %1.").arg(QString::fromStdString(e.label));
    }
    return out;
}

QStringList Backend::streamWarnings(const QVariantMap &values) const {
    // What is still out of bounds once it keeps to the service (when it does).
    const StreamTarget target = m_session.keptToService(targetFrom(values, StreamTarget{}));
    const StreamService *service = streamService(target.service.toStdString());
    if (!service) return {};
    std::string codec = "h264";
    const std::vector<RankedEncoder> ranked = m_session.streamEncoders(target);
    const EncoderInfo chosen = target.encoder.isEmpty() ? EncoderInfo{} : findEncoder(m_session.encoders(), target.encoder.toStdString());
    const bool replaced = target.enforce && !chosen.name.empty() && !service->videoCodecs.empty() &&
        std::find(service->videoCodecs.begin(), service->videoCodecs.end(), chosen.codec) == service->videoCodecs.end();
    if (!chosen.name.empty() && !replaced) {
        codec = chosen.codec;
    } else if (!ranked.empty()) {
        codec = ranked.front().encoder.codec;
    }
    QStringList out;
    const QSize size = m_session.outputSize(target.outputHeight);
    for (const std::string &w : serviceWarnings(*service, codec, size.width(), size.height(), fps(), target.bitrateKbps,
                                                target.choice.keyframeSeconds))
        out << QString::fromStdString(w);
    return out;
}

QString Backend::dynamicBitrateNote(const QVariantMap &values) const {
    const StreamTarget target = m_session.keptToService(targetFrom(values, StreamTarget{}));
    if (target.choice.rateControl != QLatin1String("cbr") && target.choice.rateControl != QLatin1String("vbr"))
        return tr("This stream holds a quality rather than a bitrate, so there is no bitrate to lower: it only drops pictures when the network falls behind.");
    // The encoder it would go out on: a choice the service refuses gives way, as when going live.
    const StreamService *service = streamService(target.service.toStdString());
    EncoderInfo encoder = target.encoder.isEmpty() ? EncoderInfo{} : findEncoder(m_session.encoders(), target.encoder.toStdString());
    if (target.enforce && service && !encoder.name.empty() && !service->videoCodecs.empty() &&
        std::find(service->videoCodecs.begin(), service->videoCodecs.end(), encoder.codec) == service->videoCodecs.end())
        encoder = {};
    if (encoder.name.empty())
        if (const std::vector<RankedEncoder> ranked = m_session.streamEncoders(target); !ranked.empty()) encoder = ranked.front().encoder;
    if (encoder.name.empty() || encoderFollowsBitrate(encoder.name)) return {};
    return tr("%1 cannot change its bitrate once it has started, so this stream only drops pictures when the network falls behind.")
        .arg(QString::fromStdString(encoder.label));
}

void Backend::toggleStreaming() {
    if (m_session.streaming()) {
        if (!m_session.streamingStopping()) m_session.stopStreaming();
        return;
    }
    goLive({});
}

void Backend::streamsInKeyring(QStringList *keys, QStringList *passwords) const {
    for (const StreamTarget &t : m_session.profile().streams) {
        if (!t.enabled) continue;
        if (t.key.isEmpty()) keys->append(t.id);
        if (t.hasPassword && t.password.isEmpty()) passwords->append(t.id);
    }
}

void Backend::goLive(const std::function<void()> &settled) {
    if (m_askingKeyring) {
        setStatus(tr("Waiting for the keyring: answer its prompt to go live."));
        if (settled) settled();
        return;
    }
    QStringList keys, passwords;
    streamsInKeyring(&keys, &passwords);
    if ((keys.isEmpty() && passwords.isEmpty()) || !m_secrets.available()) {
        startStreams({}, {});
        if (settled) settled();
        return;
    }
    // Keys and passwords come out of the keyring only now, for the targets going
    // live. On a thread: a locked keyring waits up to two minutes for its prompt,
    // and everything here, remote control too, carries on meanwhile.
    m_askingKeyring = true;
    setStatus(tr("Asking the keyring for the stream key."));
    m_keyringWork.run([this, store = m_secrets, keys, passwords, settled](const FileWork::Token &token) mutable -> FileWork::Answer {
        store.stillWanted = [token] { return token.wanted(); };
        QMap<QString, QString> gotKeys, gotPasswords;
        QString problem, why;
        for (const QString &id : std::as_const(keys)) {
            gotKeys.insert(id, QString::fromUtf8(store.lookup(id, &why)));
            if (!why.isEmpty()) {
                problem = tr("The keyring did not give the stream key: %1").arg(why);
                break;
            }
        }
        for (const QString &id : std::as_const(passwords)) {
            if (!problem.isEmpty()) break;
            gotPasswords.insert(id, QString::fromUtf8(store.lookup(id + QStringLiteral("/password"), &why)));
            if (!why.isEmpty()) problem = tr("The keyring did not give the stream password: %1").arg(why);
        }
        return [this, keys, passwords, gotKeys, gotPasswords, problem, settled] {
            m_askingKeyring = false;
            QStringList keysNow, passwordsNow;
            streamsInKeyring(&keysNow, &passwordsNow);
            if (!problem.isEmpty()) {
                setStatus(problem);
                emit failed(problem);
            } else if (keysNow != keys || passwordsNow != passwords) {
                // The prompt keeps everything running: a change of profile, say.
                setStatus(tr("The streams changed while the keyring was asked. Go live again."));
                emit failed(status());
            } else {
                startStreams(gotKeys, gotPasswords);
            }
            if (settled) settled();
        };
    });
}

void Backend::startStreams(const QMap<QString, QString> &keys, const QMap<QString, QString> &passwords) {
    QString error;
    const bool started = m_session.startStreaming(keys, &error, passwords);
    if (!started) {
        setStatus(error);
        emit failed(error);
        return;
    }
    m_streamClock.start();
    if (!error.isEmpty()) emit failed(error);  // some went live, some did not
    setStatus(tr("Going live") + (noSound() ? QStringLiteral(". ") + tr("No sound: nothing in the Audio mixer is on.") : QString()));
}

// --------------------------------------------------------------- replay buffer

QString Backend::replayHeld() const {
    const Session::ReplayStatus s = m_session.replayStatus();
    QString text = QStringLiteral("%1 s · %2").arg(s.heldNs / 1'000'000'000).arg(formatBytes(s.heldBytes));
    if (s.saving) text += tr(" · saving");
    return text;
}

QVariantMap Backend::replayOptions() const {
    const Profile &p = m_session.profile();
    return {{"seconds", p.replaySeconds}, {"megabytes", p.replayMegabytes}, {"prefix", p.replayPrefix},
            {"suffix", p.replaySuffix}};
}

void Backend::setReplayOption(const QString &key, const QVariant &value) {
    Profile &p = m_session.profile();
    const int n = value.toInt();
    if (key == QLatin1String("seconds")) p.replaySeconds = std::clamp(n, 0, 6 * 3600);
    else if (key == QLatin1String("megabytes")) p.replayMegabytes = std::clamp(n, 0, 64 * 1024);
    else if (key == QLatin1String("prefix")) p.replayPrefix = value.toString();
    else if (key == QLatin1String("suffix")) p.replaySuffix = value.toString();
    else return;
    // Something has to bound it.
    if (p.replaySeconds == 0 && p.replayMegabytes == 0) p.replaySeconds = 20;
    saveProfile();
    emit settingsChanged();
}

QString Backend::replayEstimate() const {
    const Profile &p = m_session.profile();
    const QString rate = p.recordingChoice.rateControl;
    if (p.replaySeconds == 0) return tr("Up to %1 MB").arg(p.replayMegabytes);
    // A bitrate target says how much a second takes; quality modes depend on the picture.
    if (rate != QLatin1String("cbr") && rate != QLatin1String("vbr")) return {};
    const int videoKbps = p.bitrateKbps > 0 ? p.bitrateKbps : 6000;
    const int peakKbps = rate == QLatin1String("vbr") && p.recordingChoice.maxKbps > 0 ? p.recordingChoice.maxKbps : videoKbps;
    int tracks = 0;
    for (unsigned t = p.recordingTracks & 63; t; t &= t - 1) ++tracks;
    const double bytes = (double(peakKbps) + double(p.audioBitrateKbps) * std::max(tracks, 1)) * 1000.0 / 8.0 * p.replaySeconds;
    const double cap = double(p.replayMegabytes) * 1024 * 1024;
    return p.replayMegabytes > 0 && bytes > cap ? tr("About %1 MB; the %2 MB limit comes first").arg(qRound64(bytes / 1048576)).arg(p.replayMegabytes)
                                                : tr("About %1 MB").arg(qRound64(bytes / 1048576));
}

void Backend::setReplayWhileStreaming(bool on) {
    if (on == replayWhileStreaming()) return;
    m_session.profile().replayWhileStreaming = on;
    saveProfile();
    emit settingsChanged();
}

void Backend::toggleReplay() {
    if (m_session.replayActive()) {
        m_session.stopReplay(false);
        setStatus(tr("Replay buffer stopped"));
        return;
    }
    QString error;
    if (!m_session.startReplay(&error)) {
        setStatus(error);
        emit failed(error);
        return;
    }
    const Session::ReplayStatus s = m_session.replayStatus();
    setStatus(tr("Replay buffer running with %1").arg(s.encoder));
}

void Backend::saveReplay() {
    QString error;
    if (!m_session.saveReplay(&error)) {
        setStatus(error);
        emit failed(error);
        return;
    }
    setStatus(tr("Saving the replay…"));
}

// ------------------------------------------------------------- virtual camera

void Backend::toggleVirtualCamera() {
    if (m_session.virtualCameraRunning()) {
        m_session.stopVirtualCamera();
        setStatus(tr("Virtual camera off"));
        return;
    }
    QString error;
    if (!m_session.startVirtualCamera(&error)) {
        setStatus(error);
        emit failed(error);
        return;
    }
    const QVariantMap shows = cameraShows();
    setStatus(shows.value("shows").toString() == QLatin1String("program")
                  ? tr("Virtual camera on: choose \"OmaBroadcast Camera\" in the other app")
                  : tr("Virtual camera on, showing %1: choose \"OmaBroadcast Camera\" in the other app").arg(shows.value("label").toString()));
}

QVariantList Backend::cameraChoices() const {
    QVariantList out{QVariantMap{{"shows", "program"}, {"target", QString()}, {"label", tr("Program")}},
                     QVariantMap{{"shows", "preview"}, {"target", QString()}, {"label", tr("Preview (studio mode)")}}};
    const Collection &c = m_production.collection();
    for (const Scene &s : c.scenes)
        if (!s.group) out.append(QVariantMap{{"shows", "studio"}, {"target", s.id}, {"label", tr("Studio: %1").arg(s.name)}});
    for (const SourceDef &src : c.sources) {
        // Pictures only; a studio is offered as a studio.
        if (SourceDef::isAudioKind(src.kind) || src.kind == QLatin1String("studio")) continue;
        out.append(QVariantMap{{"shows", "source"}, {"target", src.id}, {"label", tr("Source: %1").arg(src.name)}});
    }
    return out;
}

QVariantMap Backend::cameraShows() const {
    const Profile &p = m_session.profile();
    const Collection &c = m_production.collection();
    QString label = tr("Program");
    bool gone = false;
    if (p.cameraShows == QLatin1String("preview")) {
        label = tr("Preview (studio mode)");
    } else if (p.cameraShows == QLatin1String("studio")) {
        const Scene *s = c.scene(p.cameraTarget);
        gone = !s || s->group;
        if (!gone) label = tr("Studio: %1").arg(s->name);
    } else if (p.cameraShows == QLatin1String("source")) {
        const SourceDef *src = c.source(p.cameraTarget);
        gone = !src;
        if (!gone) label = tr("Source: %1").arg(src->name);
    }
    return {{"shows", gone ? QStringLiteral("program") : p.cameraShows}, {"target", gone ? QString() : p.cameraTarget},
            {"label", label}, {"gone", gone}};
}

void Backend::setCameraShows(const QString &shows, const QString &target) {
    m_session.setVirtualCameraShows(shows, target);
    saveProfile();
    if (m_session.virtualCameraRunning()) setStatus(tr("The virtual camera shows %1").arg(cameraShows().value("label").toString()));
}

// ---------------------------------------------------------------------- remux

void Backend::setRemuxToMp4(bool on) {
    if (on == remuxToMp4()) return;
    m_session.profile().remuxToMp4 = on;
    saveProfile();
    emit settingsChanged();
}

QVariantList Backend::remuxJobs() const {
    QVariantList out;
    for (const RemuxQueue::Job &j : m_session.remux().jobs()) {
        static const char *const states[] = {"waiting", "running", "done", "failed", "cancelled"};
        out.append(QVariantMap{{"id", j.id}, {"name", QFileInfo(QString::fromStdString(j.in)).fileName()},
                               {"out", QFileInfo(QString::fromStdString(j.out)).fileName()},
                               {"state", QString::fromLatin1(states[int(j.state)])}, {"progress", j.progress},
                               {"error", QString::fromStdString(j.note.empty() ? j.error : j.note)}});
    }
    return out;
}

void Backend::chooseRemuxFiles() {
    m_remuxPicker.openFiles(tr("Recordings to copy into MP4"), tr("Recordings"),
                            {"*.mkv", "*.mov", "*.ts", "*.flv", "*.mp4", "*.m3u8", "*"});
}

void Backend::remuxFiles(const QStringList &paths) {
    int added = 0;
    for (const QString &p : paths) {
        if (p.isEmpty() || !QFileInfo(p).isFile()) continue;
        m_session.remux().add(p.toStdString());
        ++added;
    }
    if (added) setStatus(tr("Copying %n file(s) into MP4…", nullptr, added));
    emit remuxChanged();
}

void Backend::cancelRemux(int id) {
    m_session.remux().cancel(id);
    emit remuxChanged();
}

void Backend::clearRemux() {
    m_session.remux().clearFinished();
    emit remuxChanged();
}

// ------------------------------------------------------------- output sizes

void Backend::setRecordingHeight(int height) {
    height = std::clamp(height, 0, 4320) & ~1;
    if (height == recordingHeight() || m_session.recording()) return;
    m_session.profile().recordingHeight = height;
    saveProfile();
    m_session.refitEncoders();
    emit settingsChanged();
}

QVariantList Backend::outputSizes() const {
    const int canvas = canvasHeight();
    QVariantList out;
    out.append(QVariantMap{{"height", 0}, {"label", tr("Canvas (%1 × %2)").arg(canvasWidth()).arg(canvas)}});
    QList<int> heights{2160, 1440, 1080, 900, 720, 540, 480, 360};
    // A size chosen before the canvas changed stays on offer.
    for (int h : {recordingHeight()}) if (h > 0 && !heights.contains(h)) heights.append(h);
    for (const StreamTarget &t : m_session.profile().streams)
        if (t.outputHeight > 0 && !heights.contains(t.outputHeight)) heights.append(t.outputHeight);
    std::sort(heights.begin(), heights.end(), std::greater<int>());
    for (int h : heights) {
        if (h == canvas) continue;
        const QSize s = m_session.outputSize(h);
        out.append(QVariantMap{{"height", h},
                               {"label", h > canvas ? tr("%1 × %2 (enlarged)").arg(s.width()).arg(s.height())
                                                    : tr("%1 × %2").arg(s.width()).arg(s.height())}});
    }
    return out;
}
