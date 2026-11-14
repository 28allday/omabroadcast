#pragma once

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

class QQmlEngine;
class QQuickWindow;
class QSettings;

// The card at a normal start: the name, the version, a quotation and OK. Nothing
// else starts until it is answered, so no share picker or first-run sheet opens
// over it. It shares the program's QML engine, as the theme's singleton belongs
// to one engine.
namespace LaunchScreen {

constexpr int Continue = -1;

QVariantList quotes();
// Each quotation once before any comes round again, and never the same twice running.
QVariantMap nextQuote(QSettings &settings);

// Null if the card cannot be made.
std::unique_ptr<QQuickWindow> create(QQmlEngine &engine, const QVariantMap &quote);
// Continue on OK, or when the card cannot be made: it must never be what stops
// the program opening. Otherwise the code to leave with.
int run(QQmlEngine &engine, const QString &stateDir);

}  // namespace LaunchScreen
