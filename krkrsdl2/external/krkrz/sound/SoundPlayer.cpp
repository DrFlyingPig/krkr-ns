//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Sound Player for QueueSoundBuffer
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "MsgIntf.h"
#ifdef __SWITCH__
#include "KrkrNSLog.h"
#else
#include <cstdio>
#endif
#include "SoundPlayer.h"
#include "QueueSoundBufferImpl.h"
#include "SoundSamples.h"

//---------------------------------------------------------------------------
tTVPSoundPlayer::tTVPSoundPlayer( tTJSNI_QueueSoundBuffer* owner )
 : Owner( owner ), Stream(nullptr), Paused(false), Playing(false), PlayStopPos(-1) {
	memset( &StreamFormat, 0, sizeof( StreamFormat ) );
}
//---------------------------------------------------------------------------
tTVPSoundPlayer::~tTVPSoundPlayer() {
}
//---------------------------------------------------------------------------
void tTVPSoundPlayer::PushSamplesBuffer( tTVPSoundSamplesBuffer* buf ) {
	tTJSCriticalSectionHolder holder(Owner->GetBufferCS());
	Samples.push_back( buf );
	if( !BufferEnded ) {
		BufferEnded = buf->IsEnded();
		if( BufferEnded ) {
			PlayStopPos = buf->GetDecodePosition() + buf->GetInSamples();
		}
	}
	if( Stream ) buf->Enqueue( Stream );
}
//---------------------------------------------------------------------------
void tTVPSoundPlayer::Callback( class iTVPAudioStream* stream ) {
	tTVPSoundSamplesBuffer* sample = nullptr;
	{
		tTJSCriticalSectionHolder holder(Owner->GetBufferCS());
		if( Samples.size() > 0 ) {
			auto itr = Samples.begin();
			sample = *itr;
			Samples.erase( itr );
		}
	}
	if( sample ) Owner->ReleasePlayedSamples( sample, !BufferEnded );
}
//---------------------------------------------------------------------------
void tTVPSoundPlayer::CreateStream( iTVPAudioDevice* device, tTVPWaveFormat& format, tjs_uint samplesCount ) {
	if( Stream ) delete Stream;

	tTVPAudioStreamParam param;
	param.Channels = format.Channels;		// チャンネル数
	param.SampleRate = format.SamplesPerSec;		// サンプリングレート
	param.BitsPerSample = format.BitsPerSample;	// サンプル当たりのビット数
	param.SampleType = astUInt8;
	if( format.IsFloat ) {
		param.SampleType = astFloat32;	// サンプルの形式
	} else if( param.BitsPerSample == 8 ) {
		param.SampleType = astUInt8;
	} else if( param.BitsPerSample == 16 ) {
		param.SampleType = astInt16;
	} else {
		TVPThrowExceptionMessage(TJS_W("Invalid format(BitsPerSample)."));
	}
	param.FramesPerBuffer = samplesCount;		// 1回のキューイングで入れるサンプル数
	Stream = device->CreateAudioStream( param );
	if( Stream == nullptr ) {
		TVPThrowExceptionMessage(TJS_W("Faild to create audio stream."));
	}
	StreamFormat = format;
	Stream->SetCallback( StreamCallback, this );
}
//---------------------------------------------------------------------------
tjs_int64 tTVPSoundPlayer::GetCurrentPlayingPosition() {
	tjs_int64 result = -1;
	if( Stream ) {
		tTJSCriticalSectionHolder holder(Owner->GetBufferCS());
		tjs_uint64 pos = Stream->GetSamplesPlayed();
		if( Samples.size() > 0 ) {
			auto itr = Samples.begin();
			tTVPSoundSamplesBuffer* sample = *itr;
			tjs_uint count = sample->GetSamplesCount();
			tjs_int offset = (tjs_int)( pos % count );
			result = sample->GetDecodePosition() + offset;
		}
	}
	return result;
}
//---------------------------------------------------------------------------
tjs_uint64 tTVPSoundPlayer::GetSamplePosition() {
	tjs_uint64 result = 0;
	if( Stream ) {
		tTJSCriticalSectionHolder holder(Owner->GetBufferCS());
		tjs_uint64 pos = Stream->GetSamplesPlayed();
		if( Samples.size() > 0 ) {
			auto itr = Samples.begin();
			tTVPSoundSamplesBuffer* sample = *itr;
			tjs_uint count = sample->GetSamplesCount();
			tjs_int offset = (tjs_int)( pos % count );
			result = sample->GetSegmentQueue().FilteredPositionToDecodePosition( offset );
		}
	}
	return result;
}
//---------------------------------------------------------------------------
void tTVPSoundPlayer::CopyVisBuffer(tjs_int16 *dest, const tjs_uint8 *src,
	tjs_int numsamples, tjs_int channels)
{
	if(channels == 1) {
		TVPConvertPCMTo16bits(dest, (const void*)src, StreamFormat.Channels,
			StreamFormat.BytesPerSample, StreamFormat.BitsPerSample,
			StreamFormat.IsFloat, numsamples, true);
	} else if(channels == StreamFormat.Channels) {
		TVPConvertPCMTo16bits(dest, (const void*)src, StreamFormat.Channels,
			StreamFormat.BytesPerSample, StreamFormat.BitsPerSample,
			StreamFormat.IsFloat, numsamples, false);
	}
}
//---------------------------------------------------------------------------
tjs_int tTVPSoundPlayer::GetVisBuffer(tjs_int16 *dest, tjs_int numsamples, tjs_int channels, tjs_int aheadsamples ) {
	tjs_int writtensamples = 0;
	tjs_uint blockAlign = StreamFormat.BytesPerSample * StreamFormat.Channels;
	if( Stream ) {
		tTJSCriticalSectionHolder holder(Owner->GetBufferCS());
		tjs_uint64 pos = Stream->GetSamplesPlayed();
		if( Samples.size() > 0 ) {
			auto itr = Samples.begin();
			if( (*itr)->GetSegmentQueue().GetFilteredLength() == 0 ) return 0;
			tTVPSoundSamplesBuffer* sample = *itr;
			tjs_int count = static_cast<tjs_int>(sample->GetSamplesCount());
			if( count <= 0 ) return 0;
			// KRKR-ns: normalise the read offset BEFORE slicing.  The original
			// loop only subtracted one buffer length per iteration, which is
			// only correct while aheadsamples < count; getSample titles ask for
			// ~8800 samples ahead and short buffers could leave the offset out
			// of range.  A missing visualization buffer (a queued buffer created
			// before useVisBuffer was enabled) is skipped instead of read.
			tjs_int offset = (tjs_int)( pos % (tjs_uint64)count ) + aheadsamples;
			while( offset >= count ) offset -= count;
			if( offset < 0 ) offset = 0;
			for( auto i = Samples.begin(); i != Samples.end(); i++ ) {
				const tjs_uint8* vis = (*i)->GetVisBuffer();
				if( !vis ) continue;
				tjs_int bufrest = count - offset;
				tjs_int copysamples = (bufrest > numsamples ? numsamples : bufrest);
				CopyVisBuffer(dest, vis + offset * blockAlign, copysamples, channels);
				numsamples -= copysamples;
				writtensamples += copysamples;
				if(numsamples <= 0) break;

				dest += channels * copysamples;
				offset = 0;
			}
		}
	}
	return writtensamples;
}
//---------------------------------------------------------------------------
void tTVPSoundPlayer::Start() {
	if(!Paused) {
		Stream->StartStream();
		Playing = true;
	}
}
//---------------------------------------------------------------------------
void tTVPSoundPlayer::Stop() {
	if( Stream ) Stream->StopStream();
	Playing = false;
}
//---------------------------------------------------------------------------
void tTVPSoundPlayer::Reset() {
	BufferEnded = false;
	PlayStopPos = -1;
}
//---------------------------------------------------------------------------
void tTVPSoundPlayer::Clear() {
	tTJSCriticalSectionHolder holder(Owner->GetBufferCS());
	Paused = false;
	Playing = false;
	PlayStopPos = -1;
	Samples.clear();
}
//---------------------------------------------------------------------------
void tTVPSoundPlayer::ClearSampleQueue() {
	tTJSCriticalSectionHolder holder(Owner->GetBufferCS());
	if( Stream ) Stream->ClearQueue();
	Samples.clear();
}
//---------------------------------------------------------------------------
void tTVPSoundPlayer::Destroy(bool resetFormat) {
	iTVPAudioStream* retired;
	{
		tTJSCriticalSectionHolder holder(Owner->GetBufferCS());
		retired = Stream;
		Stream = nullptr;
		Playing = false;
		if( resetFormat ) memset( &StreamFormat, 0, sizeof( StreamFormat ) );
	}
	// DestroyVoice waits for its last callback, which also needs BufferCS.
	// Keep the owner and sample buffers alive, but let that callback finish.
	delete retired;
}
//---------------------------------------------------------------------------
void tTVPSoundPlayer::SetVolume(tjs_int v) {
	if( Stream ) Stream->SetVolume( v );
}
//---------------------------------------------------------------------------
void tTVPSoundPlayer::SetPan(tjs_int v) {
	if( Stream ) Stream->SetPan( v );
}
//---------------------------------------------------------------------------
void tTVPSoundPlayer::SetFrequency(tjs_int freq) {
	if(Stream) Stream->SetFrequency( freq );
}
//---------------------------------------------------------------------------
void tTVPSoundPlayer::SetPsused( bool paused ) {
	tTJSCriticalSectionHolder holder(Owner->GetBufferCS());
	Playing = Paused;
}
//---------------------------------------------------------------------------
bool tTVPSoundPlayer::Update() {
	bool continued = true;
	if( Paused ) {
		if( Playing ) {
			Stream->StopStream();
			Playing = false;
		}
		return false;
	} else {
		if( !Playing ) {
			Stream->StartStream();
			Playing = true;
		}
	}
	if( PlayStopPos != -1 ) {
		tjs_uint64 samplesPlayed = Stream->GetSamplesPlayed();
		tjs_uint32 queued = Stream->GetQueuedCount();
		// Some backends reset the sample counter at EOS. Zero also occurs
		// before a newly started voice has consumed its first queued buffer,
		// so only an exhausted queue can use the zero-counter EOS fallback.
		if( PlayStopPos <= (tjs_int64)(samplesPlayed) ||
			(samplesPlayed == 0 && queued == 0) ) {
			// Update runs on the audio event thread. Keep diagnostics away
			// from the main thread's TJS log objects and console buffer.
#ifdef __SWITCH__
			KRKRNS_LOG("[audio-eos] natural stop samplesPlayed=%llu stopPos=%lld queued=%u",
				(unsigned long long)samplesPlayed, (long long)PlayStopPos, (unsigned)queued);
#else
			std::fprintf(stderr, "[audio-eos] natural stop samplesPlayed=%llu stopPos=%lld queued=%u\n",
				(unsigned long long)samplesPlayed, (long long)PlayStopPos, (unsigned)queued);
#endif
			Stream->StopStream();
			Playing = false;
			continued = false;
		}
	}
	return continued;
}
//---------------------------------------------------------------------------
