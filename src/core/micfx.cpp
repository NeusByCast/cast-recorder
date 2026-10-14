#include "micfx.h"
#include <algorithm>
#include <cmath>

namespace cr {

void fft(std::complex<float>* a, int n, bool inv) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = 2 * 3.14159265358979323846 / len * (inv ? 1 : -1);
        std::complex<float> wl((float)std::cos(ang), (float)std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1);
            for (int k = 0; k < len / 2; ++k) {
                std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inv) for (int i = 0; i < n; ++i) a[i] /= (float)n;
}

MicFx::MicFx() {
    win_.resize(kN);
    for (int i = 0; i < kN; ++i) win_[i] = (float)std::sqrt(0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * i / kN));  // sqrt periodic Hann
    reset();
}

void MicFx::reset() {
    inBuf_.assign(kN, 0.f); outAcc_.assign(kN, 0.f);
    outFifo_.assign(kHop, 0.f);  // + kN-kHop zeros in inBuf_ = total latency kN: output k == input k - kN
    inFill_ = kN - kHop; outRead_ = 0;
    spec_.assign(kN, 0);
    noise_.assign(kN / 2 + 1, 0.f); gPrev_.assign(kN / 2 + 1, 1.f);
    frames_ = 0;
}

void MicFx::nsFrame() {
    for (int i = 0; i < kN; ++i) spec_[i] = inBuf_[i] * win_[i];
    fft(spec_.data(), kN, false);
    const int B = kN / 2 + 1;
    for (int k = 0; k < B; ++k) {
        float p = std::norm(spec_[k]) + 1e-12f;
        float& nk = noise_[k];
        if (frames_ < 8) nk = frames_ == 0 ? p : nk + (p - nk) / (float)(frames_ + 1);  // initial estimate
        else if (p < 4.f * nk) nk = 0.96f * nk + 0.04f * p;                            // noise-like bin: track its mean
        else nk *= 1.003f;                                                               // speech/tone: creep up slowly
        float g = 1.f - 1.6f * nk / p;                                                   // over-subtraction
        g = std::max(g, 0.12f);                                                          // ~-18 dB floor: no musical noise
        g = 0.55f * gPrev_[k] + 0.45f * g;
        gPrev_[k] = g;
        spec_[k] *= g;
        if (k > 0 && k < kN / 2) spec_[kN - k] = std::conj(spec_[k]);
    }
    fft(spec_.data(), kN, true);
    for (int i = 0; i < kN; ++i) outAcc_[i] += spec_[i].real() * win_[i];
    // first hop is final
    for (int i = 0; i < kHop; ++i) outFifo_.push_back(outAcc_[i]);
    std::copy(outAcc_.begin() + kHop, outAcc_.end(), outAcc_.begin());
    std::fill(outAcc_.begin() + (kN - kHop), outAcc_.end(), 0.f);
    std::copy(inBuf_.begin() + kHop, inBuf_.end(), inBuf_.begin());
    inFill_ = kN - kHop;
    ++frames_;
}

void MicFx::process(float* x, size_t n, const MicFxParams& p) {
    // gain
    if (p.gain != 1.f) for (size_t i = 0; i < n; ++i) x[i] *= p.gain;
    // noise suppression (streaming STFT, constant kN latency)
    if (p.noiseSuppression != nsOn_) { nsOn_ = p.noiseSuppression; reset(); }
    if (nsOn_) {
        for (size_t i = 0; i < n; ++i) {
            inBuf_[inFill_++] = x[i];
            if (inFill_ == (size_t)kN) nsFrame();
            x[i] = outFifo_[outRead_++];
        }
        if (outRead_ > 8192) { outFifo_.erase(outFifo_.begin(), outFifo_.begin() + (long)outRead_); outRead_ = 0; }
    }
    // noise gate: 10 ms RMS envelope, 150 ms hold, 2 ms attack / 80 ms release
    if (p.gateDb > -79.5f) {
        const float thr = std::pow(10.f, p.gateDb / 20.f);
        const float envA = 1.f - std::exp(-1.f / (0.010f * 48000.f));
        const float att = 1.f - std::exp(-1.f / (0.002f * 48000.f)), rel = 1.f - std::exp(-1.f / (0.080f * 48000.f));
        for (size_t i = 0; i < n; ++i) {
            env_ += envA * (x[i] * x[i] - env_);
            float rms = std::sqrt(std::max(env_, 0.f));
            if (rms >= thr) hold_ = (int)(0.150f * 48000);
            else if (hold_ > 0) --hold_;
            float target = hold_ > 0 ? 1.f : 0.f;
            gateG_ += (target > gateG_ ? att : rel) * (target - gateG_);
            x[i] *= gateG_;
        }
    } else gateG_ = 1.f;
}

}  // namespace cr
