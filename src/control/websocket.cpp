#include "control/websocket.h"

#include <QCryptographicHash>
#include <QStringDecoder>
#include <QTcpSocket>
#include <QtEndian>

namespace {

enum Opcode { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };
constexpr int kMaxHandshake = 16 * 1024;
constexpr int kHandshakeMs = 10000, kClosingMs = 2000;
// Unsent bytes a reader may leave waiting before it is dropped: one that
// never reads would otherwise grow this process without end.
constexpr qint64 kMaxUnsent = 64ll << 20;

// The value of one header ("" when missing), names compared without case.
QByteArray header(const QList<QByteArray> &lines, const QByteArray &name) {
    for (const QByteArray &line : lines) {
        const int colon = line.indexOf(':');
        if (colon > 0 && line.left(colon).trimmed().compare(name, Qt::CaseInsensitive) == 0) return line.mid(colon + 1).trimmed();
    }
    return {};
}

bool listHas(const QByteArray &value, const QByteArray &token) {
    for (const QByteArray &part : value.split(','))
        if (part.trimmed().compare(token, Qt::CaseInsensitive) == 0) return true;
    return false;
}

}  // namespace

WebSocketPeer::WebSocketPeer(QTcpSocket *socket, const QStringList &subprotocols, int maxMessage, QObject *parent)
    : QObject(parent), m_socket(socket), m_subprotocols(subprotocols), m_maxMessage(maxMessage) {
    m_socket->setParent(this);
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] { m_socket->abort(); });
    connect(m_socket, &QTcpSocket::readyRead, this, &WebSocketPeer::read);
    connect(m_socket, &QTcpSocket::disconnected, this, &WebSocketPeer::finish);
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this] {
        if (m_socket->state() != QAbstractSocket::ConnectedState) finish();
    });
    m_deadline.start(kHandshakeMs);
}

QHostAddress WebSocketPeer::address() const { return m_socket->peerAddress(); }

void WebSocketPeer::read() {
    m_buffer += m_socket->readAll();
    if (!m_open && !handshake()) return;
    if (m_open) frames();
}

bool WebSocketPeer::handshake() {
    const int end = m_buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (m_buffer.size() > kMaxHandshake) refuse("431 Request Header Fields Too Large");
        return false;
    }
    const QList<QByteArray> lines = m_buffer.left(end).split('\n');
    m_buffer.remove(0, end + 4);
    const QList<QByteArray> request = lines.value(0).trimmed().split(' ');
    if (request.size() < 3 || request[0] != "GET") { refuse("400 Bad Request"); return false; }
    const QByteArray key = header(lines, "Sec-WebSocket-Key");
    if (!listHas(header(lines, "Upgrade"), "websocket") || !listHas(header(lines, "Connection"), "upgrade") ||
        QByteArray::fromBase64(key).size() != 16) {
        refuse("400 Bad Request");
        return false;
    }
    if (header(lines, "Sec-WebSocket-Version") != "13") {
        refuse("426 Upgrade Required", "Sec-WebSocket-Version: 13\r\n");
        return false;
    }
    // The client's order of preference among the ones served.
    for (const QByteArray &offered : header(lines, "Sec-WebSocket-Protocol").split(',')) {
        const QString name = QString::fromLatin1(offered.trimmed());
        if (m_subprotocols.contains(name)) { m_subprotocol = name; break; }
    }
    m_origin = header(lines, "Origin");
    const QByteArray accept = QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();
    QByteArray reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n";
    if (!m_subprotocol.isEmpty()) reply += "Sec-WebSocket-Protocol: " + m_subprotocol.toLatin1() + "\r\n";
    m_socket->write(reply + "\r\n");
    m_open = true;
    m_deadline.stop();
    emit opened();
    return true;
}

bool WebSocketPeer::frames() {
    const QPointer<WebSocketPeer> self(this);
    while (m_open && !m_finished) {
        if (m_buffer.size() < 2) return true;
        const auto *b = reinterpret_cast<const uchar *>(m_buffer.constData());
        const bool fin = b[0] & 0x80, masked = b[1] & 0x80;
        const int opcode = b[0] & 0x0f;
        if ((b[0] & 0x70) || !masked) { fail(1002); return false; }  // no extensions; clients mask
        qint64 length = b[1] & 0x7f;
        int at = 2;
        if (length == 126) {
            if (m_buffer.size() < 4) return true;
            length = qFromBigEndian<quint16>(b + 2);
            at = 4;
        } else if (length == 127) {
            if (m_buffer.size() < 10) return true;
            const quint64 big = qFromBigEndian<quint64>(b + 2);
            if (big >> 63) { fail(1002); return false; }
            length = qint64(big);
            at = 10;
        }
        const bool control = opcode >= Close;
        if (control && (!fin || length > 125)) { fail(1002); return false; }
        // Before any sum: the length is the sender's, up to 2^63.
        if (!control && (length > m_maxMessage || m_message.size() + length > m_maxMessage)) { fail(1009); return false; }
        if (m_buffer.size() < at + 4 + length) return true;
        const uchar *mask = b + at;
        QByteArray payload = m_buffer.mid(at + 4, length);
        for (qsizetype i = 0; i < payload.size(); ++i) payload[i] = char(payload[i] ^ mask[i % 4]);
        m_buffer.remove(0, at + 4 + length);

        if (opcode == Ping) { sendFrame(Pong, payload); continue; }
        if (opcode == Pong) continue;
        if (opcode == Close) {
            if (!m_closing) {
                m_closing = true;
                sendFrame(Close, payload.left(2));
            }
            m_deadline.start(kClosingMs);  // a sender that stops reading never lets it finish
            m_socket->disconnectFromHost();
            return false;
        }
        if (opcode == Continuation) {
            if (!m_messageOpcode) { fail(1002); return false; }
        } else if (opcode == Text || opcode == Binary) {
            if (m_messageOpcode) { fail(1002); return false; }
            m_messageOpcode = opcode;
        } else {
            fail(1002);
            return false;
        }
        m_message += payload;
        if (!fin) continue;
        const QByteArray message = std::exchange(m_message, {});
        const int kind = std::exchange(m_messageOpcode, 0);
        if (m_closing) continue;
        if (kind == Text) {
            QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
            (void)utf8.decode(message);
            if (utf8.hasError()) { fail(1007); return false; }
            emit textReceived(message);
        } else {
            emit binaryReceived(message);
        }
        // What a message set off may run an event loop, and that can end this peer.
        if (!self) return false;
    }
    return false;
}

void WebSocketPeer::sendText(const QByteArray &utf8) {
    if (isOpen()) sendFrame(Text, utf8);
}

void WebSocketPeer::sendFrame(int opcode, const QByteArray &payload) {
    QByteArray frame;
    frame.append(char(0x80 | opcode));
    if (payload.size() < 126) {
        frame.append(char(payload.size()));
    } else if (payload.size() <= 0xffff) {
        frame.append(char(126));
        uchar n[2];
        qToBigEndian<quint16>(quint16(payload.size()), n);
        frame.append(reinterpret_cast<const char *>(n), 2);
    } else {
        frame.append(char(127));
        uchar n[8];
        qToBigEndian<quint64>(quint64(payload.size()), n);
        frame.append(reinterpret_cast<const char *>(n), 8);
    }
    if (m_socket->bytesToWrite() + frame.size() + payload.size() > kMaxUnsent) { m_socket->abort(); return; }
    m_socket->write(frame + payload);
}

void WebSocketPeer::close(int code, const QString &reason) {
    if (m_finished) return;
    if (!m_open) { m_socket->abort(); return; }
    if (m_closing) return;
    m_closing = true;
    uchar n[2];
    qToBigEndian<quint16>(quint16(code), n);
    sendFrame(Close, QByteArray(reinterpret_cast<const char *>(n), 2) + reason.toUtf8().left(123));
    m_deadline.start(kClosingMs);
}

void WebSocketPeer::refuse(const QByteArray &status, const QByteArray &extra) {
    m_socket->write("HTTP/1.1 " + status + "\r\n" + extra + "Content-Length: 0\r\nConnection: close\r\n\r\n");
    m_socket->disconnectFromHost();
}

void WebSocketPeer::fail(int code) {
    close(code);
    m_buffer.clear();
}

void WebSocketPeer::finish() {
    if (m_finished) return;
    m_finished = true;
    m_deadline.stop();
    const bool wasOpen = m_open;
    m_open = false;
    if (wasOpen) emit closed();
    deleteLater();
}

WebSocketServer::WebSocketServer(QStringList subprotocols, QObject *parent) : QObject(parent), m_subprotocols(std::move(subprotocols)) {
    connect(&m_server, &QTcpServer::pendingConnectionAvailable, this, [this] { take(m_server); });
    connect(&m_server6, &QTcpServer::pendingConnectionAvailable, this, [this] { take(m_server6); });
}

void WebSocketServer::take(QTcpServer &server) {
    while (QTcpSocket *socket = server.nextPendingConnection()) {
        const int peers = int(findChildren<WebSocketPeer *>(Qt::FindDirectChildrenOnly).size());
        if (peers >= kMaxPeers || (accepts && !accepts(socket->peerAddress()))) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        auto *peer = new WebSocketPeer(socket, m_subprotocols, m_maxMessage, this);
        connect(peer, &WebSocketPeer::opened, this, [this, peer] { emit connected(peer); });
    }
}

bool WebSocketServer::listen(const QHostAddress &address, quint16 port, QString *error) {
    close();
    if (m_server.listen(address, port)) {
        // This machine only: its IPv6 name too ("localhost" can be ::1), where
        // the system has IPv6; Any already takes both.
        if (address == QHostAddress(QHostAddress::LocalHost)) m_server6.listen(QHostAddress::LocalHostIPv6, m_server.serverPort());
        return true;
    }
    if (error) *error = m_server.errorString();
    return false;
}

void WebSocketServer::close() {
    if (m_server.isListening()) m_server.close();
    if (m_server6.isListening()) m_server6.close();
    for (WebSocketPeer *peer : findChildren<WebSocketPeer *>(Qt::FindDirectChildrenOnly)) peer->close(1001);
}
