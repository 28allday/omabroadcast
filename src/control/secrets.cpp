#include "secrets.h"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QEventLoop>
#include <QMap>
#include <QTimer>

namespace {
const QString kService = QStringLiteral("org.freedesktop.secrets");
const QString kPath = QStringLiteral("/org/freedesktop/secrets");
const QString kServiceIface = QStringLiteral("org.freedesktop.Secret.Service");
const QString kDefault = QStringLiteral("/org/freedesktop/secrets/aliases/default");

// The Secret Service's (oayays): which session, its parameters, the value,
// and what the value is.
struct SecretValue {
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};

using Attributes = QMap<QString, QString>;
using Secrets = QMap<QDBusObjectPath, SecretValue>;

Attributes attributesFor(const QString &id) {
    return {{QStringLiteral("application"), QStringLiteral("omabroadcast")}, {QStringLiteral("stream"), id}};
}

QString errorOf(const QDBusMessage &reply) {
    return reply.type() == QDBusMessage::ErrorMessage ? reply.errorMessage() : QString();
}
}  // namespace

Q_DECLARE_METATYPE(SecretValue)

QDBusArgument &operator<<(QDBusArgument &a, const SecretValue &s) {
    a.beginStructure();
    a << s.session << s.parameters << s.value << s.contentType;
    a.endStructure();
    return a;
}

const QDBusArgument &operator>>(const QDBusArgument &a, SecretValue &s) {
    a.beginStructure();
    a >> s.session >> s.parameters >> s.value >> s.contentType;
    a.endStructure();
    return a;
}

// Waits for a keyring prompt's answer.
class SecretPromptWaiter : public QObject {
    Q_OBJECT
public:
    QEventLoop loop;
    bool dismissed = true;
public slots:
    void completed(bool wasDismissed, const QDBusVariant &) {
        dismissed = wasDismissed;
        loop.quit();
    }
};

SecretStore::SecretStore(QDBusConnection bus) : m_bus(bus) {
    static const bool registered = [] {
        qDBusRegisterMetaType<SecretValue>();
        qDBusRegisterMetaType<Attributes>();
        qDBusRegisterMetaType<Secrets>();
        return true;
    }();
    Q_UNUSED(registered);
}

bool SecretStore::available() const {
    if (!m_bus.isConnected() || !m_bus.interface()) return false;
    const auto registered = m_bus.interface()->isServiceRegistered(kService);
    if (registered.isValid() && registered.value()) return true;
    // A keyring that starts on demand is listed as activatable.
    const auto activatable = m_bus.interface()->activatableServiceNames();
    return activatable.isValid() && activatable.value().contains(kService);
}

QString SecretStore::openSession(QString *error) {
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kServiceIface, QStringLiteral("OpenSession"));
    // "plain": the bus is local to this login, as every keyring client uses it.
    call << QStringLiteral("plain") << QVariant::fromValue(QDBusVariant(QString()));
    const QDBusMessage reply = m_bus.call(call, QDBus::Block, 10000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
        if (error) *error = errorOf(reply).isEmpty() ? QStringLiteral("the keyring did not answer") : errorOf(reply);
        return {};
    }
    return qdbus_cast<QDBusObjectPath>(reply.arguments().at(1)).path();
}

bool SecretStore::prompt(const QString &path, QString *error) {
    if (path.isEmpty() || path == QLatin1String("/")) return true;
    SecretPromptWaiter waiter;
    m_bus.connect(kService, path, QStringLiteral("org.freedesktop.Secret.Prompt"), QStringLiteral("Completed"), &waiter,
                  SLOT(completed(bool, QDBusVariant)));
    QDBusMessage call = QDBusMessage::createMethodCall(kService, path, QStringLiteral("org.freedesktop.Secret.Prompt"),
                                                       QStringLiteral("Prompt"));
    call << QString();
    const QDBusMessage reply = m_bus.call(call, QDBus::Block, 10000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (error) *error = errorOf(reply);
        return false;
    }
    QTimer::singleShot(120000, &waiter.loop, &QEventLoop::quit);  // nobody answered
    QTimer wanted;
    if (stillWanted) {
        QObject::connect(&wanted, &QTimer::timeout, &waiter.loop, [&] { if (!stillWanted()) waiter.loop.quit(); });
        wanted.start(250);
    }
    waiter.loop.exec();
    m_bus.disconnect(kService, path, QStringLiteral("org.freedesktop.Secret.Prompt"), QStringLiteral("Completed"), &waiter,
                     SLOT(completed(bool, QDBusVariant)));
    if (waiter.dismissed && error) *error = QStringLiteral("the keyring stayed locked");
    return !waiter.dismissed;
}

bool SecretStore::store(const QString &id, const QString &label, const QByteArray &secret, QString *error) {
    const QString session = openSession(error);
    if (session.isEmpty()) return false;
    // A locked keyring is unlocked first; that is the person's to allow.
    QDBusMessage unlock = QDBusMessage::createMethodCall(kService, kPath, kServiceIface, QStringLiteral("Unlock"));
    unlock << QVariant::fromValue(QList<QDBusObjectPath>{QDBusObjectPath(kDefault)});
    const QDBusMessage unlocked = m_bus.call(unlock, QDBus::Block, 10000);
    if (unlocked.type() == QDBusMessage::ReplyMessage && unlocked.arguments().size() >= 2 &&
        !prompt(qdbus_cast<QDBusObjectPath>(unlocked.arguments().at(1)).path(), error))
        return false;

    QVariantMap properties;
    properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Label"), label);
    properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Attributes"), QVariant::fromValue(attributesFor(id)));
    SecretValue value{QDBusObjectPath(session), {}, secret, QStringLiteral("text/plain")};
    QDBusMessage create = QDBusMessage::createMethodCall(kService, kDefault, QStringLiteral("org.freedesktop.Secret.Collection"),
                                                         QStringLiteral("CreateItem"));
    create << properties << QVariant::fromValue(value) << true;
    const QDBusMessage reply = m_bus.call(create, QDBus::Block, 10000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
        if (error) *error = errorOf(reply).isEmpty() ? QStringLiteral("the keyring refused the key") : errorOf(reply);
        return false;
    }
    return prompt(qdbus_cast<QDBusObjectPath>(reply.arguments().at(1)).path(), error);
}

QByteArray SecretStore::lookup(const QString &id, QString *error) {
    QDBusMessage search = QDBusMessage::createMethodCall(kService, kPath, kServiceIface, QStringLiteral("SearchItems"));
    search << QVariant::fromValue(attributesFor(id));
    const QDBusMessage found = m_bus.call(search, QDBus::Block, 10000);
    if (found.type() != QDBusMessage::ReplyMessage || found.arguments().size() < 2) {
        if (error) *error = errorOf(found).isEmpty() ? QStringLiteral("the keyring did not answer") : errorOf(found);
        return {};
    }
    QList<QDBusObjectPath> items = qdbus_cast<QList<QDBusObjectPath>>(found.arguments().at(0));
    const QList<QDBusObjectPath> locked = qdbus_cast<QList<QDBusObjectPath>>(found.arguments().at(1));
    if (items.isEmpty() && !locked.isEmpty()) {
        QDBusMessage unlock = QDBusMessage::createMethodCall(kService, kPath, kServiceIface, QStringLiteral("Unlock"));
        unlock << QVariant::fromValue(locked);
        const QDBusMessage unlocked = m_bus.call(unlock, QDBus::Block, 10000);
        if (unlocked.type() != QDBusMessage::ReplyMessage || unlocked.arguments().size() < 2) {
            if (error) *error = errorOf(unlocked).isEmpty() ? QStringLiteral("the keyring did not answer") : errorOf(unlocked);
            return {};
        }
        if (!prompt(qdbus_cast<QDBusObjectPath>(unlocked.arguments().at(1)).path(), error)) return {};
        items = locked;
    }
    if (items.isEmpty()) return {};
    const QString session = openSession(error);
    if (session.isEmpty()) return {};
    QDBusMessage get = QDBusMessage::createMethodCall(kService, kPath, kServiceIface, QStringLiteral("GetSecrets"));
    get << QVariant::fromValue(QList<QDBusObjectPath>{items.first()}) << QVariant::fromValue(QDBusObjectPath(session));
    const QDBusMessage reply = m_bus.call(get, QDBus::Block, 10000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        if (error) *error = errorOf(reply).isEmpty() ? QStringLiteral("the keyring did not answer") : errorOf(reply);
        return {};
    }
    const Secrets secrets = qdbus_cast<Secrets>(reply.arguments().at(0));
    return secrets.isEmpty() ? QByteArray() : secrets.first().value;
}

bool SecretStore::has(const QString &id, QString *error) {
    QDBusMessage search = QDBusMessage::createMethodCall(kService, kPath, kServiceIface, QStringLiteral("SearchItems"));
    search << QVariant::fromValue(attributesFor(id));
    const QDBusMessage found = m_bus.call(search, QDBus::Block, 2000);
    if (found.type() != QDBusMessage::ReplyMessage || found.arguments().size() < 2) {
        if (error) *error = errorOf(found).isEmpty() ? QStringLiteral("the keyring did not answer") : errorOf(found);
        return false;
    }
    // Locked or not, it is there.
    return !qdbus_cast<QList<QDBusObjectPath>>(found.arguments().at(0)).isEmpty() ||
           !qdbus_cast<QList<QDBusObjectPath>>(found.arguments().at(1)).isEmpty();
}

bool SecretStore::remove(const QString &id, QString *error) {
    QDBusMessage search = QDBusMessage::createMethodCall(kService, kPath, kServiceIface, QStringLiteral("SearchItems"));
    search << QVariant::fromValue(attributesFor(id));
    const QDBusMessage found = m_bus.call(search, QDBus::Block, 10000);
    if (found.type() != QDBusMessage::ReplyMessage || found.arguments().size() < 2) {
        if (error) *error = errorOf(found);
        return false;
    }
    for (int i = 0; i < 2; ++i)
        for (const QDBusObjectPath &item : qdbus_cast<QList<QDBusObjectPath>>(found.arguments().at(i))) {
            QDBusMessage del = QDBusMessage::createMethodCall(kService, item.path(), QStringLiteral("org.freedesktop.Secret.Item"),
                                                              QStringLiteral("Delete"));
            const QDBusMessage reply = m_bus.call(del, QDBus::Block, 10000);
            if (reply.type() != QDBusMessage::ReplyMessage) {
                if (error) *error = errorOf(reply);
                return false;
            }
            if (!reply.arguments().isEmpty() && !prompt(qdbus_cast<QDBusObjectPath>(reply.arguments().at(0)).path(), error)) return false;
        }
    return true;
}

#include "secrets.moc"
