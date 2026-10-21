#include "AncProcessor.h"

namespace anc {

namespace {
constexpr int kChunk = 4096;
constexpr float kMaxDelaySec = 0.8f;
constexpr float kStartupGuardSec = 0.3f;
constexpr float kMasterFadeSec = 0.03f;
constexpr float kAncFadeSec = 0.5f;

int calibratorErrorToProcessor(int e) {
    switch (e) {
        case Calibrator::kErrNoCorrelation:
            return AncProcessor::kErrNoCorrelation;
        case Calibrator::kErrFewPoints:
            return AncProcessor::kErrFewPoints;
        case Calibrator::kErrDelayTooLarge:
            return AncProcessor::kErrDelayTooLarge;
        case Calibrator::kErrGlitch:
            return AncProcessor::kErrGlitch;
        default:
            return AncProcessor::kErrNone;
    }
}
}  // namespace

void AncProcessor::configure(int sampleRate) {
    fs_ = sampleRate;
    nOut_ = nIn_ = 0;
    calibrator_.configure(fs_, kMaxDelaySec);
    controller_.configure(fs_, static_cast<int>((kMaxDelaySec + 0.25f) * fs_) + 16384);
    controller_.setModel(&model_);
    dc_.configure(static_cast<float>(fs_));

    const int factor = std::max(1, static_cast<int>(std::lround(fs_ / 1000.0)));
    const float decRate = static_cast<float>(fs_) / factor;
    decimEstimate_.configure(static_cast<float>(fs_), factor, 0.32f * decRate);
    decimMic_.configure(static_cast<float>(fs_), factor, 0.32f * decRate);
    specEstimate_.configure(decRate, 1024, 256);
    specMic_.configure(decRate, 1024, 256);

    scratchE_.assign(kChunk, 0.0f);
    scratchD_.assign(kChunk, 0.0f);
    scratchOut_.assign(kChunk, 0.0f);
    scratchGain_.assign(kChunk * 2, 0.0f);
    scratchFind_.assign(kSpectrumBins, 0.0f);
    {
        std::lock_guard<std::mutex> lock(shareMutex_);
        sharedSpecEstimate_.assign(kSpectrumBins, 0.0f);
        sharedSpecMic_.assign(kSpectrumBins, 0.0f);
        sharedBins_ = 0;
        sharedStatus_ = Status();
    }

    state_ = State::Monitor;
    fadingOut_ = false;
    masterGain_ = 1.0f;
    ancGain_ = 0.0f;
    pendingCalStart_ = 0;
    error_ = kErrNone;
    sessionDelayKnown_ = false;
    latencyVerified_ = false;
    lagInit_ = false;
    lastLatencyN_ = -1;
    driftRate_ = 0.0;
    for (auto& t : remeasureTimes_) t = -1;
    discHandled_ = discRequests_.load();
    handledSerial_ = requestSerial_.load();
    controller_.setParams({settings_.harmonics, settings_.convergence, settings_.maxOutput, settings_.fmax});
}

// ---------------------------------------------------------------------------------------------
// Controle (outras threads)

void AncProcessor::requestMode(Mode m) {
    requestedMode_.store(static_cast<int>(m));
    requestSerial_.fetch_add(1);
}

void AncProcessor::setSettings(const Settings& s) {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pendingSettings_ = s;
    settingsDirty_.store(true);
}

void AncProcessor::setHints(const float* freqs, int count) {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pendingNumHints_ = std::clamp(count, 0, kMaxHints);
    for (int i = 0; i < pendingNumHints_; ++i) pendingHints_[i] = freqs[i];
    hintsDirty_.store(true);
}

bool AncProcessor::loadModel(const float* data, int count) {
    SecondaryPath m;
    if (!m.deserialize(data, count)) return false;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingModel_ = m;
        modelDirty_.store(true);
    }
    std::lock_guard<std::mutex> lock(exportMutex_);
    exportedModel_ = m;
    return true;
}

int AncProcessor::exportModel(float* out, int capacity) {
    std::lock_guard<std::mutex> lock(exportMutex_);
    if (!exportedModel_.valid) return 0;
    if (!out) return exportedModel_.serializedSize();
    return exportedModel_.serialize(out, capacity);
}

AncProcessor::Status AncProcessor::status() {
    std::lock_guard<std::mutex> lock(shareMutex_);
    return sharedStatus_;
}

int AncProcessor::spectrum(float* estimate, float* mic, int capacity, float* binHz) {
    std::lock_guard<std::mutex> lock(shareMutex_);
    const int n = std::min(capacity, sharedBins_);
    for (int i = 0; i < n; ++i) {
        estimate[i] = sharedSpecEstimate_[i];
        mic[i] = sharedSpecMic_[i];
    }
    if (binHz) *binHz = sharedBinHz_;
    return n;
}

// ---------------------------------------------------------------------------------------------
// Thread de áudio

void AncProcessor::applyPending() {
    if (settingsDirty_.load() && pendingMutex_.try_lock()) {
        settings_ = pendingSettings_;
        settingsDirty_.store(false);
        pendingMutex_.unlock();
        controller_.setParams({settings_.harmonics, settings_.convergence, settings_.maxOutput, settings_.fmax});
    }
    if (hintsDirty_.load() && pendingMutex_.try_lock()) {
        numHints_ = pendingNumHints_;
        for (int i = 0; i < numHints_; ++i) hints_[i] = pendingHints_[i];
        hintsDirty_.store(false);
        pendingMutex_.unlock();
    }
    // Só troca o modelo fora do ANC/calibração.
    if (state_ == State::Monitor && !fadingOut_ && modelDirty_.load() && pendingMutex_.try_lock()) {
        model_ = pendingModel_;
        modelDirty_.store(false);
        pendingMutex_.unlock();
        controller_.setModel(&model_);
        sessionDelayKnown_ = false;
        latencyVerified_ = false;
    }
}

void AncProcessor::handleRequest() {
    const uint32_t serial = requestSerial_.load();
    if (serial == handledSerial_) return;
    // Logo após abrir os streams os contadores ainda estão se acomodando.
    if (nOut_ < static_cast<int64_t>(kStartupGuardSec * fs_)) return;
    handledSerial_ = serial;
    const Mode m = static_cast<Mode>(requestedMode_.load());
    error_ = kErrNone;
    switch (m) {
        case Mode::Monitor:
            pendingCalStart_ = 0;
            beginTransition(State::Monitor, false);
            break;
        case Mode::Anc:
            if (state_ == State::Anc || (state_ == State::Latency && thenAnc_)) return;
            unstableRetried_ = false;
            beginTransition(State::Anc, false);
            break;
        case Mode::Calibrate:
            beginTransition(State::Calibrate, false);
            break;
        case Mode::MeasureLatency:
            latencyReason_ = Reason::Manual;
            beginTransition(State::Latency, false);
            break;
    }
}

void AncProcessor::beginTransition(State target, bool thenAnc) {
    const bool sounding = state_ != State::Monitor;
    if (sounding && masterGain_ > 0.0f) {
        fadingOut_ = true;
        fadeTarget_ = target;
        fadeThenAnc_ = thenAnc;
        return;
    }
    enterState(target, thenAnc);
}

void AncProcessor::enterState(State target, bool thenAnc) {
    if (state_ == State::Calibrate || state_ == State::Latency) calibrator_.abort();
    controller_.setAdapting(false);
    state_ = State::Monitor;
    switch (target) {
        case State::Monitor:
            break;
        case State::Anc:
            startAnc(true);
            break;
        case State::Calibrate: {
            Calibrator::Settings cs;
            cs.burstLevel = settings_.calBurstLevel;
            cs.probeLevel = settings_.probeLevel;
            if (calibrator_.start(Calibrator::Kind::Full, cs, nullptr, nOut_, nIn_)) {
                state_ = State::Calibrate;
                pendingCalStart_ = 0;
            } else {
                pendingCalStart_ = 1;
            }
            break;
        }
        case State::Latency:
            burstAttempts_ = 0;
            startLatency(thenAnc, settings_.burstLevel);
            break;
    }
}

void AncProcessor::startLatency(bool thenAnc, float level) {
    if (!model_.valid) {
        error_ = kErrNeedCalibration;
        return;
    }
    if (model_.sampleRate != fs_) {
        error_ = kErrRateMismatch;
        return;
    }
    Calibrator::Settings cs;
    cs.burstLevel = level;
    cs.probeLevel = settings_.probeLevel;
    thenAnc_ = thenAnc;
    pendingLatencyLevel_ = level;
    if (calibrator_.start(Calibrator::Kind::LatencyOnly, cs, &model_, nOut_, nIn_)) {
        state_ = State::Latency;
        pendingCalStart_ = 0;
    } else {
        pendingCalStart_ = 2;
    }
}

void AncProcessor::startAnc(bool allowMeasure) {
    if (!model_.valid) {
        error_ = kErrNeedCalibration;
        return;
    }
    if (model_.sampleRate != fs_) {
        error_ = kErrRateMismatch;
        return;
    }
    if (allowMeasure && settings_.autoLatency) {
        burstAttempts_ = 0;
        latencyReason_ = Reason::Start;
        startLatency(true, settings_.burstLevel);
        return;
    }
    if (!sessionDelayKnown_) {
        // Sem medição nesta sessão: usa o atraso da calibração (pode estar alguns ms errado).
        sessionDelay_ = model_.delayRef;
        sessionGain_ = 1.0f;
        sessionDelayKnown_ = true;
        latencyVerified_ = false;
    }
    controller_.setModel(&model_);
    controller_.setDelay(runDelay());
    controller_.setPathGain(sessionGain_);
    controller_.setAdapting(true);
    ancGain_ = 0.0f;
    state_ = State::Anc;
    divergenceSeen_ = controller_.divergences();
    for (auto& t : divergenceTimes_) t = -1;
}

int AncProcessor::runDelay() const {
    if (lastLatencyN_ < 0) return sessionDelay_;
    return static_cast<int>(std::lround(sessionDelay_ + driftRate_ * static_cast<double>(nOut_ - lastLatencyN_)));
}

// Nova medição de latência no meio do ANC, com teto de 6 por minuto para não virar um
// chiado constante se o sistema estiver tendo xruns sem parar.
void AncProcessor::remeasure(Reason why) {
    if (remeasureTimes_[0] >= 0 && nOut_ - remeasureTimes_[0] < 60LL * fs_) {
        controller_.setAdapting(false);
        controller_.setAdapting(true);
        return;
    }
    for (int i = 0; i < 5; ++i) remeasureTimes_[i] = remeasureTimes_[i + 1];
    remeasureTimes_[5] = nOut_;
    latencyReason_ = why;
    beginTransition(State::Latency, true);
}

void AncProcessor::checkCalibrator() {
    if (fadingOut_ || (state_ != State::Calibrate && state_ != State::Latency)) return;
    const Calibrator::Stage st = calibrator_.stage();
    if (state_ == State::Calibrate) {
        if (st == Calibrator::Stage::Done) {
            model_ = calibrator_.model();
            ++modelSerial_;
            if (exportMutex_.try_lock()) {
                exportedModel_ = model_;
                exportMutex_.unlock();
            }
            controller_.setModel(&model_);
            sessionDelay_ = calibrator_.sessionDelay();
            sessionGain_ = 1.0f;
            lastLatencyN_ = calibrator_.measuredAt();
            driftRate_ = calibrator_.driftRate();
            sessionDelayKnown_ = true;
            latencyVerified_ = true;
            latencyConfidence_ = calibrator_.confidence();
            beginTransition(State::Monitor, false);
        } else if (st == Calibrator::Stage::Failed) {
            error_ = calibratorErrorToProcessor(calibrator_.error());
            latencyConfidence_ = calibrator_.confidence();
            beginTransition(State::Monitor, false);
        }
        return;
    }
    if (st == Calibrator::Stage::Done) {
        const int measured = calibrator_.sessionDelay();
        const int64_t at = calibrator_.measuredAt();
        // Duas medições na mesma sessão, sem xrun entre elas: a diferença em relação ao previsto
        // refina a taxa de deriva de clock (filtro de 1ª ordem sobre a taxa).
        if (sessionDelayKnown_ && latencyVerified_ && lastLatencyN_ >= 0 &&
            latencyReason_ != Reason::Discontinuity) {
            const double span = static_cast<double>(at - lastLatencyN_);
            if (span >= 20.0 * fs_) {
                const double predicted = sessionDelay_ + driftRate_ * span;
                driftRate_ = std::clamp(driftRate_ + (measured - predicted) / span, -100e-6, 100e-6);
                if (std::fabs(driftRate_) < 1.5e-6) driftRate_ = 0.0;
            }
        }
        sessionDelay_ = measured;
        sessionGain_ = calibrator_.sessionGain();
        lastLatencyN_ = at;
        sessionDelayKnown_ = true;
        latencyVerified_ = true;
        latencyConfidence_ = calibrator_.confidence();
        calibrator_.abort();
        state_ = State::Monitor;
        if (thenAnc_) startAnc(false);
    } else if (st == Calibrator::Stage::Failed) {
        latencyConfidence_ = calibrator_.confidence();
        calibrator_.abort();
        state_ = State::Monitor;
        if (burstAttempts_ < 1) {
            // Ambiente barulhento (motor, música): tenta de novo com a rajada mais forte.
            ++burstAttempts_;
            startLatency(thenAnc_, std::min(0.3f, pendingLatencyLevel_ * 2.5f));
        } else {
            error_ = calibratorErrorToProcessor(calibrator_.error());
        }
    }
}

void AncProcessor::render(float* out, int frames) {
    applyPending();
    handleRequest();
    handleDiscontinuity();
    if (pendingCalStart_ != 0 && state_ == State::Monitor && !fadingOut_) {
        if (pendingCalStart_ == 1) {
            enterState(State::Calibrate, false);
        } else {
            startLatency(thenAnc_, pendingLatencyLevel_);
        }
    }

    const float masterStep = 1.0f / (kMasterFadeSec * fs_);
    const float ancStep = 1.0f / (kAncFadeSec * fs_);
    int done = 0;
    while (done < frames) {
        const int len = std::min(frames - done, kChunk);
        float* o = out + done;
        float* gAnc = scratchGain_.data();
        float* gMaster = scratchGain_.data() + kChunk;
        const float target = fadingOut_ ? 0.0f : 1.0f;
        const bool anc = state_ == State::Anc;
        for (int i = 0; i < len; ++i) {
            if (masterGain_ < target) {
                masterGain_ = std::min(target, masterGain_ + masterStep);
            } else if (masterGain_ > target) {
                masterGain_ = std::max(target, masterGain_ - masterStep);
            }
            if (anc && ancGain_ < 1.0f) ancGain_ = std::min(1.0f, ancGain_ + ancStep);
            gMaster[i] = masterGain_;
            gAnc[i] = anc ? masterGain_ * ancGain_ : 0.0f;
        }
        if (anc) {
            controller_.render(nOut_, o, len, gAnc);
        } else {
            // Mantém o histórico de fase do oscilador contínuo mesmo sem emitir a anti-onda.
            controller_.render(nOut_, scratchOut_.data(), len, gAnc);
            if (state_ == State::Calibrate || state_ == State::Latency) {
                calibrator_.render(nOut_, o, len);
                for (int i = 0; i < len; ++i) o[i] *= gMaster[i];
            } else {
                std::fill(o, o + len, 0.0f);
            }
        }
        for (int i = 0; i < len; ++i) outEnergy_ += static_cast<double>(o[i]) * o[i];
        outCount_ += len;
        nOut_ += len;
        done += len;
        if (fadingOut_ && masterGain_ <= 0.0f) {
            fadingOut_ = false;
            enterState(fadeTarget_, fadeThenAnc_);
        }
    }
    checkCalibrator();
}

void AncProcessor::notifyDiscontinuity() { discRequests_.fetch_add(1); }

// Um xrun desloca a relação entre o índice da amostra escrita e o da lida: o atraso que o
// controlador usa deixa de valer. A estimativa contínua por contadores não é confiável
// (depende de quando cada bloco do microfone chega), então a resposta é medir de novo.
void AncProcessor::handleDiscontinuity() {
    const int req = discRequests_.load();
    if (req == discHandled_) return;
    discHandled_ = req;
    ++discontinuities_;
    if (fadingOut_) return;
    switch (state_) {
        case State::Calibrate:
            error_ = kErrGlitch;
            beginTransition(State::Monitor, false);
            break;
        case State::Latency:
            latencyReason_ = Reason::Discontinuity;
            beginTransition(State::Latency, thenAnc_);
            break;
        case State::Anc:
            if (settings_.autoLatency) {
                remeasure(Reason::Discontinuity);
            } else {
                controller_.setAdapting(false);
                controller_.setAdapting(true);
            }
            break;
        case State::Monitor:
            sessionDelayKnown_ = false;
            latencyVerified_ = false;
            break;
    }
}

void AncProcessor::monitorAnc() {
    const int d = runDelay();
    if (d != controller_.delay()) controller_.setDelay(d);
    // Com deriva de clock conhecida, confere a previsão de tempos em tempos.
    if (driftRate_ != 0.0 && settings_.autoLatency && nOut_ - lastLatencyN_ > 120LL * fs_) {
        remeasure(Reason::Periodic);
        return;
    }

    const int dv = controller_.divergences();
    bool fresh = false;
    while (divergenceSeen_ < dv) {
        ++divergenceSeen_;
        fresh = true;
        divergenceTimes_[0] = divergenceTimes_[1];
        divergenceTimes_[1] = divergenceTimes_[2];
        divergenceTimes_[2] = nOut_;
    }
    // Primeira divergência depois de um bom tempo: a causa mais provável é a latência ter
    // mudado (deriva de clock, xrun não reportado). Re-mede e retoma.
    if (fresh && settings_.autoLatency && lastLatencyN_ >= 0 && nOut_ - lastLatencyN_ > 30LL * fs_) {
        remeasure(Reason::Divergence);
        return;
    }
    if (divergenceTimes_[0] >= 0 && nOut_ - divergenceTimes_[0] < 60LL * fs_) {
        for (auto& t : divergenceTimes_) t = -1;
        if (settings_.autoLatency && !unstableRetried_) {
            // Três divergências em um minuto: provavelmente a latência mudou. Re-mede e retoma.
            unstableRetried_ = true;
            remeasure(Reason::Divergence);
        } else {
            error_ = kErrUnstable;
            beginTransition(State::Monitor, false);
        }
    }
}

void AncProcessor::capture(const float* in, int frames) {
    ToneSearch search;
    search.fmin = settings_.fmin;
    search.fmax = std::min(settings_.fmax, 0.3f * specEstimate_.rate());
    search.minProminenceDb = settings_.minProminenceDb;
    search.hints = hints_;
    search.numHints = numHints_;

    bool specUpdated = false;
    int done = 0;
    while (done < frames) {
        const int len = std::min(frames - done, kChunk);
        float* e = scratchE_.data();
        float* dh = scratchD_.data();
        for (int i = 0; i < len; ++i) {
            e[i] = dc_.process(in[done + i]);
            micEnergy_ += static_cast<double>(e[i]) * e[i];
        }
        micCount_ += len;
        const bool calibrating = state_ == State::Calibrate || state_ == State::Latency;
        if (calibrating) {
            calibrator_.capture(nIn_, e, len);
            std::copy(e, e + len, dh);
        } else {
            controller_.capture(nIn_, e, dh, len);
        }
        for (int i = 0; i < len; ++i) {
            float y;
            if (decimEstimate_.process(dh[i], y) && specEstimate_.push(y)) {
                specUpdated = true;
                if (!calibrating) {
                    lastTone_ = findTone(specEstimate_.amplitude(), specEstimate_.bins(),
                                         specEstimate_.binHz(), search, scratchFind_.data());
                    controller_.onTone(lastTone_);
                }
            }
            if (decimMic_.process(e[i], y) && specMic_.push(y)) specUpdated = true;
        }
        nIn_ += len;
        done += len;
    }

    const double lag = static_cast<double>(nOut_ - nIn_);
    if (!lagInit_) {
        lagEma_ = lag;
        lagInit_ = true;
    } else {
        lagEma_ += (lag - lagEma_) * std::min(1.0, frames / (1.0 * fs_));
    }
    if (state_ == State::Anc && !fadingOut_) monitorAnc();

    if (specUpdated) publishSpectrum();
    sinceStatus_ += frames;
    if (sinceStatus_ >= fs_ / 20) {
        sinceStatus_ = 0;
        publishStatus();
    }
}

void AncProcessor::publishSpectrum() {
    if (!shareMutex_.try_lock()) return;
    const int n = std::min(kSpectrumBins, specEstimate_.bins());
    for (int i = 0; i < n; ++i) {
        sharedSpecEstimate_[i] = specEstimate_.amplitude()[i];
        sharedSpecMic_[i] = specMic_.amplitude()[i];
    }
    sharedBins_ = n;
    sharedBinHz_ = specEstimate_.binHz();
    shareMutex_.unlock();
}

void AncProcessor::publishStatus() {
    if (micCount_ > 0) micDb_ = 10.0f * std::log10(static_cast<float>(micEnergy_ / micCount_) + 1e-14f);
    if (outCount_ > 0) outDb_ = 10.0f * std::log10(static_cast<float>(outEnergy_ / outCount_) + 1e-14f);
    micEnergy_ = outEnergy_ = 0.0;
    micCount_ = outCount_ = 0;

    Status s;
    switch (state_) {
        case State::Monitor:
            s.mode = static_cast<int>(Mode::Monitor);
            break;
        case State::Anc:
            s.mode = static_cast<int>(Mode::Anc);
            break;
        case State::Calibrate:
            s.mode = static_cast<int>(Mode::Calibrate);
            break;
        case State::Latency:
            s.mode = static_cast<int>(Mode::MeasureLatency);
            break;
    }
    s.error = error_;
    s.calStage = static_cast<int>(calibrator_.stage());
    s.calProgress = calibrator_.progress();
    s.calStep = calibrator_.sweepStep();
    s.calSteps = calibrator_.sweepSteps();
    const int d = state_ == State::Anc ? controller_.delay() : (sessionDelayKnown_ ? runDelay() : model_.delayRef);
    s.latencyMs = 1000.0f * static_cast<float>(d) / fs_;
    s.pathGainDb = ampToDb(sessionGain_);
    s.latencyConfidence = latencyConfidence_;
    s.latencyVerified = latencyVerified_;
    s.toneFound = lastTone_.found;
    s.toneHz = lastTone_.freq;
    s.toneProminenceDb = lastTone_.prominenceDb;
    s.toneLevelDb = ampToDb(lastTone_.amplitude);
    s.locked = controller_.locked();
    s.ancFreq = controller_.frequency();
    for (int k = 0; k < AncController::kMaxHarmonics; ++k) s.harm[k] = controller_.harmonic(k);
    s.micDb = micDb_;
    s.outDb = outDb_;
    s.divergences = controller_.divergences();
    s.limiterHits = controller_.limiterHits();
    s.discontinuities = discontinuities_;
    s.modelValid = model_.valid;
    s.modelRate = model_.sampleRate;
    s.modelPoints = model_.validPoints();
    s.modelSerial = modelSerial_;
    s.sampleRate = fs_;
    s.lag = static_cast<float>(lagEma_);

    if (!shareMutex_.try_lock()) return;
    sharedStatus_ = s;
    shareMutex_.unlock();
}

}  // namespace anc
