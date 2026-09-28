
#include <string.h>
#include <stdlib.h>

#include "multi/RollbackGame.h"
#include "multi/MpConfig.h"
#include "multi/RollbackCpu.h"
#include "multi/RollbackHeap.h"
#include "multi/RollbackMemory.h"
#include "multi/SessionFrame.h"
#include "AnmManager.hpp"
#include "Chain.hpp"
#include "Coop.hpp"
#include "GameWindow.hpp"
#include "Player.hpp"
#include "SoundPlayer.hpp"
#include <windows.h>
#include "multi/RuntimeData.h"

namespace th07 {
namespace rollback_game {
namespace {

using netcode::Inputs;
using netcode::MaxPrediction;
using netcode::NoFrame;

struct Checkpoint {
    unsigned frame;
    rollback::Memory::Snapshot bytes;
    rollback::CpuState cpu;
};

rollback::Memory* g_memory;
bool g_enabled;
Checkpoint g_checkpoints[MaxPrediction];
unsigned g_restoredFrame = NoFrame;
// Sparse: a capture costs about a millisecond; rollbacks are rare.
unsigned g_checkpointEvery = 4;
bool g_drawReplay;
Stats g_stats;

struct Perf {
    unsigned long long captureUs, restoreUs, captureBytes;
    unsigned captures, restores, frames, replayed;
};
Perf g_perf;

unsigned g_stepFrame = NoFrame;
bool g_stepReplay;
int g_stepBgmSeq;

// A frame run again resends a music command only if its first run did not send it.
struct BgmRecord {
    unsigned frame;
    int seq;
    int type;
    int arg;
    char name[0x80];
};
BgmRecord g_bgmRecords[64];
unsigned g_bgmRecordNext;

i32 FilterBgmCommand(i32 type, i32 arg, const char* name)
{
    if (g_stepFrame == NoFrame) {
        return g_SoundSilenced ? 0 : 1;
    }
    int seq = g_stepBgmSeq++;
    BgmRecord* slot = NULL;
    for (BgmRecord& r : g_bgmRecords) {
        if (r.frame == g_stepFrame && r.seq == seq) {
            slot = &r;
            break;
        }
    }
    if (g_stepReplay && slot != NULL && slot->type == type && slot->arg == arg && strcmp(slot->name, name) == 0) {
        return 0;
    }
    if (slot == NULL) {
        slot = &g_bgmRecords[g_bgmRecordNext++ % 64];
    }
    slot->frame = g_stepFrame;
    slot->seq = seq;
    slot->type = type;
    slot->arg = arg;
    strncpy_s(slot->name, sizeof(slot->name), name, _TRUNCATE);
    if (g_stepReplay) {
        mp::Log("RB_BGM frame=%u type=%d arg=%d %s (first sent in a repair)", g_stepFrame, type, arg, name);
    }
    return 1;
}

// A frame's hit sound plays once it is confirmed.
struct HitRecord {
    unsigned frame;
    int hits[4];
    bool immediate;
};
HitRecord g_hitRecords[netcode::Ring];
unsigned g_hitAnnounced = NoFrame;
int g_hitHeard[4];

void RecordHits(unsigned frame, bool immediate)
{
    HitRecord& r = g_hitRecords[frame % netcode::Ring];
    r.frame = frame;
    r.immediate = immediate;
    for (int seat = 0; seat < 4; seat++) {
        r.hits[seat] = seat < mp::PlayerCount() ? g_Players[seat].hitSounds : 0;
    }
}

void AnnounceConfirmedHits(unsigned confirmed)
{
    if (g_hitAnnounced == NoFrame) {
        g_hitAnnounced = confirmed;
        return;
    }
    for (unsigned f = g_hitAnnounced; f < confirmed; f++) {
        const HitRecord& r = g_hitRecords[f % netcode::Ring];
        if (r.frame != f) {
            continue;
        }
        bool sound = false;
        for (int seat = 0; seat < 4; seat++) {
            if (r.hits[seat] > g_hitHeard[seat] && !r.immediate) {
                sound = true;
            }
            g_hitHeard[seat] = r.hits[seat];
        }
        if (sound) {
            g_SoundPlayer.PlaySoundByIdx(SOUND_PICHUN, 0);
            mp::Log("RB_HIT_SOUND frame=%u confirmed=%u", f, confirmed);
        }
    }
    if (confirmed > g_hitAnnounced) {
        g_hitAnnounced = confirmed;
    }
}

struct TickCost {
    void* function;
    long long ticks;
    unsigned calls;
};
TickCost g_tickCosts[64];

void ProfileReplayTick(void* function, long long ticks)
{
    for (TickCost& c : g_tickCosts) {
        if (c.function == function || c.function == NULL) {
            c.function = function;
            c.ticks += ticks;
            c.calls++;
            return;
        }
    }
}

unsigned long long PerfNow()
{
    static LARGE_INTEGER frequency;
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (unsigned long long)(now.QuadPart * 1000000 / frequency.QuadPart);
}
int g_trace = -1;

bool Trace()
{
    if (g_trace < 0) {
        char value[8];
        g_trace = GetEnvironmentVariableA("TH07_ROLLBACK_TRACE", value, sizeof(value)) && value[0] == '1';
    }
    return g_trace != 0;
}

void Fail(const char* what)
{
    mp::Log("FAIL rollback %s heap=%s memory=%s", what, rollback::heap::LastError(),
            g_memory != NULL ? g_memory->LastError() : "none");
    ExitProcess(91);
}

bool FindSection(const char* name, unsigned char** begin, size_t* bytes)
{
    unsigned char* base = (unsigned char*)GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
        if (strncmp((const char*)section->Name, name, IMAGE_SIZEOF_SHORT_NAME) == 0) {
            *begin = base + section->VirtualAddress;
            *bytes = section->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

bool RegisterSection(const char* name, unsigned key)
{
    unsigned char* begin;
    size_t bytes;
    if (!FindSection(name, &begin, &bytes)) {
        mp::Log("ROLLBACK_ROOTS no section %s", name);
        return true;
    }
    struct Hole {
        unsigned char* at;
        size_t bytes;
    } holes[1] = {
        {NULL, 0},
    };
    const size_t count = 0;
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (holes[j].at < holes[i].at) {
                Hole t = holes[i];
                holes[i] = holes[j];
                holes[j] = t;
            }
        }
    }
    unsigned char* end = begin + bytes;
    unsigned char* at = begin;
    unsigned part = 0;
    for (size_t i = 0; i < count; i++) {
        if (holes[i].at + holes[i].bytes <= begin || holes[i].at >= end) {
            continue;
        }
        if (holes[i].at > at && !g_memory->AddRegion(key + part++, at, holes[i].at - at)) {
            return false;
        }
        if (holes[i].at + holes[i].bytes > at) {
            at = holes[i].at + holes[i].bytes;
        }
    }
    if (at < end && !g_memory->AddRegion(key + part++, at, end - at)) {
        return false;
    }
    mp::Log("ROLLBACK_ROOTS section=%s at=%p bytes=%u regions=%u", name, begin, (unsigned)bytes, part);
    return true;
}

void Capture(Checkpoint& c, unsigned frame)
{
    unsigned long long begin = PerfNow();
    // A queued batch still references the vertex scratch: never save one.
    if (g_AnmManager != NULL && g_AnmManager->spritesToDraw != 0) {
        Fail("capture with an unfinished sprite batch");
    }
    if (!c.cpu.Capture()) {
        Fail("capture FP environment");
    }
    if (!rollback::heap::Capture(frame, c.bytes)) {
        Fail("capture");
    }
    c.frame = frame;
    g_stats.checkpoints++;
    g_perf.captureUs += PerfNow() - begin;
    g_perf.captures++;
    g_perf.captureBytes += c.bytes.Bytes();
    if (Trace()) {
        mp::Log("RB capture frame=%u bytes=%u", frame, (unsigned)c.bytes.Bytes());
    }
}

void Restore(const Checkpoint& c)
{
    unsigned long long begin = PerfNow();
    if (!rollback::heap::Restore(c.bytes)) {
        Fail("restore");
    }
    if (!c.cpu.Restore()) {
        Fail("restore FP environment");
    }
    g_perf.restoreUs += PerfNow() - begin;
    g_perf.restores++;
}

void DropCheckpoint(Checkpoint& c)
{
    c.frame = NoFrame;
    c.bytes = rollback::Memory::Snapshot();
}

}

// Made at the first use: GameStaticBlock places the game's large objects here during static
// initialization.
rollback::Memory* SharedArena()
{
    static rollback::Memory* s_arena;
    static bool s_tried;
    if (!s_tried) {
        s_tried = true;
        rollback::Memory* memory = new rollback::Memory;
        if (memory->Initialize(rollback::kDefaultArenaBytes)) {
            s_arena = memory;
        } else {
            delete memory;
        }
    }
    return s_arena;
}

bool Start()
{
    rollback::heap::RuntimeScope runtime;
    g_memory = SharedArena();
    if (g_memory == NULL || g_memory->Base() == NULL || !rollback::heap::Install(*g_memory)) {
        mp::Log("FAIL rollback arena: %s / %s", g_memory != NULL ? g_memory->LastError() : "none",
                rollback::heap::LastError());
        return false;
    }
    if (!RegisterSection(".gdata", 0x100) || !RegisterSection(".gbss", 0x200)) {
        mp::Log("FAIL rollback roots: %s", g_memory->LastError());
        return false;
    }
    for (unsigned i = 0; i < MaxPrediction; i++) {
        g_checkpoints[i].frame = NoFrame;
    }
    char value[8];
    if (GetEnvironmentVariableA("TH07_ROLLBACK_CHECKPOINT_EVERY", value, sizeof(value))) {
        unsigned every = (unsigned)atoi(value);
        g_checkpointEvery = every >= 1 && every <= 4 ? every : 4;
    }
    g_drawReplay = GetEnvironmentVariableA("TH07_ROLLBACK_DRAW_REPLAY", value, sizeof(value)) && value[0] == '1';
    mp::Log("ROLLBACK_OPTIONS checkpoint_every=%u draw_replay=%d", g_checkpointEvery, g_drawReplay ? 1 : 0);
    g_BgmCommandFilter = FilterBgmCommand;
    for (BgmRecord& r : g_bgmRecords) {
        r.frame = NoFrame;
    }
    g_enabled = true;
    mp::Log("ROLLBACK_START arena_mb=%u", (unsigned)(rollback::kDefaultArenaBytes >> 20));
    return true;
}

bool Enabled()
{
    return g_enabled;
}

void BeginSegment()
{
    rollback::heap::RuntimeScope runtime;
    for (unsigned i = 0; i < MaxPrediction; i++) {
        DropCheckpoint(g_checkpoints[i]);
    }
    g_restoredFrame = NoFrame;
    g_hitAnnounced = NoFrame;
    for (int seat = 0; seat < 4; seat++) {
        g_hitHeard[seat] = seat < mp::PlayerCount() ? g_Players[seat].hitSounds : 0;
    }
    for (BgmRecord& r : g_bgmRecords) {
        r.frame = NoFrame;
    }
}

const Stats& GetStats()
{
    return g_stats;
}

void LogPerf()
{
    unsigned long long tickUs = 0, drawUs = 0;
    unsigned ticks = 0, draws = 0;
    TakeFrameCosts(&tickUs, &ticks, &drawUs, &draws);
    const Perf& p = g_perf;
    mp::Log("RB_PERF frames=%u replayed=%u captures=%u capture_us=%u restore=%u restore_us=%u capture_kb=%u "
            "tick_us=%u draws=%u draw_us=%u",
            p.frames, p.replayed, p.captures, p.captures ? (unsigned)(p.captureUs / p.captures) : 0, p.restores,
            p.restores ? (unsigned)(p.restoreUs / p.restores) : 0,
            p.captures ? (unsigned)(p.captureBytes / p.captures / 1024) : 0, ticks ? (unsigned)(tickUs / ticks) : 0,
            draws, draws ? (unsigned)(drawUs / draws) : 0);
    g_perf = Perf();
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        for (int n = 0; n < 12; n++) {
            TickCost* best = NULL;
            for (TickCost& c : g_tickCosts) {
                if (c.function != NULL && c.calls != 0 && (best == NULL || c.ticks > best->ticks)) {
                    best = &c;
                }
            }
            if (best == NULL) {
                break;
            }
            mp::Log("RB_TICK fn=%p calls=%u us_per_call=%u", best->function, best->calls,
                    (unsigned)(best->ticks * 1000000 / frequency.QuadPart / best->calls));
            best->calls = 0;
        }
        memset(g_tickCosts, 0, sizeof(g_tickCosts));
    }
    if (g_memory != NULL) {
        rollback::Memory::Profile m = g_memory->TakeProfile();
        unsigned c = m.captures ? (unsigned)m.captures : 1;
        unsigned r = m.restores ? (unsigned)m.restores : 1;
        mp::Log("RB_MEM captures=%u query_us=%u share_us=%u dirty_us=%u regions_us=%u written=%u fresh=%u "
                "arena_pages=%u restores=%u restore_query_us=%u restore_copy_us=%u restored_pages=%u",
                (unsigned)m.captures, (unsigned)(m.queryUs / c), (unsigned)(m.shareUs / c), (unsigned)(m.dirtyUs / c),
                (unsigned)(m.regionsUs / c), (unsigned)(m.written / c), (unsigned)(m.fresh / c),
                (unsigned)(m.arenaPages / c), (unsigned)m.restores, (unsigned)(m.restoreQueryUs / r),
                (unsigned)(m.restoreCopyUs / r), (unsigned)(m.restoredPages / r));
    }
}

int RunFrame(netcode::Timeline& timeline, int* present, FrameDone done, void* context)
{
    int status = 1;
    const auto restore = [&](unsigned first) -> unsigned {
        const Checkpoint* found = NULL;
        for (unsigned back = 0; back < MaxPrediction && back <= first; back++) {
            const Checkpoint& c = g_checkpoints[(first - back) % MaxPrediction];
            if (c.frame == first - back) {
                found = &c;
                break;
            }
        }
        if (found == NULL) {
            return NoFrame;
        }
        unsigned frame = found->frame;
        if (Trace()) {
            mp::Log("RB restore frame=%u first=%u next=%u confirmed=%u", frame, first, timeline.Next(),
                    timeline.Confirmed());
        }
        Restore(*found);
        g_restoredFrame = frame;
        g_stats.rollbacks++;
        unsigned depth = timeline.Next() - frame;
        if (depth > g_stats.maxRollback) {
            g_stats.maxRollback = depth;
        }
        return frame;
    };
    const auto step = [&](unsigned frame, const Inputs& inputs, bool replay) -> int {
        if (SessionGameplayExitPending()) {
            g_restoredFrame = NoFrame;
            return -1;
        }
        bool keep = replay && frame == g_restoredFrame;
        g_restoredFrame = NoFrame;
        Checkpoint& c = g_checkpoints[frame % MaxPrediction];
        // This slot may hold this frame from a mispredicted history: capture it or empty it.
        bool capture = false;
        if (!keep && timeline.NeedsCheckpoint(frame)) {
            capture = true;
            for (unsigned back = 1; back < g_checkpointEvery && back <= frame; back++) {
                if (g_checkpoints[(frame - back) % MaxPrediction].frame == frame - back) {
                    capture = false;
                    break;
                }
            }
        }
        if (capture) {
            Capture(c, frame);
        } else if (!keep) {
            if (c.frame != NoFrame) {
                rollback::heap::RuntimeScope runtime;
                DropCheckpoint(c);
            }
            g_stats.skippedCheckpoints++;
        }
        // A frame that may not run on predicted input is a barrier: no restore may cross it, so the
        // checkpoints up to it go.
        const bool barrier = SessionPredictionBlocked() != 0;
        g_SoundSilenced = replay ? 1 : 0;
        g_FrameMayRollBack = timeline.NeedsCheckpoint(frame) ? 1 : 0;
        g_stepFrame = frame;
        g_stepReplay = replay;
        g_stepBgmSeq = 0;
        if (replay) {
            g_stats.replayedFrames++;
        }
        if (Trace()) {
            mp::Log("RB step frame=%u replay=%d in=%04X,%04X", frame, replay ? 1 : 0, inputs.held[0], inputs.held[1]);
        }
        g_perf.frames++;
        g_perf.replayed += replay ? 1 : 0;
        // A frame run again is not drawn: drawing changes nothing the simulation reads.
        g_ChainCalcProfile = replay ? ProfileReplayTick : NULL;
        int r = SessionRunFrame(inputs.held, mp::PlayerCount(), replay && !g_drawReplay ? 0 : 1);
        g_ChainCalcProfile = NULL;
        RecordHits(frame, g_FrameMayRollBack == 0);
        if (barrier) {
            rollback::heap::RuntimeScope runtime;
            for (unsigned i = 0; i < MaxPrediction; i++) {
                if (g_checkpoints[i].frame != NoFrame && g_checkpoints[i].frame <= frame) {
                    DropCheckpoint(g_checkpoints[i]);
                }
            }
            if (Trace()) {
                mp::Log("RB barrier frame=%u replay=%d", frame, replay ? 1 : 0);
            }
        }
        g_stepFrame = NoFrame;
        g_SoundSilenced = 0;
        g_FrameMayRollBack = 0;
        if (r <= 0) {
            status = r;
            return 0;
        }
        done(frame, context);
        return 1;
    };
    const unsigned repairedBefore = g_stats.replayedFrames;
    const auto blocked = [&](unsigned frame) -> bool {
        if (!SessionPredictionBlocked()) return false;
        mp::Log("RB_BOUNDARY frame=%u next=%u confirmed=%u", frame, timeline.Next(), timeline.Confirmed());
        return true;
    };
    if (!timeline.Repair(restore, step, blocked)) {
        mp::Log("FAIL rollback repair: %s", timeline.Error());
        return 0;
    }
    g_restoredFrame = NoFrame;
    if (status <= 0) {
        return status;
    }
    const bool predict = !SessionPredictionBlocked();
    const unsigned before = timeline.Next();
    const unsigned replayedBefore = repairedBefore;
    if (timeline.CanAdvance(predict) && !timeline.Advance(step, predict)) {
        mp::Log("FAIL rollback advance: %s", timeline.Error());
        return 0;
    }
    if (status <= 0) {
        return status;
    }
    (void)replayedBefore;
    AnnounceConfirmedHits(timeline.Confirmed());
    if (timeline.Next() != before) {
        *present = 1;
    }
    // Drop the checkpoints nobody can restore any more.
    {
        rollback::heap::RuntimeScope runtime;
        unsigned anchor = NoFrame;
        for (unsigned i = 0; i < MaxPrediction; i++) {
            const Checkpoint& c = g_checkpoints[i];
            if (c.frame != NoFrame && c.frame <= timeline.Confirmed() && (anchor == NoFrame || c.frame > anchor)) {
                anchor = c.frame;
            }
        }
        for (unsigned i = 0; i < MaxPrediction; i++) {
            Checkpoint& c = g_checkpoints[i];
            if (c.frame != NoFrame &&
                (c.frame >= timeline.Next() || (c.frame < timeline.Confirmed() && c.frame != anchor))) {
                DropCheckpoint(c);
            }
        }
    }
    if (SessionGameplayExitPending() && timeline.Dirty() == NoFrame && timeline.Confirmed() == timeline.Next()) {
        return 2;
    }
    return 1;
}

}
}

void* GameStaticBlock(size_t bytes)
{
    th07::rollback::Memory* arena = th07::rollback_game::SharedArena();
    void* block = arena != NULL ? arena->Allocate(bytes, 64) : NULL;
    if (block == NULL) {
        block = malloc(bytes);
    }
    if (block != NULL) {
        memset(block, 0, bytes);
    }
    return block;
}
