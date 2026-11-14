#pragma once
#include "format.h"
#include <spa/param/audio/format-utils.h>
#include <string_view>

inline spa_audio_info_raw pipeWireFormat(AudioFormat format) {
    format = format.normalized();
    spa_audio_info_raw result{};
    result.format = SPA_AUDIO_FORMAT_F32;
    result.rate = format.rate; result.channels = format.channels;
    const auto names = AudioFormat::labels(format.channels);
    for (int c = 0; c < format.channels; ++c) {
        const std::string_view name = names[c];
        result.position[c] = name == "L" ? SPA_AUDIO_CHANNEL_FL : name == "R" ? SPA_AUDIO_CHANNEL_FR
            : name == "C" ? SPA_AUDIO_CHANNEL_FC : name == "LFE" ? SPA_AUDIO_CHANNEL_LFE
            : name == "BC" ? SPA_AUDIO_CHANNEL_RC : name == "BL" ? SPA_AUDIO_CHANNEL_RL
            : name == "BR" ? SPA_AUDIO_CHANNEL_RR : name == "SL" ? SPA_AUDIO_CHANNEL_SL : SPA_AUDIO_CHANNEL_SR;
    }
    // MONO tells PipeWire to duplicate a mono microphone when monitoring on
    // stereo hardware; FC is the centre speaker of a surround stream.
    if (format.channels == 1) result.position[0] = SPA_AUDIO_CHANNEL_MONO;
    return result;
}
