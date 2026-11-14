#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <functional>
#include <memory>

class PrivateSink;
class QSocketNotifier;

// A browser source's page: drawn by the helper program (omabroadcast-browser,
// which alone links Qt WebEngine) off screen, its pictures handed over in
// shared memory (see browser_wire.h). The page plays its sound into a sink
// of its own so the mixer can take it; where that cannot be made the page is
// muted rather than heard on the speakers. A helper that dies is started
// again, so a crashing page never takes the app with it. Main thread only.
class BrowserPage : public QObject {
    Q_OBJECT

public:
    struct Settings {
        QString url;            // what to show: a web address or a file:// URL; empty is blank
        int width = 800, height = 600;
        int fps = 30;
        QString css;            // added to every page it loads
        bool operator==(const Settings &) const = default;
    };
    // The helper, when this machine can run it; empty when it is not
    // installed (Qt WebEngine is an optional dependency). Looked for once:
    // OMABROADCAST_BROWSER_HELPER (empty = none), beside the app, then
    // /usr/lib/omabroadcast. OMABROADCAST_NO_BROWSER=1 hides the feature.
    static QString helper();
    static bool available() { return !helper().isEmpty(); }

    // `storage` keeps the page's sign-ins and site data ("" for none);
    // `soundName` is how its sink is described to people. `channels` of sound.
    BrowserPage(const QString &storage, const QString &soundName, int channels, QObject *parent = nullptr);
    ~BrowserPage() override;

    void setSettings(const Settings &settings);
    const Settings &settings() const { return m_settings; }
    // Loads it again, past the cache.
    void reload();
    // Mouse, wheel, key and focus from the interact window, as the wire has them.
    void input(const QVariantMap &event);

    // "starting", "loading", "live", "failed: …", …
    QString state() const { return m_state; }
    QString title() const { return m_title; }
    // The sink the page plays into, once it exists; empty when muted.
    QString soundSink() const { return m_sinkReady ? m_sinkName : QString(); }
    // Times the helper has been started (the first, and after each death).
    int starts() const { return m_starts; }
    qint64 helperPid() const { return m_process ? m_process->processId() : 0; }

    // Each new picture: premultiplied BGRA, valid only during the call.
    std::function<void(const uchar *pixels, int width, int height, int stride)> onFrame;

signals:
    void stateChanged();
    void soundChanged();

private slots:
    // The sink had its moment: the page starts now, muted if there is none yet.
    void sinkWaitOver();

private:
    void launch();
    void sendSettings();
    // To the helper without waiting on it (this may be the frame loop's
    // moment). Input that does not fit is dropped; anything else that does not
    // means the helper has stopped reading, and it is started again.
    void post(const QJsonObject &message, bool droppable = false);
    void read();
    void dropBuffers();
    void helperGone(QProcess *process);
    // Lets the helper finish on its own (its socket has closed), killed if it lingers.
    // Its sink stays until then, so the page's last sound has nowhere else to go.
    static void letGo(QProcess *process, std::shared_ptr<PrivateSink> sink);
    void setState(const QString &state);

    QString m_storage;
    Settings m_settings;
    QProcess *m_process = nullptr;   // the running helper's
    int m_socket = -1;
    QSocketNotifier *m_notifier = nullptr;
    QTimer m_restart;
    int m_starts = 0, m_failures = 0;
    bool m_waitingForSink = false;
    QTimer m_sinkTimeout;
    std::shared_ptr<PrivateSink> m_sink;
    QString m_sinkName;
    bool m_sinkReady = false, m_sinkFailed = false;
    bool m_helperHasSink = false;    // the running helper was started with PULSE_SINK
    bool m_tooLong = false;          // the page's settings would not go to the helper
    uchar *m_map = nullptr;
    size_t m_mapSize = 0;
    int m_bufferWidth = 0, m_bufferHeight = 0, m_stride = 0, m_count = 0;
    QString m_state, m_title;
    bool m_loading = false;
    QString m_error;
};
