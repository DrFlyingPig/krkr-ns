/* SPDX-License-Identifier: MIT */
/* Copyright (c) Kirikiri SDL2 Developers */

#include "tjsCommHead.h"
//---------------------------------------------------------------------------
#include "tjsUtils.h"
#include "MsgIntf.h"
#include "BitmapBitsAlloc.h"
#include "SysInitIntf.h"
#include "EventIntf.h"
#include "DebugIntf.h"
#include "KrkrNSLog.h"
#include <atomic>
#include <new>
#ifdef __SWITCH__
#include <malloc.h>
// Implemented in core/sdl2/SwitchHeap.cpp: reports how much memory the
// size-classed pool currently holds, so hardware logs can attribute usage.
extern "C" void KrkrNSSwitchHeapStats(size_t *, size_t *, int *);
#endif

static std::atomic<size_t> TVPBitmapLiveBytes{0};

static std::atomic<size_t> TVPBitmapPeakBytes{0};
static std::atomic<size_t> TVPBitmapLiveCount{0};

void TVPLogBitmapMemorySnapshot()
{
	tTVPBitmapBitsAlloc::LogMemory("periodic");
}

// Helper used by the PSB reader for its MDF decompression scratch.  It stays a
// single code path on every platform and now just forwards to the system
// allocator, so the scratch follows the same lifetime rules as the bitmap it
// produces instead of keeping a private arena alive.
extern "C" void *KrkrNSAllocateArenaBuffer(size_t &size)
{
	void *result = malloc(size);
	if (result) size = size; // malloc keeps the requested size
	return result;
}

extern "C" void KrkrNSFreeArenaBuffer(void *data, size_t)
{
	free(data);
}

void tTVPBitmapBitsAlloc::LogMemory(const char *reason, size_t requested)
{
#ifdef __SWITCH__
	const auto heap = mallinfo();
	size_t tlsfReserved = 0, tlsfChunks = 0;
	int tlsfActive = 0;
	KrkrNSSwitchHeapStats(&tlsfReserved, &tlsfChunks, &tlsfActive);
	KRKRNS_LOG("[memory] %s request=%zu bitmap_live=%zu bitmap_peak=%zu bitmaps=%zu heap_used=%zu heap_free=%zu heap_arena=%zu heap_top=%zu tlsf_active=%d tlsf_reserved=%zu tlsf_chunks=%zu",
		reason, requested, TVPBitmapLiveBytes.load(std::memory_order_relaxed),
		TVPBitmapPeakBytes.load(std::memory_order_relaxed),
		TVPBitmapLiveCount.load(std::memory_order_relaxed),
		heap.uordblks, heap.fordblks, heap.arena, heap.keepcost,
		tlsfActive, tlsfReserved, tlsfChunks);
#endif
}

#if defined(__APPLE__) || defined(__linux__) || defined(ANDROID) || defined(__ANDROID__)
#define USE_MMAP_FOR_ALLOCATION
#endif

#ifdef USE_MMAP_FOR_ALLOCATION
#include <sys/mman.h>
#endif

class BasicAllocator : public iTVPMemoryAllocator
#ifdef __SWITCH__
	, public tTVPCompactEventCallbackIntf
#endif
{
public:
	BasicAllocator() {
#ifdef __SWITCH__
		TVPAddCompactEventHook(this);
#endif
	}
	~BasicAllocator() {
#ifdef __SWITCH__
		TVPRemoveCompactEventHook(this);
#endif
	}
#ifdef __SWITCH__
	void TJS_INTF_METHOD OnCompact(tjs_int level) override
	{
		// The reference compacts the CRT heap when a maximal compaction runs
		// (TVPEnableGlobalHeapCompaction). newlib's equivalent is malloc_trim,
		// which hands the releasable top of the heap back to the system.
		if (level >= TVP_COMPACT_LEVEL_MAX) ::malloc_trim(0);
	}
#endif
	void* allocate(size_t &size)
	{
#ifdef USE_MMAP_FOR_ALLOCATION
		return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
		// Switch takes this branch: libnx has no mmap, so the mmap strategy the
		// upstream SDL2 port uses on Apple/Linux/Android is not available here.
		(void)size;
		return malloc(size);
#endif
	}
	void free(void* mem, size_t size)
	{
#ifdef USE_MMAP_FOR_ALLOCATION
		munmap(mem, size);
#else
		(void)size;
		::free( mem );
#endif
	}
};

iTVPMemoryAllocator* tTVPBitmapBitsAlloc::Allocator = NULL;
tTJSCriticalSection tTVPBitmapBitsAlloc::AllocCS;

void tTVPBitmapBitsAlloc::InitializeAllocator() {
	if (Allocator == NULL)
	{
		Allocator = new BasicAllocator();
	}
}
void tTVPBitmapBitsAlloc::FreeAllocator() {
	if( Allocator ) delete Allocator;
	Allocator = NULL;
}
static tTVPAtExit
	TVPUninitMessageLoad(TVP_ATEXIT_PRI_CLEANUP, tTVPBitmapBitsAlloc::FreeAllocator);

void* tTVPBitmapBitsAlloc::Alloc( tjs_uint size, tjs_uint width, tjs_uint height ) {
	return AllocInternal(size, width, height, true);
}
void* tTVPBitmapBitsAlloc::AllocNoCompact(tjs_uint size, tjs_uint width, tjs_uint height) {
	try {
		return AllocInternal(size, width, height, false);
	} catch (const std::bad_alloc&) {
		return NULL;
	}
}
void* tTVPBitmapBitsAlloc::AllocInternal(tjs_uint size, tjs_uint width, tjs_uint height, bool allowCompact) {
	if(size == 0) return NULL;
	tTJSCriticalSectionHolder Lock(AllocCS);	// Lock

	if (allowCompact) InitializeAllocator();
	else if (!Allocator) return NULL;
	tjs_uint8 * ptrorg, * ptr;
	size_t allocbytes = 16 + static_cast<size_t>(size) + sizeof(tTVPLayerBitmapMemoryRecord) + sizeof(tjs_uint32)*2;

	ptr = ptrorg = (tjs_uint8*)Allocator->allocate(allocbytes);
	if(!ptr) {
		if (!allowCompact) return NULL;
		LogMemory("bitmap-allocation-failed", allocbytes);
		// Do GC
		TVPDeliverCompactEvent(TVP_COMPACT_LEVEL_CRITICAL);
		ptr = ptrorg = (tjs_uint8*)Allocator->allocate(allocbytes);
		LogMemory(ptr ? "bitmap-recovered" : "bitmap-retry-failed", allocbytes);
		if(!ptr) {
			TVPThrowExceptionMessage(TVPCannotAllocateBitmapBits,
				TJS_W("at TVPAllocBitmapBits"), ttstr((tjs_int)allocbytes) + TJS_W("(") +
				ttstr((int)width) + TJS_W("x") + ttstr((int)height) + TJS_W(")"));
		}
	}
	// align to a paragraph ( 16-bytes )
	ptr += 16 + sizeof(tTVPLayerBitmapMemoryRecord);
	*reinterpret_cast<tTJSPointerSizedInteger*>(&ptr) >>= 4;
	*reinterpret_cast<tTJSPointerSizedInteger*>(&ptr) <<= 4;

	tTVPLayerBitmapMemoryRecord * record =
		(tTVPLayerBitmapMemoryRecord*)
		(ptr - sizeof(tTVPLayerBitmapMemoryRecord) - sizeof(tjs_uint32));

	// fill memory allocation record
	record->alloc_ptr = (void *)ptrorg;
	record->orig_size = allocbytes;
	record->size = size;
	record->sentinel_backup1 = rand() + (rand() << 16);
	record->sentinel_backup2 = rand() + (rand() << 16);

	// set sentinel
	*(tjs_uint32*)(ptr - sizeof(tjs_uint32)) = ~record->sentinel_backup1;
	*(tjs_uint32*)(ptr + size              ) = ~record->sentinel_backup2;
		// Stored sentinels are nagated, to avoid that the sentinel backups in
		// tTVPLayerBitmapMemoryRecord becomes the same value as the sentinels.
		// This trick will make the detection of the memory corruption easier.
		// Because on some occasions, running memory writing will write the same
		// values at first sentinel and the tTVPLayerBitmapMemoryRecord.

	const size_t live = TVPBitmapLiveBytes.fetch_add(allocbytes, std::memory_order_relaxed) + allocbytes;
	size_t peak = TVPBitmapPeakBytes.load(std::memory_order_relaxed);
	while (live > peak && !TVPBitmapPeakBytes.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {}
	TVPBitmapLiveCount.fetch_add(1, std::memory_order_relaxed);
	// return buffer pointer
	return ptr;
}
void tTVPBitmapBitsAlloc::Free( void* ptr ) {
	if(ptr)
	{
#if 0
		tTJSCriticalSectionHolder Lock(AllocCS);	// Lock
#endif

		// get memory allocation record pointer
		tjs_uint8 *bptr = (tjs_uint8*)ptr;
		tTVPLayerBitmapMemoryRecord * record =
			(tTVPLayerBitmapMemoryRecord*)
			(bptr - sizeof(tTVPLayerBitmapMemoryRecord) - sizeof(tjs_uint32));

		// check sentinel
		if(~(*(tjs_uint32*)(bptr - sizeof(tjs_uint32))) != record->sentinel_backup1)
			TVPAddLog( ttstr(TVPLayerBitmapBufferUnderrunDetectedCheckYourDrawingCode) );
		if(~(*(tjs_uint32*)(bptr + record->size      )) != record->sentinel_backup2)
			TVPAddLog( ttstr(TVPLayerBitmapBufferOverrunDetectedCheckYourDrawingCode) );

		TVPBitmapLiveBytes.fetch_sub(record->orig_size, std::memory_order_relaxed);
		TVPBitmapLiveCount.fetch_sub(1, std::memory_order_relaxed);
		Allocator->free( record->alloc_ptr, record->orig_size );
	}
}

