// Forced into the game's sources by scripts/build.py: their globals go to .gdata / .gbss,
// which a rollback restores.
#pragma once

#include "GameHeap.hpp"

#pragma data_seg(".gdata")
#pragma bss_seg(".gbss")
