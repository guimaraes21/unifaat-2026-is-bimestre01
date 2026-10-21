// Calibração do caminho secundário em duas etapas:
//  1) Rajada de ruído branco + correlação cruzada GCC-PHAT -> atraso total (latência de ida e volta).
//  2) Varredura de senoides em degraus + detecção síncrona (lock-in) -> H0(f) numa grade logarítmica.
//  3) Segunda rajada: se o atraso mudou durante a varredura, há deriva de clock entre entrada e
//     saída (ou um xrun). A deriva é estimada e as fases da varredura são corrigidas.
// O modo "LatencyOnly" repete só a etapa 1 e compara com a impressão digital salva,
// porque a latência do Android muda a cada vez que os streams de áudio são abertos.
#pragma once

#include <atomic>
#include <vector>

#include "DspCommon.h"
#include "SecondaryPath.h"

namespace anc {

class Calibrator {
public:
    enum class Kind { Full, LatencyOnly };
    enum class Stage : int { Idle = 0, Burst = 1, Analyze = 2, Sweep = 3, Done = 4, Failed = 5 };
    enum Error : int { kOk = 0, kErrNoCorrelation = 1, kErrFewPoints = 2, kErrDelayTooLarge = 3, kErrGlitch = 4 };

    struct Settings {
        float burstLevel = 0.1f;
        float probeLevel = 0.2f;
        float fStart = 20.0f;
        float fEnd = 320.0f;
        int pointsPerOctave = 16;
        float settleSec = 0.15f;
        float minSnrDb = 6.0f;
    };

    // Aloca buffers (chamar fora da thread de áudio).
    void configure(int sampleRate, float maxDelaySec);
    int maxDelaySamples() const { return maxDelay_; }

    // --- thread de áudio ---
    // Retorna false se o worker ainda estiver ocupado com uma análise anterior.
    bool start(Kind kind, const Settings& settings, const SecondaryPath* reference, int64_t nowOut,
               int64_t nowIn);
    void abort();
    void render(int64_t n0, float* out, int frames);
    void capture(int64_t m0, const float* e, int frames);

    // --- thread de trabalho (cálculo pesado da correlação) ---
    bool needsOfflineWork() const { return stage_.load(std::memory_order_acquire) == Stage::Analyze; }
    void doOfflineWork();

    // --- leitura do resultado (thread de áudio, após Done) ---
    Stage stage() const { return stage_.load(std::memory_order_acquire); }
    Kind kind() const { return kind_; }
    int error() const { return error_; }
    float progress() const;
    int sweepStep() const { return step_; }
    int sweepSteps() const { return numSteps_; }
    const SecondaryPath& model() const { return model_; }
    int sessionDelay() const { return sessionDelay_; }
    // Ganho do caminho agora / ganho na calibração (1 = volume igual ao da calibração).
    float sessionGain() const { return sessionGain_; }
    // Índice de saída a que sessionDelay() se refere (meio da última rajada).
    int64_t measuredAt() const { return burstStart_ + burstLen_ / 2; }
    // Deriva do atraso (amostras por amostra) estimada entre as duas rajadas da calibração.
    double driftRate() const { return driftRate_; }
    float confidence() const { return confidence_; }

private:
    void beginStep(int64_t n);
    void finishStep();
    void computeCorrelation();
    void bandResponse(int delay, float* mag) const;
    bool matchFingerprint(const SecondaryPath& ref, int& center, double& ncc) const;
    void scheduleBurst(int64_t now);

    int fs_ = 48000;
    int burstLen_ = 0;
    int maxDelay_ = 0;
    int recordLen_ = 0;
    int fftSize_ = 0;
    std::vector<float> burst_;
    std::vector<float> record_;
    std::vector<float> corr_;
    std::vector<cfloat> fa_, fb_;
    std::vector<float> xpow_;

    std::atomic<Stage> stage_{Stage::Idle};
    std::atomic<bool> workerBusy_{false};
    Kind kind_ = Kind::Full;
    Settings settings_;
    SecondaryPath reference_;
    SecondaryPath model_;
    int error_ = kOk;
    float confidence_ = 0.0f;
    int sessionDelay_ = 0;
    float sessionGain_ = 1.0f;
    double driftRate_ = 0.0;
    bool secondBurst_ = false;
    int64_t firstBurstStart_ = 0;
    int64_t lastInput_ = 0;
    int64_t sweepEndN_ = -1;

    int64_t burstStart_ = 0;
    int64_t recorded_ = 0;

    // Varredura
    float grid_[SecondaryPath::kMaxPoints] = {};
    int numSteps_ = 0;
    int step_ = 0;
    bool stepPending_ = false;
    bool sweepStarted_ = false;
    double phase_ = 0.0;
    int64_t stepStartN_ = 0;
    double stepPhase0_ = 0.0;
    float stepFreq_ = 0.0f;
    int64_t winStart_ = 0;
    int64_t winLen_ = 0;
    double accRe_ = 0.0, accIm_ = 0.0, energy_ = 0.0;
    int64_t count_ = 0;
    int64_t stepCenter_[SecondaryPath::kMaxPoints] = {};
    int64_t fadeInStart_ = 0;
};

}  // namespace anc
