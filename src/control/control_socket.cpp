#include "control/control_socket.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTextStream>

#include <unistd.h>

ControlSocket::ControlSocket(Handler handler, QObject *parent) : QObject(parent), m_handler(std::move(handler)) {
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *client = m_server.nextPendingConnection()) serve(client);
    });
}

ControlSocket::~ControlSocket() { close(); }

QString ControlSocket::path() {
    const QString set = qEnvironmentVariable("OMABROADCAST_SOCKET");
    if (!set.isEmpty()) return set;
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (!runtime.isEmpty()) return QDir(runtime).filePath(QStringLiteral("omabroadcast.sock"));
    // No runtime folder: the shared one, named for this account alone.
    return QDir(QDir::tempPath()).filePath(QStringLiteral("omabroadcast-%1.sock").arg(::getuid()));
}

bool ControlSocket::listen(QString *error) {
    if (m_server.isListening()) return true;
    const QString where = path();
    // A socket nobody answers on is left from a crash: clear it. One that
    // answers belongs to another copy of the app.
    QLocalSocket probe;
    probe.connectToServer(where);
    if (probe.waitForConnected(300)) {
        if (error) *error = QCoreApplication::translate("ControlSocket", "Another copy of OmaBroadcast answers at %1.").arg(where);
        return false;
    }
    QLocalServer::removeServer(where);
    if (!m_server.listen(where)) {
        if (error) *error = m_server.errorString();
        return false;
    }
    return true;
}

void ControlSocket::close() {
    if (m_server.isListening()) m_server.close();
}

void ControlSocket::serve(QLocalSocket *client) {
    connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
    connect(client, &QLocalSocket::readyRead, this, [this, client] {
        while (client->canReadLine()) {
            const QByteArray line = client->readLine().trimmed();
            if (line.isEmpty()) continue;
            QJsonParseError parse;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &parse);
            QJsonObject reply;
            if (!doc.isObject()) {
                reply = {{"ok", false}, {"error", QStringLiteral("Not a JSON object: %1").arg(parse.errorString())}};
            } else {
                reply = m_handler(doc.object());
                if (doc.object().contains("id")) reply.insert("id", doc.object().value("id"));
            }
            client->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
        }
    });
}

int runControlClient(const QStringList &arguments) {
    QTextStream out(stdout), err(stderr);
    if (arguments.isEmpty() || arguments.first() == QLatin1String("--help") || arguments.first() == QLatin1String("help")) {
        out << "Usage: omabroadcast ctl ACTION [ARGUMENT]\n"
               "       omabroadcast ctl list      every action, with the arguments it takes now\n"
               "       omabroadcast ctl status    what is running\n"
               "Examples: omabroadcast ctl record.toggle\n"
               "          omabroadcast ctl studio.show \"Studio 2\"\n";
        return arguments.isEmpty() ? 1 : 0;
    }
    QLocalSocket socket;
    socket.connectToServer(ControlSocket::path());
    if (!socket.waitForConnected(2000)) {
        err << "OmaBroadcast is not running (nothing answers at " << ControlSocket::path() << ").\n";
        return 2;
    }
    QJsonObject request{{"id", 1}, {"action", arguments.first()}};
    if (arguments.size() > 1) request.insert("argument", arguments.mid(1).join(QLatin1Char(' ')));
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    if (!socket.waitForBytesWritten(2000)) return 2;
    while (!socket.canReadLine())
        if (!socket.waitForReadyRead(10000)) { err << "OmaBroadcast did not answer.\n"; return 2; }
    const QJsonObject reply = QJsonDocument::fromJson(socket.readLine()).object();
    if (!reply.value("ok").toBool()) {
        err << reply.value("error").toString() << '\n';
        return reply.value("retry").toBool() ? 3 : 1;
    }
    const QJsonValue result = reply.value("result");
    if (arguments.first() == QLatin1String("list")) {
        for (const QJsonValue &v : result.toArray()) {
            const QJsonObject a = v.toObject();
            out << a.value("id").toString();
            if (!a.value("argument").toString().isEmpty()) out << ' ' << a.value("argument").toString();
            out << "\t" << a.value("name").toString() << '\n';
        }
    } else if (!result.isUndefined() && !result.isNull()) {
        out << (result.isObject() ? QJsonDocument(result.toObject()).toJson(QJsonDocument::Indented)
                                  : QJsonDocument(QJsonArray{result}).toJson(QJsonDocument::Compact));
        if (!result.isObject()) out << '\n';
    }
    return 0;
}
