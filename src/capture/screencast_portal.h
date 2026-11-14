#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

class QDBusServiceWatcher;

// One screen-cast session with the desktop portal (org.freedesktop.portal.ScreenCast).
// The portal shows the desktop's own picker; what comes back is a PipeWire
// node id and a connection fd for it. Restore tokens let a source re-open
// without a prompt on the next run.
//
// Asynchronous: start() returns at once and the result arrives as started()
// or failed(), with failed() too if the portal itself goes before answering.
// closed() fires when the compositor ends the session.
class ScreenCastPortal : public QObject {
    Q_OBJECT

public:
    enum SourceType { Monitor = 1, Window = 2, Virtual = 4 };
    enum CursorMode { Hidden = 1, Embedded = 2, Metadata = 4 };

    explicit ScreenCastPortal(QObject *parent = nullptr);
    ~ScreenCastPortal() override;

    static bool available();
    // Bit set of CursorMode values the portal offers; 0 when unknown.
    static uint availableCursorModes();

    // Whether the portal still has a saved choice. It takes a token it has lost
    // as a request to ask, and shows the picker, so this is the only way to know
    // beforehand that starting will be quiet. Unknown when the store did not answer.
    enum class Grant { Held, Gone, Unknown };
    static Grant grant(const QString &restoreToken);

    void start(uint sourceTypes, uint cursorMode, const QString &restoreToken);
    void stop();

    QString sessionPath() const { return m_session; }

signals:
    // `props` carries the stream's properties: "position" (ii), "size" (ii),
    // "source_type" (u), "id" (s) where the backend provides them.
    void started(uint nodeId, int pipewireFd, const QString &restoreToken, const QVariantMap &props);
    void failed(const QString &message);
    void closed();

private slots:
    // Connected by name to the portal's Request.Response D-Bus signal, so it
    // must stay a slot: QDBusConnection::connect only finds slots.
    void handleResponse(uint code, const QVariantMap &results);

private:
    enum class Step { Idle, CreateSession, SelectSources, Start };

    void call(const QString &method, const QVariantList &args, const QString &token);
    void openRemote();
    QString requestPath(const QString &token) const;

    Step m_step = Step::Idle;
    quint64 m_run = 0;  // each start and stop: answers of an earlier one are let go
    QString m_session;
    QString m_requestPath;
    uint m_sourceTypes = Monitor;
    uint m_cursorMode = Embedded;
    QString m_restoreToken;
    QString m_newRestoreToken;
    QVariantList m_streams;
    QDBusServiceWatcher *m_portalGone = nullptr;
};
