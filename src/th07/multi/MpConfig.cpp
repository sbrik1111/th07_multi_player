#include "ReplayFile.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "multi/Lobby.h"
#include "multi/MpConfig.h"

namespace th07 {
namespace mp {
namespace {

Config g_cfg = {};
PreparedUdpSocket g_preparedUdp = {};
bool g_netInitialized;

bool EnvText(const char* name, char* out, size_t size)
{
    size_t got = 0;
    if (getenv_s(&got, out, size, name) != 0 || got <= 1) {
        if (size != 0) {
            out[0] = 0;
        }
        return false;
    }
    return true;
}

int EnvInt(const char* name, int fallback)
{
    char text[32];
    return EnvText(name, text, sizeof(text)) ? atoi(text) : fallback;
}

unsigned EnvUnsigned(const char* name, unsigned fallback)
{
    char text[32];
    if (!EnvText(name, text, sizeof(text))) {
        return fallback;
    }
    return static_cast<unsigned>(strtoul(text, 0, 0));
}

// CP932 whatever the PC's code page is.
void EnvName(const char* name, char* out, size_t size)
{
    out[0] = 0;
    wchar_t wideName[64];
    wchar_t wide[64];
    if (MultiByteToWideChar(CP_ACP, 0, name, -1, wideName, 64) <= 0) {
        return;
    }
    const DWORD got = GetEnvironmentVariableW(wideName, wide, 64);
    if (got == 0 || got >= 64 || WideCharToMultiByte(932, 0, wide, -1, out, static_cast<int>(size), "?", nullptr) <= 0) {
        out[0] = 0;
    }
}

void EnvList(const char* name, int* out, int lo, int hi)
{
    char text[64];
    size_t got = 0;
    for (int i = 0; i < 4; i++) {
        out[i] = -1;
    }
    if (getenv_s(&got, text, sizeof(text), name) != 0 || got <= 1) {
        return;
    }
    const char* p = text;
    for (int i = 0; i < 4 && *p != 0; i++) {
        char* end;
        long value = strtol(p, &end, 10);
        if (end != p && value >= lo && value <= hi) {
            out[i] = (int)value;
        }
        p = *end == ',' ? end + 1 : end;
        if (end == p && *p != ',') {
            break;
        }
    }
}

void InitConfig()
{
    if (g_cfg.initialized) {
        return;
    }
    g_cfg.initialized = true;
    g_cfg.mode = kLocal;
    g_cfg.localSeat = 0;
    g_cfg.menuInputDelay = 4;
    g_cfg.sessionId = 0x20260912u;
    char mode[24];
    if (EnvText("TH07_MP_MODE", mode, sizeof(mode))) {
        if (_stricmp(mode, "udp") == 0) {
            g_cfg.mode = kUdp;
        }
    }
    const int fewest = g_cfg.mode == kUdp ? 2 : 1;
    g_cfg.playerCount = EnvInt("TH07_MP_PLAYERS", fewest);
    if (g_cfg.playerCount < fewest || g_cfg.playerCount > kMaxPlayers) {
        g_cfg.playerCount = fewest;
    }
    g_cfg.localSeat = EnvInt("TH07_MP_SEAT", 0);
    if (g_cfg.localSeat < 0 || g_cfg.localSeat >= g_cfg.playerCount) {
        g_cfg.localSeat = 0;
    }
    g_cfg.testBot = EnvInt("TH07_MP_TEST_BOT", 0) != 0;
    g_cfg.testRules = EnvInt("TH07_MP_TEST_RULES", 0) != 0;
    g_cfg.testKeepAlive = EnvInt("TH07_MP_TEST_KEEP_ALIVE", 0) != 0;
    g_cfg.showStageNames = EnvInt("TH07_MP_STAGE_NAMES", 0) != 0;
    g_cfg.testTitleBot = EnvInt("TH07_MP_TEST_TITLE_BOT", 0) != 0;
    g_cfg.testCampaign = EnvInt("TH07_MP_TEST_CAMPAIGN", 0) != 0;
    g_cfg.testBotIdle = static_cast<unsigned>(EnvInt("TH07_MP_TEST_BOT_IDLE", 0)) & ((1u << g_cfg.playerCount) - 1);
    g_cfg.testPredictAlways = EnvInt("TH07_MP_TEST_PREDICT_ALWAYS", 0) != 0;
    g_cfg.talkConfirmed = EnvInt("TH07_MP_ROLLBACK_TALK_CONFIRMED", 0) != 0;
    g_cfg.testBotMash = EnvInt("TH07_MP_TEST_BOT_MASH", 0) != 0;
    g_cfg.testBotNoShot = EnvInt("TH07_MP_TEST_BOT_NO_SHOT", 0) != 0;
    g_cfg.testMenuSeat = EnvInt("TH07_MP_TEST_MENU_SEAT", -1);
    if (g_cfg.testMenuSeat < 0 || g_cfg.testMenuSeat >= g_cfg.playerCount) {
        g_cfg.testMenuSeat = -1;
    }
    g_cfg.testStageClearFrame = EnvUnsigned("TH07_MP_TEST_STAGE_CLEAR_FRAME", kNoFrame);
    g_cfg.testStageClearLast = EnvInt("TH07_MP_TEST_STAGE_CLEAR_LAST", 1);
    g_cfg.testGhostFrame = EnvUnsigned("TH07_MP_TEST_GHOST", kNoFrame);
    g_cfg.testGive = EnvInt("TH07_MP_TEST_GIVE", 0);
    EnvList("TH07_MP_TEST_CHARACTERS", g_cfg.testCharacters, 0, 2);
    EnvList("TH07_MP_TEST_SHOTTYPES", g_cfg.testShotTypes, 0, 1);
    g_cfg.testStartStage = EnvInt("TH07_MP_TEST_START_STAGE", 1);
    if (g_cfg.testStartStage < 1 || g_cfg.testStartStage > 6) {
        g_cfg.testStartStage = 1;
    }
    int menuDelay = EnvInt("TH07_MP_MENU_INPUT_DELAY", 4);
    if (menuDelay < 0) {
        menuDelay = 0;
    } else if (menuDelay > 12) {
        menuDelay = 12;
    }
    g_cfg.menuInputDelay = static_cast<unsigned>(menuDelay);
    int delay = EnvInt("TH07_MP_TEST_DELAY", 0);
    if (delay < 0) {
        delay = 0;
    } else if (delay > 12) {
        delay = 12;
    }
    g_cfg.artificialDelay = static_cast<unsigned>(delay);
    g_cfg.sessionId = EnvUnsigned("TH07_MP_SESSION", g_cfg.sessionId);
    if (!EnvText("TH07_MP_BIND", g_cfg.bindText, sizeof(g_cfg.bindText))) {
        _snprintf_s(g_cfg.bindText, sizeof(g_cfg.bindText), _TRUNCATE, "127.0.0.1:%u", 28020u + g_cfg.localSeat);
    }
    if (!EnvText("TH07_MP_PEER", g_cfg.peerText, sizeof(g_cfg.peerText))) {
        _snprintf_s(g_cfg.peerText, sizeof(g_cfg.peerText), _TRUNCATE, "127.0.0.1:%u",
                    g_cfg.localSeat == 0 ? 28021u : 28020u);
    }
    size_t logLength = 0;
    if (_wgetenv_s(&logLength, g_cfg.logPath, _countof(g_cfg.logPath), L"TH07_MP_LOG") != 0) {
        g_cfg.logPath[0] = 0;
    }
    g_cfg.rollback = EnvInt("TH07_MP_ROLLBACK", 0) != 0;
    g_cfg.rollbackWindow = static_cast<unsigned>(EnvInt("TH07_MP_ROLLBACK_WINDOW", 8));
    static const char* const nameVars[kMaxPlayers] = {"TH07_MP_P1_NAME", "TH07_MP_P2_NAME", "TH07_MP_P3_NAME",
                                                      "TH07_MP_P4_NAME"};
    for (int seat = 0; seat < PlayerCount(); ++seat) {
        char name[64] = {};
        EnvName(nameVars[seat], name, sizeof(name));
        CleanMultiplayerPlayerName(g_cfg.playerName[seat], name, sizeof(name), seat);
    }
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    static bool once;
    if (once || ep == 0 || ep->ExceptionRecord == 0) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    once = true;
    const unsigned base = reinterpret_cast<unsigned>(GetModuleHandleW(nullptr));
    const unsigned at = reinterpret_cast<unsigned>(ep->ExceptionRecord->ExceptionAddress);
    void* frames[8] = {};
    const unsigned short got = RtlCaptureStackBackTrace(0, 8, frames, nullptr);
    char line[512];
    int k = _snprintf_s(line, sizeof(line), _TRUNCATE, "CRASH code=%08X at=%08X +%06X base=%08X",
                        static_cast<unsigned>(ep->ExceptionRecord->ExceptionCode), at, at - base, base);
    if (ep->ExceptionRecord->NumberParameters >= 2 &&
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %s addr=%08X",
                         ep->ExceptionRecord->ExceptionInformation[0] != 0 ? "write" : "read",
                         static_cast<unsigned>(ep->ExceptionRecord->ExceptionInformation[1]));
    }
    // The game keeps frame pointers (/Oy-): walk ebp from the faulting context.
    k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " ebp_chain");
    if (ep->ContextRecord != 0) {
        const unsigned* frame = reinterpret_cast<const unsigned*>(ep->ContextRecord->Ebp);
        for (int i = 0; i < 12 && k > 0 && k < 470; ++i) {
            if (frame == 0 || IsBadReadPtr(frame, 8)) {
                break;
            }
            k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " +%06X", frame[1] - base);
            const unsigned* next = reinterpret_cast<const unsigned*>(frame[0]);
            if (next <= frame) {
                break;
            }
            frame = next;
        }
    }
    (void)got;
    LogRaw(line);
    OutputDebugStringA(line);
    return EXCEPTION_EXECUTE_HANDLER;
}

}

const Config& Cfg()
{
    InitConfig();
    return g_cfg;
}

void RestoreConfig(const Config& config) { g_cfg = config; }

void ConfigureReplay(const replay::Settings& settings)
{
    InitConfig();
    g_cfg.mode = kLocal;
    g_cfg.localSeat = settings.viewSeat;
    g_cfg.playerCount = settings.players;
    g_cfg.sessionId = settings.session;
    g_cfg.rollback = false;
    g_cfg.testBot = g_cfg.testTitleBot = g_cfg.testRules = g_cfg.testCampaign = false;
    g_cfg.testKeepAlive = settings.keepAlive != 0;
    g_cfg.testStartStage = settings.startStage;
    g_cfg.testStageClearFrame = settings.clearFrame;
    g_cfg.testStageClearLast = settings.clearLast;
    g_cfg.testGhostFrame = settings.ghostFrame;
    for (int seat = 0; seat < kMaxPlayers; ++seat) {
        g_cfg.testCharacters[seat] = settings.characters[seat];
        g_cfg.testShotTypes[seat] = settings.shots[seat];
        CleanMultiplayerPlayerName(g_cfg.playerName[seat], settings.names[seat], sizeof(settings.names[seat]), seat);
    }
}

bool UdpEnabled()
{
    InitConfig();
    return g_cfg.mode == kUdp;
}

bool LocalEnabled()
{
    InitConfig();
    return g_cfg.mode == kLocal;
}

int PlayerCount()
{
    InitConfig();
    return g_cfg.playerCount;
}

int LocalSeat()
{
    InitConfig();
    return g_cfg.localSeat;
}

bool AdoptPreparedUdpSocket(uintptr_t socketHandle, uint32_t peerIpv4, uint16_t peerPort, int localSeat,
                            uint32_t sessionId, unsigned inputDelay, int playerCount, const Endpoint* guests)
{
    InitConfig();
    const uintptr_t invalidSocket = ~static_cast<uintptr_t>(0); // INVALID_SOCKET
    if (g_netInitialized || g_preparedUdp.valid || socketHandle == invalidSocket || peerIpv4 == 0 ||
        peerPort == 0 || playerCount < 2 || playerCount > kMaxPlayers || localSeat < 0 ||
        localSeat >= playerCount || (localSeat == 0 && playerCount > 2 && guests == nullptr) || sessionId == 0 ||
        inputDelay > 12) {
        return false;
    }
    g_cfg.mode = kUdp;
    g_cfg.localSeat = localSeat;
    g_cfg.playerCount = playerCount;
    g_cfg.sessionId = sessionId;
    g_cfg.artificialDelay = inputDelay;
    g_preparedUdp.valid = true;
    g_preparedUdp.handle = socketHandle;
    g_preparedUdp.peerIpv4 = peerIpv4;
    g_preparedUdp.peerPort = peerPort;
    if (guests) {
        for (int s = 1; s < playerCount; ++s) {
            g_preparedUdp.guests[s] = guests[s];
        }
    } else {
        g_preparedUdp.guests[1].ipv4 = peerIpv4;
        g_preparedUdp.guests[1].port = peerPort;
    }
    return true;
}

bool TakePreparedUdpSocket(PreparedUdpSocket* out)
{
    g_netInitialized = true;
    if (!g_preparedUdp.valid) {
        return false;
    }
    if (out != nullptr) {
        *out = g_preparedUdp;
    }
    g_preparedUdp = PreparedUdpSocket();
    return true;
}

void SetPlayerName(int seat, const char* name)
{
    InitConfig();
    if (seat < 0 || seat >= kMaxPlayers) {
        return;
    }
    CleanMultiplayerPlayerName(g_cfg.playerName[seat], name, kMultiplayerPlayerNameBytes, seat);
}

const char* PlayerName(int seat)
{
    InitConfig();
    return seat >= 0 && seat < PlayerCount() ? g_cfg.playerName[seat] : "";
}

void SetPlayerNames(const char* p1, const char* p2)
{
    InitConfig();
    CleanMultiplayerPlayerName(g_cfg.playerName[0], p1, kMultiplayerPlayerNameBytes, 0);
    CleanMultiplayerPlayerName(g_cfg.playerName[1], p2, kMultiplayerPlayerNameBytes, 1);
}

void Log(const char* format, ...)
{
    char line[640];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
    va_end(args);
    char debug[700];
    _snprintf_s(debug, sizeof(debug), _TRUNCATE, "[th07-mp] %s\n", line);
    OutputDebugStringA(debug);
    InitConfig();
    if (g_cfg.logPath[0] != 0) {
        // One shared handle: a reader never makes a write fail.
        static HANDLE s_file = INVALID_HANDLE_VALUE;
        if (s_file == INVALID_HANDLE_VALUE) {
            s_file = CreateFileW(g_cfg.logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        }
        if (s_file != INVALID_HANDLE_VALUE) {
            char text[660];
            _snprintf_s(text, sizeof(text), _TRUNCATE, "%s\r\n", line);
            DWORD written = 0;
            WriteFile(s_file, text, (DWORD)strlen(text), &written, 0);
        }
    }
}

void LogRaw(const char* text)
{
    char line[640];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%s", text != 0 ? text : "");
    size_t n = strlen(line);
    while (n != 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[--n] = 0;
    }
    Log("%s", line);
}

// These end the process before the unhandled exception filter runs.
LONG WINAPI FatalFirstChance(EXCEPTION_POINTERS* ep)
{
    if (ep != 0 && ep->ExceptionRecord != 0) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code == 0xC0000374 || code == 0xC0000409 || code == 0xC0000417) {
            CrashFilter(ep);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashFilter()
{
    SetUnhandledExceptionFilter(CrashFilter);
    AddVectoredExceptionHandler(1, FatalFirstChance);
}

}
}
