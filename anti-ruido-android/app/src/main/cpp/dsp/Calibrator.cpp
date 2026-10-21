#include "Calibrator.h"

#include "Fft.h"

namespace anc {

namespace {
constexpr float kBurstSec = 0.8f;
constexpr float kFadeSec = 0.02f;
constexpr float kMinGccConfidence = 5.0f;  // pico / RMS da correlação
constexpr float kMinNcc = 0.4f;            // correlação com a impressão digital salva
constexpr double kMaxDriftRate = 100e-6;   // acima disso não é deriva de clock, é xrun
constexpr double kDriftDeadband = 1.5e-6;
constexpr double kTwoPiD = 2.0 * 3.14159265358979323846;
}  // namespace

void Calibrator::configure(int sampleRate, float maxDelaySec) {
    fs_ = sampleRate;
    burstLen_ = static_cast<int>(kBurstSec * fs_);
    maxDelay_ = static_cast<int>(maxDelaySec * fs_);
    recordLen_ = burstLen_ + maxDelay_ + SecondaryPath::kIrHalf;
    fftSize_ = nextPow2(recordLen_ + burstLen_);
    burst_.assign(burstLen_, 0.0f);
    record_.assign(recordLen_, 0.0f);
    corr_.assign(recordLen_, 0.0f);
    fa_.assign(fftSize_, cfloat());
    fb_.assign(fftSize_, cfloat());
    xpow_.assign(fftSize_, 0.0f);
    stage_.store(Stage::Idle);
}

void Calibrator::scheduleBurst(int64_t now) {
    // Começa um pouco no futuro: garante que nenhuma amostra do microfone com índice
    // >= burstStart_ já tenha sido consumida (a entrada pode estar adiantada em relação à saída).
    burstStart_ = std::max(now, lastInput_) + fs_ / 20;
    recorded_ = 0;
}

bool Calibrator::start(Kind kind, const Settings& settings, const SecondaryPath* reference,
                       int64_t nowOut, int64_t nowIn) {
    if (workerBusy_.load() || burstLen_ == 0) return false;
    kind_ = kind;
    settings_ = settings;
    if (reference) {
        reference_ = *reference;
    } else {
        reference_.clear();
    }
    if (kind_ == Kind::Full) model_.clear();
    error_ = kOk;
    confidence_ = 0.0f;
    sessionGain_ = 1.0f;
    driftRate_ = 0.0;
    secondBurst_ = false;
    sweepEndN_ = -1;

    // Ruído branco com rampas nas pontas (evita estalos). A mesma sequência serve às duas rajadas.
    Random rng(0xA5C3u + static_cast<uint32_t>(nowOut));
    const int fade = static_cast<int>(kFadeSec * fs_);
    for (int i = 0; i < burstLen_; ++i) {
        float g = 1.0f;
        if (i < fade) g = static_cast<float>(i) / fade;
        if (i > burstLen_ - fade) g = static_cast<float>(burstLen_ - i) / fade;
        burst_[i] = settings_.burstLevel * g * rng.uniform();
    }
    lastInput_ = nowIn;
    scheduleBurst(nowOut);
    firstBurstStart_ = burstStart_;

    // Grade logarítmica da varredura.
    numSteps_ = 0;
    if (kind_ == Kind::Full) {
        for (int i = 0; i < SecondaryPath::kMaxPoints; ++i) {
            const float f = settings_.fStart *
                            std::pow(2.0f, static_cast<float>(i) / settings_.pointsPerOctave);
            if (f > settings_.fEnd * 1.0001f) break;
            grid_[numSteps_++] = f;
        }
    }
    step_ = 0;
    stepPending_ = false;
    sweepStarted_ = false;
    phase_ = 0.0;
    stage_.store(Stage::Burst);
    return true;
}

void Calibrator::abort() { stage_.store(Stage::Idle); }

float Calibrator::progress() const {
    const Stage s = stage();
    const float burstFrac = static_cast<float>(recorded_) / std::max(1, recordLen_);
    if (kind_ == Kind::LatencyOnly) {
        if (s == Stage::Burst) return 0.9f * burstFrac;
        if (s == Stage::Analyze) return 0.95f;
        return s == Stage::Done ? 1.0f : 0.0f;
    }
    switch (s) {
        case Stage::Burst:
            return secondBurst_ ? 0.92f + 0.06f * burstFrac : 0.08f * burstFrac;
        case Stage::Analyze:
            return secondBurst_ ? 0.99f : 0.09f;
        case Stage::Sweep:
            return 0.1f + 0.82f * static_cast<float>(step_) / std::max(1, numSteps_);
        case Stage::Done:
            return 1.0f;
        default:
            return 0.0f;
    }
}

void Calibrator::beginStep(int64_t n) {
    stepStartN_ = n;
    stepPhase0_ = phase_;
    stepFreq_ = grid_[step_];
    const float f = stepFreq_;
    // Janela com número inteiro de períodos (cancela o termo em 2f do lock-in),
    // pelo menos 0,25 s e 10 períodos.
    const double periods = std::ceil(std::max(0.25 * f, 10.0));
    winLen_ = static_cast<int64_t>(std::llround(periods / f * fs_));
    winStart_ = n + model_.delayRef + static_cast<int64_t>(settings_.settleSec * fs_);
    accRe_ = accIm_ = energy_ = 0.0;
    count_ = 0;
    stepPending_ = false;
}

void Calibrator::render(int64_t n0, float* out, int frames) {
    const Stage s = stage();
    const int fade = static_cast<int>(kFadeSec * fs_);
    for (int i = 0; i < frames; ++i) {
        const int64_t n = n0 + i;
        float v = 0.0f;
        if (s == Stage::Burst) {
            const int64_t k = n - burstStart_;
            if (k >= 0 && k < burstLen_) v = burst_[k];
        } else if (s == Stage::Sweep) {
            if (!sweepStarted_) {
                sweepStarted_ = true;
                fadeInStart_ = n;
                beginStep(n);
            } else if (stepPending_ && step_ < numSteps_) {
                beginStep(n);
            }
            float g = 1.0f;
            if (n - fadeInStart_ < fade) g = static_cast<float>(n - fadeInStart_) / fade;
            if (step_ >= numSteps_) {
                // Varredura terminou: rampa de saída antes da segunda rajada.
                if (sweepEndN_ < 0) sweepEndN_ = n;
                g = std::max(0.0f, 1.0f - static_cast<float>(n - sweepEndN_) / fade);
            }
            v = settings_.probeLevel * g * phasor(static_cast<float>(phase_)).real();
            phase_ = fracCycles(phase_ + static_cast<double>(stepFreq_) / fs_);
        }
        out[i] = v;
    }
    if (s == Stage::Sweep && sweepEndN_ >= 0 && n0 + frames >= sweepEndN_ + fade && !secondBurst_) {
        secondBurst_ = true;
        scheduleBurst(n0 + frames);
        stage_.store(Stage::Burst);
    }
}

void Calibrator::capture(int64_t m0, const float* e, int frames) {
    lastInput_ = m0 + frames;
    const Stage s = stage();
    if (s == Stage::Burst) {
        for (int i = 0; i < frames; ++i) {
            const int64_t k = m0 + i - burstStart_;
            if (k < 0 || k >= recordLen_) continue;
            record_[k] = e[i];
            recorded_ = k + 1;
        }
        if (recorded_ >= recordLen_) stage_.store(Stage::Analyze);
        return;
    }
    if (s != Stage::Sweep || !sweepStarted_ || stepPending_ || step_ >= numSteps_) return;
    const double inc = static_cast<double>(stepFreq_) / fs_;
    for (int i = 0; i < frames; ++i) {
        const int64_t m = m0 + i;
        if (m < winStart_ || m >= winStart_ + winLen_) continue;
        const double theta = stepPhase0_ + static_cast<double>(m - stepStartN_) * inc;
        const cfloat ref = phasor(static_cast<float>(fracCycles(theta)));
        accRe_ += static_cast<double>(e[i]) * ref.real();
        accIm_ -= static_cast<double>(e[i]) * ref.imag();
        energy_ += static_cast<double>(e[i]) * e[i];
        ++count_;
        if (count_ >= winLen_) {
            finishStep();
            return;
        }
    }
}

void Calibrator::finishStep() {
    const double w = static_cast<double>(count_);
    const std::complex<double> c(2.0 * accRe_ / w, 2.0 * accIm_ / w);
    const double pTone = std::norm(c) * 0.5;
    const double pTot = energy_ / w;
    const double pNoise = std::max(pTot - pTone, 1e-14);
    const std::complex<double> h = c / static_cast<double>(settings_.probeLevel);
    // Remove o atraso bruto: o que sobra (H0) varia devagar com a frequência.
    const std::complex<double> h0 = h * std::polar(1.0, kTwoPiD * stepFreq_ * model_.delayRef / fs_);

    PathPoint& p = model_.points[step_];
    p.freq = stepFreq_;
    p.h0 = cfloat(static_cast<float>(h0.real()), static_cast<float>(h0.imag()));
    p.snrDb = static_cast<float>(10.0 * std::log10(pTone / pNoise + 1e-30));
    p.valid = p.snrDb >= settings_.minSnrDb;
    // Instante (em índice de saída) a que esta medição se refere, para corrigir deriva depois.
    stepCenter_[step_] = winStart_ + winLen_ / 2 - model_.delayRef;
    ++step_;
    model_.numPoints = step_;
    stepPending_ = true;
}

// GCC-PHAT: correlação cruzada com espectro "branqueado", que gera um pico estreito
// mesmo com a coloração forte de alto-falantes e cabine.
void Calibrator::computeCorrelation() {
    const int n = fftSize_;
    for (int i = 0; i < n; ++i) {
        fa_[i] = cfloat(i < burstLen_ ? burst_[i] : 0.0f, 0.0f);
        fb_[i] = cfloat(i < recordLen_ ? record_[i] : 0.0f, 0.0f);
    }
    Fft fft(n);
    fft.forward(fa_.data());
    fft.forward(fb_.data());
    double meanMag = 0.0;
    for (int k = 0; k < n; ++k) {
        xpow_[k] = std::norm(fa_[k]);
        fb_[k] *= std::conj(fa_[k]);  // espectro cruzado bruto (usado depois para o ganho)
        meanMag += std::abs(fb_[k]);
    }
    meanMag /= n;
    const float floorMag = static_cast<float>(meanMag * 0.01 + 1e-20);
    for (int k = 0; k < n; ++k) {
        const float f = static_cast<float>(std::min(k, n - k)) * fs_ / n;
        // Banda útil com transições suaves: 100-200 Hz e 6-8 kHz.
        float w = 0.0f;
        if (f >= 200.0f && f <= 6000.0f) {
            w = 1.0f;
        } else if (f > 100.0f && f < 200.0f) {
            w = 0.5f - 0.5f * std::cos(kPi * (f - 100.0f) / 100.0f);
        } else if (f > 6000.0f && f < 8000.0f) {
            w = 0.5f + 0.5f * std::cos(kPi * (f - 6000.0f) / 2000.0f);
        }
        const cfloat g = fb_[k];
        fa_[k] = w > 0.0f ? g * (w / std::max(std::abs(g), floorMag)) : cfloat();
    }
    fft.inverse(fa_.data());
    for (int l = 0; l < recordLen_; ++l) corr_[l] = fa_[l].real();
}

// |H0| médio em bandas de 1/3 de oitava, com o atraso removido antes de somar
// (senão a fase gira dezenas de voltas dentro da banda e a soma se anula).
void Calibrator::bandResponse(int delay, float* mag) const {
    const int n = fftSize_;
    for (int b = 0; b < SecondaryPath::kBands; ++b) {
        const float fc = SecondaryPath::bandCenter(b);
        const int k0 = static_cast<int>(std::ceil(fc * std::pow(2.0f, -1.0f / 6.0f) * n / fs_));
        const int k1 = static_cast<int>(std::floor(fc * std::pow(2.0f, 1.0f / 6.0f) * n / fs_));
        std::complex<double> num;
        double den = 0.0;
        for (int k = k0; k <= k1; ++k) {
            const double ph = kTwoPiD * k * static_cast<double>(delay) / n;
            num += std::complex<double>(fb_[k].real(), fb_[k].imag()) * std::polar(1.0, ph);
            den += xpow_[k];
        }
        mag[b] = den > 0.0 ? static_cast<float>(std::abs(num) / den) : 0.0f;
    }
}

// Casa a correlação nova com a impressão digital da calibração: robusto mesmo quando
// dois alto-falantes geram picos de altura parecida.
bool Calibrator::matchFingerprint(const SecondaryPath& ref, int& center, double& ncc) const {
    const int searchMax = std::min(maxDelay_, recordLen_ - 1);
    double irEnergy = 0.0;
    for (int i = 0; i < SecondaryPath::kIrLen; ++i) irEnergy += static_cast<double>(ref.ir[i]) * ref.ir[i];
    ncc = -1.0;
    center = 0;
    for (int c = 0; c <= searchMax; ++c) {
        double dot = 0.0, en = 0.0;
        const int first = c - SecondaryPath::kIrHalf;
        const int iStart = std::max(0, -first);
        const int iEnd = std::min(SecondaryPath::kIrLen, recordLen_ - first);
        for (int i = iStart; i < iEnd; ++i) {
            const float r = corr_[first + i];
            dot += static_cast<double>(r) * ref.ir[i];
            en += static_cast<double>(r) * r;
        }
        const double v = dot / std::sqrt(en * irEnergy + 1e-30);
        if (v > ncc) {
            ncc = v;
            center = c;
        }
    }
    return ncc >= kMinNcc;
}

void Calibrator::doOfflineWork() {
    // Marca "ocupado" antes de conferir o estágio: start() recusa enquanto isso estiver ativo,
    // então os buffers não são reescritos no meio do cálculo.
    workerBusy_.store(true);
    if (stage_.load() != Stage::Analyze) {
        workerBusy_.store(false);
        return;
    }
    computeCorrelation();

    const int searchMax = std::min(maxDelay_, recordLen_ - 1);
    Stage next = Stage::Failed;
    const bool firstOfFull = kind_ == Kind::Full && !secondBurst_;
    if (firstOfFull || (kind_ == Kind::LatencyOnly && !reference_.valid)) {
        int peak = 0;
        double sumSq = 0.0;
        for (int l = 0; l <= searchMax; ++l) {
            sumSq += static_cast<double>(corr_[l]) * corr_[l];
            if (std::fabs(corr_[l]) > std::fabs(corr_[peak])) peak = l;
        }
        const double rms = std::sqrt(sumSq / (searchMax + 1)) + 1e-20;
        confidence_ = static_cast<float>(std::fabs(corr_[peak]) / rms);
        if (confidence_ < kMinGccConfidence) {
            error_ = kErrNoCorrelation;
        } else if (peak >= searchMax - 2) {
            error_ = kErrDelayTooLarge;
        } else {
            sessionDelay_ = peak;
            if (firstOfFull) {
                model_.delayRef = peak;
                bandResponse(peak, model_.bandMag);
                const float norm = 1.0f / std::max(std::fabs(corr_[peak]), 1e-20f);
                for (int i = 0; i < SecondaryPath::kIrLen; ++i) {
                    const int l = peak - SecondaryPath::kIrHalf + i;
                    model_.ir[i] = (l >= 0 && l < recordLen_) ? corr_[l] * norm : 0.0f;
                }
                next = Stage::Sweep;
            } else {
                next = Stage::Done;
            }
        }
    } else if (kind_ == Kind::Full) {
        // Segunda rajada da calibração: quanto o atraso andou durante a varredura?
        int center = 0;
        double ncc = 0.0;
        const bool matched = matchFingerprint(model_, center, ncc);
        confidence_ = static_cast<float>(ncc);
        const double span = static_cast<double>(burstStart_ - firstBurstStart_);
        const double rate = span > 0.0 ? (center - model_.delayRef) / span : 0.0;
        if (!matched) {
            error_ = kErrNoCorrelation;
        } else if (std::fabs(rate) > kMaxDriftRate) {
            error_ = kErrGlitch;  // salto grande: xrun no meio da varredura
        } else {
            driftRate_ = std::fabs(rate) < kDriftDeadband ? 0.0 : rate;
            for (int i = 0; i < model_.numPoints; ++i) {
                const double shift = driftRate_ * static_cast<double>(stepCenter_[i] - firstBurstStart_);
                model_.points[i].h0 *= std::polar(1.0f, static_cast<float>(kTwoPiD * model_.points[i].freq * shift / fs_));
            }
            sessionDelay_ = center;
            model_.sampleRate = fs_;
            model_.valid = model_.validPoints() >= 8;
            if (model_.valid) {
                next = Stage::Done;
            } else {
                error_ = kErrFewPoints;
            }
        }
    } else {
        int center = 0;
        double ncc = 0.0;
        const bool matched = matchFingerprint(reference_, center, ncc);
        confidence_ = static_cast<float>(ncc);
        if (!matched) {
            error_ = kErrNoCorrelation;
        } else {
            sessionDelay_ = center;
            // Mediana da razão por banda: robusta a bandas com ruído (motor, música).
            float mag[SecondaryPath::kBands];
            float ratios[SecondaryPath::kBands];
            int nr = 0;
            bandResponse(center, mag);
            for (int b = 0; b < SecondaryPath::kBands; ++b) {
                if (reference_.bandMag[b] > 1e-6f && mag[b] > 0.0f) ratios[nr++] = mag[b] / reference_.bandMag[b];
            }
            if (nr >= 3) {
                std::nth_element(ratios, ratios + nr / 2, ratios + nr);
                sessionGain_ = std::clamp(ratios[nr / 2], 0.1f, 10.0f);
            }
            next = Stage::Done;
        }
    }

    Stage expected = Stage::Analyze;
    stage_.compare_exchange_strong(expected, next);
    workerBusy_.store(false);
}

}  // namespace anc
