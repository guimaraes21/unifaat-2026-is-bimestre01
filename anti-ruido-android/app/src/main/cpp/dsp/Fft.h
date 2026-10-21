#pragma once

#include <vector>

#include "DspCommon.h"

namespace anc {

// FFT complexa radix-2 iterativa. O construtor aloca (chamar fora da thread de áudio);
// forward/inverse não alocam.
class Fft {
public:
    explicit Fft(int n = 0) { resize(n); }
    void resize(int n);
    int size() const { return n_; }
    void forward(cfloat* data) const { transform(data, false); }
    // Inversa normalizada (divide por N).
    void inverse(cfloat* data) const { transform(data, true); }

private:
    void transform(cfloat* data, bool inverse) const;

    int n_ = 0;
    std::vector<cfloat> twiddle_;
    std::vector<int> bitrev_;
};

}  // namespace anc
