#include "ReplaySession.h"
#include "MpConfig.h"
#include "Session.h"
#include "SessionFrame.h"
#include "RollbackHeap.h"
#include "GameManager.hpp"
#include "GameWindow.hpp"
#include "MainMenu.hpp"
#include "SimHash.hpp"
#include "Supervisor.hpp"
#include "EnemyManager.hpp"
#include "AsciiManager.hpp"
#include "Controller.hpp"
#include "Gui.hpp"
#include "Player.hpp"
#include "SoundPlayer.hpp"
#include "ZunMemory.hpp"
#include <windows.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <algorithm>
#include <string>
#include <vector>
#include <stddef.h>
#include <string.h>
#include "RuntimeData.h"

namespace th07 { namespace replay {
namespace {
Writer writer;
Reader reader;
Header recordingHeader = {};
Header menuHeader = {};
bool playing, recording, failed, saveFailed, enabled, openMenu, boot, loading, returning;
unsigned currentSegment, currentFrame, pendingSegment, pendingFrame, selectedStage, playMode, gameNumber;
bool pendingStage;
StageState pendingState;
mp::Config viewerMp;
unsigned viewerGames;
unsigned char viewerCharacters[4], viewerShots[4];
unsigned hashes;
bool paused;
unsigned short viewerInput;
PauseMenu pauseMenu;
struct FrameTiming { unsigned segment, frame, fps; };
FrameTiming frameTiming[8192];
double frameSeconds = 1.0 / 60.0;
GameConfiguration viewerConfig;
bool viewerConfigSaved;
wchar_t replayPath[MAX_PATH];
static_assert(kHashParts == SIM_HASH_PART_COUNT, "replay hash parts");
static_assert(sizeof(GameConfiguration) == sizeof(Settings::gameConfig), "replay game settings");

bool TestMode()
{
    wchar_t value[8];
    return GetEnvironmentVariableW(L"TH07_MP_REPLAY_TEST", value, 8) && value[0] == L'1';
}

void Env(const wchar_t* name, const wchar_t* value)
{
    _wputenv_s(name, value ? value : L"");
    SetEnvironmentVariableW(name, value);
}

void Error(const char* reason)
{
    failed = true;
    mp::Log("REPLAY_FAIL reason=%s frame=%u", reason, reader.Count());
    if (!TestMode()) {
        wchar_t text[512];
        swprintf_s(text, L"リプレイを再生できません。\n%S", reason);
        MessageBoxW(g_GameWindow.window, text, L"Replay", MB_OK | MB_ICONERROR);
    }
}

bool Digest(const wchar_t* path, unsigned char* out)
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb") || !file) return false;
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
              CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
    unsigned char buffer[65536];
    size_t n;
    while (ok && (n = fread(buffer, 1, sizeof(buffer), file)) != 0)
        ok = CryptHashData(hash, buffer, (DWORD)n, 0) != 0;
    DWORD bytes = 32;
    ok = ok && !ferror(file) && CryptGetHashParam(hash, HP_HASHVAL, out, &bytes, 0) && bytes == 32;
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    fclose(file);
    return ok;
}

bool Fingerprints(Settings& settings)
{
    wchar_t exe[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return length && length < MAX_PATH && Digest(exe, settings.executable) && Digest(L"th07.dat", settings.gameData);
}

bool Compatible(const Header& header)
{
    Settings live = {};
    return Fingerprints(live) && !memcmp(live.executable, header.settings.executable, 32) &&
           !memcmp(live.gameData, header.settings.gameData, 32);
}

void SaveViewer()
{
    viewerMp = mp::Cfg();
    viewerGames = g_GameManager.gamesStarted;
    for (int seat = 0; seat < 4; ++seat) {
        viewerCharacters[seat] = g_GameManager.Character(seat);
        viewerShots[seat] = g_GameManager.ShotType(seat);
    }
}

int EndPlayback(int* present)
{
    if (TestMode()) return 0;
    paused = false;
    returning = true;
    g_Supervisor.curState = 1;
    unsigned short held[4] = {};
    *present = 1;
    return SessionRunFrame(held, mp::PlayerCount(), 1);
}

}

void ReadCommandLine()
{
    int count = 0;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return;
    if (count >= 3 && wcscmp(args[1], L"--mp-replay") == 0) {
        Env(L"TH07_MP_REPLAY", args[2]);
        Env(L"TH07_MP_MODE", L"local");

    } else if (count >= 2 && (!wcscmp(args[1], L"--mp-local-menu") || !wcscmp(args[1], L"--mp-local"))) {
        Env(L"TH07_MP_REPLAY", nullptr);
        Env(L"TH07_MP_MODE", L"local");
        Env(L"TH07_MP_ROLLBACK", L"0");
        openMenu = !wcscmp(args[1], L"--mp-local-menu");
    }
    LocalFree(args);
}

bool Requested()
{
    return GetEnvironmentVariableW(L"TH07_MP_REPLAY", replayPath, MAX_PATH) != 0;
}

bool PreparePlayback()
{
    if (!Requested()) return true;
    if (GetEnvironmentVariableW(L"TH07_MP_REPLAY", replayPath, MAX_PATH) >= MAX_PATH) {
        Error("replay path too long"); return false;
    }
    if (!reader.Open(replayPath)) { Error(reader.Error()); return false; }
    if (!Compatible(reader.Info())) { Error("different executable or th07.dat"); return false; }
    SaveViewer();
    mp::ConfigureReplay(reader.Info().settings);
    Env(L"TH07_MP_MODE", L"local");
    playing = boot = true;
    selectedStage = 0;
    for (unsigned i = 0; i < 8; ++i)
        if (reader.Info().stages[i].state.stage) { selectedStage = i + 1; break; }
    wchar_t stageText[8];
    if (TestMode() && GetEnvironmentVariableW(L"TH07_MP_REPLAY_STAGE", stageText, 8)) selectedStage = _wtoi(stageText);
    if (!reader.SeekStage(selectedStage)) { Error("stage is not recorded"); return false; }
    mp::Log("REPLAY_OPEN players=%u session=%08X", reader.Info().settings.players, reader.Info().settings.session);
    return true;
}

void ApplyGameConfig()
{
    if (!playing) return;
    viewerConfig = g_Supervisor.cfg;
    viewerConfigSaved = true;
    // Keep viewer display/audio preferences; restore simulation-sensitive options.
    GameConfiguration saved;
    memcpy(&saved, reader.Info().settings.gameConfig, sizeof(saved));
    g_Supervisor.cfg.lifeCount = saved.lifeCount;
    g_Supervisor.cfg.bombCount = saved.bombCount;
    g_Supervisor.cfg.slowMode = saved.slowMode;
    g_Supervisor.cfg.shotSlow = saved.shotSlow;
    g_Supervisor.cfg.defaultDifficulty = saved.defaultDifficulty;
}

void RestoreGameConfig()
{
    if (viewerConfigSaved) g_Supervisor.cfg = viewerConfig;
    viewerConfigSaved = false;
}

void StartRecording()
{
    enabled = true;
}

void OpenRecording()
{
    if (!enabled || playing || saveFailed) return;
    FinishSession();
    const mp::Config& cfg = mp::Cfg();
    Settings settings = {};
    settings.players = cfg.playerCount;
    settings.viewSeat = cfg.mode == mp::kUdp ? cfg.localSeat : 0;
    settings.session = cfg.sessionId;
    for (int seat = 0; seat < 4; ++seat) {
        settings.characters[seat] = pendingState.seats[seat].character;
        settings.shots[seat] = pendingState.seats[seat].shot;
        strcpy_s(settings.names[seat], cfg.playerName[seat]);
    }
    settings.startStage = cfg.testStartStage;
    settings.clearFrame = cfg.testStageClearFrame;
    settings.clearLast = cfg.testStageClearLast;
    settings.ghostFrame = cfg.testGhostFrame;
    settings.keepAlive = cfg.testKeepAlive ? 1 : 0;
    settings.arena = cfg.mode == mp::kUdp && cfg.rollback ? 1 : 0;
    memcpy(settings.gameConfig, &g_Supervisor.cfg, sizeof(g_Supervisor.cfg));
    if (!Fingerprints(settings)) { RecordingError("cannot fingerprint game files"); return; }
    recordingHeader = MakeHeader(settings);
    recordingHeader.difficulty = pendingState.difficulty;
    recordingHeader.shot = pendingState.seats[0].character * 2 + pendingState.seats[0].shot;
    SYSTEMTIME now;
    GetLocalTime(&now);
    sprintf_s(recordingHeader.date, "%02u/%02u", now.wMonth, now.wDay);
    recordingHeader.checksum = Checksum(&recordingHeader, offsetof(Header, checksum));
    wchar_t path[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"TH07_MP_RECORD", path, MAX_PATH);
    if (length >= MAX_PATH) { RecordingError("recording path too long"); return; }
    if (!length) {
        if (!CreateDirectoryW(L"replay", nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            RecordingError("cannot create replay directory"); return;
        }
        swprintf_s(path, L"replay\\th07_mp_%04u%02u%02u_%02u%02u%02u_%03u_p%u_%u.mpr",
                   now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                   now.wMilliseconds, settings.viewSeat + 1, GetCurrentProcessId());
    }
    if (length && gameNumber) {
        wchar_t base[MAX_PATH];
        wcscpy_s(base, path);
        wchar_t* extension = wcsrchr(base, L'.');
        if (extension) *extension = 0;
        if (swprintf_s(path, L"%s_game%03u.mpr", base, gameNumber + 1) < 0) {
            RecordingError("recording path too long"); return;
        }
    }
    ++gameNumber;
    if (!writer.Open(path, recordingHeader)) { RecordingError(writer.Error()); return; }
    recording = true;
    mp::Log("REPLAY_RECORD players=%u session=%08X path=%ls", settings.players, settings.session, path);
}

bool Playing() { return playing; }
bool FastPlayback()
{
    static int fast = -1;
    if (fast < 0) {
        wchar_t value[8];
        fast = TestMode() && GetEnvironmentVariableW(L"TH07_MP_REPLAY_FAST", value, 8) && value[0] == L'1';
    }
    return playing && fast != 0;
}
bool Recording() { return enabled && !playing && !saveFailed; }
bool Failed() { return failed; }
bool SaveFailed() { return saveFailed; }
int ViewSeat() { return playing ? (int)reader.Info().settings.viewSeat : 0; }
bool MenuAllowed() { return mp::LocalEnabled() && !playing; }
bool OpenMenuOnStart() { bool result = openMenu; openMenu = false; return result; }

void RecordingError(const char* reason)
{
    if (!saveFailed) mp::Log("REPLAY_SAVE_FAIL reason=%s frames=%u", reason, writer.Count());
    recording = false;
    saveFailed = true;
    writer.Close();
}

void SetSegment(unsigned segment) { currentSegment = segment; }
void FrameStarting(unsigned frame, bool resimulating)
{
    currentFrame = frame;
    if (!resimulating && Recording()) {
        unsigned fps = (unsigned)MpDisplayedFps();
        frameTiming[frame % _countof(frameTiming)] = {currentSegment, frame, fps && fps < 60 ? fps : 60};
    }
}
double FrameSeconds() { return playing && !paused ? frameSeconds : 1.0 / 60.0; }
bool LoadingStage() { return loading; }

void StageLoading()
{
    if (playing) {
        if (boot) {
            loading = true;
            RestoreStage(reader.Info().stages[selectedStage - 1].state);
        }
        return;
    }
    if (!Recording()) return;
    pendingState = CaptureStage();
    pendingSegment = currentSegment;
    pendingFrame = currentFrame;
    pendingStage = true;
}

void StageLoaded()
{
    if (loading) {
        g_Supervisor.curState = 2;
        loading = boot = false;
        mp::Log("REPLAY_STAGE stage=%u pid=%u window=%p", selectedStage, GetCurrentProcessId(), g_GameWindow.window);
    }
}

void Record(unsigned segment, unsigned frame, const unsigned short* inputs, unsigned hash, const unsigned* parts)
{
    if (!Recording()) return;
    if (pendingStage && segment == pendingSegment && frame == pendingFrame) {
        if (!recording || pendingState.scene != 3 ||
            recordingHeader.stages[pendingState.stage - 1].state.stage) OpenRecording();
        if (!recording) return;
        if (pendingState.scene == 3 && pendingState.stage > 1)
            recordingHeader.stages[pendingState.stage - 2].score = pendingState.globals[1];
        StageEntry& entry = recordingHeader.stages[pendingState.stage - 1];
        entry.state = pendingState;
        entry.index = writer.Count();
        entry.segment = segment;
        entry.frame = frame;
        entry.rolling = writer.Rolling();
        entry.score = pendingState.globals[1];
        pendingStage = false;
        if (!writer.UpdateHeader(recordingHeader)) { RecordingError(writer.Error()); return; }
    }
    if (!recording) return;
    Frame out = {};
    out.segment = segment;
    out.frame = frame;
    const FrameTiming& timing = frameTiming[frame % _countof(frameTiming)];
    if (timing.segment != segment || timing.frame != frame) { RecordingError("missing frame timing"); return; }
    out.flags = timing.fps << kFpsShift;
    memcpy(out.held, inputs, mp::PlayerCount() * sizeof(*inputs));
    if (frame % kHashInterval == 0) {
        if (!parts) { RecordingError("missing confirmed state hash"); return; }
        out.flags |= kHasHash;
        out.hash = hash;
        memcpy(out.parts, parts, sizeof(out.parts));
    }
    if (!writer.Append(out)) RecordingError(writer.Error());
}

void FinishSession()
{
    if (recording) {
        if (!pendingStage && g_GameManager.globals && g_GameManager.currentStage >= 1 && g_GameManager.currentStage <= 8) {
            recordingHeader.stages[g_GameManager.currentStage - 1].score = g_GameManager.globals->score;
            if (!writer.UpdateHeader(recordingHeader)) RecordingError(writer.Error());
        }
        recording = false;
        if (!writer.Finish()) RecordingError(writer.Error());
        else mp::Log("REPLAY_SAVED frames=%u", writer.Count());
    }
}

int RunPlayback(int* present)
{
    *present = 0;
    const unsigned short previous = viewerInput;
    viewerInput = TestMode() ? 0 : SessionMenuInput();
    if (!boot && !paused && (viewerInput & ~previous & TH_BUTTON_MENU)) {
        paused = true;
        memset(&pauseMenu, 0, sizeof(pauseMenu));
        g_SoundPlayer.PushCommand(AUDIO_PAUSE, 0, "Pause");
        g_SoundPlayer.PlaySoundByIdx(SOUND_37, 0);
        mp::Log("REPLAY_PAUSE index=%u", reader.Count());
    }
    if (paused) {
        const u16 raw = g_CurFrameRawInput, last = g_LastFrameRawInput;
        const u32 flags = g_GameManager.flags;
        const u8 gamePause = g_GameManager.isInPauseMenu;
        const Rng rng = g_Rng;
        PauseMenu gameMenu = g_AsciiManager.pauseMenu;
        g_CurFrameRawInput = viewerInput;
        g_LastFrameRawInput = pauseMenu.curState == 0 ? viewerInput : previous;
        g_GameManager.replay = 1;
        g_GameManager.isInPauseMenu = 1;
        pauseMenu.OnUpdate();
        paused = g_GameManager.isInPauseMenu != 0;
        g_AsciiManager.pauseMenu = pauseMenu;
        DrawLogicalFrame();
        g_AsciiManager.pauseMenu = gameMenu;
        g_CurFrameRawInput = raw;
        g_LastFrameRawInput = last;
        g_GameManager.flags = flags;
        g_GameManager.isInPauseMenu = gamePause;
        g_Rng = rng;
        g_SoundPlayer.ProcessQueues();
        *present = 1;
        if (g_Supervisor.curState == 1) return EndPlayback(present);
        if (!paused) mp::Log("REPLAY_RESUME index=%u", reader.Count());
        return 1;
    }
    if (boot) {
        if (!g_Supervisor.wantedState) {
            unsigned short held[4] = {};
            if (SessionRunFrame(held, mp::PlayerCount(), 1) != 1) return 0;
            for (ChainElem* e = g_Chain.calcChain.next; e; e = e->next) {
                if (e->callback == (ChainCallback)MainMenu::OnUpdate) { g_Chain.Cut(e); break; }
            }
        }
        g_Supervisor.wantedState = 1;
        g_Supervisor.curState = 2;
    }
    unsigned steps = 1;
    if (!boot && g_Gui.HasCurrentMsgIdx() && g_Gui.IsDialogueSkippable()) steps = 3;
    else if (!boot && playMode == 2 && !g_EnemyManager.HasActiveBoss()) steps = 5;
    if (viewerInput & TH_BUTTON_SKIP) steps = 8;
    for (unsigned step = 0; step < steps; ++step) {
        Frame frame;
        int next = reader.Next(frame);
        if (next <= 0) {
            if (next < 0) Error(reader.Error());
            else mp::Log("REPLAY_DONE frames=%u hashes=%u complete=%d", reader.Count(), hashes, reader.Complete() ? 1 : 0);
            return EndPlayback(present);
        }
        const unsigned fps = (frame.flags & kFpsMask) >> kFpsShift;
        frameSeconds = playMode == 1 && fps && steps == 1 ? 1.0 / fps : 1.0 / 60.0;
        int status = SessionRunFrame(frame.held, mp::PlayerCount(), 1);
        if (status != 1) { Error("simulation stopped before replay end"); return EndPlayback(present); }
        *present = 1;
        if (frame.flags & kHasHash) {
            unsigned parts[SIM_HASH_PART_COUNT];
            unsigned actual = SimFrameHash(parts);
            ++hashes;
            if (actual != frame.hash || memcmp(parts, frame.parts, sizeof(parts))) {
                mp::Log("REPLAY_DESYNC index=%u segment=%u frame=%u expected=%08X actual=%08X", frame.index,
                        frame.segment, frame.frame, frame.hash, actual);
                for (unsigned i = 0; i < kHashParts; ++i)
                    if (parts[i] != frame.parts[i]) mp::Log("REPLAY_PART %s=%08X/%08X", g_SimHashPartNames[i], frame.parts[i], parts[i]);
                Error("simulation state mismatch");
                return EndPlayback(present);
            }
        }
        if (frame.index % 600 == 0) mp::Log("REPLAY_CHECK index=%u segment=%u frame=%u hashes=%u", frame.index, frame.segment, frame.frame, hashes);
    }
    return 1;
}

void ReturnedToMenu()
{
    if (!returning) return;
    returning = playing = boot = loading = false;
    reader.Close();
    RestoreGameConfig();
    mp::RestoreConfig(viewerMp);
    MpInitSession();
    g_GameManager.gamesStarted = viewerGames;
    for (int seat = 0; seat < 4; ++seat) {
        g_GameManager.Character(seat) = viewerCharacters[seat];
        g_GameManager.ShotType(seat) = viewerShots[seat];
    }
    g_GameManager.flags = 0;
    g_GameManager.isInPauseMenu = g_GameManager.isInRetryMenu = 0;
    openMenu = true;
    mp::Log("REPLAY_RETURN pid=%u window=%p", GetCurrentProcessId(), g_GameWindow.window);
}

int FillMenu(MainMenu* menu)
{
    if (!MenuAllowed()) return 0;
    FinishSession();
    rollback::heap::RuntimeScope runtime;
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW item;
    HANDLE find = FindFirstFileW(L"replay\\*.mpr", &item);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(item.cFileName);
    } while (FindNextFileW(find, &item));
    FindClose(find);
    std::sort(files.rbegin(), files.rend());
    int count = 0;
    for (const auto& name : files) {
        if (count == 60) break;
        std::wstring path = L"replay\\" + name;
        Reader preview;
        if (!preview.Open(path.c_str()) || preview.Info().difficulty == UINT32_MAX) continue;
        bool hasStage = false;
        for (const auto& stage : preview.Info().stages) hasStage |= stage.state.stage != 0;
        Frame first;
        if (!hasStage || preview.Next(first) != 1) continue;
        if (!WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, menu->replayFilenames[count], 512, nullptr, nullptr)) continue;
        ::ReplayFile& row = menu->replays[count];
        memset(&row, 0, sizeof(row));
        row.data.difficulty = (u8)preview.Info().difficulty;
        row.data.shotType = (u8)preview.Info().shot;
        memcpy(row.data.date, preview.Info().date, sizeof(row.data.date));
        sprintf_s(row.data.name, "COOP %uP", preview.Info().settings.players);
        for (int stage = 0; stage < 8; ++stage)
            if (preview.Info().stages[stage].state.stage) row.head.stageReplayData[stage < 6 ? stage : 6].offset = 1;
        sprintf_s(menu->replayLabels[count], "MP%02d", count + 1);
        ++count;
    }
    mp::Log("REPLAY_MENU entries=%d", count);
    return count;
}

bool SelectMenuReplay(MainMenu* menu, int index)
{
    if (!MenuAllowed() || index < 0 || index >= menu->replayFilesNum) return false;
    wchar_t path[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, menu->replayFilenames[index], -1, path, MAX_PATH)) return false;
    Reader preview;
    if (!preview.Open(path) || !Compatible(preview.Info())) {
        mp::Log("REPLAY_SELECT_FAIL incompatible file");
        if (!TestMode()) MessageBoxW(g_GameWindow.window, L"このリプレイはゲームのバージョンまたはデータが異なるため再生できません。", L"Replay", MB_OK | MB_ICONERROR);
        return false;
    }
    menuHeader = preview.Info();
    double duration = 0.0;
    Frame frame;
    int status;
    while ((status = preview.Next(frame)) == 1) {
        unsigned fps = (frame.flags & kFpsMask) >> kFpsShift;
        duration += 1.0 / (fps ? fps : 60);
    }
    if (status < 0 || !preview.Count()) { Error("invalid replay inputs"); return false; }
    for (unsigned stage = 0; stage < 8; ++stage)
        if (menuHeader.stages[stage].state.stage && menuHeader.stages[stage].index >= preview.Count())
            menu->replays[index].head.stageReplayData[stage < 6 ? stage : 6].offset = 0;
    menu->replays[index].data.slowdownRate = (float)((1.0 - preview.Count() / (60.0 * duration)) * 100.0);
    if (menu->replays[index].data.slowdownRate < 0.0f) menu->replays[index].data.slowdownRate = 0.0f;
    menu->currentReplay = (::ReplayFile*)ZunMemory::Alloc(sizeof(::ReplayFile));
    if (!menu->currentReplay) return false;
    *menu->currentReplay = menu->replays[index];
    return true;
}

unsigned MenuStageScore(unsigned stage)
{
    if (menuHeader.difficulty == 5) stage = 7;
    return stage < 8 ? menuHeader.stages[stage].score : 0;
}

bool StartMenuReplay(MainMenu* menu)
{
    if (!MenuAllowed()) return false;
    wchar_t path[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, menu->replayFilenames[menu->chosenReplay], -1, path, MAX_PATH)) return false;
    reader.Close();
    if (!reader.Open(path) || !Compatible(reader.Info())) { Error("cannot open replay"); reader.Close(); return false; }
    selectedStage = reader.Info().difficulty == 5 ? 8 : menu->selectedStage + 1;
    if (!reader.SeekStage(selectedStage)) { Error("stage is not recorded"); reader.Close(); return false; }
    SaveViewer();
    mp::ConfigureReplay(reader.Info().settings);
    playing = boot = true;
    playMode = menu->cursor;
    hashes = 0;
    paused = false;
    viewerInput = SessionMenuInput();
    ApplyGameConfig();
    MpInitSession();
    g_Supervisor.curState = 2;
    mp::Log("REPLAY_SELECTED stage=%u mode=%u pid=%u window=%p", selectedStage, playMode, GetCurrentProcessId(), g_GameWindow.window);
    return true;
}

void LeaveMenu() { openMenu = false; }

} }
