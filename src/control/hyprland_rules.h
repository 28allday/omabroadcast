#pragma once
#include <QLocalSocket>
#include <QObject>
#include <QString>

// Keeps our own windows out of screen capture on Hyprland: one named window
// rule (no_screen_share) for our app id, registered through `hyprctl eval`,
// so nothing is written to the user's config. A second rule floats and pins
// projectors whose title asks to stay on top (kProjectorOnTop). Hyprland
// drops rules made this way when it reloads its config; the event socket
// says when, and they are made again. Elsewhere this does nothing.
class HyprlandRules : public QObject {
    Q_OBJECT
public:
    explicit HyprlandRules(const QString &appId, QObject *parent = nullptr);
    static bool available();
    bool hidden() const { return m_hidden; }
    // Applies at once, to open windows too; returns false if Hyprland refused.
    bool setHidden(bool hidden);
    // Windows titled with this prefix float above the others on every workspace.
    static constexpr const char *kProjectorOnTop = "OmaBroadcast projector (on top)";
    // For windows opened from now on; returns false if Hyprland refused.
    bool setProjectorsOnTop(bool onTop);
    // The prompter's window alone: out of every screen capture, floating above the
    // others on every workspace. The rest of the program is left as it is.
    static constexpr const char *kPrompter = "OmaBroadcast prompter";
    // For windows opened from now on; returns false if Hyprland refused.
    bool setPrompter(bool on);
    // The playback window: floating, and out of screen capture so that a
    // recording of the screen never takes in the one being checked.
    static constexpr const char *kPlayback = "OmaBroadcast playback";
    bool setPlayback(bool on);

private:
    bool apply();
    bool applyProjectors();
    bool applyPrompter();
    bool applyPlayback();
    bool eval(const QString &lua);
    bool m_onTop = false;
    bool m_prompter = false;
    bool m_playback = false;
    void listen();
    QString m_appId;
    bool m_hidden = false;
    QLocalSocket m_events;
    QByteArray m_pending;
};
