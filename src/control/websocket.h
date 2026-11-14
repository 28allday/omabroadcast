#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTcpServer>
#include <QTimer>

#include <functional>

class QTcpSocket;

// One WebSocket connection (RFC 6455), server side. Messages come whole;
// pings are answered. Deleted by itself after closed().
class WebSocketPeer : public QObject {
    Q_OBJECT
public:
    // The subprotocol agreed in the handshake ("" for none).
    QString subprotocol() const { return m_subprotocol; }
    // The Origin the handshake named: a web page's, "" from other programs.
    QByteArray origin() const { return m_origin; }
    QHostAddress address() const;
    bool isOpen() const { return m_open && !m_closing; }
    void sendText(const QByteArray &utf8);
    // Says goodbye with a close code; the connection ends when the other
    // side answers, or after two seconds.
    void close(int code, const QString &reason = {});

signals:
    // The handshake is done (before any message arrives).
    void opened();
    void textReceived(const QByteArray &utf8);
    void binaryReceived(const QByteArray &data);
    void closed();

private:
    friend class WebSocketServer;
    WebSocketPeer(QTcpSocket *socket, const QStringList &subprotocols, int maxMessage, QObject *parent);
    void read();
    bool handshake();
    bool frames();
    void sendFrame(int opcode, const QByteArray &payload);
    void refuse(const QByteArray &status, const QByteArray &extra = {});
    void fail(int code);
    void finish();

    QTcpSocket *m_socket;
    QStringList m_subprotocols;
    int m_maxMessage;
    QByteArray m_buffer, m_message;
    int m_messageOpcode = 0;
    QString m_subprotocol;
    QByteArray m_origin;
    bool m_open = false, m_closing = false, m_finished = false;
    QTimer m_deadline;
};

// Listens for WebSocket connections. `subprotocols` are the ones it agrees
// to, in the order it prefers them; a client offering none of them is still
// served, without one.
class WebSocketServer : public QObject {
    Q_OBJECT
public:
    explicit WebSocketServer(QStringList subprotocols, QObject *parent = nullptr);
    bool listen(const QHostAddress &address, quint16 port, QString *error = nullptr);
    void close();
    bool isListening() const { return m_server.isListening(); }
    quint16 port() const { return m_server.serverPort(); }
    // Biggest message taken (bytes); bigger ones end the connection.
    void setMaxMessage(int bytes) { m_maxMessage = bytes; }
    // Asked for every new connection before its handshake; false drops it.
    std::function<bool(const QHostAddress &)> accepts;
    // Connections at once, handshakes included; more are dropped.
    static constexpr int kMaxPeers = 32;

signals:
    void connected(WebSocketPeer *peer);

private:
    void take(QTcpServer &server);  // its new connections
    QTcpServer m_server, m_server6;  // m_server6: ::1 beside 127.0.0.1 when local only
    QStringList m_subprotocols;
    int m_maxMessage = 4 << 20;
};
