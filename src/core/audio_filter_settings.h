#pragma once
#include "audio/filters.h"
#include <QVariantList>

namespace AudioFilterSettings {
QVariantList catalog();
QVariantList normalized(const QVariantList &filters);
std::vector<AudioFilters::Config> parse(const QVariantList &filters);
}
