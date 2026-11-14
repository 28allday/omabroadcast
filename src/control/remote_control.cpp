#include "control/remote_control.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSysInfo>
#include <QTimer>

#include <memory>

// Names on the wire are the protocol's own, kept as clients expect them.

namespace {

enum Op { Hello = 0, Identify = 1, Identified = 2, Reidentify = 3, Event = 5, Request = 6, RequestResponse = 7,
          RequestBatch = 8, RequestBatchResponse = 9 };
constexpr int kIdentifyMs = 10000;
constexpr qsizetype kMaxUnidentified = 16 * 1024;
enum CloseCode { MessageDecodeError = 4002, MissingDataField = 4003, InvalidDataFieldType = 4004, UnknownOpCode = 4006,
                 NotIdentified = 4007, AlreadyIdentified = 4008, AuthenticationFailed = 4009, UnsupportedRpcVersion = 4010 };
enum Execution { SerialRealtime = 0, SerialFrame = 1, Parallel = 2, NoBatch = -1 };

constexpr int kRpcVersion = 1;
// The protocol level this speaks, and the host version clients compare
// against when they decide which requests to try.
const char *const kProtocolVersion = "5.5.0";
const char *const kHostVersion = "30.2.0";
const char *const kSubprotocol = "obswebsocket.json";
constexpr int kFailureWindowMs = 60000, kFailuresAllowed = 5;

QString randomBase64() {
    QByteArray bytes(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(bytes.data()), bytes.size() / 4);
    return QString::fromLatin1(bytes.toBase64());
}

bool sameSecret(const QByteArray &a, const QByteArray &b) {
    if (a.size() != b.size()) return false;
    char diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i) diff |= char(a[i] ^ b[i]);
    return diff == 0;
}

QJsonObject status(const RemoteControl::Reply &r) {
    QJsonObject s{{"result", r.code == RemoteControl::Success}, {"code", r.code}};
    if (!r.comment.isEmpty()) s.insert("comment", r.comment);
    return s;
}

QJsonObject response(const QString &type, const QJsonValue &id, const RemoteControl::Reply &r) {
    QJsonObject d{{"requestType", type}, {"requestStatus", status(r)}};
    if (!id.isUndefined()) d.insert("requestId", id);
    if (!r.data.isEmpty()) d.insert("responseData", r.data);
    return d;
}

}  // namespace

RemoteControl::RemoteControl(Handler handler, QStringList requests, QObject *parent)
    : QObject(parent), m_handler(std::move(handler)), m_requests(std::move(requests)), m_server({QString::fromLatin1(kSubprotocol)}) {
    m_requests << QStringLiteral("GetVersion") << QStringLiteral("BroadcastCustomEvent") << QStringLiteral("Sleep");
    m_requests.removeDuplicates();
    m_server.accepts = [this](const QHostAddress &address) { return !blocked(address); };
    connect(&m_server, &WebSocketServer::connected, this, &RemoteControl::greet);
}

QString RemoteControl::authentication(const QString &password, const QString &salt, const QString &challenge) {
    const QByteArray secret = QCryptographicHash::hash((password + salt).toUtf8(), QCryptographicHash::Sha256).toBase64();
    return QString::fromLatin1(QCryptographicHash::hash(secret + challenge.toUtf8(), QCryptographicHash::Sha256).toBase64());
}

bool RemoteControl::apply(const Settings &settings, QString *error) {
    const bool restart = settings.enabled != m_settings.enabled || settings.port != m_settings.port ||
                         settings.network != m_settings.network || settings.password != m_settings.password ||
                         settings.enabled != m_server.isListening();
    m_settings = settings;
    if (!restart) return true;
    // Everyone signs in again with the new rules.
    m_server.close();
    m_clients.clear();
    m_salt = randomBase64();
    emit clientsChanged();
    if (!settings.enabled) return true;
    return m_server.listen(settings.network ? QHostAddress::Any : QHostAddress::LocalHost, settings.port, error);
}

QVariantList RemoteControl::clients() const {
    QVariantList out;
    for (const Client &c : m_clients)
        if (c.identified && c.peer)
            out.append(QVariantMap{{"address", c.peer->address().toString()}, {"since", c.since}});
    return out;
}

bool RemoteControl::wants(int intent) const {
    for (const Client &c : m_clients)
        if (c.identified && (c.subscriptions & intent)) return true;
    return false;
}

void RemoteControl::emitEvent(const QString &type, int intent, const QJsonObject &data) {
    QJsonObject d{{"eventType", type}, {"eventIntent", intent}};
    if (!data.isEmpty()) d.insert("eventData", data);
    for (Client &c : m_clients)
        if (c.identified && (c.subscriptions & intent)) send(c, Event, d);
}

void RemoteControl::send(Client &client, int op, const QJsonObject &data) {
    if (!client.peer) return;
    ++client.outgoing;
    client.peer->sendText(QJsonDocument(QJsonObject{{"op", op}, {"d", data}}).toJson(QJsonDocument::Compact));
}

void RemoteControl::greet(WebSocketPeer *peer) {
    // A web page can reach a local port too: without a password, only
    // programs (which send no Origin) may drive the app.
    if (m_settings.password.isEmpty() && !peer->origin().isEmpty()) {
        peer->close(AuthenticationFailed, QStringLiteral("Set a password to connect from a web page."));
        return;
    }
    Client &client = m_clients[peer];
    client.peer = peer;
    // Signed in soon, or let go: an idle connection holds a place.
    QTimer::singleShot(kIdentifyMs, peer, [this, peer] {
        if (!m_clients.value(peer).identified) peer->close(NotIdentified, QStringLiteral("Identify sooner."));
    });
    connect(peer, &WebSocketPeer::textReceived, this, [this, peer](const QByteArray &text) { receive(peer, text); });
    connect(peer, &WebSocketPeer::binaryReceived, this, [peer] { peer->close(MessageDecodeError, QStringLiteral("Binary messages are not used.")); });
    connect(peer, &WebSocketPeer::closed, this, [this, peer] {
        const bool was = m_clients.value(peer).identified;
        m_clients.remove(peer);
        if (was) emit clientsChanged();
    });
    QJsonObject hello{{"obsStudioVersion", QString::fromLatin1(kHostVersion)},
                      {"obsWebSocketVersion", QString::fromLatin1(kProtocolVersion)}, {"rpcVersion", kRpcVersion}};
    if (!m_settings.password.isEmpty()) {
        client.challenge = randomBase64();
        hello.insert("authentication", QJsonObject{{"challenge", client.challenge}, {"salt", m_salt}});
    }
    send(client, Hello, hello);
}

void RemoteControl::receive(WebSocketPeer *peer, const QByteArray &text) {
    auto it = m_clients.find(peer);
    if (it == m_clients.end()) return;
    Client &client = *it;
    ++client.incoming;
    // Before signing in, nothing bigger than an Identify.
    if (!client.identified && text.size() > kMaxUnidentified) { peer->close(MessageDecodeError, QStringLiteral("Too big before Identify.")); return; }
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(text, &parse);
    if (!doc.isObject()) { peer->close(MessageDecodeError, parse.errorString()); return; }
    const QJsonObject message = doc.object();
    // An older version of the protocol.
    if (!client.identified && message.contains("request-type")) {
        peer->close(UnsupportedRpcVersion, QStringLiteral("This speaks rpc version 1 (protocol 5)."));
        return;
    }
    if (!message.contains("op") || !message.contains("d")) { peer->close(MissingDataField, QStringLiteral("A message needs op and d.")); return; }
    if (!message.value("op").isDouble() || !message.value("d").isObject()) { peer->close(InvalidDataFieldType, QStringLiteral("op is a number and d an object.")); return; }
    const int op = message.value("op").toInt(-1);
    const QJsonObject data = message.value("d").toObject();
    if (op != Identify && op != Reidentify && op != Request && op != RequestBatch) {
        peer->close(UnknownOpCode, QStringLiteral("Unknown op %1.").arg(op));
        return;
    }
    if (!client.identified && op != Identify) { peer->close(NotIdentified, QStringLiteral("Identify first.")); return; }
    if (op == Identify) {
        if (client.identified) { peer->close(AlreadyIdentified, QStringLiteral("Already identified.")); return; }
        identify(client, data, false);
    } else if (op == Reidentify) {
        identify(client, data, true);
    } else if (op == Request) {
        request(client, data);
    } else {
        batch(client, data);
    }
}

void RemoteControl::identify(Client &client, const QJsonObject &data, bool again) {
    WebSocketPeer *peer = client.peer;
    if (!again) {
        if (!data.value("rpcVersion").isDouble()) { peer->close(MissingDataField, QStringLiteral("rpcVersion is needed.")); return; }
        if (data.value("rpcVersion").toInt() != kRpcVersion) {
            peer->close(UnsupportedRpcVersion, QStringLiteral("Only rpc version %1 is spoken.").arg(kRpcVersion));
            return;
        }
        if (!m_settings.password.isEmpty()) {
            const QString given = data.value("authentication").toString();
            const QString wanted = authentication(m_settings.password, m_salt, client.challenge);
            if (!sameSecret(given.toUtf8(), wanted.toUtf8())) {
                failedLogin(peer->address());
                peer->close(AuthenticationFailed, QStringLiteral("Wrong password."));
                return;
            }
        }
    }
    if (data.contains("eventSubscriptions")) {
        if (!data.value("eventSubscriptions").isDouble()) { peer->close(InvalidDataFieldType, QStringLiteral("eventSubscriptions is a number.")); return; }
        client.subscriptions = int(data.value("eventSubscriptions").toInteger());
    }
    if (!again) {
        client.identified = true;
        client.since = QDateTime::currentMSecsSinceEpoch();
        emit clientsChanged();
    }
    send(client, Identified, {{"negotiatedRpcVersion", kRpcVersion}});
}

void RemoteControl::request(Client &client, const QJsonObject &data) {
    const QString type = data.value("requestType").toString();
    const QJsonValue id = data.value("requestId");
    QPointer<WebSocketPeer> peer = client.peer;
    if (type.isEmpty()) {
        send(client, RequestResponse, response(type, id, {MissingRequestType, QStringLiteral("requestType is needed."), {}}));
        return;
    }
    run(peer, type, data.value("requestData").toObject(), NoBatch, [this, peer, type, id](const Reply &r) {
        if (!peer || !m_clients.contains(peer)) return;
        send(m_clients[peer], RequestResponse, response(type, id, r));
    });
}

void RemoteControl::batch(Client &client, const QJsonObject &data) {
    QPointer<WebSocketPeer> peer = client.peer;
    const QJsonValue id = data.value("requestId");
    const int execution = data.value("executionType").toInt(SerialRealtime);
    const bool halt = data.value("haltOnFailure").toBool(false);
    const QJsonArray requests = data.value("requests").toArray();
    auto answer = [this, peer, id](const QJsonArray &results) {
        if (!peer || !m_clients.contains(peer)) return;
        QJsonObject d{{"results", results}};
        if (!id.isUndefined()) d.insert("requestId", id);
        send(m_clients[peer], RequestBatchResponse, d);
    };
    if (execution != SerialRealtime && execution != SerialFrame && execution != Parallel) {
        // The batch is refused as a whole, as one result.
        answer({response(QStringLiteral("RequestBatch"), id, {UnsupportedBatchExecution, QStringLiteral("Unknown executionType %1.").arg(execution), {}})});
        return;
    }
    if (execution == Parallel) {
        auto results = std::make_shared<QJsonArray>();
        auto left = std::make_shared<int>(int(requests.size()));
        for (int i = 0; i < requests.size(); ++i) results->append(QJsonValue());
        if (requests.isEmpty()) { answer({}); return; }
        for (int i = 0; i < requests.size(); ++i) {
            const QJsonObject r = requests[i].toObject();
            const QString type = r.value("requestType").toString();
            run(peer, type, r.value("requestData").toObject(), Parallel, [=](const Reply &reply) {
                (*results)[i] = response(type, r.value("requestId"), reply);
                if (--*left == 0) answer(*results);
            });
        }
        return;
    }
    // One after another: each starts when the last is answered.
    auto serial = std::make_shared<Serial>(Serial{peer, requests, {}, execution, halt, answer});
    step(serial, 0);
}

void RemoteControl::step(const std::shared_ptr<Serial> &serial, int at) {
    // Requests answered at once go round this loop rather than calling back
    // in: a batch of thousands would otherwise run out of stack.
    for (;;) {
        if (at >= serial->requests.size() || !serial->peer) { serial->answer(serial->results); return; }
        const QJsonObject r = serial->requests[at].toObject();
        const QString type = r.value("requestType").toString();
        auto inside = std::make_shared<bool>(true);
        auto next = std::make_shared<int>(-1);
        run(serial->peer, type, r.value("requestData").toObject(), serial->execution,
            [this, serial, at, type, r, inside, next](const Reply &reply) {
                serial->results.append(response(type, r.value("requestId"), reply));
                if (serial->halt && reply.code != Success) serial->answer(serial->results);
                else if (*inside) *next = at + 1;
                else step(serial, at + 1);
            });
        *inside = false;
        if (*next < 0) return;  // answered later, or halted
        at = *next;
    }
}

void RemoteControl::run(WebSocketPeer *peer, const QString &type, const QJsonObject &data, int execution, const Done &done) {
    if (type.isEmpty()) { done({MissingRequestType, QStringLiteral("requestType is needed."), {}}); return; }
    if (type == QLatin1String("GetVersion")) {
        QJsonArray formats;
        for (const QByteArray &f : QImageWriter::supportedImageFormats()) formats.append(QString::fromLatin1(f));
        done({Success, {}, {{"obsVersion", QString::fromLatin1(kHostVersion)}, {"obsWebSocketVersion", QString::fromLatin1(kProtocolVersion)},
                            {"rpcVersion", kRpcVersion}, {"availableRequests", QJsonArray::fromStringList(m_requests)},
                            {"supportedImageFormats", formats}, {"platform", QSysInfo::kernelType()},
                            {"platformDescription", QStringLiteral("%1 on %2").arg(m_product, QSysInfo::prettyProductName())}}});
        return;
    }
    if (type == QLatin1String("BroadcastCustomEvent")) {
        if (!data.value("eventData").isObject()) { done({MissingRequestField, QStringLiteral("eventData is needed."), {}}); return; }
        emitEvent(QStringLiteral("CustomEvent"), General, data.value("eventData").toObject());
        done({});
        return;
    }
    if (type == QLatin1String("Sleep")) {
        if (execution != SerialRealtime && execution != SerialFrame) {
            done({UnsupportedBatchExecution, QStringLiteral("Sleep works only in a serial batch."), {}});
            return;
        }
        const bool frames = execution == SerialFrame;
        const QString field = frames ? QStringLiteral("sleepFrames") : QStringLiteral("sleepMillis");
        if (!data.value(field).isDouble()) { done({MissingRequestField, field + QStringLiteral(" is needed."), {}}); return; }
        const double amount = data.value(field).toDouble();
        if (amount < 0 || amount > (frames ? 10000 : 50000)) { done({RequestFieldOutOfRange, field + QStringLiteral(" is out of range."), {}}); return; }
        QTimer::singleShot(int(frames ? amount * m_frameMs : amount), this, [done] { done({}); });
        return;
    }
    if (type == QLatin1String("GetStats")) {
        m_handler(type, data, [this, peer, done](const Reply &r) {
            Reply out = r;
            const Client c = m_clients.value(peer);
            out.data.insert("webSocketSessionIncomingMessages", double(c.incoming));
            out.data.insert("webSocketSessionOutgoingMessages", double(c.outgoing));
            done(out);
        });
        return;
    }
    if (!m_requests.contains(type)) { done({UnknownRequestType, QStringLiteral("Unknown request type: %1").arg(type), {}}); return; }
    m_handler(type, data, done);
}

bool RemoteControl::blocked(const QHostAddress &address) {
    QList<qint64> &tries = m_failures[address.toString()];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    tries.removeIf([now](qint64 t) { return now - t > kFailureWindowMs; });
    if (tries.isEmpty()) m_failures.remove(address.toString());
    return m_failures.value(address.toString()).size() >= kFailuresAllowed;
}

void RemoteControl::failedLogin(const QHostAddress &address) {
    m_failures[address.toString()].append(QDateTime::currentMSecsSinceEpoch());
}
