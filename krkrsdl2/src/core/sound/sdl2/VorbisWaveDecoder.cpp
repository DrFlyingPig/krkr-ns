//---------------------------------------------------------------------------
// Built-in Ogg Vorbis decoder for platforms that cannot load wuvorbis.dll.
// The decoder follows Kirikiroid2's stream-backed implementation, but keeps
// ownership and callback failures contained at the C library boundary.
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#include <vorbis/vorbisfile.h>

#include "DebugIntf.h"
#include "StorageIntf.h"
#include "WaveIntf.h"
#include "VorbisWaveDecoder.h"

namespace
{

class tTVPVorbisWaveDecoder final : public tTVPWaveDecoder
{
	OggVorbis_File VorbisFile;
	tTJSBinaryStream *InputStream;
	tTVPWaveFormat Format;
	bool VorbisFileInitialized;
	int CurrentSection;

public:
	tTVPVorbisWaveDecoder()
		: InputStream(nullptr), VorbisFileInitialized(false), CurrentSection(-1)
	{
		std::memset(&VorbisFile, 0, sizeof(VorbisFile));
		std::memset(&Format, 0, sizeof(Format));
	}

	~tTVPVorbisWaveDecoder() override
	{
		if(VorbisFileInitialized)
		{
			// ov_clear invokes CloseCallback and releases InputStream.
			ov_clear(&VorbisFile);
			VorbisFileInitialized = false;
		}
		if(InputStream)
		{
			delete InputStream;
			InputStream = nullptr;
		}
	}

	bool Open(const ttstr &storagename)
	{
		InputStream = TVPCreateStream(storagename, TJS_BS_READ);
		if(!InputStream) return false;

		ov_callbacks callbacks = {
			ReadCallback,
			SeekCallback,
			CloseCallback,
			TellCallback
		};

		const int open_result = ov_open_callbacks(this, &VorbisFile, nullptr, 0, callbacks);
		if(open_result < 0)
		{
			delete InputStream;
			InputStream = nullptr;
			return false;
		}
		VorbisFileInitialized = true;

		vorbis_info *info = ov_info(&VorbisFile, -1);
		if(!info || info->channels <= 0 || info->rate <= 0) return false;

		Format.SamplesPerSec = static_cast<tjs_uint>(info->rate);
		Format.Channels = static_cast<tjs_uint>(info->channels);
		Format.BitsPerSample = 16;
		Format.BytesPerSample = 2;
		Format.IsFloat = false;
		Format.SpeakerConfig = 0;
		Format.Seekable = ov_seekable(&VorbisFile) != 0;

		const ogg_int64_t total_samples = ov_pcm_total(&VorbisFile, -1);
		Format.TotalSamples = total_samples > 0
			? static_cast<tjs_uint64>(total_samples)
			: 0;
		Format.TotalTime = Format.TotalSamples > 0
			? (Format.TotalSamples * 1000) / Format.SamplesPerSec
			: 0;

		TVPAddLog(
			ttstr(TJS_W("[audio] built-in Vorbis opened: ")) + storagename +
			TJS_W(" (") + ttstr(static_cast<tjs_int>(Format.SamplesPerSec)) +
			TJS_W(" Hz, ") + ttstr(static_cast<tjs_int>(Format.Channels)) +
			TJS_W(" ch)"));
		return true;
	}

	void GetFormat(tTVPWaveFormat &format) override
	{
		format = Format;
	}

	bool Render(void *buffer, tjs_uint requested_samples, tjs_uint &rendered) override
	{
		rendered = 0;
		if(!VorbisFileInitialized || !buffer || requested_samples == 0) return false;

		const size_t frame_bytes = static_cast<size_t>(Format.Channels) * sizeof(tjs_int16);
		size_t remaining_bytes = static_cast<size_t>(requested_samples) * frame_bytes;
		char *output = static_cast<char *>(buffer);

		while(remaining_bytes > 0)
		{
			const size_t chunk = std::min(
				remaining_bytes,
				static_cast<size_t>(std::numeric_limits<int>::max()));
			const long result = ov_read(
				&VorbisFile,
				output,
				static_cast<int>(chunk),
				0,
				2,
				1,
				&CurrentSection);

			if(result == OV_HOLE) continue;
			if(result <= 0) break;

			output += result;
			remaining_bytes -= static_cast<size_t>(result);
		}

		const size_t decoded_bytes =
			static_cast<size_t>(requested_samples) * frame_bytes - remaining_bytes;
		rendered = static_cast<tjs_uint>(decoded_bytes / frame_bytes);
		return rendered == requested_samples;
	}

	bool SetPosition(tjs_uint64 sample_position) override
	{
		if(!VorbisFileInitialized || !Format.Seekable) return false;
		if(sample_position > static_cast<tjs_uint64>(std::numeric_limits<ogg_int64_t>::max()))
			return false;
		return ov_pcm_seek(&VorbisFile, static_cast<ogg_int64_t>(sample_position)) == 0;
	}

private:
	static size_t ReadCallback(void *ptr, size_t size, size_t count, void *datasource)
	{
		if(size == 0 || count == 0) return 0;
		auto *decoder = static_cast<tTVPVorbisWaveDecoder *>(datasource);
		if(!decoder || !decoder->InputStream) return 0;

		const size_t requested = size * count;
		const tjs_uint bounded = requested > std::numeric_limits<tjs_uint>::max()
			? std::numeric_limits<tjs_uint>::max()
			: static_cast<tjs_uint>(requested);
		try
		{
			return decoder->InputStream->Read(ptr, bounded) / size;
		}
		catch(...)
		{
			return 0;
		}
	}

	static int SeekCallback(void *datasource, ogg_int64_t offset, int origin)
	{
		auto *decoder = static_cast<tTVPVorbisWaveDecoder *>(datasource);
		if(!decoder || !decoder->InputStream) return -1;

		int whence;
		switch(origin)
		{
		case SEEK_SET: whence = TJS_BS_SEEK_SET; break;
		case SEEK_CUR: whence = TJS_BS_SEEK_CUR; break;
		case SEEK_END: whence = TJS_BS_SEEK_END; break;
		default: return -1;
		}

		try
		{
			decoder->InputStream->Seek(static_cast<tjs_int64>(offset), whence);
			return 0;
		}
		catch(...)
		{
			return -1;
		}
	}

	static int CloseCallback(void *datasource)
	{
		auto *decoder = static_cast<tTVPVorbisWaveDecoder *>(datasource);
		if(!decoder || !decoder->InputStream) return 0;
		delete decoder->InputStream;
		decoder->InputStream = nullptr;
		return 0;
	}

	static long TellCallback(void *datasource)
	{
		auto *decoder = static_cast<tTVPVorbisWaveDecoder *>(datasource);
		if(!decoder || !decoder->InputStream) return -1;
		try
		{
			const tjs_uint64 position = decoder->InputStream->GetPosition();
			if(position > static_cast<tjs_uint64>(std::numeric_limits<long>::max()))
				return -1;
			return static_cast<long>(position);
		}
		catch(...)
		{
			return -1;
		}
	}
};

class tTVPVorbisWaveDecoderCreator final : public tTVPWaveDecoderCreator
{
public:
	tTVPWaveDecoder *Create(const ttstr &storagename, const ttstr &extension) override
	{
		if(extension != TJS_W(".ogg")) return nullptr;

		auto *decoder = new tTVPVorbisWaveDecoder();
		if(decoder->Open(storagename)) return decoder;
		delete decoder;
		return nullptr;
	}
};

} // namespace

void TVPRegisterVorbisWaveDecoderCreator()
{
	static tTVPVorbisWaveDecoderCreator creator;
	static bool registered = false;
	if(registered) return;
	TVPRegisterWaveDecoderCreator(&creator);
	registered = true;
}
