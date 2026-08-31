#pragma once
#include <cstddef>
#include <cstdint>
#include "RollbackMemory.h"

namespace th07 { namespace rollback { namespace heap {

// Inside a SimulationScope (no RuntimeScope) GameAlloc uses the arena, elsewhere the CRT; frees
// go by address. Freeing a CRT block inside a simulation scope latches a fault, after which
// Capture and Restore refuse.
void* GameAlloc(std::size_t bytes);
void GameFree(void* block);
void* GameRealloc(void* block, std::size_t bytes);
std::size_t GameAllocSize(void* block);

// Install before creating any simulation object.
bool Install(Memory& memory);
bool Uninstall();
Memory* Installed();
const char* LastError();
bool Faulted();
bool InSimulationScope();

class SimulationScope {
    bool entered_;
public:
    SimulationScope();
    ~SimulationScope();
    SimulationScope(const SimulationScope&) = delete;
    SimulationScope& operator=(const SimulationScope&) = delete;
};
// Also keeps the FPU state: platform code may change it.
class RuntimeScope {
    unsigned x87_;
    unsigned sse_;
public:
    RuntimeScope();
    ~RuntimeScope();
    RuntimeScope(const RuntimeScope&) = delete;
    RuntimeScope& operator=(const RuntimeScope&) = delete;
};

// Refused while any simulation scope runs, and after an ownership fault.
bool Capture(std::uint64_t frame, Memory::Snapshot& out, bool inspectEveryPage = false);
bool Restore(const Memory::Snapshot& snapshot);

} } }

using th07::rollback::heap::GameAlloc;
using th07::rollback::heap::GameFree;
using th07::rollback::heap::GameRealloc;
using th07::rollback::heap::GameAllocSize;
