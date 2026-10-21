// Filtros IIR simples usados para remover DC e para decimar o sinal do microfone.
#pragma once

#include "DspCommon.h"

namespace anc {

// Biquad em forma direta II transposta (coeficientes RBJ "Audio EQ Cookbook").
struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void setLowpass(float fs, float fc, float q) {
        const float w0 = kTwoPi * fc / fs;
        const float cw = std::cos(w0);
        const float alpha = std::sin(w0) / (2.0f * q);
        const float a0 = 1.0f + alpha;
        b0 = (1.0f - cw) * 0.5f / a0;
        b1 = (1.0f - cw) / a0;
        b2 = b0;
        a1 = -2.0f * cw / a0;
        a2 = (1.0f - alpha) / a0;
    }

    void setHighpass(float fs, float fc, float q) {
        const float w0 = kTwoPi * fc / fs;
        const float cw = std::cos(w0);
        const float alpha = std::sin(w0) / (2.0f * q);
        const float a0 = 1.0f + alpha;
        b0 = (1.0f + cw) * 0.5f / a0;
        b1 = -(1.0f + cw) / a0;
        b2 = b0;
        a1 = -2.0f * cw / a0;
        a2 = (1.0f - alpha) / a0;
    }

    // Filtro de pico (boost/corte em torno de fc), útil para simular modos acústicos da cabine.
    void setPeaking(float fs, float fc, float q, float gainDb) {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = kTwoPi * fc / fs;
        const float cw = std::cos(w0);
        const float alpha = std::sin(w0) / (2.0f * q);
        const float a0 = 1.0f + alpha / A;
        b0 = (1.0f + alpha * A) / a0;
        b1 = -2.0f * cw / a0;
        b2 = (1.0f - alpha * A) / a0;
        a1 = -2.0f * cw / a0;
        a2 = (1.0f - alpha / A) / a0;
    }

    void reset() { z1 = z2 = 0.0f; }

    float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    // Resposta complexa em f (para testes).
    cfloat response(float fs, float f) const {
        const cfloat z1c = std::polar(1.0f, -kTwoPi * f / fs);
        const cfloat z2c = z1c * z1c;
        return (b0 + b1 * z1c + b2 * z2c) / (1.0f + a1 * z1c + a2 * z2c);
    }
};

// Bloqueador de DC de 1ª ordem (corte ~5 Hz): fase desprezível na faixa de 20-300 Hz.
class DcBlocker {
public:
    void configure(float fs, float fc = 5.0f) {
        r_ = 1.0f - kTwoPi * fc / fs;
        reset();
    }
    void reset() { x1_ = y1_ = 0.0f; }
    float process(float x) {
        const float y = x - x1_ + r_ * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }

private:
    float r_ = 0.999f;
    float x1_ = 0.0f, y1_ = 0.0f;
};

// Passa-baixas Butterworth de 6ª ordem + subamostragem por um fator inteiro.
class Decimator {
public:
    void configure(float fs, int factor, float cutoff) {
        factor_ = std::max(1, factor);
        static const float kQ[3] = {0.51763809f, 0.70710678f, 1.93185165f};
        for (int i = 0; i < 3; ++i) {
            stages_[i].setLowpass(fs, cutoff, kQ[i]);
            stages_[i].reset();
        }
        counter_ = 0;
    }

    // Retorna true quando uma nova amostra decimada foi gerada em `out`.
    bool process(float x, float& out) {
        for (auto& s : stages_) x = s.process(x);
        if (++counter_ >= factor_) {
            counter_ = 0;
            out = x;
            return true;
        }
        return false;
    }

private:
    Biquad stages_[3];
    int factor_ = 1;
    int counter_ = 0;
};

}  // namespace anc
