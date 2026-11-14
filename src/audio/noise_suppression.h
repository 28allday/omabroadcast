#pragma once
#include <memory>

// Stateful streaming processors. Deliberately move-only: the RNNoise state
// belongs to the library and must never be cloned as opaque bytes.
class NoiseSuppression {
public:
    enum Mode { Spectral, Speech };
    NoiseSuppression(Mode mode, int rate, int channels);
    ~NoiseSuppression();
    NoiseSuppression(const NoiseSuppression &) = delete;
    NoiseSuppression &operator=(const NoiseSuppression &) = delete;
    void configure(double thresholdDb, double reductionDb, double attackMs, double releaseMs);
    void process(float *frame); // one frame; no allocation
private:
    struct State;
    std::unique_ptr<State> m;
};
