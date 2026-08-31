#pragma once

#include <stdint.h>

namespace th07 {
namespace mp {

const int kMaxPlayers = 4;
const unsigned kNoFrame = 0xFFFFFFFFu;

enum Mode {
    kDisabled = 0,
    kLocal = 1,
    kUdp = 2,
};

struct Config {
    bool initialized;
    Mode mode;
    int localSeat;
    int playerCount;
    int requestedP1;
    int requestedP2;
    int resolvedP1;
    int resolvedP2;
    bool testBot;
    bool testTitleBot;
    unsigned testTitleBotHold;
    bool testTitleBotCancelP2;
    bool testTitleBotExtra;
    bool testPlayEnding;
    bool testMenuTrace;
    bool testInfiniteLives;
    int testDifficulty;
    int testCharacters[4];
    int testSubseasons[4];
    bool testBotRelease;
    int testStartStage;
    unsigned testPausePeriod;
    unsigned testPauseFrame;
    int testPauseRow;
    unsigned testDeviceLossFrame;
    unsigned testDeviceLossHold;
    int testPauseSeat;
    unsigned testCrashFrame;
    unsigned testEndGameFrame;
    unsigned testExitAfterGame;
    unsigned testStageClearFrame;
    int testStageClearLast;
    unsigned testPlayersPeriod;
    unsigned testGhostFrame;
    unsigned testGhostAll;
    bool testGhostHold;
    int testGive;
    unsigned testBotIdle;
    bool testOverIdle;
    bool testPredictAlways;
    bool testPredictTalk;
    bool talkConfirmed;
    bool testBotMash;
    bool testBotNoShot;
    bool testBotLegacy;
    int testMenuSeat;
    int testStone[kMaxPlayers][4];
    unsigned menuInputDelay;
    unsigned artificialDelay;
    unsigned anmTraceFrom;
    unsigned anmTraceTo;
    unsigned stateTraceFrom;
    unsigned stateTraceTo;
    unsigned sessionId;
    char bindText[64];
    char peerText[64];
    wchar_t logPath[260];
    bool rollback;
    unsigned rollbackWindow;
    bool testFreePoison;
    char playerName[kMaxPlayers][16];
};

const Config& Cfg();

bool Enabled();
bool UdpEnabled();
bool LocalEnabled();
bool AllowsMultipleWindows();
int PlayerCount();
int LocalSeat();

struct Endpoint {
    uint32_t ipv4; // network byte order
    uint16_t port; // network byte order
};

// Takes over the launcher's bound socket. False when refused.
bool AdoptPreparedUdpSocket(uintptr_t socketHandle, uint32_t peerIpv4, uint16_t peerPort, int localSeat,
                            uint32_t sessionId, unsigned inputDelay, int playerCount = 2,
                            const Endpoint* guests = nullptr);

struct PreparedUdpSocket {
    bool valid;
    uintptr_t handle; // a SOCKET, with its WSAStartup reference
    uint32_t peerIpv4;
    uint16_t peerPort;
    Endpoint guests[kMaxPlayers];
};
bool TakePreparedUdpSocket(PreparedUdpSocket* out);

void SetPlayerNames(const char* p1, const char* p2);
void SetPlayerName(int seat, const char* name);
const char* PlayerName(int seat);

void Log(const char* format, ...);
void LogRaw(const char* text);

void InstallCrashFilter();

}
}
