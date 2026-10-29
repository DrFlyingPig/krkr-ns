/* SPDX-License-Identifier: MIT */
/* Copyright (c) Kirikiri SDL2 Developers */

#include "tjsCommHead.h"
#include "NativeEventQueue.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE)
#include "WindowsUtil.h"
#endif

#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE)
int NativeEventQueueImplement::CreateUtilWindow() {
	::ZeroMemory( &wc_, sizeof(wc_) );
	wc_.cbSize = sizeof(WNDCLASSEX);
	wc_.lpfnWndProc = ::DefWindowProc;
	wc_.hInstance = ::GetModuleHandle(NULL);
	//wc_.lpszClassName = TJS_W("TVPUtilWindow");
	wc_.lpszClassName = TJS_W("TVPQueueWindow");

	WNDCLASSEX tmpwc = { sizeof(WNDCLASSEX) };
	BOOL ClassRegistered = ::GetClassInfoEx( wc_.hInstance, wc_.lpszClassName, &tmpwc );
	if( ClassRegistered == 0 ) {
		if( ::RegisterClassEx( &wc_ ) == 0 ) {
			TVP_WINDOWS_ERROR_LOG;
			return HRESULT_FROM_WIN32(::GetLastError());
		}
	}
	window_handle_ = ::CreateWindowEx( WS_EX_TOOLWINDOW, wc_.lpszClassName, TJS_W(""),
						WS_POPUP, 0, 0, 0, 0, NULL, NULL, wc_.hInstance, NULL );

	if( window_handle_ == NULL ) {
		TVP_WINDOWS_ERROR_LOG;
		return HRESULT_FROM_WIN32(::GetLastError());
	}
    ::SetWindowLongPtr( window_handle_, GWLP_WNDPROC, (LONG_PTR)NativeEventQueueImplement::WndProc );
	::SetWindowLongPtr( window_handle_, GWLP_USERDATA, (LONG_PTR)this );
	return S_OK;
}

LRESULT WINAPI NativeEventQueueImplement::WndProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam ) {
	NativeEvent event;
	event.Result = 0;
	event.HWnd = hWnd;
	event.Message = msg;
	event.WParam = wParam;
	event.LParam = lParam;
	NativeEventQueueIntarface *win = reinterpret_cast<NativeEventQueueIntarface*>(::GetWindowLongPtr(hWnd,GWLP_USERDATA));
	if( win != NULL ) {
		win->Dispatch( event );
		return event.Result;
	} else {
		return ::DefWindowProc(event.HWnd,event.Message,event.WParam,event.LParam);
	}
}

// デフォルトハンドラ
void NativeEventQueueImplement::HandlerDefault( NativeEvent& event ) {
	event.Result = ::DefWindowProc(event.HWnd,event.Message,event.WParam,event.LParam);
}
void NativeEventQueueImplement::Allocate() {
	CreateUtilWindow();
}
void NativeEventQueueImplement::Deallocate() {
	if( window_handle_ != NULL ) {
		::SetWindowLongPtr( window_handle_, GWLP_USERDATA, (LONG_PTR)NULL );
		::DestroyWindow( window_handle_ );
		window_handle_ = NULL;
	}
}
void NativeEventQueueImplement::PostEvent( const NativeEvent& event ) {
	::PostMessage( window_handle_, event.Message, event.WParam, event.LParam );
}
#else
#include "Application.h"
#include "DebugIntf.h"
#include <SDL.h>

tjs_uint32 NativeEventQueueImplement::native_event_queue_custom_event_type = 0;

NativeEventQueueImplement::NativeEventQueueImplement() {
	if (NativeEventQueueImplement::native_event_queue_custom_event_type != 0)
	{
		return;
	}
	if (SDL_WasInit(SDL_INIT_EVENTS) == 0)
	{
		SDL_Init(SDL_INIT_EVENTS);
	}
	NativeEventQueueImplement::native_event_queue_custom_event_type = SDL_RegisterEvents(1);
}

// KRKR-ns fix: SDL_PushEvent from the timer/limiter threads raced the main
// thread's event pump on console SDL (single-threaded event design) and the
// push-retry loop could spin forever while the queue was drained by another
// thread, deadlocking game boot after the launcher hand-off.  Background
// threads now only enqueue under a mutex and set a flag; the main pump
// drains the pending list at the top of sdl_process_events(), so all
// dispatching happens on the main thread.
static SDL_mutex * krkrsdl2_pending_events_mutex = nullptr;
static std::vector<NativeEvent*> * krkrsdl2_pending_events = nullptr;
static std::atomic<bool> krkrsdl2_pending_events_initialized{false};
static std::once_flag krkrsdl2_pending_events_once;

struct KrkrNSDefaultNativeEventBatch
{
	std::vector<NativeEvent*> Events;
	std::size_t Next = 0;
	KrkrNSDefaultNativeEventBatch* Following = nullptr;
};
struct KrkrNSDefaultNativeEventFrame
{
	KrkrNSDefaultNativeEventBatch* Head = nullptr;
	KrkrNSDefaultNativeEventFrame* Previous = nullptr;
};
static KrkrNSDefaultNativeEventFrame* krkrsdl2_active_native_frames = nullptr;
static KrkrNSDefaultNativeEventBatch* krkrsdl2_deferred_native_head = nullptr;
static KrkrNSDefaultNativeEventBatch* krkrsdl2_deferred_native_tail = nullptr;

// Caller holds the payload mutex. Cancelled/consumed nodes need no later pump
// to release their vector storage, including during engine teardown.
static void krkrsdl2_prune_empty_native_batches(KrkrNSDefaultNativeEventBatch*& head) noexcept
{
	auto** link = &head;
	while (auto* batch = *link) {
		while (batch->Next < batch->Events.size() && !batch->Events[batch->Next]) ++batch->Next;
		if (batch->Next == batch->Events.size()) {
			*link = batch->Following;
			delete batch;
		} else link = &batch->Following;
	}
}

class KrkrNSDefaultNativeEventLock
{
public:
	KrkrNSDefaultNativeEventLock() { SDL_LockMutex(krkrsdl2_pending_events_mutex); }
	~KrkrNSDefaultNativeEventLock() { SDL_UnlockMutex(krkrsdl2_pending_events_mutex); }
};

static void krkrsdl2_pending_events_init()
{
	std::call_once(krkrsdl2_pending_events_once, [] {
		std::unique_ptr<std::vector<NativeEvent*>> pending(new std::vector<NativeEvent*>());
		SDL_mutex* mutex = SDL_CreateMutex();
		if (!mutex) throw std::bad_alloc();
		krkrsdl2_pending_events_mutex = mutex;
		krkrsdl2_pending_events = pending.release();
		krkrsdl2_pending_events_initialized.store(true, std::memory_order_release);
	});
}

class KrkrNSDefaultNativeEventDrain
{
	KrkrNSDefaultNativeEventFrame Frame;
	bool Active = false;
public:
	KrkrNSDefaultNativeEventDrain()
	{
		KrkrNSDefaultNativeEventLock lock;
		if (krkrsdl2_pending_events->empty() && !krkrsdl2_deferred_native_head) return;
		// Allocate before taking any pending/deferred ownership. A failed
		// allocation leaves every callback available for the next pump.
		std::unique_ptr<KrkrNSDefaultNativeEventBatch> pending;
		if (!krkrsdl2_pending_events->empty()) pending.reset(new KrkrNSDefaultNativeEventBatch());
		Frame.Head = krkrsdl2_deferred_native_head;
		if (pending) {
			pending->Events.swap(*krkrsdl2_pending_events);
			if (krkrsdl2_deferred_native_tail) krkrsdl2_deferred_native_tail->Following = pending.get();
			else Frame.Head = pending.get();
			pending.release();
		}
		krkrsdl2_deferred_native_head = krkrsdl2_deferred_native_tail = nullptr;
		Frame.Previous = krkrsdl2_active_native_frames;
		krkrsdl2_active_native_frames = &Frame;
		Active = true;
	}
	~KrkrNSDefaultNativeEventDrain()
	{
		if (!Active) return;
		KrkrNSDefaultNativeEventLock lock;
		krkrsdl2_active_native_frames = Frame.Previous;
		krkrsdl2_prune_empty_native_batches(Frame.Head);
		if (Frame.Head) {
			// Exception unwinding transfers the remaining chain without an
			// allocation. Older outer batches precede nested deferred work,
			// which in turn precedes newly posted events on the next pump.
			auto* tail = Frame.Head;
			while (tail->Following) tail = tail->Following;
			tail->Following = krkrsdl2_deferred_native_head;
			krkrsdl2_deferred_native_head = Frame.Head;
			if (!krkrsdl2_deferred_native_tail) krkrsdl2_deferred_native_tail = tail;
		}
	}
	NativeEvent* Next()
	{
		if (!Active) return nullptr;
		KrkrNSDefaultNativeEventLock lock;
		while (auto* batch = Frame.Head) {
			while (batch->Next < batch->Events.size()) {
				auto*& slot = batch->Events[batch->Next++];
				auto* event = slot;
				slot = nullptr;
				if (event) return event;
			}
			Frame.Head = batch->Following;
			delete batch; // all payload slots have been consumed or cancelled
		}
		return nullptr;
	}
};

void krkrsdl2_drain_pending_native_events()
{
	if (!krkrsdl2_pending_events_initialized.load(std::memory_order_acquire)) return;
	KrkrNSDefaultNativeEventDrain drain;
	while (auto* event = drain.Next()) {
		// HandleEvent deletes itself after successful dispatch. On failure its
		// detached payload still belongs to this caller, exactly once.
		try { event->HandleEvent(); }
		catch (...) { delete event; throw; }
	}
}

void NativeEventQueueImplement::Clear(unsigned int message)
{
	if (!krkrsdl2_pending_events_initialized.load(std::memory_order_acquire)) return;
	KrkrNSDefaultNativeEventLock lock;
	const auto matches = [this, message](const NativeEvent* event) {
		return event && event->queue == this && (!message || event->Message == message);
	};
	for (auto it = krkrsdl2_pending_events->begin();
		it != krkrsdl2_pending_events->end();)
	{
		NativeEvent *ev = *it;
		if (matches(ev))
		{
			delete ev;
			it = krkrsdl2_pending_events->erase(it);
		}
		else
		{
			++it;
		}
	}
	const auto clearBatches = [&](KrkrNSDefaultNativeEventBatch* batch) {
		for (; batch; batch = batch->Following)
			for (std::size_t i = batch->Next; i < batch->Events.size(); ++i)
				if (matches(batch->Events[i])) { delete batch->Events[i]; batch->Events[i] = nullptr; }
	};
	clearBatches(krkrsdl2_deferred_native_head);
	krkrsdl2_prune_empty_native_batches(krkrsdl2_deferred_native_head);
	krkrsdl2_deferred_native_tail = krkrsdl2_deferred_native_head;
	if (krkrsdl2_deferred_native_tail)
		while (krkrsdl2_deferred_native_tail->Following)
			krkrsdl2_deferred_native_tail = krkrsdl2_deferred_native_tail->Following;
	for (auto* frame = krkrsdl2_active_native_frames; frame; frame = frame->Previous) {
		clearBatches(frame->Head);
		krkrsdl2_prune_empty_native_batches(frame->Head);
	}
}

void NativeEventQueueImplement::Deallocate()
{
	Clear();
}

void NativeEventQueueImplement::PostEvent(const NativeEvent& ev) {
	if (NativeEventQueueImplement::native_event_queue_custom_event_type == 0)
	{
		return;
	}
	krkrsdl2_pending_events_init();
	std::unique_ptr<NativeEvent> tmp_ev(new NativeEvent(ev));
	tmp_ev->SetQueue(this);
	{
		KrkrNSDefaultNativeEventLock lock;
		krkrsdl2_pending_events->push_back(tmp_ev.get());
		tmp_ev.release();
	}
	// Wake the main thread's SDL_WaitEvent without carrying the payload: a
	// single bounded push (no retry loop) is the documented thread-safe use;
	// if it is dropped the next pump tick still drains the pending list.
	if (NativeEventQueueImplement::native_event_queue_custom_event_type != 0)
	{
		SDL_Event wake;
		SDL_memset(&wake, 0, sizeof(wake));
		wake.type = NativeEventQueueImplement::native_event_queue_custom_event_type;
		wake.user.code = 1; // wake-only marker (no data2 payload)
		SDL_PushEvent(&wake);
	}
}
#endif
