#pragma once

#include "audio/mixer.h"
#include "audio_filter_settings.h"
#include <QVariantMap>
#include <QKeySequence>
#include <algorithm>
#include <cmath>

// Shared normalization for persisted profiles, source settings and UI edits.
namespace AudioSettings {
inline double gain(double value) { return std::isfinite(value) ? std::clamp(value, 0.0, 20.0) : 1.0; }
inline AudioMixer::Processing processing(const QVariantMap &settings) {
    AudioMixer::Processing p;
    p.mono = settings.value(QStringLiteral("mono"), false).toBool();
    const double balance = settings.value(QStringLiteral("balance"), 0.0).toDouble();
    p.balance = std::isfinite(balance) ? float(std::clamp(balance, -1.0, 1.0)) : 0.f;
    const double sync = settings.value(QStringLiteral("syncMs"), 0).toDouble();
    p.syncMs = std::isfinite(sync) ? int(std::round(std::clamp(sync, -1000.0, 2000.0))) : 0;
    return p;
}
inline PushControl::Settings push(const QVariantMap &settings) {
    return {settings.value("pushToTalk").toBool(), settings.value("pushToMute").toBool(),
            std::clamp(settings.value("talkDelayMs").toInt(), 0, 5000), std::clamp(settings.value("muteDelayMs").toInt(), 0, 5000)};
}
inline QString shortcut(const QVariant &value) {
    const auto key = QKeySequence::fromString(value.toString(), QKeySequence::PortableText);
    if (key.count() != 1 || key[0].key() == Qt::Key_unknown) return {};
    return key.toString(QKeySequence::PortableText);
}
inline bool isKey(const QString &key) {
    return key == QLatin1String("mixerHidden") || key == QLatin1String("mono") || key == QLatin1String("balance") || key == QLatin1String("syncMs") ||
           key == QLatin1String("pushToTalk") || key == QLatin1String("pushToMute") ||
           key == QLatin1String("talkDelayMs") || key == QLatin1String("muteDelayMs") ||
           key == QLatin1String("talkShortcut") || key == QLatin1String("muteShortcut") ||
           key == QLatin1String("monitor") || key == QLatin1String("solo") || key == QLatin1String("tracks") || key == QLatin1String("audioFilters");
}
inline AudioMixer::Routing routing(const QVariantMap &settings) {
    AudioMixer::Routing r;
    r.monitoring = static_cast<AudioMixer::Monitoring>(std::clamp(settings.value(QStringLiteral("monitor"), 0).toInt(), 0, 2));
    r.solo = settings.value(QStringLiteral("solo"), false).toBool();
    r.tracks = unsigned(settings.value(QStringLiteral("tracks"), 1).toInt()) & 63;
    return r;
}
inline QVariantMap normalized(const QVariantMap &settings) {
    const auto p = processing(settings);
    const auto r = routing(settings);
    const auto held = push(settings);
    return {{"mixerHidden", settings.value("mixerHidden", false).toBool()}, {"pushToTalk", held.talk}, {"pushToMute", held.mute},
            {"talkDelayMs", held.talkDelayMs}, {"muteDelayMs", held.muteDelayMs},
            {"talkShortcut", shortcut(settings.value("talkShortcut"))}, {"muteShortcut", shortcut(settings.value("muteShortcut"))},
            {QStringLiteral("mono"), p.mono}, {QStringLiteral("balance"), p.balance},
            {QStringLiteral("syncMs"), p.syncMs}, {QStringLiteral("monitor"), int(r.monitoring)},
            {QStringLiteral("solo"), r.solo}, {QStringLiteral("tracks"), r.tracks},
            {QStringLiteral("audioFilters"), AudioFilterSettings::normalized(settings.value(QStringLiteral("audioFilters")).toList())}};
}
}
