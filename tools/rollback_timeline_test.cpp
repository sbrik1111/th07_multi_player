// Portable regression tests for the actual game input timeline, including
// immutable wire history during live delay changes and prolonged ACK loss.
#include "multi/RollbackNetcode.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <vector>

using namespace th07::netcode;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
using State = std::uint64_t;

State Step(State s, unsigned frame, const Inputs& in, unsigned players)
{
    s = s * 6364136223846793005ULL + frame + 1;
    for (unsigned p = 0; p < players; ++p) s = (s ^ in.held[p]) * 1099511628211ULL;
    return s;
}

void DelayEdges()
{
    Timeline t;
    CHECK(t.Initialize(0, 8));
    CHECK(t.SampleLocalDelayed(7, 2));
    std::uint16_t wire[History] = {};
    CHECK(t.SendHistory(wire) == 3 && wire[0] == 0 && wire[1] == 0 && wire[2] == 7);
    CHECK(t.SampleLocalDelayed(9, 5));
    CHECK(t.SendHistory(wire) == 6 && wire[3] == 7 && wire[4] == 7 && wire[5] == 9);
    CHECK(t.SampleLocalDelayed(99, 0));
    CHECK(t.SendHistory(wire) == 6 && wire[5] == 9);
    CHECK(t.SampleLocalDelayed(88, 5)); // cannot rewrite even in the same host tick
    CHECK(t.SendHistory(wire) == 6 && wire[5] == 9);
    CHECK(!t.SampleLocalDelayed(0, History));
}

void IndependentClocks()
{
    // Echoes belong to the recipient, even when the sender broadcasts a
    // packet through a host. Include a clock that wraps during the test.
    RoundTrip peers[3][3];
    const unsigned offsets[] = {1000000000u, 2000000000u, 0xffff0000u};
    struct Echo { unsigned at, from, to, send, echo, hold; };
    std::vector<Echo> pending;
    for (unsigned tick = 0; tick < 300; ++tick) {
        for (const Echo& packet : pending) {
            peers[packet.to][packet.from].Receive(offsets[packet.to] + packet.at,
                                                  packet.send, packet.echo, packet.hold);
            peers[packet.to][packet.from].Publish(offsets[packet.to] + packet.at);
        }
        pending.clear();
        for (unsigned sender = 0; sender < 3; ++sender) {
            unsigned send = 0, echoes[3] = {}, holds[3] = {};
            const unsigned now = offsets[sender] + tick * 20000;
            for (unsigned receiver = 0; receiver < 3; ++receiver)
                peers[sender][receiver].Stamp(now, send, echoes[receiver], holds[receiver]);
            for (unsigned receiver = 0; receiver < 3; ++receiver) if (receiver != sender) {
                const unsigned transit = 3000 + (sender + receiver) * 1000;
                pending.push_back({tick * 20000 + transit, sender, receiver, send, echoes[receiver], holds[receiver]});
            }
        }
    }
    for (unsigned a = 0; a < 3; ++a) for (unsigned b = 0; b < 3; ++b) if (a != b) {
        unsigned micros = 0;
        CHECK(peers[a][b].Display(micros));
        CHECK(micros == 2 * (3000 + (a + b) * 1000));
    }
    std::puts("PASS independent RTT echoes with unrelated/wrapping clocks");
}

void CorrectedSceneBoundary()
{
    Timeline t;
    CHECK(t.Initialize(0, 8, 4));
    struct Scene { State value = 1; bool pending = false; } scene;
    std::map<unsigned, Scene> checkpoints;
    unsigned loads = 0, stops = 0;
    const auto restore = [&](unsigned frame) -> bool {
        if (!checkpoints.count(frame)) return false;
        scene = checkpoints.at(frame);
        return true;
    };
    const auto step = [&](unsigned frame, const Inputs& in, bool replay) -> int {
        checkpoints[frame] = scene;
        if (scene.pending) {
            CHECK(!replay);
            CHECK(!t.NeedsCheckpoint(frame));
            ++loads;
            scene.pending = false;
        }
        scene.value = Step(scene.value, frame, in, 4);
        if (frame == 1 && in.held[1]) scene.pending = true;
        return 1;
    };
    const auto blocked = [&](unsigned) -> bool {
        if (scene.pending) ++stops;
        return scene.pending;
    };
    for (unsigned frame = 0; frame < 6; ++frame) {
        CHECK(t.SampleLocal(0));
        if (!frame) for (unsigned seat = 1; seat < 4; ++seat) CHECK(t.Receive(seat, frame, 0));
        CHECK(t.Advance(step));
    }
    CHECK(t.Next() == 6 && t.Confirmed() == 1 && loads == 0);
    CHECK(t.Receive(1, 1, 1)); // the correction reaches the load at frame 2
    CHECK(t.Receive(2, 1, 0)); // seat 3's preceding input is still missing
    for (unsigned seat = 1; seat < 4; ++seat) CHECK(t.Receive(seat, 2, 0));
    CHECK(t.Repair(restore, step, blocked));
    CHECK(t.Next() == 2 && t.Confirmed() == 1 && loads == 0 && stops == 1);
    CHECK(!t.CanAdvance(false));
    Inputs used;
    CHECK(!t.Used(2, used)); // discard the whole predicted suffix
    CHECK(t.Receive(3, 1, 2));
    CHECK(t.Repair(restore, step, blocked));
    CHECK(t.Confirmed() == 2 && t.CanAdvance(false));
    CHECK(t.Advance(step, false));
    CHECK(loads == 1 && t.Confirmed() == 3);
    for (unsigned frame = 3; frame < 6; ++frame) {
        for (unsigned seat = 1; seat < 4; ++seat) CHECK(t.Receive(seat, frame, 0));
        CHECK(t.Dirty() == NoFrame && t.CanAdvance(false));
        CHECK(t.Advance(step, false));
    }
    State expected = 1;
    for (unsigned frame = 0; frame < 6; ++frame) {
        Inputs in;
        if (frame == 1) { in.held[1] = 1; in.held[3] = 2; }
        expected = Step(expected, frame, in, 4);
    }
    CHECK(scene.value == expected && t.Confirmed() == 6 && loads == 1);
    std::puts("PASS corrected scene boundary waits for confirmed inputs and loads once");
}

struct Peer {
    Timeline timeline;
    State state = 1;
    std::map<unsigned, State> before, after;
    std::map<unsigned, std::uint16_t> published;
};
struct Wire {
    unsigned at, from, to, first, count, ack;
    std::uint16_t input[History];
};

void Network(unsigned players)
{
    std::mt19937 random(7000 + players);
    Peer peers[kMaxPlayers];
    for (unsigned p = 0; p < players; ++p) CHECK(peers[p].timeline.Initialize(p, 8, players));
    std::vector<Wire> pending;
    unsigned audited = 0, changes = 0, oldDelay[kMaxPlayers] = {};
    State canonical = 1;
    static const unsigned delays[] = {0, 2, 12, 1, 0, 8, 3, 0};
    for (unsigned tick = 0; tick < 30000; ++tick) {
        // Remove packets by swap, deliberately receiving same-time packets
        // out of order. A long outage fills retransmission history.
        for (unsigned i = 0; i < pending.size();) {
            if (pending[i].at > tick) { ++i; continue; }
            const Wire w = pending[i];
            pending[i] = pending.back(); pending.pop_back();
            auto& t = peers[w.to].timeline;
            for (unsigned j = 0; j < w.count; ++j) CHECK(t.Receive(w.from, w.first + j, w.input[j]));
            CHECK(t.Acknowledge(w.from, w.ack));
        }
        for (unsigned p = 0; p < players; ++p) {
            Peer& peer = peers[p];
            Timeline& t = peer.timeline;
            const unsigned delay = delays[(tick / (113 + p * 37)) % 8];
            changes += delay != oldDelay[p]; oldDelay[p] = delay;
            CHECK(t.SampleLocalDelayed(static_cast<std::uint16_t>(random() & 0x7fff), delay));
            Wire w = {};
            w.from = p; w.first = t.PeerAck(); w.count = t.SendHistory(w.input);
            CHECK(w.count <= History);
            for (unsigned j = 0; j < w.count; ++j) {
                auto inserted = peer.published.emplace(w.first + j, w.input[j]);
                CHECK(inserted.second || inserted.first->second == w.input[j]);
            }
            for (unsigned q = 0; q < players; ++q) if (q != p) {
                if ((tick >= 800 && tick < 1300) || random() % 10 == 0) continue;
                w.to = q; w.ack = t.Received(q); w.at = tick + 1 + random() % 18;
                pending.push_back(w);
            }
            const auto restore = [&](unsigned f) -> bool {
                const auto it = peer.before.find(f);
                if (it == peer.before.end()) return false;
                peer.state = it->second;
                return true;
            };
            const auto step = [&](unsigned f, const Inputs& in, bool) -> int {
                peer.before[f] = peer.state;
                peer.state = Step(peer.state, f, in, players);
                peer.after[f] = peer.state;
                return 1;
            };
            CHECK(t.Repair(restore, step));
            if (t.CanAdvance() && random() % 8 != 0) CHECK(t.Advance(step));
        }
        unsigned final = peers[0].timeline.Confirmed();
        for (unsigned p = 1; p < players; ++p) final = (std::min)(final, peers[p].timeline.Confirmed());
        while (audited < final) {
            Inputs in;
            for (unsigned p = 0; p < players; ++p) {
                CHECK(peers[p].published.count(audited));
                in.held[p] = peers[p].published.at(audited);
            }
            canonical = Step(canonical, audited, in, players);
            for (unsigned p = 0; p < players; ++p) CHECK(peers[p].after.at(audited) == canonical);
            ++audited;
        }
    }
    CHECK(audited > 5000 && changes > 100);
    unsigned rollbacks = 0;
    for (unsigned p = 0; p < players; ++p) rollbacks += peers[p].timeline.Rollbacks();
    CHECK(rollbacks > 100);
    std::printf("PASS players=%u confirmed=%u delay_changes=%u rollbacks=%u\n", players, audited, changes, rollbacks);
}

int main()
{
    DelayEdges();
    IndependentClocks();
    CorrectedSceneBoundary();
    for (unsigned players = 2; players <= 4; ++players) Network(players);
}
