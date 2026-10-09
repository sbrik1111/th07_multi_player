#pragma once

#include <stdint.h>

namespace th07 { namespace replay { struct Settings; } }

namespace th07 {
namespace mp {

const int kMaxPlayers = 4;
const unsigned kNoFrame = 0xFFFFFFFFu;

// Single play is a local session of one player.
enum Mode {
    kLocal = 1,
    kUdp = 2,
};

struct Config {
    bool initialized;
    Mode mode;
    int localSeat;
    int playerCount;
    bool testBot;
    bool testRules;
    bool testKeepAlive;
    bool showStageNames;
    bool testTitleBot;
    bool testCampaign;
    int testMenuSeat;
    unsigned testBotIdle;
    bool testBotMash;
    bool testBotNoShot;
    int testCharacters[4];
    int testShotTypes[4];
    int testStartStage;
    unsigned testStageClearFrame;
    int testStageClearLast;
    unsigned testGhostFrame;
    int testGive;
    bool testPredictAlways;
    bool talkConfirmed;
    unsigned menuInputDelay;
    unsigned artificialDelay;
    unsigned sessionId;
    char bindText[64];
    char peerText[64];
    wchar_t logPath[260];
    bool rollback;
    unsigned rollbackWindow;
    char playerName[kMaxPlayers][16];
};

const Config& Cfg();
void RestoreConfig(const Config& config);
void ConfigureReplay(const replay::Settings& settings);

bool UdpEnabled();
bool LocalEnabled();
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
