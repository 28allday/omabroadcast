#pragma once
#include <QVariantMap>
#include <QRegularExpression>
#include <algorithm>

namespace MeterSettings {
inline QVariantMap normalized(const QVariantMap &input) {
    QVariantMap output{{"truePeak", input.value("truePeak", false).toBool()},
                       {"vertical", input.value("vertical", false).toBool()},
                       {"decay", std::clamp(input.value("decay", 1).toInt(), 0, 2)}};
    static const QRegularExpression colour(QStringLiteral("^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$"));
    for (const auto &key : {"low", "mid", "high"}) {
        const QString value = input.value(key).toString();
        output.insert(key, colour.match(value).hasMatch() ? value.toLower() : QString());
    }
    return output;
}
inline double decayRate(const QVariantMap &settings) {
    constexpr double rates[]{24, 12, 6};
    return rates[std::clamp(settings.value("decay", 1).toInt(), 0, 2)];
}
}
