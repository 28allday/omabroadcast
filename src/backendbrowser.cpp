// Browser sources (a page drawn by the helper program) and custom browser
// docks (pages in the window's dock columns). Both need Qt WebEngine, which
// is optional: without it they are hidden.

#include "backend.h"

#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QUrl>

bool Backend::browserAvailable() const { return BrowserPage::available(); }

bool Backend::webDocksAvailable() {
    static const bool found = [] {
        if (qEnvironmentVariable("OMABROADCAST_NO_BROWSER") == QLatin1String("1")) return false;
        QStringList paths{QLibraryInfo::path(QLibraryInfo::QmlImportsPath)};
        paths += qEnvironmentVariable("QML_IMPORT_PATH").split(QLatin1Char(':'), Qt::SkipEmptyParts);
        paths += qEnvironmentVariable("QML2_IMPORT_PATH").split(QLatin1Char(':'), Qt::SkipEmptyParts);
        for (const QString &path : paths)
            if (QFileInfo::exists(path + QStringLiteral("/QtWebEngine/qmldir"))) return true;
        return false;
    }();
    return found;
}

void Backend::addBrowser(const QString &url) {
    // The canvas's size and frame rate: an overlay drawn for the whole picture.
    const int fps = std::clamp(int(std::lround(double(m_session.profile().fpsNum) / std::max(1, m_session.profile().fpsDen))), 1, 60);
    QVariantMap settings{{QStringLiteral("url"), url}, {QStringLiteral("width"), canvasWidth()},
                         {QStringLiteral("height"), canvasHeight()}, {QStringLiteral("fps"), fps},
                         {QStringLiteral("css"), QStringLiteral("body { background-color: rgba(0, 0, 0, 0); margin: 0; overflow: hidden; }")}};
    const QString item = m_production.addSource(QStringLiteral("browser"), nextName(tr("Browser")), settings, {});
    setSelectedItem(item);
}

void Backend::chooseBrowserFile(const QString &sourceId) {
    const quint64 generation = m_production.generation();
    m_otherChosen = [this, sourceId, generation](const QString &path) {
        const SourceDef *source = m_production.collection().source(sourceId);
        if (generation != m_production.generation() || !source || source->kind != QLatin1String("browser")) return;
        QVariantMap settings = source->settings;
        settings.insert(QStringLiteral("path"), path);
        settings.insert(QStringLiteral("local"), true);
        m_production.setSourceSettings(sourceId, settings);  // one step: the file, and showing it
    };
    m_otherPicker.openFile(tr("Show a page from a file"), tr("Web pages"),
                           {QStringLiteral("*.html"), QStringLiteral("*.htm"), QStringLiteral("*.svg"), QStringLiteral("*")});
}

QVariantMap Backend::browserStatus(const QString &sourceId) const {
    const SourceDef *source = m_production.collection().source(sourceId);
    if (!source || source->kind != QLatin1String("browser")) return {};
    QVariantMap status = m_session.browserStatus(sourceId);
    status.insert(QStringLiteral("name"), source->name);
    if (!status.contains(QStringLiteral("state")))
        for (const Session::FeedInfo &f : m_session.feeds()) if (f.sourceId == sourceId) status.insert(QStringLiteral("state"), f.state);
    return status;
}

void Backend::reloadBrowser(const QString &sourceId) {
    if (!m_session.reloadBrowser(sourceId)) setStatus(tr("The page is not running."));
}

QString Backend::browserAddress(const QString &typed) {
    const QString text = typed.trimmed();
    return text.isEmpty() ? QString() : QUrl::fromUserInput(text, QDir::homePath()).toString();
}

QString Backend::addBrowserDock(const QString &name, const QString &url) {
    QVariantList docks = browserDocks();
    int n = 1;
    auto taken = [&](const QString &id) {
        for (const QVariant &v : docks) if (v.toMap().value(QStringLiteral("id")).toString() == id) return true;
        return false;
    };
    while (taken(QStringLiteral("web-%1").arg(n))) ++n;
    const QString id = QStringLiteral("web-%1").arg(n);
    const QString title = name.trimmed().isEmpty() ? tr("Browser dock %1").arg(n) : name.trimmed();
    docks.append(QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("name"), title}, {QStringLiteral("url"), browserAddress(url)}});
    setPreference(QStringLiteral("browserDocks"), docks);
    return id;
}

bool Backend::setBrowserDock(const QString &id, const QString &name, const QString &url) {
    QVariantList docks = browserDocks();
    for (QVariant &v : docks) {
        QVariantMap dock = v.toMap();
        if (dock.value(QStringLiteral("id")).toString() != id) continue;
        if (!name.trimmed().isEmpty()) dock.insert(QStringLiteral("name"), name.trimmed());
        dock.insert(QStringLiteral("url"), browserAddress(url));
        v = dock;
        setPreference(QStringLiteral("browserDocks"), docks);
        return true;
    }
    return false;
}

void Backend::removeBrowserDock(const QString &id) {
    QVariantList docks = browserDocks();
    docks.removeIf([&](const QVariant &v) { return v.toMap().value(QStringLiteral("id")).toString() == id; });
    // Out of the columns too, and its sizes forgotten.
    QVariantMap layout = dockLayout();
    for (const QString side : {QStringLiteral("left"), QStringLiteral("right")}) {
        QVariantList column = layout.value(side).toList();
        column.removeAll(QVariant(id));
        if (layout.contains(side)) layout.insert(side, column);
    }
    QVariantMap sizes = dockSizes();
    sizes.remove(id);
    setPreference(QStringLiteral("docks"), layout);
    setPreference(QStringLiteral("dockSizes"), sizes);
    setPreference(QStringLiteral("browserDocks"), docks);
}
