#pragma once

#include <QJsonObject>
#include <QLocalServer>
#include <QObject>

#include <functional>

// The running app's control socket: one JSON object per line in, one per
// line out. A request is {"id": anything, "action": "...", "argument": "..."};
// the reply carries the same id, "ok", and "error" or "result". Only this
// user can connect. `omabroadcast ctl` is its command-line client.
class ControlSocket : public QObject {
    Q_OBJECT
public:
    using Handler = std::function<QJsonObject(const QJsonObject &request)>;
    explicit ControlSocket(Handler handler, QObject *parent = nullptr);
    ~ControlSocket() override;
    // $OMABROADCAST_SOCKET, or omabroadcast.sock in the runtime directory.
    static QString path();
    // False when another copy of the app already answers there.
    bool listen(QString *error = nullptr);
    void close();
    bool listening() const { return m_server.isListening(); }

private:
    void serve(class QLocalSocket *client);
    Handler m_handler;
    QLocalServer m_server;
};

// `omabroadcast ctl …`: asks the running app and prints its answer.
// 0 done, 1 refused, 2 no app answering.
int runControlClient(const QStringList &arguments);
