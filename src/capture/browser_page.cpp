#include "browser_page.h"

#include "audio/private_sink.h"
#include "capture/browser_wire.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonObject>
#include <QPointer>
#include <QSocketNotifier>

#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <utility>

namespace {
std::atomic<int> gSinks{0};
}

QString BrowserPage::helper() {
    static const QString found = [] {
        if (qEnvironmentVariable("OMABROADCAST_NO_BROWSER") == QLatin1String("1")) return QString();
        QStringList candidates;
        if (qEnvironmentVariableIsSet("OMABROADCAST_BROWSER_HELPER")) {
            candidates << qEnvironmentVariable("OMABROADCAST_BROWSER_HELPER");
        } else {
            const QString dir = QCoreApplication::applicationDirPath();
            candidates << dir + QStringLiteral("/omabroadcast-browser") << dir + QStringLiteral("/browser/omabroadcast-browser")
                       << QStringLiteral("/usr/lib/omabroadcast/omabroadcast-browser");
        }
        for (const QString &path : candidates) {
            if (path.isEmpty() || !QFileInfo(path).isExecutable()) continue;
            // It starts only where Qt WebEngine is installed: removing the
            // optional package leaves the program behind, unable to run.
            QProcess check;
            check.start(path, {QStringLiteral("--check")});
            if (check.waitForFinished(5000) && check.exitStatus() == QProcess::NormalExit && check.exitCode() == 0) return path;
        }
        return QString();
    }();
    return found;
}

BrowserPage::BrowserPage(const QString &storage, const QString &soundName, int channels, QObject *parent)
    : QObject(parent), m_storage(storage) {
    m_restart.setSingleShot(true);
    connect(&m_restart, &QTimer::timeout, this, &BrowserPage::launch);
    // The page's sound: into a sink of its own once that exists. Pages wait
    // a moment for it, then start muted rather than play to the speakers.
    m_sinkTimeout.setSingleShot(true);
    m_sinkTimeout.setInterval(3000);
    connect(&m_sinkTimeout, &QTimer::timeout, this, &BrowserPage::sinkWaitOver);
    if (PrivateSink::pulseAvailable()) {
        m_sinkName = QStringLiteral("omabroadcast-browser-%1-%2").arg(QCoreApplication::applicationPid()).arg(++gSinks);
        QPointer<BrowserPage> self(this);
        m_sink = std::make_shared<PrivateSink>(m_sinkName.toStdString(), tr("OmaBroadcast: %1").arg(soundName).toStdString(),
                                               channels, [self](bool ok) {
            QMetaObject::invokeMethod(qApp, [self, ok] {
                if (!self) return;
                self->m_sinkReady = ok;
                self->m_sinkFailed = !ok;
                emit self->soundChanged();
                if (self->m_waitingForSink) {
                    self->m_waitingForSink = false;
                    self->m_sinkTimeout.stop();
                    self->launch();
                } else if (ok && self->m_process && !self->m_helperHasSink) {
                    // Later than the page would wait. Its sound can only reach the sink
                    // from a new helper: helperGone starts one, which plays into it.
                    self->m_process->kill();
                }
            }, Qt::QueuedConnection);
        });
    } else {
        m_sinkFailed = true;
    }
    m_state = tr("starting");
}

BrowserPage::~BrowserPage() {
    m_restart.stop();
    if (m_notifier) { delete m_notifier; m_notifier = nullptr; }
    if (m_socket >= 0) {
        // Silent first; the helper quits when its socket closes.
        browserwire::send(m_socket, {{QStringLiteral("t"), QStringLiteral("page")}, {QStringLiteral("url"), m_settings.url},
                                     {QStringLiteral("width"), m_settings.width}, {QStringLiteral("height"), m_settings.height},
                                     {QStringLiteral("fps"), m_settings.fps}, {QStringLiteral("css"), m_settings.css},
                                     {QStringLiteral("muted"), true}}, -1, false);
        ::close(m_socket);
        m_socket = -1;
    }
    letGo(std::exchange(m_process, nullptr), std::move(m_sink));
    dropBuffers();
}

void BrowserPage::letGo(QProcess *process, std::shared_ptr<PrivateSink> sink) {
    if (!process) return;
    process->disconnect();
    // Never waited for here: this may be the frame loop's moment. The page
    // gets a few seconds to write its site data, then it is stopped.
    process->setParent(qApp);
    if (process->state() == QProcess::NotRunning) { process->deleteLater(); return; }
    QObject::connect(process, &QProcess::finished, process, [process, sink]() mutable { sink.reset(); process->deleteLater(); });
    QTimer::singleShot(5000, process, [process] { process->kill(); });
}

void BrowserPage::sinkWaitOver() {
    m_sinkFailed = !m_sinkReady;
    if (m_waitingForSink) { m_waitingForSink = false; launch(); }
}

void BrowserPage::setSettings(const Settings &settings) {
    const bool changed = !(settings == m_settings);
    m_settings = settings;
    if (!m_process && !m_restart.isActive() && !m_waitingForSink) launch();
    else if (changed) sendSettings();
}

void BrowserPage::reload() {
    if (m_socket >= 0) post({{QStringLiteral("t"), QStringLiteral("reload")}});
}

void BrowserPage::post(const QJsonObject &message, bool droppable) {
    if (m_socket < 0 || browserwire::send(m_socket, message, -1, false)) return;
    if (errno == EMSGSIZE) {
        // The helper is well; this can never reach it. Only the page's settings get this long.
        if (!droppable) { m_tooLong = true; setState(tr("failed: the page's address or CSS is too long")); }
        return;
    }
    if ((errno != EAGAIN && errno != EWOULDBLOCK) || droppable) return;  // gone: its end says so
    if (m_process) m_process->kill();  // stuck: helperGone starts it again
}

void BrowserPage::input(const QVariantMap &event) {
    if (m_socket < 0) return;
    const QJsonObject m = QJsonObject::fromVariantMap(event);
    static const QStringList kinds{QStringLiteral("mouse"), QStringLiteral("wheel"), QStringLiteral("key"), QStringLiteral("focus")};
    if (kinds.contains(m.value(QStringLiteral("t")).toString())) post(m, true);
}

void BrowserPage::launch() {
    const QString program = helper();
    if (program.isEmpty()) { setState(tr("failed: browser sources need Qt WebEngine (qt6-webengine)")); return; }
    if (m_process) return;
    // Sound first: with no sink yet the page would play to the speakers.
    if (m_sink && !m_sinkReady && !m_sinkFailed) {
        m_waitingForSink = true;
        if (!m_sinkTimeout.isActive()) m_sinkTimeout.start();
        return;
    }
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) != 0) { setState(tr("failed: no socket for the page")); return; }
    if (m_notifier) { delete m_notifier; m_notifier = nullptr; }
    if (m_socket >= 0) ::close(m_socket);
    m_socket = fds[0];
    const int child = fds[1];
    // Only the helper's end crosses into it, as the descriptor it is told.
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    m_process->setStandardOutputFile(QProcess::nullDevice());
    QProcess *process = m_process;
    connect(process, &QProcess::finished, this, [this, process] { helperGone(process); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) helperGone(process);
    });
    m_process->setChildProcessModifier([child] { ::fcntl(child, F_SETFD, 0); });
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char *key : {"QT_QPA_PLATFORM", "QT_QUICK_BACKEND", "QSG_RHI_BACKEND", "PULSE_SINK"}) env.remove(QString::fromLatin1(key));
    m_helperHasSink = m_sinkReady;
    if (m_helperHasSink) env.insert(QStringLiteral("PULSE_SINK"), m_sinkName);
    // Its streams are the app's own: never offered as an application to capture.
    env.insert(QStringLiteral("PULSE_PROP_application.id"), QStringLiteral("org.omarchy.OmaBroadcast"));
    // Should its sink go first, its sound stops instead of moving to the speakers.
    env.insert(QStringLiteral("PULSE_PROP_node.dont-reconnect"), QStringLiteral("true"));
    m_process->setProcessEnvironment(env);
    QStringList arguments{QStringLiteral("--socket"), QString::number(child)};
    if (!m_storage.isEmpty()) arguments << QStringLiteral("--storage") << m_storage;
    ++m_starts;
    dropBuffers();
    m_loading = true;
    m_error.clear();
    setState(tr("loading"));
    m_process->start(program, arguments);
    ::close(child);
    m_notifier = new QSocketNotifier(m_socket, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &BrowserPage::read);
    sendSettings();
}

void BrowserPage::sendSettings() {
    if (m_socket < 0) return;
    const bool wasTooLong = std::exchange(m_tooLong, false);
    // Heard only by way of a sink it was started with: with none it would play to the speakers.
    post({{QStringLiteral("t"), QStringLiteral("page")}, {QStringLiteral("url"), m_settings.url},
          {QStringLiteral("width"), m_settings.width}, {QStringLiteral("height"), m_settings.height},
          {QStringLiteral("fps"), m_settings.fps}, {QStringLiteral("css"), m_settings.css},
          {QStringLiteral("muted"), !(m_helperHasSink && m_sinkReady)}});
    if (wasTooLong && !m_tooLong)
        setState(!m_error.isEmpty() ? tr("failed: %1").arg(m_error) : m_loading ? tr("loading") : tr("live"));
}

void BrowserPage::read() {
    for (int n = 0; n < 64 && m_socket >= 0; ++n) {
        QJsonObject m;
        int fd = -1;
        bool closed = false;
        if (!browserwire::receive(m_socket, &m, &fd, &closed)) {
            if (closed && m_notifier) m_notifier->setEnabled(false);  // the process's end says the rest
            return;
        }
        const QString t = m.value(QStringLiteral("t")).toString();
        if (t == QLatin1String("buffers") && fd >= 0) {
            dropBuffers();
            const int w = m.value(QStringLiteral("width")).toInt(), h = m.value(QStringLiteral("height")).toInt();
            const int stride = m.value(QStringLiteral("stride")).toInt(), count = m.value(QStringLiteral("count")).toInt();
            const size_t size = browserwire::picturesSize(w, h, stride, count);
            struct stat st{};
            // As big as it says, and unable to shrink later: reading past the end
            // of the memory is a crash. The seal first, so the size then holds.
            const int seals = ::fcntl(fd, F_GET_SEALS);
            if (size > 0 && seals >= 0 && (seals & F_SEAL_SHRINK) && ::fstat(fd, &st) == 0 && st.st_size >= 0 &&
                size_t(st.st_size) >= size) {
                void *map = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
                if (map != MAP_FAILED) {
                    m_map = static_cast<uchar *>(map);
                    m_mapSize = size;
                    m_bufferWidth = w; m_bufferHeight = h; m_stride = stride; m_count = count;
                }
            }
            if (!m_map) setState(tr("failed: the page's pictures could not be read"));
        } else if (t == QLatin1String("frame")) {
            const int b = m.value(QStringLiteral("buffer")).toInt();
            if (m_map && b >= 0 && b < m_count) {
                if (onFrame) onFrame(m_map + size_t(b) * m_stride * m_bufferHeight, m_bufferWidth, m_bufferHeight, m_stride);
                m_failures = 0;  // it has been drawing: a later death starts again promptly
                if (!m_tooLong && !m_loading && m_error.isEmpty()) setState(tr("live"));
            }
            post({{QStringLiteral("t"), QStringLiteral("done")}, {QStringLiteral("buffer"), b}});
        } else if (t == QLatin1String("state")) {
            m_loading = m.value(QStringLiteral("loading")).toBool();
            m_error = m.value(QStringLiteral("error")).toString();
            m_title = m.value(QStringLiteral("title")).toString();
            if (!m_tooLong)
                setState(m.value(QStringLiteral("crashed")).toBool() ? tr("the page crashed; loading it again")
                         : !m_error.isEmpty() ? tr("failed: %1").arg(m_error)
                         : m_loading ? tr("loading") : tr("live"));
        }
        if (fd >= 0) ::close(fd);
    }
}

void BrowserPage::dropBuffers() {
    if (m_map) ::munmap(m_map, m_mapSize);
    m_map = nullptr;
    m_mapSize = 0;
    m_count = 0;
}

void BrowserPage::helperGone(QProcess *process) {
    if (process != m_process) return;
    m_process = nullptr;
    process->disconnect(this);
    process->deleteLater();
    if (m_notifier) { delete m_notifier; m_notifier = nullptr; }
    if (m_socket >= 0) { ::close(m_socket); m_socket = -1; }
    dropBuffers();
    // Started again, sooner while it had been drawing, later if it keeps dying.
    const int delay = std::min(10000, 500 << std::min(m_failures, 5));
    ++m_failures;
    setState(tr("the browser stopped; starting it again"));
    m_restart.start(delay);
}

void BrowserPage::setState(const QString &state) {
    if (state == m_state) return;
    m_state = state;
    emit stateChanged();
}
