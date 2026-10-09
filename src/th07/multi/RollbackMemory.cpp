#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "RollbackMemory.h"
#include "RollbackBytes.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace th07 { namespace rollback {
namespace {
constexpr std::size_t kPage = 4096;
constexpr std::size_t kDirtyListMin = 1024;
constexpr std::uint32_t kMagic = 0x52424D31;
constexpr std::uint32_t kBlockMagic = 0x52424231;
struct Header {
    std::uint32_t magic, used, freeHead, nextSerial;
    std::uint32_t liveCount, reserved[3];
};
struct Block {
    std::uint32_t magic, size, capacity, payload;
    std::uint32_t freeNext, live, serial, alignment;
};
static_assert(sizeof(Header) == 32 && sizeof(Block) == 32, "arena layout");
std::size_t Align(std::size_t n, std::size_t alignment) {
    return (n + alignment - 1) & ~(alignment - 1);
}
std::uint64_t Fold(std::uint64_t h, const void* bytes, std::size_t count) {
    return detail::FoldBytes(h, bytes, count);
}
bool Overlap(std::uintptr_t a, std::size_t n, std::uintptr_t b, std::size_t m) {
    return a < b + m && b < a + n;
}
struct IdBuffer {
    std::unique_ptr<std::uint32_t[]> data;
    std::size_t capacity = 0;
};
}

struct Memory::Image {
    struct SavedRegion {
        std::uint32_t key;
        std::size_t bytes, first;
        std::size_t Pages() const { return (bytes + kPage - 1) / kPage; }
    };
    // Images keep their owner until their page references are released.
    std::shared_ptr<Owner> owner;
    Image* prior = nullptr;
    Image* next = nullptr;
    std::uint64_t frame = 0;
    mutable std::uint64_t hash = 0;
    std::size_t bytes = 0;
    std::size_t filled = 0;
    IdBuffer ids;
    std::vector<SavedRegion> regions;
    Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    ~Image();
    const std::uint32_t* Ids(const SavedRegion& region) const { return ids.data.get() + region.first; }
    std::uint64_t Hash() const;
};

// Saved pages are 4KB entries in 1MB slabs; a page id is slab << 8 | index. Releases never
// allocate.
struct Memory::Owner {
    static constexpr std::uint32_t kSlabShift = 8, kSlabPages = 1u << kSlabShift, kMaxSlabs = 2048,
        kMaxPages = kMaxSlabs * kSlabPages, kSpareImages = 64, kZero = 0, kNone = 0xFFFFFFFFu,
        kAuditDigests = 256;
    unsigned char* slabs[kMaxSlabs] = {};
    std::size_t slabCount = 0, slabLimit = kMaxSlabs;
    void* meta = nullptr;
    std::uint32_t* refs = nullptr;
    std::uint32_t* generations = nullptr;
    std::uint32_t* freeIds = nullptr;
    std::uint64_t* digests = nullptr;
    std::size_t freeCount = 0, peakLive = 0, imageCount = 0, spareCount = 0, auditCursor = 0;
    Image* images = nullptr;
    IdBuffer spare[kSpareImages];
    DWORD thread = GetCurrentThreadId();
    const char* fault = nullptr;
    bool (*releaseForbidden)() = nullptr;

    Owner() = default;
    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;
    ~Owner() {
        for (std::size_t i = 0; i < slabCount; ++i) VirtualFree(slabs[i], 0, MEM_RELEASE);
        if (meta) VirtualFree(meta, 0, MEM_RELEASE);
    }
    bool Initialize() {
        meta = VirtualAlloc(nullptr, std::size_t(kMaxPages) * 20, MEM_RESERVE, PAGE_READWRITE);
        if (!meta) return false;
        auto* at = static_cast<unsigned char*>(meta);
        refs = reinterpret_cast<std::uint32_t*>(at);
        generations = reinterpret_cast<std::uint32_t*>(at + std::size_t(kMaxPages) * 4);
        freeIds = reinterpret_cast<std::uint32_t*>(at + std::size_t(kMaxPages) * 8);
        digests = reinterpret_cast<std::uint64_t*>(at + std::size_t(kMaxPages) * 12);
        // Id 0 is the shared all-zero page, read-only so a stray write faults.
        DWORD protection;
        return AddSlab() && Allocate() == kZero
            && VirtualProtect(slabs[0], kPage, PAGE_READONLY, &protection);
    }
    bool AddSlab() {
        if (slabCount >= slabLimit) return false;
        const std::size_t pages = kSlabPages, first = slabCount * pages;
        auto* slab = static_cast<unsigned char*>(VirtualAlloc(nullptr, pages * kPage,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!slab) return false;
        if (!VirtualAlloc(refs + first, pages * sizeof(*refs), MEM_COMMIT, PAGE_READWRITE)
            || !VirtualAlloc(generations + first, pages * sizeof(*generations), MEM_COMMIT, PAGE_READWRITE)
            || !VirtualAlloc(freeIds + first, pages * sizeof(*freeIds), MEM_COMMIT, PAGE_READWRITE)
            || !VirtualAlloc(digests + first, pages * sizeof(*digests), MEM_COMMIT, PAGE_READWRITE)) {
            VirtualFree(slab, 0, MEM_RELEASE);
            return false;
        }
        slabs[slabCount++] = slab;
        for (std::size_t i = pages; i--;) freeIds[freeCount++] = static_cast<std::uint32_t>(first + i);
        return true;
    }
    unsigned char* Data(std::uint32_t id) const {
        return slabs[id >> kSlabShift] + (id & (kSlabPages - 1)) * kPage;
    }
    std::uint32_t Allocate() {
        if (!freeCount && !AddSlab()) return kNone;
        const std::uint32_t id = freeIds[--freeCount];
        refs[id] = 1; ++generations[id]; digests[id] = 0;
        const std::size_t live = slabCount * kSlabPages - freeCount;
        if (live > peakLive) peakLive = live;
        return id;
    }
    std::uint32_t Fresh(const unsigned char* source, std::size_t count) {
        if (std::memcmp(source, Data(kZero), count) == 0) { ++refs[kZero]; return kZero; }
        const std::uint32_t id = Allocate();
        if (id == kNone) return kNone;
        auto* page = Data(id);
        std::memcpy(page, source, count);
        if (count != kPage) std::memset(page + count, 0, kPage - count);
        return id;
    }
    void Retain(const std::uint32_t* ids, std::size_t count) {
        auto* const counts = refs;
        for (std::size_t i = 0; i < count; ++i) ++counts[ids[i]];
    }
    void Release(const std::uint32_t* ids, std::size_t count) {
        auto* const counts = refs;
        auto* const stack = freeIds;
        std::size_t top = freeCount;
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t id = ids[i];
            if (!--counts[id]) stack[top++] = id;
        }
        freeCount = top;
    }
    std::uint64_t Digest(std::uint32_t id) {
        auto& value = digests[id];
        if (!value) value = Fold(14695981039346656037ull, Data(id), kPage);
        return value;
    }
    void Check(const char* message) {
#ifndef NDEBUG
        if (GetCurrentThreadId() != thread && !fault) fault = message;
#else
        (void)message;
#endif
    }
    void Link(Image* image) {
        image->next = images;
        if (images) images->prior = image;
        images = image;
        ++imageCount;
    }
    void Unlink(Image* image) {
        (image->prior ? image->prior->next : images) = image->next;
        if (image->next) image->next->prior = image->prior;
        image->prior = image->next = nullptr;
        --imageCount;
    }
    IdBuffer TakeIds(std::size_t count) {
        while (spareCount) {
            IdBuffer buffer = std::move(spare[--spareCount]);
            if (buffer.capacity >= count) return buffer;
        }
        IdBuffer buffer;
        const std::size_t capacity = count + count / 8 + kSlabPages;
        buffer.data.reset(new std::uint32_t[capacity]);
        buffer.capacity = capacity;
        return buffer;
    }
    void Recycle(IdBuffer& buffer) {
        if (buffer.data && spareCount < kSpareImages) spare[spareCount++] = std::move(buffer);
    }
};
// C++14: an odr-used static constexpr member needs a definition outside the class.
constexpr std::uint32_t Memory::Owner::kSlabShift;
constexpr std::uint32_t Memory::Owner::kSlabPages;
constexpr std::uint32_t Memory::Owner::kMaxSlabs;
constexpr std::uint32_t Memory::Owner::kMaxPages;
constexpr std::uint32_t Memory::Owner::kSpareImages;
constexpr std::uint32_t Memory::Owner::kZero;
constexpr std::uint32_t Memory::Owner::kNone;
constexpr std::uint32_t Memory::Owner::kAuditDigests;

Memory::Image::~Image() {
    if (!owner) return;
    auto& store = *owner;
    store.Check("snapshot released off its owner thread");
    if (store.releaseForbidden && store.releaseForbidden() && !store.fault)
        store.fault = "snapshot released inside simulation scope";
    store.Unlink(this);
    store.Release(ids.data.get(), filled);
    store.Recycle(ids);
}
std::uint64_t Memory::Image::Hash() const {
    if (hash) return hash;
    auto& store = *owner;
    store.Check("page digest computed off the owner thread");
    auto value = std::uint64_t(14695981039346656037ull);
    for (const auto& region : regions) {
        value = Fold(value, &region.key, sizeof(region.key));
        const auto length = static_cast<std::uint64_t>(region.bytes);
        value = Fold(value, &length, sizeof(length));
        const auto* page = Ids(region);
        for (std::size_t p = 0, n = region.Pages(); p < n; ++p) {
            const auto digest = store.Digest(page[p]);
            value = Fold(value, &digest, sizeof(digest));
        }
    }
    hash = value;
    return value;
}

Memory::Memory() : base_(nullptr), capacity_(0), committed_(0), sealed_(false),
    forceInspect_(false), wideQuery_(false), dirtyCapacity_(kDirtyListMin), error_(""), writtenCount_(0) {}
Memory::~Memory() { if (base_) VirtualFree(base_, 0, MEM_RELEASE); }
bool Memory::Fail(const char* message) { error_ = message; return false; }

bool Memory::Initialize(std::size_t capacity, void* preferredBase) {
    if (base_) return Fail("already initialized");
    if (capacity < kPage || capacity > 0x7FFF0000u)
        return Fail("invalid arena capacity");
    const std::size_t rounded = Align(capacity, kPage);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    if (system.dwPageSize != kPage) return Fail("unsupported write-watch page size");
    auto* p = static_cast<unsigned char*>(VirtualAlloc(preferredBase, rounded,
        MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE));
    if (!p) return Fail("arena reservation failed");
    if (preferredBase && p != preferredBase) {
        VirtualFree(p, 0, MEM_RELEASE);
        return Fail("requested arena address unavailable");
    }
    if (!VirtualAlloc(p, kPage, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(p, 0, MEM_RELEASE);
        return Fail("arena commit failed");
    }
    try {
        auto owner = std::make_shared<Owner>();
        if (!owner->Initialize()) {
            VirtualFree(p, 0, MEM_RELEASE);
            return Fail("page store reservation failed");
        }
        writtenPages_.resize(rounded / kPage);
        dirtyPages_.resize(rounded / kPage);
        owner_ = std::move(owner);
    }
    catch (const std::bad_alloc&) {
        VirtualFree(p, 0, MEM_RELEASE);
        return Fail("owner allocation failed");
    }
    base_ = p; capacity_ = rounded; committed_ = kPage;
    *reinterpret_cast<Header*>(base_) = {kMagic, sizeof(Header), 0, 1, 0, {0, 0, 0}};
    error_ = "";
    return true;
}

bool Memory::AddRegion(std::uint32_t key, void* address, std::size_t bytes) {
    if (!base_ || sealed_) return Fail("register roots before first capture");
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    if (!key || !at || !bytes || bytes > std::numeric_limits<std::uintptr_t>::max() - at)
        return Fail("invalid fixed region");
    if (Overlap(at, bytes, reinterpret_cast<std::uintptr_t>(base_), capacity_))
        return Fail("fixed region overlaps arena");
    for (const auto& r : regions_) {
        if (r.key == key || Overlap(at, bytes,
                reinterpret_cast<std::uintptr_t>(r.data), r.bytes))
            return Fail("duplicate or overlapping fixed region");
    }
    try {
        regions_.push_back({key, static_cast<unsigned char*>(address), bytes});
        std::sort(regions_.begin(), regions_.end(),
            [](const Region& a, const Region& b) { return a.key < b.key; });
    } catch (const std::bad_alloc&) { return Fail("region allocation failed"); }
    return true;
}

std::size_t Memory::UsedBytes() const {
    return base_ ? reinterpret_cast<const Header*>(base_)->used : 0;
}
bool Memory::Grow(std::size_t logicalBytes) {
    if (logicalBytes > capacity_) return Fail("arena exhausted");
    const std::size_t end = Align(logicalBytes, kPage);
    if (end > committed_) {
        if (!VirtualAlloc(base_ + committed_, end - committed_, MEM_COMMIT, PAGE_READWRITE))
            return Fail("arena commit failed");
        committed_ = end;
    }
    // Pages committed by a discarded future must read as fresh zero pages.
    const std::size_t oldEnd = Align(UsedBytes(), kPage);
    if (end > oldEnd) std::memset(base_ + oldEnd, 0, end - oldEnd);
    return true;
}

void* Memory::Allocate(std::size_t bytes, std::size_t alignment) {
    if (!base_ || !alignment || alignment > kPage || (alignment & (alignment - 1))) {
        Fail("invalid allocation or alignment"); return nullptr;
    }
    bytes = bytes ? bytes : 1;
    if (bytes > capacity_) { Fail("allocation exceeds arena"); return nullptr; }
    auto* h = reinterpret_cast<Header*>(base_);
    if (h->nextSerial == 0) { Fail("allocation serial exhausted"); return nullptr; }
    auto* link = &h->freeHead;
    while (*link) {
        auto* b = reinterpret_cast<Block*>(base_ + *link);
        if (b->capacity >= bytes && !(b->payload & (alignment - 1))) {
            *link = b->freeNext;
            b->freeNext = 0; b->live = 1; b->size = static_cast<std::uint32_t>(bytes);
            b->serial = h->nextSerial++; b->alignment = static_cast<std::uint32_t>(alignment);
            ++h->liveCount;
            std::memset(base_ + b->payload, 0, b->capacity);
            return base_ + b->payload;
        }
        link = &b->freeNext;
    }
    const std::size_t offset = Align(h->used, 16);
    const std::size_t payload = Align(offset + sizeof(Block) + sizeof(std::uint32_t), alignment);
    const std::size_t capacity = Align(bytes, 16);
    const std::size_t end = payload + capacity;
    if (end < payload || end > capacity_ || !Grow(end)) {
        Fail("arena exhausted or commit failed"); return nullptr;
    }
    std::memset(base_ + offset, 0, end - offset);
    auto* b = reinterpret_cast<Block*>(base_ + offset);
    *b = {kBlockMagic, static_cast<std::uint32_t>(bytes), static_cast<std::uint32_t>(capacity),
        static_cast<std::uint32_t>(payload), 0, 1, h->nextSerial++,
        static_cast<std::uint32_t>(alignment)};
    const auto blockOffset = static_cast<std::uint32_t>(offset);
    std::memcpy(base_ + payload - sizeof(blockOffset), &blockOffset, sizeof(blockOffset));
    h->used = static_cast<std::uint32_t>(end);
    ++h->liveCount;
    return base_ + payload;
}

bool Memory::Owns(const void* address) const {
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    const auto begin = reinterpret_cast<std::uintptr_t>(base_);
    return base_ && at >= begin && at - begin < capacity_;
}
void* Memory::FindBlock(const void* address) const {
    if (!Owns(address)) return nullptr;
    const std::size_t payload = reinterpret_cast<std::uintptr_t>(address)
        - reinterpret_cast<std::uintptr_t>(base_);
    if (payload < sizeof(Header) + sizeof(Block) + 4 || payload >= UsedBytes()) return nullptr;
    std::uint32_t offset;
    std::memcpy(&offset, base_ + payload - 4, sizeof(offset));
    if (offset < sizeof(Header) || offset > UsedBytes() - sizeof(Block) || (offset & 15))
        return nullptr;
    auto* b = reinterpret_cast<Block*>(base_ + offset);
    if (b->magic != kBlockMagic || b->payload != payload || !b->live
            || b->capacity > UsedBytes() - payload) return nullptr;
    return b;
}
std::size_t Memory::AllocationSize(const void* address) const {
    const auto* b = static_cast<const Block*>(FindBlock(address));
    return b ? b->size : 0;
}
std::size_t Memory::LiveAllocations() const {
    return base_ ? reinterpret_cast<const Header*>(base_)->liveCount : 0;
}
// Freed blocks are cleared so stale reads match between peers.
namespace {
const int kFreeFillUnset = -2;
volatile long freeFill = kFreeFillUnset;
int FreeFillFromEnvironment() {
    char text[16] = {};
    if (GetEnvironmentVariableA("TH07_MP_TEST_FREE_POISON", text, sizeof(text))
        && std::atoi(text) != 0) {
        char seat[16] = {};
        GetEnvironmentVariableA("TH07_MP_SEAT", seat, sizeof(seat));
        return std::atoi(seat) == 1 ? 0x5A : 0xA5;
    }
    char mode[16] = {};
    const DWORD length = GetEnvironmentVariableA("TH07_MP_MODE", mode, sizeof(mode));
    return length != 0 && length < sizeof(mode) ? 0 : -1;
}
}
int FreeFill() {
    long value = freeFill;
    if (value == kFreeFillUnset) {
        InterlockedCompareExchange(&freeFill, FreeFillFromEnvironment(), kFreeFillUnset);
        value = freeFill;
    }
    return static_cast<int>(value);
}
void SetFreeFill(int value) {
    InterlockedExchange(&freeFill, value < 0 ? -1 : (value & 0xFF));
}
bool Memory::Free(void* address) {
    if (!address) return true;
    auto* b = static_cast<Block*>(FindBlock(address));
    if (!b) return Fail("invalid or duplicate arena free");
    const int fill = FreeFill();
    if (fill >= 0) {
        std::memset(address, fill, b->size);
    }
    auto* h = reinterpret_cast<Header*>(base_);
    b->live = 0; b->freeNext = h->freeHead;
    h->freeHead = static_cast<std::uint32_t>(reinterpret_cast<unsigned char*>(b) - base_);
    --h->liveCount;
    return true;
}
void* Memory::Reallocate(void* address, std::size_t bytes) {
    if (!address) return Allocate(bytes);
    auto* b = static_cast<Block*>(FindBlock(address));
    if (!b) { Fail("invalid arena reallocation"); return nullptr; }
    if (!bytes) { Free(address); return nullptr; }
    void* next = Allocate(bytes, b->alignment);
    if (!next) return nullptr;
    std::memcpy(next, address, std::min<std::size_t>(b->size, bytes));
    Free(address);
    return next;
}

bool Memory::StoreUsable() {
    if (!owner_) return Fail("arena not initialized");
    owner_->Check("snapshot store used off its owner thread");
    return owner_->fault ? Fail(owner_->fault) : true;
}

// complete=false: the list may be truncated, so every page counts as written.
bool Memory::QueryDirtyPages(bool reset, bool& complete) {
    writtenCount_ = 0;
    complete = false;
    std::size_t pages = Align(UsedBytes(), kPage) / kPage;
    if (previous_) pages = std::max(pages, previous_->regions[0].Pages());
    pages = std::min(pages, committed_ / kPage);
    // A truncated reset query is never repeated (which pages it reset is undocumented).
    std::size_t capacity = wideQuery_ ? pages : std::min(pages, dirtyCapacity_);
    ULONG_PTR count = 0;
    for (;;) {
        if (reset) forceInspect_ = true;
        count = capacity;
        ULONG granularity = 0;
        if (GetWriteWatch(reset ? WRITE_WATCH_FLAG_RESET : 0, base_, pages * kPage,
                writtenPages_.data(), &count, &granularity) || granularity != kPage)
            return Fail("cannot query arena write tracking");
        complete = count < capacity || capacity == pages;
        if (complete || reset) break;
        capacity = pages;
    }
    wideQuery_ = !complete;
    if (!complete) return true;
    const std::size_t wanted = std::max(kDirtyListMin, std::size_t(count) * 2);
    dirtyCapacity_ = std::max(wanted, dirtyCapacity_ - dirtyCapacity_ / 8);
    for (ULONG_PTR i = 0; i < count; ++i) {
        const auto offset = static_cast<unsigned char*>(writtenPages_[i]) - base_;
        if (offset < 0 || static_cast<std::size_t>(offset) >= pages * kPage || offset % kPage)
            return Fail("invalid write-watch page address");
    }
    writtenCount_ = count;
    return true;
}

namespace {
std::uint64_t ProfileNow() {
    static LARGE_INTEGER frequency;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<std::uint64_t>(now.QuadPart) * 1000000 / static_cast<std::uint64_t>(frequency.QuadPart);
}
}

bool Memory::Capture(std::uint64_t frame, Snapshot& out, bool inspectEveryPage) {
    if (!base_) return Fail("arena not initialized");
    std::uint64_t t0 = ProfileNow(), t1 = t0, t2 = t0, t3 = t0;
    if (!StoreUsable()) return false;
    bool every = inspectEveryPage || forceInspect_;
    if (inspectEveryPage) {
        forceInspect_ = true;
        if (ResetWriteWatch(base_, committed_)) return Fail("cannot reset arena write tracking");
    } else {
        bool complete = false;
        if (!QueryDirtyPages(true, complete)) return false;
        every = every || !complete;
    }
    t1 = ProfileNow();
    auto& store = *owner_;
    try {
        const std::size_t arenaBytes = Align(UsedBytes(), kPage);
        std::size_t total = arenaBytes / kPage;
        for (const auto& region : regions_) total += (region.bytes + kPage - 1) / kPage;
        auto image = std::make_shared<Image>();
        image->owner = owner_;
        store.Link(image.get());
        image->frame = frame;
        image->regions.reserve(regions_.size() + 1);
        image->ids = store.TakeIds(total);
        auto* ids = image->ids.data.get();
        for (std::size_t r = 0; r <= regions_.size(); ++r) {
            const Region region = r ? regions_[r - 1] : Region{0, base_, arenaBytes};
            const Image::SavedRegion saved{region.key, region.bytes, image->filled};
            image->regions.push_back(saved);
            const std::size_t pages = saved.Pages();
            const std::uint32_t* old = nullptr;
            std::size_t shared = 0;
            if (previous_ && r < previous_->regions.size() && previous_->regions[r].key == region.key) {
                old = previous_->Ids(previous_->regions[r]);
                shared = std::min(pages, previous_->regions[r].Pages());
            }
            auto* page = ids + saved.first;
            std::size_t p = 0;
            if (!r && !every && shared) {
                std::memcpy(page, old, shared * sizeof(*page));
                store.Retain(page, shared);
                image->filled += shared;
                t2 = ProfileNow();
                profile_.written += writtenCount_;
                profile_.arenaPages += shared;
                for (std::size_t i = 0; i < writtenCount_; ++i) {
                    const auto at = static_cast<std::size_t>(static_cast<unsigned char*>(writtenPages_[i]) - base_);
                    const std::size_t index = at / kPage;
                    if (index >= shared || !std::memcmp(store.Data(page[index]), base_ + at, kPage)) continue;
                    const std::uint32_t fresh = store.Fresh(base_ + at, kPage);
                    if (fresh == Owner::kNone) return Fail("snapshot allocation failed");
                    ++profile_.fresh;
                    store.Release(page + index, 1);
                    page[index] = fresh;
                }
                p = shared;
                t3 = ProfileNow();
            }
            for (; p < pages; ++p) {
                const std::size_t at = p * kPage, count = std::min(kPage, region.bytes - at);
                std::uint32_t id;
                if (p < shared && !std::memcmp(store.Data(old[p]), region.data + at, count)) {
                    id = old[p];
                    ++store.refs[id];
                } else if ((id = store.Fresh(region.data + at, count)) == Owner::kNone)
                    return Fail("snapshot allocation failed");
                page[p] = id;
                ++image->filled;
            }
            image->bytes += saved.bytes;
        }
        out.image_ = image; previous_ = std::move(image); sealed_ = true;
        forceInspect_ = false;
        const std::uint64_t t4 = ProfileNow();
        ++profile_.captures;
        profile_.queryUs += t1 - t0;
        if (t3 > t1) {
            profile_.shareUs += t2 - t1;
            profile_.dirtyUs += t3 - t2;
            profile_.regionsUs += t4 - t3;
        } else {
            profile_.regionsUs += t4 - t1;
        }
        return true;
    } catch (const std::bad_alloc&) { return Fail("snapshot allocation failed"); }
}

bool Memory::Restore(const Snapshot& snapshot) {
    const auto& image = snapshot.image_;
    if (!image || image->owner != owner_ || image->regions.size() != regions_.size() + 1)
        return Fail("snapshot belongs to another arena");
    if (!StoreUsable()) return false;
    const std::uint64_t r0 = ProfileNow();
    bool complete = false;
    if (!QueryDirtyPages(false, complete)) return false;
    const std::uint64_t r1 = ProfileNow();
    const bool every = forceInspect_ || !complete;
    forceInspect_ = true;
    const auto markWritten = [this](unsigned char value) {
        for (std::size_t i = 0; i < writtenCount_; ++i)
            dirtyPages_[(static_cast<unsigned char*>(writtenPages_[i]) - base_) / kPage] = value;
    };
    if (!every) markWritten(1);
    const auto& store = *owner_;
    const std::uint32_t* latest = previous_ ? previous_->Ids(previous_->regions[0]) : nullptr;
    const std::size_t latestPages = previous_ ? previous_->regions[0].Pages() : 0;
    for (std::size_t r = 0; r < image->regions.size(); ++r) {
        const auto& saved = image->regions[r];
        const auto* ids = image->Ids(saved);
        unsigned char* dest = r ? regions_[r - 1].data : base_;
        for (std::size_t at = 0, p = 0; at < saved.bytes; at += kPage, ++p) {
            // Identity is safe only for pages not written since the latest capture/restore.
            if (!r && !every && p < latestPages && !dirtyPages_[p] && ids[p] == latest[p]) continue;
            const auto count = std::min(kPage, saved.bytes - at);
            ++profile_.restoredPages;
            if (r) detail::CopyChangedBytes(dest + at, store.Data(ids[p]), count);
            else std::memcpy(dest + at, store.Data(ids[p]), count);
        }
    }
    if (!every) markWritten(0);
    if (ResetWriteWatch(base_, committed_)) return Fail("cannot reset restored arena write tracking");
    forceInspect_ = false;
    previous_ = image;
    ++profile_.restores;
    profile_.restoreQueryUs += r1 - r0;
    profile_.restoreCopyUs += ProfileNow() - r1;
    return true;
}

bool Memory::Compare(const Snapshot& snapshot, Difference* first) const {
    const auto& image = snapshot.image_;
    if (!image || image->owner != owner_ || image->regions.size() != regions_.size() + 1)
        return false;
    const auto& store = *owner_;
    for (std::size_t r = 0; r < image->regions.size(); ++r) {
        const auto& saved = image->regions[r];
        const auto* ids = image->Ids(saved);
        const unsigned char* data = r ? regions_[r - 1].data : base_;
        for (std::size_t at = 0, p = 0; at < saved.bytes; at += kPage, ++p) {
            const std::size_t n = std::min(kPage, saved.bytes - at);
            const auto* page = store.Data(ids[p]);
            if (std::memcmp(data + at, page, n) == 0) continue;
            for (std::size_t i = 0; i < n; ++i) if (data[at + i] != page[i]) {
                if (first) *first = {saved.key, at + i, page[i], data[at + i]};
                return false;
            }
        }
    }
    return true;
}

bool Memory::StoreStatistics(StoreStats& out) const {
    if (!owner_) return false;
    const auto& s = *owner_;
    const std::size_t pages = s.slabCount * Owner::kSlabPages;
    out = {s.slabCount, pages, pages - s.freeCount, s.peakLive, s.imageCount, 0,
        s.refs[Owner::kZero] - 1, s.spareCount};
    return true;
}
bool Memory::AuditStore(StoreStats* out) {
    if (!StoreUsable()) return false;
    auto& s = *owner_;
    const std::size_t pages = s.slabCount * Owner::kSlabPages;
    std::size_t references = 1, images = 0;
    try {
        std::vector<std::uint32_t> seen(pages);
        seen[Owner::kZero] = 1;
        for (const Image* image = s.images; image; image = image->next, ++images) {
            if (image->owner != owner_ || images >= s.imageCount) return Fail("store audit: corrupt snapshot list");
            std::size_t expected = 0;
            for (const auto& region : image->regions) {
                if (region.first != expected) return Fail("store audit: snapshot region layout");
                expected += region.Pages();
            }
            if (image->filled != expected) return Fail("store audit: incomplete live snapshot");
            const auto* ids = image->ids.data.get();
            for (std::size_t i = 0; i < expected; ++i) {
                if (ids[i] >= pages) return Fail("store audit: page id out of range");
                ++seen[ids[i]];
            }
            references += expected;
        }
        if (images != s.imageCount) return Fail("store audit: snapshot count mismatch");
        std::size_t unreferenced = 0;
        for (std::size_t id = 0; id < pages; ++id) {
            if (s.refs[id] != seen[id]) return Fail("store audit: page reference count mismatch");
            if (!s.refs[id]) ++unreferenced;
            else if (!s.generations[id]) return Fail("store audit: referenced page was never allocated");
            seen[id] = 0;
        }
        if (unreferenced != s.freeCount) return Fail("store audit: free page count mismatch");
        for (std::size_t i = 0; i < s.freeCount; ++i) {
            const std::uint32_t id = s.freeIds[i];
            if (id >= pages || s.refs[id] || seen[id]) return Fail("store audit: invalid or duplicate free page");
            seen[id] = 1;
        }
        const auto* zero = s.Data(Owner::kZero);
        for (std::size_t i = 0; i < kPage; ++i) if (zero[i]) return Fail("store audit: shared zero page modified");
        for (std::size_t checked = 0, n = 0; n < pages && checked < Owner::kAuditDigests; ++n) {
            const auto id = static_cast<std::uint32_t>(s.auditCursor++ % pages);
            if (!s.refs[id] || !s.digests[id]) continue;
            ++checked;
            if (Fold(14695981039346656037ull, s.Data(id), kPage) != s.digests[id])
                return Fail("store audit: saved page changed after its digest");
        }
    } catch (const std::bad_alloc&) { return Fail("store audit allocation failed"); }
    if (out) {
        StoreStatistics(*out);
        out->references = references;
    }
    return true;
}
void Memory::LimitStoreSlabs(std::size_t slabs) {
    if (owner_) owner_->slabLimit = std::min<std::size_t>(slabs, Owner::kMaxSlabs);
}
void Memory::SetReleaseGuard(bool (*forbidden)()) {
    if (owner_) owner_->releaseForbidden = forbidden;
}

std::uint64_t Memory::Snapshot::Frame() const { return image_ ? image_->frame : 0; }
bool Memory::LocateOffset(std::size_t offset, AllocationInfo& out) const {
    const std::size_t end = UsedBytes();
    for (std::size_t at = sizeof(Header); at + sizeof(Block) <= end;) {
        const auto& b = *reinterpret_cast<const Block*>(base_ + at);
        if (b.magic != kBlockMagic || b.payload < at + sizeof(Block)
            || b.payload > end || b.capacity > end - b.payload) return false;
        if (offset >= at && offset < b.payload + b.capacity) {
            out = {b.payload, b.size, b.capacity, b.serial, b.live != 0};
            return true;
        }
        at = Align(b.payload + b.capacity, 16);
    }
    return false;
}
void Memory::FindReferences(const void* address, std::vector<Reference>& out, std::size_t limit) const {
    out.clear();
    for (std::size_t r = 0; r <= regions_.size(); ++r) {
        const auto* data = r ? regions_[r - 1].data : base_;
        const auto bytes = r ? regions_[r - 1].bytes : UsedBytes();
        const auto key = r ? regions_[r - 1].key : 0u;
        for (std::size_t at = 0; at + sizeof(address) <= bytes; at += sizeof(address)) {
            const void* value;
            std::memcpy(&value, data + at, sizeof(value));
            if (value == address) {
                out.push_back({key, at});
                if (out.size() >= limit) return;
            }
        }
    }
}
bool Memory::Snapshot::ReadBytes(std::uint32_t key, std::size_t offset, void* out, std::size_t bytes) const {
    if (!image_ || !out) return false;
    const auto& store = *image_->owner;
    for (const auto& region : image_->regions) if (region.key == key) {
        if (offset > region.bytes || bytes > region.bytes - offset) return false;
        const auto* ids = image_->Ids(region);
        auto* dest = static_cast<unsigned char*>(out);
        while (bytes) {
            const std::size_t count = std::min(bytes, kPage - offset % kPage);
            std::memcpy(dest, store.Data(ids[offset / kPage]) + offset % kPage, count);
            offset += count; dest += count; bytes -= count;
        }
        return true;
    }
    return false;
}
std::uint64_t Memory::Snapshot::LocalHash() const { return image_ ? image_->Hash() : 0; }
bool Memory::Snapshot::Compare(const Snapshot& other, Difference* first) const {
    if (!image_ || !other.image_ || image_->owner != other.image_->owner
        || image_->regions.size() != other.image_->regions.size()) return false;
    const auto& store = *image_->owner;
    for (std::size_t r = 0; r < image_->regions.size(); ++r) {
        const auto& expected = image_->regions[r];
        const auto& actual = other.image_->regions[r];
        if (expected.key != actual.key || expected.bytes != actual.bytes) return false;
        const auto* left = image_->Ids(expected);
        const auto* right = other.image_->Ids(actual);
        for (std::size_t p = 0, n = expected.Pages(); p < n; ++p) {
            if (left[p] == right[p]) continue;
            const auto count = std::min(kPage, expected.bytes - p * kPage);
            const auto* a = store.Data(left[p]);
            const auto* b = store.Data(right[p]);
            if (!std::memcmp(a, b, count)) continue;
            for (std::size_t i = 0; i < count; ++i) if (a[i] != b[i]) {
                if (first) *first = {expected.key, p * kPage + i, a[i], b[i]};
                return false;
            }
        }
    }
    return true;
}
bool Memory::Snapshot::HashBytes(std::uint32_t key, std::size_t offset, std::size_t bytes, std::uint64_t& out) const {
    if (!image_) return false;
    auto& store = *image_->owner;
    for (const auto& region : image_->regions) if (region.key == key) {
        if (offset > region.bytes || bytes > region.bytes - offset) return false;
        store.Check("page digest computed off the owner thread");
        const auto* ids = image_->Ids(region);
        const std::uint64_t bounds[] = {offset, bytes};
        auto hash = Fold(14695981039346656037ull, bounds, sizeof(bounds));
        while (bytes) {
            const std::uint32_t id = ids[offset / kPage];
            const auto count = std::min(bytes, kPage - offset % kPage);
            if (count == kPage) {
                const auto digest = store.Digest(id);
                hash = Fold(hash, &digest, sizeof(digest));
            }
            else hash = Fold(hash, store.Data(id) + offset % kPage, count);
            offset += count; bytes -= count;
        }
        out = hash;
        return true;
    }
    return false;
}
std::size_t Memory::Snapshot::Bytes() const { return image_ ? image_->bytes : 0; }
std::size_t Memory::Snapshot::SharedPagesWith(const Snapshot& other) const {
    if (!image_ || !other.image_ || image_->owner != other.image_->owner) return 0;
    std::size_t count = 0;
    for (std::size_t r = 0; r < image_->regions.size() && r < other.image_->regions.size(); ++r) {
        const auto& a = image_->regions[r]; const auto& b = other.image_->regions[r];
        if (a.key != b.key) continue;
        const auto* left = image_->Ids(a);
        const auto* right = other.image_->Ids(b);
        for (std::size_t p = 0, n = std::min(a.Pages(), b.Pages()); p < n; ++p)
            count += left[p] == right[p];
    }
    return count;
}
std::size_t Memory::Snapshot::RegionCount() const { return image_ ? image_->regions.size() : 0; }
bool Memory::Snapshot::RegionAt(std::size_t index, std::uint32_t& key, std::size_t& bytes) const {
    if (!image_ || index >= image_->regions.size()) return false;
    key = image_->regions[index].key;
    bytes = image_->regions[index].bytes;
    return true;
}
bool Memory::Snapshot::PageIdentity(std::size_t index, std::size_t page, std::uint64_t& identity) const {
    if (!image_ || index >= image_->regions.size() || page >= image_->regions[index].Pages()) return false;
    const std::uint32_t id = image_->Ids(image_->regions[index])[page];
    identity = (std::uint64_t(image_->owner->generations[id]) << 32) | id;
    return true;
}

} }
