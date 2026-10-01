//---------------------------------------------------------------------------
// Kirikiroid2's stream-backed FFmpeg wave fallback, adapted to the current
// send/receive API. Keep the decoder's sample rate, channels and PCM precision;
// only planar samples are interleaved for the engine's wave interface.
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
}

#include "DebugIntf.h"
#include "StorageIntf.h"
#include "WaveIntf.h"
#include "FFWaveDecoder.h"

namespace
{

class tTVPFFWaveDecoder final : public tTVPWaveDecoder
{
	std::unique_ptr<tTJSBinaryStream> InputStream;
	AVIOContext *IO = nullptr;
	AVFormatContext *Container = nullptr;
	AVCodecContext *Codec = nullptr;
	AVPacket *Packet = nullptr;
	AVFrame *Frame = nullptr;
	AVStream *AudioStream = nullptr;
	tTVPWaveFormat Format = {};
	AVSampleFormat OutputFormat = AV_SAMPLE_FMT_NONE;
	int StreamIndex = -1;
	int FrameOffset = 0;
	int64_t FramePosition = 0;
	int64_t NextFramePosition = 0;
	int64_t FirstPacketTimestamp = AV_NOPTS_VALUE;
	bool PacketPending = false;
	bool Draining = false;

public:
	~tTVPFFWaveDecoder() override
	{
		av_packet_free(&Packet);
		av_frame_free(&Frame);
		avcodec_free_context(&Codec);
		avformat_close_input(&Container);
		if(IO)
		{
			// libavformat may replace the original AVIO buffer.
			av_freep(&IO->buffer);
			avio_context_free(&IO);
		}
	}

	bool Open(const ttstr &storagename)
	{
		InputStream.reset(TVPCreateStream(storagename, TJS_BS_READ));
		if(!InputStream) return false;

		constexpr int buffer_size = 32 * 1024;
		auto *buffer = static_cast<unsigned char *>(av_malloc(buffer_size));
		if(!buffer) return false;
		IO = avio_alloc_context(buffer, buffer_size, 0, this,
			ReadCallback, nullptr, SeekCallback);
		if(!IO)
		{
			av_free(buffer);
			return false;
		}

		Container = avformat_alloc_context();
		if(!Container) return false;
		Container->pb = IO;
		Container->flags |= AVFMT_FLAG_CUSTOM_IO;
		// An empty filename makes the probe depend on storage contents, even
		// when a package keeps .ogg on an MP4/AAC voice file.
		if(avformat_open_input(&Container, "", nullptr, nullptr) < 0 ||
			avformat_find_stream_info(Container, nullptr) < 0) return false;

		StreamIndex = av_find_best_stream(Container, AVMEDIA_TYPE_AUDIO,
			-1, -1, nullptr, 0);
		if(StreamIndex < 0) return false;
		AudioStream = Container->streams[StreamIndex];
		if(!OpenCodec()) return false;

		Packet = av_packet_alloc();
		Frame = av_frame_alloc();
		if(!Packet || !Frame || !DecodeNextFrame()) return false;
		OutputFormat = av_get_packed_sample_fmt(static_cast<AVSampleFormat>(Frame->format));
		if(OutputFormat != AV_SAMPLE_FMT_S16 && OutputFormat != AV_SAMPLE_FMT_S32 &&
			OutputFormat != AV_SAMPLE_FMT_FLT) return false;
		if(Frame->sample_rate <= 0 || Frame->ch_layout.nb_channels <= 0) return false;

		Format.SamplesPerSec = Frame->sample_rate;
		Format.Channels = Frame->ch_layout.nb_channels;
		Format.BytesPerSample = av_get_bytes_per_sample(OutputFormat);
		Format.BitsPerSample = Format.BytesPerSample * 8;
		Format.IsFloat = OutputFormat == AV_SAMPLE_FMT_FLT;
		Format.Seekable = (IO->seekable & AVIO_SEEKABLE_NORMAL) != 0;
		if(AudioStream->duration != AV_NOPTS_VALUE && AudioStream->duration > 0)
		{
			Format.TotalSamples = av_rescale_q(AudioStream->duration,
				AudioStream->time_base, AVRational{1, static_cast<int>(Format.SamplesPerSec)});
			Format.TotalTime = av_rescale_q(AudioStream->duration,
				AudioStream->time_base, AVRational{1, 1000});
		}
		else if(Container->duration != AV_NOPTS_VALUE && Container->duration > 0)
		{
			Format.TotalSamples = av_rescale_q(Container->duration,
				AVRational{1, AV_TIME_BASE}, AVRational{1, static_cast<int>(Format.SamplesPerSec)});
			Format.TotalTime = Container->duration / (AV_TIME_BASE / 1000);
		}

		TVPAddLog(ttstr(TJS_W("[audio] built-in FFmpeg opened: ")) + storagename +
			TJS_W(" (") + ttstr(static_cast<tjs_int>(Format.SamplesPerSec)) +
			TJS_W(" Hz, ") + ttstr(static_cast<tjs_int>(Format.Channels)) + TJS_W(" ch)"));
		return true;
	}

	void GetFormat(tTVPWaveFormat &format) override { format = Format; }

	bool Render(void *buffer, tjs_uint requested_samples, tjs_uint &rendered) override
	{
		rendered = 0;
		if(!Codec || !Frame || (!buffer && requested_samples)) return false;
		auto *output = static_cast<unsigned char *>(buffer);
		const size_t frame_bytes = static_cast<size_t>(Format.Channels) * Format.BytesPerSample;
		while(rendered < requested_samples)
		{
			if(FrameOffset >= Frame->nb_samples && !DecodeNextFrame()) break;
			if(Frame->sample_rate != static_cast<int>(Format.SamplesPerSec) ||
				Frame->ch_layout.nb_channels != static_cast<int>(Format.Channels) ||
				av_get_packed_sample_fmt(static_cast<AVSampleFormat>(Frame->format)) != OutputFormat)
				break;
			const tjs_uint samples = std::min(requested_samples - rendered,
				static_cast<tjs_uint>(Frame->nb_samples - FrameOffset));
			if(Format.Channels > 1 && av_sample_fmt_is_planar(static_cast<AVSampleFormat>(Frame->format)))
			{
				for(tjs_uint i = 0; i < samples; ++i)
					for(tjs_uint channel = 0; channel < Format.Channels; ++channel)
					{
						std::memcpy(output, Frame->extended_data[channel] +
							static_cast<size_t>(FrameOffset + i) * Format.BytesPerSample,
							Format.BytesPerSample);
						output += Format.BytesPerSample;
					}
			}
			else
			{
				std::memcpy(output, Frame->extended_data[0] +
					static_cast<size_t>(FrameOffset) * frame_bytes, samples * frame_bytes);
				output += samples * frame_bytes;
			}
			FrameOffset += samples;
			rendered += samples;
		}
		return rendered == requested_samples;
	}

	bool SetPosition(tjs_uint64 sample_position) override
	{
		if(!Codec || !Container || !Format.Seekable ||
			sample_position > static_cast<tjs_uint64>(std::numeric_limits<int64_t>::max()))
			return false;
		const int64_t target_sample = static_cast<int64_t>(sample_position);
		int64_t timestamp = av_rescale_q(target_sample,
			AVRational{1, static_cast<int>(Format.SamplesPerSec)}, AudioStream->time_base);
		const int64_t stream_start = AudioStream->start_time == AV_NOPTS_VALUE
			? 0 : AudioStream->start_time;
		if(timestamp > std::numeric_limits<int64_t>::max() - std::max<int64_t>(stream_start, 0))
			return false;
		timestamp += stream_start;
		// Decode preceding frames before trimming to the requested sample,
		// rebuilding codec overlap after resetting the codec's state.
		const int64_t preroll = av_rescale_q(std::max<int64_t>(static_cast<int64_t>(Codec->frame_size) * 2, 1),
			AVRational{1, static_cast<int>(Format.SamplesPerSec)}, AudioStream->time_base);
		// Remember the first packet, including negative AAC priming timestamps.
		// Seeking to zero in the container can otherwise omit the codec history
		// needed to reproduce the first audible frame.
		const int64_t first_timestamp = FirstPacketTimestamp == AV_NOPTS_VALUE
			? stream_start : FirstPacketTimestamp;
		const int64_t seek_timestamp = sample_position == 0 ? first_timestamp
			: std::max(first_timestamp, timestamp - preroll);
		if(avformat_seek_file(Container, StreamIndex,
			std::numeric_limits<int64_t>::min(), seek_timestamp, seek_timestamp,
			AVSEEK_FLAG_BACKWARD) < 0) return false;
		// Recreate the codec as well as clearing queued frames. AAC's flush
		// retains some prediction/noise state, which changes a replayed voice.
		avcodec_free_context(&Codec);
		if(!OpenCodec()) return false;
		av_packet_unref(Packet);
		av_frame_unref(Frame);
		PacketPending = false;
		Draining = false;
		FrameOffset = 0;
		NextFramePosition = av_rescale_q(seek_timestamp - stream_start,
			AudioStream->time_base, AVRational{1, static_cast<int>(Format.SamplesPerSec)});
		while(DecodeNextFrame())
		{
			const int64_t end = FramePosition + Frame->nb_samples;
			if(end <= target_sample) continue;
			FrameOffset = static_cast<int>(std::max<int64_t>(0, target_sample - FramePosition));
			return true;
		}
		return Format.TotalSamples != 0 && sample_position == Format.TotalSamples;
	}

private:
	bool OpenCodec()
	{
		const AVCodec *decoder = avcodec_find_decoder(AudioStream->codecpar->codec_id);
		if(!decoder) return false;
		Codec = avcodec_alloc_context3(decoder);
		if(!Codec || avcodec_parameters_to_context(Codec, AudioStream->codecpar) < 0)
		{
			avcodec_free_context(&Codec);
			return false;
		}
		Codec->pkt_timebase = AudioStream->time_base;
		if(avcodec_open2(Codec, decoder, nullptr) < 0)
		{
			avcodec_free_context(&Codec);
			return false;
		}
		return true;
	}

	bool DecodeNextFrame()
	{
		av_frame_unref(Frame);
		for(;;)
		{
			const int received = avcodec_receive_frame(Codec, Frame);
			if(received == 0)
			{
				FramePosition = NextFramePosition;
				if(Frame->best_effort_timestamp != AV_NOPTS_VALUE && Frame->sample_rate > 0)
				{
					const int64_t start = AudioStream->start_time == AV_NOPTS_VALUE
						? 0 : AudioStream->start_time;
					FramePosition = av_rescale_q(Frame->best_effort_timestamp - start,
						AudioStream->time_base, AVRational{1, Frame->sample_rate});
				}
				NextFramePosition = FramePosition + Frame->nb_samples;
				FrameOffset = 0;
				return true;
			}
			if(received != AVERROR(EAGAIN) || Draining) return false;
			if(PacketPending)
			{
				const int sent = avcodec_send_packet(Codec, Packet);
				if(sent == AVERROR(EAGAIN)) continue;
				av_packet_unref(Packet);
				PacketPending = false;
				// Match K2's handling of a bad packet: try the next frame.
				continue;
			}
			int read;
			for(;;)
			{
				read = av_read_frame(Container, Packet);
				if(read < 0 || Packet->stream_index == StreamIndex) break;
				av_packet_unref(Packet);
			}
			if(read < 0)
			{
				const int sent = avcodec_send_packet(Codec, nullptr);
				if(sent != AVERROR(EAGAIN)) Draining = true;
			}
			else
			{
				if(FirstPacketTimestamp == AV_NOPTS_VALUE)
					FirstPacketTimestamp = Packet->dts != AV_NOPTS_VALUE ? Packet->dts : Packet->pts;
				PacketPending = true;
			}
		}
	}

	static int ReadCallback(void *opaque, unsigned char *buffer, int size)
	{
		auto *decoder = static_cast<tTVPFFWaveDecoder *>(opaque);
		try
		{
			const tjs_uint read = decoder->InputStream->Read(buffer, static_cast<tjs_uint>(size));
			return read ? static_cast<int>(read) : AVERROR_EOF;
		}
		catch(...) { return AVERROR(EIO); }
	}

	static int64_t SeekCallback(void *opaque, int64_t offset, int origin)
	{
		auto *decoder = static_cast<tTVPFFWaveDecoder *>(opaque);
		try
		{
			if(origin & AVSEEK_SIZE) return decoder->InputStream->GetSize();
			int whence;
			switch(origin & ~AVSEEK_FORCE)
			{
			case SEEK_SET: whence = TJS_BS_SEEK_SET; break;
			case SEEK_CUR: whence = TJS_BS_SEEK_CUR; break;
			case SEEK_END: whence = TJS_BS_SEEK_END; break;
			default: return AVERROR(EINVAL);
			}
			return decoder->InputStream->Seek(offset, whence);
		}
		catch(...) { return AVERROR(EIO); }
	}
};

class tTVPFFWaveDecoderCreator final : public tTVPWaveDecoderCreator
{
public:
	tTVPWaveDecoder *Create(const ttstr &storagename, const ttstr &) override
	{
		auto decoder = std::make_unique<tTVPFFWaveDecoder>();
		if(!decoder->Open(storagename)) return nullptr;
		return decoder.release();
	}
};

} // namespace

void TVPRegisterFFWaveDecoderCreator()
{
	static tTVPFFWaveDecoderCreator creator;
	TVPRegisterWaveDecoderCreator(&creator);
}
