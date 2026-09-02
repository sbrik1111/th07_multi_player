// Reads simulation state only: no allocations, game RNG calls or game mutation.
#pragma once

namespace th07 {
namespace bot {

// `lead`: frames of this seat's input decided but not simulated yet.
unsigned short GameplayMask(unsigned frame, int seat, int lead = 0);

bool SoloEnabled();

}
}
