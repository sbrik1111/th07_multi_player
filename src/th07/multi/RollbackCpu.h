#pragma once
#include <fenv.h>
#include <cstdint>

namespace th07 { namespace rollback {

// Capture at a frame boundary: the x87 stack is empty.
struct CpuState {
    fenv_t environment;
    unsigned x87;
    unsigned mxcsr;
    CpuState() : environment(), x87(0), mxcsr(0) {}
    bool Capture();
    bool Restore() const;
    bool operator==(const CpuState& other) const;
    std::uint64_t FoldHash(std::uint64_t hash) const;
};

} }
