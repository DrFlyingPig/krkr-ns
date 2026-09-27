//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Sound Buffer Base implementation
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <algorithm>
#include "SoundBufferBaseImpl.h"
#ifdef WIN32
#endif
#if defined(TVP_COMPILING_KRKRSDL2) || defined(WIN32)
#include "WaveImpl.h"
#endif
#if 0
#ifdef ANDROID
extern void TVPWaveSoundBufferCommitSettings();
#endif
#endif

#include "TVPTimer.h"

//---------------------------------------------------------------------------
// Sound Buffer Timer Dispatcher ( for fading or commiting defered settings )
//---------------------------------------------------------------------------
static class TVPTimer * TVPSoundBufferTimer = NULL;
static std::vector<tTJSNI_SoundBuffer *> TVPSoundBufferVector;
//---------------------------------------------------------------------------
class tTVPSoundBufferTimerDispatcher
{
public:
	void Handler()
	{
		std::vector<tTJSNI_SoundBuffer *>::iterator i;
		for(i = TVPSoundBufferVector.begin(); i != TVPSoundBufferVector.end();
			i++)
		{
			(*i)->TimerBeatHandler();
		}
		TVPWaveSoundBufferCommitSettings();	// for DirectSound(Windows7)
	}
} static TVPSoundBufferTimerDispatcher;
//---------------------------------------------------------------------------
void TVPResetSoundBufferTimerForEngineRestart()
{
	// Script shutdown can leave native sound objects alive until a later
	// finalization. Their process-global dispatcher must not retain the timer
	// whose registration is lost when the session's TimerThread is destroyed.
	// Clear this session's registry so its late Invalidate calls cannot remove
	// a rebuilt session's timer or receive that session's timer beats.
	TVPTimer *timer = TVPSoundBufferTimer;
	TVPSoundBufferTimer = NULL;
	TVPSoundBufferVector.clear();
	delete timer;
}
//---------------------------------------------------------------------------
void TVPAddSoundBuffer(tTJSNI_SoundBuffer * buf)
{
	if(!TVPSoundBufferTimer)
	{
		// first buffer
		TVPSoundBufferTimer = new TVPTimer(); // Create Timer Object
		TVPSoundBufferTimer->SetInterval( TVP_SB_BEAT_INTERVAL );
		TVPSoundBufferTimer->SetOnTimerHandler( &TVPSoundBufferTimerDispatcher, &tTVPSoundBufferTimerDispatcher::Handler );
		TVPSoundBufferTimer->SetEnabled( true );
	}

	TVPSoundBufferVector.push_back(buf);
}
//---------------------------------------------------------------------------
void TVPRemoveSoundBuffer(tTJSNI_SoundBuffer *buf)
{
	std::vector<tTJSNI_SoundBuffer *>::iterator i;
	i = std::find(TVPSoundBufferVector.begin(), TVPSoundBufferVector.end(), buf);
	if(i == TVPSoundBufferVector.end()) return;
	TVPSoundBufferVector.erase(i);

	if(TVPSoundBufferVector.size() == 0)
	{
		// all buffer was removed
		delete TVPSoundBufferTimer;
		TVPSoundBufferTimer = NULL;
	}
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// tTJSNI_SoundBuffer
//---------------------------------------------------------------------------
tTJSNI_SoundBuffer::tTJSNI_SoundBuffer()
{
}
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD tTJSNI_SoundBuffer::Construct(tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 *tjs_obj)
{
	tjs_error hr = inherited::Construct(numparams, param, tjs_obj);
	if(TJS_FAILED(hr)) return hr;

	TVPAddSoundBuffer(this);

	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_SoundBuffer::Invalidate()
{
	TVPRemoveSoundBuffer(this);

	inherited::Invalidate();
}
//---------------------------------------------------------------------------

