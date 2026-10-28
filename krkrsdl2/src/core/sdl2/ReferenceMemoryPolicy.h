/* SPDX-License-Identifier: MIT */
#pragma once

#include <cstdint>
#include <cstring>

namespace krkrns {
struct ReferenceMemoryPolicy {
    std::uint64_t assumedBytes;
    std::uint64_t graphicCacheBytes;
    int hashBitsCeiling;
    bool lowMemory;
};

// Kirikiroid2 obtains MemTotal in KiB, reserves memory for the system, then
// applies the selected game's memusage preference before sizing caches.
inline ReferenceMemoryPolicy MakeReferenceMemoryPolicy(std::uint64_t totalBytes,
                                                        const char* usage) noexcept {
    constexpr std::uint64_t mib = 1024ULL * 1024;
    totalBytes = (totalBytes / 1024) * 1024;
    std::uint64_t assumed = totalBytes > 768 * mib ? totalBytes - 512 * mib : totalBytes / 2;
    if (assumed <= 256 * mib) {
        assumed = 0;
    } else if (usage && std::strcmp(usage, "low") == 0) {
        assumed = 0;
    } else if (usage && std::strcmp(usage, "medium") == 0) {
        assumed = 128 * mib;
    } else if (usage && std::strcmp(usage, "high") == 0) {
        assumed = 256 * mib;
    }
    if (assumed < 128 * mib) assumed = 0;

    std::uint64_t cacheMb = 0;
    if (assumed > 512 * mib) cacheMb = assumed / (10 * mib);
    else if (assumed > 256 * mib) cacheMb = 40;
    else if (assumed > 192 * mib) cacheMb = 20;
    else if (assumed > 128 * mib) cacheMb = 12;
    else if (assumed > 96 * mib) cacheMb = 8;
    else if (assumed > 64 * mib) cacheMb = 4;
    if (cacheMb > 512) cacheMb = 512;
    const bool low = assumed < 128 * mib;
    return {assumed, cacheMb * mib, low ? 0 : (assumed < 256 * mib ? 4 : 32), low};
}
}
