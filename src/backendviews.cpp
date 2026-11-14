#include "backend.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QScreen>

// Pictures for other windows: projectors, multiview tiles, screenshots.

void Backend::requestView(const QString &id, const QString &shows, const QString &target, int width, int height, int fps) {
    const Session::View view{id, shows, target, std::max(2, width), std::max(2, height), std::clamp(fps, 1, 60)};
    const auto it = m_views.find(id);
    if (it != m_views.end() && it->second == view) return;
    m_views[id] = view;
    m_viewsChanged.start();
}

void Backend::releaseView(const QString &id) {
    if (m_views.erase(id)) m_viewsChanged.start();
}

QVariantList Backend::screens() const {
    QVariantList out;
    const auto all = QGuiApplication::screens();
    for (int i = 0; i < all.size(); ++i)
        out.append(QVariantMap{{"index", i}, {"name", all[i]->name()},
                               {"width", all[i]->geometry().width()}, {"height", all[i]->geometry().height()}});
    return out;
}

QString Backend::projectorLabel(const QVariantMap &p) const {
    const QString shows = p.value("shows").toString(), target = p.value("target").toString();
    const Collection &c = m_production.collection();
    if (shows == QLatin1String("preview")) return tr("Preview");
    if (shows == QLatin1String("multiview")) return tr("Multiview");
    if (shows == QLatin1String("studio")) {
        const Scene *s = c.scene(target);
        return s ? tr("Studio: %1").arg(s->name) : QString();
    }
    if (shows == QLatin1String("source")) {
        const SourceDef *s = c.source(target);
        return s ? tr("Source: %1").arg(s->name) : QString();
    }
    return tr("Program");
}

// One whose studio or source is not in this collection waits for it: ids start
// again in each, so it says which it was opened in.
bool Backend::projectorShown(const QVariantMap &p) const {
    return (!p.contains("collection") || p.value("collection").toString() == m_session.profile().collection) && !projectorLabel(p).isEmpty();
}

QVariantList Backend::projectors() const {
    QVariantList out;
    for (const QVariant &v : preference(QStringLiteral("projectors"), QVariantList()).toList()) {
        QVariantMap p = v.toMap();
        if (!projectorShown(p)) continue;
        p.insert("label", projectorLabel(p));
        out.append(p);
    }
    return out;
}

// One that would wait for ever. As a collection loads there is no undo left to
// bring back what has gone from it, and ids are not given twice.
void Backend::dropGoneProjectors() {
    QVariantList all = preference(QStringLiteral("projectors"), QVariantList()).toList();
    const auto before = all.size();
    all.removeIf([this](const QVariant &v) {
        const QVariantMap p = v.toMap();
        const QString collection = p.value("collection").toString();
        if (collection.isEmpty()) return false;
        return collection == m_session.profile().collection ? projectorLabel(p).isEmpty() : !QFile::exists(collectionFile(collection));
    });
    if (all.size() == before) return;
    setPreference(QStringLiteral("projectors"), all);
    m_projectorsSeen = projectors();
    emit projectorsChanged();
}

QString Backend::openProjector(const QString &shows, const QString &target, int screen) {
    static const QStringList kinds{"program", "preview", "studio", "source", "multiview"};
    if (!kinds.contains(shows)) return {};
    QVariantList all = preference(QStringLiteral("projectors"), QVariantList()).toList();
    // A screen takes one fullscreen projector: the earlier one there closes.
    if (screen >= 0)
        all.removeIf([&](const QVariant &v) { return v.toMap().value("fullscreen").toBool() && v.toMap().value("screen").toInt() == screen; });
    // Every projector on screen is a window, reopened at each start: a bound on them.
    const auto open = std::count_if(all.cbegin(), all.cend(), [this](const QVariant &v) { return projectorShown(v.toMap()); });
    if (open >= 16) {
        setStatus(tr("Close a projector first: %1 are open.").arg(open));
        return {};
    }
    // Ids by the time, made unique should two come in the same millisecond.
    QString id = QStringLiteral("projector-%1").arg(QDateTime::currentMSecsSinceEpoch(), 0, 36);
    const auto taken = [&all](const QString &candidate) {
        return std::any_of(all.cbegin(), all.cend(), [&](const QVariant &v) { return v.toMap().value("id").toString() == candidate; });
    };
    for (int n = 2; taken(id); ++n) id = QStringLiteral("projector-%1-%2").arg(QDateTime::currentMSecsSinceEpoch(), 0, 36).arg(n);
    QVariantMap opened{{"id", id}, {"shows", shows}, {"target", target}, {"screen", screen},
                       {"fullscreen", screen >= 0}, {"onTop", false}, {"hideCursor", screen >= 0}};
    if (shows == QLatin1String("studio") || shows == QLatin1String("source")) opened.insert("collection", m_session.profile().collection);
    all.append(opened);
    setPreference(QStringLiteral("projectors"), all);
    m_projectorsSeen = projectors();
    emit projectorsChanged();
    return id;
}

void Backend::setProjectorOption(const QString &id, const QString &key, const QVariant &value) {
    static const QStringList keys{"screen", "fullscreen", "onTop", "hideCursor", "x", "y", "width", "height"};
    if (!keys.contains(key)) return;
    QVariantList all = preference(QStringLiteral("projectors"), QVariantList()).toList();
    bool changed = false;
    for (QVariant &v : all) {
        QVariantMap p = v.toMap();
        if (p.value("id").toString() != id || p.value(key) == value) continue;
        p.insert(key, value);
        v = p;
        changed = true;
    }
    if (!changed) return;
    setPreference(QStringLiteral("projectors"), all);
    // Window size and place are remembered quietly; the rest reopens the window.
    if (key != QLatin1String("x") && key != QLatin1String("y") && key != QLatin1String("width") && key != QLatin1String("height")) {
        applyProjectorRules();
        m_projectorsSeen = projectors();
        emit projectorsChanged();
    }
}

void Backend::closeProjector(const QString &id) {
    QVariantList all = preference(QStringLiteral("projectors"), QVariantList()).toList();
    const auto before = all.size();
    all.removeIf([&](const QVariant &v) { return v.toMap().value("id").toString() == id; });
    if (all.size() == before) return;
    setPreference(QStringLiteral("projectors"), all);
    applyProjectorRules();
    m_projectorsSeen = projectors();
    emit projectorsChanged();
}

void Backend::applyProjectorRules() {
    // The rule only touches windows whose title asks for it, so once made it
    // can stay; it is made when a projector first wants it.
    for (const QVariant &v : preference(QStringLiteral("projectors"), QVariantList()).toList())
        if (v.toMap().value("onTop").toBool() && !v.toMap().value("fullscreen").toBool()) {
            if (!m_hyprland.setProjectorsOnTop(true) && HyprlandRules::available()) setStatus(tr("Hyprland did not accept the projector rule."));
            return;
        }
}

void Backend::screenshot(const QString &shows, const QString &target) {
    const QString id = QStringLiteral("screenshot-%1").arg(++m_screenshotSerial);
    m_screenshots.insert(id, shows);
    m_session.requestSnapshot(id, shows, target);
}
