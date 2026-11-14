#include "noise_suppression.h"

#include <rnnoise.h>
extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
}
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>
#include <vector>

struct NoiseSuppression::State {
    static constexpr int size = 1024, hop = size / 4;
    using Complex = std::complex<float>;
    Mode mode;
    int channels, rate, at = 0, count = 0, block = 0;
    double threshold = 0, floor = 1, attack = 0, release = 0, windowSum = 0;
    std::array<float, size> window{};
    std::array<Complex, size / 2> twiddle{};
    std::array<float, size / 2 + 1> gains{}, target{};
    std::vector<std::vector<float>> input, output;
    std::vector<std::vector<Complex>> spectrum;
    struct Deleter { void operator()(DenoiseState *state) const { rnnoise_destroy(state); } };
    std::vector<std::unique_ptr<DenoiseState, Deleter>> speech;

    SwrContext *up = nullptr, *down = nullptr;
    int inputFrames = 0, inputAt = 0, speechAt = 0;
    size_t read = 0, write = 0;
    std::vector<float> pcm, raised, processed, lowered, queue;
    ~State() { swr_free(&up); swr_free(&down); }

    void resampledSpeech(float *frame) {
        const size_t capacity = queue.size() / channels;
        for (int c = 0; c < channels; ++c) {
            pcm[inputAt*channels+c] = std::clamp(frame[c], -1.f, 1.f);
            frame[c] = queue[(read % capacity)*channels+c];
        }
        ++read;
        if (++inputAt != inputFrames) return;
        inputAt = 0;
        const uint8_t *src = reinterpret_cast<const uint8_t *>(pcm.data());
        uint8_t *dst = reinterpret_cast<uint8_t *>(raised.data());
        const int n = swr_convert(up, &dst, 512, &src, inputFrames);
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < channels; ++c) input[c][speechAt] = raised[i*channels+c]*32768.f;
            if (++speechAt != block) continue;
            speechAt = 0;
            for (int c = 0; c < channels; ++c) {
                rnnoise_process_frame(speech[c].get(), output[c].data(), input[c].data());
                for (int f = 0; f < block; ++f) processed[f*channels+c] = output[c][f]/32768.f;
            }
            src = reinterpret_cast<const uint8_t *>(processed.data());
            dst = reinterpret_cast<uint8_t *>(lowered.data());
            const int count = swr_convert(down, &dst, 512, &src, block);
            for (int f = 0; f < count; ++f, ++write)
                for (int c = 0; c < channels; ++c) queue[(write % capacity)*channels+c] = lowered[f*channels+c];
        }
    }

    void fft(std::vector<Complex> &data, bool inverse) const {
        for (unsigned i = 1, j = 0; i < size; ++i) {
            unsigned bit = size / 2;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(data[i], data[j]);
        }
        for (int width = 2; width <= size; width *= 2)
            for (int base = 0; base < size; base += width)
                for (int k = 0; k < width / 2; ++k) {
                    const auto w = inverse ? std::conj(twiddle[k * size / width]) : twiddle[k * size / width];
                    const auto even = data[base + k], odd = data[base + k + width / 2] * w;
                    data[base + k] = even + odd;
                    data[base + k + width / 2] = even - odd;
                }
        if (inverse) for (auto &value : data) value /= size;
    }

    void spectralBlock() {
        for (int c = 0; c < channels; ++c) {
            auto &bins = spectrum[c];
            for (int i = 0; i < size; ++i) bins[i] = input[c][(at + i) % size] * window[i];
            fft(bins, false);
        }
        for (int k = 0; k <= size / 2; ++k) {
            double magnitude = 0;
            for (int c = 0; c < channels; ++c) magnitude = std::max(magnitude, double(std::abs(spectrum[c][k])));
            magnitude *= (k == 0 || k == size / 2 ? 1.0 : 2.0) / windowSum;
            // A soft transition over the 6 dB below the threshold avoids a
            // hard on/off decision on every bin. Channels share one mask.
            const double opening = std::clamp(2 * magnitude / threshold - 1, 0.0, 1.0);
            target[k] = float(floor + (1 - floor) * opening);
        }
        for (int k = 0; k <= size / 2; ++k) {
            const double desired = (target[std::max(0, k-1)] + 2 * target[k] + target[std::min(size/2, k+1)]) / 4.0;
            const double coefficient = desired > gains[k] ? attack : release;
            gains[k] = float(desired + coefficient * (gains[k] - desired));
        }
        for (int c = 0; c < channels; ++c) {
            auto &bins = spectrum[c];
            for (int k = 0; k < size; ++k) bins[k] *= gains[std::min(k, size-k)];
            fft(bins, true);
            // Four overlapping sqrt-Hann windows have sum-of-squares 2.
            for (int i = 0; i < size; ++i)
                output[c][(at + i) % size] += bins[i].real() * window[i] * 0.5f;
        }
    }
};

NoiseSuppression::NoiseSuppression(Mode mode, int rate, int channels) : m(std::make_unique<State>()) {
    m->mode = mode; m->rate = rate; m->channels = channels;
    if (mode == Speech) {
        if (rate != 48000 && rate != 44100) throw std::invalid_argument("Unsupported speech suppression sample rate");
        m->block = rnnoise_get_frame_size();
        if (m->block <= 0) throw std::runtime_error("RNNoise returned an invalid frame size");
        for (int c = 0; c < channels; ++c) {
            m->speech.emplace_back(rnnoise_create(nullptr));
            if (!m->speech.back()) throw std::bad_alloc();
        }
    } else {
        m->block = State::size;
        m->gains.fill(1.f);
        for (int i = 0; i < State::size; ++i) {
            m->window[i] = std::sin(std::numbers::pi * (i + 0.5) / State::size);
            m->windowSum += m->window[i];
        }
        for (int i = 0; i < State::size/2; ++i)
            m->twiddle[i] = std::polar(1.f, float(-2 * std::numbers::pi * i / State::size));
        m->spectrum.assign(channels, std::vector<State::Complex>(State::size));
    }
    m->input.assign(channels, std::vector<float>(m->block));
    m->output.assign(channels, std::vector<float>(m->block));
    if (mode == Speech && rate == 44100) {
        AVChannelLayout layout{}; av_channel_layout_default(&layout, channels);
        const int a = swr_alloc_set_opts2(&m->up, &layout, AV_SAMPLE_FMT_FLT, 48000, &layout, AV_SAMPLE_FMT_FLT, rate, 0, nullptr);
        const int b = swr_alloc_set_opts2(&m->down, &layout, AV_SAMPLE_FMT_FLT, rate, &layout, AV_SAMPLE_FMT_FLT, 48000, 0, nullptr);
        av_channel_layout_uninit(&layout);
        if (a < 0 || b < 0 || swr_init(m->up) < 0 || swr_init(m->down) < 0)
            throw std::runtime_error("Speech suppression resampler initialization failed");
        m->inputFrames = rate/100;
        m->pcm.resize(m->inputFrames*channels);
        m->raised.resize(512*channels); m->lowered.resize(512*channels);
        m->processed.resize(m->block*channels); m->queue.resize(4*m->inputFrames*channels);
        // Two input blocks plus a small resampler margin keep a continuous
        // output clock through both converters' initial lookahead.
        m->write = 2*m->inputFrames + 64;
    }
    configure(-50, 20, 5, 100);
}

NoiseSuppression::~NoiseSuppression() = default;

void NoiseSuppression::configure(double thresholdDb, double reductionDb, double attackMs, double releaseMs) {
    m->threshold = std::pow(10., thresholdDb / 20.);
    m->floor = std::pow(10., -reductionDb / 20.);
    const auto coefficient = [&](double ms) { return ms <= 0 ? 0. : std::exp(-1000. * State::hop / (ms * m->rate)); };
    m->attack = coefficient(attackMs); m->release = coefficient(releaseMs);
}

void NoiseSuppression::process(float *frame) {
    if (m->up) { m->resampledSpeech(frame); return; }
    if (m->mode == Speech) {
        // The public API uses float samples on the 16-bit PCM amplitude scale.
        // Block buffering adds one frame of latency, plus the model's own delay.
        for (int c = 0; c < m->channels; ++c) {
            m->input[c][m->at] = std::clamp(frame[c], -1.f, 1.f) * 32768.f;
            frame[c] = m->output[c][m->at] / 32768.f;
        }
        if (++m->at == m->block) {
            for (int c = 0; c < m->channels; ++c)
                rnnoise_process_frame(m->speech[c].get(), m->output[c].data(), m->input[c].data());
            m->at = 0;
        }
    } else {
        for (int c = 0; c < m->channels; ++c) {
            m->input[c][m->at] = frame[c];
            frame[c] = m->output[c][m->at];
            m->output[c][m->at] = 0;
        }
        m->at = (m->at + 1) % State::size;
        if (++m->count == State::hop) { m->count = 0; m->spectralBlock(); }
    }
}
