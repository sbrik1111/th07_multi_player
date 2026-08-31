#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <malloc.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <locale>
#include "RollbackHeap.h"
#include <float.h>

namespace th07 { namespace rollback { namespace heap {
namespace {
// No dynamic initializers: operator new may run before this file's static initialization.
Memory* volatile memory = nullptr;
CRITICAL_SECTION guard;
bool guardReady = false;
unsigned activeScopes = 0;
thread_local unsigned simulationDepth = 0, runtimeDepth = 0;
thread_local bool runtimePrepared = false;
const char* error = "";
bool faulted = false;

class Lock {
public:
    Lock() { EnterCriticalSection(&guard); }
    ~Lock() { LeaveCriticalSection(&guard); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};
bool Fail(const char* message) { error = message; return false; }
// Called with the guard held.
bool Fault(const char* message, const void* pointer = nullptr) {
    if (faulted) return false;
    faulted = true;
    RuntimeScope runtime;
    char line[1024];
    void* trace[32];
    const unsigned count = CaptureStackBackTrace(0, 32, trace, nullptr);
    int used = std::snprintf(line, sizeof(line), "[th07 rollback heap] FAULT %s pointer=%p stack=", message, pointer);
    for (unsigned i = 0; i < count && used < 960; ++i)
        used += std::snprintf(line + used, sizeof(line) - used, "%p,", trace[i]);
    std::snprintf(line + used, sizeof(line) - used, "\n");
    OutputDebugStringA(line);
    return Fail(message);
}
bool Simulating() { return simulationDepth != 0 && runtimeDepth == 0; }
}

void* GameAlloc(std::size_t bytes) {
    Memory* const arena = memory;
    if (!arena || !Simulating()) return std::malloc(bytes);
    void* p;
    {
        Lock lock;
        p = arena->Allocate(bytes);
        if (!p) Fail(arena->LastError());
    }
    if (!p) errno = ENOMEM;
    return p;
}

void GameFree(void* block) {
    if (!block) return;
    Memory* const arena = memory;
    if (arena) {
        if (arena->Owns(block)) {
            Lock lock;
            if (!arena->Free(block)) Fault("invalid simulation heap free", block);
            return;
        }
        if (Simulating()) {
            Lock lock;
            Fault("freeing an untracked allocation in simulation scope", block);
        }
    }
    std::free(block);
}

void* GameRealloc(void* block, std::size_t bytes) {
    if (!block) return GameAlloc(bytes);
    Memory* const arena = memory;
    if (arena) {
        if (arena->Owns(block)) {
            void* result;
            {
                Lock lock;
                if (!arena->AllocationSize(block)) {
                    Fault("invalid simulation heap realloc", block);
                    return nullptr;
                }
                result = arena->Reallocate(block, bytes);
                if (!result && bytes) Fail(arena->LastError());
            }
            if (!result && bytes) errno = ENOMEM;
            return result;
        }
        if (Simulating()) {
            Lock lock;
            Fault("reallocating an untracked allocation in simulation scope", block);
        }
    }
    return std::realloc(block, bytes);
}

std::size_t GameAllocSize(void* block) {
    if (!block) return 0;
    Memory* const arena = memory;
    if (arena && arena->Owns(block)) {
        Lock lock;
        return arena->AllocationSize(block);
    }
    return _msize(block);
}

const char* LastError() { return error; }
bool Faulted() { return faulted; }
Memory* Installed() { return memory; }
bool InSimulationScope() { return Simulating(); }

bool Install(Memory& target) {
    if (memory || !target.Base()) return Fail("invalid or already installed heap domain");
    if (!guardReady) {
        if (!InitializeCriticalSectionEx(&guard, 0, 0)) return Fail("heap domain lock initialization failed");
        guardReady = true;
    }
    target.SetReleaseGuard(&InSimulationScope);
    error = ""; faulted = false;
    MemoryBarrier();
    memory = &target;
    return true;
}

bool Uninstall() {
    if (!memory) return true;
    RuntimeScope runtime;
    Lock lock;
    if (activeScopes || memory->LiveAllocations()) return Fail("heap domain is still in use");
    memory->SetReleaseGuard(nullptr);
    memory = nullptr;
    return true;
}

SimulationScope::SimulationScope() : entered_(false) {
    if (!memory) return;
    if (!runtimePrepared) {
        RuntimeScope runtime;
        // The standard library makes the classic locale lazily with operator new: make it in the CRT
        // before routing this thread's allocations.
        const std::locale& locale = std::locale::classic();
        (void)std::use_facet<std::ctype<char> >(locale);
        (void)std::use_facet<std::ctype<wchar_t> >(locale);
        (void)std::use_facet<std::numpunct<char> >(locale);
        (void)std::use_facet<std::numpunct<wchar_t> >(locale);
        (void)std::use_facet<std::num_get<char> >(locale);
        (void)std::use_facet<std::num_get<wchar_t> >(locale);
        (void)std::use_facet<std::num_put<char> >(locale);
        (void)std::use_facet<std::num_put<wchar_t> >(locale);
        (void)std::use_facet<std::codecvt<char, char, std::mbstate_t> >(locale);
        (void)std::use_facet<std::codecvt<wchar_t, char, std::mbstate_t> >(locale);
        runtimePrepared = true;
    }
    Lock lock;
    ++activeScopes; ++simulationDepth; entered_ = true;
}
SimulationScope::~SimulationScope() {
    if (!entered_) return;
    Lock lock;
    --simulationDepth; --activeScopes;
}
RuntimeScope::RuntimeScope() {
    ++runtimeDepth;
    x87_ = 0;
    sse_ = 0;
    __control87_2(0, 0, &x87_, &sse_);
}
RuntimeScope::~RuntimeScope() {
    --runtimeDepth;
    unsigned x87 = 0, sse = 0;
    __control87_2(0, 0, &x87, &sse);
    if (x87 != x87_) __control87_2(x87_, _MCW_EM | _MCW_RC | _MCW_PC | _MCW_IC, &x87, nullptr);
    if (sse != sse_) __control87_2(sse_, _MCW_EM | _MCW_RC | _MCW_DN, nullptr, &sse);
}

bool Capture(std::uint64_t frame, Memory::Snapshot& out, bool inspectEveryPage) {
    if (!memory) return Fail("heap domain not installed");
    RuntimeScope runtime;
    Lock lock;
    if (faulted) return false;
    if (activeScopes) return Fail("snapshot requested during simulation or loader execution");
    if (!memory->Capture(frame, out, inspectEveryPage)) return Fail(memory->LastError());
    return true;
}
bool Restore(const Memory::Snapshot& snapshot) {
    if (!memory) return Fail("heap domain not installed");
    RuntimeScope runtime;
    Lock lock;
    if (faulted) return false;
    if (activeScopes) return Fail("restore requested during simulation or loader execution");
    if (!memory->Restore(snapshot)) return Fail(memory->LastError());
    return true;
}

} } }
