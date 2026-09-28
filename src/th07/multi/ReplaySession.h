#pragma once
#include "ReplayFile.h"
struct MainMenu;

namespace th07 { namespace replay {

void ReadCommandLine();
bool Requested();
bool PreparePlayback();
void ApplyGameConfig();
void RestoreGameConfig();
void StartRecording();
void FinishSession();
bool Playing();
bool FastPlayback();
bool Recording();
bool Failed();
bool SaveFailed();
bool UsesArena();
int ViewSeat();
void BeginGameplay();
bool MenuAllowed();
int FillMenu(MainMenu* menu);
bool SelectMenuReplay(MainMenu* menu, int index);
void LeaveMenu();
bool OpenMenuOnStart();
void RestartIfRequested();
int RunPlayback(int* present);
void Record(unsigned segment, unsigned frame, const unsigned short* inputs,
            unsigned hash, const unsigned* parts);
void RecordingError(const char* reason);

} }
