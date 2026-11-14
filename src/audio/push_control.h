#pragma once
#include <algorithm>
#include <cstdint>

// Release delays advance on the mixer's sample clock. Held state is transient;
// different input origins (mouse, local key, portal) may overlap independently.
class PushControl {
public:
    struct Settings {
        bool talk = false, mute = false;
        int talkDelayMs = 0, muteDelayMs = 0;
        bool operator==(const Settings &) const = default;
    };
    void configure(Settings settings) {
        settings.talkDelayMs = std::clamp(settings.talkDelayMs, 0, 5000);
        settings.muteDelayMs = std::clamp(settings.muteDelayMs, 0, 5000);
        if (settings == m_settings) return;
        m_settings = settings; cancel();
    }
    void hold(bool talk, bool down, unsigned origin, int64_t cursor, int rate) {
        if (!(talk ? m_settings.talk : m_settings.mute)) return;
        auto &state = talk ? m_talk : m_mute;
        const bool was = state.held != 0;
        if (down) state.held |= origin; else state.held &= ~origin;
        if (down) { state.until = -1; state.delayOrigin = 0; }
        else if (was && !state.held) {
            state.until = cursor + int64_t(talk ? m_settings.talkDelayMs : m_settings.muteDelayMs)*rate/1000;
            state.delayOrigin = origin;
        }
    }
    void cancel(unsigned origins = ~0u) {
        for (auto *state : {&m_talk, &m_mute}) {
            state->held &= ~origins;
            if (state->delayOrigin & origins) { state->until = -1; state->delayOrigin = 0; }
        }
    }
    bool audible(int64_t frame) const {
        return (!m_settings.talk || m_talk.held || frame < m_talk.until)
            && !(m_settings.mute && (m_mute.held || frame < m_mute.until));
    }
    bool held(bool talk) const { return (talk ? m_talk : m_mute).held != 0; }
private:
    struct State { unsigned held = 0, delayOrigin = 0; int64_t until = -1; };
    Settings m_settings;
    State m_talk, m_mute;
};
