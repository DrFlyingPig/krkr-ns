/* SPDX-License-Identifier: MIT */
/* Copyright (c) Kirikiri SDL2 Developers */

#include "tjsCommHead.h"
#include "NativeEventQueue.h"
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
static bool krkrsdl2_pending_events_initialized = false;

static void krkrsdl2_pending_events_init()
{
	if (!krkrsdl2_pending_events_initialized)
	{
		krkrsdl2_pending_events_initialized = true;
		krkrsdl2_pending_events_mutex = SDL_CreateMutex();
		krkrsdl2_pending_events = new std::vector<NativeEvent*>();
	}
}

void krkrsdl2_drain_pending_native_events()
{
	if (!krkrsdl2_pending_events_initialized) return;
	SDL_LockMutex(krkrsdl2_pending_events_mutex);
	std::vector<NativeEvent*> local;
	local.swap(*krkrsdl2_pending_events);
	SDL_UnlockMutex(krkrsdl2_pending_events_mutex);
	for (NativeEvent* ev : local)
	{
		ev->HandleEvent();
	}
}

void NativeEventQueueImplement::PostEvent(const NativeEvent& ev) {
	if (NativeEventQueueImplement::native_event_queue_custom_event_type == 0)
	{
		return;
	}
	krkrsdl2_pending_events_init();
	NativeEvent * tmp_ev = new NativeEvent(ev);
	tmp_ev->SetQueue(this);
	SDL_LockMutex(krkrsdl2_pending_events_mutex);
	krkrsdl2_pending_events->push_back(tmp_ev);
	SDL_UnlockMutex(krkrsdl2_pending_events_mutex);
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
