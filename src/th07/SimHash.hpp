#pragma once

#include "inttypes.hpp"

enum SimHashPart
{
    SIM_HASH_RNG,
    SIM_HASH_GLOBALS,
    SIM_HASH_STOCKS,
    SIM_HASH_PLAYERS,
    SIM_HASH_SHOTS,
    SIM_HASH_BOMBS,
    SIM_HASH_BULLETS,
    SIM_HASH_LASERS,
    SIM_HASH_ENEMIES,
    SIM_HASH_ITEMS,
    SIM_HASH_SPELL,
    SIM_HASH_FLOW,
    SIM_HASH_INPUT,
    SIM_HASH_PART_COUNT,
};

extern const char *const g_SimHashPartNames[SIM_HASH_PART_COUNT];

u32 SimFrameHash(u32 *parts);

void SimDumpState(u32 frame, u32 pass);

struct SimItemRecord
{
    i32 index;
    i32 state;
    i32 type;
    u32 pos_x, pos_y, pos_z;
    u32 vel_x, vel_y, vel_z;
    u32 magnitude;
    u32 towards;
    i32 time;
    i32 intangible;
    i32 collector;
};
i32 SimCollectItems(SimItemRecord *out, i32 max);
