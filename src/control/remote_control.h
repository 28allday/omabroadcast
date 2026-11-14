#pragma once

#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>
#include <memory>

#include "control/websocket.h"

// Remote control over WebSocket, in the version 5 remote-control protocol
// (rpc version 1) that stream controllers, Stream Deck plugins and
// companion apps speak: JSON text messages {op, d}. A client is greeted
// (Hello), proves the password if there is one (Identify), then sends
// requests, alone or in batches, and hears the events it subscribed to.
// What the requests do is the handler's business; this class keeps the
// conversation.
class RemoteControl : public QObject {
    Q_OBJECT
public:
    // Event subscription bits, as clients give them.
    enum Intent {
        General = 1 << 0, Config = 1 << 1, Scenes = 1 << 2, Inputs = 1 << 3, Transitions = 1 << 4,
        Filters = 1 << 5, Outputs = 1 << 6, SceneItems = 1 << 7, MediaInputs = 1 << 8, Vendors = 1 << 9,
        Ui = 1 << 10, Canvases = 1 << 11, All = (1 << 12) - 1,
        InputVolumeMeters = 1 << 16, InputActiveStateChanged = 1 << 17, InputShowStateChanged = 1 << 18,
        SceneItemTransformChanged = 1 << 19,
    };
    // Request status codes.
    enum Status {
        Success = 100, MissingRequestType = 203, UnknownRequestType = 204, GenericError = 205,
        UnsupportedBatchExecution = 206, NotReady = 207, MissingRequestField = 300, MissingRequestData = 301,
        InvalidRequestField = 400, InvalidRequestFieldType = 401, RequestFieldOutOfRange = 402,
        RequestFieldEmpty = 403, OutputRunning = 500, OutputNotRunning = 501, OutputPaused = 502,
        OutputNotPaused = 503, OutputDisabled = 504, StudioModeActive = 505, StudioModeNotActive = 506,
        ResourceNotFound = 600, ResourceAlreadyExists = 601, InvalidResourceType = 602,
        NotEnoughResources = 603, InvalidResourceState = 604, InvalidInputKind = 605, ResourceNotConfigurable = 606, ResourceActionFailed = 701,
        RequestProcessingFailed = 702, CannotAct = 703,
    };
    struct Reply {
        int code = Success;
        QString comment;
        QJsonObject data;
    };
    using Done = std::function<void(const Reply &)>;
    // Answers a request through `done`, now or later, once.
    using Handler = std::function<void(const QString &type, const QJsonObject &data, const Done &done)>;

    struct Settings {
        bool enabled = false;
        quint16 port = 4455;
        bool network = true;   // other machines may connect, not only this one
        QString password;      // "" lets anyone connect
        bool operator==(const Settings &) const = default;
    };

    // `requests` lists the request types the handler knows, for GetVersion.
    RemoteControl(Handler handler, QStringList requests, QObject *parent = nullptr);
    // Starts, stops or moves the server; false (and why) when it cannot listen.
    bool apply(const Settings &settings, QString *error = nullptr);
    bool listening() const { return m_server.isListening(); }
    quint16 port() const { return m_server.port(); }
    // Identified clients: [{address, since}].
    QVariantList clients() const;
    // Whether anyone is identified and subscribed to `intent`, to skip work
    // nobody hears.
    bool wants(int intent) const;
    void emitEvent(const QString &type, int intent, const QJsonObject &data = {});
    // Frames for Sleep in a frame-serial batch.
    void setFrameMs(double ms) { m_frameMs = ms; }
    // The app's name and version, for GetVersion.
    void setProduct(const QString &description) { m_product = description; }

    // The authentication string a client sends for this password, salt and
    // challenge (base64 SHA-256 twice).
    static QString authentication(const QString &password, const QString &salt, const QString &challenge);

signals:
    void clientsChanged();

private:
    struct Client {
        QPointer<WebSocketPeer> peer;
        QString challenge;
        bool identified = false;
        int subscriptions = All;
        qint64 since = 0, incoming = 0, outgoing = 0;
    };
    void greet(WebSocketPeer *peer);
    void receive(WebSocketPeer *peer, const QByteArray &text);
    void identify(Client &client, const QJsonObject &data, bool again);
    void request(Client &client, const QJsonObject &data);
    void batch(Client &client, const QJsonObject &data);
    struct Serial {
        QPointer<WebSocketPeer> peer;
        QJsonArray requests, results;
        int execution = 0;
        bool halt = false;
        std::function<void(const QJsonArray &)> answer;
    };
    void step(const std::shared_ptr<Serial> &serial, int at);
    // One request, by the handler or here (GetVersion, Sleep, …).
    void run(WebSocketPeer *peer, const QString &type, const QJsonObject &data, int execution, const Done &done);
    void send(Client &client, int op, const QJsonObject &data);
    bool blocked(const QHostAddress &address);
    void failedLogin(const QHostAddress &address);

    Handler m_handler;
    QStringList m_requests;
    WebSocketServer m_server;
    Settings m_settings;
    QString m_salt;
    QHash<WebSocketPeer *, Client> m_clients;
    QHash<QString, QList<qint64>> m_failures;  // address → failed logins (ms)
    double m_frameMs = 1000.0 / 60;
    QString m_product;
};
