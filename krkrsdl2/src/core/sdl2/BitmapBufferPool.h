/* SPDX-License-Identifier: MIT */
#ifndef KRKR_BITMAP_BUFFER_POOL_H
#define KRKR_BITMAP_BUFFER_POOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>

// Allocate large bitmaps in separate arenas so small, long-lived allocations
// cannot split a freed frame buffer in the general-purpose (newlib) heap.
// Adjacent free pages can be reused together, even for a different image size.
// Live pixels never move. Only completely empty arenas may be returned to malloc.
class KrkrBitmapBufferPool
{
public:
    static constexpr size_t Budget = 128 * 1024 * 1024; // completely idle arenas
    static constexpr size_t MinBlock = 256 * 1024;
    static constexpr size_t MaxBlock = 32 * 1024 * 1024;
    static constexpr size_t Granularity = 256 * 1024;
    static constexpr size_t ArenaSize = 64 * 1024 * 1024;
    static constexpr size_t Pages = ArenaSize / Granularity;
    struct Stats { size_t bytes, count; uint64_t hits, misses; size_t reserved, unused; };

private:
    struct Arena
    {
        unsigned char *data = nullptr;
        std::array<uint64_t, Pages / 64> used{};
        size_t freePages = Pages;
        bool IsUsed(size_t page) const { return (used[page / 64] >> (page % 64)) & 1; }
        void Mark(size_t first, size_t count, bool allocated)
        {
            for (size_t page = first; page < first + count; ++page)
            {
                const uint64_t mask = uint64_t(1) << (page % 64);
                if (allocated) used[page / 64] |= mask;
                else used[page / 64] &= ~mask;
            }
            if (allocated) freePages -= count; else freePages += count;
        }
    };
    std::array<Arena, 64> Arenas{};
    uint64_t Hits = 0, Misses = 0;
    mutable std::mutex Mutex;

    void ClearLocked()
    {
        for (auto &arena : Arenas)
            if (arena.data && arena.freePages == Pages)
            {
                std::free(arena.data);
                arena = {};
            }
    }

public:
    ~KrkrBitmapBufferPool() { Clear(); }
    KrkrBitmapBufferPool() = default;
    KrkrBitmapBufferPool(const KrkrBitmapBufferPool &) = delete;
    KrkrBitmapBufferPool &operator=(const KrkrBitmapBufferPool &) = delete;

    // size is the actual capacity on return; the caller must retain it for Free.
    void *Allocate(size_t &size)
    {
        std::lock_guard<std::mutex> lock(Mutex);
        if (size >= MinBlock && size <= MaxBlock)
        {
            size = (size + Granularity - 1) / Granularity * Granularity;
            const size_t needed = size / Granularity;
            Arena *best = nullptr;
            size_t bestStart = 0, bestLength = Pages + 1;
            for (auto &arena : Arenas)
            {
                if (!arena.data || arena.freePages < needed) continue;
                size_t start = 0, length = 0;
                for (size_t page = 0; page <= Pages; ++page)
                {
                    if (page < Pages && !arena.IsUsed(page))
                    {
                        if (!length) start = page;
                        ++length;
                    }
                    else
                    {
                        if (length >= needed && length < bestLength)
                        { best = &arena; bestStart = start; bestLength = length; }
                        length = 0;
                    }
                }
                if (bestLength == needed) break;
            }
            if (best)
            {
                best->Mark(bestStart, needed, true);
                ++Hits;
                return best->data + bestStart * Granularity;
            }
            // Arena metadata is fixed-size; allocation/free never allocates a
            // bookkeeping node into a just-freed image's address range.
            for (auto &arena : Arenas) if (!arena.data)
            {
                arena.data = static_cast<unsigned char *>(std::malloc(ArenaSize));
                if (arena.data)
                {
                    arena.Mark(0, needed, true);
                    ++Misses;
                    return arena.data;
                }
                break;
            }
        }
        ++Misses;
        // Small/oversized images and an arena-growth failure can still use any
        // available general-heap memory. Free identifies these by address.
        void *result = std::malloc(size);
        if (!result)
        {
            // Do not let idle buffers cause an allocation failure for another
            // size. Script/cache compaction is done by the caller, outside this lock.
            ClearLocked();
            result = std::malloc(size);
        }
        return result;
    }

    void Free(void *data, size_t size)
    {
        if (!data) return;
        std::lock_guard<std::mutex> lock(Mutex);
        const uintptr_t address = reinterpret_cast<uintptr_t>(data);
        for (auto &arena : Arenas)
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(arena.data);
            if (arena.data && address >= base && address - base < ArenaSize)
            {
                arena.Mark((address - base) / Granularity, size / Granularity, false);
                if (arena.freePages == Pages)
                {
                    size_t idle = 0;
                    for (const auto &other : Arenas)
                        if (other.data && other.freePages == Pages) idle += ArenaSize;
                    if (idle > Budget) { std::free(arena.data); arena = {}; }
                }
                return;
            }
        }
        std::free(data);
    }

    void Clear() { std::lock_guard<std::mutex> lock(Mutex); ClearLocked(); }
    Stats GetStats() const
    {
        std::lock_guard<std::mutex> lock(Mutex);
        Stats result{0, 0, Hits, Misses, 0, 0};
        for (const auto &arena : Arenas) if (arena.data)
        {
            result.reserved += ArenaSize;
            result.unused += arena.freePages * Granularity;
            if (arena.freePages == Pages) { result.bytes += ArenaSize; ++result.count; }
        }
        return result;
    }
};
#endif
