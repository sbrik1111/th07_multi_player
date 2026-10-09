#include "FrameInput.hpp"
#include "Controller.hpp"
#include "Supervisor.hpp"
#include "multi/Launcher.h"
#include "multi/LocalKeyboard.h"
#include "multi/MpConfig.h"
#include <dinput.h>
#include <ctype.h>
#include <string.h>

namespace th07 { namespace input {

KeyBinding ParseKeyBinding(const char* name, KeyBinding fallback)
{
    if (!name || !*name)
        return fallback;
    if (_stricmp(name, "none") == 0)
        return KeyBinding{};
    if (_strnicmp(name, "key_", 4) == 0)
        name += 4;
    unsigned vk = 0;
    if (strlen(name) == 1 && isalnum(static_cast<unsigned char>(*name))) {
        vk = toupper(static_cast<unsigned char>(*name));
    } else {
        struct NamedKey { const char* name; unsigned vk; };
        static const NamedKey keys[] = {
            {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
            {"enter", VK_RETURN}, {"space", VK_SPACE}, {"tab", VK_TAB}, {"backspace", VK_BACK},
            {"lshift", VK_LSHIFT}, {"rshift", VK_RSHIFT},
            {"lcontrol", VK_LCONTROL}, {"rcontrol", VK_RCONTROL},
            {"lmenu", VK_LMENU}, {"rmenu", VK_RMENU},
            {"home", VK_HOME}, {"end", VK_END}, {"prior", VK_PRIOR}, {"next", VK_NEXT},
            {"insert", VK_INSERT}, {"delete", VK_DELETE},
            {"minus", VK_OEM_MINUS}, {"equals", VK_OEM_PLUS},
            {"lbracket", VK_OEM_4}, {"rbracket", VK_OEM_6},
            {"semicolon", VK_OEM_1}, {"apostrophe", VK_OEM_7}, {"grave", VK_OEM_3},
            {"backslash", VK_OEM_5}, {"comma", VK_OEM_COMMA},
            {"period", VK_OEM_PERIOD}, {"slash", VK_OEM_2},
            {"multiply", VK_MULTIPLY}, {"subtract", VK_SUBTRACT},
            {"add", VK_ADD}, {"divide", VK_DIVIDE}, {"decimal", VK_DECIMAL}
        };
        if (_stricmp(name, "numpad_enter") == 0)
            return KeyBinding{VK_RETURN, DIK_NUMPADENTER};
        if (_strnicmp(name, "numpad_", 7) == 0 && strlen(name) == 8 && name[7] >= '0' && name[7] <= '9')
            vk = VK_NUMPAD0 + name[7] - '0';
        for (const auto& key : keys)
            if (_stricmp(name, key.name) == 0)
                vk = key.vk;
    }
    if (!vk)
        return fallback;
    const unsigned scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    if (!scan)
        return fallback;
    return KeyBinding{vk, (scan & 0xff) | ((scan & 0xff00) ? 0x80u : 0u)};
}

bool KeyBindingDown(const KeyBinding& binding, const unsigned char* keys, bool directInput)
{
    const unsigned index = directInput ? binding.scanCode : binding.virtualKey;
    return index > 0 && index < 256 && (keys[index] & 0x80) != 0;
}

} }

namespace {

struct ActionBinding {
    const wchar_t* setting;
    const wchar_t* defaultName;
    u16 button;
    th07::input::KeyBinding key;
};

ActionBinding g_bindings[] = {
    {L"Up_2P", L"key_I", TH_BUTTON_UP, {'I', DIK_I}},
    {L"Down_2P", L"key_K", TH_BUTTON_DOWN, {'K', DIK_K}},
    {L"Left_2P", L"key_J", TH_BUTTON_LEFT, {'J', DIK_J}},
    {L"Right_2P", L"key_L", TH_BUTTON_RIGHT, {'L', DIK_L}},
    {L"Shoot_2P", L"key_F", TH_BUTTON_SHOOT, {'F', DIK_F}},
    {L"Bomb_2P", L"key_G", TH_BUTTON_BOMB, {'G', DIK_G}},
    {L"Focus_2P", L"key_D", TH_BUTTON_FOCUS, {'D', DIK_D}},
};

void LoadBindings()
{
    static bool loaded = false;
    if (loaded)
        return;
    loaded = true;
    for (auto& binding : g_bindings) {
        wchar_t value[32];
        char name[32];
        GetPrivateProfileStringW(L"KeyBind", binding.setting, binding.defaultName, value, 32,
                                  th07::launcher::SettingsPath());
        if (WideCharToMultiByte(CP_ACP, 0, value, -1, name, 32, nullptr, nullptr))
            binding.key = th07::input::ParseKeyBinding(name, binding.key);
    }
    th07::mp::Log("LOCAL_KEYS up=%u down=%u left=%u right=%u shot=%u bomb=%u focus=%u",
                  g_bindings[0].key.virtualKey, g_bindings[1].key.virtualKey, g_bindings[2].key.virtualKey,
                  g_bindings[3].key.virtualKey, g_bindings[4].key.virtualKey, g_bindings[5].key.virtualKey,
                  g_bindings[6].key.virtualKey);
}

}

u16 ReadLocalSecondKeyboardButtons()
{
    LoadBindings();
    if (GetForegroundWindow() != g_Supervisor.hwndGameWindow)
        return 0;
    u8 keys[256] = {};
    const bool lowLatency = th07::launcher::LowLatencyEnabled();
    const bool directInput = g_Supervisor.keyboard != nullptr && !lowLatency;
    if (lowLatency) {
        for (const auto& binding : g_bindings)
            keys[binding.key.virtualKey] = (GetAsyncKeyState(binding.key.virtualKey) & 0x8000) ? 0x80 : 0;
    } else if (directInput) {
        HRESULT hr = g_Supervisor.keyboard->GetDeviceState(sizeof(keys), keys);
        if (hr == DIERR_INPUTLOST)
            g_Supervisor.keyboard->Acquire();
        if (FAILED(hr))
            return 0;
    } else if (!GetKeyboardState(keys)) {
        return 0;
    }
    u16 buttons = 0;
    for (const auto& binding : g_bindings)
        if (th07::input::KeyBindingDown(binding.key, keys, directInput))
            buttons |= binding.button;
    return buttons;
}
