#include "DspCommon.h"

namespace anc {
namespace detail {

float gSinTable[kSinTableSize + 1];

namespace {
// Preenche a tabela no carregamento da biblioteca, nunca na thread de áudio.
struct SinTableInit {
    SinTableInit() {
        for (int i = 0; i <= kSinTableSize; ++i) {
            gSinTable[i] = static_cast<float>(
                std::sin(2.0 * 3.14159265358979323846 * i / kSinTableSize));
        }
    }
} gSinTableInit;
}  // namespace

}  // namespace detail
}  // namespace anc
