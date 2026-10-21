// Análise espectral do sinal decimado (~1 kHz) e detecção do "ronco" tonal do motor.
#pragma once

#include <vector>

#include "DspCommon.h"
#include "Fft.h"

namespace anc {

// Janela deslizante + FFT com janela de Hann. Produz o espectro de amplitude
// (uma senoide de amplitude A aparece com valor ~A no pico).
class SpectrumAnalyzer {
public:
    void configure(float rate, int fftSize, int hop);
    void reset();
    // Empurra uma amostra; retorna true quando um novo espectro foi calculado.
    bool push(float x);

    float rate() const { return rate_; }
    int fftSize() const { return n_; }
    int bins() const { return n_ / 2 + 1; }
    float binHz() const { return rate_ / static_cast<float>(n_); }
    const float* amplitude() const { return amp_.data(); }

private:
    float rate_ = 1000.0f;
    int n_ = 1024;
    int hop_ = 256;
    int writePos_ = 0;
    int filled_ = 0;
    int sinceLast_ = 0;
    Fft fft_;
    std::vector<float> ring_;
    std::vector<float> window_;
    std::vector<cfloat> work_;
    std::vector<float> amp_;
};

struct TonePeak {
    bool found = false;
    float freq = 0.0f;        // Hz
    float amplitude = 0.0f;   // amplitude linear (escala do sinal)
    float prominenceDb = 0.0f;  // pico acima da mediana do espectro na faixa
};

struct ToneSearch {
    float fmin = 25.0f;
    float fmax = 250.0f;
    float minProminenceDb = 10.0f;
    // Frequências esperadas (ex.: ordens do motor a partir do RPM do OBD-II).
    // Se houver dicas, o pico só é procurado em janelas de +-hintRelWidth em volta delas.
    const float* hints = nullptr;
    int numHints = 0;
    float hintRelWidth = 0.04f;
    // Se houver um pico relevante em f/2 ou f/3, prefere a fundamental mais baixa
    // (as harmônicas acima dela são tratadas pelo controlador).
    bool preferSubharmonic = true;
};

// `scratch` deve ter pelo menos `bins` posições (evita alocação na thread de áudio).
TonePeak findTone(const float* amp, int bins, float binHz, const ToneSearch& search,
                  float* scratch);

}  // namespace anc
