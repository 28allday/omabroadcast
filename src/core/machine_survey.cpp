#include "core/machine_survey.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>
#include <QThread>

#include <cmath>

namespace machinesurvey {

std::vector<presets::Display> parseHyprlandMonitors(const QByteArray &json) {
    std::vector<presets::Display> out;
    const QJsonArray monitors = QJsonDocument::fromJson(json).array();
    for (const QJsonValue &v : monitors) {
        const QJsonObject m = v.toObject();
        if (m.value(QStringLiteral("disabled")).toBool()) continue;
        presets::Display d;
        d.name = m.value(QStringLiteral("name")).toString();
        // The model alone: the description adds the maker's full name and the serial number.
        d.description = m.value(QStringLiteral("model")).toString().trimmed();
        if (d.description.isEmpty()) d.description = m.value(QStringLiteral("description")).toString();
        d.width = m.value(QStringLiteral("width")).toInt();
        d.height = m.value(QStringLiteral("height")).toInt();
        d.refresh = m.value(QStringLiteral("refreshRate")).toDouble(60);
        // Transforms 1, 3, 5 and 7 turn the picture a quarter.
        if (m.value(QStringLiteral("transform")).toInt() % 2) std::swap(d.width, d.height);
        if (d.width > 0 && d.height > 0) out.push_back(d);
    }
    return out;
}

std::vector<presets::Display> displays() {
    if (!qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE") && !QStandardPaths::findExecutable(QStringLiteral("hyprctl")).isEmpty()) {
        QProcess hyprctl;
        hyprctl.start(QStringLiteral("hyprctl"), {QStringLiteral("-j"), QStringLiteral("monitors")});
        if (hyprctl.waitForFinished(2000) && hyprctl.exitCode() == 0) {
            auto list = parseHyprlandMonitors(hyprctl.readAllStandardOutput());
            if (!list.empty()) return list;
        }
    }
    std::vector<presets::Display> out;
    for (QScreen *screen : QGuiApplication::screens()) {
        presets::Display d;
        d.name = screen->name();
        d.description = screen->model().isEmpty() ? screen->manufacturer() : screen->model();
        // Qt rounds a fractional scale, so this can be off; Hyprland's list is not.
        d.width = int(std::lround(screen->size().width() * screen->devicePixelRatio()));
        d.height = int(std::lround(screen->size().height() * screen->devicePixelRatio()));
        d.refresh = screen->refreshRate() > 0 ? screen->refreshRate() : 60;
        out.push_back(d);
    }
    return out;
}

void parseMemInfo(const QByteArray &text, qint64 *total, qint64 *available) {
    *total = *available = 0;
    for (const QByteArray &line : text.split('\n')) {
        const QList<QByteArray> parts = line.simplified().split(' ');
        if (parts.size() < 2) continue;
        const qint64 kib = parts[1].toLongLong();
        if (parts[0] == "MemTotal:") *total = kib * 1024;
        else if (parts[0] == "MemAvailable:") *available = kib * 1024;
    }
}

Power readPower(const QString &given) {
    const QString folder = !given.isEmpty() ? given : qEnvironmentVariable("OMABROADCAST_POWER_DIR", QStringLiteral("/sys/class/power_supply"));
    Power power;
    bool mains = false, charging = false;
    const auto read = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()).trimmed() : QString();
    };
    for (const QString &name : QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString base = folder + QLatin1Char('/') + name + QLatin1Char('/');
        const QString type = read(base + QStringLiteral("type"));
        // A mouse or a pad reports its own battery: only the machine's counts.
        if (type == QLatin1String("Battery") && read(base + QStringLiteral("scope")) != QLatin1String("Device")) {
            power.battery = true;
            const QString status = read(base + QStringLiteral("status"));
            charging |= status == QLatin1String("Charging") || status == QLatin1String("Full") || status == QLatin1String("Not charging");
        } else if (type == QLatin1String("Mains") || type == QLatin1String("USB")) {
            mains |= read(base + QStringLiteral("online")) == QLatin1String("1");
        }
    }
    power.onBattery = power.battery && !mains && !charging;
    return power;
}

void fill(presets::Survey *survey) {
    survey->displays = displays();
    QFile meminfo(QStringLiteral("/proc/meminfo"));
    if (meminfo.open(QIODevice::ReadOnly)) parseMemInfo(meminfo.readAll(), &survey->memoryBytes, &survey->availableMemoryBytes);
    survey->cpuThreads = QThread::idealThreadCount();
    const Power power = readPower();
    survey->battery = power.battery;
    survey->onBattery = power.onBattery;
}

}  // namespace machinesurvey
