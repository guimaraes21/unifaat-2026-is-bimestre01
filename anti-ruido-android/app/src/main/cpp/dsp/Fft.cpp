#include "Fft.h"

namespace anc {

void Fft::resize(int n) {
    n_ = n;
    twiddle_.assign(n > 1 ? n / 2 : 0, cfloat(1.0f, 0.0f));
    bitrev_.assign(n, 0);
    if (n <= 1) return;
    for (int k = 0; k < n / 2; ++k) {
        const double a = -2.0 * 3.14159265358979323846 * k / n;
        twiddle_[k] = cfloat(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    for (int i = 0; i < n; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) {
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        }
        bitrev_[i] = r;
    }
}

void Fft::transform(cfloat* x, bool inverse) const {
    const int n = n_;
    if (n <= 1) return;
    for (int i = 0; i < n; ++i) {
        const int j = bitrev_[i];
        if (j > i) std::swap(x[i], x[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < half; ++j) {
                cfloat w = twiddle_[j * step];
                if (inverse) w = std::conj(w);
                const cfloat u = x[i + j];
                const cfloat v = x[i + j + half] * w;
                x[i + j] = u + v;
                x[i + j + half] = u - v;
            }
        }
    }
    if (inverse) {
        const float s = 1.0f / static_cast<float>(n);
        for (int i = 0; i < n; ++i) x[i] *= s;
    }
}

}  // namespace anc
