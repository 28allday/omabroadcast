#pragma once

#include "engine/frame.h"
#include <QVariantMap>
#include <algorithm>
#include <cstdint>

struct DeinterlaceSettings {
    engine::DeinterlaceMode mode = engine::DeinterlaceMode::Off;
    bool doubleRate = false;
    // -1 uses frame metadata, defaulting to top first when unknown.
    int firstField = -1;
    static DeinterlaceSettings fromSettings(const QVariantMap &settings) {
        DeinterlaceSettings out;
        auto name = settings.value("deinterlace", "off").toString();
        out.doubleRate = name.endsWith("2x");
        if (out.doubleRate) name.chop(2);
        if (name == "discard") out.mode = engine::DeinterlaceMode::Discard;
        else if (name == "linear") out.mode = engine::DeinterlaceMode::Linear;
        else if (name == "blend") out.mode = engine::DeinterlaceMode::Blend;
        else if (name == "adaptive") out.mode = engine::DeinterlaceMode::Adaptive;
        else out.doubleRate = false;
        const auto order = settings.value("fieldOrder", "auto").toString();
        out.firstField = order == "top" ? 0 : order == "bottom" ? 1 : -1;
        return out;
    }
};

// The second field becomes due halfway through this input frame, then holds.
// It never alternates repeatedly if a camera stalls or a file ends. Time is
// the same monotonic timeline as source pictures and the audio mixer.
struct VideoFieldClock {
    int64_t startNs = 0, durationNs = 33'333'333;
    int metadataFirst = -1;
    bool valid = false;
    bool push(int64_t time, int64_t duration, int first) {
        const auto delta = time - startNs;
        const bool reset = valid && (first != metadataFirst || delta <= 0 || delta > std::max<int64_t>(durationNs * 4, 250'000'000));
        if (duration > 0) durationNs = std::clamp<int64_t>(duration, 1'000'000, 10'000'000'000ll);
        else if (valid && !reset && delta > 0) durationNs = delta;
        startNs = time; metadataFirst = first; valid = true;
        return reset;
    }
    int first(const DeinterlaceSettings &settings) const {
        return settings.firstField >= 0 ? settings.firstField : metadataFirst >= 0 ? metadataFirst : 0;
    }
    bool second(const DeinterlaceSettings &settings, int64_t now) const {
        return valid && settings.doubleRate && now - startNs >= durationNs / 2;
    }
    int field(const DeinterlaceSettings &settings, int64_t now) const {
        return first(settings) ^ int(second(settings, now));
    }
};
