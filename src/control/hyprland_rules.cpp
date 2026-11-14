#include "control/hyprland_rules.h"

#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

namespace {
QString signature() { return qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE"); }
}  // namespace

HyprlandRules::HyprlandRules(const QString &appId, QObject *parent) : QObject(parent), m_appId(appId) {
    connect(&m_events, &QLocalSocket::readyRead, this, [this] {
        m_pending += m_events.readAll();
        bool reloaded = false;
        for (qsizetype nl; (nl = m_pending.indexOf('\n')) >= 0; m_pending.remove(0, nl + 1))
            if (m_pending.startsWith("configreloaded>>")) reloaded = true;
        if (reloaded && m_hidden) apply();
        if (reloaded && m_onTop) applyProjectors();
        if (reloaded && m_prompter) applyPrompter();
        if (reloaded && m_playback) applyPlayback();
    });
    // Hyprland restarting its socket (or a slow start) is retried quietly.
    connect(&m_events, &QLocalSocket::disconnected, this, [this] {
        if (m_hidden || m_onTop || m_prompter || m_playback) QTimer::singleShot(2000, this, &HyprlandRules::listen);
    });
}

bool HyprlandRules::available() {
    return !signature().isEmpty() && !QStandardPaths::findExecutable(QStringLiteral("hyprctl")).isEmpty();
}

bool HyprlandRules::setHidden(bool hidden) {
    // Applied even when unchanged: an earlier run may have left the rule on.
    m_hidden = hidden;
    if (!available()) return false;
    if (hidden && m_events.state() == QLocalSocket::UnconnectedState) listen();
    return apply();
}

bool HyprlandRules::apply() {
    // The rule object lives in Hyprland's Lua state, so a second call
    // re-enables it rather than adding another; a config reload clears both.
    QString id = m_appId;
    id.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")));
    const QString enabled = m_hidden ? QStringLiteral("true") : QStringLiteral("false");
    return eval(QStringLiteral("if _G.omabroadcast_hide_from_capture then _G.omabroadcast_hide_from_capture:set_enabled(%1) "
                               "elseif %1 then _G.omabroadcast_hide_from_capture = hl.window_rule({ name = \"omabroadcast-hide-from-capture\", "
                               "match = { class = \"^(%2)$\" }, no_screen_share = true }) end")
                    .arg(enabled, QRegularExpression::escape(id).replace(QLatin1Char('\\'), QStringLiteral("\\\\"))));
}

bool HyprlandRules::setProjectorsOnTop(bool onTop) {
    m_onTop = onTop;
    if (!available()) return false;
    if (onTop && m_events.state() == QLocalSocket::UnconnectedState) listen();
    return applyProjectors();
}

bool HyprlandRules::applyProjectors() {
    const QString enabled = m_onTop ? QStringLiteral("true") : QStringLiteral("false");
    const QString title = QRegularExpression::escape(QString::fromLatin1(kProjectorOnTop)).replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    return eval(QStringLiteral("if _G.omabroadcast_projectors_on_top then _G.omabroadcast_projectors_on_top:set_enabled(%1) "
                               "elseif %1 then _G.omabroadcast_projectors_on_top = hl.window_rule({ name = \"omabroadcast-projectors-on-top\", "
                               "match = { title = \"^(%2.*)$\" }, float = true, pin = true }) end")
                    .arg(enabled, title));
}

bool HyprlandRules::setPrompter(bool on) {
    m_prompter = on;
    if (!available()) return false;
    if (on && m_events.state() == QLocalSocket::UnconnectedState) listen();
    return applyPrompter();
}

bool HyprlandRules::applyPrompter() {
    const QString enabled = m_prompter ? QStringLiteral("true") : QStringLiteral("false");
    const QString title = QRegularExpression::escape(QString::fromLatin1(kPrompter)).replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    return eval(QStringLiteral("if _G.omabroadcast_prompter then _G.omabroadcast_prompter:set_enabled(%1) "
                               "elseif %1 then _G.omabroadcast_prompter = hl.window_rule({ name = \"omabroadcast-prompter\", "
                               "match = { title = \"^(%2)$\" }, no_screen_share = true, float = true, pin = true }) end")
                    .arg(enabled, title));
}

bool HyprlandRules::setPlayback(bool on) {
    m_playback = on;
    if (!available()) return false;
    if (on && m_events.state() == QLocalSocket::UnconnectedState) listen();
    return applyPlayback();
}

bool HyprlandRules::applyPlayback() {
    const QString enabled = m_playback ? QStringLiteral("true") : QStringLiteral("false");
    const QString title = QRegularExpression::escape(QString::fromLatin1(kPlayback)).replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    return eval(QStringLiteral("if _G.omabroadcast_playback then _G.omabroadcast_playback:set_enabled(%1) "
                               "elseif %1 then _G.omabroadcast_playback = hl.window_rule({ name = \"omabroadcast-playback\", "
                               "match = { title = \"^(%2)$\" }, no_screen_share = true, float = true }) end")
                    .arg(enabled, title));
}

bool HyprlandRules::eval(const QString &lua) {
    QProcess hyprctl;
    hyprctl.start(QStringLiteral("hyprctl"), {QStringLiteral("eval"), lua});
    if (!hyprctl.waitForFinished(3000)) {
        hyprctl.kill();
        return false;
    }
    return hyprctl.exitCode() == 0 && hyprctl.readAllStandardOutput().trimmed() == "ok";
}

void HyprlandRules::listen() {
    if ((!m_hidden && !m_onTop && !m_prompter && !m_playback) || m_events.state() != QLocalSocket::UnconnectedState) return;
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    m_events.connectToServer(runtime + QStringLiteral("/hypr/") + signature() + QStringLiteral("/.socket2.sock"));
}
