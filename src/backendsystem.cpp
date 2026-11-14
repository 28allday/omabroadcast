// Tools: the stats dock, the log, crash reports, repair.
#include "backend.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include "core/repair.h"

void Backend::setUpTools() {
    m_systemStatsTimer.setInterval(1000);
    connect(&m_systemStatsTimer, &QTimer::timeout, this, &Backend::refreshSystemStats);
}

void Backend::setStatsWatched(bool on) {
    if (on == m_systemStatsTimer.isActive()) return;
    if (on) {
        m_systemStatsTimer.start();
        refreshSystemStats();  // CPU use needs two readings: the first says nothing yet
    } else {
        m_systemStatsTimer.stop();
    }
    emit systemStatsChanged();
}

void Backend::refreshSystemStats() {
    const SystemStats::Sample m = m_systemStats.sample();
    const Session::Stats s = m_session.stats();
    QVariantMap out;
    out[QStringLiteral("processCpu")] = m.processCpu;
    out[QStringLiteral("systemCpu")] = m.systemCpu;
    out[QStringLiteral("cpus")] = m.cpus;
    out[QStringLiteral("residentBytes")] = m.residentBytes;
    out[QStringLiteral("resident")] = m.residentBytes >= 0 ? formatBytes(quint64(m.residentBytes)) : QString();
    const QString engineNode = m_session.engineRenderNode();
    QVariantList gpus;
    for (const SystemStats::Gpu &g : m.gpus) {
        const bool compositing = !g.renderNode.isEmpty() && g.renderNode == engineNode;
        gpus.append(QVariantMap{{QStringLiteral("name"), compositing ? s.engine : tr("Graphics card %1").arg(g.card.mid(4))},
                                {QStringLiteral("busy"), g.busy},
                                {QStringLiteral("compositing"), compositing}});
    }
    out[QStringLiteral("gpus")] = gpus;

    // The folder may not be made yet: the disk it will be on.
    QString folder = m_session.profile().resolvedFolder();
    while (!folder.isEmpty() && !QFileInfo::exists(folder) && QFileInfo(folder).absolutePath() != folder)
        folder = QFileInfo(folder).absolutePath();
    const qint64 free = m_session.freeSpace(folder);
    out[QStringLiteral("freeBytes")] = free;
    out[QStringLiteral("free")] = free >= 0 ? formatBytes(quint64(free)) : QString();
    double minutes = -1;
    if (s.recording && free >= 0 && s.recorder.durationNs > 2'000'000'000ll && s.recorder.bytesWritten > 0)
        minutes = free / (s.recorder.bytesWritten / (s.recorder.durationNs / 1e9)) / 60.0;
    out[QStringLiteral("recording")] = s.recording;
    out[QStringLiteral("minutesLeft")] = minutes;

    out[QStringLiteral("fps")] = s.fps;
    out[QStringLiteral("renderMs")] = s.renderMs;
    out[QStringLiteral("renderWorstMs")] = s.renderWorstMs;
    out[QStringLiteral("lateTicks")] = qulonglong(s.lateTicks);
    out[QStringLiteral("engine")] = s.engine;
    out[QStringLiteral("engineWhy")] = s.engineWhy;

    QVariantList outputs;
    for (const Session::OutputStats &o : m_session.outputStats()) {
        QString name = o.name;
        if (o.kind == QLatin1String("recording")) name = tr("Recording");
        else if (o.kind == QLatin1String("replay")) name = tr("Replay buffer");
        else if (o.kind == QLatin1String("camera")) name = tr("Virtual camera");
        outputs.append(QVariantMap{{QStringLiteral("kind"), o.kind}, {QStringLiteral("name"), name},
                                   {QStringLiteral("encodeMs"), o.encodeMs}, {QStringLiteral("lagMs"), o.lagMs},
                                   {QStringLiteral("encoder"), o.encoder}, {QStringLiteral("hardware"), o.hardware},
                                   {QStringLiteral("frames"), qulonglong(o.frames)},
                                   {QStringLiteral("dropped"), qulonglong(o.dropped)}});
    }
    out[QStringLiteral("outputs")] = outputs;
    m_systemStatsMap = out;
    emit systemStatsChanged();
}

QList<AppLog::Line> Backend::logSelection(bool previous, int minimumLevel, const QString &search) {
    QList<AppLog::Line> all;
    if (previous) {
        if (!m_previousLogRead && AppLog::instance()) {
            m_previousLog = AppLog::readFile(AppLog::instance()->previousPath());
            m_previousLogRead = true;
        }
        all = m_previousLog;
    } else if (AppLog::instance()) {
        all = AppLog::instance()->lines();
    }
    if (minimumLevel <= AppLog::Debug && search.isEmpty()) return all;
    QList<AppLog::Line> out;
    for (const AppLog::Line &line : std::as_const(all))
        if (line.level >= minimumLevel && (search.isEmpty() || line.text.contains(search, Qt::CaseInsensitive))) out.append(line);
    return out;
}

QVariantList Backend::logLines(bool previous, int minimumLevel, const QString &search) {
    QVariantList out;
    for (const AppLog::Line &line : logSelection(previous, minimumLevel, search))
        out.append(QVariantMap{{QStringLiteral("time"), line.msecs > 0 ? QDateTime::fromMSecsSinceEpoch(line.msecs).toString(QStringLiteral("HH:mm:ss.zzz")) : QString()},
                               {QStringLiteral("level"), line.level},
                               {QStringLiteral("text"), line.text}});
    return out;
}

bool Backend::hasPreviousLog() {
    return !logSelection(true, AppLog::Debug, {}).isEmpty();
}

void Backend::copyLog(bool previous, int minimumLevel, const QString &search) {
    QStringList rows;
    for (const AppLog::Line &line : logSelection(previous, minimumLevel, search)) rows << AppLog::format(line);
    copyText(rows.join(QLatin1Char('\n')) + QLatin1Char('\n'));
    setStatus(tr("Copied %n log line(s)", nullptr, int(rows.size())));
}

bool Backend::saveLogTo(bool previous, const QString &path) {
    QByteArray bytes;
    for (const AppLog::Line &line : logSelection(previous, AppLog::Debug, {})) bytes += AppLog::format(line).toUtf8() + '\n';
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
}

void Backend::saveLog(bool previous) {
    m_otherChosen = [this, previous](const QString &path) {
        if (saveLogTo(previous, path)) setStatus(tr("Log saved to %1").arg(path));
        else setStatus(tr("Could not save the log to %1").arg(path));
    };
    const QString name = previous ? QStringLiteral("omabroadcast-previous.log")
                                  : QStringLiteral("omabroadcast-%1.log").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd-HHmm")));
    m_otherPicker.saveFile(tr("Save the log"), name, tr("Logs"), {QStringLiteral("*.log"), QStringLiteral("*.txt")});
}

void Backend::startAfterCrash(bool safe) {
    if (!m_crashOffered) return;
    m_crashOffered = false;
    if (safe) m_session.setSafeMode(true);
    qInfo().noquote() << (m_session.safeMode() ? "starting in safe mode after a run that did not quit cleanly"
                                               : "starting normally after a run that did not quit cleanly");
    start();
    emit startStateChanged();
    if (const QUrl wanted = std::exchange(m_openAfterCrash, {}); !wanted.isEmpty()) open(wanted);
}

namespace {
QString configDir() { return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation); }
}  // namespace

void Backend::beginRepair() { m_repairWhen = QDateTime::currentDateTime(); }

QString Backend::repairFolder() const {
    return repair::asideFolder(configDir(), m_repairWhen.isValid() ? m_repairWhen : QDateTime::currentDateTime());
}

QStringList Backend::staleGrantKeys() const {
    QStringList captures;
    for (const SourceDef &s : m_production.collection().sources)
        if (s.kind == QLatin1String("display") || s.kind == QLatin1String("window") || s.kind == QLatin1String("region")) captures << s.id;
    return repair::staleGrants(m_session.profile().restoreTokens, configDir(), m_session.profile().collection, captures);
}

QVariantMap Backend::repairCheck() const {
    QVariantList problems;
    for (const repair::Problem &p : repair::check(configDir(), &Backend::profileFileReadable))
        problems.append(QVariantMap{{QStringLiteral("kind"), p.kind}, {QStringLiteral("id"), p.id},
                                    {QStringLiteral("path"), p.path}, {QStringLiteral("why"), p.why}});
    const int collections = int(QDir(configDir() + QStringLiteral("/scenes")).entryList({QStringLiteral("*.json")}, QDir::Files).size());
    const int profiles = int(QDir(configDir() + QStringLiteral("/profiles")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size());
    return {{QStringLiteral("problems"), problems}, {QStringLiteral("collections"), collections},
            {QStringLiteral("profiles"), profiles}, {QStringLiteral("staleGrants"), int(staleGrantKeys().size())},
            {QStringLiteral("folder"), repairFolder()}};
}

bool Backend::setAsideUnreadable() {
    const QString folder = repairFolder();
    int moved = 0;
    for (const repair::Problem &p : repair::check(configDir(), &Backend::profileFileReadable)) {
        QString error;
        if (!repair::setAside(configDir(), folder, p.path, false, &error)) {
            setStatus(error);
            emit collectionsChanged();
            emit profilesChanged();
            return false;
        }
        ++moved;
        // What is open was read before the file went bad: written afresh from memory.
        if (p.kind == QLatin1String("collection") && p.id == m_session.profile().collection) m_production.save();
        if (p.kind == QLatin1String("profile") && p.id == m_profileId) saveProfile();
    }
    emit collectionsChanged();
    emit profilesChanged();
    setStatus(moved ? tr("Set aside %n file(s) in %1", nullptr, moved).arg(folder) : tr("Nothing needed setting aside."));
    return true;
}

bool Backend::resetWindowPreferences() {
    const QString folder = repairFolder(), path = configDir() + QStringLiteral("/global.json");
    QString error;
    if (QFile::exists(path) && !repair::setAside(configDir(), folder, path, true, &error)) {
        setStatus(error);
        return false;
    }
    for (const QString &key : repair::windowPreferenceKeys()) m_preferences.remove(key);
    QJsonObject global;
    {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) global = QJsonDocument::fromJson(f.read(1024 * 1024)).object();
    }
    global.insert(QStringLiteral("preferences"), QJsonObject::fromVariantMap(m_preferences));
    QDir().mkpath(configDir());
    QSaveFile f(path);
    const QByteArray bytes = QJsonDocument(global).toJson(QJsonDocument::Indented);
    const bool saved = f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
    emit preferencesChanged();
    emit projectorsChanged();
    setStatus(saved ? tr("Window layout reset; the old settings are in %1").arg(folder) : tr("Could not save preferences."));
    return saved;
}

bool Backend::clearStaleGrants() {
    const QStringList stale = staleGrantKeys();
    if (stale.isEmpty()) {
        setStatus(tr("No capture grant is stale."));
        return true;
    }
    QVariantMap removed;
    for (const QString &key : stale) removed.insert(key, m_session.profile().restoreTokens.value(key));
    QString error;
    if (!repair::writeAside(repairFolder(), QStringLiteral("capture-grants-%1.json").arg(m_profileId), removed, &error)) {
        setStatus(error);
        return false;
    }
    for (const QString &key : stale) m_session.profile().restoreTokens.remove(key);
    saveProfile();
    setStatus(tr("Cleared %n capture grant(s); a copy is in %1", nullptr, int(stale.size())).arg(repairFolder()));
    return true;
}
