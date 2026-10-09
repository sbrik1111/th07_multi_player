// Independent full-byte witnesses for the write-watch snapshot backend.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "RollbackMemory.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using th07::rollback::Memory;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

struct Witness {
    Memory::Snapshot snapshot;
    std::vector<unsigned char> arena, root;
};

static Witness Save(Memory& memory, unsigned frame, const std::vector<unsigned char>& root) {
    Witness w;
    const auto* base = static_cast<const unsigned char*>(memory.Base());
    w.arena.assign(base, base + memory.UsedBytes());
    w.root = root;
    CHECK(memory.Capture(frame, w.snapshot));
    CHECK(memory.Compare(w.snapshot));
    return w;
}

static void Restore(Memory& memory, const Witness& w, const std::vector<unsigned char>& root) {
    CHECK(memory.Restore(w.snapshot));
    CHECK(memory.UsedBytes() == w.arena.size());
    CHECK(std::memcmp(memory.Base(), w.arena.data(), w.arena.size()) == 0);
    CHECK(root == w.root);
    CHECK(memory.Compare(w.snapshot));
    CHECK(memory.AuditStore());
}

static void Tracking() {
    Memory memory;
    CHECK(memory.Initialize(64u << 20));
    std::vector<unsigned char> root(8195, 7); // an unaligned, partial last fixed page
    CHECK(memory.AddRegion(1, root.data(), root.size()));
    auto* data = static_cast<unsigned char*>(memory.Allocate(40u << 20, 4096));
    CHECK(data);
    // Nonzero pages exercise real copies and retained references as well as zero sharing.
    for (unsigned p = 0; p < 10240; ++p) data[p * 4096] = 1;
    std::vector<Witness> history;
    history.push_back(Save(memory, 0, root));
    const unsigned counts[] = {0, 1, 32, 1024, 4096, 10000, 1, 200, 800, 0};
    for (unsigned f = 1; f <= 80; ++f) {
        if (f % 3 == 0) {
            const auto& w = history[(f * 7) % history.size()];
            Restore(memory, w, root);
            // A second restore, with no intervening capture, must undo fresh writes.
            data[(f * 173) % (40u << 20)] ^= 0x5a;
            root.back() ^= 3;
            Restore(memory, w, root);
            // Restoring another older/newer image also has no intervening capture.
            Restore(memory, history[(f * 11) % history.size()], root);
        }
        for (unsigned p = 0; p < counts[f % 10]; ++p)
            data[((p * 7919u + f) % 10240) * 4096 + f % 4096] ^= (unsigned char)f;
        root[f % root.size()] ^= (unsigned char)f;
        auto next = Save(memory, f, root);
        // Inspect every byte independently, ignoring the OS dirty-page optimization.
        Memory::Snapshot full;
        CHECK(memory.Capture(f, full, true));
        CHECK(next.snapshot.Compare(full));
        CHECK(next.snapshot.LocalHash() == full.LocalHash());
        if (history.size() == 6) history.erase(history.begin());
        history.push_back(std::move(next));
    }
    // Rewind growth, then reuse the discarded future. Allocator state and bytes agree.
    const auto old = Save(memory, 81, root);
    auto* future = static_cast<unsigned char*>(memory.Allocate(2u << 20));
    CHECK(future);
    std::memset(future, 0xa5, 2u << 20);
    const auto grown = Save(memory, 82, root);
    Restore(memory, old, root);
    auto* replacement = static_cast<unsigned char*>(memory.Allocate(2u << 20));
    CHECK(replacement == future);
    for (unsigned i = 0; i < (2u << 20); ++i) CHECK(replacement[i] == 0);
    Restore(memory, grown, root);
    CHECK(memory.Free(future));
    Restore(memory, grown, root);
    CHECK(memory.AllocationSize(future) == (2u << 20));
    // Kernel writes are tracked too (not only compiler-generated stores).
    HANDLE read, write;
    CHECK(CreatePipe(&read, &write, nullptr, 0));
    DWORD count;
    const char bytes[] = "kernel write";
    CHECK(WriteFile(write, bytes, sizeof(bytes), &count, nullptr));
    CHECK(ReadFile(read, data + 9000, sizeof(bytes), &count, nullptr));
    CHECK(count == sizeof(bytes));
    CloseHandle(read); CloseHandle(write);
    const auto kernel = Save(memory, 83, root);
    Restore(memory, old, root);
    Restore(memory, kernel, root);
}

static void FailedCapture() {
    Memory memory;
    CHECK(memory.Initialize(8u << 20));
    std::vector<unsigned char> root(17);
    CHECK(memory.AddRegion(1, root.data(), root.size()));
    auto* data = static_cast<unsigned char*>(memory.Allocate(4u << 20, 4096));
    CHECK(data);
    const auto zero = Save(memory, 0, root);
    memory.LimitStoreSlabs(1);
    std::memset(data, 7, 4u << 20);
    Memory::Snapshot failed;
    CHECK(!memory.Capture(1, failed));
    CHECK(!failed.Valid());
    CHECK(memory.AuditStore());
    // A reset query was consumed by the failed capture. Both recovery routes work.
    memory.LimitStoreSlabs(2048);
    const auto recovered = Save(memory, 2, root);
    Restore(memory, zero, root);
    Restore(memory, recovered, root);
    memory.LimitStoreSlabs(1);
    std::memset(data, 9, 4u << 20);
    CHECK(!memory.Capture(3, failed));
    Restore(memory, zero, root);
    memory.LimitStoreSlabs(2048);
    const auto restored = Save(memory, 4, root);
    CHECK(restored.snapshot.Compare(zero.snapshot));
}

static unsigned long long Now() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

static void Benchmark(unsigned dirtyPages) {
    Memory memory;
    CHECK(memory.Initialize(64u << 20));
    auto* data = static_cast<unsigned char*>(memory.Allocate(40u << 20, 4096));
    CHECK(data);
    std::memset(data, 1, 40u << 20);
    Memory::Snapshot ring[8];
    CHECK(memory.Capture(0, ring[0]));
    memory.TakeProfile();
    const auto begin = Now();
    for (unsigned f = 1; f <= 1200; ++f) {
        for (unsigned p = 0; p < dirtyPages; ++p)
            data[((p * 7919u + f) % 10240) * 4096 + f % 4096] ^= (unsigned char)f;
        CHECK(memory.Capture(f, ring[f % 8]));
        if (f > 8 && f % 3 == 0) {
            data[((f * 73) % 10240) * 4096] ^= 1;
            CHECK(memory.Restore(ring[(f - 2) % 8]));
        }
    }
    const auto elapsed = Now() - begin;
    const auto p = memory.TakeProfile();
    LARGE_INTEGER hz;
    QueryPerformanceFrequency(&hz);
    std::printf("BENCH dirty=%u iterations=1200 total_us=%llu capture_us=%llu restore_us=%llu query_us=%llu\n",
        dirtyPages, elapsed * 1000000 / hz.QuadPart,
        (p.queryUs + p.shareUs + p.dirtyUs + p.regionsUs) / p.captures,
        (p.restoreQueryUs + p.restoreCopyUs) / p.restores, p.queryUs / p.captures);
    CHECK(memory.AuditStore());
}

int main() {
    Tracking();
    FailedCapture();
    std::printf("PASS whole-byte witnesses checks=%u\n", checks);
    Benchmark(128);
    Benchmark(512);
    return 0;
}
