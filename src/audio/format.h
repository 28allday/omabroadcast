#pragma once
#include <array>

// Interleaved float PCM in the named speaker order. A channel count alone
// must never let a library guess a different layout (notably 2.1 and 4.0).
struct AudioFormat {
    int rate = 48000;
    int channels = 2;
    static int validRate(int value) { return value == 44100 ? 44100 : 48000; }
    static int validChannels(int value) {
        switch (value) { case 1: case 2: case 3: case 4: case 5: case 6: case 8: return value; default: return 2; }
    }
    AudioFormat normalized() const { return {validRate(rate), validChannels(channels)}; }
    bool operator==(const AudioFormat &) const = default;
    static const char *layoutName(int channels) {
        switch (channels) {
        case 1: return "mono"; case 3: return "2.1"; case 4: return "4.0";
        case 5: return "4.1"; case 6: return "5.1"; case 8: return "7.1";
        default: return "stereo";
        }
    }
    static std::array<const char *, 8> labels(int channels) {
        switch (channels) {
        case 1: return {"C"};
        case 3: return {"L", "R", "LFE"};
        case 4: return {"L", "R", "C", "BC"};
        case 5: return {"L", "R", "C", "LFE", "BC"};
        case 6: return {"L", "R", "C", "LFE", "BL", "BR"};
        case 8: return {"L", "R", "C", "LFE", "BL", "BR", "SL", "SR"};
        default: return {"L", "R"};
        }
    }
};
