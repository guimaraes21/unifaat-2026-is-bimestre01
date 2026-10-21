// Controlador de cancelamento ativo de banda estreita (FxLMS por harmônica).
//
// O ronco do motor é periódico: em vez de "inverter o microfone" (impossível com a latência
// do Android), o app sintetiza senoides nas ordens do motor e ajusta amplitude/fase de cada uma
// com o algoritmo FxLMS para minimizar o que o microfone escuta. A latência entra no modelo
// do caminho secundário como um atraso conhecido; como o sinal é previsível, ela não impede
// o cancelamento, apenas deixa a adaptação mais lenta.
#pragma once

#include <vector>

#include "DspCommon.h"
#include "SecondaryPath.h"
#include "ToneTracker.h"

namespace anc {

class AncController {
public:
    static constexpr int kMaxHarmonics = 4;
    static constexpr int kBlock = 256;

    struct Params {
        int harmonics = 2;          // fundamental + harmônicas
        float convergence = 0.2f;   // ganho de malha x atraso (estável abaixo de ~1,5)
        float maxOutput = 0.25f;    // soma das amplitudes geradas (fundo de escala = 1)
        float fmax = 250.0f;
        float minPathDb = -25.0f;   // ignora frequências que o alto-falante quase não reproduz
    };

    struct HarmonicStatus {
        bool active = false;
        float freq = 0.0f;
        float errorAmp = 0.0f;        // o que sobra no microfone
        float disturbanceAmp = 0.0f;  // estimativa do ruído sem o ANC
        float outputAmp = 0.0f;       // amplitude gerada
        float reductionDb = 0.0f;
        bool verified = false;        // modelo conferido em operação (regressão erro x anti-onda)
        bool pll = false;             // PLL fina ativa (só na fundamental)
        float corrDb = 0.0f;          // correção de ganho aplicada ao modelo nesta frequência
        float corrDeg = 0.0f;         // correção de fase aplicada ao modelo nesta frequência
    };

    void configure(int sampleRate, int historySize);
    void setModel(const SecondaryPath* model);
    void setParams(const Params& p);
    void setDelay(int samples);
    int delay() const { return delay_; }
    // Correção de ganho do caminho medida na sessão (volume diferente do da calibração).
    void setPathGain(float g);

    // Liga/desliga a adaptação (modo ANC). Ao ligar, zera os pesos.
    void setAdapting(bool on);
    bool adapting() const { return adapting_; }

    // Gera a anti-onda para as amostras [n0, n0+frames) e registra o histórico da referência.
    void render(int64_t n0, float* out, int frames, const float* gain);
    // Processa o microfone [m0, m0+frames). Escreve em `dhat` a estimativa do ruído original
    // (microfone menos a contribuição prevista da anti-onda), usada para rastrear a frequência.
    void capture(int64_t m0, const float* e, float* dhat, int frames);
    // Nova estimativa do rastreador de tom (a cada ~0,25 s).
    void onTone(const TonePeak& tone);

    bool locked() const { return locked_; }
    float frequency() const { return freq_; }
    int divergences() const { return divergences_; }
    int limiterHits() const { return limiterHits_; }
    HarmonicStatus harmonic(int k) const;

private:
    enum class VerifyOutcome { Accepted, Inconclusive, Rejected };

    void blockUpdate();
    void updateVerification(int k, float dt);
    VerifyOutcome finishVerification(int k);
    float verifyWindow(int k) const;
    void updatePll(float dt);
    void resetPll();
    void refreshPath();
    // Zera a harmônica e a mantém em silêncio por `restSec` (mede a linha de base sem anti-onda).
    void resetHarmonic(int k, float restSec);
    void resetAll();

    int fs_ = 48000;
    int histSize_ = 0;
    int64_t histMask_ = 0;
    std::vector<float> phaseRing_;
    std::vector<cfloat> zRing_[kMaxHarmonics];
    int64_t renderedUpTo_ = 0;

    const SecondaryPath* model_ = nullptr;
    float modelMaxMag_ = 0.0f;
    float pathGain_ = 1.0f;
    Params params_;
    int delay_ = 0;
    bool adapting_ = false;

    // Oscilador de referência (fase em ciclos)
    double phase_ = 0.0;
    float freq_ = 50.0f;
    bool locked_ = false;
    int lost_ = 0;
    bool jumpPending_ = false;
    float jumpFreq_ = 0.0f;

    // PLL de 2ª ordem que mantém a fase do oscilador presa à do ronco estimado.
    // Liberada ao fim da primeira janela de verificação: se o modelo foi confirmado, ou se a
    // janela foi inconclusiva (rotação variando) e o modelo ainda merece confiança (a rajada
    // acabou de conferir latência e ganho, e não houve divergência desde então).
    bool pllAllowed_ = false;
    bool modelTrusted_ = true;
    bool pllActive_ = false;
    float pllAge_ = 0.0f;
    float pllRefArg_ = 0.0f;
    float pllInteg_ = 0.0f;
    float pllCenter_ = 0.0f;
    int disagree_ = 0;

    // Estado por harmônica
    cfloat w_[kMaxHarmonics];
    cfloat h0_[kMaxHarmonics];
    float mu_[kMaxHarmonics] = {};
    bool active_[kMaxHarmonics] = {};
    bool track_[kMaxHarmonics] = {};
    float alphaScale_[kMaxHarmonics] = {1, 1, 1, 1};
    cfloat eLp_[kMaxHarmonics];
    cfloat dLp_[kMaxHarmonics];
    cfloat wLp_[kMaxHarmonics];   // anti-onda que chega ao microfone, em unidades do modelo

    // Verificação do modelo: durante a convergência a anti-onda varia bastante, então
    // E = D0 + D1*t + H*W (mínimos quadrados) revela o H verdadeiro nesta frequência.
    struct Fit {
        int n = 0;
        std::complex<double> a[3][3];
        std::complex<double> b[3];
        double ee = 0.0;
    };
    Fit fit_[kMaxHarmonics];
    float verifyAge_[kMaxHarmonics] = {};
    int verifyTick_ = 0;
    bool verified_[kMaxHarmonics] = {};
    bool verifyDone_[kMaxHarmonics] = {};  // uma tentativa por convergência (depois W quase não varia)
    float verifyFreqMin_[kMaxHarmonics] = {};  // faixa percorrida pelo oscilador na janela
    float verifyFreqMax_[kMaxHarmonics] = {};
    cfloat corr_[kMaxHarmonics];
    float eSlow_[kMaxHarmonics] = {};
    float dSlow_[kMaxHarmonics] = {};
    float age_[kMaxHarmonics] = {};
    float rest_[kMaxHarmonics] = {};
    float baseline_[kMaxHarmonics] = {};
    float badTime_[kMaxHarmonics] = {};

    float lpA_ = 0.0f;
    float leakKeep_ = 1.0f;
    float mu0_ = 0.0f;
    int blockCount_ = 0;
    int divergences_ = 0;
    int limiterHits_ = 0;
    int corrections_ = 0;

public:
    int corrections() const { return corrections_; }
};

}  // namespace anc
