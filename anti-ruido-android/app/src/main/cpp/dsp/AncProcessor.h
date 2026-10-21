// Orquestra o processamento: modos (monitor, calibração, ANC), rastreamento de frequência,
// compensação da latência e troca de dados com a interface.
//
// Threads:
//  - render()/capture()/notifyDiscontinuity(): thread de áudio (sem alocação, sem bloqueio).
//  - doOfflineWork(): thread de trabalho (correlação da calibração).
//  - demais métodos públicos: qualquer outra thread (JNI/UI).
#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include "AncController.h"
#include "Calibrator.h"
#include "Filters.h"
#include "SecondaryPath.h"
#include "ToneTracker.h"

namespace anc {

class AncProcessor {
public:
    enum class Mode : int { Monitor = 0, Anc = 1, Calibrate = 2, MeasureLatency = 3 };
    enum Error : int {
        kErrNone = 0,
        kErrNeedCalibration = 1,
        kErrRateMismatch = 2,
        kErrNoCorrelation = 3,
        kErrFewPoints = 4,
        kErrDelayTooLarge = 5,
        kErrUnstable = 6,
        kErrGlitch = 7,  // falha de buffer (xrun) durante a calibração
    };
    static constexpr int kMaxHints = 8;
    static constexpr int kSpectrumBins = 513;

    struct Settings {
        int harmonics = 2;
        float convergence = 0.2f;
        float maxOutput = 0.25f;
        float fmin = 25.0f;
        float fmax = 250.0f;
        float minProminenceDb = 10.0f;
        bool autoLatency = true;
        float burstLevel = 0.05f;     // rajada curta ao ligar o ANC
        float calBurstLevel = 0.1f;   // rajada da calibração completa
        float probeLevel = 0.2f;      // senoides da calibração
    };

    struct Status {
        int mode = 0;
        int error = 0;
        int calStage = 0;
        float calProgress = 0.0f;
        int calStep = 0;
        int calSteps = 0;
        float latencyMs = 0.0f;
        float pathGainDb = 0.0f;
        float latencyConfidence = 0.0f;
        bool latencyVerified = false;
        bool toneFound = false;
        float toneHz = 0.0f;
        float toneProminenceDb = 0.0f;
        float toneLevelDb = -120.0f;
        bool locked = false;
        float ancFreq = 0.0f;
        AncController::HarmonicStatus harm[AncController::kMaxHarmonics];
        float micDb = -120.0f;
        float outDb = -120.0f;
        int divergences = 0;
        int limiterHits = 0;
        int discontinuities = 0;
        bool modelValid = false;
        int modelRate = 0;
        int modelPoints = 0;
        int modelSerial = 0;
        int sampleRate = 0;
        float lag = 0.0f;
    };

    void configure(int sampleRate);

    // --- thread de áudio ---
    void render(float* out, int frames);
    void capture(const float* in, int frames);
    // Chamar quando a latência ou o ganho do caminho podem ter mudado: xrun reportado pelo
    // Android, volume alterado, troca de saída de áudio. No ANC, dispara uma nova medição.
    void notifyDiscontinuity();

    // --- thread de trabalho ---
    bool hasOfflineWork() const { return calibrator_.needsOfflineWork(); }
    void doOfflineWork() { calibrator_.doOfflineWork(); }

    // --- controle ---
    void requestMode(Mode m);
    void setSettings(const Settings& s);
    void setHints(const float* freqs, int count);
    bool loadModel(const float* data, int count);
    int exportModel(float* out, int capacity);
    Status status();
    int spectrum(float* estimate, float* mic, int capacity, float* binHz);
    int sampleRate() const { return fs_; }

private:
    enum class State { Monitor, Anc, Calibrate, Latency };
    // Por que a latência está sendo medida (decide se a medição entra na estimativa de deriva).
    enum class Reason { Start, Discontinuity, Divergence, Periodic, Manual };

    void applyPending();
    void handleRequest();
    void beginTransition(State target, bool thenAnc);
    void remeasure(Reason why);
    void enterState(State target, bool thenAnc);
    void startAnc(bool allowMeasure);
    void startLatency(bool thenAnc, float level);
    void checkCalibrator();
    void handleDiscontinuity();
    void monitorAnc();
    void publishStatus();
    void publishSpectrum();
    int runDelay() const;

    int fs_ = 48000;
    int64_t nOut_ = 0;
    int64_t nIn_ = 0;

    AncController controller_;
    Calibrator calibrator_;
    SecondaryPath model_;
    int modelSerial_ = 0;
    Settings settings_;
    float hints_[kMaxHints] = {};
    int numHints_ = 0;

    State state_ = State::Monitor;
    bool thenAnc_ = false;
    bool fadingOut_ = false;
    State fadeTarget_ = State::Monitor;
    bool fadeThenAnc_ = false;
    float masterGain_ = 1.0f;
    float ancGain_ = 0.0f;
    int pendingCalStart_ = 0;  // 1 = completa, 2 = só latência (worker ainda ocupado)
    float pendingLatencyLevel_ = 0.0f;
    int burstAttempts_ = 0;
    int error_ = kErrNone;

    int sessionDelay_ = 0;
    float sessionGain_ = 1.0f;
    bool sessionDelayKnown_ = false;
    bool latencyVerified_ = false;
    float latencyConfidence_ = 0.0f;
    int64_t lastLatencyN_ = -1;
    Reason latencyReason_ = Reason::Start;
    double driftRate_ = 0.0;  // amostras de atraso por amostra (clocks de entrada e saída diferentes)
    int64_t remeasureTimes_[6] = {-1, -1, -1, -1, -1, -1};
    double lagEma_ = 0.0;  // só diagnóstico: saída - entrada (amostras)
    bool lagInit_ = false;
    std::atomic<int> discRequests_{0};
    int discHandled_ = 0;
    int discontinuities_ = 0;

    int divergenceSeen_ = 0;
    int64_t divergenceTimes_[3] = {-1, -1, -1};
    bool unstableRetried_ = false;

    DcBlocker dc_;
    Decimator decimEstimate_, decimMic_;
    SpectrumAnalyzer specEstimate_, specMic_;
    std::vector<float> scratchE_, scratchD_, scratchOut_, scratchGain_, scratchFind_;
    TonePeak lastTone_;
    double micEnergy_ = 0.0, outEnergy_ = 0.0;
    int64_t micCount_ = 0, outCount_ = 0;
    float micDb_ = -120.0f, outDb_ = -120.0f;
    int sinceStatus_ = 0;

    // Troca com outras threads: o lado de áudio só usa try_lock.
    std::mutex shareMutex_;
    Status sharedStatus_;
    std::vector<float> sharedSpecEstimate_, sharedSpecMic_;
    float sharedBinHz_ = 1.0f;
    int sharedBins_ = 0;

    std::mutex pendingMutex_;
    std::atomic<bool> settingsDirty_{false};
    Settings pendingSettings_;
    std::atomic<bool> hintsDirty_{false};
    float pendingHints_[kMaxHints] = {};
    int pendingNumHints_ = 0;
    std::atomic<bool> modelDirty_{false};
    SecondaryPath pendingModel_;
    std::mutex exportMutex_;
    SecondaryPath exportedModel_;

    std::atomic<int> requestedMode_{0};
    std::atomic<uint32_t> requestSerial_{0};
    uint32_t handledSerial_ = 0;
};

}  // namespace anc
