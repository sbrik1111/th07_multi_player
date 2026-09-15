#include "FrameInput.hpp"

#include <dinput.h>

#include "Controller.hpp"
#include "Supervisor.hpp"

namespace {

u16 ButtonsFromVirtualKeys(const u8* keys)
{
    u16 buttons = 0;
    if (keys['I'] & 0x80) buttons |= TH_BUTTON_UP;
    if (keys['K'] & 0x80) buttons |= TH_BUTTON_DOWN;
    if (keys['J'] & 0x80) buttons |= TH_BUTTON_LEFT;
    if (keys['L'] & 0x80) buttons |= TH_BUTTON_RIGHT;
    if (keys['F'] & 0x80) buttons |= TH_BUTTON_SHOOT;
    if (keys['G'] & 0x80) buttons |= TH_BUTTON_BOMB;
    if (keys['D'] & 0x80) buttons |= TH_BUTTON_FOCUS;
    return buttons;
}

u16 ButtonsFromDirectInput(const u8* keys)
{
    u16 buttons = 0;
    if (keys[DIK_I] & 0x80) buttons |= TH_BUTTON_UP;
    if (keys[DIK_K] & 0x80) buttons |= TH_BUTTON_DOWN;
    if (keys[DIK_J] & 0x80) buttons |= TH_BUTTON_LEFT;
    if (keys[DIK_L] & 0x80) buttons |= TH_BUTTON_RIGHT;
    if (keys[DIK_F] & 0x80) buttons |= TH_BUTTON_SHOOT;
    if (keys[DIK_G] & 0x80) buttons |= TH_BUTTON_BOMB;
    if (keys[DIK_D] & 0x80) buttons |= TH_BUTTON_FOCUS;
    return buttons;
}

}

u16 ReadLocalSecondKeyboardButtons()
{
    u8 keys[256] = {};
    if (g_Supervisor.keyboard == nullptr)
    {
        return GetKeyboardState(keys) ? ButtonsFromVirtualKeys(keys) : 0;
    }
    HRESULT hr = g_Supervisor.keyboard->GetDeviceState(sizeof(keys), keys);
    if (hr == DIERR_INPUTLOST)
    {
        g_Supervisor.keyboard->Acquire();
    }
    return SUCCEEDED(hr) ? ButtonsFromDirectInput(keys) : 0;
}
