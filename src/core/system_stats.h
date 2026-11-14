#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <cstdint>

// What the machine and this process are using, from /proc and sysfs. The
// readers take the files' text; SystemStats
// reads the files under a root (the real ones by default) and turns two
// readings into rates. Graphics cards are found by what sysfs says of every
// card alike: one that reports how busy it is shows it, others say nothing.
namespace systemstats {
// /proc/stat's first line: clock ticks busy and in all, every CPU together.
struct CpuTimes {
    uint64_t busy = 0, total = 0;
};
bool parseCpuTimes(const QByteArray &procStat, CpuTimes *out);
// How many CPUs /proc/stat lists.
int parseCpuCount(const QByteArray &procStat);
// /proc/<pid>/stat: the ticks this process has run, user and system.
bool parseProcessTicks(const QByteArray &pidStat, uint64_t *ticks);
// /proc/<pid>/status: resident memory in bytes; -1 when it does not say.
qint64 parseResidentBytes(const QByteArray &pidStatus);
// A card's gpu_busy_percent: 0 to 100, or -1.
int parseBusyPercent(const QByteArray &text);
}  // namespace systemstats

class SystemStats {
public:
    struct Gpu {
        QString card;        // card0, card1, …
        QString renderNode;  // /dev/dri/renderD128, when it has one
        int busy = -1;       // percent; -1 where the system does not say
    };
    struct Sample {
        double processCpu = -1;  // percent of the whole machine; -1 until two readings
        double systemCpu = -1;
        int cpus = 0;
        qint64 residentBytes = -1;
        QList<Gpu> gpus;
    };

    explicit SystemStats(const QString &procRoot = QStringLiteral("/proc"),
                         const QString &sysRoot = QStringLiteral("/sys"));
    // Reads now; CPU use is over the time since the last call.
    Sample sample();

private:
    QString m_proc, m_sys;
    systemstats::CpuTimes m_lastCpu;
    uint64_t m_lastProcess = 0;
    bool m_haveLast = false, m_haveLastProcess = false;
};
