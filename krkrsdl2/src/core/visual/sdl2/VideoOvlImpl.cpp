//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Video Overlay support implementation
//---------------------------------------------------------------------------


#include "tjsCommHead.h"

#include <algorithm>
#include "MsgIntf.h"
#include "VideoOvlImpl.h"
#include "DebugIntf.h"
#include "LayerIntf.h"
#include "LayerBitmapIntf.h"
#include "SysInitIntf.h"
#include "StorageImpl.h"
#include "../win32/krmovie.h"
#include "SwitchMovieOverlay.h" // Phase 4: FFmpeg player on Switch
#ifdef __SWITCH__
#include "KrkrNSLog.h"
#endif
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
#endif
#include "PluginImpl.h"
#include "WaveImpl.h"  // for DirectSound attenuate <-> TVP volume
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
#include <evcode.h>
#endif

#include "Application.h"
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
#include "TVPVideoOverlay.h"
#else
#define TVPDSAttenuateToPan(x) x
#define TVPDSAttenuateToVolume(x) x
#endif

#ifdef __SWITCH__
#include <SDL.h>
#include <algorithm>
#include <cstdint>
#include "SDLBitmapBridge.h"
#endif

//---------------------------------------------------------------------------
static std::vector<tTJSNI_VideoOverlay *> TVPVideoOverlayVector;
//---------------------------------------------------------------------------
static void TVPAddVideOverlay(tTJSNI_VideoOverlay *ovl)
{
	TVPVideoOverlayVector.push_back(ovl);
}
//---------------------------------------------------------------------------
static void TVPRemoveVideoOverlay(tTJSNI_VideoOverlay *ovl)
{
	std::vector<tTJSNI_VideoOverlay*>::iterator i;
	i = std::find(TVPVideoOverlayVector.begin(), TVPVideoOverlayVector.end(), ovl);
	if(i != TVPVideoOverlayVector.end())
		TVPVideoOverlayVector.erase(i);
}
//---------------------------------------------------------------------------
static void TVPShutdownVideoOverlay()
{
	// shutdown all overlay object and release krmovie.dll / krflash.dll
	std::vector<tTJSNI_VideoOverlay*>::iterator i;
	for(i = TVPVideoOverlayVector.begin(); i != TVPVideoOverlayVector.end(); i++)
	{
		(*i)->Shutdown();
	}
}
static tTVPAtExit TVPShutdownVideoOverlayAtExit
	(TVP_ATEXIT_PRI_PREPARE, TVPShutdownVideoOverlay);
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// tTJSNI_VideoOverlay
//---------------------------------------------------------------------------
tTJSNI_VideoOverlay::tTJSNI_VideoOverlay()
: EventQueue(this,&tTJSNI_VideoOverlay::WndProc)
{
	VideoOverlay = NULL;
	Rect.left = 0;
	Rect.top = 0;
	Rect.right = 320;
	Rect.bottom = 240;
	Visible = false;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	OwnerWindow = NULL;
#endif
	LocalTempStorageHolder = NULL;

	EventQueue.Allocate();

	Layer1 = NULL;
	Layer2 = NULL;
	Mode = vomOverlay;
	Loop = false;
	IsPrepare = false;
	SegLoopStartFrame = -1;
	SegLoopEndFrame = -1;
	IsEventPast = false;
	EventFrame = -1;

#if (defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)) || defined(__SWITCH__)
	Bitmap[0] = Bitmap[1] = NULL;
	BmpBits[0] = BmpBits[1] = NULL;
#endif
#ifdef __SWITCH__
	VideoFramesApplied = 0;
	// 0 = "no decoded frame yet": the player's frame counter starts at 0 and
	// only becomes 1 after the first frame is published, so an uninitialized
	// frame buffer is never blitted.
	LastPresentedFrame = 0;
#endif

	// Register for the overlay-mode present (the vector also feeds the
	// at-exit shutdown).  The win32 source declares these helpers but never
	// calls them -- on Switch the present pump is what needs the live list.
#ifdef __SWITCH__
	TVPAddVideOverlay(this);
#endif
}
//---------------------------------------------------------------------------
tTJSNI_VideoOverlay::~tTJSNI_VideoOverlay()
{
#ifdef __SWITCH__
	// Invalidate() unregisters too, but the registry must never keep a pointer
	// to a freed instance if a session leaked the object without invalidating.
	TVPRemoveVideoOverlay(this);
#endif
}
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD
tTJSNI_VideoOverlay::Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *tjs_obj)
{
	tjs_error hr = inherited::Construct(numparams, param, tjs_obj);
	if(TJS_FAILED(hr)) return hr;

	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_VideoOverlay::Invalidate()
{
#ifdef __SWITCH__
	TVPRemoveVideoOverlay(this);
#endif

	inherited::Invalidate();

	Close();

	EventQueue.Deallocate();
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Open(const ttstr &_name)
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	// open

	// first, close
	Close();


	// check window
	if(!Window) TVPThrowExceptionMessage(TVPWindowAlreadyMissing);

	// open target storage
	ttstr name(_name);
	ttstr param;

	const tjs_char * param_pos;
	int param_pos_ind;
	param_pos = TJS_strchr(name.c_str(), TJS_W('?'));
	param_pos_ind = (int)(param_pos - name.c_str());
	if(param_pos != NULL)
	{
		param = param_pos;
		name = ttstr(name, param_pos_ind);
	}

	IStream *istream = NULL;
	long size;
	ttstr ext = TVPExtractStorageExt(name).c_str();
	ext.ToLowerCase();

	{
		// prepate IStream
		tTJSBinaryStream *stream0 = NULL;
		try
		{
			stream0 = TVPCreateStream(name);
			size = (long)stream0->GetSize();
		}
		catch(...)
		{
			if(stream0) delete stream0;
			throw;
		}

		istream = new tTVPIStreamAdapter(stream0);
	}

	// 'istream' is an IStream instance at this point

	// create video overlay object
	try
	{
		{
			if(Mode == vomLayer)
				GetVideoLayerObject(EventQueue.GetOwner(), istream, name.c_str(), ext.c_str(), size, &VideoOverlay);
			else if(Mode == vomMixer)
				GetMixingVideoOverlayObject(EventQueue.GetOwner(), istream, name.c_str(), ext.c_str(), size, &VideoOverlay);
			else if(Mode == vomMFEVR)
				GetMFVideoOverlayObject(EventQueue.GetOwner(), istream, name.c_str(), ext.c_str(), size, &VideoOverlay);
			else
				GetVideoOverlayObject(EventQueue.GetOwner(), istream, name.c_str(), ext.c_str(), size, &VideoOverlay);
		}

		if( (Mode == vomOverlay) || (Mode == vomMixer) || (Mode == vomMFEVR) )
		{
			ResetOverlayParams();
		}
		else
		{	// set font and back buffer to layerVideo
			long	width, height;
			long			size;
			VideoOverlay->GetVideoSize( &width, &height );

			if( width <= 0 || height <= 0 )
				TVPThrowExceptionMessage(TVPErrorInKrMovieDLL, (const tjs_char*)TVPInvalidVideoSize);

			size = width * height * 4;
			if( Bitmap[0] != NULL )
				delete Bitmap[0];
			if( Bitmap[1] != NULL )
				delete Bitmap[1];
			Bitmap[0] = new tTVPBaseBitmap( width, height, 32 );
			Bitmap[1] = new tTVPBaseBitmap( width, height, 32 );

			BmpBits[0] = static_cast<BYTE*>(Bitmap[0]->GetBitmap()->GetScanLine( Bitmap[0]->GetBitmap()->GetHeight()-1 ));
			BmpBits[1] = static_cast<BYTE*>(Bitmap[1]->GetBitmap()->GetScanLine( Bitmap[1]->GetBitmap()->GetHeight()-1 ));
			//BmpBits[0] = static_cast<BYTE*>(Bitmap[0]->GetBitmap()->GetScanLine( 0 ));
			//BmpBits[1] = static_cast<BYTE*>(Bitmap[1]->GetBitmap()->GetScanLine( 0 ));

			VideoOverlay->SetVideoBuffer( BmpBits[0], BmpBits[1], size );
		}
	}
	catch(...)
	{
		if(istream) istream->Release();
		Close();
		throw;
	}
	if(istream) istream->Release();

	// set Status
	ClearWndProcMessages();
	SetStatus(tTVPVideoOverlayStatus::Stop);
#elif defined(__SWITCH__)
	// Match Kirikiri's normal lifecycle even when the Switch movie decoder is
	// not available yet.  The old port made open() a silent no-op, leaving the
	// object in "unload" forever and causing games to wait on their title movie.
	Close();
	if(!Window) TVPThrowExceptionMessage(TVPWindowAlreadyMissing);

	// KRKR-ns diagnostic: record every movie open attempt at entry.  On a title
	// screen that shows a movie the UI can render while the movie layer stays
	// black; without this line there is no way to tell "the game never asked for
	// a movie" from "the request failed before the stream was opened".
	KRKRNS_LOG("[video] open requested: %s", _name.AsNarrowStdString().c_str());

	ttstr name(_name);
	const tjs_char *param_pos = TJS_strchr(name.c_str(), TJS_W('?'));
	if(param_pos != NULL)
		name = ttstr(name, static_cast<int>(param_pos - name.c_str()));

	tTJSBinaryStream *stream = NULL;
	try
	{
		stream = TVPCreateStream(name);
		const tjs_uint64 size = stream->GetSize();
		TVPAddLog(TJS_W("[video] open stream=") + name +
			TJS_W(" size=") + ttstr(static_cast<tjs_int>(size)) +
			TJS_W(" mode=") + ttstr(static_cast<tjs_int>(Mode)));
	}
	catch(...)
	{
		KRKRNS_LOG("[video] open FAILED: stream not found for '%s'",
			name.AsNarrowStdString().c_str());
		if(stream) delete stream;
		throw;
	}

	// Phase 4: the FFmpeg-backed player. Follow the win32 layer path by
	// handing it the exact Kirikiri storage stream (which may be an archive
	// entry), then take the
	// frame size, build the two engine bitmaps the decode thread writes
	// into, and hand them to SetVideoBuffer.
	{
		SwitchMovieOverlay *player = new SwitchMovieOverlay(&EventQueue);
		VideoOverlay = player;
		long width = 0, height = 0;
		const bool opened = player->OpenStream(name, stream, width, height);
		stream = NULL; // ownership transferred even when OpenStream fails
		if (!opened)
		{
			VideoOverlay->Release();
			VideoOverlay = NULL;
			TVPThrowExceptionMessage(TVPErrorInKrMovieDLL,
				(const tjs_char *)TVPInvalidVideoSize);
		}
		if (width <= 0 || height <= 0)
		{
			VideoOverlay->Release();
			VideoOverlay = NULL;
			TVPThrowExceptionMessage(TVPErrorInKrMovieDLL,
				(const tjs_char *)TVPInvalidVideoSize);
		}

		long bmpsize = width * height * 4;
		if (Bitmap[0] != NULL) delete Bitmap[0];
		if (Bitmap[1] != NULL) delete Bitmap[1];
		Bitmap[0] = new tTVPBaseBitmap(width, height, 32);
		Bitmap[1] = new tTVPBaseBitmap(width, height, 32);
		BmpBits[0] = static_cast<BYTE*>(Bitmap[0]->GetBitmap()->GetScanLine(
			Bitmap[0]->GetBitmap()->GetHeight() - 1));
		BmpBits[1] = static_cast<BYTE*>(Bitmap[1]->GetBitmap()->GetScanLine(
			Bitmap[1]->GetBitmap()->GetHeight() - 1));
		player->SetVideoBuffer(BmpBits[0], BmpBits[1], bmpsize);
		VideoFramesApplied = 0;
		LastPresentedFrame = 0;

		TVPAddLog(TJS_W("[video] player ready ") +
			ttstr(static_cast<tjs_int>(width)) + TJS_W("x") +
			ttstr(static_cast<tjs_int>(height)));
	}

	ClearWndProcMessages();
	SetStatus(tTVPVideoOverlayStatus::Stop);
#endif
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Close()
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	// close
	// release VideoOverlay object
	if(VideoOverlay)
	{
		VideoOverlay->Release(), VideoOverlay = NULL;
		::SetFocus(Window->GetWindowHandle());
	}
	if(LocalTempStorageHolder)
		delete LocalTempStorageHolder, LocalTempStorageHolder = NULL;
	ClearWndProcMessages();
	SetStatus(tTVPVideoOverlayStatus::Unload);

	if( Bitmap[0] )
		delete Bitmap[0];
	if( Bitmap[1] )
		delete Bitmap[1];

	Bitmap[0] = Bitmap[1] = NULL;
	BmpBits[0] = BmpBits[1] = NULL;
#elif defined(__SWITCH__)
	// release the FFmpeg player and the frame bitmaps
	if(VideoOverlay)
	{
		VideoOverlay->Stop();
		ClearWndProcMessages();
		VideoOverlay->Release(), VideoOverlay = NULL;
	}
	else
	{
		EventQueue.Clear(WM_GRAPHNOTIFY);
	}
	if( Bitmap[0] )
		delete Bitmap[0];
	if( Bitmap[1] )
		delete Bitmap[1];
	Bitmap[0] = Bitmap[1] = NULL;
	BmpBits[0] = BmpBits[1] = NULL;
	// the frame buffers are gone; nothing may be presented until the next
	// open publishes its first decoded frame
	LastPresentedFrame = 0;
	ClearWndProcMessages();
	SetStatus(tTVPVideoOverlayStatus::Unload);
#endif
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Shutdown()
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	// shutdown the system
	// this functions closes the overlay object, but must not fire any events.
	bool c = CanDeliverEvents;
	ClearWndProcMessages();
	SetStatus(tTVPVideoOverlayStatus::Unload);
	try
	{
		if(VideoOverlay) VideoOverlay->Release(), VideoOverlay = NULL;
	}
	catch(...)
	{
		CanDeliverEvents = c;
		throw;
	}
	CanDeliverEvents = c;
#elif defined(__SWITCH__)
	bool c = CanDeliverEvents;
	CanDeliverEvents = false;
	if( VideoOverlay )
	{
		VideoOverlay->Stop();
		ClearWndProcMessages();
		VideoOverlay->Release(), VideoOverlay = NULL;
	}
	else
	{
		EventQueue.Clear(WM_GRAPHNOTIFY);
	}
	ClearWndProcMessages();
	SetStatus(tTVPVideoOverlayStatus::Unload);
	CanDeliverEvents = c;
#endif
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Disconnect()
{
	// disconnect the object
	Shutdown();

	Window = NULL;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Play()
{
#ifdef __SWITCH__
	TVPAddLog(TJS_W("[video] VideoOverlay.play"));
	if( VideoOverlay )
	{
		ClearWndProcMessages();
		VideoOverlay->Play();
		SetStatus(tTVPVideoOverlayStatus::Play);
	}
#endif
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	// start playing
	if(VideoOverlay)
	{
		VideoOverlay->Play();
		ClearWndProcMessages();
		if( Mode != vomMFEVR ) SetStatus(tTVPVideoOverlayStatus::Play);
	}
#endif
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Stop()
{
#ifdef __SWITCH__
	TVPAddLog(TJS_W("[video] VideoOverlay.stop"));
	if( VideoOverlay )
	{
		VideoOverlay->Stop();
		ClearWndProcMessages();
		if( Mode != vomMFEVR ) SetStatus(tTVPVideoOverlayStatus::Stop);
	}
#endif
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	// stop playing
	if(VideoOverlay)
	{
		VideoOverlay->Stop();
		ClearWndProcMessages();
		if( Mode != vomMFEVR ) SetStatus(tTVPVideoOverlayStatus::Stop);
	}
#endif
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Pause()
{
#ifdef __SWITCH__
	TVPAddLog(TJS_W("[video] VideoOverlay.pause"));
	if( VideoOverlay )
	{
		VideoOverlay->Pause();
		if( Mode != vomMFEVR ) SetStatus(tTVPVideoOverlayStatus::Pause);
	}
#endif
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	// pause playing
	if(VideoOverlay)
	{
		VideoOverlay->Pause();
//		ClearWndProcMessages();
		if( Mode != vomMFEVR ) SetStatus(tTVPVideoOverlayStatus::Pause);
	}
#endif
}
void tTJSNI_VideoOverlay::Rewind()
{
#ifdef __SWITCH__
	if( VideoOverlay )
	{
		VideoOverlay->Stop();
		ClearWndProcMessages();
		VideoOverlay->Rewind();
		// the player resets its frame counter on rewind; do not blit the
		// previous pass' last frame while the new decode starts
		LastPresentedFrame = 0;
	}
#endif
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	// rewind playing
	if(VideoOverlay)
	{
		VideoOverlay->Rewind();
		ClearWndProcMessages();

		if( EventFrame >= 0 && IsEventPast )
			IsEventPast = false;
	}
#endif
}
void tTJSNI_VideoOverlay::Prepare()
{	// prepare movie
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if( VideoOverlay && (Mode == vomLayer) )
	{
		Pause();
		Rewind();
		IsPrepare = true;
		Play();
	}
#endif
}
void tTJSNI_VideoOverlay::SetSegmentLoop( int comeFrame, int goFrame )
{
	SegLoopStartFrame = comeFrame;
	SegLoopEndFrame = goFrame;
}
void tTJSNI_VideoOverlay::SetPeriodEvent( int eventFrame )
{
	EventFrame = eventFrame;

	if( eventFrame <= GetFrame() )
		IsEventPast = true;
	else
		IsEventPast = false;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetRectangleToVideoOverlay()
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	// set Rectangle to video overlay
	if(VideoOverlay && OwnerWindow)
	{
		tjs_int ofsx, ofsy;
		Window->GetVideoOffset(ofsx, ofsy);
		tjs_int l = Rect.left;
		tjs_int t = Rect.top;
		tjs_int r = Rect.right;
		tjs_int b = Rect.bottom;
		TVPAddLog(TJS_W("Video zoom: (") + ttstr(l) + TJS_W(",") + ttstr(t) + TJS_W(")-(") +
			ttstr(r) + TJS_W(",") + ttstr(b) + TJS_W(") ->"));
		Window->ZoomRectangle(l, t, r, b);
		TVPAddLog(TJS_W("(") + ttstr(l) + TJS_W(",") + ttstr(t) + TJS_W(")-(") +
			ttstr(r) + TJS_W(",") + ttstr(b) + TJS_W(")"));
		RECT rect = {l + ofsx, t + ofsy, r + ofsx, b + ofsy};
		VideoOverlay->SetRect(&rect);
	}
#endif
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetPosition(tjs_int left, tjs_int top)
{
	if( Mode == vomLayer )
	{
		if( Layer1 != NULL ) Layer1->SetPosition( left, top );
		if( Layer2 != NULL ) Layer2->SetPosition( left, top );
	}
	else
	{
		Rect.set_offsets(left, top);
		SetRectangleToVideoOverlay();
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetSize(tjs_int width, tjs_int height)
{
	if( Mode == vomLayer ) return;

	Rect.set_size(width, height);
	SetRectangleToVideoOverlay();
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetBounds(const tTVPRect & rect)
{
	if( Mode == vomLayer ) return;

	Rect = rect;
	SetRectangleToVideoOverlay();
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetLeft(tjs_int l)
{
	if( Mode == vomLayer )
	{
		if( Layer1 != NULL ) Layer1->SetLeft( l );
		if( Layer2 != NULL ) Layer2->SetLeft( l );
	}
	else
	{
		Rect.set_offsets(l, Rect.top);
		SetRectangleToVideoOverlay();
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetTop(tjs_int t)
{
	if( Mode == vomLayer )
	{
		if( Layer1 != NULL ) Layer1->SetTop( t );
		if( Layer2 != NULL ) Layer2->SetTop( t );
	}
	else
	{
		Rect.set_offsets(Rect.left, t);
		SetRectangleToVideoOverlay();
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetWidth(tjs_int w)
{
	if( Mode == vomLayer ) return;

	Rect.right = Rect.left + w;
	SetRectangleToVideoOverlay();
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetHeight(tjs_int h)
{
	if( Mode == vomLayer ) return;

	Rect.bottom = Rect.top + h;
	SetRectangleToVideoOverlay();
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetVisible(bool b)
{
	Visible = b;
#if (defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)) || defined(__SWITCH__)
	if(VideoOverlay)
	{
		if( Mode == vomLayer )
		{
			if( Layer1 != NULL ) Layer1->SetVisible( Visible );
			if( Layer2 != NULL ) Layer2->SetVisible( Visible );
		}
		else
		{
			VideoOverlay->SetVisible(Visible);
		}
	}
#endif
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::ResetOverlayParams()
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	// retrieve new window information from owner window and
	// set video owner window / message drain window.
	// also sets rectangle and visible state.
	if(VideoOverlay && Window && (Mode == vomOverlay || Mode == vomMixer || Mode == vomMFEVR) )
	{
		OwnerWindow = Window->GetWindowHandle();
		VideoOverlay->SetWindow(OwnerWindow);

		VideoOverlay->SetMessageDrainWindow(Window->GetSurfaceWindowHandle());

		// set Rectangle
		SetRectangleToVideoOverlay();

		// set Visible
		VideoOverlay->SetVisible(Visible);
	}
#endif
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::DetachVideoOverlay()
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay && Window && (Mode == vomOverlay || Mode == vomMixer || Mode == vomMFEVR) )
	{
		VideoOverlay->SetWindow(NULL);
		VideoOverlay->SetMessageDrainWindow(EventQueue.GetOwner());
			// once set to util window
	}
#endif
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetRectOffset(tjs_int ofsx, tjs_int ofsy)
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		RECT r = {Rect.left + ofsx, Rect.top + ofsy,
			Rect.right + ofsx, Rect.bottom + ofsy};
		VideoOverlay->SetRect(&r);
	}
#endif
}
//---------------------------------------------------------------------------
//void __fastcall tTJSNI_VideoOverlay::WndProc(Messages::TMessage &Msg)
void tTJSNI_VideoOverlay::WndProc( NativeEvent& ev )
{
#if (defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)) || defined(__SWITCH__)
	// EventQueue's message procedure
	if(VideoOverlay)
	{
		switch(ev.Message) {
		case WM_GRAPHNOTIFY:
		{
			long evcode;
			LONG_PTR p1, p2;
			bool got;
			do {
				VideoOverlay->GetEvent(&evcode, &p1, &p2, &got);
				if( got == false)
					return;

				switch( evcode )
				{
					case EC_COMPLETE:
						if( Status == tTVPVideoOverlayStatus::Play )
						{
							if( Loop )
							{
								Rewind();
								FirePeriodEvent(perLoop); // fire period event by loop rewind
							}
							else
							{
								// Graph manager seems not to complete playing
								// at this point (rewinding the movie at the event
								// handler called asynchronously from SetStatusAsync
								// makes continuing playing, but the graph seems to
								// be unstable).
								// We manually stop the manager anyway.
								VideoOverlay->Stop();
								SetStatusAsync(tTVPVideoOverlayStatus::Stop); // All data has been rendered
							}
						}
						break;
					case EC_UPDATE:
						if( Mode == vomLayer && Status == tTVPVideoOverlayStatus::Play )
						{
							int		curFrame = (int)p1;
							if( Layer1 == NULL && Layer2 == NULL )	// nothing to do.
								return;

							// 2フレーム以上差があるときはGetFrame() を現在のフレームとする
							int frame = GetFrame();
							if( (frame+1) < curFrame || (frame-1) > curFrame )
								curFrame = frame;

							if( (!IsPrepare) && (SegLoopEndFrame > 0) && (frame >= SegLoopEndFrame) ) {
								SetFrame( SegLoopStartFrame > 0 ? SegLoopStartFrame : 0 );
								FirePeriodEvent(perSegLoop); // fire period event by segment loop rewind
								return; // Updateを行わない
							}

							// get video image size
							long	width, height;
							VideoOverlay->GetVideoSize( &width, &height );

							tTJSNI_BaseLayer	*l1 = Layer1;
							tTJSNI_BaseLayer	*l2 = Layer2;

							// Check layer image size
							if( l1 != NULL )
							{
								if( (long)l1->GetImageWidth() != width || (long)l1->GetImageHeight() != height )
									l1->SetImageSize( width, height );
								if( (long)l1->GetWidth() != width || (long)l1->GetHeight() != height )
									l1->SetSize( width, height );
							}
							if( l2 != NULL )
							{
								if( (long)l2->GetImageWidth() != width || (long)l2->GetImageHeight() != height )
									l2->SetImageSize( width, height );
								if( (long)l2->GetWidth() != width || (long)l2->GetHeight() != height )
									l2->SetSize( width, height );
							}
							BYTE *buff;
							VideoOverlay->GetFrontBuffer( &buff );
							if( buff == BmpBits[0] )
							{
								if( l1 ) l1->AssignMainImage( Bitmap[0] );
								if( l2 ) l2->AssignMainImage( Bitmap[0] );
							}
							else	// 0じゃなかったら、1とみなす。
							{
								if( l1 ) l1->AssignMainImage( Bitmap[1] );
								if( l2 ) l2->AssignMainImage( Bitmap[1] );
							}
							if( l1 ) l1->Update();
							if( l2 ) l2->Update();
							#ifdef __SWITCH__
							++VideoFramesApplied;
							if(VideoFramesApplied == 1 || (VideoFramesApplied % 120) == 0)
								KRKRNS_LOG("[video] applied frame=%d slot=%d layers=%d/%d",
									curFrame, buff == BmpBits[0] ? 0 : 1,
									l1 ? 1 : 0, l2 ? 1 : 0);
							#endif
							FireFrameUpdateEvent( curFrame );

							// ! Prepare mode ?
							if( !IsPrepare )
							{
								// Send period event ?
								if( EventFrame >= 0 && !IsEventPast && curFrame >= EventFrame )
								{
									EventFrame = -1;
									FirePeriodEvent(perPeriod); // fire period event by setPeriodEvent()
								}
							}
							else
							{	// Prepare mode
								FirePeriodEvent(perPrepare); // fire period event by prepare()
								Pause();
								Rewind();
								IsPrepare = false;
							}
						}
						else if( Mode == vomMixer && Status == tTVPVideoOverlayStatus::Play )
						{
							int frame = GetFrame();
							if( (!IsPrepare) && (SegLoopEndFrame > 0) && (frame >= SegLoopEndFrame) ) {
								SetFrame( SegLoopStartFrame > 0 ? SegLoopStartFrame : 0 );
								FirePeriodEvent(perSegLoop); // fire period event by segment loop rewind
								return;
							}
							VideoOverlay->PresentVideoImage();
							FireFrameUpdateEvent( frame );
							// Send period event ?
							if( EventFrame >= 0 && !IsEventPast && frame >= EventFrame )
							{
								EventFrame = -1;
								FirePeriodEvent(perPeriod); // fire period event by setPeriodEvent()
							}
						}
						break;
				}
				VideoOverlay->FreeEventParams( evcode, p1, p2 );
			} while( got );
			return;
		}
		case WM_CALLBACKCMD:
		{
			// wparam : command
			// lparam : argument
			FireCallbackCommand((tjs_char*)ev.WParam, (tjs_char*)ev.LParam);
			return;
		}
		case WM_STATE_CHANGE:
			{
				switch( ev.WParam ) {
				case vsStopped:
					SetStatusAsync( tTVPVideoOverlayStatus::Stop );
					break;
				case vsPlaying:
					SetStatusAsync( tTVPVideoOverlayStatus::Play );
					break;
				case vsPaused:
					SetStatusAsync( tTVPVideoOverlayStatus::Pause );
					break;
				case vsReady:
					SetStatusAsync( tTVPVideoOverlayStatus::Ready );
					break;
				case vsEnded:
					if( Status == tTVPVideoOverlayStatus::Play )
					{
						if( Loop )
						{
							VideoOverlay->Play();
							FirePeriodEvent(perLoop); // fire period event by loop rewind
						}
						else
						{
							VideoOverlay->Stop();
							SetStatusAsync(tTVPVideoOverlayStatus::Stop); // All data has been rendered
						}
					}
					break;
				}
				return;
			}
		}
	}

	EventQueue.HandlerDefault(ev);
#endif
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetTimePosition( tjs_uint64 p )
{
#if (defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)) || defined(__SWITCH__)
	if(VideoOverlay)
	{
		VideoOverlay->SetPosition( p );
	}
#endif
}
tjs_uint64 tTJSNI_VideoOverlay::GetTimePosition()
{
	tjs_uint64	result = 0;
#if (defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)) || defined(__SWITCH__)
	if(VideoOverlay)
	{
		unsigned long long position = 0;
		VideoOverlay->GetPosition( &position );
		result = static_cast<tjs_uint64>(position);
	}
#endif
	return result;
}
void tTJSNI_VideoOverlay::SetFrame( tjs_int f )
{
#if (defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)) || defined(__SWITCH__)
	if(VideoOverlay)
	{
		VideoOverlay->SetFrame( f );

		if( EventFrame >= f && IsEventPast )
			IsEventPast = false;
	}
#endif
}
tjs_int tTJSNI_VideoOverlay::GetFrame()
{
	tjs_int	result = 0;
#if (defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)) || defined(__SWITCH__)
	if(VideoOverlay)
	{
		VideoOverlay->GetFrame( &result );
	}
#endif
	return result;
}
void tTJSNI_VideoOverlay::SetStopFrame( tjs_int f )
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->SetStopFrame( f );
	}
#endif
}
void tTJSNI_VideoOverlay::SetDefaultStopFrame()
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->SetDefaultStopFrame();
	}
#endif
}
tjs_int tTJSNI_VideoOverlay::GetStopFrame()
{
	tjs_int	result = 0;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetStopFrame( &result );
	}
#endif
	return result;
}
tjs_real tTJSNI_VideoOverlay::GetFPS()
{
	tjs_real	result = 0.0;
#if (defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)) || defined(__SWITCH__)
	if(VideoOverlay)
	{
		VideoOverlay->GetFPS( &result );
	}
#endif
	return result;
}
tjs_int tTJSNI_VideoOverlay::GetNumberOfFrame()
{
	tjs_int	result = 0;
#if (defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)) || defined(__SWITCH__)
	if(VideoOverlay)
	{
		VideoOverlay->GetNumberOfFrame( &result );
	}
#endif
	return result;
}
tjs_int64 tTJSNI_VideoOverlay::GetTotalTime()
{
	tjs_int64	result = 0;
#if (defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)) || defined(__SWITCH__)
	if(VideoOverlay)
	{
		long long total = 0;
		VideoOverlay->GetTotalTime( &total );
		result = static_cast<tjs_int64>(total);
	}
#endif
	return result;
}
void tTJSNI_VideoOverlay::SetLoop( bool b )
{
	Loop = b;
}
void tTJSNI_VideoOverlay::SetLayer1( tTJSNI_BaseLayer *l )
{
	Layer1 = l;
}
void tTJSNI_VideoOverlay::SetLayer2( tTJSNI_BaseLayer *l )
{
	Layer2 = l;
}
void tTJSNI_VideoOverlay::SetMode( tTVPVideoOverlayMode m )
{
	// ビデオオープン後のモード変更は禁止
	if( !VideoOverlay )
	{
		Mode = m;
	}
}

tjs_real tTJSNI_VideoOverlay::GetPlayRate()
{
	tjs_real	result = 0.0;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetPlayRate( &result );
	}
#endif
	return result;
}
void tTJSNI_VideoOverlay::SetPlayRate(tjs_real r)
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->SetPlayRate( r );
	}
#endif
}

tjs_int tTJSNI_VideoOverlay::GetAudioBalance()
{
	long	result = 0;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetAudioBalance( &result );
	}
#endif
	return TVPDSAttenuateToPan( result );
}
void tTJSNI_VideoOverlay::SetAudioBalance(tjs_int b)
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->SetAudioBalance( TVPPanToDSAttenuate( b ) );
	}
#endif
}
tjs_int tTJSNI_VideoOverlay::GetAudioVolume()
{
	long	result = 0;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetAudioVolume( &result );
	}
#endif
	return TVPDSAttenuateToVolume( result );
}
void tTJSNI_VideoOverlay::SetAudioVolume(tjs_int b)
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->SetAudioVolume( TVPVolumeToDSAttenuate( b ) );
	}
#endif
}
tjs_uint tTJSNI_VideoOverlay::GetNumberOfAudioStream()
{
	unsigned long	result = 0;
#if (defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)) || defined(__SWITCH__)
	if(VideoOverlay)
	{
		VideoOverlay->GetNumberOfAudioStream( &result );
	}
#endif
	return result;
}
void tTJSNI_VideoOverlay::SelectAudioStream(tjs_uint n)
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->SelectAudioStream( n );
	}
#endif
}
tjs_int tTJSNI_VideoOverlay::GetEnabledAudioStream()
{
	long		result = -1;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetEnableAudioStreamNum( &result );
	}
#endif
	return result;
}
void tTJSNI_VideoOverlay::DisableAudioStream()
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->DisableAudioStream();
	}
#endif
}

tjs_uint tTJSNI_VideoOverlay::GetNumberOfVideoStream()
{
	unsigned long	result = 0;
#if (defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)) || defined(__SWITCH__)
	if(VideoOverlay)
	{
		VideoOverlay->GetNumberOfVideoStream( &result );
	}
#endif
	return result;
}
void tTJSNI_VideoOverlay::SelectVideoStream(tjs_uint n)
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->SelectVideoStream( n );
	}
#endif
}
tjs_int tTJSNI_VideoOverlay::GetEnabledVideoStream()
{
	long		result = -1;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetEnableVideoStreamNum( &result );
	}
#endif
	return result;
}
void tTJSNI_VideoOverlay::SetMixingLayer( tTJSNI_BaseLayer *l )
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		if( l )
		{
			if( l->GetVisible() )
			{
				float	alpha = static_cast<float>(l->GetOpacity()) / 255.0f;
				RECT	dest;
				dest.left = l->GetLeft() + l->GetImageLeft();
				dest.top = l->GetTop() + l->GetImageTop();
				dest.right = dest.left + l->GetImageWidth();
				dest.bottom = dest.top + l->GetImageHeight();

				// tTVPBaseBitmap->tTVPBitmap
				tTVPBitmap *bmp = l->GetMainImage()->GetBitmap();
				if( bmp )
				{
					// 自前でDCを作る
					HDC hdc;
					HDC			ref = GetDC(0);
					HBITMAP		myDIB = CreateDIBitmap( ref, bmp->GetBITMAPINFOHEADER(), CBM_INIT, bmp->GetBits(), bmp->GetBITMAPINFO(), bmp->Is8bit() ? DIB_PAL_COLORS : DIB_RGB_COLORS );
					hdc = CreateCompatibleDC( NULL );
					HGDIOBJ		hOldBmp = SelectObject( hdc, myDIB );

					VideoOverlay->SetMixingBitmap( hdc, &dest, alpha );

					SelectObject( hdc, hOldBmp );
					DeleteObject( myDIB );
					DeleteDC( hdc );
				}
			}
			else
			{
				VideoOverlay->ResetMixingBitmap();
			}
		}
		else
		{
			VideoOverlay->ResetMixingBitmap();
		}
	}
#endif
}
void tTJSNI_VideoOverlay::ResetMixingBitmap()
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->ResetMixingBitmap();
	}
#endif
}
void tTJSNI_VideoOverlay::SetMixingMovieAlpha( tjs_real a )
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->SetMixingMovieAlpha( static_cast<float>(a) );
	}
#endif
}
tjs_real tTJSNI_VideoOverlay::GetMixingMovieAlpha()
{
	float	ret = 0.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetMixingMovieAlpha( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
void tTJSNI_VideoOverlay::SetMixingMovieBGColor( tjs_uint col )
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->SetMixingMovieBGColor( col );
	}
#endif
}
tjs_uint tTJSNI_VideoOverlay::GetMixingMovieBGColor()
{
	unsigned long	ret;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetMixingMovieBGColor( &ret );
	}
#endif
	return static_cast<tjs_uint>(ret);
}



tjs_real tTJSNI_VideoOverlay::GetContrastRangeMin()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetContrastRangeMin( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetContrastRangeMax()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetContrastRangeMax( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetContrastDefaultValue()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetContrastDefaultValue( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetContrastStepSize()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetContrastStepSize( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetContrast()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetContrast( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
void tTJSNI_VideoOverlay::SetContrast( tjs_real v )
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->SetContrast( static_cast<float>(v) );
	}
#endif
}
tjs_real tTJSNI_VideoOverlay::GetBrightnessRangeMin()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetBrightnessRangeMin( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetBrightnessRangeMax()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetBrightnessRangeMax( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetBrightnessDefaultValue()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetBrightnessDefaultValue( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetBrightnessStepSize()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetBrightnessStepSize( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetBrightness()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetBrightness( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
void tTJSNI_VideoOverlay::SetBrightness( tjs_real v )
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->SetBrightness( static_cast<float>(v) );
	}
#endif
}

tjs_real tTJSNI_VideoOverlay::GetHueRangeMin()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetHueRangeMin( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetHueRangeMax()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetHueRangeMax( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetHueDefaultValue()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetHueDefaultValue( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetHueStepSize()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetHueStepSize( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetHue()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetHue( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
void tTJSNI_VideoOverlay::SetHue( tjs_real v )
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->SetHue( static_cast<float>(v) );
	}
#endif
}

tjs_real tTJSNI_VideoOverlay::GetSaturationRangeMin()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetSaturationRangeMin( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetSaturationRangeMax()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetSaturationRangeMax( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetSaturationDefaultValue()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetSaturationDefaultValue( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetSaturationStepSize()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetSaturationStepSize( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetSaturation()
{
	float ret = -1.0f;
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->GetSaturation( &ret );
	}
#endif
	return static_cast<tjs_real>(ret);
}
void tTJSNI_VideoOverlay::SetSaturation( tjs_real v )
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	if(VideoOverlay)
	{
		VideoOverlay->SetSaturation( static_cast<float>(v) );
	}
#endif
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_VideoOverlay::GetOriginalWidth()
{
	// retrieve original (coded in the video stream) width size
#if (defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)) || defined(__SWITCH__)
	if(!VideoOverlay) return 0;
#endif

	long	width, height;
#if (defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)) || defined(__SWITCH__)
	VideoOverlay->GetVideoSize( &width, &height );
#else
	width = 0;
#endif

	return (tjs_int)width;
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_VideoOverlay::GetOriginalHeight()
{
	// retrieve original (coded in the video stream) height size
	if(!VideoOverlay) return 0;

	long	width, height;
#if (defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)) || defined(__SWITCH__)
	VideoOverlay->GetVideoSize( &width, &height );
#else
	height = 0;
#endif

	return (tjs_int)height;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::ClearWndProcMessages()
{
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY)
	// clear WndProc's message queue
	MSG msg;
	while(PeekMessage(&msg, EventQueue.GetOwner(), WM_GRAPHNOTIFY, WM_GRAPHNOTIFY+2, PM_REMOVE))
	{
		if(VideoOverlay)
		{
			long evcode;
			LONG_PTR p1, p2;
			bool got;
			VideoOverlay->GetEvent(&evcode, &p1, &p2, &got); // dummy call
			if( got )
				VideoOverlay->FreeEventParams( evcode, p1, p2 );
		}
	}
#elif defined(__SWITCH__)
	// The FFmpeg worker queues its payload in SwitchMovieOverlay and posts one
	// coalesced NativeEvent as a main-thread wake-up.  Clear both halves only
	// while the worker is stopped (all Switch callers enforce that ordering).
	EventQueue.Clear(WM_GRAPHNOTIFY);
	if(VideoOverlay)
		static_cast<SwitchMovieOverlay *>(VideoOverlay)->ClearEvents();
#endif
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// tTJSNC_VideoOverlay::CreateNativeInstance : returns proper instance object
//---------------------------------------------------------------------------
tTJSNativeInstance *tTJSNC_VideoOverlay::CreateNativeInstance()
{
	return new tTJSNI_VideoOverlay();
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// TVPCreateNativeClass_VideoOverlay
//---------------------------------------------------------------------------
tTJSNativeClass * TVPCreateNativeClass_VideoOverlay()
{
	return new tTJSNC_VideoOverlay();
}
//---------------------------------------------------------------------------

#ifdef __SWITCH__
//---------------------------------------------------------------------------
// Overlay / mixer mode presentation.
//
// The win32 implementation leaves these modes to DirectShow's own video
// window (SetWindow/SetRect on the overlay object); Kirikiroid2 draws them as
// a sprite above the scene.  Neither exists on the Switch, so TickBeat blits
// the decoded front buffer into the window's compose surface after the layer
// tree has been composited and before the texture upload -- above the scene,
// which is where a win32 overlay video appears.  Layer-mode movies return
// false here: they reach the screen through the layer tree as before.
//---------------------------------------------------------------------------
static bool TVPVideoBlitScaled(SDL_Surface *surface, const void *bits,
	int bitmapWidth, int bitmapHeight, int srcW, int srcH,
	int dx, int dy, int dw, int dh, SDL_Rect &copied)
{
	copied = SDL_Rect{0, 0, 0, 0};
	if(!surface || !surface->format || surface->format->BytesPerPixel != 4)
		return false;
	const int bmpH = bitmapHeight < 0 ? -bitmapHeight : bitmapHeight;
	if(!bits || bitmapWidth <= 0 || bmpH <= 0 ||
		srcW <= 0 || srcH <= 0 || dw <= 0 || dh <= 0 ||
		srcW > bitmapWidth || srcH > bmpH)
		return false;

	const SDL_Rect requested = {dx, dy, dw, dh};
	const SDL_Rect bounds = {0, 0, surface->w, surface->h};
	if(!SDL_IntersectRect(&requested, &bounds, &copied) || copied.w <= 0 || copied.h <= 0)
		return false;

	// same DIB layout convention as TVPCopyBitmapToSurface: positive height
	// means bottom-up memory, so the logical top row sits at the end.
	const std::ptrdiff_t stride = std::ptrdiff_t(bitmapWidth) * 4;
	const std::ptrdiff_t pitch = bitmapHeight < 0 ? stride : -stride;
	const uint8_t *top = static_cast<const uint8_t *>(bits);
	if(bitmapHeight > 0) top += std::ptrdiff_t(bmpH - 1) * stride;

	const bool locked = SDL_MUSTLOCK(surface);
	if(locked && SDL_LockSurface(surface) != 0)
		return false;
	for(int row = 0; row < copied.h; ++row)
	{
		const int sy = (int)((int64_t)(copied.y + row - dy) * srcH / dh);
		const uint8_t *src = top + std::ptrdiff_t(sy) * pitch;
		uint8_t *dst = static_cast<uint8_t *>(surface->pixels) +
			std::ptrdiff_t(copied.y + row) * surface->pitch +
			std::ptrdiff_t(copied.x) * 4;
		for(int col = 0; col < copied.w; ++col)
		{
			const int sx = (int)((int64_t)(copied.x + col - dx) * srcW / dw);
			SDL_memcpy(dst + col * 4, src + std::ptrdiff_t(sx) * 4, 4);
		}
	}
	if(locked)
		SDL_UnlockSurface(surface);
	return true;
}
//---------------------------------------------------------------------------
static bool TVPOverlayIsPresentable(tTJSNI_VideoOverlay *ovl)
{
	return ovl && ovl->IsPresentable();
}
//---------------------------------------------------------------------------
bool tTJSNI_VideoOverlay::IsPresentable() const
{
	// mirrors PresentFrameToSurface's gates minus the pixel work
	if(!VideoOverlay || Mode == vomLayer)
		return false;
	int frame = 0;
	VideoOverlay->GetFrame(&frame);
	tTVPVideoStatus playerStatus = vsStopped;
	VideoOverlay->GetStatus(&playerStatus);
	// Throttled diagnostic: an overlay-mode movie that never presents is a
	// black screen with sound, and only this line says which gate held it
	// back (visible / status / frame counter / missing buffers / rect).
	static Uint32 lastProbe = 0;
	const Uint32 nowTick = SDL_GetTicks();
	if(nowTick - lastProbe >= 1000)
	{
		lastProbe = nowTick;
		KRKRNS_LOG("[video] overlay state: visible=%d status=%d player=%d frame=%d last=%d bmp=%d/%d rect=(%d,%d)-(%d,%d)",
			(int)Visible, (int)Status, (int)playerStatus, frame,
			(int)LastPresentedFrame, Bitmap[0] ? 1 : 0, Bitmap[1] ? 1 : 0,
			Rect.left, Rect.top, Rect.right, Rect.bottom);
	}
	if(!Visible)
		return false;
	if(Status != tTVPVideoOverlayStatus::Play &&
		Status != tTVPVideoOverlayStatus::Pause)
		return false;
	if(!Bitmap[0] || !Bitmap[1])
		return false;
	return frame != LastPresentedFrame;
}
//---------------------------------------------------------------------------
bool tTJSNI_VideoOverlay::PresentFrameToSurface(SDL_Surface *surface, SDL_Rect &dirty)
{
	dirty = SDL_Rect{0, 0, 0, 0};
	if(!surface || !VideoOverlay)
		return false;
	if(Mode == vomLayer) // composited by the layer tree
		return false;
	if(!Visible)
		return false;
	if(Status != tTVPVideoOverlayStatus::Play &&
		Status != tTVPVideoOverlayStatus::Pause) // keeps the last frame on pause
		return false;
	if(!Bitmap[0] || !Bitmap[1])
		return false;

	int movieFrame = 0;
	VideoOverlay->GetFrame(&movieFrame);
	if(movieFrame == LastPresentedFrame)
		return false; // no new decoded frame since the last blit

	BYTE *buff = NULL;
	VideoOverlay->GetFrontBuffer(&buff);
	tTVPBaseBitmap *frame = NULL;
	if(buff && buff == BmpBits[0]) frame = Bitmap[0];
	else if(buff && buff == BmpBits[1]) frame = Bitmap[1];
	if(!frame)
		return false;
	tTVPBitmap *bmp = frame->GetBitmap();
	if(!bmp || !bmp->Is32bit())
		return false;

	long vw = 0, vh = 0;
	VideoOverlay->GetVideoSize(&vw, &vh);
	const BitmapInfomation *info = bmp->GetBitmapInfomation();
	if(vw <= 0 || vh <= 0 || !info || !info->GetBITMAPINFO())
		return false;

	const int dw = Rect.get_width();
	const int dh = Rect.get_height();
	SDL_Rect copied;
	bool drawn;
	if(dw == (int)vw && dh == (int)vh)
		drawn = TVPCopyBitmapToSurface(surface, bmp->GetBits(),
			(int)vw, info->GetBITMAPINFO()->bmiHeader.biHeight,
			SDL_Rect{0, 0, (int)vw, (int)vh}, Rect.left, Rect.top, copied);
	else
		drawn = TVPVideoBlitScaled(surface, bmp->GetBits(),
			(int)vw, info->GetBITMAPINFO()->bmiHeader.biHeight,
			(int)vw, (int)vh, Rect.left, Rect.top, dw, dh, copied);

	if(drawn)
	{
		dirty = copied;
		LastPresentedFrame = movieFrame;
		++VideoFramesApplied;
		if(VideoFramesApplied == 1 || (VideoFramesApplied % 120) == 0)
			KRKRNS_LOG("[video] overlay frame=%u mode=%d rect=(%d,%d)-(%d,%d) vid=%ldx%ld surface=%dx%d",
				(unsigned)VideoFramesApplied, (int)Mode,
				Rect.left, Rect.top, Rect.right, Rect.bottom,
				vw, vh, surface->w, surface->h);
	}
	return drawn;
}
//---------------------------------------------------------------------------
// GPU-composite sibling of PresentFrameToSurface: hands the decoded frame's
// pixels + destination rect to the caller (the glc compositor draws it as a
// quad inside the compose FBO) instead of blitting into the CPU surface.
// Consumes the frame (LastPresentedFrame) exactly like the CPU path so the
// two can never both present the same frame.
bool krkrsdl2_video_overlay_take_frame(const void **bits, int *bw, int *bh, int *pitch,
	int *dx, int *dy, int *dw, int *dh, bool *bottomup)
{
	if(bits) *bits = nullptr;
	if(bw) *bw = 0;
	if(bh) *bh = 0;
	if(pitch) *pitch = 0;
	if(dx) *dx = 0;
	if(dy) *dy = 0;
	if(dw) *dw = 0;
	if(dh) *dh = 0;
	if(bottomup) *bottomup = false;
	for(size_t i = 0; i < TVPVideoOverlayVector.size(); ++i)
	{
		tTJSNI_VideoOverlay *ov = TVPVideoOverlayVector[i];
		if(!ov)
			continue;
		if(ov->TakeFrameForGpu(bits, bw, bh, pitch, dx, dy, dw, dh, bottomup))
			return true;
	}
	return false;
}
//---------------------------------------------------------------------------
// The GPU-composite twin of PresentFrameToSurface: same gates, but instead of
// blitting into the CPU surface it hands out the frame's pixels + destination
// rect and consumes the frame (LastPresentedFrame) so the CPU and GPU paths
// can never both present the same decoded frame.
bool tTJSNI_VideoOverlay::TakeFrameForGpu(const void **bits, int *bw, int *bh, int *pitch,
	int *dx, int *dy, int *dw, int *dh, bool *bottomup)
{
	if(!VideoOverlay)
		return false;
	if(Mode == vomLayer) // composited by the layer tree
		return false;
	if(!Visible)
		return false;
	if(Status != tTVPVideoOverlayStatus::Play &&
		Status != tTVPVideoOverlayStatus::Pause) // keeps the last frame on pause
		return false;
	if(!Bitmap[0] || !Bitmap[1])
		return false;

	int movieFrame = 0;
	VideoOverlay->GetFrame(&movieFrame);
	if(movieFrame == LastPresentedFrame)
		return false; // no new decoded frame since the last present

	BYTE *buff = NULL;
	VideoOverlay->GetFrontBuffer(&buff);
	tTVPBaseBitmap *frame = NULL;
	if(buff && buff == BmpBits[0]) frame = Bitmap[0];
	else if(buff && buff == BmpBits[1]) frame = Bitmap[1];
	if(!frame)
		return false;
	tTVPBitmap *bmp = frame->GetBitmap();
	if(!bmp || !bmp->Is32bit())
		return false;

	long vw = 0, vh = 0;
	VideoOverlay->GetVideoSize(&vw, &vh);
	const BitmapInfomation *info = bmp->GetBitmapInfomation();
	if(vw <= 0 || vh <= 0 || !info || !info->GetBITMAPINFO())
		return false;

	if(bits) *bits = bmp->GetBits();
	if(bw) *bw = (int)vw;
	if(bh) *bh = (int)vh;
	if(pitch) *pitch = frame->GetPitchBytes();
	if(dx) *dx = Rect.left;
	if(dy) *dy = Rect.top;
	if(dw) *dw = Rect.get_width();
	if(dh) *dh = Rect.get_height();
	if(bottomup) *bottomup = info->GetBITMAPINFO()->bmiHeader.biHeight > 0;

	LastPresentedFrame = movieFrame;
	++VideoFramesApplied;
	if(VideoFramesApplied == 1 || (VideoFramesApplied % 120) == 0)
		KRKRNS_LOG("[video] overlay gpu frame=%u mode=%d rect=(%d,%d)-(%d,%d) vid=%ldx%ld",
			(unsigned)VideoFramesApplied, (int)Mode,
			Rect.left, Rect.top, Rect.right, Rect.bottom, vw, vh);
	return true;
}
//---------------------------------------------------------------------------
bool krkrsdl2_video_overlay_present(SDL_Surface *surface, SDL_Rect *dirty)
{
	if(dirty)
		*dirty = SDL_Rect{0, 0, 0, 0};
	if(!surface)
		return false;
	bool any = false;
	SDL_Rect acc = {0, 0, 0, 0};
	for(size_t i = 0; i < TVPVideoOverlayVector.size(); ++i)
	{
		SDL_Rect r;
		if(!TVPVideoOverlayVector[i]->PresentFrameToSurface(surface, r))
			continue;
		if(!any)
		{
			acc = r;
			any = true;
		}
		else
		{
			const int l = std::min(acc.x, r.x);
			const int t = std::min(acc.y, r.y);
			const int rr = std::max(acc.x + acc.w, r.x + r.w);
			const int bb = std::max(acc.y + acc.h, r.y + r.h);
			acc = SDL_Rect{l, t, rr - l, bb - t};
		}
	}
	if(any && dirty)
		*dirty = acc;
	return any;
}
//---------------------------------------------------------------------------
bool krkrsdl2_video_overlay_pending()
{
	for(size_t i = 0; i < TVPVideoOverlayVector.size(); ++i)
	{
		if(TVPOverlayIsPresentable(TVPVideoOverlayVector[i]))
			return true;
	}
	return false;
}
//---------------------------------------------------------------------------
void TVPClearVideoOverlays()
{
	// Engine-restart path: the script engine is rebuilt inside this process,
	// so a video overlay the old session leaked must not stay in the registry
	// (the present pump would dereference it through the next session).
	TVPVideoOverlayVector.clear();
}
//---------------------------------------------------------------------------
#endif // __SWITCH__

