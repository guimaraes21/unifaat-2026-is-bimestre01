#include "AncController.h"

namespace anc {

namespace {
constexpr float kD0 = 480.0f;          // evita passo enorme se o atraso for pequeno
constexpr float kHardClip = 0.98f;
constexpr float kLeakPerSec = 0.02f;
constexpr float kDemodTau = 0.06f;     // s, demodulação por harmônica
constexpr float kSlowTau = 0.5f;       // s, médias para métricas e proteção
constexpr float kRestAfterLock = 0.25f;     // s de silêncio (+ atraso) antes de adaptar
constexpr float kRestAfterDivergence = 1.0f;
constexpr float kBaselineTau = 0.1f;
constexpr float kBadLimit = 1.0f;      // s acima do limite antes de declarar divergência
constexpr float kAbsFloor = 1e-5f;
// PLL tipo 2 (proporcional + integral). A banda cai com a latência para manter margem de fase.
constexpr float kPllWnMax = 2.5f;
constexpr float kPllZeta = 0.8f;
constexpr float kPllWnTimesDelay = 0.3f;
constexpr int kVerifyDecimation = 4;    // blocos entre pontos da regressão (amostras menos correlacionadas)
constexpr double kVerifyMaxRelErr = 0.1;

// Resolve o sistema 3x3 complexo a*x = b (eliminação de Gauss com pivotamento parcial).
bool solve3(std::complex<double> a[3][3], std::complex<double> b[3], std::complex<double> x[3]) {
    for (int c = 0; c < 3; ++c) {
        int p = c;
        for (int r = c + 1; r < 3; ++r) {
            if (std::abs(a[r][c]) > std::abs(a[p][c])) p = r;
        }
        if (std::abs(a[p][c]) < 1e-30) return false;
        if (p != c) {
            for (int k = 0; k < 3; ++k) std::swap(a[p][k], a[c][k]);
            std::swap(b[p], b[c]);
        }
        for (int r = c + 1; r < 3; ++r) {
            const std::complex<double> f = a[r][c] / a[c][c];
            for (int k = c; k < 3; ++k) a[r][k] -= f * a[c][k];
            b[r] -= f * b[c];
        }
    }
    for (int r = 2; r >= 0; --r) {
        std::complex<double> v = b[r];
        for (int k = r + 1; k < 3; ++k) v -= a[r][k] * x[k];
        x[r] = v / a[r][r];
    }
    return true;
}
}  // namespace

void AncController::configure(int sampleRate, int historySize) {
    fs_ = sampleRate;
    histSize_ = nextPow2(historySize);
    histMask_ = histSize_ - 1;
    phaseRing_.assign(histSize_, 0.0f);
    for (auto& r : zRing_) r.assign(histSize_, cfloat());
    renderedUpTo_ = 0;
    lpA_ = 1.0f / (kDemodTau * fs_);
    leakKeep_ = 1.0f - kLeakPerSec / fs_;
    phase_ = 0.0;
    locked_ = false;
    resetPll();
    resetAll();
    for (int k = 0; k < kMaxHarmonics; ++k) {
        eLp_[k] = dLp_[k] = wLp_[k] = cfloat();
        eSlow_[k] = dSlow_[k] = 0.0f;
        corr_[k] = cfloat(1.0f, 0.0f);
    }
}

void AncController::setModel(const SecondaryPath* model) {
    model_ = model;
    modelMaxMag_ = model ? model->maxMagnitude() : 0.0f;
    for (int k = 0; k < kMaxHarmonics; ++k) {
        corr_[k] = cfloat(1.0f, 0.0f);
        verified_[k] = false;
    }
    refreshPath();
}

void AncController::setParams(const Params& p) {
    params_ = p;
    params_.harmonics = std::clamp(p.harmonics, 1, kMaxHarmonics);
    refreshPath();
}

void AncController::setDelay(int samples) {
    delay_ = std::max(0, samples);
    refreshPath();
}

void AncController::setPathGain(float g) {
    pathGain_ = std::clamp(g, 0.05f, 20.0f);
    for (int k = 0; k < kMaxHarmonics; ++k) {
        corr_[k] = cfloat(1.0f, 0.0f);
        verified_[k] = false;
    }
    refreshPath();
}

void AncController::setAdapting(bool on) {
    if (on == adapting_) return;
    adapting_ = on;
    resetAll();
}

void AncController::resetHarmonic(int k, float restSec) {
    w_[k] = cfloat();
    age_[k] = 0.0f;
    badTime_[k] = 0.0f;
    baseline_[k] = -1.0f;
    // O microfone só deixa de ouvir a anti-onda depois do atraso do caminho.
    rest_[k] = restSec + static_cast<float>(delay_) / fs_;
    // Toda nova convergência a partir de zero é uma chance de conferir o modelo.
    fit_[k] = Fit();
    verifyAge_[k] = 0.0f;
    verified_[k] = false;
    verifyDone_[k] = false;
}

void AncController::resetAll() {
    for (int k = 0; k < kMaxHarmonics; ++k) {
        resetHarmonic(k, kRestAfterLock);
        alphaScale_[k] = 1.0f;
    }
    pllAllowed_ = false;
    modelTrusted_ = true;
    refreshPath();
}

void AncController::resetPll() {
    pllActive_ = false;
    pllAge_ = 0.0f;
    pllInteg_ = 0.0f;
    disagree_ = 0;
}

void AncController::refreshPath() {
    const float effDelay = static_cast<float>(delay_) + kD0;
    // Ganho de malha por amostra g = convergence / atraso  =>  g*atraso = convergence.
    mu0_ = 2.0f * params_.convergence / effDelay;
    const float minMag = modelMaxMag_ * std::pow(10.0f, params_.minPathDb / 20.0f);
    for (int k = 0; k < kMaxHarmonics; ++k) {
        const float f = freq_ * (k + 1);
        track_[k] = locked_ && k < params_.harmonics && f <= params_.fmax && f >= 15.0f;
        active_[k] = false;
        cfloat h;
        if (track_[k] && model_ && model_->lookup(f, h) && std::abs(h) >= minMag && std::abs(h) > 0.0f) {
            active_[k] = true;
            h0_[k] = h * pathGain_ * corr_[k];
            mu_[k] = mu0_ * alphaScale_[k] / std::norm(h0_[k]);
        }
    }
}

void AncController::render(int64_t n0, float* out, int frames, const float* gain) {
    const double inc = static_cast<double>(freq_) / fs_;
    for (int i = 0; i < frames; ++i) {
        const int64_t slot = (n0 + i) & histMask_;
        const double ph = phase_;
        phaseRing_[slot] = static_cast<float>(ph);
        float y = 0.0f;
        for (int k = 0; k < kMaxHarmonics; ++k) {
            cfloat z;
            if (adapting_ && w_[k] != cfloat()) {
                z = gain[i] * w_[k] * phasor(static_cast<float>((k + 1) * ph));
            }
            zRing_[k][slot] = z;
            y += z.real();
        }
        out[i] = std::clamp(y, -kHardClip, kHardClip);
        phase_ = fracCycles(ph + inc);
    }
    renderedUpTo_ = n0 + frames;
}

void AncController::capture(int64_t m0, const float* e, float* dhat, int frames) {
    cfloat ref[kMaxHarmonics];
    for (int i = 0; i < frames; ++i) {
        const float ei = e[i];
        const int64_t idx = m0 + i - delay_;
        float dh = ei;
        if (idx >= 0 && idx < renderedUpTo_ && idx > renderedUpTo_ - histSize_) {
            const int64_t slot = idx & histMask_;
            const float ph = phaseRing_[slot];
            float pred = 0.0f;
            for (int k = 0; k < kMaxHarmonics; ++k) {
                ref[k] = phasor(static_cast<float>(k + 1) * ph);
                const cfloat z = zRing_[k][slot];
                if (z != cfloat()) pred += (h0_[k] * z).real();
                if (!track_[k]) continue;
                eLp_[k] += (2.0f * ei * std::conj(ref[k]) - eLp_[k]) * lpA_;
                wLp_[k] += (z * std::conj(ref[k]) - wLp_[k]) * lpA_;
                if (adapting_ && active_[k] && rest_[k] <= 0.0f) {
                    // FxLMS: w <- w - mu * e * conj(x filtrado), com x filtrado = H0 * referência atrasada.
                    w_[k] = w_[k] * leakKeep_ - (mu_[k] * ei) * std::conj(h0_[k] * ref[k]);
                }
            }
            dh = ei - pred;
            for (int k = 0; k < kMaxHarmonics; ++k) {
                if (track_[k]) dLp_[k] += (2.0f * dh * std::conj(ref[k]) - dLp_[k]) * lpA_;
            }
        }
        dhat[i] = dh;
        if (++blockCount_ >= kBlock) {
            blockCount_ = 0;
            blockUpdate();
        }
    }
}

// Mantém constante a fase do ronco estimado em relação ao oscilador. Assim o peso ótimo do
// LMS fica parado mesmo quando a rotação varia devagar, e o LMS (lento por causa da latência)
// só precisa acompanhar mudanças de amplitude.
void AncController::updatePll(float dt) {
    // Com erro de modelo grande a própria anti-onda contamina a estimativa do ronco e puxa a PLL.
    // Depois de uma divergência, a frequência segue só o rastreador por FFT (mais lento, robusto)
    // até a verificação online confirmar o modelo.
    if (!locked_ || !track_[0] || (adapting_ && !pllAllowed_)) {
        resetPll();
        return;
    }
    pllAge_ += dt;
    const float mag = std::abs(dLp_[0]);
    if (mag < kAbsFloor) return;
    const float arg = std::arg(dLp_[0]);
    if (!pllActive_) {
        // Espera a demodulação assentar (atraso do caminho + filtro) antes de ancorar.
        const float settle = 4.0f * kDemodTau + static_cast<float>(delay_) / fs_;
        if (pllAge_ > settle) {
            pllActive_ = true;
            pllRefArg_ = arg;
            pllInteg_ = 0.0f;
            pllCenter_ = freq_;
        }
        return;
    }
    const float err = wrapPi(arg - pllRefArg_);
    if (std::fabs(err) > 2.0f) {
        // Escorregou um ciclo (aceleração forte): reancora na frequência atual.
        pllRefArg_ = arg;
        pllCenter_ = freq_;
        pllInteg_ = 0.0f;
        return;
    }
    const float loopDelay = static_cast<float>(delay_) / fs_ + kDemodTau;
    const float wn = std::min(kPllWnMax, kPllWnTimesDelay / loopDelay);
    const float kp = 2.0f * kPllZeta * wn / kTwoPi;  // Hz por radiano
    const float ki = wn * wn / kTwoPi;               // Hz por radiano*segundo
    pllInteg_ += err * dt;
    freq_ = pllCenter_ + kp * err + ki * pllInteg_;
}

void AncController::blockUpdate() {
    const float dt = static_cast<float>(kBlock) / fs_;
    const float slowA = dt / kSlowTau;
    for (int k = 0; k < kMaxHarmonics; ++k) {
        eSlow_[k] += (std::abs(eLp_[k]) - eSlow_[k]) * slowA;
        dSlow_[k] += (std::abs(dLp_[k]) - dSlow_[k]) * slowA;
    }
    updatePll(dt);
    if (!adapting_) {
        refreshPath();
        return;
    }

    // Limitador: a soma das amplitudes nunca passa do máximo configurado.
    float sum = 0.0f;
    for (const auto& w : w_) sum += std::abs(w);
    if (sum > params_.maxOutput && sum > 0.0f) {
        const float s = params_.maxOutput / sum;
        for (auto& w : w_) w *= s;
        ++limiterHits_;
    }

    // Proteção contra divergência (modelo errado, volume muito alterado, latência mudou):
    // se o erro numa harmônica fica bem acima do que era antes de a anti-onda crescer,
    // zera aquela harmônica e reduz o passo dela.
    const float tauLms = (static_cast<float>(delay_) + kD0) / std::max(params_.convergence, 1e-3f) / fs_;
    for (int k = 0; k < kMaxHarmonics; ++k) {
        if (!active_[k]) {
            w_[k] *= 0.9f;  // harmônica fora de faixa ou sem modelo: some suavemente
            continue;
        }
        if (rest_[k] > 0.0f) {
            // Em silêncio: o erro é o próprio ronco. Vira a linha de base da proteção.
            rest_[k] -= dt;
            const float now = std::abs(eLp_[k]);
            baseline_[k] = baseline_[k] < 0.0f ? now : baseline_[k] + (now - baseline_[k]) * (dt / kBaselineTau);
            continue;
        }
        age_[k] += dt;
        if (!verifyDone_[k]) updateVerification(k, dt);
        if (age_[k] < std::max(1.0f, 3.0f * tauLms / alphaScale_[k])) continue;
        const bool bad = eSlow_[k] > 1.5f * baseline_[k] + kAbsFloor ||
                         eSlow_[k] > 2.0f * dSlow_[k] + kAbsFloor;
        badTime_[k] = bad ? badTime_[k] + dt : std::max(0.0f, badTime_[k] - dt);
        if (badTime_[k] > kBadLimit) {
            corr_[k] = cfloat(1.0f, 0.0f);  // a correção online pode ter sido a culpada
            resetHarmonic(k, kRestAfterDivergence);
            if (k == 0) {
                pllAllowed_ = false;
                modelTrusted_ = false;
            }
            alphaScale_[k] = std::max(0.125f, alphaScale_[k] * 0.5f);
            ++divergences_;
        }
    }

    if (!locked_) {
        for (auto& w : w_) w *= 0.9f;
    }
    // Pesos que decaíram a quase zero viram zero: evita números subnormais (lentos em ARM).
    for (auto& w : w_) {
        if (std::norm(w) < 1e-24f) w = cfloat();
    }
    ++verifyTick_;
    refreshPath();
}

float AncController::verifyWindow(int k) const {
    const float tauLms = (static_cast<float>(delay_) + kD0) / std::max(params_.convergence, 1e-3f) / fs_;
    return std::clamp(2.0f * tauLms / alphaScale_[k], 0.6f, 5.0f);
}

void AncController::updateVerification(int k, float dt) {
    const float f0 = freq_ * (k + 1);
    if (verifyAge_[k] == 0.0f) verifyFreqMin_[k] = verifyFreqMax_[k] = f0;
    verifyFreqMin_[k] = std::min(verifyFreqMin_[k], f0);
    verifyFreqMax_[k] = std::max(verifyFreqMax_[k], f0);
    verifyAge_[k] += dt;
    Fit& f = fit_[k];
    if (verifyTick_ % kVerifyDecimation == 0) {
        const std::complex<double> x[3] = {1.0, static_cast<double>(verifyAge_[k]),
                                           std::complex<double>(wLp_[k].real(), wLp_[k].imag())};
        const std::complex<double> e(eLp_[k].real(), eLp_[k].imag());
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) f.a[r][c] += std::conj(x[r]) * x[c];
            f.b[r] += std::conj(x[r]) * e;
        }
        f.ee += std::norm(e);
        ++f.n;
    }
    if (verifyAge_[k] < verifyWindow(k)) return;
    const VerifyOutcome out = finishVerification(k);
    if (k == 0) {
        if (out == VerifyOutcome::Accepted) {
            pllAllowed_ = true;
            modelTrusted_ = true;
        } else {
            pllAllowed_ = out == VerifyOutcome::Inconclusive && modelTrusted_;
        }
    }
}

// Fim da janela: resolve E = D0 + D1*t + H*W e confere o erro-padrão de H.
AncController::VerifyOutcome AncController::finishVerification(int k) {
    const Fit fit = fit_[k];
    fit_[k] = Fit();
    verifyAge_[k] = 0.0f;
    verifyDone_[k] = true;
    std::complex<double> a[3][3], b[3], beta[3];
    for (int r = 0; r < 3; ++r) {
        b[r] = fit.b[r];
        for (int c = 0; c < 3; ++c) a[r][c] = fit.a[r][c];
    }
    const int n = fit.n;
    if (n < 8 || !solve3(a, b, beta)) return VerifyOutcome::Inconclusive;
    // Resíduo: E'E - beta^H b (b original, não o eliminado).
    std::complex<double> bh;
    for (int r = 0; r < 3; ++r) bh += std::conj(beta[r]) * fit.b[r];
    const double resid = std::max(0.0, fit.ee - bh.real()) / std::max(1, n - 3);
    // Variância de H: resid * (A^-1)[2][2].
    std::complex<double> a2[3][3], unit[3] = {0.0, 0.0, 1.0}, col[3];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) a2[r][c] = fit.a[r][c];
    }
    if (!solve3(a2, unit, col)) return VerifyOutcome::Inconclusive;
    const double varH = resid * std::abs(col[2]);
    const std::complex<double> h = beta[2];
    const std::complex<double> eMean = fit.b[0] / static_cast<double>(n);
    const double varE = fit.ee / n - std::norm(eMean);
    const double r2 = varE > 0.0 ? 1.0 - resid * (n - 3) / (varE * n) : 0.0;
    const double drift = std::abs(beta[1]) * verifyWindow(k) / (std::abs(beta[0]) + 1e-12);
    // Rotação do motor variando na janela: D e W se moveram juntos e a regressão fica enviesada.
    if (verifyFreqMax_[k] - verifyFreqMin_[k] > 0.15f || drift > 0.5) return VerifyOutcome::Inconclusive;
    // Pouca excitação (W mal variou): não dá para concluir nada.
    if (std::abs(h) < 1e-12 || std::sqrt(varH) > kVerifyMaxRelErr * std::abs(h)) return VerifyOutcome::Inconclusive;
    // Condições boas e mesmo assim o modelo linear não explica o erro: algo está errado.
    if (r2 < 0.9) return VerifyOutcome::Rejected;
    cfloat base;
    if (!model_ || !model_->lookup(freq_ * (k + 1), base)) return VerifyOutcome::Inconclusive;
    base *= pathGain_;
    const cfloat c = cfloat(static_cast<float>(h.real()), static_cast<float>(h.imag())) / base;
    // Um modelo com mais de ~80 graus de erro faria o LMS divergir antes; corrigir tanto assim
    // indica ajuste ruim, não modelo ruim.
    if (std::abs(c) < 0.33f || std::abs(c) > 3.0f || std::fabs(std::arg(c)) > 80.0f * kPi / 180.0f) {
        return VerifyOutcome::Rejected;
    }
    verified_[k] = true;
    if (std::abs(c - corr_[k]) > 0.1f * std::abs(corr_[k])) {
        corr_[k] = c;
        ++corrections_;
        refreshPath();
    }
    return VerifyOutcome::Accepted;
}

void AncController::onTone(const TonePeak& tone) {
    if (!tone.found) {
        if (locked_ && ++lost_ >= 6) {
            locked_ = false;
            jumpPending_ = false;
            resetPll();
            refreshPath();
        }
        return;
    }
    lost_ = 0;
    const float fm = tone.freq;
    if (!locked_) {
        freq_ = fm;
        locked_ = true;
        jumpPending_ = false;
        resetPll();
        for (int k = 0; k < kMaxHarmonics; ++k) resetHarmonic(k, kRestAfterLock);
        refreshPath();
        return;
    }
    // O rastreador pode ter achado uma harmônica da fundamental atual: compara com a família.
    float best = fm;
    float bestErr = std::fabs(fm - freq_);
    for (int r = 2; r <= params_.harmonics; ++r) {
        const float cand = fm / r;
        if (std::fabs(cand - freq_) < bestErr) {
            best = cand;
            bestErr = std::fabs(cand - freq_);
        }
    }
    const float delta = best - freq_;
    const float jumpThr = std::max(1.5f, 0.03f * freq_);
    if (std::fabs(delta) > jumpThr) {
        // Mudança grande (troca de marcha, VCM): exige duas estimativas concordando.
        if (jumpPending_ && std::fabs(fm - jumpFreq_) < 1.0f) {
            freq_ = fm;
            jumpPending_ = false;
            resetPll();
            for (int k = 0; k < kMaxHarmonics; ++k) resetHarmonic(k, kRestAfterLock);
            refreshPath();
        } else {
            jumpPending_ = true;
            jumpFreq_ = fm;
        }
        return;
    }
    jumpPending_ = false;
    if (pllActive_) {
        // A PLL manda; a FFT só intervém se discordar de forma persistente (PLL perdida).
        disagree_ = std::fabs(delta) > 0.6f ? disagree_ + 1 : 0;
        if (disagree_ >= 3) {
            freq_ = best;
            resetPll();
        }
    } else {
        freq_ += 0.5f * delta;
    }
    refreshPath();
}

AncController::HarmonicStatus AncController::harmonic(int k) const {
    HarmonicStatus s;
    if (k < 0 || k >= kMaxHarmonics) return s;
    s.active = active_[k] && adapting_;
    s.freq = freq_ * (k + 1);
    s.errorAmp = eSlow_[k];
    s.disturbanceAmp = dSlow_[k];
    s.outputAmp = std::abs(w_[k]);
    s.reductionDb = (track_[k] && dSlow_[k] > 0.0f) ? ampToDb(eSlow_[k] / dSlow_[k]) : 0.0f;
    s.verified = verified_[k];
    s.pll = k == 0 && pllActive_;
    s.corrDb = ampToDb(std::abs(corr_[k]));
    s.corrDeg = std::arg(corr_[k]) * 180.0f / kPi;
    return s;
}

}  // namespace anc
