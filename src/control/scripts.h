#pragma once

#include <QJSEngine>
#include <QJSValue>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include <atomic>
#include <functional>
#include <map>
#include <memory>

class QThread;
class QTimer;

// What a script reaches the app through (main thread).
struct ScriptCalls {
    std::function<QString(const QString &action, const QString &argument)> run;
    std::function<QVariantMap()> state;
    std::function<QVariantList()> actions;
    // A script's saved settings, and its form being declared (main thread).
    std::function<QVariantMap(const QString &path)> settings;
    std::function<void(const QString &path, const QVariantMap &form)> declare;
};

// One script: a JavaScript file in its own engine on its own thread. It sees
// a global `omabroadcast` with run(action, argument), state(), actions(),
// on(event, fn), log(text), setTimeout/setInterval(fn, ms) and
// clearTimer(id); console.log and print go to the log. No files, no network.
// settings({key: {label, type: text|number|bool|studio, default}}) declares a
// form shown under Tools → Scripts and returns the values; setting(key) reads
// one; a change arrives as the "settings" event.
class Script : public QObject {
    Q_OBJECT
public:
    Script(const QString &path, const ScriptCalls &calls, class ScriptHost *host);
    QJSEngine &engine() { return m_engine; }
    // Since when a call into the script has been running (0: none).
    std::atomic<qint64> busySince{0};

    Q_INVOKABLE QString run(const QString &action, const QString &argument = {});
    Q_INVOKABLE QVariantMap state();
    Q_INVOKABLE QVariantList actions();
    Q_INVOKABLE void on(const QString &event, const QJSValue &handler);
    Q_INVOKABLE void log(const QString &text);
    Q_INVOKABLE int setTimeout(const QJSValue &handler, int ms);
    Q_INVOKABLE int setInterval(const QJSValue &handler, int ms);
    Q_INVOKABLE void clearTimer(int id);
    Q_INVOKABLE QVariantMap settings(const QVariantMap &form);
    Q_INVOKABLE QVariant setting(const QString &key);

public slots:
    void load();
    void deliver(const QString &event, const QVariantMap &data);

private:
    int addTimer(const QJSValue &handler, int ms, bool repeat);
    void call(QJSValue handler, const QJSValueList &args, const QString &where);
    QString m_path;
    ScriptCalls m_calls;
    ScriptHost *m_host;
    QJSEngine m_engine;
    std::map<QString, std::vector<QJSValue>> m_handlers;
    std::map<int, std::pair<QTimer *, QJSValue>> m_timers;
    int m_nextTimer = 1;
};

// The scripts people added, each running on its own thread, with one log.
class ScriptHost : public QObject {
    Q_OBJECT
public:
    explicit ScriptHost(ScriptCalls calls, QObject *parent = nullptr);
    ~ScriptHost() override;
    // [{path, enabled}]; scripts start, stop and restart to match.
    void setScripts(const QVariantList &scripts);
    void reload(const QString &path);
    // Every running script's handlers for `event` get `data`.
    void send(const QString &event, const QVariantMap &data);
    QStringList log() const { return m_log; }
    void clearLog();
    // Calls into a script that run this long are stopped.
    void setPatience(int ms) { m_patienceMs = ms; }
    void write(const QString &path, const QString &text);  // any thread
    bool running(const QString &path) const { return m_running.count(path) > 0; }

signals:
    void logChanged();
    void event(const QString &name, const QVariantMap &data);

private:
    struct Running {
        QThread *thread = nullptr;
        Script *script = nullptr;
    };
    void start(const QString &path);
    void stop(const QString &path);
    ScriptCalls m_calls;
    std::map<QString, Running> m_running;
public:
    // A script's settings changed (the form): it hears "settings".
    void settingsChanged(const QString &path, const QVariantMap &values);
private:
    QStringList m_log;
    QTimer *m_watchdog = nullptr;
    int m_patienceMs = 5000;
};
