#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace th07 { namespace rollback {

// Reserved early, before big allocations fragment the 2 GiB address space.
const std::size_t kDefaultArenaBytes = std::size_t(512) << 20;

// -1 leaves freed bytes as they are, 0..255 fills them.
int FreeFill();
void SetFreeFill(int value);

// Call only at a quiescent frame boundary, on the thread that called Initialize.
class Memory {
    struct Owner;
    struct Image;
    struct Region { std::uint32_t key; unsigned char* data; std::size_t bytes; };
public:
    struct Difference;
    class Snapshot {
        friend class Memory;
        std::shared_ptr<const Image> image_;
    public:
        bool Valid() const { return bool(image_); }
        std::uint64_t Frame() const;
        // Address-sensitive local digest, not a cross-process hash.
        std::uint64_t LocalHash() const;
        std::size_t Bytes() const;
        std::size_t SharedPagesWith(const Snapshot& other) const;
        bool Compare(const Snapshot& other, Difference* first = nullptr) const;
        bool ReadBytes(std::uint32_t region, std::size_t offset, void* out, std::size_t bytes) const;
        bool HashBytes(std::uint32_t region, std::size_t offset, std::size_t bytes, std::uint64_t& out) const;
        std::size_t RegionCount() const;
        bool RegionAt(std::size_t index, std::uint32_t& key, std::size_t& bytes) const;
        bool PageIdentity(std::size_t index, std::size_t page, std::uint64_t& identity) const;
    };

    struct Difference {
        std::uint32_t region;
        std::size_t offset;
        unsigned char expected, actual;
    };
    struct AllocationInfo {
        std::size_t payload, size, capacity;
        std::uint32_t serial;
        bool live;
    };
    bool LocateOffset(std::size_t arenaOffset, AllocationInfo& out) const;
    struct Reference { std::uint32_t region; std::size_t offset; };
    void FindReferences(const void* address, std::vector<Reference>& out, std::size_t limit = 24) const;
    struct StoreStats {
        std::size_t slabs, pages, livePages, peakLivePages, images, references, zeroReferences, spareBuffers;
    };

    Memory();
    ~Memory();
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;

    bool Initialize(std::size_t capacity, void* preferredBase = nullptr);
    // Register before the first capture. Regions must not overlap.
    bool AddRegion(std::uint32_t key, void* address, std::size_t bytes);
    void* Allocate(std::size_t bytes, std::size_t alignment = 16);
    bool Free(void* address);
    void* Reallocate(void* address, std::size_t bytes);
    bool Owns(const void* address) const;
    std::size_t AllocationSize(const void* address) const;
    std::size_t LiveAllocations() const;

    bool Capture(std::uint64_t frame, Snapshot& out, bool inspectEveryPage = false);
    bool Restore(const Snapshot& snapshot);
    bool Compare(const Snapshot& snapshot, Difference* first = nullptr) const;
    const char* LastError() const { return error_; }
    void* Base() const { return base_; }
    std::size_t Capacity() const { return capacity_; }
    std::size_t UsedBytes() const;

    struct Profile {
        std::uint64_t captures, queryUs, shareUs, dirtyUs, regionsUs, written, fresh, arenaPages;
        std::uint64_t restores, restoreQueryUs, restoreCopyUs, restoredPages;
    };
    Profile TakeProfile() { Profile p = profile_; profile_ = Profile(); return p; }

    bool StoreStatistics(StoreStats& out) const;
    // Allocates: call outside simulation scopes.
    bool AuditStore(StoreStats* out = nullptr);
    // Test hook.
    void LimitStoreSlabs(std::size_t slabs);
    void SetReleaseGuard(bool (*forbidden)());

private:
    bool Fail(const char* message);
    bool StoreUsable();
    bool QueryDirtyPages(bool reset, bool& complete);
    bool Grow(std::size_t logicalBytes);
    void* FindBlock(const void* address) const;
    unsigned char* base_;
    std::size_t capacity_, committed_;
    bool sealed_;
    bool forceInspect_;
    bool wideQuery_;
    const char* error_;
    Profile profile_ = {};
    std::shared_ptr<Owner> owner_;
    std::vector<Region> regions_;
    std::shared_ptr<const Image> previous_;
    std::vector<void*> writtenPages_;
    std::size_t writtenCount_;
    std::vector<unsigned char> dirtyPages_;
};

} }
