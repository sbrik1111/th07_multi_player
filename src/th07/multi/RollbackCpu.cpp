#include "RollbackCpu.h"
#include <float.h>
#include <xmmintrin.h>
#include <cstring>

namespace th07 { namespace rollback {

bool CpuState::Capture() {
    if (fegetenv(&environment)) return false;
    __control87_2(0, 0, &x87, nullptr);
    mxcsr = _mm_getcsr();
    return true;
}
bool CpuState::Restore() const {
    if (fesetenv(&environment)) return false;
    unsigned unused;
    __control87_2(x87, _MCW_EM | _MCW_RC | _MCW_PC | _MCW_IC, &unused, nullptr);
    _mm_setcsr(mxcsr);
    return true;
}
bool CpuState::operator==(const CpuState& other) const {
    return x87 == other.x87 && mxcsr == other.mxcsr
        && std::memcmp(&environment, &other.environment, sizeof(environment)) == 0;
}
std::uint64_t CpuState::FoldHash(std::uint64_t h) const {
    const unsigned words[] = {environment._Fe_ctl, environment._Fe_stat, x87, mxcsr};
    for (unsigned w = 0; w != sizeof(words) / sizeof(words[0]); ++w)
        for (unsigned shift = 0; shift != 32; shift += 8)
            h = (h ^ static_cast<unsigned char>(words[w] >> shift)) * 1099511628211ull;
    return h;
}

} }
