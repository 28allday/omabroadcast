#include "crash_guard.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace {
QString programOf(qint64 pid) {
    QString exe = QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).symLinkTarget();
    // Updated while it runs: the old file shows as deleted, still the same program.
    if (exe.endsWith(QLatin1String(" (deleted)"))) exe.chop(10);
    return exe;
}
}  // namespace

CrashGuard::CrashGuard(const QString &dir) : m_dir(dir) {}

CrashGuard::~CrashGuard() { end(); }

QString CrashGuard::bootId() {
    QFile f(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()).trimmed() : QString();
}

bool CrashGuard::writeMarker(const QString &path, qint64 pid, const QString &bootId, const QString &program) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QStringLiteral("%1\n%2\n%3\n").arg(pid).arg(bootId, program).toUtf8());
    return f.commit();
}

void CrashGuard::begin() {
    if (m_dir.isEmpty() || !QDir().mkpath(m_dir)) return;
    look();
    const qint64 self = QCoreApplication::applicationPid();
    m_marker = QDir(m_dir).filePath(QStringLiteral("running-%1").arg(self));
    if (!writeMarker(m_marker, self, bootId(), programOf(self))) m_marker.clear();
}

void CrashGuard::look() {
    const qint64 self = QCoreApplication::applicationPid();
    const QString boot = bootId();
    const QDir dir(m_dir);
    m_otherRunning = false;
    for (const QString &name : dir.entryList({QStringLiteral("running-*")}, QDir::Files)) {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QStringList fields = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
        f.close();
        const qint64 pid = fields.value(0).toLongLong();
        // This very run; one of an earlier boot can have had the same number.
        if (pid == self && fields.value(1) == boot) continue;
        // Still running: the same boot, and that process is the same program.
        const bool running = pid > 0 && fields.value(1) == boot && !fields.value(2).isEmpty() && programOf(pid) == fields.value(2);
        if (running) {
            m_otherRunning = true;
        } else {
            m_crashed = true;
            QFile::remove(dir.filePath(name));
        }
    }
}

void CrashGuard::end() {
    if (!m_marker.isEmpty()) QFile::remove(m_marker);
    m_marker.clear();
}
