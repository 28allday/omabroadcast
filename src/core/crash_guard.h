#pragma once

#include <QString>

// Did the last run end properly? Each running copy leaves a marker in the
// state folder (its process id, the boot it started in and its program) and
// removes it when it quits cleanly. A marker whose copy is no longer running
// means that run crashed, was killed or lost power. Nothing leaves the machine.
class CrashGuard {
public:
    explicit CrashGuard(const QString &dir);
    ~CrashGuard();  // a clean exit: our marker goes

    // Reads the markers left (removing those of runs that are over), then
    // leaves ours.
    void begin();
    // Reads them again: a copy that was running may have gone.
    void look();
    void end();
    bool previousRunCrashed() const { return m_crashed; }
    // Another copy of the app is running now.
    bool otherCopyRunning() const { return m_otherRunning; }
    QString markerPath() const { return m_marker; }

    // Writes a marker as a run would.
    static bool writeMarker(const QString &path, qint64 pid, const QString &bootId, const QString &program);
    static QString bootId();

private:
    QString m_dir, m_marker;
    bool m_crashed = false, m_otherRunning = false;
};
