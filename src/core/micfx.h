#pragma once
// Microphone processing chain (portable, unit-tested on Linux): gain -> spectral noise suppression -> noise gate.
// Mono, 48 kHz float. Streaming: process() returns exactly as many samples as it is given.
#include <complex>
#include <cstddef>
#include <vector>

namespace cr {

struct MicFxParams {
    float gain = 1.0f;        // 0..2 (0-200 %)
    bool noiseSuppression = false;
    float gateDb = -42.0f;    // -80 (off) .. 0
};

class MicFx {
public:
    static constexpr int kN = 1024, kHop = 512;  // ~21 ms window, 50 % overlap (sqrt-Hann analysis + synthesis)
    MicFx();
    // In-place. Latency: kN samples when noise suppression is on (see delay()), 0 when off.
    void process(float* x, size_t n, const MicFxParams& p);
    int delay() const { return nsOn_ ? kN : 0; }
    void reset();
    float gateGain() const { return gateG_; }
private:
    void nsFrame();
    // noise suppression state
    bool nsOn_ = false;
    std::vector<float> win_, inBuf_, outAcc_, outFifo_;
    size_t inFill_ = 0, outRead_ = 0;
    std::vector<std::complex<float>> spec_;
    std::vector<float> noise_, gPrev_;
    int frames_ = 0;
    // gate state
    float env_ = 0, gateG_ = 1;
    int hold_ = 0;
};

void fft(std::complex<float>* a, int n, bool inverse);  // radix-2, n power of two

}  // namespace cr
