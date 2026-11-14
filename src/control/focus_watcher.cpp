#include "control/focus_watcher.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace {
QString signature() { return qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE"); }
}  // namespace

FocusWatcher::FocusWatcher(QObject *parent) : QObject(parent) {
    connect(&m_events, &QLocalSocket::readyRead, this, [this] {
        m_pending += m_events.readAll();
        for (qsizetype nl; (nl = m_pending.indexOf('\n')) >= 0; m_pending.remove(0, nl + 1)) handleEvent(m_pending.left(nl));
    });
    // Hyprland restarting its socket (or a slow start) is retried quietly.
    connect(&m_events, &QLocalSocket::disconnected, this, [this] {
        if (m_running) QTimer::singleShot(2000, this, &FocusWatcher::connectSocket);
    });
}

bool FocusWatcher::available() {
    return !signature().isEmpty() && !QStandardPaths::findExecutable(QStringLiteral("hyprctl")).isEmpty();
}

void FocusWatcher::start() {
    if (m_running || !available()) return;
    m_running = true;
    connectSocket();
    // What has the keyboard already; events only say when it changes.
    auto *ask = new QProcess(this);
    connect(ask, &QProcess::finished, this, [this, ask] {
        const QJsonObject w = QJsonDocument::fromJson(ask->readAllStandardOutput()).object();
        ask->deleteLater();
        if (m_running && !w.isEmpty()) handleEvent("activewindow>>" + w.value("class").toString().toUtf8() + ',' + w.value("title").toString().toUtf8());
    });
    ask->start(QStringLiteral("hyprctl"), {QStringLiteral("activewindow"), QStringLiteral("-j")});
}

void FocusWatcher::stop() {
    m_running = false;
    m_events.abort();
    m_pending.clear();
}

void FocusWatcher::connectSocket() {
    if (!m_running || m_events.state() != QLocalSocket::UnconnectedState) return;
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    m_events.connectToServer(runtime + QStringLiteral("/hypr/") + signature() + QStringLiteral("/.socket2.sock"));
}

void FocusWatcher::handleEvent(const QByteArray &line) {
    static const QByteArray kWindow = "activewindow>>";
    if (!line.startsWith(kWindow)) return;
    // The class has no comma; the title may.
    const QByteArray rest = line.mid(kWindow.size());
    const qsizetype comma = rest.indexOf(',');
    const QString cls = QString::fromUtf8(comma < 0 ? rest : rest.left(comma));
    const QString title = comma < 0 ? QString() : QString::fromUtf8(rest.mid(comma + 1));
    if (cls == m_class && title == m_title) return;
    m_class = cls;
    m_title = title;
    emit focused(cls, title);
}
