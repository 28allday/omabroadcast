#include "system_stats.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>

#include <algorithm>

namespace systemstats {

namespace {
QList<QByteArray> fields(const QByteArray &line) {
    QList<QByteArray> out;
    for (const QByteArray &f : line.simplified().split(' '))
        if (!f.isEmpty()) out << f;
    return out;
}
}  // namespace

bool parseCpuTimes(const QByteArray &procStat, CpuTimes *out) {
    for (const QByteArray &line : procStat.split('\n')) {
        const QList<QByteArray> f = fields(line);
        if (f.isEmpty() || f[0] != "cpu") continue;
        // user nice system idle iowait irq softirq steal; guest time is in user already.
        if (f.size() < 5) return false;
        uint64_t total = 0, idle = 0;
        for (int i = 1; i < std::min<qsizetype>(f.size(), 9); ++i) {
            bool ok = false;
            const uint64_t v = f[i].toULongLong(&ok);
            if (!ok) return false;
            total += v;
            if (i == 4 || i == 5) idle += v;
        }
        out->total = total;
        out->busy = total - idle;
        return true;
    }
    return false;
}

int parseCpuCount(const QByteArray &procStat) {
    static const QRegularExpression cpu(QStringLiteral("^cpu\\d+$"));
    int n = 0;
    for (const QByteArray &line : procStat.split('\n')) {
        const QList<QByteArray> f = fields(line);
        if (!f.isEmpty() && cpu.match(QString::fromLatin1(f[0])).hasMatch()) ++n;
    }
    return n;
}

bool parseProcessTicks(const QByteArray &pidStat, uint64_t *ticks) {
    // The name in brackets may hold spaces and brackets: count from the last one.
    const qsizetype close = pidStat.lastIndexOf(')');
    if (close < 0) return false;
    const QList<QByteArray> f = fields(pidStat.mid(close + 1));
    // From the state (field 3): utime is field 14, stime 15.
    if (f.size() < 13) return false;
    bool okUser = false, okSystem = false;
    const uint64_t user = f[11].toULongLong(&okUser), system = f[12].toULongLong(&okSystem);
    if (!okUser || !okSystem) return false;
    *ticks = user + system;
    return true;
}

qint64 parseResidentBytes(const QByteArray &pidStatus) {
    for (const QByteArray &line : pidStatus.split('\n')) {
        if (!line.startsWith("VmRSS:")) continue;
        const QList<QByteArray> f = fields(line.mid(6));
        if (f.isEmpty()) return -1;
        bool ok = false;
        const qint64 v = f[0].toLongLong(&ok);
        if (!ok) return -1;
        const QByteArray unit = f.value(1).toLower();
        return unit == "kb" ? v * 1024 : unit == "mb" ? v * 1024 * 1024 : unit.isEmpty() || unit == "b" ? v : -1;
    }
    return -1;
}

int parseBusyPercent(const QByteArray &text) {
    bool ok = false;
    const int v = text.trimmed().toInt(&ok);
    return ok && v >= 0 && v <= 100 ? v : -1;
}

}  // namespace systemstats

namespace {
QByteArray readSmall(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.read(256 * 1024);  // procfs reports no size: read what there is
}
}  // namespace

SystemStats::SystemStats(const QString &procRoot, const QString &sysRoot) : m_proc(procRoot), m_sys(sysRoot) {}

SystemStats::Sample SystemStats::sample() {
    using namespace systemstats;
    Sample s;
    const QByteArray stat = readSmall(m_proc + QStringLiteral("/stat"));
    s.cpus = parseCpuCount(stat);
    CpuTimes cpu;
    uint64_t process = 0;
    const bool haveCpu = parseCpuTimes(stat, &cpu);
    const bool haveProcess = parseProcessTicks(readSmall(m_proc + QStringLiteral("/self/stat")), &process);
    if (haveCpu && m_haveLast && cpu.total > m_lastCpu.total) {
        const double elapsed = double(cpu.total - m_lastCpu.total);
        s.systemCpu = std::clamp(100.0 * double(cpu.busy - std::min(cpu.busy, m_lastCpu.busy)) / elapsed, 0.0, 100.0);
        if (haveProcess && m_haveLastProcess)
            s.processCpu = std::clamp(100.0 * double(process - std::min(process, m_lastProcess)) / elapsed, 0.0, 100.0);
    }
    if (haveCpu) {
        m_lastCpu = cpu;
        m_haveLast = true;
    }
    m_lastProcess = process;
    m_haveLastProcess = haveProcess;
    s.residentBytes = parseResidentBytes(readSmall(m_proc + QStringLiteral("/self/status")));

    static const QRegularExpression cardName(QStringLiteral("^card(\\d+)$"));
    const QDir drm(m_sys + QStringLiteral("/class/drm"));
    QStringList cards;
    for (const QString &name : drm.entryList(QDir::AllEntries | QDir::NoDotAndDotDot))
        if (cardName.match(name).hasMatch()) cards << name;
    std::sort(cards.begin(), cards.end(), [](const QString &a, const QString &b) {
        return a.mid(4).toInt() < b.mid(4).toInt();
    });
    for (const QString &card : cards) {
        Gpu g;
        g.card = card;
        const QString device = drm.filePath(card) + QStringLiteral("/device");
        if (QFile::exists(device + QStringLiteral("/gpu_busy_percent")))
            g.busy = parseBusyPercent(readSmall(device + QStringLiteral("/gpu_busy_percent")));
        for (const QString &node : QDir(device + QStringLiteral("/drm")).entryList({QStringLiteral("renderD*")}, QDir::AllEntries))
            g.renderNode = QStringLiteral("/dev/dri/") + node;
        s.gpus << g;
    }
    return s;
}
