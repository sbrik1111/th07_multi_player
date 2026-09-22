#pragma once

#include <stdint.h>

namespace th07 {

const uint32_t kMultiplayerLobbyMagic = 0x4C373054u;  // "T07L"
const uint16_t kMultiplayerLobbyVersion = 7;

enum MultiplayerLobbyKind {
    kMultiplayerLobbyHello = 1,
    kMultiplayerLobbyWelcome = 2,
    kMultiplayerLobbyStart = 3,
    kMultiplayerLobbyStartAck = 4,
    kMultiplayerLobbyCancel = 5,
};

#pragma pack(push, 1)
struct MultiplayerLobbyPacket {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    uint32_t kind;
    uint32_t sessionId;
    uint32_t serial;
    uint32_t senderSeat;
    uint16_t inputDelay;
    // 1 rollback, 0 lockstep; the Host's value decides.
    uint16_t rollbackEnabled;
    char playerName[16];
    uint32_t playerCount;
    uint32_t assignedSeat;
    uint32_t connectedMask;
    char roster[4][16];
};
#pragma pack(pop)

static_assert(sizeof(MultiplayerLobbyPacket) == 120, "launcher lobby packet must stay fixed-size");

// A-Z, a-z and 0-9, at most 15 characters; CP932 pairs are skipped whole.
const unsigned kMultiplayerPlayerNameBytes = 16;

inline void CleanMultiplayerPlayerName(char* out, const char* in, unsigned inBytes, int seat)
{
    const unsigned room = kMultiplayerPlayerNameBytes - 1;
    unsigned used = 0;
    for (unsigned i = 0; in != nullptr && i < inBytes && in[i] != 0;) {
        const unsigned lead = static_cast<unsigned char>(in[i]);
        const unsigned trail = i + 1 < inBytes ? static_cast<unsigned char>(in[i + 1]) : 0u;
        const bool pair = ((lead >= 0x81 && lead <= 0x9F) || (lead >= 0xE0 && lead <= 0xFC)) &&
                          ((trail >= 0x40 && trail <= 0x7E) || (trail >= 0x80 && trail <= 0xFC));
        if (pair) {
            i += 2;
            continue;
        }
        if (used + 1 > room) {
            break;
        }
        const bool alphanumeric =
            (lead >= 'A' && lead <= 'Z') || (lead >= 'a' && lead <= 'z') || (lead >= '0' && lead <= '9');
        if (alphanumeric) {
            out[used++] = static_cast<char>(lead);
        }
        i += 1;
    }
    if (used == 0) {
        const char* const names[] = {"PLAYER1", "PLAYER2", "PLAYER3", "PLAYER4"};
        const char* const fallback = names[seat >= 0 && seat < 4 ? seat : 0];
        used = 0;
        while (fallback[used] != 0) {
            out[used] = fallback[used];
            ++used;
        }
    }
    out[used] = 0;
}

}
