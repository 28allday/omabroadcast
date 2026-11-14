#pragma once
#include "engine/frame.h"
#include <QStringList>
#include <QVariantList>
#include <memory>
#include <vector>

// A source's video filters as its settings keep them ("videoFilters": an
// ordered list of {id, type, enabled, parameters…}), the catalog the Filters
// sheet draws from, and the engine chain they become.
namespace VideoFilterSettings {
// Every kind: {type, label, description, async (only for sources that arrive
// on their own clock), presets [{label, values}], parameters [{key, label,
// kind: number|choice|colour|file|toggle, unit, initial, minimum, maximum,
// step, options [{value, label}], fileTypes, showIf {key: value}}]}.
QVariantList catalog();
// Known kinds only, at most 32, unique ids, every parameter present and in range.
QVariantList normalized(const QVariantList &filters);
// The enabled filters in order, with their files read (cached by path and
// time). A filter whose file cannot be used is left out and said so in
// `problems`; so is every filter with a file, without `loadFiles` (safe mode).
std::vector<engine::VideoFilter> parse(const QVariantList &filters, QStringList *problems = nullptr, bool loadFiles = true);
// Whether parse would read any file: a list with none costs no disk.
bool readsFiles(const QVariantList &filters);
// Whether a chain must keep something off air: Remove background, switched on.
bool cutsOut(const QVariantList &filters);
// .cube (1D or 3D) or a picture: a Hald or tiled square, or a strip.
std::shared_ptr<const engine::LookupTable> loadLookupTable(const QString &path, bool tiles, QString *error);
std::shared_ptr<const engine::FilterImage> loadImage(const QString &path, QString *error);
// A chroma key's backdrop as filmed, "#rrggbb", from a source's own picture:
// at (x, y) (0..1 of the picture) when given, else the commonest strong colour
// near the screen's hue ("green", "blue", "magenta", or "custom": near
// `current`). Empty, with `why`, when there is none.
QString findBackdrop(const QImage &picture, const QString &screen, const QString &current, double x, double y, QString *why);
}
