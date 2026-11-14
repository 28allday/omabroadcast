#pragma once
#include <QObject>
#include <QProcess>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

// Drawing a display region on the screen itself, on Hyprland: our windows
// step aside to a hidden special workspace (so what is under them shows),
// slurp takes the rectangle, the windows come back where they were, and the
// rectangle becomes pixels of the captured display. Elsewhere, or without
// slurp, available() is false and the snapshot sheet is used instead.
class RegionDrawer : public QObject {
    Q_OBJECT
public:
    explicit RegionDrawer(QObject *parent = nullptr);
    ~RegionDrawer() override;  // windows still aside are put back
    static bool available();
    bool busy() const { return m_slurp.state() != QProcess::NotRunning || !m_parked.isEmpty(); }
    // `display`: the captured display's size in pixels; `area`: where the
    // portal said it sits in the layout, if it did (only its corner is used,
    // to tell displays of one size apart). False if already drawing.
    bool start(const QString &sourceId, QSize display, QRect area);

    // The rectangle drawn (`drawn`, layout points) in pixels of a display of
    // `display` pixels at `area`; empty when the two do not meet.
    static QRect toPixels(QRect drawn, QSize display, QRect area);
    // Where a display of `display` pixels sits in the layout, from `hyprctl
    // monitors -j` output: of the monitors that size, the one at the portal's
    // corner, else the one holding `drawn`'s centre, else the only one;
    // empty when none fits.
    static QRect findArea(const QByteArray &monitorsJson, QSize display, QRect drawn, QRect portal = {});

signals:
    void drawn(const QString &sourceId, const QRect &pixels);
    // An empty message: cancelled (Esc).
    void failed(const QString &sourceId, const QString &message);
    // A window did not go back to where it was.
    void windowsMoved(const QString &message);

private:
    struct Parked { QString address, workspace; };
    void park();
    void restore();
    void finished(int code, QProcess::ExitStatus status);
    static QByteArray hyprctl(const QStringList &args);
    static bool moveWindow(const QString &workspace, const QString &address);
    QString m_sourceId;
    QSize m_display;
    QRect m_area;
    QVector<Parked> m_parked;
    QProcess m_slurp;
};
