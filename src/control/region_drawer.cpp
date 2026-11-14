#include "region_drawer.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {
// Where our windows wait while the region is drawn.
constexpr const char *kAside = "special:omabroadcast-draw";

QString luaString(const QString &s) {
    QString out = s;
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + out + QLatin1Char('"');
}
}  // namespace

RegionDrawer::RegionDrawer(QObject *parent) : QObject(parent) {
    connect(&m_slurp, &QProcess::finished, this, &RegionDrawer::finished);
    connect(&m_slurp, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        restore();
        emit failed(m_sourceId, tr("slurp could not be started."));
    });
}

RegionDrawer::~RegionDrawer() {
    if (m_slurp.state() != QProcess::NotRunning) {
        m_slurp.kill();
        m_slurp.waitForFinished(1000);
    }
    restore();
}

bool RegionDrawer::available() {
    // On Hyprland's own screen: never from an off-screen run that only inherits its variables.
    return qEnvironmentVariableIsSet("HYPRLAND_INSTANCE_SIGNATURE") && QGuiApplication::platformName() == QLatin1String("wayland") && !QStandardPaths::findExecutable(QStringLiteral("slurp")).isEmpty() &&
           !QStandardPaths::findExecutable(QStringLiteral("hyprctl")).isEmpty();
}

QByteArray RegionDrawer::hyprctl(const QStringList &args) {
    QProcess p;
    p.start(QStringLiteral("hyprctl"), args);
    if (!p.waitForFinished(3000)) {
        p.kill();
        return {};
    }
    return p.exitCode() == 0 ? p.readAllStandardOutput() : QByteArray();
}

bool RegionDrawer::moveWindow(const QString &workspace, const QString &address) {
    const QByteArray answer = hyprctl({QStringLiteral("dispatch"), QStringLiteral("hl.dsp.window.move({ workspace = %1, follow = false, window = %2 })")
                                                                       .arg(luaString(workspace), luaString(QStringLiteral("address:") + address))});
    return answer.trimmed() == "ok";
}

bool RegionDrawer::start(const QString &sourceId, QSize display, QRect area) {
    if (busy() || display.isEmpty()) return false;
    m_sourceId = sourceId;
    m_display = display;
    m_area = area;
    park();
    // A moment for the compositor to draw what was under our windows.
    QTimer::singleShot(250, this, [this] {
        m_slurp.start(QStringLiteral("slurp"), {QStringLiteral("-d"), QStringLiteral("-f"), QStringLiteral("%x %y %w %h")});
    });
    return true;
}

void RegionDrawer::park() {
    const auto clients = QJsonDocument::fromJson(hyprctl({QStringLiteral("clients"), QStringLiteral("-j")})).array();
    const qint64 pid = QCoreApplication::applicationPid();
    for (const auto &value : clients) {
        const QJsonObject c = value.toObject();
        if (c.value(QStringLiteral("pid")).toInteger() != pid || !c.value(QStringLiteral("mapped")).toBool()) continue;
        const QJsonObject ws = c.value(QStringLiteral("workspace")).toObject();
        const int id = ws.value(QStringLiteral("id")).toInt();
        const QString name = ws.value(QStringLiteral("name")).toString();
        // A normal workspace by its number; a special one by its name, which
        // says so; a named one has to be asked for as a name.
        const QString workspace = id > 0 ? QString::number(id)
                                  : name.isEmpty() || name.startsWith(QLatin1String("special:")) ? name
                                                                                                 : QStringLiteral("name:") + name;
        const QString address = c.value(QStringLiteral("address")).toString();
        if (address.isEmpty() || workspace.isEmpty() || workspace == QLatin1String(kAside)) continue;
        moveWindow(QLatin1String(kAside), address);
        m_parked.append({address, workspace});
    }
}

void RegionDrawer::restore() {
    int moved = 0, lost = 0;
    for (const Parked &p : std::as_const(m_parked)) {
        if (moveWindow(p.workspace, p.address)) continue;
        // Never left hidden: where it can be seen, if not where it was.
        if (moveWindow(QStringLiteral("e+0"), p.address)) ++moved;
        else ++lost;
    }
    if (lost) emit windowsMoved(tr("%n window(s) could not come back from Hyprland's hidden workspace %1.", nullptr, lost).arg(QLatin1String(kAside)));
    else if (moved) emit windowsMoved(tr("%n window(s) could not go back to their workspace and are on this one instead.", nullptr, moved));
    if (!m_parked.isEmpty())
        hyprctl({QStringLiteral("dispatch"),
                 QStringLiteral("hl.dsp.focus({ window = %1 })").arg(luaString(QStringLiteral("address:") + m_parked.first().address))});
    m_parked.clear();
}

void RegionDrawer::finished(int code, QProcess::ExitStatus status) {
    restore();
    const QStringList parts = QString::fromUtf8(m_slurp.readAllStandardOutput()).simplified().split(QLatin1Char(' '));
    if (status != QProcess::NormalExit || code != 0 || parts.size() != 4) {
        emit failed(m_sourceId, QString());  // Esc, or nothing drawn
        return;
    }
    const QRect onScreen(parts[0].toInt(), parts[1].toInt(), parts[2].toInt(), parts[3].toInt());
    const QRect area = findArea(hyprctl({QStringLiteral("monitors"), QStringLiteral("-j")}), m_display, onScreen, m_area);
    const QRect pixels = area.isEmpty() ? QRect() : toPixels(onScreen, m_display, area);
    if (pixels.width() < 2 || pixels.height() < 2) {
        emit failed(m_sourceId, tr("Draw the region on the display this source captures (%1 × %2 pixels).")
                                    .arg(m_display.width()).arg(m_display.height()));
        return;
    }
    emit drawn(m_sourceId, pixels);
}

QRect RegionDrawer::findArea(const QByteArray &monitorsJson, QSize display, QRect drawn, QRect portal) {
    QVector<QRect> fits;
    for (const auto &value : QJsonDocument::fromJson(monitorsJson).array()) {
        const QJsonObject m = value.toObject();
        int w = m.value(QStringLiteral("width")).toInt(), h = m.value(QStringLiteral("height")).toInt();
        // Turned a quarter: its picture is the other way round.
        if (m.value(QStringLiteral("transform")).toInt() % 2) std::swap(w, h);
        if (QSize(w, h) != display) continue;
        const double scale = m.value(QStringLiteral("scale")).toDouble(1.0);
        fits.append(QRect(m.value(QStringLiteral("x")).toInt(), m.value(QStringLiteral("y")).toInt(), int(std::lround(w / scale)),
                          int(std::lround(h / scale))));
    }
    // The one the portal named, then the one drawn on.
    if (!portal.isEmpty())
        for (const QRect &r : fits)
            if (r.topLeft() == portal.topLeft()) return r;
    for (const QRect &r : fits)
        if (r.contains(drawn.center())) return r;
    return fits.size() == 1 ? fits.first() : QRect();
}

QRect RegionDrawer::toPixels(QRect drawn, QSize display, QRect area) {
    const QRect inside = drawn.intersected(area);
    if (inside.isEmpty() || area.isEmpty()) return {};
    const double sx = double(display.width()) / area.width(), sy = double(display.height()) / area.height();
    const int left = std::clamp(int(std::lround((inside.x() - area.x()) * sx)), 0, display.width());
    const int top = std::clamp(int(std::lround((inside.y() - area.y()) * sy)), 0, display.height());
    const int right = std::clamp(int(std::lround((inside.x() + inside.width() - area.x()) * sx)), 0, display.width());
    const int bottom = std::clamp(int(std::lround((inside.y() + inside.height() - area.y()) * sy)), 0, display.height());
    return QRect(left, top, right - left, bottom - top);
}
