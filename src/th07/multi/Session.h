// A logical frame runs the calc and draw chains: draw callbacks run ANM scripts, so every PC
// draws every frame.
#pragma once

void MpInitSession();
void MpDrawTitleSession();
void MpDrawPlaySession();
void MpDrawSelectLabels();
void MpLogBgmState();
// 0 quit, -1 restart, else go on. *present: a new frame was drawn.
int MpRunHostTick(int* present);
void MpRequestShutdown();
