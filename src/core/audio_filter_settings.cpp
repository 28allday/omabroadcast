#include "audio_filter_settings.h"
#include <QCoreApplication>
#include <QSet>
#include <QVariantMap>
#include <algorithm>
#include <cmath>

// Where each kind sits in the Add filter menu, and the menu's order of groups.
static std::pair<int, const char *> groupOf(AudioFilters::Type type) {
    switch (type) {
    case AudioFilters::Gain: case AudioFilters::Invert: return {0, QT_TRANSLATE_NOOP("AudioFilters", "Level")};
    case AudioFilters::Gate: case AudioFilters::Compressor: case AudioFilters::Limiter: case AudioFilters::Expander:
    case AudioFilters::Upward: return {1, QT_TRANSLATE_NOOP("AudioFilters", "Dynamics")};
    case AudioFilters::Equalizer: case AudioFilters::Deesser: return {2, QT_TRANSLATE_NOOP("AudioFilters", "Tone")};
    case AudioFilters::Spectral: case AudioFilters::Rnnoise: return {3, QT_TRANSLATE_NOOP("AudioFilters", "Noise")};
    default: return {4, QT_TRANSLATE_NOOP("AudioFilters", "Timing")};
    }
}

QVariantList AudioFilterSettings::catalog() {
    QVariantList list;
    const auto tr = [](const char *text) { return QCoreApplication::translate("AudioFilters", text); };
    for (const auto &d : AudioFilters::definitions()) {
        QVariantList parameters;
        for (const auto &p : d.parameters)
            parameters.append(QVariantMap{{"key", p.key}, {"label", tr(p.label)}, {"unit", p.unit},
                {"initial", p.initial}, {"minimum", p.minimum}, {"maximum", p.maximum}, {"step", p.step}});
        const auto group = groupOf(d.type);
        list.append(QVariantMap{{"type", d.key}, {"label", tr(d.label)}, {"description", tr(d.description)}, {"group", tr(group.second)},
                                {"groupOrder", group.first}, {"parameters", parameters}});
    }
    return list;
}

QVariantList AudioFilterSettings::normalized(const QVariantList &filters) {
    QVariantList list;
    QSet<QString> ids;
    for (const auto &value : filters) {
        if (list.size() == 16) break;
        const auto input = value.toMap();
        const auto type = input.value("type").toString();
        const auto &definitions = AudioFilters::definitions();
        const auto definition = std::find_if(definitions.begin(), definitions.end(), [&](const auto &d) { return type == QLatin1String(d.key); });
        if (definition == definitions.end()) continue;
        QString id = input.value("id").toString().left(128);
        if (id.isEmpty() || ids.contains(id)) {
            id = type + QString::number(list.size());
            while (ids.contains(id)) id += QLatin1Char('_');
        }
        ids.insert(id);
        QVariantMap item{{"id", id}, {"type", type}, {"enabled", input.value("enabled", true).toBool()}};
        for (const auto &p : definition->parameters) {
            bool valid = false;
            const double number = input.value(p.key, p.initial).toDouble(&valid);
            item.insert(p.key, valid && std::isfinite(number) ? std::clamp(number, p.minimum, p.maximum) : p.initial);
        }
        if (definition->type == AudioFilters::Gate) item["closeDb"] = std::min(item["openDb"].toDouble(), item["closeDb"].toDouble());
        list.append(item);
    }
    return list;
}

std::vector<AudioFilters::Config> AudioFilterSettings::parse(const QVariantList &filters) {
    std::vector<AudioFilters::Config> result;
    for (const auto &value : normalized(filters)) {
        const auto item = value.toMap();
        for (const auto &d : AudioFilters::definitions()) if (item.value("type").toString() == QLatin1String(d.key)) {
            auto config = AudioFilters::defaults(d.type, item.value("id").toString().toStdString());
            config.enabled = item.value("enabled").toBool();
            for (size_t i = 0; i < d.parameters.size(); ++i) config.values[i] = item.value(d.parameters[i].key).toDouble();
            result.push_back(config);
        }
    }
    return result;
}
