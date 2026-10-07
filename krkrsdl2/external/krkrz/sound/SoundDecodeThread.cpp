//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Sound Decode Thead for QueueSoundBuffer
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "KrkrNSLog.h"
#include "TickCount.h"

#include "MsgIntf.h"
#include "SoundDecodeThread.h"
#include "QueueSoundBufferImpl.h"
#include "SoundSamples.h"

//---------------------------------------------------------------------------
static const tTVPThreadPriority TVPDecodeThreadHighPriority = ttpHigher;
//---------------------------------------------------------------------------
tTVPSoundDecodeThread::tTVPSoundDecodeThread( tTJSNI_QueueSoundBuffer * owner )
 : Owner(owner), DecodedSamples(0) {
	krkrns_lastWork = TVPGetRoughTickCount32();
	// The OS thread is started on first playback (tTJSNI_QueueSoundBuffer::
	// StartPlay), NOT here: KAG titles construct pools of sound buffers during
	// boot, and the real console's thread limit cannot take one thread per
	// unused buffer (a console boot died on exactly this thread).
}
//---------------------------------------------------------------------------
tTVPSoundDecodeThread::~tTVPSoundDecodeThread() {
	Terminate();
	ClearQueue();
	Event.Set();
	WaitFor();
}
//---------------------------------------------------------------------------
void tTVPSoundDecodeThread::Execute(void) {
	SetPriority(TVPDecodeThreadHighPriority);
	while( !GetTerminated() ) {
		tjs_uint32 count = 0;
		tjs_int labelEventStep = TVP_TIMEOFS_INVALID_VALUE;
		tjs_uint32 labelEventTick = 0;
		{
			// KRKR-ns device diagnosis: this thread holds OneLoopCS across
			// buf->Decode(), which reads the storage.  The main thread's
			// StartPlay waits for that lock (Thread->ClearQueue), so a slow SD
			// read here parks the main thread.  Time the cycle and prove
			// liveness during a stall.
			const tjs_uint32 krkrns_t0 = TVPGetRoughTickCount32();
			tTJSCriticalSectionHolder cs_holder(OneLoopCS);
			// Completion callbacks only wake us. Recycle their buffers here,
			// using the same decoder -> owner lock order as normal decoding.
			Owner->DispatchAudioCallbacks();
			count = Samples.size();
			if( count ) {
				// バッファにデコードしたSampleを入れる
				auto itr = Samples.begin();
				tTVPSoundSamplesBuffer* buf = *itr;
				buf->Decode();
				buf->SetDecodePosition( DecodedSamples );
				DecodedSamples += buf->GetInSamples();

				// デコード済みSampleを再生ストリームへ移動
				labelEventStep = Owner->PushPlayStream( buf );
				labelEventTick = TVPGetRoughTickCount32();
				Samples.erase( itr );
				count = Samples.size();
			}
			const tjs_uint32 krkrns_dt = TVPGetRoughTickCount32() - krkrns_t0;
			if( krkrns_dt >= 300 )
				KRKRNS_LOG("[slow] snd-decode %ums queued=%u", krkrns_dt, (unsigned)count);
			krkrns_lastWork = krkrns_t0;
		}
		// The event thread holds the global sound-list lock. It may wait for
		// us during shutdown, so never acquire that lock while holding ours.
		if( labelEventStep != TVP_TIMEOFS_INVALID_VALUE ) {
			const tjs_int remaining = (tjs_int)(labelEventTick + labelEventStep - TVPGetRoughTickCount32());
			Owner->ReschedulePendingLabelEvent(remaining > 0 ? remaining : 0);
		}
		{
			// Liveness: one line per 10 s of an otherwise silent thread, so a
			// frozen session distinguishes "the decode thread is stuck too"
			// from "only the main thread is stuck".
			static tjs_uint32 krkrns_lastAlive = 0;
			const tjs_uint32 krkrns_now = TVPGetRoughTickCount32();
			if( krkrns_now - krkrns_lastAlive >= 10000 )
			{
				krkrns_lastAlive = krkrns_now;
				KRKRNS_LOG("[snd] decode thread alive, last work %ums ago, queued=%u",
					(unsigned)(krkrns_now - krkrns_lastWork), (unsigned)count);
			}
		}

		if( GetTerminated() ) break;

		if( count == 0 ) {
			// buffer is empty; sleep infinite
			Event.WaitFor(0);
		} else {
			// buffer is not full; sleep shorter
			// ダブルバッファリングなのでここに来る可能性は低いが、デコードが極度に送れている場合は来る
			// 一応スレッド切り替えして占有は避け、次のバッファをデコードする
#ifdef KRKRZ_USE_SDL_THREADS
			SDL_Delay(1);
#else
#if !defined(__EMSCRIPTEN__) || (defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__))
			std::this_thread::yield();
#endif
#endif
		}
	}
}
//---------------------------------------------------------------------------
void tTVPSoundDecodeThread::Interrupt() {
	ClearQueue();
}
//---------------------------------------------------------------------------
void tTVPSoundDecodeThread::StartDecoding( tjs_int64 predecoded ) {
	DecodedSamples = predecoded;
	Event.Set();
}
//---------------------------------------------------------------------------
void tTVPSoundDecodeThread::PushSamplesBuffer( tTVPSoundSamplesBuffer* buf ) {
	// キューにバッファを入れ、スレッドを起こす
	tTJSCriticalSectionHolder cs_holder(OneLoopCS);
	Samples.push_back(buf);
	Event.Set();
}
//---------------------------------------------------------------------------
void tTVPSoundDecodeThread::ClearQueue() {
	// キューを空にする
	tTJSCriticalSectionHolder cs_holder(OneLoopCS);
	Samples.clear();
}
//---------------------------------------------------------------------------
