#pragma once

// As RunLogicalFrame: 0 quit, -1 restart, else 1.
int SessionRunFrame(const unsigned short* held, int count, int draw);
unsigned short SessionLocalInput(bool gameplay);
unsigned short SessionBotMask(int seat);
int SessionGameplayActive();
void SessionLogGameEnd();
int SessionGameplayExitPending();

// The next frame must not run on predicted input (scene or stage switches, loads, menus).
int SessionPredictionBlocked();

unsigned short SessionEncodeButtons(unsigned buttons);
unsigned SessionDecodeButtons(unsigned short bits);
