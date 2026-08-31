#pragma once

namespace th07 {
namespace launcher {

enum Mode {
    kCancelled,
    kSingle,
    kLocal,
    kHost,
    kGuest,
    kSkipped,
};

struct Selection {
    Mode mode;
    char playerName[4][16];
    int playerCount;
    int localSeat;
    bool bot;
    int displayMode; // resolution (0 640, 1 960, 2 1280) + 3 when windowed
    bool displaySelected;
    bool bgm;
    bool se;
    bool audioSelected;
};

// kSkipped when the environment configures the session (TH07_MP_MODE or TH07_MP_GUI=0).
bool Run(Selection* selection);

// Call after th07.cfg is read.
void ApplyGameConfig();
// Call before th07.cfg is written back.
void RestoreGameConfig();
// In halves of 640 x 480.
int WindowScale();

}
}
