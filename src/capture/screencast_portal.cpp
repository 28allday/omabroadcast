#include "screencast_portal.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDBusUnixFileDescriptor>
#include <QGuiApplication>

#include <unistd.h>

namespace {
const QString kService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kIface = QStringLiteral("org.freedesktop.portal.ScreenCast");
const QString kRequestIface = QStringLiteral("org.freedesktop.portal.Request");
const QString kSessionIface = QStringLiteral("org.freedesktop.portal.Session");

// Request and session tokens must be unique across every portal object in
// the process: the portal builds object paths from them, and a reused path
// fails with "File exists".
uint nextSerial() {
    static uint serial = 0;
    return ++serial;
}

// a(ua{sv}): one entry per stream.
using Stream = QPair<uint, QVariantMap>;
using Streams = QList<Stream>;

QVariantMap unpackMap(const QVariant &v) {
    if (v.canConvert<QDBusArgument>()) {
        QVariantMap out;
        v.value<QDBusArgument>() >> out;
        return out;
    }
    return v.toMap();
}
}  // namespace

Q_DECLARE_METATYPE(Stream)
Q_DECLARE_METATYPE(Streams)

QDBusArgument &operator<<(QDBusArgument &arg, const Stream &s) {
    arg.beginStructure();
    arg << s.first << s.second;
    arg.endStructure();
    return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, Stream &s) {
    arg.beginStructure();
    arg >> s.first >> s.second;
    arg.endStructure();
    return arg;
}

ScreenCastPortal::ScreenCastPortal(QObject *parent) : QObject(parent) {
    qDBusRegisterMetaType<Stream>();
    qDBusRegisterMetaType<Streams>();
    // A person can leave a picker open for as long as they like, so there is no
    // timeout on an answer. A portal that has died will not answer at all.
    m_portalGone = new QDBusServiceWatcher(kService, QDBusConnection::sessionBus(),
                                           QDBusServiceWatcher::WatchForUnregistration, this);
    connect(m_portalGone, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
        if (m_step == Step::Idle) return;
        stop();
        emit failed(QStringLiteral("The desktop portal went away before it answered"));
    });
}

ScreenCastPortal::~ScreenCastPortal() { stop(); }

bool ScreenCastPortal::available() {
    QDBusInterface props(kService, kPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                         QDBusConnection::sessionBus());
    const QDBusReply<QDBusVariant> version = props.call(QStringLiteral("Get"), kIface, QStringLiteral("version"));
    return version.isValid();
}

uint ScreenCastPortal::availableCursorModes() {
    QDBusInterface props(kService, kPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                         QDBusConnection::sessionBus());
    const QDBusReply<QDBusVariant> modes =
        props.call(QStringLiteral("Get"), kIface, QStringLiteral("AvailableCursorModes"));
    return modes.isValid() ? modes.value().variant().toUInt() : 0u;
}

QString ScreenCastPortal::requestPath(const QString &token) const {
    QString sender = QDBusConnection::sessionBus().baseService();
    sender.remove(0, 1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
}

void ScreenCastPortal::call(const QString &method, const QVariantList &args, const QString &token) {
    // The Response signal can arrive before the method reply, so subscribe to
    // the request object first.
    if (!m_requestPath.isEmpty())
        QDBusConnection::sessionBus().disconnect(kService, m_requestPath, kRequestIface, QStringLiteral("Response"),
                                                 this, SLOT(handleResponse(uint, QVariantMap)));
    m_requestPath = requestPath(token);
    if (!QDBusConnection::sessionBus().connect(kService, m_requestPath, kRequestIface, QStringLiteral("Response"),
                                               this, SLOT(handleResponse(uint, QVariantMap)))) {
        m_step = Step::Idle;
        emit failed(QStringLiteral("%1: could not listen for the portal's answer").arg(method));
        return;
    }

    QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, kIface, method);
    msg.setArguments(args);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, method, run = m_run](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (run != m_run) return;
        const QDBusPendingReply<QDBusObjectPath> reply = *w;
        if (reply.isError()) {
            m_step = Step::Idle;
            emit failed(QStringLiteral("%1: %2").arg(method, reply.error().message()));
        }
    });
}

ScreenCastPortal::Grant ScreenCastPortal::grant(const QString &restoreToken) {
    QDBusMessage lookup = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.impl.portal.PermissionStore"),
        QStringLiteral("/org/freedesktop/impl/portal/PermissionStore"),
        QStringLiteral("org.freedesktop.impl.portal.PermissionStore"), QStringLiteral("Lookup"));
    lookup << QStringLiteral("screencast") << restoreToken;
    const QDBusMessage reply = QDBusConnection::sessionBus().call(lookup, QDBus::Block, 1000);
    if (reply.type() == QDBusMessage::ReplyMessage) return Grant::Held;
    return reply.errorName() == QLatin1String("org.freedesktop.portal.Error.NotFound") ? Grant::Gone : Grant::Unknown;
}

void ScreenCastPortal::start(uint sourceTypes, uint cursorMode, const QString &restoreToken) {
    if (m_step != Step::Idle) return;
    ++m_run;
    m_sourceTypes = sourceTypes;
    m_cursorMode = cursorMode;
    m_restoreToken = restoreToken;
    m_streams.clear();
    m_newRestoreToken.clear();

    const QString token = QStringLiteral("omabroadcast%1").arg(nextSerial());
    const QString sessionToken = QStringLiteral("omabroadcast_s%1_%2").arg(getpid()).arg(nextSerial());
    QVariantMap options{{QStringLiteral("handle_token"), token},
                        {QStringLiteral("session_handle_token"), sessionToken}};
    m_step = Step::CreateSession;
    call(QStringLiteral("CreateSession"), {options}, token);
}

void ScreenCastPortal::handleResponse(uint code, const QVariantMap &rawResults) {
    QVariantMap results;
    for (auto it = rawResults.begin(); it != rawResults.end(); ++it) results.insert(it.key(), it.value());

    if (code != 0) {
        const Step failedStep = m_step;
        m_step = Step::Idle;
        if (code == 1)
            emit failed(QStringLiteral("Cancelled"));
        else
            emit failed(QStringLiteral("The portal refused (%1) at step %2").arg(code).arg(int(failedStep)));
        return;
    }

    switch (m_step) {
    case Step::CreateSession: {
        m_session = results.value(QStringLiteral("session_handle")).toString();
        if (m_session.isEmpty()) {
            m_step = Step::Idle;
            emit failed(QStringLiteral("No session handle from the portal"));
            return;
        }
        QDBusConnection::sessionBus().connect(kService, m_session, kSessionIface, QStringLiteral("Closed"), this,
                                              SIGNAL(closed()));
        const QString token = QStringLiteral("omabroadcast%1").arg(nextSerial());
        QVariantMap options{{QStringLiteral("handle_token"), token},
                            {QStringLiteral("types"), m_sourceTypes},
                            {QStringLiteral("multiple"), false},
                            {QStringLiteral("cursor_mode"), m_cursorMode},
                            {QStringLiteral("persist_mode"), 2u}};
        if (!m_restoreToken.isEmpty()) options.insert(QStringLiteral("restore_token"), m_restoreToken);
        m_step = Step::SelectSources;
        call(QStringLiteral("SelectSources"), {QVariant::fromValue(QDBusObjectPath(m_session)), options}, token);
        return;
    }
    case Step::SelectSources: {
        const QString token = QStringLiteral("omabroadcast%1").arg(nextSerial());
        QVariantMap options{{QStringLiteral("handle_token"), token}};
        m_step = Step::Start;
        call(QStringLiteral("Start"),
             {QVariant::fromValue(QDBusObjectPath(m_session)), QString(), options}, token);
        return;
    }
    case Step::Start: {
        m_newRestoreToken = results.value(QStringLiteral("restore_token")).toString();
        const QVariant streamsVar = results.value(QStringLiteral("streams"));
        Streams streams;
        if (streamsVar.canConvert<QDBusArgument>())
            streamsVar.value<QDBusArgument>() >> streams;
        else
            streams = streamsVar.value<Streams>();
        if (streams.isEmpty()) {
            m_step = Step::Idle;
            emit failed(QStringLiteral("The portal started no stream"));
            return;
        }
        for (const Stream &s : streams) {
            QVariantMap props;
            for (auto it = s.second.begin(); it != s.second.end(); ++it) props.insert(it.key(), it.value());
            m_streams.append(QVariant::fromValue(QPair<uint, QVariantMap>(s.first, props)));
        }
        openRemote();
        return;
    }
    case Step::Idle:
        return;
    }
}

void ScreenCastPortal::openRemote() {
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, kIface, QStringLiteral("OpenPipeWireRemote"));
    msg.setArguments({QVariant::fromValue(QDBusObjectPath(m_session)), QVariantMap()});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, run = m_run](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (run != m_run) return;  // stopped meanwhile
        const QDBusPendingReply<QDBusUnixFileDescriptor> reply = *w;
        m_step = Step::Idle;
        if (reply.isError()) {
            emit failed(QStringLiteral("OpenPipeWireRemote: %1").arg(reply.error().message()));
            return;
        }
        const int fd = dup(reply.value().fileDescriptor());
        if (fd < 0) {
            emit failed(QStringLiteral("Could not keep the PipeWire connection"));
            return;
        }
        const auto first = m_streams.first().value<QPair<uint, QVariantMap>>();
        emit started(first.first, fd, m_newRestoreToken, first.second);
    });
}

void ScreenCastPortal::stop() {
    ++m_run;
    // An answer still on its way is for nobody now.
    if (!m_requestPath.isEmpty())
        QDBusConnection::sessionBus().disconnect(kService, m_requestPath, kRequestIface, QStringLiteral("Response"),
                                                 this, SLOT(handleResponse(uint, QVariantMap)));
    m_requestPath.clear();
    m_step = Step::Idle;
    if (m_session.isEmpty()) return;
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, m_session, kSessionIface, QStringLiteral("Close"));
    QDBusConnection::sessionBus().asyncCall(msg);
    m_session.clear();
    m_step = Step::Idle;
}
