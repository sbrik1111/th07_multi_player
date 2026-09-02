#pragma once

#include "multi/RollbackNetcode.h"

namespace th07 {
namespace rollback_game {

// Call before any game object exists. False when the arena cannot be made.
bool Start();
bool Enabled();

void BeginSegment();

// Its state is final once every input through the frame is known.
typedef void (*FrameDone)(unsigned frame, void* context);

// 0 quit, 2 the gameplay ends here, else 1.
int RunFrame(netcode::Timeline& timeline, int* present, FrameDone done, void* context);

struct Stats {
    unsigned rollbacks;
    unsigned replayedFrames;
    unsigned checkpoints;
    unsigned skippedCheckpoints;
    unsigned maxRollback;
};
const Stats& GetStats();
void LogPerf();

}
}
