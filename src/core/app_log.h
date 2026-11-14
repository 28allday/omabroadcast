#pragma once

#include <QList>
#include <QString>
#include <QtGlobal>

#include <atomic>
#include <deque>
#include <mutex>

// The app's own log. Every message Qt logs still goes to stderr as before,
// and also into a ring of the latest lines (for the log viewer) and into a
// small file for this run, beside the run before's. A file that passes its
// cap is moved aside (.1) and started again, so a run never keeps much more
// than twice the cap on disk. A fatal signal adds one line to the file.
class AppLog {
public:
    enum Level { Debug, Info, Warning, Critical, Fatal };
    struct Line {
        qint64 msecs = 0;  // since the epoch; 0 where the file does not say
        int level = Info;
        QString text;
    };

    explicit AppLog(int capacity = 5000);
    ~AppLog();

    // Starts this run's file in `dir`, moving the last run's aside as the
    // previous one. With `files` false only memory is used (another copy of
    // the app is writing there).
    bool open(const QString &dir, qint64 fileCap = 512 * 1024, bool files = true);
    // Qt's messages and fatal signals come here from now on, until destroyed.
    void install();

    void append(int level, const QString &text);
    QList<Line> lines() const;
    quint64 serial() const { return m_serial.load(); }  // lines appended so far
    QString currentPath() const { return m_current; }
    QString previousPath() const { return m_previous; }

    // The installed log, or null.
    static AppLog *instance();
    // A log file, with the .1 before it, read back.
    static QList<Line> readFile(const QString &path);
    // One line as the files and the viewer's copies have it.
    static QString format(const Line &line);
    static QString levelName(int level);
    static int levelOf(QtMsgType type);
    // File names in a log directory.
    static QString currentName() { return QStringLiteral("omabroadcast.log"); }
    static QString previousName() { return QStringLiteral("omabroadcast.previous.log"); }

private:
    void writeLine(const Line &line);
    void startFile();

    int m_capacity;
    mutable std::mutex m_mutex;
    std::deque<Line> m_lines;
    std::atomic<quint64> m_serial{0};
    QString m_current, m_previous;
    int m_fd = -1;
    qint64 m_cap = 0, m_written = 0;
    bool m_installed = false;
};
