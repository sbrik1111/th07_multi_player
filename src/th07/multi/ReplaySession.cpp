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
bool playing, recording, failed, saveFailed, started, returnToMenu, openMenu;
unsigned hashes;
GameConfiguration viewerConfig;
bool viewerConfigSaved;
wchar_t replayPath[MAX_PATH], restartArgs[MAX_PATH * 2];
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

void QueueLocalMenu(bool menu)
{
    wcscpy_s(restartArgs, menu ? L"--mp-local-menu" : L"--mp-local");
    MpRequestShutdown();
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
        returnToMenu = count >= 4 && wcscmp(args[3], L"--return-replay-menu") == 0;
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
    mp::ConfigureReplay(reader.Info().settings);
    Env(L"TH07_MP_MODE", L"local");
    playing = true;
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
}

void StartRecording()
{
    if (started || playing || openMenu) return;
    started = true;
    const mp::Config& cfg = mp::Cfg();
    Settings settings = {};
    settings.players = cfg.playerCount;
    settings.viewSeat = cfg.mode == mp::kUdp ? cfg.localSeat : 0;
    settings.session = cfg.sessionId;
    for (int seat = 0; seat < 4; ++seat) {
        settings.characters[seat] = cfg.testCharacters[seat];
        settings.shots[seat] = cfg.testShotTypes[seat];
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
bool Recording() { return recording; }
bool Failed() { return failed; }
bool SaveFailed() { return saveFailed; }
bool UsesArena() { return playing && reader.Info().settings.arena; }
int ViewSeat() { return playing ? (int)reader.Info().settings.viewSeat : 0; }
bool MenuAllowed() { return mp::LocalEnabled() && !playing; }
bool OpenMenuOnStart() { return openMenu; }

void RecordingError(const char* reason)
{
    if (!saveFailed) mp::Log("REPLAY_SAVE_FAIL reason=%s frames=%u", reason, writer.Count());
    recording = false;
    saveFailed = true;
    writer.Close();
}

void BeginGameplay()
{
    if (recording && recordingHeader.difficulty == UINT32_MAX) {
        recordingHeader.difficulty = g_GameManager.difficulty;
        recordingHeader.shot = g_GameManager.Character(0) * 2 + g_GameManager.ShotType(0);
        if (!writer.UpdateHeader(recordingHeader)) RecordingError(writer.Error());
    }
}

void Record(unsigned segment, unsigned frame, const unsigned short* inputs, unsigned hash, const unsigned* parts)
{
    if (!recording) return;
    Frame out = {};
    out.segment = segment;
    out.frame = frame;
    memcpy(out.held, inputs, mp::PlayerCount() * sizeof(*inputs));
    if (frame % kHashInterval == 0) {
        if (!parts) { RecordingError("missing confirmed state hash"); return; }
        out.flags = kHasHash;
        out.hash = hash;
        memcpy(out.parts, parts, sizeof(out.parts));
    }
    if (!writer.Append(out)) RecordingError(writer.Error());
}

void FinishSession()
{
    if (recording) {
        recording = false;
        if (!writer.Finish()) RecordingError(writer.Error());
        else mp::Log("REPLAY_SAVED frames=%u", writer.Count());
    }
}

int RunPlayback(int* present)
{
    *present = 0;
    DWORD foreground = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    if (!TestMode() && foreground == GetCurrentProcessId() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
        if (returnToMenu) QueueLocalMenu(true);
        return 0;
    }
    Frame frame;
    int next = reader.Next(frame);
    if (next <= 0) {
        if (next < 0) Error(reader.Error());
        else mp::Log("REPLAY_DONE frames=%u hashes=%u complete=%d", reader.Count(), hashes, reader.Complete() ? 1 : 0);
        if (returnToMenu) QueueLocalMenu(true);
        return 0;
    }
    int status = SessionRunFrame(frame.held, mp::PlayerCount(), 1);
    if (status != 1) { Error("simulation stopped before replay end"); return 0; }
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
            if (returnToMenu) QueueLocalMenu(true);
            return 0;
        }
    }
    if (frame.index % 600 == 0) mp::Log("REPLAY_CHECK index=%u segment=%u frame=%u hashes=%u", frame.index, frame.segment, frame.frame, hashes);
    return 1;
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
        if (!WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, menu->replayFilenames[count], 512, nullptr, nullptr)) continue;
        ::ReplayFile& row = menu->replays[count];
        memset(&row, 0, sizeof(row));
        row.data.difficulty = (u8)preview.Info().difficulty;
        row.data.shotType = (u8)preview.Info().shot;
        memcpy(row.data.date, preview.Info().date, sizeof(row.data.date));
        sprintf_s(row.data.name, "COOP %uP", preview.Info().settings.players);
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
    swprintf_s(restartArgs, L"--mp-replay \"%s\" --return-replay-menu", path);
    mp::Log("REPLAY_SELECTED index=%d", index);
    MpRequestShutdown();
    return true;
}

void LeaveMenu()
{
    if (MenuAllowed()) QueueLocalMenu(false);
}

void RestartIfRequested()
{
    if (!restartArgs[0]) return;
    wchar_t exe[MAX_PATH], command[MAX_PATH * 4];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    swprintf_s(command, L"\"%s\" %s", exe, restartArgs);
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    if (CreateProcessW(exe, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        mp::Log("REPLAY_RUNTIME pid=%u", process.dwProcessId);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } else Error("cannot restart replay runtime");
}

} }
