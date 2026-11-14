#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>

class QProcess;
class QTimer;

// OmaBroadcast's icon in the Omarchy bar. Omarchy keeps a plugin in the person's
// own config folder, where a package cannot put it, so the app carries it and
// puts it there. Whether it is in the bar is read from the bar's own settings:
// the person can move or remove it from there as with any other.
class OmarchyBar : public QObject {
    Q_OBJECT
public:
    static constexpr const char *kPluginId = "nosignal.omabroadcast";

    explicit OmarchyBar(QObject *parent = nullptr);
    OmarchyBar(const QString &configDir, const QString &command, QObject *parent = nullptr);

    bool available() const { return m_available; }
    bool active() const { return m_active; }
    QString pluginDir() const;

    // The plugin's files on disk, replaced only when they differ. A folder that
    // is a link is someone working on the plugin and is left alone.
    bool install(QString *why = nullptr);
    void show();
    void hide();

signals:
    void activeChanged();
    void failed(const QString &why);

private:
    void readActive();
    void watch();
    void run(const QString &verb, const QStringList &extra);
    void finished(int exitCode);

    QString m_dir, m_command;
    bool m_available = false, m_active = false, m_wanted = false;
    int m_tries = 0;
    QString m_verb;
    QStringList m_extra;
    QProcess *m_process = nullptr;
    QTimer *m_retry = nullptr;
    QFileSystemWatcher m_watcher;
};
