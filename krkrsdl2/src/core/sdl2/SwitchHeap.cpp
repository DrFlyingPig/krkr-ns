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
#include "KrkrNSLog.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>
#include <vector>
#include <new>
#include <sys/reent.h>

extern "C" struct _reent *__getreent(void);

extern "C" void *__real_malloc(size_t size);
extern "C" void __real_free(void *pointer);
extern "C" void *__real_realloc(void *pointer, size_t size);
extern "C" void *__real_calloc(size_t count, size_t size);

namespace
{
constexpr size_t kChunkBytes = 64u * 1024u * 1024u;
constexpr size_t kHeaderBytes = 32; // keeps a 16-byte aligned payload
constexpr size_t kPayloadAlign = 16;
constexpr size_t kTrailerBytes = 16; // overflow canary after the payload
constexpr uint64_t kMagic = 0x4B52544C53463100ull; // "KRTLSF1"
constexpr uint64_t kTrailerCookie = 0xA5C0DEC0FFEE1234ull;
constexpr int kMaxChunks = 96;

struct alignas(16) Header
{
    uint64_t magic;
    size_t size;
    void *allocCaller;  // return address of the allocating caller
    uint32_t allocSeq;  // allocation order, for "allocated long before" checks
    uint32_t allocThread;
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
uint32_t gAllocSequence = 0;

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

std::atomic<bool> gHeapAudit{false};
// Blocks allocated before the audit came up carry no trailer and are absent from
// the live set; their verdicts would be false positives.
std::atomic<uint32_t> gAuditStartSeq{0};

// The marker is read once, from engine startup (KrkrNSSwitchHeapInit below),
// never from inside the allocator: fopen() allocates, and doing that while gLock
// is held deadlocked the first release build.
extern "C" void KrkrNSSwitchHeapInit()
{
    if (FILE *marker = std::fopen("sdmc:/switch/KRKR-ns/heap-audit.txt", "rb"))
    {
        std::fclose(marker);
        gAuditStartSeq.store(gAllocSequence, std::memory_order_relaxed);
        gHeapAudit.store(true, std::memory_order_relaxed);
    }
}

bool HeapAuditEnabled()
{
    return gHeapAudit.load(std::memory_order_relaxed);
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
{    void *chunk = __real_malloc(kChunkBytes);
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

// Implemented with the LiveSet below; the allocator records every live payload
// so a later corrupt free can be classified as overwritten-live or already freed.
void LiveInsert(void *pointer);
bool LiveErase(void *pointer);
bool LiveContains(void *pointer);

void *AllocateLocked(size_t size, void *caller = nullptr)
{
    if (size > SIZE_MAX - kHeaderBytes) return nullptr;
    if (!gReady) GrowLocked();
    if (!gReady) return nullptr;
    void *block = tlsf_memalign(gPool, kPayloadAlign, size + kHeaderBytes + kTrailerBytes);
    if (!block)
    {
        GrowLocked();
        block = tlsf_memalign(gPool, kPayloadAlign, size + kHeaderBytes + kTrailerBytes);
    }
    if (!block) return nullptr;
    Header *header = static_cast<Header *>(block);
    header->magic = kMagic;
    header->size = size;
    header->allocCaller = caller ? caller : __builtin_return_address(0);
    header->allocSeq = ++gAllocSequence;
    header->allocThread = 0;
    void *payload = static_cast<unsigned char *>(block) + kHeaderBytes;
    if (HeapAuditEnabled())
    {
        // Canary right after the payload: a write past the end of this block
        // flips it, which is the signature of header corruption on neighbours.
        uint64_t *trailer = reinterpret_cast<uint64_t *>(static_cast<unsigned char *>(payload) + size);
        trailer[0] = kTrailerCookie;
        trailer[1] = kTrailerCookie ^ header->allocSeq;
        LiveInsert(payload);
    }
    return payload;
}

bool OwnsLocked(void *pointer, Header **out)
{
    if (!gReady || !pointer || !InChunkLocked(pointer)) return false;
    Header *header = reinterpret_cast<Header *>(static_cast<unsigned char *>(pointer) - kHeaderBytes);
    if (header->magic != kMagic) return false;
    *out = header;
    return true;
}

// Recently freed blocks.  tlsf rewrites a block's header when it frees it, so a
// lost magic alone cannot tell a double free from an overwritten header; this
// ring supplies the missing evidence.  Guarded by gLock like the allocator.
struct FreeRecord
{
    void *pointer = nullptr;
    void *caller = nullptr;
    size_t size = 0;
    unsigned seq = 0;
};
// Large enough that the first free of a block freed minutes earlier is still
// recorded: the 2560-byte TJS objects were double freed long after their first
// free, which a 512-entry ring missed.
constexpr unsigned kRecentFreeSlots = 65536;
FreeRecord gRecentFrees[kRecentFreeSlots];
unsigned gFreeSequence = 0;

void RememberFreed(void *pointer, size_t size, void *caller)
{
    const unsigned seq = ++gFreeSequence;
    gRecentFrees[seq % kRecentFreeSlots] = FreeRecord{pointer, caller, size, seq};
}
const FreeRecord *FindRecentFree(const void *pointer)
{
    for (unsigned i = 0; i < kRecentFreeSlots; ++i)
        if (gRecentFrees[i].seq && gRecentFrees[i].pointer == pointer) return &gRecentFrees[i];
    return nullptr;
}

// Exact live/dead classification for the corrupt-free path.  The recent-free
// ring misses old double frees, and tlsf rewrites a block header when it frees
// it, so this set is the authoritative answer: a pointer still in it was handed
// out by us and never freed (its header must have been overwritten); a pointer
// missing from it was already freed (double free) or was never ours.  Every
// access happens under gLock.
constexpr size_t kLiveSlots = 1u << 20; // 1M pointers, 8 MiB of .bss; the set must
                                       // cover every live allocation or a corrupt
                                       // free would be misread as a double free
void *gLiveSlots[kLiveSlots];
// Allocation-free live-pointer table.  This code runs inside __wrap_malloc and
// __wrap_free, so it must never allocate: a std::vector here recursed into the
// allocator it instruments and hung the first build of this probe.
struct LiveSet
{
    static constexpr size_t kSlots = kLiveSlots;
    size_t count = 0;
    bool tracking = true;

    static size_t Hash(const void *pointer)
    {
        uintptr_t value = reinterpret_cast<uintptr_t>(pointer) >> 4;
        value ^= value >> 17;
        value *= 0x9E3779B97F4A7C15ull;
        value ^= value >> 29;
        return static_cast<size_t>(value);
    }
    void Insert(void *pointer)
    {
        if (!pointer || !tracking) return;
        if ((count + 1) * 10 >= kSlots * 7)
        {
            tracking = false;
            KRKRNS_LOG("[heap] live set full at %u entries; corrupt-free verdicts become unreliable",
                (unsigned)count);
            return;
        }
        const size_t mask = kSlots - 1;
        size_t index = Hash(pointer) & mask;
        while (gLiveSlots[index] && gLiveSlots[index] != pointer) index = (index + 1) & mask;
        if (!gLiveSlots[index]) { gLiveSlots[index] = pointer; ++count; }
    }
    bool Erase(void *pointer)
    {
        if (!pointer || !tracking) return false;
        const size_t mask = kSlots - 1;
        size_t index = Hash(pointer) & mask;
        while (gLiveSlots[index])
        {
            if (gLiveSlots[index] == pointer)
            {
                gLiveSlots[index] = nullptr;
                --count;
                for (size_t next = (index + 1) & mask; gLiveSlots[next]; next = (next + 1) & mask)
                {
                    void *moved = gLiveSlots[next];
                    gLiveSlots[next] = nullptr;
                    --count;
                    Insert(moved);
                }
                return true;
            }
            index = (index + 1) & mask;
        }
        return false;
    }
    bool Contains(const void *pointer) const
    {
        if (!pointer || !tracking) return false;
        const size_t mask = kSlots - 1;
        size_t index = Hash(pointer) & mask;
        while (gLiveSlots[index])
        {
            if (gLiveSlots[index] == pointer) return true;
            index = (index + 1) & mask;
        }
        return false;
    }
};
// The table itself is a plain array so it lands in .bss: a zero-filled 4 MiB
// initializer inside the object would be stored in .data and grow the NRO.
LiveSet gLiveSet;

void LiveInsert(void *pointer) { gLiveSet.Insert(pointer); }
bool LiveErase(void *pointer) { return gLiveSet.Erase(pointer); }
bool LiveContains(void *pointer) { return gLiveSet.Contains(pointer); }
} // namespace

extern "C" void *__wrap_malloc(size_t size)
{
    if (size == 0) size = 1;
    if (UseSystemAllocator()) return __real_malloc(size);
    std::lock_guard<std::mutex> lock(gLock);
    void *result = AllocateLocked(size, __builtin_return_address(0));
    if (result) return result;
    // Falling back mixes allocators: the pointer then has to be freed through
    // libc, which needs a valid per-thread reent.  Record it so exhaustion and
    // the resulting foreign pointers are visible in the log.
    static unsigned fallbacks = 0;
    if (fallbacks < 16)
    {
        ++fallbacks;
        KRKRNS_LOG("[heap] TLSF exhaustion, falling back to libc malloc size=%u", (unsigned)size);
    }
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
            const size_t freedSize = header->size;
            void *freeCaller = __builtin_return_address(0);
            const bool audit = HeapAuditEnabled() &&
                header->allocSeq >= gAuditStartSeq.load(std::memory_order_relaxed);
            if (audit)
            {
                // Verify the canary first: a write past the end of THIS block is
                // the usual way a following block's header gets destroyed.
                const uint64_t *trailer = reinterpret_cast<const uint64_t *>(
                    static_cast<unsigned char *>(pointer) + freedSize);
                const uint64_t expectedSecond = kTrailerCookie ^ header->allocSeq;
                if (trailer[0] != kTrailerCookie || trailer[1] != expectedSecond)
                {
                    static unsigned reportedOverflow = 0;
                    if (reportedOverflow < 32)
                    {
                        ++reportedOverflow;
                        KRKRNS_LOG("[heap] payload overflow ptr=%p size=%u alloc_caller=%p "
                            "free_caller=%p trailer=%llx,%llx expected=%llx,%llx",
                            pointer, (unsigned)freedSize, header->allocCaller, freeCaller,
                            (unsigned long long)trailer[0], (unsigned long long)trailer[1],
                            (unsigned long long)kTrailerCookie, (unsigned long long)expectedSecond);
                    }
                }
                LiveErase(pointer);
            }
            header->magic = 0;
            tlsf_free(gPool, header);
            if (audit) RememberFreed(pointer, freedSize, freeCaller);
            return;
        }
        if (InChunkLocked(pointer))
        {
            // The pointer lies inside our own chunks but no longer carries the
            // magic: the block was already freed (tlsf rewrites the header on
            // free) or live code overwrote it.  Handing it to libc free corrupts
            // newlib's allocator, so leak it.  The full verdict needs the audit
            // tables; a release build just reports that it happened.
            void *caller = __builtin_return_address(0);
            if (HeapAuditEnabled() && FindRecentFree(pointer) == nullptr &&
                LiveContains(pointer))
            {
                const Header *raw = reinterpret_cast<const Header *>(
                    static_cast<const unsigned char *>(pointer) - kHeaderBytes);
                const uint64_t magic = raw->magic;
                const size_t sizeField = raw->size;
                const void *rawAllocCaller = raw->allocCaller;
                const uint32_t rawAllocSeq = raw->allocSeq;
                if (rawAllocSeq < gAuditStartSeq.load(std::memory_order_relaxed))
                {
                    // Allocated before the audit came up: no trailer, not in the
                    // live set, so neither verdict would be meaningful.
                    KRKRNS_LOG("[heap] corrupt free ptr=%p (block predates the heap audit)", pointer);
                    return;
                }
                const FreeRecord *previous = FindRecentFree(pointer);
                const bool stillLive = LiveContains(pointer);
                const unsigned alignment = static_cast<unsigned>(
                    reinterpret_cast<uintptr_t>(pointer) & 0xfu);
                static unsigned reportedDouble = 0, reportedCorrupt = 0;
                if (previous || !stillLive)
                {
                    if (reportedDouble < 32)
                    {
                        ++reportedDouble;
                        KRKRNS_LOG("[heap] corrupt free ptr=%p verdict=double-free align=%u size=%u "
                            "first_free_seq=%u first_free_caller=%p free_caller=%p "
                            "(now magic=0x%llx size=%u alloc_caller=%p alloc_seq=%u)", pointer,
                            alignment, (unsigned)(previous ? previous->size : sizeField),
                            previous ? previous->seq : 0u, previous ? previous->caller : nullptr,
                            caller, (unsigned long long)magic, (unsigned)sizeField,
                            rawAllocCaller, rawAllocSeq);
                        KRKRNS_LOG("[heap] corrupt context stage=%s", krkrsdl2_get_stage());
                    }
                }
                else if (reportedCorrupt < 32)
                {
                    ++reportedCorrupt;
                    KRKRNS_LOG("[heap] corrupt free ptr=%p verdict=header-overwritten align=%u "
                        "size_field=%u magic=0x%llx alloc_caller=%p alloc_seq=%u free_caller=%p",
                        pointer, alignment, (unsigned)sizeField, (unsigned long long)magic,
                        rawAllocCaller, rawAllocSeq, caller);
                    KRKRNS_LOG("[heap] corrupt context stage=%s", krkrsdl2_get_stage());
                }
            }
            else
            {
                static unsigned reported = 0;
                if (reported < 8)
                {
                    ++reported;
                    KRKRNS_LOG("[heap] corrupt free ptr=%p free_caller=%p (leaked; "
                        "drop sdmc:/switch/KRKR-ns/heap-audit.txt + rebuild for the verdict)",
                        pointer, caller);
                }
            }
            return;
        }
    }
    // Foreign pointer (libc internal allocation, or the alloc-system.txt A/B).
    // newlib's free takes the allocator lock; a thread without a usable libc
    // state dereferences null there.  Skip the free rather than die.
    if (__getreent() == nullptr)
    {
        static unsigned reportedReent = 0;
        if (reportedReent < 16)
        {
            ++reportedReent;
            KRKRNS_LOG("[heap] foreign free without reent ptr=%p (leaked)", pointer);
        }
        return;
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
            void *grown = AllocateLocked(size, __builtin_return_address(0));
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
