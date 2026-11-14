#include "control/omarchy_bar.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace {

constexpr int kTries = 6;
constexpr int kRetryMs = 2000;  // the shell finds a new plugin a moment after it lands
const char *const kSource = ":/omarchy-bar";

bool sameBytes(const QString &a, const QString &b) {
    QFile first(a), second(b);
    return first.open(QIODevice::ReadOnly) && second.open(QIODevice::ReadOnly) && first.readAll() == second.readAll();
}

}  // namespace

OmarchyBar::OmarchyBar(QObject *parent)
    : OmarchyBar(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
                 QStandardPaths::findExecutable(QStringLiteral("omarchy")), parent) {}

OmarchyBar::OmarchyBar(const QString &configDir, const QString &command, QObject *parent)
    : QObject(parent), m_dir(configDir + QStringLiteral("/omarchy")), m_command(command) {
    m_available = !m_command.isEmpty() && QFile::exists(m_dir + QStringLiteral("/shell.json"));
    if (!m_available) return;
    m_retry = new QTimer(this);
    m_retry->setSingleShot(true);
    m_retry->setInterval(kRetryMs);
    connect(m_retry, &QTimer::timeout, this, [this] { run(m_verb, m_extra); });
    // The bar's settings are replaced whole when saved, so the folder is watched too.
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { readActive(); watch(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { readActive(); watch(); });
    watch();
    readActive();
}

QString OmarchyBar::pluginDir() const {
    return m_dir + QStringLiteral("/plugins/") + QLatin1String(kPluginId);
}

void OmarchyBar::watch() {
    const QString file = m_dir + QStringLiteral("/shell.json");
    if (!m_watcher.directories().contains(m_dir)) m_watcher.addPath(m_dir);
    if (QFile::exists(file) && !m_watcher.files().contains(file)) m_watcher.addPath(file);
}

void OmarchyBar::readActive() {
    QFile file(m_dir + QStringLiteral("/shell.json"));
    bool found = false;
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject layout = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("bar")).toObject()
                                       .value(QStringLiteral("layout")).toObject();
        for (auto section = layout.constBegin(); section != layout.constEnd() && !found; ++section) {
            if (!section->isArray()) continue;
            for (const QJsonValue &entry : section->toArray()) {
                const QString id = entry.isObject() ? entry.toObject().value(QStringLiteral("id")).toString() : entry.toString();
                if (id == QLatin1String(kPluginId)) { found = true; break; }
            }
        }
    }
    if (found == m_active) return;
    m_active = found;
    emit activeChanged();
}

bool OmarchyBar::install(QString *why) {
    const auto refuse = [&](const QString &text) {
        if (why) *why = text;
        return false;
    };
    if (!m_available) return refuse(tr("Omarchy's bar is not here."));
    const QString target = pluginDir();
    if (QFileInfo(target).isSymLink()) return true;
    const QDir source{QLatin1String(kSource)};
    const QStringList files = source.entryList(QDir::Files);
    if (files.isEmpty()) return refuse(tr("The bar icon's files are missing from this build."));
    bool same = QDir(target).entryList(QDir::Files) == files;
    for (const QString &name : files) {
        if (!same) break;
        same = sameBytes(source.filePath(name), target + QLatin1Char('/') + name);
    }
    if (same) return true;
    // Built beside the plugins folder, not in it, and moved in whole: the shell
    // is watching that folder and must never see half a plugin.
    const QString stage = m_dir + QStringLiteral("/.") + QLatin1String(kPluginId) + QStringLiteral(".new");
    const QString old = m_dir + QStringLiteral("/.") + QLatin1String(kPluginId) + QStringLiteral(".old");
    QDir(stage).removeRecursively();
    QDir(old).removeRecursively();
    if (!QDir().mkpath(stage) || !QDir().mkpath(m_dir + QStringLiteral("/plugins"))) return refuse(tr("Cannot write to %1.").arg(m_dir));
    for (const QString &name : files) {
        const QString to = stage + QLatin1Char('/') + name;
        if (!QFile::copy(source.filePath(name), to)) { QDir(stage).removeRecursively(); return refuse(tr("Cannot write %1.").arg(to)); }
        QFile::setPermissions(to, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    }
    if (QFileInfo::exists(target) && !QDir().rename(target, old)) { QDir(stage).removeRecursively(); return refuse(tr("Cannot replace %1.").arg(target)); }
    if (!QDir().rename(stage, target)) {
        QDir().rename(old, target);
        QDir(stage).removeRecursively();
        return refuse(tr("Cannot write %1.").arg(target));
    }
    QDir(old).removeRecursively();
    return true;
}

void OmarchyBar::show() {
    if (!m_available) return;
    QString why;
    if (!install(&why)) { emit failed(why); return; }
    m_wanted = true;
    m_tries = 0;
    m_retry->stop();
    m_verb = QStringLiteral("enable");
    m_extra = {QStringLiteral("--section"), QStringLiteral("right")};
    // A plugin just put there is not known to the shell for a moment yet.
    m_retry->start();
}

void OmarchyBar::hide() {
    if (!m_available) return;
    m_wanted = false;
    m_tries = 0;
    m_retry->stop();
    run(QStringLiteral("disable"), {});
}

void OmarchyBar::run(const QString &verb, const QStringList &extra) {
    // Kept first: with the last command still running, the one tried again
    // afterwards must be this one, not that one once more.
    m_verb = verb;
    m_extra = extra;
    if (m_process) { m_retry->start(); return; }
    ++m_tries;
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        finished(status == QProcess::NormalExit ? exitCode : -1);
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finished(-1);
    });
    m_process->start(m_command, QStringList{QStringLiteral("plugin"), verb, QLatin1String(kPluginId)} + extra);
}

void OmarchyBar::finished(int exitCode) {
    if (!m_process) return;
    const QString said = QString::fromUtf8(m_process->readAll()).trimmed();
    m_process->deleteLater();
    m_process = nullptr;
    readActive();
    if (m_active == m_wanted) return;
    if (m_tries < kTries) { m_retry->start(); return; }
    emit failed(said.isEmpty() ? tr("Omarchy's bar did not take the OmaBroadcast icon (%1).").arg(exitCode) : said);
}
