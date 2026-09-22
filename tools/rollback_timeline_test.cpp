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
    for (unsigned players = 2; players <= 4; ++players) Network(players);
}
