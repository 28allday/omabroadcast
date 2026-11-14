#include "global_shortcuts.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QUuid>
#include <algorithm>

namespace {
const QString service = "org.freedesktop.portal.Desktop";
const QString path = "/org/freedesktop/portal/desktop";
const QString interface = "org.freedesktop.portal.GlobalShortcuts";
const QString requestInterface = "org.freedesktop.portal.Request";
const QString sessionInterface = "org.freedesktop.portal.Session";
QString token() { return "omab_hotkeys_" + QUuid::createUuid().toString(QUuid::Id128); }
void close(const QString &path, const QString &interface) {
    if (!path.isEmpty()) QDBusConnection::sessionBus().asyncCall(QDBusMessage::createMethodCall(service, path, interface, "Close"));
}
}
QDBusArgument &operator<<(QDBusArgument &arg, const PortalShortcut &value) {
    arg.beginStructure(); arg << value.id << value.properties; arg.endStructure(); return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, PortalShortcut &value) {
    arg.beginStructure(); arg >> value.id >> value.properties; arg.endStructure(); return arg;
}
GlobalShortcuts::GlobalShortcuts(QObject *parent) : QObject(parent) {
    qDBusRegisterMetaType<PortalShortcut>(); qDBusRegisterMetaType<PortalShortcuts>();
    auto bus = QDBusConnection::sessionBus();
    bus.connect(service, path, interface, "Activated", this, SLOT(activated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
    bus.connect(service, path, interface, "Deactivated", this, SLOT(deactivated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
    bus.connect(service, path, interface, "ShortcutsChanged", this, SLOT(shortcutsChanged(QDBusObjectPath,PortalShortcuts)));
    auto *watcher = new QDBusServiceWatcher(service, bus, QDBusServiceWatcher::WatchForUnregistration, this);
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
        if (!m_session.isEmpty() || busy()) fail(tr("Global shortcuts disconnected. Apply them again to reconnect."));
    });
    m_status = tr("Global audio shortcuts are off.");
}
GlobalShortcuts::~GlobalShortcuts() { stop(); }
void GlobalShortcuts::disconnectRequest() {
    if (m_request.isEmpty()) return;
    QDBusConnection::sessionBus().disconnect(service, m_request, requestInterface, "Response", this, SLOT(response(uint,QVariantMap)));
    m_request.clear();
}
void GlobalShortcuts::stop() {
    ++m_generation;
    close(m_request, requestInterface); disconnectRequest();
    if (!m_session.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(service, m_session, sessionInterface, "Closed", this, SLOT(sessionClosed()));
        close(m_session, sessionInterface); m_session.clear();
    }
    m_step = Idle; m_refreshPending = false; m_triggers.clear(); m_status = tr("Global audio shortcuts are off.");
    emit cleared(); emit changed();
}
void GlobalShortcuts::fail(const QString &message) { stop(); m_status = message; emit changed(); }
void GlobalShortcuts::bind(const PortalShortcuts &shortcuts) {
    stop(); m_shortcuts = shortcuts;
    if (shortcuts.isEmpty()) { m_status = tr("Enable push-to-talk or push-to-mute on a channel first."); emit changed(); return; }
    m_step = Create; m_status = tr("Connecting global shortcuts…"); emit changed();
    call("CreateSession", {}, {{"session_handle_token", token()}});
}
void GlobalShortcuts::call(const QString &method, QVariantList arguments, QVariantMap options) {
    disconnectRequest();
    const QString id = token(); options["handle_token"] = id;
    QString sender = QDBusConnection::sessionBus().baseService().mid(1); sender.replace('.', '_');
    m_request = path + "/request/" + sender + "/" + id;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.connect(service, m_request, requestInterface, "Response", this, SLOT(response(uint,QVariantMap)))) {
        fail(tr("Could not listen for the global shortcut response.")); return;
    }
    arguments.append(options);
    auto message = QDBusMessage::createMethodCall(service, path, interface, method); message.setArguments(arguments);
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
    const auto generation = m_generation;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher *watcher) {
        const QDBusPendingReply<QDBusObjectPath> reply = *watcher; watcher->deleteLater();
        if (generation == m_generation && reply.isError()) fail(tr("Global shortcuts unavailable: %1").arg(reply.error().message()));
    });
}
void GlobalShortcuts::response(uint code, const QVariantMap &results) {
    disconnectRequest();
    if (code) { fail(code == 1 ? tr("Global shortcut setup cancelled.") : tr("The desktop refused global shortcut setup.")); return; }
    if (m_step == Create) {
        const auto value = results.value("session_handle");
        m_session = value.canConvert<QDBusObjectPath>() ? value.value<QDBusObjectPath>().path() : value.toString();
        if (m_session.isEmpty() || !m_session.startsWith('/')) { fail(tr("The desktop returned no shortcut session.")); return; }
        QDBusConnection::sessionBus().connect(service, m_session, sessionInterface, "Closed", this, SLOT(sessionClosed()));
        m_step = Bind; m_status = tr("Choose audio shortcuts in the desktop dialog…"); emit changed();
        call("BindShortcuts", {QVariant::fromValue(QDBusObjectPath(m_session)), QVariant::fromValue(m_shortcuts), QString()}, {});
    } else if (m_step == Bind || m_step == List) {
        m_step = Idle;
        const auto raw = results.value("shortcuts");
        PortalShortcuts shortcuts;
        if (raw.canConvert<QDBusArgument>()) raw.value<QDBusArgument>() >> shortcuts;
        else shortcuts = raw.value<PortalShortcuts>();
        updateTriggers(shortcuts);
        if (m_refreshPending) {
            m_refreshPending = false;
            shortcutsChanged(QDBusObjectPath(m_session), {});
        }
    }
}
void GlobalShortcuts::updateTriggers(const PortalShortcuts &shortcuts) {
    emit cleared(); m_triggers.clear();
    for (const auto &shortcut : shortcuts) {
        const auto known = std::find_if(m_shortcuts.begin(), m_shortcuts.end(), [&](const auto &s) { return s.id == shortcut.id; });
        if (known != m_shortcuts.end()) m_triggers[shortcut.id] = shortcut.properties.value("trigger_description", tr("Desktop binding"));
    }
    m_status = tr("%1 global audio shortcuts registered.").arg(m_triggers.size()); emit changed();
}
void GlobalShortcuts::activated(const QDBusObjectPath &session, const QString &id, qulonglong, const QVariantMap &) {
    if (session.path() == m_session && m_triggers.contains(id)) emit pressed(id, true);
}
void GlobalShortcuts::deactivated(const QDBusObjectPath &session, const QString &id, qulonglong, const QVariantMap &) {
    if (session.path() == m_session && m_triggers.contains(id)) emit pressed(id, false);
}
void GlobalShortcuts::shortcutsChanged(const QDBusObjectPath &session, const PortalShortcuts &shortcuts) {
    Q_UNUSED(shortcuts);
    if (session.path() != m_session) return;
    // A notification may describe only changed entries. Read the complete
    // current list so rebinding one action cannot discard other actions.
    emit cleared(); m_triggers.clear();
    if (busy()) { m_refreshPending = true; return; }
    m_step = List;
    call("ListShortcuts", {QVariant::fromValue(QDBusObjectPath(m_session))}, {});
    emit changed();
}
void GlobalShortcuts::sessionClosed() { fail(tr("Global shortcut session closed. Apply them again to reconnect.")); }
