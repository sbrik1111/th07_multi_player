#pragma once

namespace th07 { namespace input {

struct KeyBinding {
    unsigned virtualKey;
    unsigned scanCode;
};

KeyBinding ParseKeyBinding(const char* name, KeyBinding fallback);
bool KeyBindingDown(const KeyBinding& binding, const unsigned char* keys, bool directInput);

} }
