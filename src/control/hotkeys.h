#pragma once
#include "global_shortcuts.h"
#include <QKeySequence>
#include <QHash>
#include <QMap>

// Keys for the app: held ones (push to talk or mute a channel) and ones
// that run an action when pressed. Locally while the window has the keys,
// and through the desktop's GlobalShortcuts portal once asked.
class Hotkeys : public QObject {
    Q_OBJECT
public:
    struct Binding {
        QString id, target, description;
        bool talk = true;
        QKeySequence key;
        QString action, argument;   // set: a press runs it (target and talk unused)
        bool operator==(const Binding &) const = default;
    };
    explicit Hotkeys(QObject *parent = nullptr);
    ~Hotkeys() override;
    void setBindings(const QList<Binding> &bindings);
    void setEnabled(bool enabled);
    void bindGlobal();
    void stopGlobal() { m_globalWanted = false; m_global.stop(); }
    QString status() const { return m_global.status(); }
    QVariantMap triggers() const { return m_global.triggers(); }
    bool busy() const { return m_global.busy(); }
    static QString portalTrigger(const QKeySequence &sequence);
signals:
    void held(const QString &target, bool talk, bool down, unsigned origin);
    // `local`: from this window's keys, not the desktop's. The same key
    // arriving both ways at once runs once.
    void triggered(const QString &action, const QString &argument, bool local);
    void cancelled(unsigned origins);
    void changed();
protected:
    bool eventFilter(QObject *, QEvent *) override;
private:
    void release(int key);
    void fire(const Binding &binding, bool local);
    QList<Binding> m_bindings;
    QHash<QString, qint64> m_fired;  // binding id → when it last ran
    QMap<int, QList<Binding>> m_pressed;
    bool m_enabled = false;
    GlobalShortcuts m_global;
    bool m_globalWanted = false;  // bound by bindGlobal(), until stopGlobal()
};
