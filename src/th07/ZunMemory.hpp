#pragma once

#include <stdlib.h>

#include "GameHeap.hpp"

// the th08 decompilation has a ZunMemory class
// (https://github.com/GensokyoClub/th08/blob/main/src/Global.hpp)
// that was most likely used in debug to track allocations and frees,
// and to track and prevent memory leaks.
// a similar, smaller system was most likely used in th07 judging by the
// parameter being copied into an inline temporary before mallocs and frees.
// though evidently it wasn't used very consistently.
// it's also basically completely empty since presumably it's only enabled
// on debug builds

namespace ZunMemory
{
inline void Free(void *p)
{
    GameFree(p);
}

inline void *Alloc(size_t size)
{
    return GameAlloc(size);
}

// sometimes using zunmemory::alloc just doesnt work since the parameter isn't
// copied into a temporary properly so this just forces that copy
inline void *Alloc2(size_t size)
{
    size_t tmp = size;
    return GameAlloc(tmp);
}
} // namespace ZunMemory
