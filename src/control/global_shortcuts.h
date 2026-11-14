#pragma once
#include <QObject>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QVariantMap>
#include <QList>

struct PortalShortcut { QString id; QVariantMap properties; };
using PortalShortcuts = QList<PortalShortcut>;
Q_DECLARE_METATYPE(PortalShortcut)
Q_DECLARE_METATYPE(PortalShortcuts)
QDBusArgument &operator<<(QDBusArgument &, const PortalShortcut &);
const QDBusArgument &operator>>(const QDBusArgument &, PortalShortcut &);

// GlobalShortcuts v1: asynchronous registration and press/release signals.
// Only an explicit user request opens the portal's binding dialog.
class GlobalShortcuts : public QObject {
    Q_OBJECT
public:
    explicit GlobalShortcuts(QObject *parent = nullptr);
    ~GlobalShortcuts() override;
    void bind(const PortalShortcuts &shortcuts);
    void stop();
    QString status() const { return m_status; }
    bool busy() const { return m_step != Idle; }
    QVariantMap triggers() const { return m_triggers; }
signals:
    void changed();
    void pressed(const QString &id, bool held);
    void cleared();
private slots:
    void response(uint code, const QVariantMap &results);
    void activated(const QDBusObjectPath &, const QString &, qulonglong, const QVariantMap &);
    void deactivated(const QDBusObjectPath &, const QString &, qulonglong, const QVariantMap &);
    void shortcutsChanged(const QDBusObjectPath &, const PortalShortcuts &);
    void sessionClosed();
private:
    enum Step { Idle, Create, Bind, List };
    void call(const QString &method, QVariantList arguments, QVariantMap options);
    void fail(const QString &message);
    void updateTriggers(const PortalShortcuts &shortcuts);
    void disconnectRequest();
    Step m_step = Idle;
    bool m_refreshPending = false;
    unsigned m_generation = 0;
    QString m_session, m_request, m_status;
    PortalShortcuts m_shortcuts;
    QVariantMap m_triggers;
};
