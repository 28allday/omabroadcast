#include "launch_screen.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QSettings>
#include <QStringList>
#include <QUrl>

namespace LaunchScreen {

QVariantList quotes() {
    QFile file(QStringLiteral(":/launch/quotes.json"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).array().toVariantList();
}

QVariantMap nextQuote(QSettings &settings) {
    const QVariantList entries = quotes();
    QStringList ids;
    for (const QVariant &entry : entries) ids << entry.toMap().value(QStringLiteral("id")).toString();
    if (ids.isEmpty()) return {};
    const QString last = settings.value(QStringLiteral("lastQuote")).toString();
    QStringList remaining = settings.value(QStringLiteral("remainingQuotes")).toStringList();
    remaining.removeDuplicates();
    remaining.removeIf([&](const QString &id) { return !ids.contains(id); });
    if (remaining.isEmpty()) remaining = ids;
    QStringList choices = remaining;
    if (choices.size() > 1) choices.removeAll(last);
    const QString chosen = choices.at(QRandomGenerator::global()->bounded(int(choices.size())));
    remaining.removeAll(chosen);
    settings.setValue(QStringLiteral("remainingQuotes"), remaining);
    settings.setValue(QStringLiteral("lastQuote"), chosen);
    settings.sync();
    for (const QVariant &entry : entries)
        if (entry.toMap().value(QStringLiteral("id")).toString() == chosen) return entry.toMap();
    return {};
}

std::unique_ptr<QQuickWindow> create(QQmlEngine &engine, const QVariantMap &quote) {
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/LaunchWindow.qml")));
    std::unique_ptr<QObject> object(component.createWithInitialProperties(
        {{QStringLiteral("version"), QCoreApplication::applicationVersion()}, {QStringLiteral("quote"), quote}}));
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    if (!window) {
        qWarning("Could not show the launch window: %s", qPrintable(component.errorString()));
        return {};
    }
    object.release();
    return std::unique_ptr<QQuickWindow>(window);
}

int run(QQmlEngine &engine, const QString &stateDir) {
    // Its closing is not the last window closing: the program's is yet to open.
    const bool quitOnClose = QGuiApplication::quitOnLastWindowClosed();
    QGuiApplication::setQuitOnLastWindowClosed(false);
    const auto restore = qScopeGuard([quitOnClose] { QGuiApplication::setQuitOnLastWindowClosed(quitOnClose); });

    QSettings settings(stateDir + QStringLiteral("/launch.ini"), QSettings::IniFormat);
    const std::unique_ptr<QQuickWindow> window = create(engine, nextQuote(settings));
    if (!window) return Continue;
    QEventLoop loop;
    QObject::connect(window.get(), &QWindow::visibleChanged, &loop, [&loop](bool visible) {
        if (!visible) loop.quit();
    });
    window->setVisible(true);
    // A quit signal ends this loop with its own code, and the card unanswered.
    const int result = loop.exec();
    return window->property("confirmed").toBool() ? Continue : result;
}

}  // namespace LaunchScreen
