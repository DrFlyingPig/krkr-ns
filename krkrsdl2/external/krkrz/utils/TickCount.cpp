//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// safe 64bit System Tick Count
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "tjsUtils.h"
#include "TickCount.h"
#include "SysInitIntf.h"
#include "ThreadIntf.h"

#ifdef TVP_COMPILING_KRKRSDL2
#include <SDL.h>
#include <atomic>
#include <mutex>
#else
#ifdef _WIN32
#include <mmsystem.h>
#else
#include <time.h>
#endif
#endif

#if 0
// システムに依存しない実装ではあるが、乱数の偏り等懸念される
// 環境ごとにシステム軌道からのtickを取得することにする
#include <chrono>
//---------------------------------------------------------------------------
class tTVPTickCounter {
	std::chrono::time_point<std::chrono::system_clock> start_;
public:
	tTVPTickCounter() : start_(std::chrono::system_clock::now()) {}
	tjs_uint32 Count() const {
		auto current = std::chrono::system_clock::now();
		auto duration = current - start_;
		auto msec = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
		return static_cast<tjs_uint32>(msec);
	}
} static TVPTickCounter;
//---------------------------------------------------------------------------
#endif

#ifndef TVP_COMPILING_KRKRSDL2
//---------------------------------------------------------------------------
// 64bit may enough to hold usual time count.
// ( 32bit is clearly insufficient )
//---------------------------------------------------------------------------
static tjs_uint64 TVPTickCountBias = 0;
static tjs_uint TVPWatchLastTick;
static tTJSCriticalSection TVPTickWatchCS;
//---------------------------------------------------------------------------
#endif

//---------------------------------------------------------------------------
// KRKR-ns: freeze the engine clock across an application suspension
//---------------------------------------------------------------------------
// A suspended host app resumes with a huge jump in the raw tick source: the
// Switch goes to sleep or to the home menu, an emulator gets paused, or the
// whole machine is suspended.  Everything that derives elapsed time from the
// engine clock then sees one enormous step, so a single frame replays the whole
// backlog.  A game resumed after ~10 hours fast-forwarded its E-mote animation
// to the end of every motion (progress() fed the whole gap in as one delta),
// rebuilt the E-mote resource manager ~32 times in 40 seconds and pushed the
// heap into the process ceiling, failing with std::bad_alloc.
//
// Treat any single step longer than any plausible frame as a suspension instead:
// advance the engine clock by about one frame and swallow the rest of the gap.
// Clamping the single 32bit accessor keeps timer deadlines AND the elapsed-time
// readers that compare against stored ticks (System.getTickCount -> KAG's
// motion clock, sound label events) in one frozen domain, instead of mixing a
// clamped clock with a raw one.  Host-level readers of SDL_GetTicks() itself
// (log rate limits, profiler windows, diagnostics) keep real time.
//---------------------------------------------------------------------------
#ifdef TVP_COMPILING_KRKRSDL2
static tjs_uint64 TVPGetRawSdlTick64()
{
	return SDL_GetTicks64();
}

// Longer than any frame a working engine should take, short enough to catch a
// real suspension.  A legitimately blocked frame (scene load) also lands here,
// which is the wanted behaviour: the engine was not running, so its timer queue
// must not be replayed.
static const tjs_uint64 TVP_TICK_SUSPEND_GAP_MS = 30000;
// Engine time advanced by a detected resumption (one 60 Hz frame).
static const tjs_uint64 TVP_TICK_RESUME_STEP_MS = 17;
// Time swallowed by suspension gaps so far; engine clock = raw - skip.
static std::atomic<tjs_uint64> TVPTickClampSkip(0);
// Raw tick observed by the previous read; the plain data race between readers
// is harmless (every writer publishes a real recent tick) and the gap decision
// is re-taken under the mutex below.  Kept in 64 bits so the 49-day rollover of
// the 32bit public accessor cannot look like a suspension.
static std::atomic<tjs_uint64> TVPTickClampLastRaw(0);
static std::atomic<bool> TVPTickClampPrimed(false);
static std::mutex TVPTickClampMutex;

static tjs_uint64 TVPGetClampedSdlTick64()
{
	const tjs_uint64 raw = TVPGetRawSdlTick64();
	const tjs_uint64 last = TVPTickClampLastRaw.load(std::memory_order_relaxed);
	// A reader that sampled the clock earlier than another one can observe
	// last > raw; that needs no decision, only a suspension gap does.
	const bool suspension =
		TVPTickClampPrimed.load(std::memory_order_relaxed) &&
		raw > last && raw - last > TVP_TICK_SUSPEND_GAP_MS;

	if(!TVPTickClampPrimed.load(std::memory_order_relaxed) || suspension)
	{
		// Cold start or a gap that needs a decision: take the mutex and re-read,
		// so two threads waking from the same suspension cannot both swallow it.
		std::lock_guard<std::mutex> holder(TVPTickClampMutex);
		const tjs_uint64 previous =
			TVPTickClampLastRaw.load(std::memory_order_relaxed);

		if(!TVPTickClampPrimed.load(std::memory_order_relaxed))
		{
			TVPTickClampPrimed.store(true, std::memory_order_relaxed);
			TVPTickClampLastRaw.store(raw, std::memory_order_relaxed);
		}
		else if(raw > previous)
		{
			if(raw - previous > TVP_TICK_SUSPEND_GAP_MS)
				TVPTickClampSkip.fetch_add(raw - previous - TVP_TICK_RESUME_STEP_MS,
					std::memory_order_relaxed);
			TVPTickClampLastRaw.store(raw, std::memory_order_relaxed);
		}
	}
	else if(raw > last)
	{
		// Fast path: track the latest tick, no gap worth a decision.
		TVPTickClampLastRaw.store(raw, std::memory_order_relaxed);
	}

	return raw - TVPTickClampSkip.load(std::memory_order_relaxed);
}
#endif

//---------------------------------------------------------------------------
// TVPGetRoughTickCount
// 32bit値のtickカウントを得る
//---------------------------------------------------------------------------
tjs_uint32 TVPGetRoughTickCount32()
{
#ifdef TVP_COMPILING_KRKRSDL2
	return (tjs_uint32)TVPGetClampedSdlTick64();
#else
#ifdef _WIN32
	return timeGetTime();	// win32 mmsystem.h
#else
	struct timespec now;
	clock_gettime( CLOCK_MONOTONIC, &now );	// Android の SystemClock.uptimeMillis() では SYSTEM_TIME_MONOTONIC(CLOCK_MONOTONIC) が使われている
	//clock_gettime( CLOCK_BOOTTIME, &now );
	return static_cast<tjs_uint32>( now.tv_sec * 1000LL + now.tv_nsec / 1000000LL );
#endif
#endif
//	return TVPTickCounter.Count();
}


//---------------------------------------------------------------------------
#ifndef TVP_COMPILING_KRKRSDL2
static tjs_uint TVPCheckTickOverflow()
{
	tjs_uint curtick;
	{	// thread-protected
		tTJSCriticalSectionHolder holder(TVPTickWatchCS);

		curtick = TVPGetRoughTickCount32();
		if(curtick < TVPWatchLastTick)
		{
			// timeGetTime() was overflowed
			TVPTickCountBias += 0x100000000L; // add 1<<32
		}
		TVPWatchLastTick = curtick;
	}	// end-of-thread-protected
	return curtick;
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
class tTVPWatchThread : public tTVPThread
{
	// thread which watches overflow of 32bit counter of TVPGetRoughTickCount32

	tTVPThreadEvent Event;

public:

	tTVPWatchThread();
	~tTVPWatchThread();

protected:
	void Execute();

} static * TVPWatchThread = NULL;
//---------------------------------------------------------------------------
tTVPWatchThread::tTVPWatchThread()
{
	TVPWatchLastTick = TVPGetRoughTickCount32();
	SetPriority(ttpNormal);
	StartTread();
}
//---------------------------------------------------------------------------
tTVPWatchThread::~tTVPWatchThread()
{
	Terminate();
	Event.Set();
	WaitFor();
}
//---------------------------------------------------------------------------
void tTVPWatchThread::Execute()
{
	while(!GetTerminated())
	{
		TVPCheckTickOverflow();

		Event.WaitFor(0x10000000);
			// 0x10000000 will be enough to watch timeGetTime()'s counter overflow.
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
static void TVPWatchThreadInit()
{
	if(!TVPWatchThread)
	{
		TVPWatchThread = new tTVPWatchThread();
	}
}
//---------------------------------------------------------------------------
static void TVPWatchThreadUninit()
{
	if(TVPWatchThread)
	{
		delete TVPWatchThread;
		TVPWatchThread = NULL;
	}
}
//---------------------------------------------------------------------------
static tTVPAtExit TVPWatchThreadUninitAtExit(TVP_ATEXIT_PRI_SHUTDOWN,
	TVPWatchThreadUninit);
//---------------------------------------------------------------------------
#endif



//---------------------------------------------------------------------------
// TVPGetTickCount
//---------------------------------------------------------------------------
tjs_uint64 TVPGetTickCount()
{
#ifdef TVP_COMPILING_KRKRSDL2
	return TVPGetRoughTickCount32();
#else
	TVPWatchThreadInit();

	tjs_uint curtick = TVPCheckTickOverflow();

	return curtick + TVPTickCountBias;
#endif
}
//---------------------------------------------------------------------------
// TVPGetTickClampSkippedMs
//---------------------------------------------------------------------------
tjs_uint64 TVPGetTickClampSkippedMs()
{
#ifdef TVP_COMPILING_KRKRSDL2
	return TVPTickClampSkip.load(std::memory_order_relaxed);
#else
	return 0;
#endif
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPStartTickCount
//---------------------------------------------------------------------------
void TVPStartTickCount()
{
#ifndef TVP_COMPILING_KRKRSDL2
	TVPWatchThreadInit();
#endif
}
//---------------------------------------------------------------------------



