#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "Supervisor.hpp"
#include "multi/Launcher.h"
#include "multi/Lobby.h"
#include "multi/MpConfig.h"

namespace th07 {
namespace launcher {
namespace {

enum Control {
    kRoleHost = 100,
    kRoleGuest,
    kHostLabel,
    kHostEdit,
    kPortEdit,
    kDelayLabel,
    kDelayEdit,
    kRollback,
    kDisplay640 = 108,
    kDisplay960,
    kDisplay1280,
    kDisplayFullscreen,
    kBgm,
    kSe,
    kBot,
    kStartNetwork,
    kStartLocal,
    kStartSingle,
    kStartGame,
    kStatus,
    kRoster,
    kCancel,
    kPlayerNameLabel,
    kPlayerName,
    kLanguageLabel = 131,
    kLanguageCombo,
    kConnectAsLabel,
    kDisplayGroup,
    kAudioGroup,
    kControlGroup,
    kPortLabel,
    kPlayersLabel,
    kDelayHint,
    kPlayersCombo,
    kStageNames,
};

enum Language {
    kEnglish,
    kJapanese,
    kChinese,
    kLanguageCount,
};

enum TextId {
    kTextLanguageName,
    kTextWindowTitle,
    kTextLanguageLabel,
    kTextConnectAs,
    kTextRoleHost,
    kTextRoleGuest,
    kTextDisplayMode,
    kTextDisplay640,
    kTextDisplay960,
    kTextDisplay1280,
    kTextDisplayFullscreen,
    kTextAudio,
    kTextBgm,
    kTextSe,
    kTextControl,
    kTextBot,
    kTextRollback,
    kTextPlayerName,
    kTextHostIp,
    kTextUdpPort,
    kTextInputDelay,
    kTextDelayHint,
    kTextDelayHintRollback,
    kTextPlayers,
    kTextStartHosting,
    kTextConnectToHost,
    kTextStartLocal,
    kTextSinglePlayer,
    kTextStartGame,
    kTextCancel,
    kTextStopSearch,
    kTextStatusReady,
    kTextStatusEnterHost,
    kTextStatusWaitHost,
    kTextButtonConnecting,
    kTextButtonHosting,
    kTextStatusConnecting,
    kTextStatusHosting,
    kTextStatusConnectedHost,
    kTextStatusConnectedGuest,
    kTextStatusStartingAck,
    kTextStatusStartingGame,
    kTextStatusCancelled,
    kTextErrorWinsock,
    kTextErrorSocket,
    kTextErrorPortUsed,
    kTextErrorNonblock,
    kTextErrorHost,
    kTextErrorProtocol,
    kTextErrorPeerStopped,
    kTextErrorTimeout,
    kTextErrorHandoff,
    kTextStageNames,
    kTextCount,
};

const wchar_t* const kText[kTextCount][kLanguageCount] = {
    {L"English", L"日本語", L"中文"},
    {L"th07 multiplayer - Connection", L"th07 multiplayer - 接続", L"th07 multiplayer - 连接"},
    {L"Language:", L"言語:", L"语言:"},
    {L"Connect as:", L"接続方法:", L"连接方式:"},
    {L"Host", L"ホスト", L"主机"},
    {L"Guest", L"ゲスト", L"客机"},
    {L"Display mode", L"画面モード", L"显示模式"},
    {L"640 x 480", L"640 x 480", L"640 x 480"},
    {L"960 x 720", L"960 x 720", L"960 x 720"},
    {L"1280 x 960 (recommended)", L"1280 x 960 (推奨)", L"1280 x 960 (推荐)"},
    {L"Fullscreen", L"フルスクリーン", L"全屏"},
    {L"Audio", L"音声", L"音频"},
    {L"BGM", L"BGM", L"背景音乐"},
    {L"Sound effects (SE)", L"効果音 (SE)", L"音效 (SE)"},
    {L"Control", L"操作", L"操作"},
    {L"Use BOT on this PC", L"この PC で BOT を使う", L"在本机使用 BOT"},
    {L"Rollback (delay 0)", L"ロールバック (遅延 0)", L"回滚 (延迟 0)"},
    {L"Player name:", L"プレイヤー名:", L"玩家名称:"},
    {L"Host name / IP:", L"ホスト名 / IP:", L"主机名 / IP:"},
    {L"UDP port:", L"UDP ポート:", L"UDP 端口:"},
    {L"Input delay:", L"入力遅延:", L"输入延迟:"},
    {L"(fixed lockstep)", L"(固定ロックステップ)", L"(固定同步)"},
    {L"(rollback uses 0)", L"(ロールバックは 0)", L"(回滚使用 0)"},
    {L"Players:", L"人数:", L"人数:"},
    {L"Start hosting", L"ホストを開始", L"开始主机"},
    {L"Connect to host", L"ホストに接続", L"连接到主机"},
    {L"Start Game (local)", L"ゲーム開始 (ローカル)", L"开始游戏 (本地)"},
    {L"Single player", L"一人プレイ", L"单人游戏"},
    {L"Start Game", L"ゲーム開始", L"开始游戏"},
    {L"Cancel", L"キャンセル", L"取消"},
    {L"Stop search", L"検索を中止", L"停止搜索"},
    {L"Ready", L"準備完了", L"准备就绪"},
    {L"Enter the Host name or IPv4 address.", L"ホスト名または IPv4 アドレスを入力してください。",
     L"请输入主机名或 IPv4 地址。"},
    {L"Waiting for a guest after Start hosting.", L"「ホストを開始」を押すとゲストを待ちます。",
     L"点击「开始主机」后等待客机。"},
    {L"Connecting...", L"接続中...", L"连接中..."},
    {L"Hosting...", L"ホスト中...", L"主机启动中..."},
    {L"Connecting to host...", L"ホストに接続しています...", L"正在连接主机..."},
    {L"Hosting; waiting for guest (0/1).", L"ホスト中; ゲストを待っています (0/1)。", L"正在等待客机 (0/1)。"},
    {L"Connected; press Start Game.", L"接続しました。「ゲーム開始」を押してください。", L"已连接，请点击「开始游戏」。"},
    {L"Connected; waiting for Host Start Game.", L"接続しました。ホストの開始を待っています。",
     L"已连接，等待主机开始游戏。"},
    {L"Starting game; waiting for guest ACK...", L"ゲーム開始中; ゲストの応答を待っています...",
     L"正在开始游戏; 等待客机确认..."},
    {L"Starting game...", L"ゲームを開始しています...", L"正在开始游戏..."},
    {L"Search cancelled.", L"検索を中止しました。", L"已停止搜索。"},
    {L"Winsock startup failed.", L"Winsock の初期化に失敗しました。", L"Winsock 初始化失败。"},
    {L"Could not create UDP socket.", L"UDP ソケットを作成できませんでした。", L"无法创建 UDP 套接字。"},
    {L"UDP port is already in use.", L"UDP ポートは既に使用されています。", L"UDP 端口已被占用。"},
    {L"Could not make UDP socket nonblocking.", L"UDP ソケットをノンブロッキングにできませんでした。",
     L"无法将 UDP 套接字设为非阻塞。"},
    {L"Host name or IPv4 address could not be resolved.", L"ホスト名または IPv4 アドレスを解決できませんでした。",
     L"无法解析主机名或 IPv4 地址。"},
    {L"The peer uses an incompatible build.", L"相手のマルチプレイ版に互換性がありません。",
     L"对方的多人游戏版本不兼容。"},
    {L"The peer stopped the connection.", L"相手が接続を終了しました。", L"对方已停止连接。"},
    {L"Connection timed out.", L"接続がタイムアウトしました。", L"连接超时。"},
    {L"Could not hand the UDP socket to the game.", L"UDP ソケットをゲームへ引き渡せませんでした。",
     L"无法将 UDP 套接字交给游戏。"},
    {L"Stage intro names", L"開始時に名前を表示", L"开场显示名称"},
};

enum LobbyResult {
    kLobbySearching,
    kLobbyConnected,
    kLobbyReady,
    kLobbyFailed,
};

struct Lobby {
    SOCKET socket;
    sockaddr_in peer;
    sockaddr_in guests[4];
    unsigned receivedAt[4];
    unsigned connectedMask;
    unsigned ackMask;
    int playerCount;
    int localSeat;
    char roster[4][16];
    bool winsock;
    bool active;
    bool host;
    bool connected;
    bool startRequested;
    bool protocolMismatch;
    bool peerCancelled;
    uint32_t sessionId;
    uint32_t serial;
    uint32_t startedAt;
    uint32_t lastSend;
    uint32_t lastReceive;
    unsigned inputDelay;
    bool rollback;
    char localName[16];
    char peerName[16];
};

struct Ui {
    HWND window;
    HWND host;
    HWND name;
    HWND port;
    HWND delay;
    HWND status;
    HWND startNetwork;
    HWND startGame;
    HWND cancel;
    HWND roster;
    HWND languageCombo;
    HWND playersCombo;
    HFONT font;
    Selection* result;
    Language language;
    bool done;
    bool networkAttempting;
    bool networkConnected;
    bool settingsReady;
    bool ownRollback;
    int lockstepDelay;
};

const UINT_PTR kTimerId = 1;
const UINT kTimerMs = 15;
const wchar_t kWindowClass[] = L"th07_multiplayer_launcher";
const wchar_t kSettingsSection[] = L"launcher";

Ui g_ui = {};
Lobby g_lobby = {INVALID_SOCKET};
Selection g_launchSelection = {};
bool g_hasLaunchSelection;
bool g_audioApplied;
char g_savedBgm;
char g_savedSe;
wchar_t g_settingsPath[MAX_PATH] = {};

const wchar_t* Text(TextId id)
{
    return kText[id][g_ui.language];
}

void Copy(char* destination, size_t room, const char* source)
{
    if (room == 0) {
        return;
    }
    size_t i = 0;
    while (i + 1 < room && source != nullptr && source[i] != 0) {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = 0;
}

void Append(char* destination, size_t room, const char* source)
{
    size_t used = 0;
    while (used < room && destination[used] != 0) {
        ++used;
    }
    if (used < room) {
        Copy(destination + used, room - used, source);
    }
}

void SetText(HWND control, const wchar_t* text)
{
    if (control != nullptr) {
        SetWindowTextW(control, text != nullptr ? text : L"");
    }
}

void SetNumberText(HWND control, unsigned value)
{
    wchar_t text[16];
    swprintf_s(text, L"%u", value);
    SetText(control, text);
}

HFONT MakeFont(Language language)
{
    const wchar_t* face = language == kJapanese  ? L"Yu Gothic UI"
                          : language == kChinese ? L"Microsoft YaHei UI"
                                                 : L"Segoe UI";
    const DWORD charset = language == kJapanese ? SHIFTJIS_CHARSET : language == kChinese ? GB2312_CHARSET : DEFAULT_CHARSET;
    return CreateFontW(language == kEnglish ? -11 : -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, charset,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

// Not wcscat_s: its overflow ends the process.
bool AppendPath(wchar_t* path, const wchar_t* tail)
{
    const size_t used = wcslen(path);
    const size_t more = wcslen(tail);
    if (used + more >= MAX_PATH) {
        return false;
    }
    memcpy(path + used, tail, (more + 1) * sizeof(wchar_t));
    return true;
}

bool UnderFolder(const wchar_t* path, const wchar_t* variable)
{
    wchar_t folder[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(variable, folder, MAX_PATH);
    return length != 0 && length < MAX_PATH && _wcsnicmp(path, folder, length) == 0 &&
           (path[length] == L'\\' || path[length] == 0);
}

bool ExeFilePath(const wchar_t* name, wchar_t* path)
{
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* const slash = length != 0 && length < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    if (slash == nullptr) {
        return false;
    }
    slash[1] = 0;
    return AppendPath(path, name);
}

bool OpenFileForWriting(const wchar_t* path, bool truncate)
{
    const HANDLE file =
        CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, truncate ? CREATE_ALWAYS : OPEN_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(file);
    return true;
}

// Beside the executable unless that is a protected folder (writes would land in VirtualStore);
// else %APPDATA%\ShanghaiAlice\th07.
bool OpenDataFile(const wchar_t* name, bool truncate, wchar_t* path, bool* besideExe)
{
    if (besideExe != nullptr) {
        *besideExe = false;
    }
    if (ExeFilePath(L"", path)) {
        static const wchar_t* const protectedFolders[] = {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432",
                                                          L"ProgramData", L"SystemRoot"};
        bool beside = true;
        for (const wchar_t* folder : protectedFolders) {
            beside = beside && !UnderFolder(path, folder);
        }
        if (beside && AppendPath(path, name) && OpenFileForWriting(path, truncate)) {
            if (besideExe != nullptr) {
                *besideExe = true;
            }
            return true;
        }
    }
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH || !AppendPath(path, L"\\ShanghaiAlice")) {
        return false;
    }
    CreateDirectoryW(path, nullptr);
    if (!AppendPath(path, L"\\th07")) {
        return false;
    }
    CreateDirectoryW(path, nullptr);
    return AppendPath(path, L"\\") && AppendPath(path, name) && OpenFileForWriting(path, truncate);
}

void ResolveSettingsPath()
{
    bool besideExe = false;
    if (!OpenDataFile(L"th07_coop.ini", false, g_settingsPath, &besideExe)) {
        g_settingsPath[0] = 0;
        return;
    }
    wchar_t probe[8] = {};
    wchar_t old[MAX_PATH];
    if (!besideExe && GetPrivateProfileSectionW(kSettingsSection, probe, 8, g_settingsPath) == 0 &&
        ExeFilePath(L"th07_coop.ini", old)) {
        static wchar_t section[8192];
        const DWORD got =
            GetPrivateProfileSectionW(kSettingsSection, section, static_cast<DWORD>(sizeof(section) / sizeof(section[0])), old);
        if (got > 0) {
            WritePrivateProfileSectionW(kSettingsSection, section, g_settingsPath);
        }
    }
}

void SettingText(const char* key, const char* fallback, char* output, unsigned room)
{
    if (g_settingsPath[0] == 0) {
        Copy(output, room, fallback);
        return;
    }
    wchar_t wideKey[64] = {};
    wchar_t wideFallback[256] = {};
    wchar_t wideValue[256] = {};
    MultiByteToWideChar(CP_ACP, 0, key, -1, wideKey, 64);
    MultiByteToWideChar(CP_ACP, 0, fallback, -1, wideFallback, 256);
    GetPrivateProfileStringW(kSettingsSection, wideKey, wideFallback, wideValue, 256, g_settingsPath);
    if (WideCharToMultiByte(CP_ACP, 0, wideValue, -1, output, static_cast<int>(room), nullptr, nullptr) == 0) {
        Copy(output, room, fallback);
    }
}

int SettingNumber(const char* key, int fallback, int low, int high)
{
    char fallbackText[24];
    _snprintf_s(fallbackText, sizeof(fallbackText), _TRUNCATE, "%d", fallback);
    char text[24] = {};
    SettingText(key, fallbackText, text, sizeof(text));
    const int value = atoi(text);
    return value >= low && value <= high ? value : fallback;
}

void SaveText(const char* key, const char* value)
{
    if (g_settingsPath[0] != 0) {
        wchar_t wideKey[64] = {};
        wchar_t wideValue[256] = {};
        MultiByteToWideChar(CP_ACP, 0, key, -1, wideKey, 64);
        MultiByteToWideChar(CP_ACP, 0, value, -1, wideValue, 256);
        WritePrivateProfileStringW(kSettingsSection, wideKey, wideValue, g_settingsPath);
    }
}

void SaveNumber(const char* key, int value)
{
    char text[24];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%d", value);
    SaveText(key, text);
}

bool HasEnvironment(const char* name, char* value, DWORD room)
{
    const DWORD length = GetEnvironmentVariableA(name, value, room);
    return length != 0 && length < room;
}

void PutEnvironment(const char* name, const char* value)
{
    const char* actual = value != nullptr && value[0] != 0 ? value : "";
    // Update both the Win32 and the CRT environment: the config reads getenv_s.
    _putenv_s(name, actual);
    SetEnvironmentVariableA(name, actual[0] != 0 ? actual : nullptr);
}

unsigned ReadNumber(HWND edit, unsigned fallback, unsigned low, unsigned high)
{
    char text[24];
    GetWindowTextA(edit, text, sizeof(text));
    char* end = nullptr;
    const unsigned long value = strtoul(text, &end, 10);
    return end != text && *end == 0 && value >= low && value <= high ? static_cast<unsigned>(value) : fallback;
}

void ReadPlayerName(char* output, int seat)
{
    wchar_t wide[64] = {};
    char bytes[128] = {};
    if (g_ui.name != nullptr) {
        GetWindowTextW(g_ui.name, wide, static_cast<int>(sizeof(wide) / sizeof(wide[0])));
    }
    if (WideCharToMultiByte(932, 0, wide, -1, bytes, sizeof(bytes), "?", nullptr) <= 0) {
        bytes[0] = 0;
    }
    CleanMultiplayerPlayerName(output, bytes, sizeof(bytes), seat);
}

HWND AddControl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id)
{
    HWND control = CreateWindowExW(0, cls, text != nullptr ? text : L"", WS_CHILD | WS_VISIBLE | style, x, y, width,
                                   height, g_ui.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    if (control != nullptr && g_ui.font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font), TRUE);
    }
    return control;
}

bool SameEndpoint(const sockaddr_in& a, const sockaddr_in& b)
{
    return a.sin_family == b.sin_family && a.sin_port == b.sin_port && a.sin_addr.s_addr == b.sin_addr.s_addr;
}

void LauncherLog(const char* format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
    va_end(args);
    char debug[580];
    _snprintf_s(debug, sizeof(debug), _TRUNCATE, "[th07-launcher] %s\n", line);
    OutputDebugStringA(debug);
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"TH07_MP_LOG", path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"at") == 0 && file != nullptr) {
        fprintf(file, "LAUNCHER %s\n", line);
        fclose(file);
    }
}

void CloseLobbySocket()
{
    if (g_lobby.socket != INVALID_SOCKET) {
        closesocket(g_lobby.socket);
        g_lobby.socket = INVALID_SOCKET;
    }
    if (g_lobby.winsock) {
        WSACleanup();
        g_lobby.winsock = false;
    }
}

void ClearLobby()
{
    CloseLobbySocket();
    g_lobby = Lobby();
    g_lobby.socket = INVALID_SOCKET;
}

void FillLobbyPacket(MultiplayerLobbyPacket* packet, MultiplayerLobbyKind kind)
{
    *packet = MultiplayerLobbyPacket();
    packet->magic = kMultiplayerLobbyMagic;
    packet->version = kMultiplayerLobbyVersion;
    packet->bytes = sizeof(*packet);
    packet->kind = kind;
    packet->sessionId = g_lobby.sessionId;
    packet->serial = ++g_lobby.serial;
    packet->senderSeat = g_lobby.localSeat;
    packet->playerCount = g_lobby.playerCount;
    packet->assignedSeat = g_lobby.localSeat;
    packet->connectedMask = g_lobby.connectedMask;
    memcpy(packet->roster, g_lobby.roster, sizeof(packet->roster));
    packet->rollbackEnabled = g_lobby.rollback ? 1u : 0u;
    packet->inputDelay = static_cast<uint16_t>(g_lobby.inputDelay);
    Copy(packet->playerName, sizeof(packet->playerName), g_lobby.localName);
}

bool AllLobbySeats(unsigned mask)
{
    return mask == (1u << g_lobby.playerCount) - 1;
}

void SendLobbyPacket(MultiplayerLobbyKind kind, int repeat = 1)
{
    if (g_lobby.socket == INVALID_SOCKET) {
        return;
    }
    const int first = g_lobby.host ? 1 : 0;
    const int last = g_lobby.host ? g_lobby.playerCount : 1;
    for (int seat = first; seat < last; ++seat) {
        const sockaddr_in& endpoint = g_lobby.host ? g_lobby.guests[seat] : g_lobby.peer;
        if (!endpoint.sin_addr.s_addr) {
            continue;
        }
        MultiplayerLobbyPacket packet;
        FillLobbyPacket(&packet, kind);
        packet.assignedSeat = g_lobby.host ? seat : g_lobby.localSeat;
        for (int i = 0; i < repeat; ++i) {
            sendto(g_lobby.socket, reinterpret_cast<const char*>(&packet), sizeof(packet), 0,
                   reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));
        }
    }
    g_lobby.lastSend = GetTickCount();
}

bool ResolveHost(const char* text, unsigned port, sockaddr_in* output)
{
    char service[16];
    _snprintf_s(service, sizeof(service), _TRUNCATE, "%u", port);
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* result = nullptr;
    if (getaddrinfo(text, service, &hints, &result) != 0 || result == nullptr ||
        result->ai_addrlen < static_cast<int>(sizeof(sockaddr_in))) {
        if (result != nullptr) {
            freeaddrinfo(result);
        }
        return false;
    }
    *output = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
    freeaddrinfo(result);
    return true;
}

bool OpenLobby(bool guest, const char* address, unsigned port, const char* localName, bool rollback,
               unsigned inputDelay, int playerCount, TextId* error)
{
    ClearLobby();
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        *error = kTextErrorWinsock;
        return false;
    }
    g_lobby.winsock = true;
    g_lobby.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_lobby.socket == INVALID_SOCKET) {
        *error = kTextErrorSocket;
        ClearLobby();
        return false;
    }
    BOOL exclusive = TRUE;
    setsockopt(g_lobby.socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof(exclusive));
    u_long nonblocking = 1;
    if (ioctlsocket(g_lobby.socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        *error = kTextErrorNonblock;
        ClearLobby();
        return false;
    }
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = guest ? 0 : htons(static_cast<unsigned short>(port));
    if (bind(g_lobby.socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        *error = kTextErrorPortUsed;
        ClearLobby();
        return false;
    }
    if (guest && !ResolveHost(address, port, &g_lobby.peer)) {
        *error = kTextErrorHost;
        ClearLobby();
        return false;
    }
    g_lobby.active = true;
    g_lobby.host = !guest;
    g_lobby.playerCount = playerCount;
    g_lobby.localSeat = guest ? 1 : 0;
    g_lobby.connectedMask = guest ? 0u : 1u;
    g_lobby.ackMask = 1;
    if (!guest) {
        Copy(g_lobby.roster[0], 16, localName);
    }
    g_lobby.rollback = rollback || (!guest && playerCount > 2);
    g_lobby.inputDelay = g_lobby.rollback ? 0u : inputDelay;
    Copy(g_lobby.localName, sizeof(g_lobby.localName), localName);
    g_lobby.sessionId = guest ? 0u : (GetTickCount() ^ GetCurrentProcessId() * 0x45D9F3Bu ^ 0x20260913u);
    if (!guest && g_lobby.sessionId == 0) {
        g_lobby.sessionId = 1;
    }
    g_lobby.startedAt = g_lobby.lastReceive = GetTickCount();
    if (guest) {
        SendLobbyPacket(kMultiplayerLobbyHello);
    }
    LauncherLog("LOBBY_BEGIN role=%s port=%u rollback=%u delay=%u", guest ? "guest" : "host", port,
                g_lobby.rollback ? 1u : 0u, g_lobby.inputDelay);
    return true;
}

void CancelLobby()
{
    if (g_lobby.active && g_lobby.connected) {
        SendLobbyPacket(kMultiplayerLobbyCancel, 4);
    }
    LauncherLog("LOBBY_CANCEL role=%s", g_lobby.host ? "host" : "guest");
    ClearLobby();
}

LobbyResult PollLobby()
{
    if (!g_lobby.active) {
        return kLobbyFailed;
    }
    for (int i = 0; i < 192; ++i) {
        MultiplayerLobbyPacket packet = {};
        sockaddr_in from = {};
        int fromBytes = sizeof(from);
        const int got = recvfrom(g_lobby.socket, reinterpret_cast<char*>(&packet), sizeof(packet), 0,
                                 reinterpret_cast<sockaddr*>(&from), &fromBytes);
        if (got == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAECONNRESET || error == WSAEMSGSIZE) {
                break;
            }
            return kLobbyFailed;
        }
        if (got < 8 || packet.magic != kMultiplayerLobbyMagic) {
            continue;
        }
        if (!g_lobby.host && !SameEndpoint(from, g_lobby.peer)) {
            continue;
        }
        if (packet.version != kMultiplayerLobbyVersion || packet.bytes != sizeof(packet)) {
            g_lobby.protocolMismatch = true;
            return kLobbyFailed;
        }
        if (got != sizeof(packet)) {
            continue;
        }
        const unsigned now = GetTickCount();
        if (g_lobby.host) {
            int seat = -1;
            for (int s = 1; s < g_lobby.playerCount; ++s) {
                if ((g_lobby.connectedMask & (1u << s)) && SameEndpoint(from, g_lobby.guests[s])) {
                    seat = s;
                }
            }
            if (packet.kind == kMultiplayerLobbyHello) {
                if (seat < 0 && !g_lobby.startRequested) {
                    for (int s = 1; s < g_lobby.playerCount; ++s) {
                        if (!(g_lobby.connectedMask & (1u << s))) {
                            seat = s;
                            break;
                        }
                    }
                }
                if (seat < 0) {
                    continue;
                }
                const bool joined = (g_lobby.connectedMask & (1u << seat)) == 0;
                g_lobby.guests[seat] = from;
                g_lobby.connectedMask |= 1u << seat;
                g_lobby.connected = true;
                g_lobby.receivedAt[seat] = g_lobby.lastReceive = now;
                g_lobby.peer = g_lobby.guests[1];
                CleanMultiplayerPlayerName(g_lobby.roster[seat], packet.playerName, sizeof(packet.playerName), seat);
                if (!g_lobby.startRequested) {
                    SendLobbyPacket(kMultiplayerLobbyWelcome);
                }
                if (joined) {
                    LauncherLog("LOBBY_GUEST_JOIN seat=%d name=%s players=%d session=%08X", seat,
                                g_lobby.roster[seat], g_lobby.playerCount, g_lobby.sessionId);
                }
                continue;
            }
            if (seat < 0 || packet.sessionId != g_lobby.sessionId || packet.senderSeat != static_cast<uint32_t>(seat)) {
                continue;
            }
            g_lobby.receivedAt[seat] = g_lobby.lastReceive = now;
            if (packet.kind == kMultiplayerLobbyStartAck && g_lobby.startRequested &&
                packet.playerCount == static_cast<uint32_t>(g_lobby.playerCount)) {
                g_lobby.ackMask |= 1u << seat;
                if (AllLobbySeats(g_lobby.ackMask)) {
                    LauncherLog("LOBBY_READY role=host start_ack=%u players=%d", g_lobby.ackMask, g_lobby.playerCount);
                    return kLobbyReady;
                }
            }
        } else {
            if (packet.kind == kMultiplayerLobbyWelcome || packet.kind == kMultiplayerLobbyStart) {
                if (!packet.sessionId || packet.playerCount < 2 || packet.playerCount > 4 || packet.assignedSeat == 0 ||
                    packet.assignedSeat >= packet.playerCount || (packet.playerCount > 2 && !packet.rollbackEnabled) ||
                    (g_lobby.connected && packet.sessionId != g_lobby.sessionId)) {
                    continue;
                }
                g_lobby.sessionId = packet.sessionId;
                g_lobby.localSeat = packet.assignedSeat;
                g_lobby.playerCount = packet.playerCount;
                g_lobby.connectedMask = packet.connectedMask;
                g_lobby.connected = true;
                g_lobby.lastReceive = now;
                g_lobby.rollback = packet.rollbackEnabled != 0;
                g_lobby.inputDelay = g_lobby.rollback ? 0 : (packet.inputDelay > 12 ? 12 : packet.inputDelay);
                for (int seat = 0; seat < g_lobby.playerCount; ++seat) {
                    CleanMultiplayerPlayerName(g_lobby.roster[seat], packet.roster[seat], 16, seat);
                }
                if (packet.kind == kMultiplayerLobbyStart && AllLobbySeats(g_lobby.connectedMask)) {
                    SendLobbyPacket(kMultiplayerLobbyStartAck, 4);
                    LauncherLog("LOBBY_READY role=guest seat=%d players=%d serial=%u", g_lobby.localSeat,
                                g_lobby.playerCount, packet.serial);
                    return kLobbyReady;
                }
                continue;
            }
            if (!g_lobby.connected || packet.sessionId != g_lobby.sessionId) {
                continue;
            }
            g_lobby.lastReceive = now;
        }
        if (packet.kind == kMultiplayerLobbyCancel) {
            g_lobby.peerCancelled = true;
            return kLobbyFailed;
        }
    }
    const unsigned now = GetTickCount();
    if (now - g_lobby.startedAt >= 120000u) {
        return kLobbyFailed;
    }
    if (g_lobby.host) {
        for (int seat = 1; seat < g_lobby.playerCount; ++seat) {
            if ((g_lobby.connectedMask & (1u << seat)) && !(g_lobby.ackMask & (1u << seat)) &&
                now - g_lobby.receivedAt[seat] >= 5000u) {
                g_lobby.peerCancelled = true;
                return kLobbyFailed;
            }
        }
        if (g_lobby.connected && now - g_lobby.lastSend >= (g_lobby.startRequested ? 100u : 250u)) {
            SendLobbyPacket(g_lobby.startRequested ? kMultiplayerLobbyStart : kMultiplayerLobbyWelcome);
        }
    } else {
        if (g_lobby.connected && now - g_lobby.lastReceive >= 5000u) {
            g_lobby.peerCancelled = true;
            return kLobbyFailed;
        }
        if (now - g_lobby.lastSend >= 250u) {
            SendLobbyPacket(kMultiplayerLobbyHello);
        }
    }
    return g_lobby.connected && AllLobbySeats(g_lobby.connectedMask) ? kLobbyConnected : kLobbySearching;
}

int SelectedPlayerCount()
{
    const int selected = static_cast<int>(SendMessageW(g_ui.playersCombo, CB_GETCURSEL, 0, 0));
    return selected >= 0 && selected <= 2 ? selected + 2 : 2;
}

void ReadSelection(Mode mode)
{
    Selection& selection = *g_ui.result;
    selection.mode = mode;
    selection.playerCount = mode == kSingle ? 1 : SelectedPlayerCount();
    selection.localSeat = 0;
    for (int seat = 0; seat < 4; ++seat) {
        CleanMultiplayerPlayerName(selection.playerName[seat], "", 0, seat);
    }
    if ((mode == kHost || mode == kGuest) && g_lobby.connected) {
        selection.playerCount = g_lobby.playerCount;
        selection.localSeat = g_lobby.localSeat;
        memcpy(selection.playerName, g_lobby.roster, sizeof(selection.playerName));
    } else {
        ReadPlayerName(selection.playerName[0], 0);
    }
    selection.bot = IsDlgButtonChecked(g_ui.window, kBot) == BST_CHECKED;
    const int resolution = IsDlgButtonChecked(g_ui.window, kDisplay1280) == BST_CHECKED  ? 2
                           : IsDlgButtonChecked(g_ui.window, kDisplay960) == BST_CHECKED ? 1
                                                                                         : 0;
    selection.displayMode = resolution + (IsDlgButtonChecked(g_ui.window, kDisplayFullscreen) == BST_CHECKED ? 0 : 3);
    selection.displaySelected = true;
    selection.bgm = IsDlgButtonChecked(g_ui.window, kBgm) == BST_CHECKED;
    selection.se = IsDlgButtonChecked(g_ui.window, kSe) == BST_CHECKED;
    selection.audioSelected = true;
}

void SaveSettings()
{
    if (!g_ui.window) {
        return;
    }
    char text[128];
    if (g_settingsPath[0] != 0) {
        wchar_t name[64] = {};
        GetWindowTextW(g_ui.name, name, static_cast<int>(sizeof(name) / sizeof(name[0])));
        WritePrivateProfileStringW(kSettingsSection, L"name", name, g_settingsPath);
    }
    GetWindowTextA(g_ui.host, text, sizeof(text));
    SaveText("host", text);
    SaveNumber("port", static_cast<int>(ReadNumber(g_ui.port, 22020, 1, 65535)));
    SaveNumber("players", SelectedPlayerCount());
    SaveNumber("delay", g_ui.lockstepDelay);
    SaveNumber("rollback", g_ui.ownRollback ? 1 : 0);
    SaveNumber("role", IsDlgButtonChecked(g_ui.window, kRoleGuest) == BST_CHECKED ? 1 : 0);
    SaveNumber("resolution", IsDlgButtonChecked(g_ui.window, kDisplay1280) == BST_CHECKED  ? 2
                             : IsDlgButtonChecked(g_ui.window, kDisplay960) == BST_CHECKED ? 1
                                                                                           : 0);
    SaveNumber("fullscreen", IsDlgButtonChecked(g_ui.window, kDisplayFullscreen) == BST_CHECKED ? 1 : 0);
    SaveNumber("bgm", IsDlgButtonChecked(g_ui.window, kBgm) == BST_CHECKED ? 1 : 0);
    SaveNumber("se", IsDlgButtonChecked(g_ui.window, kSe) == BST_CHECKED ? 1 : 0);
    SaveNumber("bot", IsDlgButtonChecked(g_ui.window, kBot) == BST_CHECKED ? 1 : 0);
    SaveNumber("stage_names", IsDlgButtonChecked(g_ui.window, kStageNames) == BST_CHECKED ? 1 : 0);
    SaveNumber("language", static_cast<int>(g_ui.language));
}

bool IsLiveSettingControl(int id)
{
    switch (id) {
    case kRoleHost:
    case kRoleGuest:
    case kHostEdit:
    case kPortEdit:
    case kDelayEdit:
    case kRollback:
    case kPlayerName:
    case kDisplay640:
    case kDisplay960:
    case kDisplay1280:
    case kDisplayFullscreen:
    case kBgm:
    case kSe:
    case kBot:
    case kStageNames:
    case kLanguageCombo:
    case kPlayersCombo:
        return true;
    default:
        return false;
    }
}

void SaveLiveSettings(int id)
{
    if (g_ui.settingsReady && IsLiveSettingControl(id)) {
        SaveSettings();
    }
}

bool CreateSeatLog(Mode mode, wchar_t* path)
{
    (void)mode;
    if (g_settingsPath[0] == 0) {
        return false;
    }
    memcpy(path, g_settingsPath, sizeof(g_settingsPath));
    wchar_t* const slash = wcsrchr(path, L'\\');
    if (slash == nullptr) {
        return false;
    }
    slash[1] = 0;
    if (!AppendPath(path, L"th07_coop_logs")) {
        return false;
    }
    CreateDirectoryW(path, nullptr);
    const size_t folder = wcslen(path);
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    for (int copy = 1; copy < 100; ++copy) {
        wchar_t name[80];
        const int written =
            copy == 1 ? swprintf_s(name, L"\\th07_coop_p%d_%04u%02u%02u_%02u%02u%02u.log", g_lobby.localSeat + 1,
                                   now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond)
                      : swprintf_s(name, L"\\th07_coop_p%d_%04u%02u%02u_%02u%02u%02u_%d.log", g_lobby.localSeat + 1,
                                   now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, copy);
        path[folder] = 0;
        if (written <= 0 || !AppendPath(path, name)) {
            return false;
        }
        const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            return true;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) {
            return false;
        }
    }
    return false;
}

void ConfigureEnvironment(Mode mode, unsigned sessionId = 0, bool rollback = false, unsigned inputDelay = 4)
{
    char number[32];
    // Local play must not inherit TH07_MP_ROLLBACK.
    _snprintf_s(number, sizeof(number), _TRUNCATE, "%d", g_ui.result->playerCount);
    PutEnvironment("TH07_MP_PLAYERS", number);
    PutEnvironment("TH07_MP_MODE", mode == kHost || mode == kGuest ? "udp" : "local");
    PutEnvironment("TH07_MP_TEST_BOT", g_ui.result->bot ? "1" : "0");
    PutEnvironment("TH07_MP_STAGE_NAMES", IsDlgButtonChecked(g_ui.window, kStageNames) == BST_CHECKED ? "1" : "0");
    if (mode == kSingle || mode == kLocal) {
        PutEnvironment("TH07_MP_SEAT", "0");
        PutEnvironment("TH07_MP_ROLLBACK", nullptr);
        return;
    }
    _snprintf_s(number, sizeof(number), _TRUNCATE, "%d", g_lobby.localSeat);
    PutEnvironment("TH07_MP_SEAT", number);
    char logPath[MAX_PATH];
    wchar_t seatLog[MAX_PATH];
    if (!HasEnvironment("TH07_MP_LOG", logPath, sizeof(logPath)) && CreateSeatLog(mode, seatLog)) {
        _wputenv_s(L"TH07_MP_LOG", seatLog);
        SetEnvironmentVariableW(L"TH07_MP_LOG", seatLog);
    }
    _snprintf_s(number, sizeof(number), _TRUNCATE, "0x%08X", sessionId);
    PutEnvironment("TH07_MP_SESSION", number);
    PutEnvironment("TH07_MP_ROLLBACK", rollback ? "1" : "0");
    PutEnvironment("TH07_MP_ROLLBACK_WINDOW", rollback ? "8" : nullptr);
    _snprintf_s(number, sizeof(number), _TRUNCATE, "%u", rollback ? 0u : inputDelay);
    PutEnvironment("TH07_MP_TEST_DELAY", number);
    PutEnvironment("TH07_MP_MENU_INPUT_DELAY", "4");
}

void Finish(Mode mode)
{
    if (mode != kCancelled) {
        SaveSettings();
        ReadSelection(mode);
        if (mode == kSingle || mode == kLocal) {
            ConfigureEnvironment(mode);
        }
        g_launchSelection = *g_ui.result;
        g_hasLaunchSelection = true;
    } else {
        g_ui.result->mode = kCancelled;
    }
    g_ui.done = true;
    if (g_ui.window != nullptr) {
        DestroyWindow(g_ui.window);
    }
}

void ShowDelayHint()
{
    SetText(GetDlgItem(g_ui.window, kDelayHint),
            Text(IsDlgButtonChecked(g_ui.window, kRollback) == BST_CHECKED ? kTextDelayHintRollback : kTextDelayHint));
}

void UpdateRollback()
{
    const bool multiple = SelectedPlayerCount() > 2;
    if (multiple) {
        CheckDlgButton(g_ui.window, kRollback, BST_CHECKED);
    }
    const bool guest = IsDlgButtonChecked(g_ui.window, kRoleGuest) == BST_CHECKED;
    const bool rollback = IsDlgButtonChecked(g_ui.window, kRollback) == BST_CHECKED;
    if (rollback) {
        SetText(g_ui.delay, L"0");
    }
    const BOOL delayEditable = !guest && !rollback ? TRUE : FALSE;
    EnableWindow(GetDlgItem(g_ui.window, kRollback), guest || multiple ? FALSE : TRUE);
    EnableWindow(g_ui.delay, delayEditable);
    EnableWindow(GetDlgItem(g_ui.window, kDelayLabel), delayEditable);
    EnableWindow(GetDlgItem(g_ui.window, kDelayHint), delayEditable);
    ShowDelayHint();
}

void ShowOwnDelayChoice()
{
    CheckDlgButton(g_ui.window, kRollback, g_ui.ownRollback ? BST_CHECKED : BST_UNCHECKED);
    wchar_t delay[16];
    swprintf_s(delay, L"%d", g_ui.lockstepDelay);
    SetText(g_ui.delay, delay);
}

bool DelayBoxIsOwnChoice()
{
    return IsDlgButtonChecked(g_ui.window, kRoleGuest) != BST_CHECKED &&
           IsDlgButtonChecked(g_ui.window, kRollback) != BST_CHECKED;
}

void UpdateRole()
{
    const bool guest = IsDlgButtonChecked(g_ui.window, kRoleGuest) == BST_CHECKED;
    EnableWindow(g_ui.playersCombo, guest ? FALSE : TRUE);
    EnableWindow(g_ui.host, guest ? TRUE : FALSE);
    EnableWindow(GetDlgItem(g_ui.window, kHostLabel), guest ? TRUE : FALSE);
    SetText(g_ui.startNetwork, Text(guest ? kTextConnectToHost : kTextStartHosting));
    if (!g_ui.networkAttempting) {
        SetText(g_ui.status, Text(guest ? kTextStatusEnterHost : kTextStatusWaitHost));
    }
    UpdateRollback();
}

void SetNetworkControls(bool enabled)
{
    const BOOL value = enabled ? TRUE : FALSE;
    const int controls[] = {
        kRoleHost, kRoleGuest, kHostEdit, kPortEdit, kDelayEdit, kRollback,
        kBot, kPlayerName, kStartLocal, kStartSingle,
        kStartNetwork, kLanguageCombo, kPlayersCombo,
    };
    for (int id : controls) {
        EnableWindow(GetDlgItem(g_ui.window, id), value);
    }
    if (enabled) {
        UpdateRole();
    }
}

void ShowRoster()
{
    char roster[128] = {};
    for (int seat = 0; seat < g_lobby.playerCount; ++seat) {
        char label[16];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "%sP%d: ", seat == 0 ? "" : seat == 2 ? "\r\n" : "    ", seat + 1);
        Append(roster, sizeof(roster), label);
        Append(roster, sizeof(roster), g_lobby.connectedMask & (1u << seat) ? g_lobby.roster[seat] : "...");
    }
    wchar_t wide[128];
    if (MultiByteToWideChar(932, 0, roster, -1, wide, static_cast<int>(sizeof(wide) / sizeof(wide[0]))) > 0) {
        SetWindowTextW(g_ui.roster, wide);
    }
    ShowWindow(g_ui.roster, SW_SHOW);
}

void ResetAttempt(TextId status)
{
    g_ui.networkAttempting = false;
    g_ui.networkConnected = false;
    EnableWindow(g_ui.startGame, FALSE);
    SetText(g_ui.cancel, Text(kTextCancel));
    ShowWindow(g_ui.roster, SW_HIDE);
    ShowOwnDelayChoice();
    SetNetworkControls(true);
    SetText(g_ui.status, Text(status));
}

void StartAttempt()
{
    const Mode mode = IsDlgButtonChecked(g_ui.window, kRoleGuest) == BST_CHECKED ? kGuest : kHost;
    SaveSettings();
    ReadSelection(mode);
    char address[128] = {};
    char localName[kMultiplayerPlayerNameBytes] = {};
    GetWindowTextA(g_ui.host, address, sizeof(address));
    ReadPlayerName(localName, mode == kGuest ? 1 : 0);
    const unsigned port = ReadNumber(g_ui.port, 22020, 1, 65535);
    const bool rollback = IsDlgButtonChecked(g_ui.window, kRollback) == BST_CHECKED;
    const unsigned delay = rollback ? 0u : ReadNumber(g_ui.delay, 4, 0, 12);
    g_ui.networkAttempting = true;
    g_ui.networkConnected = false;
    SetNetworkControls(false);
    SetText(g_ui.cancel, Text(kTextStopSearch));
    SetText(g_ui.startNetwork, Text(mode == kGuest ? kTextButtonConnecting : kTextButtonHosting));
    SetText(g_ui.status, Text(mode == kGuest ? kTextStatusConnecting : kTextStatusHosting));
    TextId error = kTextErrorSocket;
    if (!OpenLobby(mode == kGuest, address[0] ? address : "127.0.0.1", port, localName, rollback, delay,
                   SelectedPlayerCount(), &error)) {
        ResetAttempt(error);
    }
}

bool CommitNetworkHandoff()
{
    const Mode mode = g_lobby.host ? kHost : kGuest;
    ReadSelection(mode);
    ConfigureEnvironment(mode, g_lobby.sessionId, g_lobby.rollback, g_lobby.inputDelay);
    sockaddr_in local = {};
    int localBytes = sizeof(local);
    getsockname(g_lobby.socket, reinterpret_cast<sockaddr*>(&local), &localBytes);
    char address[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &g_lobby.peer.sin_addr, address, sizeof(address));
    char endpoint[80];
    _snprintf_s(endpoint, sizeof(endpoint), _TRUNCATE, "%s:%u", address,
                static_cast<unsigned>(ntohs(g_lobby.peer.sin_port)));
    PutEnvironment("TH07_MP_PEER", endpoint);
    _snprintf_s(endpoint, sizeof(endpoint), _TRUNCATE, "0.0.0.0:%u", static_cast<unsigned>(ntohs(local.sin_port)));
    PutEnvironment("TH07_MP_BIND", endpoint);
    mp::Endpoint guests[4] = {};
    for (int seat = 1; seat < g_lobby.playerCount; ++seat) {
        guests[seat].ipv4 = g_lobby.guests[seat].sin_addr.s_addr;
        guests[seat].port = g_lobby.guests[seat].sin_port;
    }
    if (!mp::AdoptPreparedUdpSocket(static_cast<uintptr_t>(g_lobby.socket), g_lobby.peer.sin_addr.s_addr,
                                    g_lobby.peer.sin_port, g_lobby.localSeat, g_lobby.sessionId, g_lobby.inputDelay,
                                    g_lobby.playerCount, guests)) {
        return false;
    }
    LauncherLog("LOBBY_HANDOFF role=%s seat=%d session=%08X rollback=%u delay=%u peer=%s", g_lobby.host ? "host" : "guest",
                g_lobby.localSeat, g_lobby.sessionId, g_lobby.rollback ? 1u : 0u, g_lobby.inputDelay, address);
    // The game now owns the socket and its WSAStartup reference: do not close it here.
    g_lobby.socket = INVALID_SOCKET;
    g_lobby.winsock = false;
    g_lobby.active = false;
    g_ui.networkAttempting = false;
    Finish(mode);
    return true;
}

void PollNetwork()
{
    if (!g_ui.networkAttempting) {
        return;
    }
    const LobbyResult result = PollLobby();
    if (g_lobby.connected) {
        ShowRoster();
        if (!g_lobby.host) {
            SendMessageW(g_ui.playersCombo, CB_SETCURSEL, g_lobby.playerCount - 2, 0);
        }
    }
    if (result == kLobbyFailed) {
        const TextId reason = g_lobby.protocolMismatch ? kTextErrorProtocol
                              : g_lobby.peerCancelled  ? kTextErrorPeerStopped
                                                       : kTextErrorTimeout;
        CancelLobby();
        ResetAttempt(reason);
        return;
    }
    if (result == kLobbyReady) {
        SetText(g_ui.status, Text(kTextStatusStartingGame));
        if (!CommitNetworkHandoff()) {
            CancelLobby();
            ResetAttempt(kTextErrorHandoff);
        }
        return;
    }
    if (result == kLobbyConnected) {
        g_ui.networkConnected = true;
        if (!g_lobby.host) {
            CheckDlgButton(g_ui.window, kRollback, g_lobby.rollback ? BST_CHECKED : BST_UNCHECKED);
            ShowDelayHint();
            SetNumberText(g_ui.delay, g_lobby.inputDelay);
        }
        EnableWindow(g_ui.startGame, g_lobby.host && !g_lobby.startRequested ? TRUE : FALSE);
        SetText(g_ui.status, Text(g_lobby.host ? kTextStatusConnectedHost : kTextStatusConnectedGuest));
    } else {
        SetText(g_ui.status, Text(g_lobby.host ? kTextStatusHosting : kTextStatusConnecting));
    }
}

void FillLanguageCombo()
{
    SendMessageW(g_ui.languageCombo, CB_RESETCONTENT, 0, 0);
    for (int language = 0; language < kLanguageCount; ++language) {
        SendMessageW(g_ui.languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kText[kTextLanguageName][language]));
    }
    SendMessageW(g_ui.languageCombo, CB_SETCURSEL, g_ui.language, 0);
}

void ApplyLanguage()
{
    struct Label {
        int control;
        TextId text;
    };
    static const Label labels[] = {
        {kLanguageLabel, kTextLanguageLabel},
        {kConnectAsLabel, kTextConnectAs},
        {kRoleHost, kTextRoleHost},
        {kRoleGuest, kTextRoleGuest},
        {kDisplayGroup, kTextDisplayMode},
        {kDisplay640, kTextDisplay640},
        {kDisplay960, kTextDisplay960},
        {kDisplay1280, kTextDisplay1280},
        {kDisplayFullscreen, kTextDisplayFullscreen},
        {kAudioGroup, kTextAudio},
        {kBgm, kTextBgm},
        {kSe, kTextSe},
        {kControlGroup, kTextControl},
        {kBot, kTextBot},
        {kStageNames, kTextStageNames},
        {kRollback, kTextRollback},
        {kPlayerNameLabel, kTextPlayerName},
        {kHostLabel, kTextHostIp},
        {kPortLabel, kTextUdpPort},
        {kDelayLabel, kTextInputDelay},
        {kPlayersLabel, kTextPlayers},
        {kStartLocal, kTextStartLocal},
        {kStartSingle, kTextSinglePlayer},
        {kStartGame, kTextStartGame},
    };
    HFONT oldFont = g_ui.font;
    g_ui.font = MakeFont(g_ui.language);
    for (int id = kRoleHost; id <= kStageNames; ++id) {
        HWND control = GetDlgItem(g_ui.window, id);
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font), TRUE);
        }
    }
    for (const Label& label : labels) {
        SetText(GetDlgItem(g_ui.window, label.control), Text(label.text));
    }
    ShowDelayHint();
    SetText(g_ui.window, Text(kTextWindowTitle));
    FillLanguageCombo();
    SetText(g_ui.cancel, Text(g_ui.networkAttempting ? kTextStopSearch : kTextCancel));
    if (g_ui.networkAttempting) {
        SetText(g_ui.startNetwork, Text(g_lobby.host ? kTextButtonHosting : kTextButtonConnecting));
        if (g_lobby.connected) {
            SetText(g_ui.status, Text(g_lobby.host ? kTextStatusConnectedHost : kTextStatusConnectedGuest));
        }
    } else {
        UpdateRole();
    }
    InvalidateRect(g_ui.window, nullptr, TRUE);
    if (oldFont != nullptr) {
        DeleteObject(oldFont);
    }
}

void LoadSettings()
{
    SendMessageW(g_ui.playersCombo, CB_SETCURSEL, SettingNumber("players", 2, 2, 4) - 2, 0);
    const int role = SettingNumber("role", 0, 0, 1);
    CheckRadioButton(g_ui.window, kRoleHost, kRoleGuest, role ? kRoleGuest : kRoleHost);
    const int resolution = SettingNumber("resolution", 1, 0, 2);
    CheckRadioButton(g_ui.window, kDisplay640, kDisplay1280,
                     resolution == 2 ? kDisplay1280 : resolution == 1 ? kDisplay960 : kDisplay640);
    CheckDlgButton(g_ui.window, kDisplayFullscreen, SettingNumber("fullscreen", 0, 0, 1) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_ui.window, kBgm, SettingNumber("bgm", 1, 0, 1) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_ui.window, kSe, SettingNumber("se", 1, 0, 1) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_ui.window, kBot, SettingNumber("bot", 0, 0, 1) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_ui.window, kStageNames, SettingNumber("stage_names", 0, 0, 1) ? BST_CHECKED : BST_UNCHECKED);
    g_ui.ownRollback = SettingNumber("rollback", 0, 0, 1) != 0;
    CheckDlgButton(g_ui.window, kRollback, g_ui.ownRollback ? BST_CHECKED : BST_UNCHECKED);
    const int language = SettingNumber("language", static_cast<int>(g_ui.language), 0, kLanguageCount - 1);
    if (language != static_cast<int>(g_ui.language)) {
        g_ui.language = static_cast<Language>(language);
        ApplyLanguage();
    } else {
        FillLanguageCombo();
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    switch (message) {
    case WM_CREATE: {
        g_ui.window = window;
        char savedHost[128];
        wchar_t wideHost[128] = {};
        wchar_t savedPort[24];
        wchar_t savedDelay[24];
        SettingText("host", "127.0.0.1", savedHost, sizeof(savedHost));
        MultiByteToWideChar(CP_ACP, 0, savedHost, -1, wideHost, 128);
        swprintf_s(savedPort, L"%d", SettingNumber("port", 22020, 1, 65535));
        g_ui.lockstepDelay = SettingNumber("delay", 4, 0, 12);
        swprintf_s(savedDelay, L"%d", g_ui.lockstepDelay);

        AddControl(L"STATIC", Text(kTextLanguageLabel), SS_LEFT, 20, 14, 80, 22, kLanguageLabel);
        g_ui.languageCombo =
            AddControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 104, 10, 170, 200, kLanguageCombo);
        AddControl(L"STATIC", Text(kTextConnectAs), SS_LEFT, 20, 48, 100, 22, kConnectAsLabel);
        AddControl(L"BUTTON", Text(kTextRoleHost), BS_AUTORADIOBUTTON | WS_GROUP, 122, 46, 90, 22, kRoleHost);
        AddControl(L"BUTTON", Text(kTextRoleGuest), BS_AUTORADIOBUTTON, 218, 46, 90, 22, kRoleGuest);

        AddControl(L"BUTTON", Text(kTextDisplayMode), BS_GROUPBOX, 10, 78, 260, 148, kDisplayGroup);
        AddControl(L"BUTTON", Text(kTextDisplay640), BS_AUTORADIOBUTTON | WS_GROUP, 26, 102, 230, 22, kDisplay640);
        AddControl(L"BUTTON", Text(kTextDisplay960), BS_AUTORADIOBUTTON, 26, 126, 230, 22, kDisplay960);
        AddControl(L"BUTTON", Text(kTextDisplay1280), BS_AUTORADIOBUTTON, 26, 150, 230, 22, kDisplay1280);
        AddControl(L"BUTTON", Text(kTextDisplayFullscreen), BS_AUTOCHECKBOX | WS_TABSTOP, 26, 182, 230, 22,
                   kDisplayFullscreen);

        AddControl(L"BUTTON", Text(kTextAudio), BS_GROUPBOX, 280, 78, 180, 78, kAudioGroup);
        AddControl(L"BUTTON", Text(kTextBgm), BS_AUTOCHECKBOX | WS_TABSTOP, 298, 102, 130, 22, kBgm);
        AddControl(L"BUTTON", Text(kTextSe), BS_AUTOCHECKBOX | WS_TABSTOP, 298, 128, 150, 22, kSe);
        AddControl(L"BUTTON", Text(kTextControl), BS_GROUPBOX, 280, 160, 180, 66, kControlGroup);
        AddControl(L"BUTTON", Text(kTextBot), BS_AUTOCHECKBOX | WS_TABSTOP, 298, 178, 150, 22, kBot);
        AddControl(L"BUTTON", Text(kTextStageNames), BS_AUTOCHECKBOX | WS_TABSTOP, 298, 200, 154, 22, kStageNames);
        AddControl(L"BUTTON", Text(kTextRollback), BS_AUTOCHECKBOX | WS_TABSTOP, 20, 238, 310, 22, kRollback);
        AddControl(L"STATIC", Text(kTextPlayerName), SS_LEFT, 20, 272, 84, 22, kPlayerNameLabel);
        g_ui.name = AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 110, 270, 150, 24, kPlayerName);
        SendMessageW(g_ui.name, EM_LIMITTEXT, kMultiplayerPlayerNameBytes - 1, 0);
        {
            wchar_t name[64] = L"PLAYER1";
            if (g_settingsPath[0] != 0) {
                GetPrivateProfileStringW(kSettingsSection, L"name", L"PLAYER1", name,
                                         static_cast<DWORD>(sizeof(name) / sizeof(name[0])), g_settingsPath);
            }
            SetWindowTextW(g_ui.name, name);
        }
        AddControl(L"STATIC", Text(kTextPlayers), SS_LEFT, 298, 272, 60, 22, kPlayersLabel);
        g_ui.playersCombo =
            AddControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 360, 268, 80, 120, kPlayersCombo);
        static const wchar_t* const counts[] = {L"2", L"3", L"4"};
        for (const wchar_t* count : counts) {
            SendMessageW(g_ui.playersCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(count));
        }
        AddControl(L"STATIC", Text(kTextHostIp), SS_LEFT, 20, 306, 90, 22, kHostLabel);
        g_ui.host = AddControl(L"EDIT", wideHost, WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 110, 304, 270, 24, kHostEdit);
        AddControl(L"STATIC", Text(kTextUdpPort), SS_LEFT, 20, 340, 80, 22, kPortLabel);
        g_ui.port = AddControl(L"EDIT", savedPort, WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, 110, 338, 100,
                               24, kPortEdit);
        AddControl(L"STATIC", Text(kTextInputDelay), SS_LEFT, 222, 340, 66, 22, kDelayLabel);
        g_ui.delay = AddControl(L"EDIT", savedDelay, WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, 290, 338, 40,
                                24, kDelayEdit);
        AddControl(L"STATIC", Text(kTextDelayHint), SS_LEFT, 336, 340, 126, 22, kDelayHint);

        g_ui.roster = AddControl(L"STATIC", L"", SS_LEFT | WS_BORDER, 20, 374, 420, 34, kRoster);
        ShowWindow(g_ui.roster, SW_HIDE);
        g_ui.startNetwork = AddControl(L"BUTTON", Text(kTextStartHosting), BS_DEFPUSHBUTTON | WS_TABSTOP, 20, 416, 420,
                                       34, kStartNetwork);
        AddControl(L"BUTTON", Text(kTextStartLocal), BS_PUSHBUTTON | WS_TABSTOP, 20, 458, 200, 32, kStartLocal);
        AddControl(L"BUTTON", Text(kTextSinglePlayer), BS_PUSHBUTTON | WS_TABSTOP, 240, 458, 200, 32, kStartSingle);
        g_ui.startGame =
            AddControl(L"BUTTON", Text(kTextStartGame), BS_DEFPUSHBUTTON | WS_TABSTOP, 20, 500, 200, 32, kStartGame);
        EnableWindow(g_ui.startGame, FALSE);
        g_ui.status = AddControl(L"STATIC", Text(kTextStatusReady), SS_LEFT | WS_BORDER, 20, 540, 420, 48, kStatus);
        g_ui.cancel =
            AddControl(L"BUTTON", Text(kTextCancel), BS_PUSHBUTTON | WS_TABSTOP, 340, 598, 100, 30, kCancel);

        LoadSettings();
        UpdateRole();
        g_ui.settingsReady = true;
        SetTimer(window, kTimerId, kTimerMs, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == kPlayersCombo && HIWORD(wp) == CBN_SELCHANGE) {
            ShowOwnDelayChoice();
            UpdateRollback();
            SaveLiveSettings(kPlayersCombo);
            return 0;
        }
        if (LOWORD(wp) == kLanguageCombo && HIWORD(wp) == CBN_SELCHANGE) {
            const LRESULT selected = SendMessageW(g_ui.languageCombo, CB_GETCURSEL, 0, 0);
            if (selected >= 0 && selected < kLanguageCount) {
                g_ui.language = static_cast<Language>(selected);
                ApplyLanguage();
            }
            SaveLiveSettings(kLanguageCombo);
            return 0;
        }
        if (HIWORD(wp) == EN_CHANGE && IsLiveSettingControl(LOWORD(wp))) {
            if (LOWORD(wp) == kDelayEdit && g_ui.settingsReady && DelayBoxIsOwnChoice()) {
                g_ui.lockstepDelay = static_cast<int>(ReadNumber(g_ui.delay, 4, 0, 12));
            }
            SaveLiveSettings(LOWORD(wp));
            return 0;
        }
        if (HIWORD(wp) == BN_CLICKED) {
            switch (LOWORD(wp)) {
            case kRoleHost:
            case kRoleGuest:
                ShowOwnDelayChoice();
                UpdateRole();
                SaveLiveSettings(LOWORD(wp));
                return 0;
            case kRollback:
                g_ui.ownRollback = IsDlgButtonChecked(window, kRollback) == BST_CHECKED;
                if (!g_ui.ownRollback) {
                    ShowOwnDelayChoice();
                }
                UpdateRollback();
                SaveLiveSettings(kRollback);
                return 0;
            case kDisplay640:
            case kDisplay960:
            case kDisplay1280:
            case kDisplayFullscreen:
            case kBgm:
            case kSe:
            case kBot:
            case kStageNames:
                SaveLiveSettings(LOWORD(wp));
                return 0;
            case kStartNetwork:
                if (!g_ui.networkAttempting) {
                    StartAttempt();
                }
                return 0;
            case kStartGame:
                if (g_ui.networkAttempting && g_ui.networkConnected && g_lobby.host && !g_lobby.startRequested &&
                    AllLobbySeats(g_lobby.connectedMask)) {
                    g_lobby.startRequested = true;
                    SendLobbyPacket(kMultiplayerLobbyStart);
                    EnableWindow(g_ui.startGame, FALSE);
                    SetText(g_ui.status, Text(kTextStatusStartingAck));
                    LauncherLog("LOBBY_START_REQUEST session=%08X", g_lobby.sessionId);
                }
                return 0;
            case kStartLocal:
                Finish(kLocal);
                return 0;
            case kStartSingle:
                Finish(kSingle);
                return 0;
            case kCancel:
                if (g_ui.networkAttempting) {
                    CancelLobby();
                    ResetAttempt(kTextStatusCancelled);
                } else {
                    Finish(kCancelled);
                }
                return 0;
            }
        }
        break;
    case WM_TIMER:
        if (wp == kTimerId) {
            PollNetwork();
            return 0;
        }
        break;
    case WM_CLOSE:
        if (g_ui.networkAttempting) {
            CancelLobby();
        }
        Finish(kCancelled);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kTimerId);
        g_ui.window = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

}

bool Run(Selection* selection)
{
    if (selection == nullptr) {
        return false;
    }
    *selection = Selection();
    selection->mode = kSkipped;
    Copy(selection->playerName[0], sizeof(selection->playerName[0]), "PLAYER1");
    Copy(selection->playerName[1], sizeof(selection->playerName[1]), "PLAYER2");
    selection->displayMode = 4;
    selection->bgm = true;
    selection->se = true;
#ifdef TH07_HARNESS
    return true;
#else
    ResolveSettingsPath();

    char value[24] = {};
    char force[24] = {};
    const bool forceGui = HasEnvironment("TH07_MP_TEST_GUI", force, sizeof(force)) && force[0] != '0';
    if (!forceGui && HasEnvironment("TH07_MP_MODE", value, sizeof(value))) {
        return true;
    }
    if (HasEnvironment("TH07_MP_GUI", value, sizeof(value)) && value[0] == '0') {
        return true;
    }

    g_ui = Ui();
    g_ui.result = selection;
    g_ui.language = kEnglish;
    char language[8];
    if (HasEnvironment("TH07_MP_LANGUAGE", language, sizeof(language)) && language[0] >= '0' &&
        language[0] < '0' + kLanguageCount) {
        g_ui.language = static_cast<Language>(language[0] - '0');
    }
    g_ui.font = MakeFont(g_ui.language);

    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DeleteObject(g_ui.font);
        return false;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rectangle = {0, 0, 470, 668};
    AdjustWindowRectEx(&rectangle, style, FALSE, WS_EX_APPWINDOW);
    g_ui.window = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, Text(kTextWindowTitle), style, CW_USEDEFAULT,
                                  CW_USEDEFAULT, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
                                  nullptr, nullptr, windowClass.hInstance, nullptr);
    if (g_ui.window == nullptr) {
        UnregisterClassW(kWindowClass, windowClass.hInstance);
        DeleteObject(g_ui.font);
        return false;
    }
    ShowWindow(g_ui.window, SW_SHOW);
    UpdateWindow(g_ui.window);
    MSG message;
    while (!g_ui.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_ui.window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (!g_ui.done) {
        if (g_ui.networkAttempting) {
            CancelLobby();
        }
        selection->mode = kCancelled;
    }
    if (g_ui.window != nullptr) {
        DestroyWindow(g_ui.window);
    }
    UnregisterClassW(kWindowClass, windowClass.hInstance);
    if (g_ui.font != nullptr) {
        DeleteObject(g_ui.font);
    }
    return true;
#endif
}

const wchar_t* SettingsPath()
{
    return g_settingsPath;
}

void ApplyGameConfig()
{
    if (!g_hasLaunchSelection) {
        return;
    }
    GameConfiguration& config = g_Supervisor.cfg;
    if (g_launchSelection.displaySelected) {
        config.windowed = g_launchSelection.displayMode >= 3 ? 1 : 0;
    }
    if (g_launchSelection.audioSelected && !g_audioApplied) {
        g_savedBgm = config.musicMode;
        g_savedSe = config.playSounds;
        if (!g_launchSelection.bgm) {
            config.musicMode = 0; // MUSIC_OFF
        }
        if (!g_launchSelection.se) {
            config.playSounds = 0;
        }
        g_audioApplied = true;
    }
    LauncherLog("GAME_CONFIG display=%d windowed=%u bgm=%u se=%u", g_launchSelection.displayMode, config.windowed,
                g_launchSelection.bgm ? 1u : 0u, g_launchSelection.se ? 1u : 0u);
}

void RestoreGameConfig()
{
    if (!g_audioApplied) {
        return;
    }
    GameConfiguration& config = g_Supervisor.cfg;
    if (!g_launchSelection.bgm) {
        config.musicMode = g_savedBgm;
    }
    if (!g_launchSelection.se) {
        config.playSounds = g_savedSe;
    }
    g_audioApplied = false;
}

int WindowScale()
{
    if (!g_hasLaunchSelection || !g_launchSelection.displaySelected || g_launchSelection.displayMode < 3) {
        return 2;
    }
    return 2 + (g_launchSelection.displayMode - 3);
}

}
}
