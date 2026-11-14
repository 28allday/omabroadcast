#pragma once
#include <cstdint>
#include <vector>
struct AVFilterGraph;
struct AVFilterContext;
// Pitch-preserving tempo conversion of interleaved float PCM. Worker-owned.
class AudioTempo {
public:
    ~AudioTempo();
    bool open(int rate, int channels, double speed);
    void close();
    bool push(const float *samples, int frames); // nullptr flushes the tail
    int take(std::vector<float> &samples);
private:
    AVFilterGraph *m_graph = nullptr;
    AVFilterContext *m_input = nullptr, *m_output = nullptr;
    int m_rate = 48000, m_channels = 2;
    long long m_frames = 0;
};
