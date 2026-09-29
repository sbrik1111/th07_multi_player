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
int ViewSeat();
void SetSegment(unsigned segment);
void FrameStarting(unsigned frame, bool resimulating = false);
double FrameSeconds();
void StageLoading();
void StageLoaded();
bool LoadingStage();
StageState CaptureStage();
void RestoreStage(const StageState& state);
bool StartMenuReplay(MainMenu* menu);
void ReturnedToMenu();
bool MenuAllowed();
int FillMenu(MainMenu* menu);
bool SelectMenuReplay(MainMenu* menu, int index);
unsigned MenuStageScore(unsigned stage);
void LeaveMenu();
bool OpenMenuOnStart();
int RunPlayback(int* present);
void Record(unsigned segment, unsigned frame, const unsigned short* inputs,
            unsigned hash, const unsigned* parts);
void RecordingError(const char* reason);

} }
