#pragma once

#include "GameManager.hpp"
#include "inttypes.hpp"

struct FrameInputs
{
    u16 menu;              // the menus' g_CurFrameRawInput
    u16 held[MAX_PLAYERS];
};

extern const FrameInputs *g_FrameInputs;

u16 ReadDeviceButtons(i32 keyboardOnly);
u16 ReadJoypadButtonsOf(i32 index);
u16 ReadLocalSecondKeyboardButtons();
