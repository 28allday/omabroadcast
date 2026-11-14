#include "app_log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegularExpression>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace {
std::atomic<AppLog *> g_log{nullptr};
std::atomic<int> g_fd{-1};  // read by the signal handler
QtMessageHandler g_previousHandler = nullptr;
constexpr int kSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
struct sigaction g_previousActions[std::size(kSignals)];

void onMessage(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (AppLog *log = g_log.load()) log->append(AppLog::levelOf(type), qFormatLogMessage(type, context, message));
    if (g_previousHandler) g_previousHandler(type, context, message);
}

// Only async-signal-safe calls: a fixed line and the number, by write(2),
// then whatever handled the signal before (by default, the core dump).
void onFatalSignal(int sig) {
    const int fd = g_fd.load(std::memory_order_relaxed);
    if (fd >= 0) {
        char line[32] = "fatal signal ";
        int n = 13;
        char digits[12];
        int d = 0;
        for (int v = sig; v > 0 && d < 11; v /= 10) digits[d++] = char('0' + v % 10);
        while (d > 0) line[n++] = digits[--d];
        line[n++] = '\n';
        [[maybe_unused]] const ssize_t written = ::write(fd, line, size_t(n));
    }
    for (size_t i = 0; i < std::size(kSignals); ++i)
        if (kSignals[i] == sig) ::sigaction(sig, &g_previousActions[i], nullptr);
    ::raise(sig);
}

const char *const kLevelNames[] = {"debug", "info", "warning", "critical", "fatal"};
constexpr qsizetype kLineLimit = 16 * 1024;  // a longer message is cut in the file
}  // namespace

AppLog::AppLog(int capacity) : m_capacity(std::max(1, capacity)) {}

AppLog::~AppLog() {
    if (m_installed) {
        g_log.store(nullptr);
        g_fd.store(-1);
        qInstallMessageHandler(g_previousHandler);
        g_previousHandler = nullptr;
        for (size_t i = 0; i < std::size(kSignals); ++i) ::sigaction(kSignals[i], &g_previousActions[i], nullptr);
    }
    if (m_fd >= 0) ::close(m_fd);
}

AppLog *AppLog::instance() { return g_log.load(); }

bool AppLog::open(const QString &dir, qint64 fileCap, bool files) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cap = std::max<qint64>(fileCap, 1024);
    if (!files || dir.isEmpty() || !QDir().mkpath(dir)) return false;
    m_current = dir + QLatin1Char('/') + currentName();
    m_previous = dir + QLatin1Char('/') + previousName();
    // Only the run before is kept.
    for (const QString &part : {QString(), QStringLiteral(".1")}) {
        QFile::remove(m_previous + part);
        if (QFile::exists(m_current + part)) QFile::rename(m_current + part, m_previous + part);
    }
    startFile();
    return m_fd >= 0;
}

void AppLog::install() {
    if (m_installed) return;
    m_installed = true;
    g_log.store(this);
    g_fd.store(m_fd);
    g_previousHandler = qInstallMessageHandler(onMessage);
    struct sigaction action {};
    action.sa_handler = onFatalSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_NODEFER;
    for (size_t i = 0; i < std::size(kSignals); ++i) ::sigaction(kSignals[i], &action, &g_previousActions[i]);
}

void AppLog::startFile() {
    const int fd = ::open(QFile::encodeName(m_current).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0600);
    const int old = m_fd;
    m_fd = fd;
    if (m_installed) g_fd.store(fd);  // before the old one closes
    if (old >= 0) ::close(old);
    m_written = 0;
}

void AppLog::append(int level, const QString &text) {
    Line line{QDateTime::currentMSecsSinceEpoch(), std::clamp(level, int(Debug), int(Fatal)), text};
    // Cut here, as in the file: the lines kept in memory are as many, not as long.
    if (line.text.size() > kLineLimit) line.text = line.text.left(kLineLimit) + QStringLiteral(" …");
    std::lock_guard<std::mutex> lock(m_mutex);
    writeLine(line);
    m_lines.push_back(std::move(line));
    while (int(m_lines.size()) > m_capacity) m_lines.pop_front();
    ++m_serial;
}

void AppLog::writeLine(const Line &line) {
    if (m_fd < 0) return;
    const QByteArray bytes = format(line).toUtf8() + '\n';
    if (m_written > 0 && m_written + bytes.size() > m_cap) {
        // Full: this file becomes .1 (the one before goes) and a new one starts.
        ::rename(QFile::encodeName(m_current).constData(), QFile::encodeName(m_current + QStringLiteral(".1")).constData());
        startFile();
        if (m_fd < 0) return;
    }
    qsizetype done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(m_fd, bytes.constData() + done, size_t(bytes.size() - done));
        if (n <= 0) break;
        done += n;
    }
    m_written += done;
}

QList<AppLog::Line> AppLog::lines() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return QList<Line>(m_lines.begin(), m_lines.end());
}

QString AppLog::levelName(int level) { return QString::fromLatin1(kLevelNames[std::clamp(level, int(Debug), int(Fatal))]); }

int AppLog::levelOf(QtMsgType type) {
    switch (type) {
    case QtDebugMsg: return Debug;
    case QtInfoMsg: return Info;
    case QtWarningMsg: return Warning;
    case QtCriticalMsg: return Critical;
    case QtFatalMsg: return Fatal;
    }
    return Info;
}

QString AppLog::format(const Line &line) {
    QString text = line.text;
    text.replace(QLatin1Char('\n'), QStringLiteral("\n\t"));  // a message's further lines, indented
    const QString when = line.msecs > 0 ? QDateTime::fromMSecsSinceEpoch(line.msecs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
                                        : QStringLiteral("-");
    return when + QLatin1Char(' ') + levelName(line.level) + QLatin1Char(' ') + text;
}

QList<AppLog::Line> AppLog::readFile(const QString &path) {
    static const QRegularExpression head(QStringLiteral("^(\\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d\\.\\d{3}|-) (debug|info|warning|critical|fatal) (.*)$"));
    static const QRegularExpression fatal(QStringLiteral("^fatal signal \\d+$"));
    QList<Line> out;
    for (const QString &file : {path + QStringLiteral(".1"), path}) {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QStringList rows = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
        for (const QString &row : rows) {
            if (const auto m = head.match(row); m.hasMatch()) {
                Line line;
                const QDateTime when = QDateTime::fromString(m.captured(1), QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
                line.msecs = when.isValid() ? when.toMSecsSinceEpoch() : 0;
                line.level = int(std::find(std::begin(kLevelNames), std::end(kLevelNames), m.captured(2).toLatin1()) - std::begin(kLevelNames));
                line.level = std::clamp(line.level, int(Debug), int(Fatal));
                line.text = m.captured(3);
                out.append(line);
            } else if (fatal.match(row).hasMatch()) {
                out.append({0, Fatal, row});
            } else if (row.startsWith(QLatin1Char('\t')) && !out.isEmpty()) {
                out.last().text += QLatin1Char('\n') + row.mid(1);
            } else if (!row.isEmpty()) {
                out.append({0, Info, row});
            }
        }
    }
    return out;
}
