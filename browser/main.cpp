// omabroadcast-browser — draws one browser source's page off screen for
// OmaBroadcast and hands it the pictures. A separate program so the app
// never links Qt WebEngine, and a page (or the browser) that falls over
// cannot take the app with it. Started by the app, never by people.

#include <QGuiApplication>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSocketNotifier>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

#include "capture/browser_wire.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>

namespace {

class Helper : public QObject {
    Q_OBJECT

public:
    Helper(int socket, const QString &storage) : m_socket(socket), m_notifier(socket, QSocketNotifier::Read) {
        m_window.setColor(Qt::transparent);
        m_window.resize(m_width, m_height);
        QQmlComponent component(&m_engine, QUrl(QStringLiteral("qrc:/Page.qml")));
        m_view = qobject_cast<QQuickItem *>(component.createWithInitialProperties({{QStringLiteral("storage"), storage}}));
        if (!m_view) {
            std::fprintf(stderr, "omabroadcast-browser: %s\n", qPrintable(component.errorString()));
            std::exit(2);
        }
        m_view->setParentItem(m_window.contentItem());
        m_view->setSize(QSizeF(m_width, m_height));
        connect(m_view, SIGNAL(report(QVariant)), this, SLOT(report(QVariant)));
        // Anything drawn since the last picture: one goes out at the next tick.
        connect(&m_window, &QQuickWindow::frameSwapped, this, [this] { m_dirty = true; });
        connect(&m_notifier, &QSocketNotifier::activated, this, &Helper::read);
        connect(&m_tick, &QTimer::timeout, this, &Helper::tick);
        m_tick.setTimerType(Qt::PreciseTimer);
        m_tick.start(1000 / m_fps);
        m_window.show();
    }

public slots:
    // The page's report (a QML signal); everything else is a lambda.
    void report(const QVariant &state) {
        QJsonObject m = QJsonObject::fromVariantMap(state.toMap());
        m[QStringLiteral("t")] = QStringLiteral("state");
        browserwire::send(m_socket, m);
    }

private:
    void read() {
        for (;;) {
            QJsonObject m;
            int fd = -1;
            bool closed = false;
            if (!browserwire::receive(m_socket, &m, &fd, &closed)) {
                if (closed) QCoreApplication::quit();  // the app has gone
                return;
            }
            if (fd >= 0) ::close(fd);
            handle(m);
        }
    }

    void handle(const QJsonObject &m) {
        const QString t = m.value(QStringLiteral("t")).toString();
        if (t == QLatin1String("page")) {
            const int w = std::clamp(m.value(QStringLiteral("width")).toInt(800), 1, browserwire::kMaxSide);
            const int h = std::clamp(m.value(QStringLiteral("height")).toInt(600), 1, browserwire::kMaxSide);
            if (w != m_width || h != m_height) {
                m_width = w; m_height = h;
                m_window.resize(w, h);
                m_view->setSize(QSizeF(w, h));
                m_dirty = true;
            }
            const int fps = std::clamp(m.value(QStringLiteral("fps")).toInt(30), 1, 120);
            if (fps != m_fps) { m_fps = fps; m_tick.start(1000 / fps); }
            m_view->setProperty("audioMuted", m.value(QStringLiteral("muted")).toBool());
            const QString css = m.value(QStringLiteral("css")).toString();
            const QUrl url = QUrl(m.value(QStringLiteral("url")).toString());
            // CSS first, so the page loads with it.
            if (!m_cssSet || css != m_view->property("css").toString()) {
                m_cssSet = true;
                QMetaObject::invokeMethod(m_view, "applyCss", Q_ARG(QVariant, css));
            }
            // The first always loads, even an empty one: until a page loads,
            // the app is told nothing and says "loading" for ever.
            if (!m_urlSet || url != m_url) {
                m_urlSet = true;
                m_url = url;
                m_view->setProperty("url", url.isEmpty() ? QUrl(QStringLiteral("about:blank")) : url);
            }
        } else if (t == QLatin1String("reload")) {
            QMetaObject::invokeMethod(m_view, "reloadAndBypassCache");
        } else if (t == QLatin1String("done")) {
            const int b = m.value(QStringLiteral("buffer")).toInt();
            if (b >= 0 && b < kBuffers) m_busy[b] = false;
        } else if (t == QLatin1String("mouse")) {
            const QString type = m.value(QStringLiteral("type")).toString();
            const QPointF p(m.value(QStringLiteral("x")).toDouble(), m.value(QStringLiteral("y")).toDouble());
            const QEvent::Type et = type == QLatin1String("press") ? QEvent::MouseButtonPress
                                  : type == QLatin1String("release") ? QEvent::MouseButtonRelease
                                  : type == QLatin1String("double") ? QEvent::MouseButtonDblClick : QEvent::MouseMove;
            QMouseEvent e(et, p, p, m_window.mapToGlobal(p), Qt::MouseButton(m.value(QStringLiteral("button")).toInt()),
                          Qt::MouseButtons(m.value(QStringLiteral("buttons")).toInt()),
                          Qt::KeyboardModifiers(m.value(QStringLiteral("modifiers")).toInt()));
            QCoreApplication::sendEvent(&m_window, &e);
        } else if (t == QLatin1String("wheel")) {
            const QPointF p(m.value(QStringLiteral("x")).toDouble(), m.value(QStringLiteral("y")).toDouble());
            const QPoint delta(m.value(QStringLiteral("dx")).toInt(), m.value(QStringLiteral("dy")).toInt());
            QWheelEvent e(p, m_window.mapToGlobal(p), QPoint(), delta, Qt::NoButton,
                          Qt::KeyboardModifiers(m.value(QStringLiteral("modifiers")).toInt()), Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(&m_window, &e);
        } else if (t == QLatin1String("key")) {
            QKeyEvent e(m.value(QStringLiteral("type")).toString() == QLatin1String("release") ? QEvent::KeyRelease : QEvent::KeyPress,
                        m.value(QStringLiteral("key")).toInt(), Qt::KeyboardModifiers(m.value(QStringLiteral("modifiers")).toInt()),
                        m.value(QStringLiteral("text")).toString(), m.value(QStringLiteral("repeat")).toBool());
            QCoreApplication::sendEvent(&m_window, &e);
        } else if (t == QLatin1String("focus")) {
            if (m.value(QStringLiteral("on")).toBool()) { m_window.requestActivate(); m_view->forceActiveFocus(); }
            else m_view->setFocus(false);
        }
    }

    // Shared memory for kBuffers pictures at the page's size, handed to the app.
    bool ensureBuffers(int w, int h) {
        if (m_map && w == m_bufferWidth && h == m_bufferHeight) return true;
        if (m_map) ::munmap(m_map, m_mapSize);
        m_map = nullptr;
        const size_t one = size_t(w) * h * 4;
        const int fd = ::memfd_create("omabroadcast-browser", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        // Sealed at its size: the app will not read memory that could be taken from under it.
        if (fd < 0 || ::ftruncate(fd, off_t(one * kBuffers)) != 0 || ::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) {
            if (fd >= 0) ::close(fd);
            return false;
        }
        void *map = ::mmap(nullptr, one * kBuffers, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { ::close(fd); return false; }
        m_map = static_cast<uchar *>(map);
        m_mapSize = one * kBuffers;
        m_bufferWidth = w; m_bufferHeight = h;
        for (bool &b : m_busy) b = false;
        browserwire::send(m_socket, {{QStringLiteral("t"), QStringLiteral("buffers")}, {QStringLiteral("width"), w},
                                     {QStringLiteral("height"), h}, {QStringLiteral("stride"), w * 4},
                                     {QStringLiteral("count"), kBuffers}}, fd);
        ::close(fd);
        return true;
    }

    void tick() {
        if (!m_dirty) return;
        int free = -1;
        for (int b = 0; b < kBuffers && free < 0; ++b) if (!m_busy[b]) free = b;
        if (free < 0) return;  // the app is behind; the newest picture goes when it catches up
        m_dirty = false;
        QImage picture = m_window.grabWindow();
        if (picture.isNull()) return;
        picture = picture.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (!ensureBuffers(picture.width(), picture.height())) return;
        uchar *to = m_map + size_t(free) * m_bufferWidth * m_bufferHeight * 4;
        for (int y = 0; y < m_bufferHeight; ++y) std::memcpy(to + size_t(y) * m_bufferWidth * 4, picture.constScanLine(y), size_t(m_bufferWidth) * 4);
        m_busy[free] = true;
        browserwire::send(m_socket, {{QStringLiteral("t"), QStringLiteral("frame")}, {QStringLiteral("buffer"), free}});
    }

    static constexpr int kBuffers = 2;
    static_assert(kBuffers <= browserwire::kMaxBuffers);
    int m_socket;
    QSocketNotifier m_notifier;
    QQuickWindow m_window;
    QQmlEngine m_engine;
    QQuickItem *m_view = nullptr;
    QTimer m_tick;
    int m_width = 800, m_height = 600, m_fps = 30;
    bool m_dirty = true, m_cssSet = false, m_urlSet = false;
    QUrl m_url;
    uchar *m_map = nullptr;
    size_t m_mapSize = 0;
    int m_bufferWidth = 0, m_bufferHeight = 0;
    bool m_busy[kBuffers] = {};
};

}  // namespace

int main(int argc, char *argv[]) {
    // `--check`: the app asking whether this machine can run pages at all
    // (the program starts only when Qt WebEngine is installed).
    if (argc > 1 && std::strcmp(argv[1], "--check") == 0) { std::puts("ok"); return 0; }
    int socket = -1;
    QString storage;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0) socket = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--storage") == 0) storage = QString::fromLocal8Bit(argv[++i]);
    }
    if (socket < 0) {
        std::fprintf(stderr, "omabroadcast-browser is started by OmaBroadcast.\n");
        return 1;
    }
    // Gone with the app, however it ends.
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
    std::signal(SIGTERM, [](int) { std::_Exit(0); });
    // Nothing here is ever shown: the page draws into memory.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("omabroadcast-browser"));
    app.setDesktopFileName(QStringLiteral("omabroadcast"));
    Helper helper(socket, storage);
    return app.exec();
}

#include "main.moc"
