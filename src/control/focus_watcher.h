#pragma once

#include <QLocalSocket>
#include <QObject>
#include <QString>

// The window that has the keyboard, as Hyprland reports it on its event
// socket (activewindow>>class,title), as it changes. Elsewhere it stays
// unavailable and says nothing.
class FocusWatcher : public QObject {
    Q_OBJECT
public:
    explicit FocusWatcher(QObject *parent = nullptr);
    static bool available();
    void start();
    void stop();
    bool running() const { return m_running; }
    QString windowClass() const { return m_class; }
    QString windowTitle() const { return m_title; }
    // One line from the event socket.
    void handleEvent(const QByteArray &line);

signals:
    void focused(const QString &windowClass, const QString &title);

private:
    void connectSocket();
    QLocalSocket m_events;
    QByteArray m_pending;
    QString m_class, m_title;
    bool m_running = false;
};
