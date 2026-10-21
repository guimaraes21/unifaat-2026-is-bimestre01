// Modelo do "caminho secundário": tudo que existe entre a amostra que o app escreve
// e a amostra que ele lê do microfone (mixer do Android, DSP/amplificador da multimídia,
// alto-falantes, acústica da cabine, microfone, ADC e buffers).
//
// H(f) = H0(f) * exp(-j*2*pi*f*delayRef/fs)
//
// O atraso "bruto" (delayRef, dezenas a centenas de ms no Android) é separado de H0,
// que varia devagar com a frequência e por isso pode ser medido numa grade esparsa e interpolado.
#pragma once

#include "DspCommon.h"

namespace anc {

struct PathPoint {
    float freq = 0.0f;
    cfloat h0{0.0f, 0.0f};
    float snrDb = 0.0f;
    bool valid = false;
};

class SecondaryPath {
public:
    static constexpr int kMaxPoints = 96;
    // Trecho da correlação cruzada (GCC-PHAT) em volta do pico, guardado como
    // "impressão digital" para re-medir só o atraso a cada sessão.
    static constexpr int kIrHalf = 1024;
    static constexpr int kIrLen = 2 * kIrHalf + 1;
    // Ganho medido pela rajada em bandas de 1/3 de oitava (100-800 Hz): permite detectar
    // mudança de volume a cada nova medição de latência.
    static constexpr int kBands = 10;
    static constexpr int kFormatVersion = 1;

    int sampleRate = 0;
    int delayRef = 0;
    int numPoints = 0;
    PathPoint points[kMaxPoints];
    float ir[kIrLen] = {};
    float bandMag[kBands] = {};
    bool valid = false;

    void clear();
    int validPoints() const;
    float maxMagnitude() const;
    // H0 interpolado em f. Retorna false fora da faixa medida ou sem pontos válidos próximos.
    bool lookup(float f, cfloat& h0) const;
    static float bandCenter(int b) { return 100.0f * std::pow(2.0f, static_cast<float>(b) / 3.0f); }

    int serializedSize() const;
    int serialize(float* out, int capacity) const;
    bool deserialize(const float* in, int count);
};

}  // namespace anc
