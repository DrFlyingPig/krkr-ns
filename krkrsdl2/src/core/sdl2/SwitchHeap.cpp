/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * KRKR-ns: the Switch build had no allocator with size-class separation.
 *
 * Upstream krkrsdl2 maps every bitmap with mmap on Apple/Linux/Android and falls
 * back to malloc elsewhere; that fallback is what this port inherited.  On
 * Switch it does not hold up: newlib's dlmalloc keeps one arena, so a
 * fast-forward session reached a 3.4 GB arena holding 2.6 GB of free space while
 * an 11 MB title background could no longer be allocated, and later font
 * pre-rendering failed with std::bad_alloc.
 *
 * mimalloc and rpmalloc both need virtual-memory reservation, which libnx does
 * not provide (there is no sys/mman.h in the toolchain).  TLSF (Two Level
 * Segregated Fit, BSD-3) needs no OS support: it manages a pool carved from a
 * chunk we hand it, and every allocation lands in a size-segregated free list.
 *
 * Hardware notes:
 *  - Every payload is 16-byte aligned (AArch64 max_align_t), which a plain TLSF
 *    pool does not guarantee because its own granularity is 8 bytes.
 *  - Ownership is decided from the chunk address ranges recorded below.  Reading
 *    a magic word in front of a foreign pointer (for example one produced by
 *    libc before the first wrapped call) can land on unrelated bytes and is not
 *    a safe test on this platform.
 *  - A marker file keeps the wrapper inactive, so a build can compare the TLSF
 *    path with the plain system allocator on hardware without being rebuilt.
 *  - TLSF is not thread safe, so the wrappers serialize with a mutex and grow by
 *    asking the original allocator for another chunk.
 */
#include "tlsf.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

extern "C" void *__real_malloc(size_t size);
extern "C" void __real_free(void *pointer);
extern "C" void *__real_realloc(void *pointer, size_t size);
extern "C" void *__real_calloc(size_t count, size_t size);

namespace
{
constexpr size_t kChunkBytes = 64u * 1024u * 1024u;
constexpr size_t kHeaderBytes = 32; // keeps a 16-byte aligned payload
constexpr size_t kPayloadAlign = 16;
constexpr uint64_t kMagic = 0x4B52544C53463100ull; // "KRTLSF1"
constexpr int kMaxChunks = 96;

struct alignas(16) Header
{
    uint64_t magic;
    size_t size;
};

struct ChunkRange
{
    unsigned char *base;
    size_t size;
};

std::mutex gLock;
tlsf_t gPool = nullptr;
bool gReady = false;
bool gDisabled = false;
bool gMarkerChecked = false;
size_t gReserved = 0;
ChunkRange gChunks[kMaxChunks];
int gChunkCount = 0;

bool UseSystemAllocator()
{
    if (!gMarkerChecked)
    {
        gMarkerChecked = true;
        if (FILE *marker = std::fopen("sdmc:/switch/KRKR-ns/alloc-system.txt", "rb"))
        {
            std::fclose(marker);
            gDisabled = true;
        }
    }
    return gDisabled;
}

bool InChunkLocked(const void *pointer)
{
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    for (int i = 0; i < gChunkCount; ++i)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(gChunks[i].base);
        if (address >= base && address - base < gChunks[i].size) return true;
    }
    return false;
}

void GrowLocked()
{
    void *chunk = __real_malloc(kChunkBytes);
    if (!chunk) return;
    if (!gPool)
    {
        gPool = tlsf_create_with_pool(chunk, kChunkBytes);
        if (!gPool)
        {
            __real_free(chunk);
            return;
        }
        gReady = true;
        gReserved += kChunkBytes;
        gChunks[gChunkCount++] = {static_cast<unsigned char *>(chunk), kChunkBytes};
        return;
    }
    if (gChunkCount >= kMaxChunks || tlsf_add_pool(gPool, chunk, kChunkBytes) == 0)
    {
        __real_free(chunk);
        return;
    }
    gReserved += kChunkBytes;
    gChunks[gChunkCount++] = {static_cast<unsigned char *>(chunk), kChunkBytes};
}

void *AllocateLocked(size_t size)
{
    if (size > SIZE_MAX - kHeaderBytes) return nullptr;
    if (!gReady) GrowLocked();
    if (!gReady) return nullptr;
    void *block = tlsf_memalign(gPool, kPayloadAlign, size + kHeaderBytes);
    if (!block)
    {
        GrowLocked();
        block = tlsf_memalign(gPool, kPayloadAlign, size + kHeaderBytes);
    }
    if (!block) return nullptr;
    Header *header = static_cast<Header *>(block);
    header->magic = kMagic;
    header->size = size;
    return static_cast<unsigned char *>(block) + kHeaderBytes;
}

bool OwnsLocked(void *pointer, Header **out)
{
    if (!gReady || !pointer || !InChunkLocked(pointer)) return false;
    Header *header = reinterpret_cast<Header *>(static_cast<unsigned char *>(pointer) - kHeaderBytes);
    if (header->magic != kMagic) return false;
    *out = header;
    return true;
}
} // namespace

extern "C" void *__wrap_malloc(size_t size)
{
    if (size == 0) size = 1;
    if (UseSystemAllocator()) return __real_malloc(size);
    std::lock_guard<std::mutex> lock(gLock);
    void *result = AllocateLocked(size);
    if (result) return result;
    return __real_malloc(size);
}

extern "C" void __wrap_free(void *pointer)
{
    if (!pointer) return;
    if (!UseSystemAllocator())
    {
        std::lock_guard<std::mutex> lock(gLock);
        Header *header = nullptr;
        if (OwnsLocked(pointer, &header))
        {
            header->magic = 0;
            tlsf_free(gPool, header);
            return;
        }
    }
    __real_free(pointer);
}

extern "C" void *__wrap_calloc(size_t count, size_t size)
{
    if (count == 0 || size == 0) return __wrap_malloc(1);
    if (count > SIZE_MAX / size) return nullptr;
    const size_t total = count * size;
    void *result = __wrap_malloc(total);
    if (result) std::memset(result, 0, total);
    return result;
}

extern "C" void *__wrap_realloc(void *pointer, size_t size)
{
    if (!pointer) return __wrap_malloc(size);
    if (size == 0)
    {
        __wrap_free(pointer);
        return nullptr;
    }
    if (!UseSystemAllocator())
    {
        std::lock_guard<std::mutex> lock(gLock);
        Header *header = nullptr;
        if (OwnsLocked(pointer, &header))
        {
            const size_t previous = header->size;
            if (size <= previous) return pointer; // TLSF blocks keep their capacity
            void *grown = AllocateLocked(size);
            if (!grown) return nullptr;
            std::memcpy(grown, pointer, previous);
            header->magic = 0;
            tlsf_free(gPool, header);
            return grown;
        }
    }
    return __real_realloc(pointer, size);
}

// Diagnostics for the periodic memory line and for hardware comparison.
extern "C" void KrkrNSSwitchHeapStats(size_t *reserved, size_t *chunks, int *active)
{
    if (UseSystemAllocator())
    {
        if (reserved) *reserved = 0;
        if (chunks) *chunks = 0;
        if (active) *active = 0;
        return;
    }
    std::lock_guard<std::mutex> lock(gLock);
    if (reserved) *reserved = gReserved;
    if (chunks) *chunks = static_cast<size_t>(gChunkCount);
    if (active) *active = gReady ? 1 : 0;
}
