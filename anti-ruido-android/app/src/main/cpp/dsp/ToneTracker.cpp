#include "ToneTracker.h"

namespace anc {

void SpectrumAnalyzer::configure(float rate, int fftSize, int hop) {
    rate_ = rate;
    n_ = fftSize;
    hop_ = hop;
    fft_.resize(n_);
    ring_.assign(n_, 0.0f);
    window_.resize(n_);
    for (int i = 0; i < n_; ++i) {
        window_[i] = 0.5f - 0.5f * std::cos(kTwoPi * static_cast<float>(i) / static_cast<float>(n_));
    }
    work_.assign(n_, cfloat());
    amp_.assign(n_ / 2 + 1, 0.0f);
    reset();
}

void SpectrumAnalyzer::reset() {
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    writePos_ = 0;
    filled_ = 0;
    sinceLast_ = 0;
}

bool SpectrumAnalyzer::push(float x) {
    ring_[writePos_] = x;
    writePos_ = (writePos_ + 1) % n_;
    if (filled_ < n_) ++filled_;
    if (++sinceLast_ < hop_ || filled_ < n_) return false;
    sinceLast_ = 0;
    for (int i = 0; i < n_; ++i) {
        work_[i] = cfloat(ring_[(writePos_ + i) % n_] * window_[i], 0.0f);
    }
    fft_.forward(work_.data());
    // Soma da janela de Hann = N/2; uma senoide de amplitude A gera |X| = A*N/4 no pico.
    const float scale = 4.0f / static_cast<float>(n_);
    for (int k = 0; k <= n_ / 2; ++k) amp_[k] = std::abs(work_[k]) * scale;
    return true;
}

namespace {

bool inSearch(float f, const ToneSearch& s) {
    if (f < s.fmin || f > s.fmax) return false;
    if (s.numHints <= 0) return true;
    for (int i = 0; i < s.numHints; ++i) {
        const float h = s.hints[i];
        if (h > 0.0f && std::fabs(f - h) <= h * s.hintRelWidth) return true;
    }
    return false;
}

// Interpolação parabólica sobre log(amplitude): refina frequência e amplitude do pico.
void refinePeak(const float* amp, int k, float binHz, float& freq, float& amplitude) {
    const float a = std::log(std::max(amp[k - 1], 1e-12f));
    const float b = std::log(std::max(amp[k], 1e-12f));
    const float c = std::log(std::max(amp[k + 1], 1e-12f));
    const float den = a - 2.0f * b + c;
    float p = 0.0f;
    if (std::fabs(den) > 1e-12f) p = std::clamp(0.5f * (a - c) / den, -0.5f, 0.5f);
    freq = (static_cast<float>(k) + p) * binHz;
    amplitude = std::exp(b - 0.25f * (a - c) * p);
}

// Maior máximo local num intervalo de bins; -1 se nada.
int localMaxNear(const float* amp, int bins, int center, int radius) {
    int best = -1;
    for (int k = std::max(1, center - radius); k <= std::min(bins - 2, center + radius); ++k) {
        if (amp[k] >= amp[k - 1] && amp[k] > amp[k + 1] && (best < 0 || amp[k] > amp[best])) best = k;
    }
    return best;
}

}  // namespace

TonePeak findTone(const float* amp, int bins, float binHz, const ToneSearch& s, float* scratch) {
    TonePeak result;
    const int kLo = std::max(1, static_cast<int>(std::ceil(s.fmin / binHz)));
    const int kHi = std::min(bins - 2, static_cast<int>(std::floor(s.fmax / binHz)));
    if (kHi - kLo < 4) return result;

    // Piso de ruído = mediana do espectro na faixa toda (robusta a alguns picos).
    int count = 0;
    for (int k = kLo; k <= kHi; ++k) scratch[count++] = amp[k];
    std::nth_element(scratch, scratch + count / 2, scratch + count);
    const float floorAmp = std::max(scratch[count / 2], 1e-9f);

    int best = -1;
    for (int k = kLo; k <= kHi; ++k) {
        if (!(amp[k] >= amp[k - 1] && amp[k] > amp[k + 1])) continue;
        if (!inSearch(static_cast<float>(k) * binHz, s)) continue;
        if (best < 0 || amp[k] > amp[best]) best = k;
    }
    if (best < 0) return result;

    if (s.preferSubharmonic) {
        // Motores geram famílias de ordens (ex.: 1,5 / 3 / 4,5 no V6 com VCM).
        // Uma fundamental mais baixa e ainda forte cobre as demais via harmônicas.
        for (int div = 3; div >= 2; --div) {
            const int center = static_cast<int>(std::lround(static_cast<float>(best) / div));
            if (static_cast<float>(center) * binHz < s.fmin) continue;
            const int k = localMaxNear(amp, bins, center, 2);
            if (k < 0) continue;
            const float relDb = ampToDb(amp[k] / amp[best]);
            const float promDb = ampToDb(amp[k] / floorAmp);
            if (relDb > -10.0f && promDb >= s.minProminenceDb) {
                best = k;
                break;
            }
        }
    }

    refinePeak(amp, best, binHz, result.freq, result.amplitude);
    result.prominenceDb = ampToDb(amp[best] / floorAmp);
    result.found = result.prominenceDb >= s.minProminenceDb;
    return result;
}

}  // namespace anc
