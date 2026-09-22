#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace th07 { namespace netcode {

// Must match MAX_PLAYERS.
const unsigned kMaxPlayers = 4;

constexpr unsigned NoFrame = 0xFFFFFFFFu;
constexpr unsigned History = 32;
constexpr unsigned Ring = 128;
constexpr unsigned MaxPrediction = 16;
struct Inputs { std::uint16_t held[kMaxPlayers] = {}; };

class Timeline {
    struct Cell {
        unsigned frame = NoFrame;
        Inputs actual, used;
        bool known[kMaxPlayers] = {}, simulated = false;
    };
    std::array<Cell, Ring> cells_ = {};
    unsigned seat_ = 0, limit_ = 8, next_ = 0, confirmed_ = 0;
    unsigned players_ = 2, localCount_ = 0;
    unsigned received_[kMaxPlayers] = {}, acknowledged_[kMaxPlayers] = {};
    unsigned dirty_ = NoFrame, rollbacks_ = 0;
    const char* error_ = nullptr;

    bool Fail(const char* reason) { error_ = reason; return false; }
    bool AllKnown(const Cell* c) const {
        if (!c) return false;
        for (unsigned s = 0; s < players_; ++s) if (!c->known[s]) return false;
        return true;
    }
    const Cell* Find(unsigned frame) const {
        const auto& c = cells_[frame % Ring];
        return c.frame == frame ? &c : nullptr;
    }
    Cell& At(unsigned frame) {
        auto& c = cells_[frame % Ring];
        if (c.frame != frame) { c = Cell{}; c.frame = frame; }
        return c;
    }
    void Confirm() {
        // Never confirm predicted bytes before repairing the affected suffix.
        if (dirty_ != NoFrame) return;
        while (confirmed_ < next_) {
            const auto* c = Find(confirmed_);
            if (!AllKnown(c)) break;
            ++confirmed_;
        }
    }
public:
    bool Initialize(unsigned seat, unsigned limit, unsigned players = 2) {
        if (players < 2 || players > kMaxPlayers || seat >= players || !limit || limit > MaxPrediction)
            return Fail("invalid rollback seat/window");
        *this = Timeline{};
        seat_ = seat; limit_ = limit; players_ = players;
        return true;
    }
    const char* Error() const { return error_; }
    unsigned Next() const { return next_; }
    unsigned Confirmed() const { return confirmed_; }
    unsigned Received(unsigned seat) const { return seat == seat_ ? localCount_ : received_[seat]; }
    unsigned RemoteReceived() const {
        unsigned result = NoFrame;
        for (unsigned s = 0; s < players_; ++s) if (s != seat_) result = (std::min)(result, received_[s]);
        return result;
    }
    unsigned PeerAck() const {
        unsigned result = localCount_;
        for (unsigned s = 0; s < players_; ++s) if (s != seat_) result = (std::min)(result, acknowledged_[s]);
        return result;
    }
    unsigned LocalCount() const { return localCount_; }
    unsigned MissingMask(unsigned frame) const {
        const Cell* c = Find(frame);
        unsigned mask = 0;
        for (unsigned s = 0; s < players_; ++s) if (!c || !c->known[s]) mask |= 1u << s;
        return mask;
    }
    unsigned Dirty() const { return dirty_; }
    unsigned Rollbacks() const { return rollbacks_; }
    unsigned Limit() const { return limit_; }

    // During Repair, Confirmed() has not advanced yet: inspect the whole prefix.
    bool NeedsCheckpoint(unsigned frame) const {
        if (frame == NoFrame || frame > next_) return true;
        for (unsigned f = confirmed_; f <= frame; ++f)
            if (!AllKnown(Find(f))) return true;
        return false;
    }

    bool SampleLocal(std::uint16_t mask) {
        if (error_) return false;
        if (next_ == NoFrame) return Fail("rollback frame counter exhausted");
        // Stop before a lost ACK can evict an input.
        if (next_ >= PeerAck() && next_ - PeerAck() >= History) return true;
        auto& c = At(next_);
        if (!c.known[seat_]) {
            c.actual.held[seat_] = mask; c.known[seat_] = true;
            localCount_ = next_ + 1;
        }
        return true;
    }
    // Sent input is immutable: an increase pads with the last held input, a decrease waits for the
    // queue to drain.
    bool SampleLocalDelayed(std::uint16_t mask, unsigned delay) {
        if (error_) return false;
        if (delay >= History || next_ >= NoFrame - delay)
            return Fail("invalid input delay/frame");
        const unsigned target = next_ + delay;
        if (localCount_ > target) return true;
        const unsigned first = PeerAck();
        if (target >= first && target - first >= History) return true;
        const Cell* previous = localCount_ ? Find(localCount_ - 1) : nullptr;
        const std::uint16_t held = previous ? previous->actual.held[seat_] : 0;
        while (localCount_ <= target) {
            auto& c = At(localCount_);
            c.actual.held[seat_] = localCount_ == target ? mask : held;
            c.known[seat_] = true;
            ++localCount_;
        }
        return true;
    }
    bool Acknowledge(unsigned exclusive) { return Acknowledge(seat_ ^ 1, exclusive); }
    bool Acknowledge(unsigned peer, unsigned exclusive) {
        if (peer >= players_ || peer == seat_) return Fail("invalid acknowledging seat");
        if (exclusive > localCount_) return Fail("peer acknowledged unsent input");
        if (exclusive > acknowledged_[peer]) acknowledged_[peer] = exclusive;
        return true;
    }
    bool Receive(unsigned frame, std::uint16_t mask) { return Receive(seat_ ^ 1, frame, mask); }
    bool Receive(unsigned remote, unsigned frame, std::uint16_t mask) {
        if (error_) return false;
        if (remote >= players_ || remote == seat_) return Fail("invalid remote seat");
        // Rewind lowers next_ but not localCount_.
        const unsigned base = localCount_ > next_ ? localCount_ : next_;
        if (frame == NoFrame || (frame >= base && frame - base >= History))
            return Fail("remote input outside receive window");
        // Old reordered datagrams must not overwrite a reused ring slot.
        if (frame < next_ && next_ - frame >= Ring) return true;
        auto& c = At(frame);
        if (c.known[remote])
            return c.actual.held[remote] == mask || Fail("conflicting remote input");
        if (frame < confirmed_) return Fail("missing confirmed input history");
        c.actual.held[remote] = mask; c.known[remote] = true;
        if (c.simulated && c.used.held[remote] != mask && frame < dirty_) dirty_ = frame;
        while (const auto* next = Find(received_[remote])) {
            if (!next->known[remote]) break;
            ++received_[remote];
        }
        Confirm();
        return true;
    }
    unsigned SendHistory(std::uint16_t (&out)[History]) const {
        const unsigned first = PeerAck();
        const unsigned count = localCount_ - first;
        for (unsigned i = 0; i < count; ++i) out[i] = Find(first + i)->actual.held[seat_];
        return count;
    }
    Inputs Select(unsigned frame) const {
        const auto* c = Find(frame);
        Inputs result = c->actual;
        for (unsigned remote = 0; remote < players_; ++remote) if (!c->known[remote]) {
            const auto* previous = frame ? Find(frame - 1) : nullptr;
            result.held[remote] = previous ? previous->used.held[remote] : 0;
        }
        return result;
    }
    bool Used(unsigned frame, Inputs& out) const {
        const auto* c = Find(frame);
        if (!c || !c->simulated) return false;
        out = c->used;
        return true;
    }
    // predict=false: advance only on a final frame.
    bool CanAdvance(bool predict = true) const {
        const auto* c = Find(next_);
        return !error_ && dirty_ == NoFrame && next_ != NoFrame
            && next_ - confirmed_ < limit_ && (PeerAck() > next_ || next_ - PeerAck() < History)
            && c && c->known[seat_]
            && (predict || (confirmed_ == next_ && AllKnown(c)));
    }
    // step returns 1 simulated, 0 failure, -1 a boundary (not simulated). No callback may receive
    // packets.
    static unsigned RestoredFrom(unsigned frame, unsigned) { return frame; }
    static unsigned RestoredFrom(bool restored, unsigned first) { return restored ? first : NoFrame; }
    template<class Restore, class Step>
    bool Repair(Restore restore, Step step) {
        if (error_) return false;
        if (dirty_ == NoFrame) { Confirm(); return true; }
        const unsigned first = dirty_;
        if (first < confirmed_ || next_ - first > limit_)
            return Fail("correction outside retained checkpoints");
        const unsigned from = RestoredFrom(restore(first), first);
        if (from == NoFrame || from > first) return Fail("checkpoint restore failed");
        for (unsigned f = from; f < next_; ++f) {
            auto input = Select(f);
            const int r = step(f, input, true);
            if (r < 0) {
                for (unsigned g = f; g < next_; ++g) {
                    auto& c = cells_[g % Ring];
                    if (c.frame == g) c.simulated = false;
                }
                next_ = f;
                break;
            }
            if (!r) return Fail("corrected simulation failed");
            At(f).used = input;
        }
        dirty_ = NoFrame;
        ++rollbacks_;
        Confirm();
        return true;
    }
    bool Rewind(unsigned first) {
        if (error_) return false;
        if (first == NoFrame || first > next_ || first < confirmed_ || next_ - first > limit_)
            return Fail("rewind outside retained checkpoints");
        for (unsigned g = first; g < next_; ++g) {
            auto& c = cells_[g % Ring];
            if (c.frame == g) c.simulated = false;
        }
        next_ = first;
        dirty_ = NoFrame;
        Confirm();
        return true;
    }
    template<class Step>
    bool Advance(Step step, bool predict = true) {
        if (!CanAdvance(predict)) return false;
        auto input = Select(next_);
        const int r = step(next_, input, false);
        if (r < 0) return true;
        if (!r) return Fail("forward simulation failed");
        auto& c = At(next_);
        c.used = input; c.simulated = true;
        ++next_;
        Confirm();
        return true;
    }
};

constexpr unsigned Magic = 0x52363154u; // T07R
constexpr unsigned Version = 6;
constexpr unsigned HashSampleFrames = 16, PartSampleFrames = 64;
struct ReadyState {
    unsigned hash = 0, rng[14] = {}, anm = 0;
    unsigned generation = 0, stageFrame = 0, loadout[kMaxPlayers][5] = {}, infiniteLives = 0;
    unsigned playerCount = 2;
    // Packets of an earlier game are ignored.
    unsigned epoch = 0;
};
#pragma pack(push, 1)
struct Packet {
    unsigned magic = Magic, version = Version, bytes = sizeof(Packet);
    unsigned session = 0, seat = 0, window = 0;
    ReadyState ready;
    unsigned received = 0, first = 0, count = 0;
    unsigned receivedBySeat[kMaxPlayers] = {};
    std::uint16_t input[History] = {};
    unsigned sendTime = 0, echoTime = 0, echoHold = 0;
    unsigned hashFrame = NoFrame, hash = 0;
    unsigned partFrame = NoFrame, parts[15] = {};
};
#pragma pack(pop)
static_assert(sizeof(Packet) <= 1472, "rollback packet exceeds IPv4 MTU");

// RTT = now - echo - hold, without synchronized clocks.
constexpr unsigned NoEcho = 0xFFFFFFFFu;
class RoundTrip {
    unsigned peerTime_ = 0, peerArrival_ = 0, samples_[256] = {}, count_ = 0;
    unsigned windowStart_ = 0, lastSample_ = 0, display_ = 0;
    bool peer_ = false, started_ = false, valid_ = false;
public:
    void Stamp(unsigned now, unsigned& send, unsigned& echo, unsigned& hold) const {
        send = now;
        echo = peer_ ? peerTime_ : 0;
        hold = peer_ ? now - peerArrival_ : NoEcho;
    }
    void Receive(unsigned now, unsigned send, unsigned echo, unsigned hold) {
        // Reordered datagrams must not rewind the echoed peer time.
        if (!peer_ || send - peerTime_ < 0x80000000u) { peerTime_ = send; peerArrival_ = now; peer_ = true; }
        if (hold == NoEcho) return;
        const unsigned rtt = now - echo - hold;
        if (rtt > 2000000u) return;
        if (count_ < 256) samples_[count_++] = rtt;
        lastSample_ = now;
        if (!started_) { started_ = true; windowStart_ = now; }
    }
    void Publish(unsigned now) {
        if (!started_ || now - windowStart_ < 1000000u) return;
        windowStart_ = now;
        if (count_) {
            std::sort(samples_, samples_ + count_);
            display_ = samples_[count_ / 4]; valid_ = true; count_ = 0;
        } else if (now - lastSample_ > 3000000u) valid_ = false;
    }
    bool Display(unsigned& micros) const { micros = display_; return valid_; }
};

inline bool ValidPacket(const Packet& p, unsigned bytes, unsigned session, unsigned localSeat, unsigned players = 2) {
    return bytes == sizeof(Packet) && p.magic == Magic && p.version == Version
        && p.bytes == sizeof(Packet) && p.session == session && p.seat < players && p.seat != localSeat
        && p.ready.playerCount == players
        && p.window && p.window <= MaxPrediction && p.count <= History
        && p.first <= NoFrame - p.count && p.received != NoFrame;
}

} }

namespace th07 { namespace rollback { namespace netcode = ::th07::netcode; } }
