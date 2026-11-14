#include "control/scripts.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QTimer>

#include <set>

namespace {
// Calls into the main thread, waiting for the answer.
template <typename R, typename F> R onMain(F f) {
    R result{};
    QObject *app = QCoreApplication::instance();
    if (QThread::currentThread() == app->thread()) return f();
    QMetaObject::invokeMethod(app, [&] { result = f(); }, Qt::BlockingQueuedConnection);
    return result;
}
}  // namespace

Script::Script(const QString &path, const ScriptCalls &calls, ScriptHost *host)
    : m_path(path), m_calls(calls), m_host(host) {}

void Script::load() {
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) { log(tr("Could not open the script: %1").arg(file.errorString())); return; }
    QJSEngine::setObjectOwnership(this, QJSEngine::CppOwnership);  // the host deletes it, not the collector
    // Only these, bound: the object itself (its deleteLater, its signals) is
    // never within reach of the script.
    const QJSValue bind = m_engine.evaluate(QStringLiteral(
        "(function(host) { var api = {};"
        " ['run','state','actions','on','log','setTimeout','setInterval','clearTimer','settings','setting']"
        ".forEach(function(name) { api[name] = host[name].bind(host); });"
        " return Object.freeze(api); })"));
    m_engine.globalObject().setProperty(QStringLiteral("omabroadcast"), bind.call({m_engine.newQObject(this)}));
    // console.log and print write to the script log; timers are the usual names.
    m_engine.evaluate(QStringLiteral(
        "var console = { log: function() { omabroadcast.log(Array.prototype.join.call(arguments, ' ')); } };"
        "console.info = console.warn = console.error = console.log;"
        "function print() { omabroadcast.log(Array.prototype.join.call(arguments, ' ')); }"
        "function setTimeout(f, ms) { return omabroadcast.setTimeout(f, ms || 0); }"
        "function setInterval(f, ms) { return omabroadcast.setInterval(f, ms || 0); }"
        "function clearTimeout(id) { omabroadcast.clearTimer(id); }"
        "function clearInterval(id) { omabroadcast.clearTimer(id); }"));
    busySince.store(QDateTime::currentMSecsSinceEpoch());
    const QJSValue result = m_engine.evaluate(QString::fromUtf8(file.readAll()), m_path);
    busySince.store(0);
    if (result.isError())
        log(tr("Error at line %1: %2").arg(result.property(QStringLiteral("lineNumber")).toInt()).arg(result.toString()));
    m_engine.setInterrupted(false);
}

QString Script::run(const QString &action, const QString &argument) {
    auto calls = m_calls;
    return onMain<QString>([&] { return calls.run(action, argument); });
}

QVariantMap Script::state() {
    auto calls = m_calls;
    return onMain<QVariantMap>([&] { return calls.state(); });
}

QVariantList Script::actions() {
    auto calls = m_calls;
    return onMain<QVariantList>([&] { return calls.actions(); });
}

QVariantMap Script::settings(const QVariantMap &form) {
    auto calls = m_calls;
    const QString path = m_path;
    QMetaObject::invokeMethod(QCoreApplication::instance(), [calls, path, form] { calls.declare(path, form); }, Qt::QueuedConnection);
    // Saved values first, then each field's default.
    QVariantMap values = onMain<QVariantMap>([&] { return calls.settings(path); });
    for (auto it = form.begin(); it != form.end(); ++it)
        if (!values.contains(it.key())) values.insert(it.key(), it.value().toMap().value("default"));
    return values;
}

QVariant Script::setting(const QString &key) {
    auto calls = m_calls;
    const QString path = m_path;
    return onMain<QVariantMap>([&] { return calls.settings(path); }).value(key);
}

void Script::on(const QString &event, const QJSValue &handler) {
    if (handler.isCallable()) m_handlers[event].push_back(handler);
}

void Script::log(const QString &text) { m_host->write(m_path, text); }

int Script::setTimeout(const QJSValue &handler, int ms) { return addTimer(handler, ms, false); }
int Script::setInterval(const QJSValue &handler, int ms) { return addTimer(handler, std::max(10, ms), true); }

int Script::addTimer(const QJSValue &handler, int ms, bool repeat) {
    if (!handler.isCallable()) return 0;
    const int id = m_nextTimer++;
    auto *timer = new QTimer(this);
    timer->setSingleShot(!repeat);
    timer->setInterval(std::max(0, ms));
    m_timers[id] = {timer, handler};
    connect(timer, &QTimer::timeout, this, [this, id, repeat] {
        const auto it = m_timers.find(id);
        if (it == m_timers.end()) return;
        const QJSValue handler = it->second.second;
        if (!repeat) clearTimer(id);
        call(handler, {}, tr("a timer"));
    });
    timer->start();
    return id;
}

void Script::clearTimer(int id) {
    const auto it = m_timers.find(id);
    if (it == m_timers.end()) return;
    it->second.first->deleteLater();
    m_timers.erase(it);
}

void Script::deliver(const QString &event, const QVariantMap &data) {
    const auto it = m_handlers.find(event);
    if (it == m_handlers.end()) return;
    const auto handlers = it->second;
    for (const QJSValue &h : handlers) call(h, {m_engine.toScriptValue(data)}, event);
}

void Script::call(QJSValue handler, const QJSValueList &args, const QString &where) {
    busySince.store(QDateTime::currentMSecsSinceEpoch());
    const QJSValue result = handler.call(args);
    busySince.store(0);
    if (result.isError()) log(tr("Error in %1, line %2: %3").arg(where).arg(result.property(QStringLiteral("lineNumber")).toInt()).arg(result.toString()));
    m_engine.setInterrupted(false);
}

ScriptHost::ScriptHost(ScriptCalls calls, QObject *parent) : QObject(parent), m_calls(std::move(calls)) {
    // A call that never ends would hold its script's thread: stop it.
    m_watchdog = new QTimer(this);
    m_watchdog->setInterval(250);
    connect(m_watchdog, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto &[path, r] : m_running) {
            const qint64 since = r.script->busySince.load();
            if (since > 0 && now - since > m_patienceMs && !r.script->engine().isInterrupted()) {
                r.script->engine().setInterrupted(true);
                write(path, tr("Stopped: it ran for more than %1 seconds without a break.").arg(m_patienceMs / 1000.0));
            }
        }
    });
    m_watchdog->start();
}

ScriptHost::~ScriptHost() {
    const auto paths = m_running;
    for (const auto &[path, r] : paths) stop(path);
}

void ScriptHost::setScripts(const QVariantList &scripts) {
    std::set<QString> wanted;
    for (const QVariant &v : scripts)
        if (v.toMap().value("enabled", true).toBool()) wanted.insert(v.toMap().value("path").toString());
    const auto running = m_running;
    for (const auto &[path, r] : running)
        if (!wanted.count(path)) stop(path);
    for (const QString &path : wanted)
        if (!m_running.count(path)) start(path);
}

void ScriptHost::reload(const QString &path) {
    if (!m_running.count(path)) return;
    stop(path);
    start(path);
}

void ScriptHost::start(const QString &path) {
    auto *thread = new QThread;
    thread->setObjectName(QStringLiteral("script"));
    auto *script = new Script(path, m_calls, this);
    script->moveToThread(thread);
    script->engine().moveToThread(thread);
    connect(this, &ScriptHost::event, script, &Script::deliver, Qt::QueuedConnection);
    connect(thread, &QThread::finished, script, &QObject::deleteLater);
    thread->start();
    m_running[path] = {thread, script};
    write(path, tr("Started."));
    QMetaObject::invokeMethod(script, &Script::load, Qt::QueuedConnection);
}

void ScriptHost::stop(const QString &path) {
    const auto it = m_running.find(path);
    if (it == m_running.end()) return;
    Running r = it->second;
    m_running.erase(it);
    disconnect(this, nullptr, r.script, nullptr);
    r.script->engine().setInterrupted(true);
    r.thread->quit();
    // A script may be waiting for this thread to answer it: keep answering.
    while (!r.thread->wait(10)) QCoreApplication::processEvents();
    delete r.thread;
    write(path, tr("Stopped."));
}

void ScriptHost::send(const QString &name, const QVariantMap &data) { emit event(name, data); }

void ScriptHost::settingsChanged(const QString &path, const QVariantMap &values) {
    const auto it = m_running.find(path);
    if (it == m_running.end()) return;
    QMetaObject::invokeMethod(it->second.script, [script = it->second.script, values] { script->deliver(QStringLiteral("settings"), values); },
                              Qt::QueuedConnection);
}

void ScriptHost::write(const QString &path, const QString &text) {
    const QString line = QStringLiteral("%1  %2: %3").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), QFileInfo(path).fileName(), text);
    QMetaObject::invokeMethod(this, [this, line] {
        m_log.append(line);
        while (m_log.size() > 500) m_log.removeFirst();
        emit logChanged();
    }, Qt::QueuedConnection);
}

void ScriptHost::clearLog() {
    m_log.clear();
    emit logChanged();
}
