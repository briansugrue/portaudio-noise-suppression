// wiener_vad.h
//
// Real-time Wiener filter with a VAD-gated noise estimate.
// C++ port of wienerFilter.m. The VAD follows vad_vad() from
// "Real-Time Digital Signal Processing" (3rd ed.) by Kuo, Lee and Tian.
//
// FFT: KissFFT real transforms (kiss_fftr / kiss_fftri, float build).
// The FFT plans are allocated once in the constructor, and the transforms
// use only that preallocated memory, so process() never allocates, locks
// or prints and is safe to call from the PortAudio callback.
// Call process() once per audio block of exactly `hop` samples.
//
// Algorithm, per hop (frame = 2 * hop samples, 50% overlap):
//   sqrt-Hann window -> real FFT -> VAD on 250-820 Hz magnitude sum
//   -> noise PSD update on noise frames only
//   -> Wiener gain with decision-directed a priori SNR
//   -> inverse real FFT -> sqrt-Hann window -> overlap-add.
// sqrt-Hann analysis + synthesis at 50% overlap sums to exactly 1, so no
// window-sum normalisation is needed.
//
// Algorithmic latency: one hop (frame length minus hop).

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include <kiss_fftr.h>

static_assert(std::is_same<kiss_fft_scalar, float>::value,
    "wiener_vad.h needs KissFFT built with float (KISSFFT_DATATYPE=float)");

// All time constants are in seconds, so behaviour does not depend on the
// sample rate. Defaults match the MATLAB version tuned at 8 kHz / 32 ms hop.
struct WienerVadParams {
    double calibrationSec = 0.5;    // noise-only time at start (stay quiet)
    double energyTau = 0.14;   // smoothing of Em            (alpham)
    double floorFallTau = 0.046;  // noise floor falling to Em  (alphal)
    double floorRiseTau = 3.0;    // noise floor rising         (alphan)
    double marginRatio = 1.0;    // margin = marginRatio * initial floor
    double hangoverSec = 0.25;   // VAD hangover               (HOV)
    double noiseTau = 0.30;   // noise PSD averaging on noise frames
    float  minGain = 0.1f;   // gain floor (about -20 dB)
    double ddAlpha = 0.95;   // decision-directed smoothing (0.9 - 0.98)
    double vadLowHz = 250.0;  // VAD band
    double vadHighHz = 820.0;
};

class WienerVad {
public:
    // hop must be at least 1; the frame length 2 * hop is even, as KissFFT's
    // real transforms require.
    WienerVad(double sampleRate, std::size_t hop = 256,
        const WienerVadParams& params = WienerVadParams())
        : hop_(hop), nfft_(2 * hop), nBins_(hop + 1), p_(params)
    {
        if (sampleRate <= 0.0)
            throw std::invalid_argument("WienerVad: bad sample rate");
        if (hop_ < 1)
            throw std::invalid_argument("WienerVad: hop must be positive");

        // FFT plans (allocated once; no allocation during transforms)
        fwd_ = kiss_fftr_alloc(static_cast<int>(nfft_), 0, nullptr, nullptr);
        inv_ = kiss_fftr_alloc(static_cast<int>(nfft_), 1, nullptr, nullptr);
        if (!fwd_ || !inv_) {
            releasePlans();
            throw std::runtime_error("WienerVad: KissFFT plan allocation failed");
        }

        // Per-hop coefficients from time constants
        const double T = static_cast<double>(hop_) / sampleRate;
        auto coef = [T](double tau) { return static_cast<float>(1.0 - std::exp(-T / tau)); };
        aEm_ = coef(p_.energyTau);
        aFall_ = coef(p_.floorFallTau);
        aRise_ = coef(p_.floorRiseTau);
        aNoise_ = coef(p_.noiseTau);
        hangoverFrames_ = std::max(1, static_cast<int>(std::lround(p_.hangoverSec / T)));
        calibFrames_ = std::max(3, static_cast<int>(std::lround(p_.calibrationSec / T)));

        // VAD bins (0-based): 250 Hz .. 820 Hz
        ss_ = static_cast<std::size_t>(std::ceil(p_.vadLowHz * nfft_ / sampleRate));
        ee_ = static_cast<std::size_t>(std::floor(p_.vadHighHz * nfft_ / sampleRate));
        ee_ = std::min(ee_, nfft_ / 2);
        if (ss_ < 1) ss_ = 1;
        if (ee_ < ss_) ee_ = ss_;

        inBuf_.assign(nfft_, 0.0f);
        outAcc_.assign(nfft_, 0.0f);
        outBlock_.assign(hop_, 0.0f);
        win_.resize(nfft_);
        timeBuf_.assign(nfft_, 0.0f);
        spec_.assign(nBins_, kiss_fft_cpx{ 0.0f, 0.0f });
        psd_.assign(nBins_, 0.0f);
        noisePSD_.assign(nBins_, 0.0f);
        noiseSum_.assign(nBins_, 0.0f);
        gain_.assign(nBins_, 1.0f);
        prevClean_.assign(nBins_, 0.0f);

        const double pi = 3.14159265358979323846;
        for (std::size_t n = 0; n < nfft_; ++n) {
            const double hann = 0.5 * (1.0 - std::cos(2.0 * pi * n / nfft_));  // periodic
            win_[n] = static_cast<float>(std::sqrt(hann));
        }

        startCalibration();
    }

    ~WienerVad() { releasePlans(); }

    WienerVad(const WienerVad&) = delete;
    WienerVad& operator=(const WienerVad&) = delete;

    // Processes exactly `hop` samples. Returns a pointer to `hop` enhanced
    // samples (valid until the next call). If n != hop, the input is
    // returned unchanged.
    // Call from one thread only (the audio callback).
    const float* process(const float* in, std::size_t n) {
        if (n != hop_) return in;

        if (recalRequested_.load(std::memory_order_relaxed) &&
            recalRequested_.exchange(false))
            startCalibration();

        // Slide the input buffer and append the new block
        std::copy(inBuf_.begin() + hop_, inBuf_.end(), inBuf_.begin());
        std::copy(in, in + hop_, inBuf_.end() - hop_);

        processFrame();

        // The first `hop` samples of the accumulator are now complete
        std::copy(outAcc_.begin(), outAcc_.begin() + hop_, outBlock_.begin());
        std::copy(outAcc_.begin() + hop_, outAcc_.end(), outAcc_.begin());
        std::fill(outAcc_.end() - hop_, outAcc_.end(), 0.0f);
        return outBlock_.data();
    }

    // Safe to call from another thread: re-learns the noise at the next block.
    void recalibrate() { recalRequested_.store(true); }

    bool isCalibrating() const { return calibrating_.load(); }
    bool speechDetected() const { return vad_.load(); }
    std::size_t hopSize() const { return hop_; }

private:
    void releasePlans() {
        if (fwd_) { kiss_fftr_free(fwd_); fwd_ = nullptr; }
        if (inv_) { kiss_fftr_free(inv_); inv_ = nullptr; }
    }

    void startCalibration() {
        calibrating_.store(true);
        calibCount_ = 0;
        // The first frames still contain the zero-filled start of the buffer
        warmupFrames_ = static_cast<int>(nfft_ / hop_) - 1;
        std::fill(noiseSum_.begin(), noiseSum_.end(), 0.0f);
        std::fill(prevClean_.begin(), prevClean_.end(), 0.0f);
        enSum_ = 0.0;
        std::fill(outAcc_.begin(), outAcc_.end(), 0.0f);
        hov_ = 0;
    }

    void processFrame() {
        // Window, then real FFT: nfft real samples -> nfft/2 + 1 complex bins
        for (std::size_t k = 0; k < nfft_; ++k)
            timeBuf_[k] = inBuf_[k] * win_[k];
        kiss_fftr(fwd_, timeBuf_.data(), spec_.data());

        for (std::size_t k = 0; k < nBins_; ++k)
            psd_[k] = spec_[k].r * spec_[k].r + spec_[k].i * spec_[k].i;

        // VAD "energy": sum of magnitudes over the VAD band
        float En = 0.0f;
        for (std::size_t k = ss_; k <= ee_; ++k) En += std::sqrt(psd_[k]);

        // ---- Calibration: learn the noise, output silence ----
        if (calibrating_.load()) {
            if (warmupFrames_ > 0) { --warmupFrames_; return; }
            for (std::size_t k = 0; k < nBins_; ++k) noiseSum_[k] += psd_[k];
            enSum_ += En;
            if (++calibCount_ >= calibFrames_) {
                const float inv = 1.0f / static_cast<float>(calibCount_);
                for (std::size_t k = 0; k < nBins_; ++k)
                    noisePSD_[k] = std::max(noiseSum_[k] * inv, kMinPSD);
                Nf_ = static_cast<float>(enSum_ / calibCount_);
                Em_ = Nf_;
                margin_ = static_cast<float>(p_.marginRatio) * Nf_;
                hov_ = 0;
                calibrating_.store(false);
            }
            return;
        }

        // ---- VAD (port of vad_vad) ----
        Em_ += aEm_ * (En - Em_);                       // Em = am1*Em + alpham*En
        if (Nf_ > Em_) Nf_ += aFall_ * (En - Nf_);      // fall quickly
        else           Nf_ += aRise_ * (En - Nf_);      // rise slowly
        const float thres = Nf_ + margin_;

        bool vad = (Em_ >= thres);
        if (vad) {
            hov_ = hangoverFrames_;
        }
        else {
            const int old = hov_;                       // C: if (hov-- <= 0)
            --hov_;
            if (old <= 0) hov_ = 0;
            else          vad = true;                   // still in hangover
        }
        vad_.store(vad, std::memory_order_relaxed);

        // ---- Noise PSD update on noise frames only ----
        if (!vad) {
            for (std::size_t k = 0; k < nBins_; ++k)
                noisePSD_[k] += aNoise_ * (psd_[k] - noisePSD_[k]);
        }

        // ---- Wiener gain with decision-directed a priori SNR ----
        const float a = static_cast<float>(p_.ddAlpha);
        for (std::size_t k = 0; k < nBins_; ++k) {
            const float Pn = std::max(noisePSD_[k], kMinPSD);
            const float post = psd_[k] / Pn;                     // a posteriori SNR
            const float prio = a * (prevClean_[k] / Pn)          // previous frame's estimate
                + (1.0f - a) * std::max(post - 1.0f, 0.0f);
            gain_[k] = std::max(prio / (prio + 1.0f), p_.minGain);
            prevClean_[k] = gain_[k] * gain_[k] * psd_[k];       // for the next frame
        }

        // Apply the gain to the half spectrum; the inverse real FFT supplies
        // the mirrored negative frequencies, so the output stays real.
        for (std::size_t k = 0; k < nBins_; ++k) {
            spec_[k].r *= gain_[k];
            spec_[k].i *= gain_[k];
        }

        // KissFFT's inverse is unscaled: divide by nfft.
        kiss_fftri(inv_, spec_.data(), timeBuf_.data());
        const float scale = 1.0f / static_cast<float>(nfft_);

        // Synthesis window and overlap-add
        for (std::size_t k = 0; k < nfft_; ++k)
            outAcc_[k] += timeBuf_[k] * scale * win_[k];
    }

    static constexpr float kMinPSD = 1e-12f;

    std::size_t hop_, nfft_, nBins_;
    WienerVadParams p_;

    // KissFFT plans
    kiss_fftr_cfg fwd_ = nullptr;
    kiss_fftr_cfg inv_ = nullptr;

    // Coefficients
    float aEm_ = 0, aFall_ = 0, aRise_ = 0, aNoise_ = 0;
    int hangoverFrames_ = 1, calibFrames_ = 3;
    std::size_t ss_ = 1, ee_ = 1;

    // Buffers (all allocated in the constructor)
    std::vector<float> inBuf_, outAcc_, outBlock_, win_, timeBuf_;
    std::vector<kiss_fft_cpx> spec_;
    std::vector<float> psd_, noisePSD_, noiseSum_, gain_, prevClean_;

    // VAD state
    float Em_ = 0, Nf_ = 0, margin_ = 0;
    int hov_ = 0;

    // Calibration state
    int calibCount_ = 0, warmupFrames_ = 0;
    double enSum_ = 0.0;

    // Cross-thread flags
    std::atomic<bool> calibrating_{ true };
    std::atomic<bool> recalRequested_{ false };
    std::atomic<bool> vad_{ false };
};