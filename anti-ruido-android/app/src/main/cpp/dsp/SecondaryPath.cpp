#include "SecondaryPath.h"

namespace anc {

void SecondaryPath::clear() {
    sampleRate = 0;
    delayRef = 0;
    numPoints = 0;
    valid = false;
    for (auto& p : points) p = PathPoint();
    std::fill(std::begin(ir), std::end(ir), 0.0f);
    std::fill(std::begin(bandMag), std::end(bandMag), 0.0f);
}

int SecondaryPath::validPoints() const {
    int n = 0;
    for (int i = 0; i < numPoints; ++i) n += points[i].valid ? 1 : 0;
    return n;
}

float SecondaryPath::maxMagnitude() const {
    float m = 0.0f;
    for (int i = 0; i < numPoints; ++i) {
        if (points[i].valid) m = std::max(m, std::abs(points[i].h0));
    }
    return m;
}

bool SecondaryPath::lookup(float f, cfloat& h0) const {
    if (!valid || numPoints == 0 || f <= 0.0f) return false;
    // Ponto válido imediatamente abaixo e acima de f (a grade é crescente).
    int lo = -1, hi = -1;
    for (int i = 0; i < numPoints; ++i) {
        if (!points[i].valid) continue;
        if (points[i].freq <= f) lo = i;
        if (points[i].freq >= f) {
            hi = i;
            break;
        }
    }
    if (lo < 0 || hi < 0) return false;
    if (lo == hi) {
        h0 = points[lo].h0;
        return true;
    }
    const float flo = points[lo].freq, fhi = points[hi].freq;
    // Buraco grande na grade (pontos inválidos no meio): não confiar na interpolação.
    if (fhi / flo > 1.35f) return false;
    const float t = std::log(f / flo) / std::log(fhi / flo);
    const float mlo = std::abs(points[lo].h0), mhi = std::abs(points[hi].h0);
    const float plo = std::arg(points[lo].h0);
    const float dp = wrapPi(std::arg(points[hi].h0) - plo);
    const float mag = std::exp((1.0f - t) * std::log(std::max(mlo, 1e-9f)) +
                               t * std::log(std::max(mhi, 1e-9f)));
    h0 = std::polar(mag, plo + t * dp);
    return true;
}

// Formato: [versão, fs, delayRef, numPontos, (f, re, im, snr, válido) * n, ir * kIrLen, bandMag * kBands]
int SecondaryPath::serializedSize() const { return 4 + numPoints * 5 + kIrLen + kBands; }

int SecondaryPath::serialize(float* out, int capacity) const {
    const int need = serializedSize();
    if (!valid || capacity < need) return 0;
    int k = 0;
    out[k++] = static_cast<float>(kFormatVersion);
    out[k++] = static_cast<float>(sampleRate);
    out[k++] = static_cast<float>(delayRef);
    out[k++] = static_cast<float>(numPoints);
    for (int i = 0; i < numPoints; ++i) {
        out[k++] = points[i].freq;
        out[k++] = points[i].h0.real();
        out[k++] = points[i].h0.imag();
        out[k++] = points[i].snrDb;
        out[k++] = points[i].valid ? 1.0f : 0.0f;
    }
    for (int i = 0; i < kIrLen; ++i) out[k++] = ir[i];
    for (int i = 0; i < kBands; ++i) out[k++] = bandMag[i];
    return k;
}

bool SecondaryPath::deserialize(const float* in, int count) {
    clear();
    if (count < 4 || static_cast<int>(in[0]) != kFormatVersion) return false;
    const int n = static_cast<int>(in[3]);
    if (n < 0 || n > kMaxPoints || count < 4 + n * 5 + kIrLen + kBands) return false;
    sampleRate = static_cast<int>(in[1]);
    delayRef = static_cast<int>(in[2]);
    numPoints = n;
    int k = 4;
    for (int i = 0; i < n; ++i) {
        points[i].freq = in[k++];
        const float re = in[k++];
        const float im = in[k++];
        points[i].h0 = cfloat(re, im);
        points[i].snrDb = in[k++];
        points[i].valid = in[k++] > 0.5f;
    }
    for (int i = 0; i < kIrLen; ++i) ir[i] = in[k++];
    for (int i = 0; i < kBands; ++i) bandMag[i] = in[k++];
    valid = sampleRate > 0 && delayRef > 0 && validPoints() > 0;
    return valid;
}

}  // namespace anc
