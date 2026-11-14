#include "hotkeys.h"
#include <QGuiApplication>
#include <QDateTime>
#include <QKeyEvent>

Hotkeys::Hotkeys(QObject *parent) : QObject(parent) {
    qApp->installEventFilter(this);
    connect(&m_global, &GlobalShortcuts::changed, this, &Hotkeys::changed);
    connect(&m_global, &GlobalShortcuts::cleared, this, [this] { emit cancelled(4); });
    connect(&m_global, &GlobalShortcuts::pressed, this, [this](const QString &id, bool down) {
        for (const auto &binding : m_bindings) {
            if (binding.id != id) continue;
            if (!binding.action.isEmpty()) { if (down) fire(binding, false); }
            else emit held(binding.target, binding.talk, down, 4);
        }
    });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) { m_pressed.clear(); emit cancelled(3); }
    });
}
Hotkeys::~Hotkeys() { qApp->removeEventFilter(this); m_global.stop(); }
void Hotkeys::setBindings(const QList<Binding> &bindings) {
    if (m_bindings == bindings) return;
    // A new name alone (a renamed source) keeps every key as it is.
    const auto sameKeys = [&] {
        if (bindings.size() != m_bindings.size()) return false;
        for (qsizetype i = 0; i < bindings.size(); ++i) {
            Binding a = bindings[i];
            a.description = m_bindings[i].description;
            if (!(a == m_bindings[i])) return false;
        }
        return true;
    };
    if (sameKeys()) { m_bindings = bindings; return; }
    m_pressed.clear(); emit cancelled(6); m_global.stop(); m_bindings = bindings;
    // Global keys that were on stay on, with the new set.
    if (m_globalWanted) bindGlobal();
}
void Hotkeys::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (!enabled) { m_pressed.clear(); emit cancelled(3); }
}
void Hotkeys::release(int key) {
    for (const auto &binding : m_pressed.take(key)) if (binding.action.isEmpty()) emit held(binding.target, binding.talk, false, 2);
}
bool Hotkeys::eventFilter(QObject *, QEvent *event) {
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease && event->type() != QEvent::ShortcutOverride) return false;
    const auto *key = static_cast<QKeyEvent *>(event);
    if (event->type() == QEvent::KeyRelease && !key->isAutoRepeat()) {
        const bool used = m_pressed.contains(key->key());
        release(key->key());
        Qt::KeyboardModifier modifier = Qt::NoModifier;
        if (key->key() == Qt::Key_Control) modifier = Qt::ControlModifier;
        if (key->key() == Qt::Key_Shift) modifier = Qt::ShiftModifier;
        if (key->key() == Qt::Key_Alt) modifier = Qt::AltModifier;
        if (key->key() == Qt::Key_Meta) modifier = Qt::MetaModifier;
        if (modifier != Qt::NoModifier) for (int pressed : m_pressed.keys()) {
            for (const auto &binding : m_pressed.value(pressed))
                if (binding.key[0].keyboardModifiers().testFlag(modifier)) { release(pressed); break; }
        }
        return used;
    }
    if (!m_enabled || event->type() == QEvent::KeyRelease) return false;
    QList<Binding> matches;
    const QKeyCombination combination(key->modifiers() & ~Qt::KeypadModifier, Qt::Key(key->key()));
    // A key that types (a letter, Shift and a digit) belongs to the text field
    // being typed in, not to a hotkey.
    const bool types = key->key() < Qt::Key_Escape &&
                       !(combination.keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    if (types) {
        if (QObject *focus = QGuiApplication::focusObject()) {
            QInputMethodQueryEvent query(Qt::ImEnabled);
            QCoreApplication::sendEvent(focus, &query);
            if (query.value(Qt::ImEnabled).toBool()) return false;
        }
    }
    for (const auto &binding : m_bindings)
        if (binding.key.count() == 1 && binding.key[0] == combination) matches.append(binding);
    if (matches.isEmpty()) return false;
    if (event->type() == QEvent::ShortcutOverride) { event->accept(); return true; }
    if (!key->isAutoRepeat() && !m_pressed.contains(key->key())) {
        m_pressed[key->key()] = matches;
        for (const auto &binding : matches) {
            if (!binding.action.isEmpty()) fire(binding, true);
            else emit held(binding.target, binding.talk, true, 2);
        }
    }
    return true;
}
void Hotkeys::fire(const Binding &binding, bool local) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_fired.value(binding.id, -1000) < 250) return;
    m_fired.insert(binding.id, now);
    emit triggered(binding.action, binding.argument, local);
}

QString Hotkeys::portalTrigger(const QKeySequence &sequence) {
    if (sequence.count() != 1) return {};
    const auto key = sequence[0]; QString name;
    if (key.key() >= Qt::Key_A && key.key() <= Qt::Key_Z) name = QChar('a' + key.key()-Qt::Key_A);
    else if (key.key() >= Qt::Key_0 && key.key() <= Qt::Key_9) name = QChar('0' + key.key()-Qt::Key_0);
    else if (key.key() >= Qt::Key_F1 && key.key() <= Qt::Key_F35) name = QString("F%1").arg(key.key()-Qt::Key_F1+1);
    else switch (key.key()) {
        case Qt::Key_Space: name = "space"; break; case Qt::Key_Return: name = "Return"; break;
        case Qt::Key_Tab: name = "Tab"; break; case Qt::Key_Escape: name = "Escape"; break;
        case Qt::Key_Backspace: name = "BackSpace"; break; case Qt::Key_Delete: name = "Delete"; break;
        case Qt::Key_Insert: name = "Insert"; break; case Qt::Key_Home: name = "Home"; break;
        case Qt::Key_End: name = "End"; break; case Qt::Key_PageUp: name = "Prior"; break;
        case Qt::Key_PageDown: name = "Next"; break; case Qt::Key_Left: name = "Left"; break;
        case Qt::Key_Right: name = "Right"; break; case Qt::Key_Up: name = "Up"; break;
        case Qt::Key_Down: name = "Down"; break; default: return {};
    }
    QStringList parts;
    if (key.keyboardModifiers() & Qt::ControlModifier) parts << "CTRL";
    if (key.keyboardModifiers() & Qt::AltModifier) parts << "ALT";
    if (key.keyboardModifiers() & Qt::ShiftModifier) parts << "SHIFT";
    if (key.keyboardModifiers() & Qt::MetaModifier) parts << "LOGO";
    parts << name; return parts.join('+');
}
void Hotkeys::bindGlobal() {
    m_globalWanted = true;
    PortalShortcuts shortcuts;
    for (const auto &binding : m_bindings) {
        QVariantMap properties{{"description", binding.description}};
        const auto trigger = portalTrigger(binding.key);
        if (!trigger.isEmpty()) properties["preferred_trigger"] = trigger;
        shortcuts.append({binding.id, properties});
    }
    m_global.bind(shortcuts);
}
