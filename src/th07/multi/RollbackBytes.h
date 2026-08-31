#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(_MSC_VER) && defined(_M_IX86)
#include <intrin.h>
#endif

namespace th07 { namespace rollback { namespace detail {

// 64-bit FNV-1a
inline std::uint64_t FoldBytes(std::uint64_t hash, const void* bytes, std::size_t count) {
    const unsigned char* source = static_cast<const unsigned char*>(bytes);
#if defined(_MSC_VER) && defined(_M_IX86)
    // Avoids an __allmul call per byte.
    std::uint32_t low = static_cast<std::uint32_t>(hash);
    std::uint32_t high = static_cast<std::uint32_t>(hash >> 32);
    for (std::size_t i = 0; i < count; ++i) {
        low ^= source[i];
        const unsigned __int64 product = __emulu(low, 435u);
        high = high * 435u + (low << 8) + static_cast<std::uint32_t>(product >> 32);
        low = static_cast<std::uint32_t>(product);
    }
    return (static_cast<std::uint64_t>(high) << 32) | low;
#else
    for (std::size_t i = 0; i < count; ++i)
        hash = (hash ^ source[i]) * 1099511628211ull;
    return hash;
#endif
}

// Copies only the differing 4 KiB chunks, so unchanged pages stay clean (write-watch).
inline std::size_t CopyChangedBytes(void* destination, const void* source, std::size_t bytes) {
    unsigned char* dest = static_cast<unsigned char*>(destination);
    const unsigned char* src = static_cast<const unsigned char*>(source);
    std::size_t copied = 0;
    for (std::size_t offset = 0; offset < bytes; offset += 4096) {
        const std::size_t count = (std::min)(std::size_t(4096), bytes - offset);
        if (std::memcmp(dest + offset, src + offset, count) == 0) continue;
        std::memcpy(dest + offset, src + offset, count);
        copied += count;
    }
    return copied;
}

} } }
