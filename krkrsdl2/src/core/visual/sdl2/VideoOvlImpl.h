//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Video Overlay support implementation
//---------------------------------------------------------------------------
#ifndef VideoOvlImplH
#define VideoOvlImplH
//---------------------------------------------------------------------------
#include "tjsNative.h"
#include "WindowIntf.h"

#include "VideoOvlIntf.h"
#include "StorageIntf.h"
#include "UtilStreams.h"

#include "voMode.h"

#include "NativeEventQueue.h"

#ifdef __SWITCH__
struct SDL_Surface;
struct SDL_Rect;
#endif

//---------------------------------------------------------------------------
// tTJSNI_VideoOverlay : VideoOverlay Native Instance
//---------------------------------------------------------------------------
class iTVPVideoOverlay;
class tTJSNI_VideoOverlay : public tTJSNI_BaseVideoOverlay
{
	typedef tTJSNI_BaseVideoOverlay inherited;

	iTVPVideoOverlay *VideoOverlay;

	tTVPRect Rect;
	bool Visible;

#ifdef _WIN32
	HWND OwnerWindow;
#endif

	// HWND UtilWindow; // window which receives messages from video overlay object
	NativeEventQueue<tTJSNI_VideoOverlay> EventQueue;

	tTVPLocalTempStorageHolder *LocalTempStorageHolder;
	class tTJSNI_BaseLayer	*Layer1;
	class tTJSNI_BaseLayer	*Layer2;
	tTVPVideoOverlayMode	Mode;	//!< Modeの動的な変更は出来ない。open前にセットしておくこと
	bool	Loop;

#ifdef _WIN32
	class tTVPBaseBitmap	*Bitmap[2];	//!< Layer描画用バッファ用Bitmap
	BYTE			*BmpBits[2];
#elif defined(__SWITCH__)
	// Phase 4: same frame-buffer pair for the FFmpeg player
	class tTVPBaseBitmap	*Bitmap[2];
	unsigned char		*BmpBits[2];
	tjs_uint		VideoFramesApplied;
	// Movie frame number already blitted into the compose surface, so a
	// present only happens when the decoder actually published a new frame
	// (the pump runs at display rate, the movie at its own fps).
	tjs_int			LastPresentedFrame;
#endif

	bool	IsPrepare;			//!< 準備モードかどうか

	int		SegLoopStartFrame;	//!< セグメントループ開始フレーム
	int		SegLoopEndFrame;	//!< セグメントループ終了フレーム

	//! イベントが設定された時、現在フレームの方が進んでいたかどうか。
	//! イベントが設定されているフレームより前に現在フレームが移動した時、このフラグは解除される。
	bool	IsEventPast;
	int		EventFrame;		//!< イベントを発生させるフレーム

public:
	tTJSNI_VideoOverlay();
	~tTJSNI_VideoOverlay();
	tjs_error TJS_INTF_METHOD Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *tjs_obj);
	void TJS_INTF_METHOD Invalidate();


public:
	void Open(const ttstr &name);
	void Close();
	void Shutdown();
	void Disconnect(); // tTJSNI_BaseVideoOverlay::Disconnect override

	void Play();
	void Stop();
	void Pause();
	void Rewind();
	void Prepare();

	void SetSegmentLoop( int comeFrame, int goFrame );
	void CancelSegmentLoop() { SegLoopStartFrame = -1; SegLoopEndFrame = -1; }
	void SetPeriodEvent( int eventFrame );

	void SetStopFrame( tjs_int f );
	void SetDefaultStopFrame();
	tjs_int GetStopFrame();

public:
	void SetRectangleToVideoOverlay();

	void SetPosition(tjs_int left, tjs_int top);
	void SetSize(tjs_int width, tjs_int height);
	void SetBounds(const tTVPRect & rect);

	void SetLeft(tjs_int l);
	tjs_int GetLeft() const { return Rect.left; }
	void SetTop(tjs_int t);
	tjs_int GetTop() const { return Rect.top; }
	void SetWidth(tjs_int w);
	tjs_int GetWidth() const { return Rect.get_width(); }
	void SetHeight(tjs_int h);
	tjs_int GetHeight() const { return Rect.get_height(); }

	void SetVisible(bool b);
	bool GetVisible() const { return Visible; }

	void SetTimePosition( tjs_uint64 p );
	tjs_uint64 GetTimePosition();

	void SetFrame( tjs_int f );
	tjs_int GetFrame();

	tjs_real GetFPS();
	tjs_int GetNumberOfFrame();
	tjs_int64 GetTotalTime();

	void SetLoop( bool b );
	bool GetLoop() const { return Loop; }

	void SetLayer1( tTJSNI_BaseLayer *l );
	tTJSNI_BaseLayer *GetLayer1() { return Layer1; }
	void SetLayer2( tTJSNI_BaseLayer *l );
	tTJSNI_BaseLayer *GetLayer2() { return Layer2; }

	void SetMode( tTVPVideoOverlayMode m );
	tTVPVideoOverlayMode GetMode() { return Mode; }

	tjs_real GetPlayRate();
	void SetPlayRate(tjs_real r);

	tjs_int GetSegmentLoopStartFrame() { return SegLoopStartFrame; }
	tjs_int GetSegmentLoopEndFrame() { return SegLoopEndFrame; }
	tjs_int GetPeriodEventFrame() { return EventFrame; }

	tjs_int GetAudioBalance();
	void SetAudioBalance(tjs_int b);
	tjs_int GetAudioVolume();
	void SetAudioVolume(tjs_int v);

	tjs_uint GetNumberOfAudioStream();
	void SelectAudioStream(tjs_uint n);
	tjs_int GetEnabledAudioStream();
	void DisableAudioStream();

	tjs_uint GetNumberOfVideoStream();
	void SelectVideoStream(tjs_uint n);
	tjs_int GetEnabledVideoStream();
	void SetMixingLayer( tTJSNI_BaseLayer *l );
	void ResetMixingBitmap();

	void SetMixingMovieAlpha( tjs_real a );
	tjs_real GetMixingMovieAlpha();
	void SetMixingMovieBGColor( tjs_uint col );
	tjs_uint GetMixingMovieBGColor();


	tjs_real GetContrastRangeMin();
	tjs_real GetContrastRangeMax();
	tjs_real GetContrastDefaultValue();
	tjs_real GetContrastStepSize();
	tjs_real GetContrast();
	void SetContrast( tjs_real v );

	tjs_real GetBrightnessRangeMin();
	tjs_real GetBrightnessRangeMax();
	tjs_real GetBrightnessDefaultValue();
	tjs_real GetBrightnessStepSize();
	tjs_real GetBrightness();
	void SetBrightness( tjs_real v );

	tjs_real GetHueRangeMin();
	tjs_real GetHueRangeMax();
	tjs_real GetHueDefaultValue();
	tjs_real GetHueStepSize();
	tjs_real GetHue();
	void SetHue( tjs_real v );

	tjs_real GetSaturationRangeMin();
	tjs_real GetSaturationRangeMax();
	tjs_real GetSaturationDefaultValue();
	tjs_real GetSaturationStepSize();
	tjs_real GetSaturation();
	void SetSaturation( tjs_real v );

	tjs_int GetOriginalWidth();
	tjs_int GetOriginalHeight();

	void ResetOverlayParams();
	void SetRectOffset(tjs_int ofsx, tjs_int ofsy);
	void DetachVideoOverlay();

#ifdef __SWITCH__
	// Overlay/mixer modes have no DirectShow video window on Switch: TickBeat
	// draws the current front buffer into the compose surface instead (see
	// the comment on the implementation).  Layer-mode movies return false --
	// they reach the screen through the layer tree.  `dirty` receives the
	// touched surface rect.
	bool PresentFrameToSurface(SDL_Surface *surface, SDL_Rect &dirty);
	// GPU-composite twin: hands out the frame's pixels + destination rect and
	// consumes it, so the glc compositor can draw it as a quad inside the
	// compose FBO (mode 5) instead of blitting into the CPU surface.
	bool TakeFrameForGpu(const void **bits, int *bw, int *bh, int *pitch,
		int *dx, int *dy, int *dw, int *dh, bool *bottomup);
	// Would PresentFrameToSurface draw right now?  Cheap probe (no pixel
	// work), used to decide whether the frame needs an upload at all.
	bool IsPresentable() const;
#endif

private:
	void WndProc( NativeEvent& ev );
		// UtilWindow's window procedure
	void ClearWndProcMessages(); // clear WndProc's message queue

};
//---------------------------------------------------------------------------

#ifdef __SWITCH__
// Draws every visible overlay-mode movie above the composed scene, in the
// order the game created them.  Called from TVPWindowWindow::TickBeat with
// the window's compose surface.  Returns true when anything was drawn and
// fills `dirty` (when non-null) with the touched surface rect, so the caller
// can force the frame to be uploaded even when the layer tree reported no
// damage (a movie over a static scene produces no layer notification).
bool krkrsdl2_video_overlay_present(SDL_Surface *surface, SDL_Rect *dirty);
// GPU-composite variant: fills the frame's pixels/size/pitch and its
// destination rect (canvas coords) and consumes the frame.  Used by the glc
// compositor so an overlay movie reaches the screen without mixing the raw
// GL swap with the SDL renderer's present path.
bool krkrsdl2_video_overlay_take_frame(const void **bits, int *bw, int *bh, int *pitch,
	int *dx, int *dy, int *dw, int *dh, bool *bottomup);
// Cheap probe for the same condition (no pixel work): lets TickBeat decide
// whether to enter its upload branch before doing the actual blit.
bool krkrsdl2_video_overlay_pending();
// Engine restart: forget every registered overlay.  The registry is
// process-global while the script engine is rebuilt in place, so a game's
// overlay must not survive into the next (launcher) session.
void TVPClearVideoOverlays();
#endif

#endif
