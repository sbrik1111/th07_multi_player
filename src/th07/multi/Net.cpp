// Menus run in lockstep; gameplay in lockstep or rollback. The Host relays the Guests'
// datagrams; inputs are resent until acknowledged.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "multi/Net.h"
#include "multi/Lobby.h"
#include "multi/MpConfig.h"
#include "multi/RollbackNetcode.h"
#include "multi/SessionFrame.h"
#include "multi/Session.h"
#include "multi/RollbackGame.h"
#include "SimHash.hpp"
#include "multi/RuntimeData.h"

namespace th07 {
namespace net {
namespace {

using netcode::Timeline;
using netcode::Inputs;

const unsigned kMagic = 0x4E373054u; // "T07N"
const unsigned short kVersion = 4;
const unsigned kHistory = netcode::History;
const unsigned kHashRing = 256;
const unsigned kHashPeriod = 16;
const unsigned kPartPeriod = 64;
const unsigned kTailFrames = 180;

enum Phase {
    kMenu = 0,
    kGameplay = 1,
};

#pragma pack(push, 1)
struct Packet {
    unsigned magic;
    unsigned short version;
    unsigned short bytes;
    unsigned session;
    unsigned char seat;
    unsigned char players;
    unsigned char flags; // bit 0 rollback, bit 1 ready hash valid
    unsigned char phase;
    unsigned segment; // 0 the first menus, 1 the first game, 2 the menus after it, ...
    unsigned delay;
    unsigned window;
    unsigned first; // the frame of input[0]
    unsigned count;
    unsigned short input[kHistory];
    unsigned received[netcode::kMaxPlayers];
    unsigned next;
    unsigned seenNext[netcode::kMaxPlayers];
    unsigned readyHash;
    unsigned readyParts[SIM_HASH_PART_COUNT];
    unsigned hashFrame;
    unsigned hash;
    unsigned partFrame;
    unsigned parts[SIM_HASH_PART_COUNT];
    unsigned sendTime, echoTime[netcode::kMaxPlayers], echoHold[netcode::kMaxPlayers];
};
#pragma pack(pop)
static_assert(sizeof(Packet) <= 1472, "a session packet must fit one IPv4 datagram");

struct HashCell {
    unsigned frame;
    unsigned hash;
    bool hasParts;
    unsigned parts[SIM_HASH_PART_COUNT];
};

struct Segment {
    bool active;
    unsigned index;
    Phase phase;
    unsigned delay;
    bool rollback;
    Timeline timeline;
    unsigned tailFrames;
    bool readyTaken;
    unsigned readyHash;
    unsigned readyParts[SIM_HASH_PART_COUNT];
    bool peerReady[netcode::kMaxPlayers];
    bool readyChecked;
    HashCell own[kHashRing];
    HashCell peer[netcode::kMaxPlayers][kHashRing];
    unsigned lastHashFrame;
    unsigned lastPartFrame;
    bool mismatchLogged;
    bool partMismatchLogged;
    unsigned hashChecked;
    unsigned hashMismatches;
    unsigned syncTicks;
    unsigned syncWaits;
    int syncAdvantage;
    float syncAverage;
    bool testStalled;
    int peerAdvantage[netcode::kMaxPlayers];
    bool peerAdvantageKnown[netcode::kMaxPlayers];
    unsigned peerNext[netcode::kMaxPlayers], peerStamp[netcode::kMaxPlayers];
};

struct Net {
    bool initialized;
    bool failed;
    SOCKET socket;
    sockaddr_in host;
    sockaddr_in guests[netcode::kMaxPlayers];
    bool guestKnown[netcode::kMaxPlayers];
    int seat;
    int players;
    unsigned session;
    netcode::RoundTrip roundTrip[netcode::kMaxPlayers];
    Segment segments[2]; // current and previous, by index & 1
    unsigned current;
    unsigned hostFrames;
    unsigned framesRun;
    unsigned waitFrames;
    unsigned lastNetLog;
    unsigned startedMs, perfMs, perfFrame, perfTicks, perfIdle, perfSync;
    unsigned perfRollbacks, perfReplayed;
    unsigned idleSinceMs, longestIdleMs;
    unsigned gameplayDelay;
    unsigned delayKeys, testDelayStep;
    unsigned stallSince; // 0: frames run
};

Net g_net;

unsigned NowMicros()
{
    static LARGE_INTEGER frequency;
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (unsigned)(now.QuadPart / frequency.QuadPart * 1000000 +
                      now.QuadPart % frequency.QuadPart * 1000000 / frequency.QuadPart);
}

bool WorstRoundTrip(unsigned& micros)
{
    bool known = false;
    micros = 0;
    for (int seat = 0; seat < g_net.players; ++seat) {
        unsigned value = 0;
        if (seat != g_net.seat && g_net.roundTrip[seat].Display(value)) {
            known = true;
            if (value > micros) micros = value;
        }
    }
    return known;
}

void Fail(const char* what)
{
    if (!g_net.failed) {
        mp::Log("FAULT %s", what);
    }
    g_net.failed = true;
}

bool ParseEndpoint(const char* text, sockaddr_in* out)
{
    char host[64];
    const char* colon = strrchr(text, ':');
    if (colon == NULL || colon - text >= (int)sizeof(host)) {
        return false;
    }
    memcpy(host, text, colon - text);
    host[colon - text] = 0;
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons((unsigned short)atoi(colon + 1));
    return inet_pton(AF_INET, host, &out->sin_addr) == 1;
}

Segment& Current()
{
    return g_net.segments[g_net.current & 1];
}

Segment* Previous()
{
    if (g_net.current == 0) {
        return NULL;
    }
    Segment& s = g_net.segments[(g_net.current - 1) & 1];
    return s.active && s.index == g_net.current - 1 ? &s : NULL;
}

void StartSegment(unsigned index)
{
    const mp::Config& cfg = mp::Cfg();
    Segment& s = g_net.segments[index & 1];
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.index = index;
    s.phase = (index & 1) ? kGameplay : kMenu;
    s.rollback = s.phase == kGameplay && cfg.rollback;
    s.delay = s.phase == kMenu ? cfg.menuInputDelay : g_net.gameplayDelay;
    g_net.testDelayStep = 0;
    unsigned window = s.rollback ? cfg.rollbackWindow : 1;
    s.lastHashFrame = netcode::NoFrame;
    s.lastPartFrame = netcode::NoFrame;
    for (unsigned i = 0; i < kHashRing; i++) {
        s.own[i].frame = netcode::NoFrame;
        for (int p = 0; p < netcode::kMaxPlayers; p++) {
            s.peer[p][i].frame = netcode::NoFrame;
        }
    }
    if (!s.timeline.Initialize(g_net.seat, window, g_net.players)) {
        Fail(s.timeline.Error());
    }
    g_net.current = index;
    g_net.perfMs = GetTickCount();
    g_net.perfFrame = g_net.perfTicks = g_net.perfIdle = g_net.perfSync = 0;
    g_net.idleSinceMs = g_net.longestIdleMs = 0;
    g_net.perfRollbacks = rollback_game::GetStats().rollbacks;
    g_net.perfReplayed = rollback_game::GetStats().replayedFrames;
    if (s.rollback) {
        rollback_game::BeginSegment();
    }
    mp::Log("SEGMENT index=%u phase=%s delay=%u window=%u rollback=%d host_frame=%u", index,
            s.phase == kMenu ? "menu" : "gameplay", s.delay, window, s.rollback ? 1 : 0, g_net.hostFrames);
}

bool InitNetwork()
{
    if (g_net.initialized) {
        return !g_net.failed;
    }
    g_net.initialized = true;
    g_net.startedMs = GetTickCount();
    const mp::Config& cfg = mp::Cfg();
    g_net.seat = cfg.localSeat;
    g_net.gameplayDelay = cfg.artificialDelay;
    g_net.players = cfg.playerCount;
    g_net.session = cfg.sessionId;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Fail("WSAStartup");
        return false;
    }
    mp::PreparedUdpSocket prepared = {};
    if (mp::TakePreparedUdpSocket(&prepared)) {
        g_net.socket = (SOCKET)prepared.handle;
        memset(&g_net.host, 0, sizeof(g_net.host));
        g_net.host.sin_family = AF_INET;
        g_net.host.sin_addr.s_addr = prepared.peerIpv4;
        g_net.host.sin_port = prepared.peerPort;
        for (int seat = 1; seat < g_net.players; seat++) {
            if (prepared.guests[seat].port != 0) {
                memset(&g_net.guests[seat], 0, sizeof(sockaddr_in));
                g_net.guests[seat].sin_family = AF_INET;
                g_net.guests[seat].sin_addr.s_addr = prepared.guests[seat].ipv4;
                g_net.guests[seat].sin_port = prepared.guests[seat].port;
                g_net.guestKnown[seat] = true;
            }
        }
        mp::Log("UDP_ADOPT seat=%d players=%d session=%08X", g_net.seat, g_net.players, g_net.session);
    } else {
        sockaddr_in bind_to, peer;
        if (!ParseEndpoint(cfg.bindText, &bind_to) || !ParseEndpoint(cfg.peerText, &peer)) {
            Fail("bad TH07_MP_BIND / TH07_MP_PEER");
            return false;
        }
        g_net.socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_net.socket == INVALID_SOCKET || bind(g_net.socket, (sockaddr*)&bind_to, sizeof(bind_to)) != 0) {
            Fail("bind");
            return false;
        }
        u_long nonblocking = 1;
        ioctlsocket(g_net.socket, FIONBIO, &nonblocking);
        g_net.host = peer;
        for (int seat = 1; seat < g_net.players; seat++) {
            g_net.guests[seat] = peer;
            g_net.guests[seat].sin_port = htons((unsigned short)(ntohs(peer.sin_port) + seat - 1));
            g_net.guestKnown[seat] = true;
        }
        mp::Log("UDP seat=%d players=%d bind=%s peer=%s session=%08X", g_net.seat, g_net.players, cfg.bindText,
                cfg.peerText, g_net.session);
    }
    StartSegment(0);
    return !g_net.failed;
}

void SendTo(const void* bytes, int size, const sockaddr_in& to)
{
    sendto(g_net.socket, (const char*)bytes, size, 0, (const sockaddr*)&to, sizeof(to));
}

void SendOut(const void* bytes, int size, int except)
{
    if (g_net.seat != 0) {
        SendTo(bytes, size, g_net.host);
        return;
    }
    for (int seat = 1; seat < g_net.players; seat++) {
        if (seat != except && g_net.guestKnown[seat]) {
            SendTo(bytes, size, g_net.guests[seat]);
        }
    }
}

void SendSegment(Segment& s)
{
    Packet p;
    memset(&p, 0, sizeof(p));
    p.magic = kMagic;
    p.version = kVersion;
    p.bytes = sizeof(Packet);
    p.session = g_net.session;
    p.seat = (unsigned char)g_net.seat;
    p.players = (unsigned char)g_net.players;
    p.flags = (mp::Cfg().rollback ? 1 : 0) | (s.readyTaken ? 2 : 0);
    p.phase = (unsigned char)s.phase;
    p.segment = s.index;
    p.delay = s.delay;
    p.window = s.timeline.Limit();
    p.first = s.timeline.PeerAck();
    p.count = s.timeline.SendHistory(p.input);
    p.next = s.timeline.Next();
    for (int seat = 0; seat < g_net.players; seat++) {
        p.received[seat] = s.timeline.Received(seat);
        p.seenNext[seat] = s.peerNext[seat];
    }
    p.readyHash = s.readyHash;
    memcpy(p.readyParts, s.readyParts, sizeof(p.readyParts));
    p.hashFrame = s.lastHashFrame;
    if (s.lastHashFrame != netcode::NoFrame) {
        p.hash = s.own[s.lastHashFrame % kHashRing].hash;
    }
    p.partFrame = s.lastPartFrame;
    if (s.lastPartFrame != netcode::NoFrame) {
        memcpy(p.parts, s.own[s.lastPartFrame % kHashRing].parts, sizeof(p.parts));
    }
    unsigned now = NowMicros();
    for (int seat = 0; seat < g_net.players; ++seat) {
        g_net.roundTrip[seat].Publish(now);
        g_net.roundTrip[seat].Stamp(now, p.sendTime, p.echoTime[seat], p.echoHold[seat]);
    }
    SendOut(&p, sizeof(p), -1);
}

void LogDesync(Segment& s, unsigned frame, const HashCell& own, const HashCell& peer, int seat, bool parts)
{
    bool& logged = parts ? s.partMismatchLogged : s.mismatchLogged;
    if (logged) {
        return;
    }
    logged = true;
    char line[640];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "DESYNC%s segment=%u frame=%u seat=%d local=%08X remote=%08X",
                        parts ? "_PART" : "_HASH", s.index, frame, seat, own.hash, peer.hash);
    if (parts) {
        for (int i = 0; i < SIM_HASH_PART_COUNT && n > 0; i++) {
            if (own.parts[i] != peer.parts[i]) {
                n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %s=%08X/%08X", g_SimHashPartNames[i],
                                 own.parts[i], peer.parts[i]);
            }
        }
    }
    mp::LogRaw(line);
}

unsigned g_dumpFrame = netcode::NoFrame;
unsigned g_dumpPasses;

const int kTraceFrames = 64;
const int kTraceItems = 1024;
struct ItemTrace {
    unsigned frame;
    int count;
    SimItemRecord items[kTraceItems];
};
ItemTrace* g_itemTrace;

void TraceItems(unsigned frame)
{
    if (g_itemTrace == nullptr) {
        g_itemTrace = (ItemTrace*)calloc(kTraceFrames, sizeof(ItemTrace));
        if (g_itemTrace == nullptr) {
            return;
        }
    }
    ItemTrace& t = g_itemTrace[(frame / kHashPeriod) % kTraceFrames];
    t.frame = frame;
    t.count = SimCollectItems(t.items, kTraceItems);
}

void WriteItemTrace(unsigned last)
{
    if (g_itemTrace == nullptr) {
        return;
    }
    for (int k = kTraceFrames - 1; k >= 0; k--) {
        unsigned frame = last - (unsigned)k * kHashPeriod;
        const ItemTrace& t = g_itemTrace[(frame / kHashPeriod) % kTraceFrames];
        if (t.frame != frame) {
            continue;
        }
        mp::Log("TRACE_FRAME frame=%u items=%d", frame, t.count);
        for (int i = 0; i < t.count; i++) {
            const SimItemRecord& r = t.items[i];
            mp::Log("TRACE_ITEM frame=%u i=%d state=%d type=%d pos=%08X,%08X z=%08X vel=%08X,%08X vz=%08X mag=%08X tow=%08X "
                    "time=%d intang=%d collector=%d target=%d transfer=%d",
                    frame, r.index, r.state, r.type, r.pos_x, r.pos_y, r.pos_z, r.vel_x, r.vel_y, r.vel_z, r.magnitude,
                    r.towards, r.time,
                    r.intangible, r.collector, r.targetSeat, r.transfer);
        }
    }
}

void MaybeDump(unsigned frame)
{
    if (frame == g_dumpFrame) {
        SimDumpState(frame, ++g_dumpPasses);
    }
}

void CheckHashes(Segment& s, int seat, unsigned frame, bool parts)
{
    if (frame == netcode::NoFrame) {
        return;
    }
    const HashCell& own = s.own[frame % kHashRing];
    const HashCell& peer = s.peer[seat][frame % kHashRing];
    if (own.frame != frame || peer.frame != frame) {
        return;
    }
    if (s.rollback && frame >= s.timeline.Confirmed()) {
        return; // ours may still be a predicted frame's
    }
    if (!parts) {
        s.hashChecked++;
        if (own.hash != peer.hash) {
            if (s.hashMismatches == 0 && g_dumpFrame == netcode::NoFrame) {
                g_dumpFrame = frame + 256;
                mp::Log("DUMP_SCHEDULED frame=%u", g_dumpFrame);
                WriteItemTrace(frame);
            }
            s.hashMismatches++;
            LogDesync(s, frame, own, peer, seat, false);
        }
    } else if (own.hasParts && peer.hasParts && memcmp(own.parts, peer.parts, sizeof(own.parts)) != 0) {
        LogDesync(s, frame, own, peer, seat, true);
    }
}

void CheckReady(Segment& s)
{
    if (s.readyChecked || !s.readyTaken) {
        return;
    }
    for (int seat = 0; seat < g_net.players; seat++) {
        if (seat != g_net.seat && !s.peerReady[seat]) {
            return;
        }
    }
    s.readyChecked = true;
    mp::Log("READY segment=%u hash=%08X", s.index, s.readyHash);
}

void AcceptPacket(const Packet& p, int bytes, const sockaddr_in& from)
{
    if (bytes != (int)sizeof(Packet) || p.magic != kMagic || p.version != kVersion || p.bytes != sizeof(Packet) ||
        p.session != g_net.session || p.seat >= g_net.players || p.seat == g_net.seat) {
        return;
    }
    if (p.players != g_net.players) {
        Fail("peers selected different player counts");
        return;
    }
    if (((p.flags & 1) != 0) != mp::Cfg().rollback) {
        Fail("peers selected different rollback modes");
        return;
    }
    if (g_net.seat == 0) {
        g_net.guests[p.seat] = from;
        g_net.guestKnown[p.seat] = true;
        SendOut(&p, sizeof(p), p.seat);
    }
    // Each receiver gets its own echo: never read another PC's clock.
    g_net.roundTrip[p.seat].Receive(NowMicros(), p.sendTime, p.echoTime[g_net.seat], p.echoHold[g_net.seat]);
    Segment* target = NULL;
    if (p.segment == Current().index) {
        target = &Current();
    } else if (Previous() != NULL && p.segment == Previous()->index) {
        target = Previous();
    }
    if (target == NULL) {
        return;
    }
    Segment& s = *target;
    if (p.window != s.timeline.Limit()) {
        Fail("peers use different rollback windows");
        return;
    }
    if (p.delay > 12 || (!s.rollback && p.delay != s.delay)) {
        Fail("peers use different input delays");
        return;
    }
    for (unsigned i = 0; i < p.count && i < kHistory; i++) {
        if (!s.timeline.Receive(p.seat, p.first + i, p.input[i])) {
            Fail(s.timeline.Error());
            return;
        }
    }
    // Simulation positions, not queued input counts: each seat sets its own delay.
    if (!s.peerAdvantageKnown[p.seat] || p.sendTime - s.peerStamp[p.seat] < 0x80000000u) {
        s.peerAdvantage[p.seat] = (int)p.next - (int)p.seenNext[g_net.seat];
        s.peerNext[p.seat] = p.next;
        s.peerStamp[p.seat] = p.sendTime;
        s.peerAdvantageKnown[p.seat] = true;
    }
    unsigned acked = p.received[g_net.seat];
    if (acked > s.timeline.LocalCount()) {
        acked = s.timeline.LocalCount();
    }
    s.timeline.Acknowledge(p.seat, acked);
    if (p.flags & 2) {
        if (!s.peerReady[p.seat]) {
            s.peerReady[p.seat] = true;
            if (s.readyTaken && p.readyHash != s.readyHash) {
                char line[640];
                int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "DESYNC_READY segment=%u seat=%d local=%08X remote=%08X",
                                    s.index, p.seat, s.readyHash, p.readyHash);
                for (int i = 0; i < SIM_HASH_PART_COUNT && n > 0; i++) {
                    if (s.readyParts[i] != p.readyParts[i]) {
                        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %s=%08X/%08X", g_SimHashPartNames[i],
                                         s.readyParts[i], p.readyParts[i]);
                    }
                }
                mp::LogRaw(line);
            }
        }
        CheckReady(s);
    }
    if (p.hashFrame != netcode::NoFrame) {
        HashCell& cell = s.peer[p.seat][p.hashFrame % kHashRing];
        if (cell.frame != p.hashFrame) {
            cell.hasParts = false;
        }
        cell.frame = p.hashFrame;
        cell.hash = p.hash;
        CheckHashes(s, p.seat, p.hashFrame, false);
    }
    if (p.partFrame != netcode::NoFrame) {
        HashCell& cell = s.peer[p.seat][p.partFrame % kHashRing];
        if (cell.frame == p.partFrame) {
            memcpy(cell.parts, p.parts, sizeof(cell.parts));
            cell.hasParts = true;
            CheckHashes(s, p.seat, p.partFrame, true);
        }
    }
}

void AnswerLobby(const MultiplayerLobbyPacket& lobby, const sockaddr_in& from)
{
    if (lobby.magic != kMultiplayerLobbyMagic || lobby.version != kMultiplayerLobbyVersion) {
        return;
    }
    if (lobby.kind == kMultiplayerLobbyStart && g_net.seat != 0) {
        MultiplayerLobbyPacket ack = lobby;
        ack.kind = kMultiplayerLobbyStartAck;
        ack.senderSeat = g_net.seat;
        SendTo(&ack, sizeof(ack), from);
        mp::Log("LOBBY_START_ACK_RECOVERY");
    } else if (lobby.kind == kMultiplayerLobbyCancel) {
        Fail("lobby peer cancelled after handoff");
    }
}

void Receive()
{
    for (int i = 0; i < 256; i++) {
        unsigned char buffer[1500];
        sockaddr_in from;
        int size = sizeof(from);
        int got = recvfrom(g_net.socket, (char*)buffer, sizeof(buffer), 0, (sockaddr*)&from, &size);
        if (got < 0) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                return;
            }
            if (error == WSAECONNRESET || error == WSAEMSGSIZE) {
                continue;
            }
            char what[64];
            _snprintf_s(what, sizeof(what), _TRUNCATE, "recv error=%d", error);
            Fail(what);
            return;
        }
        if (got == (int)sizeof(MultiplayerLobbyPacket)) {
            AnswerLobby(*(const MultiplayerLobbyPacket*)buffer, from);
        } else if (got >= 4 && *(const unsigned*)buffer == kMagic) {
            AcceptPacket(*(const Packet*)buffer, got, from);
        }
        if (g_net.failed) {
            return;
        }
    }
}

void Send()
{
    SendSegment(Current());
    Segment* previous = Previous();
    if (previous != NULL && previous->tailFrames > 0) {
        previous->tailFrames--;
        SendSegment(*previous);
    }
}

void RecordHash(Segment& s, unsigned frame)
{
    MaybeDump(frame);
    if (frame % kHashPeriod != 0) {
        return;
    }
    HashCell& cell = s.own[frame % kHashRing];
    cell.frame = frame;
    cell.hash = SimFrameHash(cell.parts);
    cell.hasParts = true;
    TraceItems(frame);
    s.lastHashFrame = frame;
    if (frame % kPartPeriod == 0) {
        s.lastPartFrame = frame;
    }
    for (int seat = 0; seat < g_net.players; seat++) {
        if (seat != g_net.seat) {
            CheckHashes(s, seat, frame, false);
            CheckHashes(s, seat, frame, true);
        }
    }
}

void SwitchTo(unsigned index)
{
    Segment& old = Current();
    old.tailFrames = kTailFrames;
    if (old.phase == kGameplay) SessionLogGameEnd();
    mp::Log("SEGMENT_END index=%u frames=%u hash_checked=%u hash_mismatches=%u", old.index, old.timeline.Next(),
            old.hashChecked, old.hashMismatches);
    StartSegment(index);
}

int RunLockstep(Segment& s, int* present)
{
    if (s.phase == kGameplay) {
        if (!s.readyTaken) {
            s.readyHash = SimFrameHash(s.readyParts);
            s.readyTaken = true;
            CheckReady(s);
        }
        if (!s.readyChecked) {
            return 1;
        }
        if (SessionGameplayExitPending()) {
            SwitchTo(s.index + 1);
            return 1;
        }
    }
    if (!s.timeline.CanAdvance(false)) {
        g_net.waitFrames++;
        return 1;
    }
    int status = 1;
    s.timeline.Advance(
        [&](unsigned frame, const Inputs& inputs, bool) -> int {
            status = SessionRunFrame(inputs.held, g_net.players, 1);
            RecordHash(s, frame);
            return 1;
        },
        false);
    if (!s.timeline.Error()) {
        *present = 1;
    } else {
        Fail(s.timeline.Error());
    }
    g_net.framesRun++;
    if (s.phase == kMenu && SessionGameplayActive()) {
        SwitchTo(s.index + 1);
    }
    return status;
}

void OnRollbackFrame(unsigned frame, void* context)
{
    Segment& s = *(Segment*)context;
    MaybeDump(frame);
    if (frame % kHashPeriod != 0) {
        return;
    }
    HashCell& cell = s.own[frame % kHashRing];
    cell.frame = frame;
    cell.hash = SimFrameHash(cell.parts);
    cell.hasParts = true;
    TraceItems(frame);
}

void PublishFinalHashes(Segment& s)
{
    unsigned confirmed = s.timeline.Confirmed();
    if (confirmed == 0) {
        return;
    }
    unsigned frame = (confirmed - 1) / kHashPeriod * kHashPeriod;
    if (s.own[frame % kHashRing].frame != frame || frame == s.lastHashFrame) {
        return;
    }
    s.lastHashFrame = frame;
    if (frame % kPartPeriod == 0) {
        s.lastPartFrame = frame;
    }
    for (int seat = 0; seat < g_net.players; seat++) {
        if (seat != g_net.seat) {
            CheckHashes(s, seat, frame, false);
            CheckHashes(s, seat, frame, true);
        }
    }
}

// GGPO's frame advantage: a PC 1.5 frames or more ahead of its peer skips one host tick in four.
bool TimeSyncWait(Segment& s)
{
    static int enabled = -1;
    if (enabled < 0) {
        char value[8];
        enabled = !(GetEnvironmentVariableA("TH07_MP_TIME_SYNC", value, sizeof(value)) && value[0] == '0');
    }
    if (!enabled) {
        return false;
    }
    int mine = (int)s.timeline.Next();
    int ahead = INT_MIN;
    for (int seat = 0; seat < g_net.players; seat++) {
        if (seat == g_net.seat || (g_net.seat != 0 && seat != 0) || !s.peerAdvantageKnown[seat]) {
            continue;
        }
        int local = mine - (int)s.peerNext[seat];
        int difference = local - s.peerAdvantage[seat];
        if (difference > ahead) {
            ahead = difference;
        }
    }
    if (ahead == INT_MIN) {
        return false;
    }
    s.syncAdvantage = ahead;
    // A tick waited moves the peer's difference by 2: a lower bound would make both wait in turn.
    s.syncAverage += ((float)ahead - s.syncAverage) / 32.0f;
    if (s.syncAverage < 3.0f || ++s.syncTicks % 4 != 0) {
        return false;
    }
    s.syncWaits++;
    s.syncAverage -= 2.0f;
    return true;
}

void UpdateInputDelay(Segment& s)
{
    if (!s.rollback) return;
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    const unsigned keys = foregroundPid == GetCurrentProcessId()
        ? ((GetAsyncKeyState(VK_F5) & 0x8000 ? 1u : 0u) | (GetAsyncKeyState(VK_F6) & 0x8000 ? 2u : 0u)) : 0;
    const unsigned pressed = keys & ~g_net.delayKeys;
    g_net.delayKeys = keys;
    unsigned delay = g_net.gameplayDelay;
    if (pressed == 1 && delay) --delay;
    if (pressed == 2 && delay < 12) ++delay;
    static int testInterval = -1;
    if (testInterval < 0) {
        char text[16];
        testInterval = GetEnvironmentVariableA("TH07_MP_TEST_DELAY_CYCLE", text, sizeof(text)) ? atoi(text) : 0;
        if (testInterval < 60) testInterval = 0;
    }
    const unsigned step = testInterval ? s.timeline.Next() / (unsigned)testInterval : 0;
    if (step > g_net.testDelayStep) {
        static const unsigned delays[] = {0, 2, 5, 1, 0, 8, 3, 0};
        delay = delays[step % _countof(delays)];
        g_net.testDelayStep = step;
    }
    if (delay != g_net.gameplayDelay) {
        mp::Log("INPUT_DELAY segment=%u frame=%u old=%u new=%u source=%s", s.index, s.timeline.Next(),
                g_net.gameplayDelay, delay, pressed ? "key" : "test");
        g_net.gameplayDelay = s.delay = delay;
    }
}

int RunRollback(Segment& s, int* present)
{
    if (!s.readyTaken) {
        s.readyHash = SimFrameHash(s.readyParts);
        s.readyTaken = true;
        CheckReady(s);
    }
    if (!s.readyChecked) {
        return 1;
    }
    if (!s.testStalled) {
        s.testStalled = true;
        char value[16];
        if (GetEnvironmentVariableA("TH07_MP_TEST_STALL_MS", value, sizeof(value))) {
            Sleep((DWORD)atoi(value));
        }
    }
    if (TimeSyncWait(s)) {
        *present = 2;
        return 1;
    }
    unsigned before = s.timeline.Next();
    int r = rollback_game::RunFrame(s.timeline, present, OnRollbackFrame, &s);
    g_net.framesRun += s.timeline.Next() - before;
    if (s.timeline.Error()) {
        Fail(s.timeline.Error());
        return 0;
    }
    PublishFinalHashes(s);
    if (r == 2) {
        SwitchTo(s.index + 1);
        return 1;
    }
    return r;
}

}

int RunHostTick(int* present)
{
    *present = 0;
    if (!InitNetwork() || g_net.failed) {
        return 0;
    }
    g_net.hostFrames++;
    Receive();
    Segment& s = Current();
    UpdateInputDelay(s);
    unsigned short local = SessionLocalInput(s.phase == kGameplay);
    if (!s.timeline.SampleLocalDelayed(local, s.delay)) {
        Fail(s.timeline.Error());
        return 0;
    }
    Send();
    Receive();
    if (g_net.failed) {
        return 0;
    }
    int status;
    if (Current().rollback && rollback_game::Enabled()) {
        status = RunRollback(Current(), present);
    } else {
        status = RunLockstep(Current(), present);
    }
    if (*present) {
        g_net.stallSince = 0;
    } else if (g_net.stallSince == 0) {
        g_net.stallSince = NowMicros() | 1;
    }
    const unsigned nowMs = GetTickCount();
    ++g_net.perfTicks;
    if (*present != 1) {
        ++g_net.perfIdle;
        if (*present == 2) ++g_net.perfSync;
        if (!g_net.idleSinceMs) g_net.idleSinceMs = nowMs | 1;
        const unsigned idleMs = nowMs >= g_net.idleSinceMs ? nowMs - g_net.idleSinceMs : 0;
        if (idleMs > g_net.longestIdleMs) g_net.longestIdleMs = idleMs;
    } else {
        g_net.idleSinceMs = 0;
    }
    if (nowMs - g_net.perfMs >= 5000) {
        const unsigned dt = nowMs - g_net.perfMs;
        const unsigned next = Current().timeline.Next();
        const rollback_game::Stats& stats = rollback_game::GetStats();
        unsigned rtt = 0;
        const bool hasRtt = WorstRoundTrip(rtt);
        mp::Log("NET_PERF elapsed_ms=%u segment=%u phase=%s dt_ms=%u from=%u next=%u confirmed=%u "
                "fps_milli=%u ticks=%u idle=%u sync=%u longest_idle_ms=%u rollbacks=%u replayed=%u delay=%u rtt_us=%d",
                nowMs - g_net.startedMs, Current().index, Current().phase == kGameplay ? "gameplay" : "menu",
                dt, g_net.perfFrame, next, Current().timeline.Confirmed(),
                (unsigned)((unsigned long long)(next - g_net.perfFrame) * 1000000 / dt),
                g_net.perfTicks, g_net.perfIdle, g_net.perfSync, g_net.longestIdleMs,
                stats.rollbacks - g_net.perfRollbacks, stats.replayedFrames - g_net.perfReplayed,
                Current().delay, hasRtt ? (int)rtt : -1);
        g_net.perfMs = nowMs;
        g_net.perfFrame = next;
        g_net.perfTicks = g_net.perfIdle = g_net.perfSync = g_net.longestIdleMs = 0;
        g_net.perfRollbacks = stats.rollbacks;
        g_net.perfReplayed = stats.replayedFrames;
    }
    if (Current().timeline.Next() % 600 == 0 && Current().timeline.Next() != g_net.lastNetLog) {
        g_net.lastNetLog = Current().timeline.Next();
        unsigned rtt = 0;
        bool hasRtt = WorstRoundTrip(rtt);
        if (Current().rollback && rollback_game::Enabled()) {
            rollback_game::LogPerf();
        }
        MpLogBgmState();
        mp::Log("NET host_frame=%u segment=%u next=%u confirmed=%u run=%u wait=%u rtt_us=%d hash_checked=%u "
                "hash_mismatches=%u rollbacks=%u replayed=%u max_rollback=%u advantage=%d sync_waits=%u",
                g_net.hostFrames, Current().index, Current().timeline.Next(), Current().timeline.Confirmed(),
                g_net.framesRun, g_net.waitFrames, hasRtt ? (int)rtt : -1, Current().hashChecked,
                Current().hashMismatches, rollback_game::GetStats().rollbacks, rollback_game::GetStats().replayedFrames,
                rollback_game::GetStats().maxRollback, Current().syncAdvantage, Current().syncWaits);
    }
    return g_net.failed ? 0 : status;
}

unsigned TestInputFrame()
{
    return Current().timeline.Next();
}

unsigned TestGameIndex()
{
    static int offset = -1;
    if (offset < 0) {
        char text[16];
        offset = GetEnvironmentVariableA("TH07_MP_TEST_CAMPAIGN_OFFSET", text, sizeof(text)) ? atoi(text) : 0;
        if (offset < 0 || offset > 3) offset = 0;
    }
    return g_net.current / 2 + (unsigned)offset;
}

unsigned short TestBotMask(int seat)
{
    return SessionBotMask(seat);
}

}
}

namespace th07 {
namespace net {

void GetStatus(Status* out)
{
    out->inputDelay = g_net.initialized ? g_net.gameplayDelay : mp::Cfg().artificialDelay;
    out->hasRoundTrip = g_net.initialized && WorstRoundTrip(out->roundTripMicros);
    static unsigned s_waitingMask;
    static unsigned s_waitingSeen;
    unsigned now = NowMicros();
    if (g_net.initialized && g_net.stallSince != 0 && now - g_net.stallSince > 300000) {
        const Segment& s = Current();
        unsigned frame = s.rollback ? s.timeline.Confirmed() : s.timeline.Next();
        unsigned mask = s.timeline.MissingMask(frame) & ~(1u << g_net.seat);
        if (mask != 0) {
            s_waitingMask = mask;
            s_waitingSeen = now;
        }
    }
    out->waitingMask = s_waitingMask != 0 && now - s_waitingSeen < 1000000 ? s_waitingMask : 0;
}

}
}
