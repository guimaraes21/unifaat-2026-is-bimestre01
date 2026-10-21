// Utilitários matemáticos compartilhados pelo núcleo de DSP.
// Nada aqui depende do Android: o mesmo código roda no app e no simulador (dsp-test).
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>

namespace anc {

using cfloat = std::complex<float>;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

inline double fracCycles(double c) { return c - std::floor(c); }

// Ângulo em (-pi, pi].
inline float wrapPi(float a) { return std::remainder(a, kTwoPi); }

inline float ampToDb(float a) { return 20.0f * std::log10(std::max(a, 1e-9f)); }

inline int nextPow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

namespace detail {
constexpr int kSinTableSize = 4096;
extern float gSinTable[kSinTableSize + 1];
}  // namespace detail

// e^{j*2*pi*cycles} por tabela com interpolação linear (erro < 3e-7).
// Usado no laço por amostra, onde sin/cos da libm custariam caro em CPUs fracas de multimídia.
inline cfloat phasor(float cycles) {
    using detail::gSinTable;
    using detail::kSinTableSize;
    float c = cycles - std::floor(cycles);
    float x = c * static_cast<float>(kSinTableSize);
    int i = static_cast<int>(x);
    float fr = x - static_cast<float>(i);
    if (i >= kSinTableSize) {
        i = 0;
        fr = 0.0f;
    }
    const float s = gSinTable[i] + fr * (gSinTable[i + 1] - gSinTable[i]);
    const int j = (i + kSinTableSize / 4) & (kSinTableSize - 1);
    const float co = gSinTable[j] + fr * (gSinTable[j + 1] - gSinTable[j]);
    return {co, s};
}

// Gerador pseudoaleatório xorshift32: determinístico e sem alocação (seguro na thread de áudio).
class Random {
public:
    explicit Random(uint32_t seed = 0x12345678u) : state_(seed ? seed : 1u) {}
    void seed(uint32_t s) { state_ = s ? s : 1u; }
    uint32_t next() {
        uint32_t x = state_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state_ = x;
        return x;
    }
    // Uniforme em [-1, 1).
    float uniform() { return static_cast<float>(next() >> 8) * (2.0f / 16777216.0f) - 1.0f; }

private:
    uint32_t state_;
};

}  // namespace anc
