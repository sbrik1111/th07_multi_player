// Game allocations go to the rollback arena inside a SimulationScope.
#pragma once

#include <stddef.h>

namespace th07
{
namespace rollback
{
namespace heap
{
void *GameAlloc(size_t bytes);
void GameFree(void *block);
void *GameRealloc(void *block, size_t bytes);
size_t GameAllocSize(void *block);
}
}
}
using th07::rollback::heap::GameAlloc;
using th07::rollback::heap::GameAllocSize;
using th07::rollback::heap::GameFree;
using th07::rollback::heap::GameRealloc;

// Never freed.
void *GameStaticBlock(size_t bytes);
