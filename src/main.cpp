// OmaBroadcast — record, stream and compose live video. Qt Quick UI, C++ engine.

#include <QGuiApplication>

#include "control/control_socket.h"
#include <QDeadlineTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QTranslator>
#include <QFont>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QProcess>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

#include "backend.h"
#include "app_version.h"
#include "launch_screen.h"
#include "core/app_log.h"
#include "core/crash_guard.h"
#include "core/machine_survey.h"
#include "core/session.h"
#include "core/start_options.h"
#include "output/encoder_helper.h"
#include "output/encoders.h"
#include "engine/engine.h"
#include "preview_view.h"
#include "scope_view.h"
#include "theme.h"

#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <QSocketNotifier>
#include <QLocalSocket>
#include <cstdio>
#include <cstdio>

namespace {
// `omabroadcast --probe`: print what this machine can capture and encode with.
int probe() {
    // What Automatic picks, as the session would (the saved choice aside).
    const std::vector<engine::GpuDevice> gpus = engine::listGpus();
    const bool onBattery = machinesurvey::readPower().onBattery;
    const engine::GpuPick pick = engine::chooseGpu(gpus, {}, onBattery);
    std::string why = "no usable GPU";
    auto gpu = pick.index >= 0 ? engine::createVulkanEngine(pick.index, &why) : nullptr;
    std::printf("engine: %s%s\n", gpu ? gpu->deviceName().c_str() : ("CPU (" + why + ")").c_str(),
                pick.why == engine::GpuPick::Why::OnBattery ? "  (integrated: on battery)" : "");
    if (gpu) {
        const auto mods = gpu->importModifiers(0x34325258);
        std::printf("dma-buf import: %zu modifiers for XR24\n", mods.size());
    }
    for (const engine::GpuDevice &g : gpus) {
        using Kind = engine::GpuDevice::Kind;
        const char *kind = g.kind == Kind::Discrete ? "discrete" : g.kind == Kind::Integrated ? "integrated"
                         : g.kind == Kind::Virtual  ? "virtual"  : "other";
        std::printf("gpu %d: %s  %s%s%s%s  id %s\n", g.index, g.name.c_str(), kind, g.usable ? "" : " (not usable)",
                    g.renderNode.empty() ? "" : "  ", g.renderNode.c_str(), g.id.c_str());
    }
    const std::vector<EncoderInfo> encoders = probeEncoders(gpus);
    for (const EncoderInfo &e : encoders) {
        std::string formats;
        for (AVPixelFormat f : e.formats) {
            engine::ColourFormat c;
            if (colourFormatOf(f, &c)) formats += std::string(formats.empty() ? "  " : " ") + colourFormatName(c);
        }
        std::printf("%-12s %-44s %s%s%s\n", e.name.c_str(), e.label.c_str(), e.available ? "available" : "no",
                    e.available ? "" : (" (" + e.note + ")").c_str(), formats.c_str());
    }
    const EngineDevice engine = gpu ? EngineDevice{gpu->deviceName(), gpu->renderNode(), pick.why == engine::GpuPick::Why::OnBattery}
                                    : EngineDevice{};
    for (const auto &[purpose, title] : {std::pair{Purpose::Record, "record (mkv)"}, std::pair{Purpose::Stream, "stream"}}) {
        std::printf("%s:\n", title);
        int n = 0;
        for (const RankedEncoder &r : rankEncoders(encoders, purpose, purpose == Purpose::Record ? "mkv" : "", engine))
            std::printf("  %d. %-40s %s\n", ++n, r.encoder.label.c_str(), r.reason.c_str());
    }
    return 0;
}
// Logging out, shutting down, Ctrl+C or `kill`: told to the event loop by a
// pipe (all a signal handler may safely do), which ends the event loop without
// asking the windows, whose questions no one is there to answer. The Backend
// then closes a recording properly and saves the settings. Another one a little
// later ends at once, should that hang. One right behind the first is the same
// request: a program stopping us can signal both us and our process group.
int g_quitPipe[2] = {-1, -1};
volatile sig_atomic_t g_quitAsked = 0;
timespec g_quitAskedAt{};

void onQuitSignal(int sig) {
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    if (g_quitAsked) {
        if (now.tv_sec - g_quitAskedAt.tv_sec < 2) return;
        ::signal(sig, SIG_DFL);
        ::raise(sig);
        return;
    }
    g_quitAskedAt = now;
    g_quitAsked = 1;
    const char c = 1;
    [[maybe_unused]] const ssize_t written = ::write(g_quitPipe[1], &c, 1);
}

void quitOnSignals(QCoreApplication *app) {
    if (::pipe2(g_quitPipe, O_CLOEXEC | O_NONBLOCK) != 0) return;
    auto *notifier = new QSocketNotifier(g_quitPipe[0], QSocketNotifier::Read, app);
    QObject::connect(notifier, &QSocketNotifier::activated, app, [] {
        char c;
        while (::read(g_quitPipe[0], &c, 1) > 0) {}
        qInfo("Asked to quit by a signal.");
        QCoreApplication::exit(0);
    });
    struct sigaction action{};
    action.sa_handler = onQuitSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (const int sig : {SIGTERM, SIGINT, SIGHUP}) ::sigaction(sig, &action, nullptr);
}

// Asks the copy running to come forward, and to take a file if one was named.
// True once it has answered: a copy can listen and still not be reading, on its
// way out or with its window still loading.
bool askRunningCopy(const QString &file, const QDeadlineTimer &until) {
    QLocalSocket running;
    running.connectToServer(ControlSocket::path());
    if (!running.waitForConnected(500)) return false;
    QByteArray requests = R"({"action":"window.raise"})" "\n";
    int answers = 1;
    if (!file.isEmpty()) {
        requests += QJsonDocument(QJsonObject{{"action", "file.open"}, {"argument", file}}).toJson(QJsonDocument::Compact) + '\n';
        ++answers;
    }
    running.write(requests);
    while (answers > 0 && !until.hasExpired()) {
        if (running.canReadLine()) { running.readLine(); --answers; continue; }
        if (running.state() != QLocalSocket::ConnectedState) return false;
        running.waitForReadyRead(200);
    }
    return answers == 0;
}
}  // namespace

int main(int argc, char *argv[]) {
    // SVT-AV1 narrates every open on its own logger (version banners, memory
    // statistics), past FFmpeg's log level. Errors only, unless the person
    // asked for more themselves; the helper and `--probe` inherit it.
    if (qEnvironmentVariableIsEmpty("SVT_LOG")) qputenv("SVT_LOG", "1");
    // A failed connection names its address on stderr, stream key and all.
    keepSecretsOutOfAvLog();
    // The encoder probe and fit checks run here in a child process, so their
    // driver locks never stall the parent's frame loop. No window, no display.
    if (encoderhelper::isHelperCall(argc, argv)) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QCoreApplication helper(argc, argv);
        return encoderhelper::run(argc, argv);
    }
    // `omabroadcast ctl …` asks the running app; no window either.
    if (argc > 1 && QByteArray(argv[1]) == "ctl") {
        QCoreApplication ctl(argc, argv);
        return runControlClient(ctl.arguments().mid(2));
    }
    // Custom browser docks load Qt WebEngine's QML here when it is installed;
    // it needs shared OpenGL contexts, asked for before the application.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    // `--probe` prints and leaves: over SSH, with no display, it runs offscreen.
    if (argc > 1 && QByteArray(argv[1]) == "--probe" && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") &&
        qEnvironmentVariableIsEmpty("DISPLAY")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("QT_QPA_PLATFORMTHEME", "");  // the GTK theme plugin exits without a display
    }
    QGuiApplication app(argc, argv);
    LeaveAroundHeldWork leave;
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--probe")) return probe();

    // The identity trio. setDesktopFileName is what becomes the Wayland app_id,
    // which is what Hyprland rules, the taskbar and the icon lookup key off.
    app.setApplicationName(QStringLiteral("omabroadcast"));
    app.setApplicationDisplayName(QStringLiteral("OmaBroadcast"));
    app.setDesktopFileName(QStringLiteral("omabroadcast"));
    app.setApplicationVersion(QStringLiteral(OMABROADCAST_VERSION));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("omabroadcast")));

    // A restart: the copy before finishes quitting first.
    const StartOptions options = StartOptions::parse(app.arguments());
    for (int i = 0; options.waitFor > 0 && i < 300 && ::kill(pid_t(options.waitFor), 0) == 0; ++i) QThread::msleep(100);

    // Whether the last run quit cleanly, and the log: stderr as before, and
    // Help → Log's lines and this run's file in the state folder, beside the
    // run before's (unless another copy is running and writing there).
    const QString stateDir = QStandardPaths::writableLocation(QStandardPaths::StateLocation);
    CrashGuard crashGuard(stateDir);
    crashGuard.begin();
    // From the command line in any folder, it is kept by its full path.
    const QString openFile = options.open.isEmpty() ? QString() : QFileInfo(options.open).absoluteFilePath();
    // One copy at a time (a second would save over the first's settings and
    // studios as it quits): the one running comes forward instead, whatever this
    // one was asked for. One on its way out answers nothing: this one waits for it
    // to finish, then starts. One that never answers does not hold this one for ever.
    for (const QDeadlineTimer until(30000); crashGuard.otherCopyRunning() && !until.hasExpired(); crashGuard.look()) {
        if (askRunningCopy(openFile, until)) {
            std::fprintf(stderr, options.safeMode ? "OmaBroadcast is already running: quit it to start in safe mode.\n"
                                                  : "OmaBroadcast is already running.\n");
            crashGuard.end();
            return options.safeMode ? 1 : 0;
        }
        QThread::msleep(100);
    }
    AppLog log;
    log.open(stateDir, 512 * 1024, !crashGuard.otherCopyRunning());
    log.install();
    if (crashGuard.previousRunCrashed()) qWarning("The last run did not quit cleanly.");
    quitOnSignals(&app);

    // The interface in the desktop's language when a translation is built in
    // (see TRANSLATIONS in omabroadcast.pro); English otherwise.
    QTranslator translator;
    if (translator.load(QLocale(), QStringLiteral("omabroadcast"), QStringLiteral("_"), QStringLiteral(":/i18n")))
        app.installTranslator(&translator);

    // OmaBroadcast's own Controls style (src/style/OmaBroadcastStyle, in qrc
    // under the default qt/qml import path): the Oma suite look, falling back
    // to Basic for anything it does not restyle.
    QQuickStyle::setStyle(QStringLiteral("OmaBroadcastStyle"));

    OmarchyTheme theme;
    Backend backend;
    backend.setSafeMode(options.safeMode);
    backend.setPreviousRunCrashed(crashGuard.previousRunCrashed());
    // Before anything that takes a while, the crash sheet above all: a copy started
    // now finds this one by it.
    if (QString why; !backend.listenForControl(&why)) qInfo("control socket: %s", qPrintable(why));

    qmlRegisterType<PreviewView>("OmaBroadcast", 1, 0, "PreviewView");
    qmlRegisterType<ScopeView>("OmaBroadcast", 1, 0, "ScopeView");
    qmlRegisterSingletonInstance("OmaBroadcast", 1, 0, "OmarchyTheme", &theme);
    qmlRegisterSingletonType(QUrl(QStringLiteral("qrc:/Theme.qml")), "OmaBroadcast", 1, 0, "Theme");
    qmlRegisterSingletonType(QUrl(QStringLiteral("qrc:/ui/icons/Icons.qml")), "OmaBroadcast", 1, 0, "Icons");
    qmlRegisterType(QUrl(QStringLiteral("qrc:/ui/Icon.qml")), "OmaBroadcast", 1, 0, "Icon");
    qmlRegisterSingletonType(QUrl(QStringLiteral("qrc:/ui/Sheets.qml")), "OmaBroadcast", 1, 0, "Sheets");

    // Carry the desktop's text size into the default font so every control
    // grows with `omarchy display text size`, without a restart.
    const qreal basePointSize = app.font().pointSizeF();
    const auto scaleFont = [&app, basePointSize](qreal scale) {
        QFont font = app.font();
        font.setPointSizeF(basePointSize * scale);
        app.setFont(font);
    };
    scaleFont(theme.textScale());
    QObject::connect(&theme, &OmarchyTheme::textScaleChanged, &app, [&theme, scaleFont] {
        scaleFont(theme.textScale());
    });

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                     [](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings)
            qWarning().noquote() << warning.toString();
    });
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);

    backend.applyCaptureHiding();

    // Not on the way back from a restart, in safe mode or after a crash, when
    // what the person came for is the next thing on screen.
    if (options.waitFor == 0 && !options.safeMode && !crashGuard.previousRunCrashed()
        && qEnvironmentVariableIsEmpty("OMABROADCAST_NO_LAUNCH_WINDOW")) {
        const int answer = LaunchScreen::run(engine, stateDir);
        if (answer != LaunchScreen::Continue) {
            leave.code = answer;
            return answer;
        }
    }

    engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        qCritical() << "Could not load the omabroadcast interface.";
        return -1;
    }

    // After a crash the session waits for the crash sheet's choice.
    if (!backend.crashOffered()) backend.start();

    // "Open with", and `omabroadcast somefile` from a terminal.
    if (!openFile.isEmpty())
        backend.open(QUrl::fromLocalFile(openFile));

    const int code = app.exec();
    leave.code = code;
    // Safe mode's "Restart normally": a new copy waits for this one to end.
    if (backend.restartRequested())
        QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                StartOptions::restartArguments(QCoreApplication::applicationPid()));
    return code;
}
