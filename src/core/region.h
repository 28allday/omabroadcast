#pragma once

#include "scene.h"
#include <QRect>
#include <algorithm>
#include <cmath>
#include <limits>

// Coordinates are physical pixels in the captured display, independent of
// the item's placement on the production canvas. Zero extent means to edge.
struct CaptureRegion {
    int x = 0, y = 0, width = 0, height = 0;
    static int coordinate(double value) {
        return std::isfinite(value) ? int(std::clamp(std::round(value), 0., double(std::numeric_limits<int>::max()))) : 0;
    }
    static CaptureRegion fromSettings(const QVariantMap &settings) {
        return {coordinate(settings.value("regionX").toDouble()), coordinate(settings.value("regionY").toDouble()),
                coordinate(settings.value("regionWidth").toDouble()), coordinate(settings.value("regionHeight").toDouble())};
    }
    QRect bounds(int fullWidth, int fullHeight) const {
        if (fullWidth <= 0 || fullHeight <= 0) return {};
        const int left = std::clamp(x, 0, fullWidth - 1), top = std::clamp(y, 0, fullHeight - 1);
        return {left, top, width > 0 ? std::min(width, fullWidth - left) : fullWidth - left,
                           height > 0 ? std::min(height, fullHeight - top) : fullHeight - top};
    }
    engine::Layer layer(Transform transform, int fullWidth, int fullHeight) const {
        const auto area = bounds(fullWidth, fullHeight);
        if (area.isEmpty()) return {};
        // An instance's saved crop may outlive a smaller region/display.
        // Clamp it before remapping UVs so no surrounding pixels leak in.
        transform.cropLeft = std::clamp(transform.cropLeft, 0, area.width()-1);
        transform.cropRight = std::clamp(transform.cropRight, 0, area.width()-1-transform.cropLeft);
        transform.cropTop = std::clamp(transform.cropTop, 0, area.height()-1);
        transform.cropBottom = std::clamp(transform.cropBottom, 0, area.height()-1-transform.cropTop);
        auto result = itemLayer(transform, area.width(), area.height());
        result.u0 = float((area.x() + result.u0 * area.width()) / fullWidth);
        result.u1 = float((area.x() + result.u1 * area.width()) / fullWidth);
        result.v0 = float((area.y() + result.v0 * area.height()) / fullHeight);
        result.v1 = float((area.y() + result.v1 * area.height()) / fullHeight);
        return result;
    }
};
