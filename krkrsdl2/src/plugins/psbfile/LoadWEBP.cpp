#include "tjsCommHead.h"
#include "GraphicsLoaderIntf.h"
#include "MsgIntf.h"
#include "tjsDictionary.h"
#ifdef __SWITCH__
#include "KrkrNSLog.h"
#endif
#ifdef __SWITCH__
#include "ThreadIntf.h"
#endif

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include <webp/decode.h>

namespace
{
std::vector<uint8_t> ReadRemaining(tTJSBinaryStream *source)
{
	const tjs_uint64 position = source->GetPosition();
	const tjs_uint64 total = source->GetSize();
	if (position > total || total - position > std::numeric_limits<tjs_uint>::max())
		TVPThrowExceptionMessage(TVPImageLoadError, TJS_W("Invalid WebP stream size"));
	std::vector<uint8_t> data(static_cast<size_t>(total - position));
	if (!data.empty()) source->ReadBuffer(data.data(), static_cast<tjs_uint>(data.size()));
	return data;
}
}

void TVPLoadWEBP(void *, void *callbackdata,
	tTVPGraphicSizeCallback sizecallback,
	tTVPGraphicScanLineCallback scanlinecallback,
	tTVPMetaInfoPushCallback,
	tTJSBinaryStream *source, tjs_int,
	tTVPGraphicLoadMode mode)
{
	const std::vector<uint8_t> data = ReadRemaining(source);
	int width = 0;
	int height = 0;
	if (data.empty() || !WebPGetInfo(data.data(), data.size(), &width, &height) ||
		width <= 0 || height <= 0 || width >= 65536 || height >= 65536)
		TVPThrowExceptionMessage(TVPImageLoadError, TJS_W("Invalid WebP image"));

	sizecallback(callbackdata, static_cast<tjs_uint>(width), static_cast<tjs_uint>(height));

	if (mode == glmPalettized)
		TVPThrowExceptionMessage(TVPImageLoadError, TJS_W("WebP does not support palettized loading"));

	const size_t stride = static_cast<size_t>(width) * 4;
	if (static_cast<size_t>(height) > std::numeric_limits<size_t>::max() / stride)
		TVPThrowExceptionMessage(TVPImageLoadError, TJS_W("Invalid WebP output size"));
	const size_t decodedBytes = stride * static_cast<size_t>(height);
	// libwebp fills every output pixel before we expose a scanline. Avoid a
	// redundant full-image zero pass, but retain the tight temporary: callback
	// rows are not required to be contiguous or to have a particular pitch.
	std::unique_ptr<uint8_t[]> decoded(new uint8_t[decodedBytes]);
	const auto decode = [&](bool threaded) {
		WebPDecoderConfig config;
		if (!WebPInitDecoderConfig(&config))
			TVPThrowExceptionMessage(TVPImageLoadError, TJS_W("WebP decoder initialization failed"));
		struct DecoderBufferScope {
			WebPDecBuffer *buffer;
			~DecoderBufferScope() { WebPFreeDecBuffer(buffer); }
		} decoderBufferScope{&config.output};
		config.output.colorspace = mode == glmNormalRGBA ? MODE_RGBA : MODE_BGRA;
		config.output.is_external_memory = 1;
		config.output.u.RGBA.rgba = decoded.get();
		config.output.u.RGBA.stride = static_cast<int>(stride);
		config.output.u.RGBA.size = decodedBytes;
		config.options.use_threads = threaded;
		return WebPDecode(data.data(), data.size(), &config);
	};
	bool threaded = false;
#ifdef __SWITCH__
	// KRKR-ns (2026-11-05): libwebp's decoder worker threads are disabled here.
	// A deterministic crash in one Yuzusoft title loads a 1 MB WebP character
	// sheet right after several TLG loads; the log reached the RIFF signature and
	// then the process died with an invalid guest PC, which is the signature of a
	// worker thread the engine cannot join safely.  Decoding stays correct and
	// only a few milliseconds slower per large image.
	threaded = false;
	(void)TVPGetProcessorNum();
#endif
	VP8StatusCode status = decode(threaded);
	// A worker cannot always be created under memory/thread pressure. Retain
	// the old single-thread decoder as recovery, with fresh decoder state and
	// the same private output. No partial pixels reach callbacks on failure.
	if (threaded && status == VP8_STATUS_OUT_OF_MEMORY) status = decode(false);
	if (status != VP8_STATUS_OK)
		TVPThrowExceptionMessage(TVPImageLoadError, TJS_W("WebP decode failed"));

	for (int y = 0; y < height; ++y)
	{
		void *destination = scanlinecallback(callbackdata, y);
		if (!destination) break;
		const uint8_t *row = decoded.get() + static_cast<size_t>(y) * stride;
		if (mode == glmGrayscale)
		{
			uint8_t *gray = static_cast<uint8_t *>(destination);
			for (int x = 0; x < width; ++x)
			{
				const uint8_t *pixel = row + static_cast<size_t>(x) * 4;
				// decoded is BGRA in grayscale mode; coefficients sum to 256.
				gray[x] = static_cast<uint8_t>((pixel[0] * 19u + pixel[1] * 183u + pixel[2] * 54u) >> 8);
			}
		}
		else
		{
			std::copy(row, row + stride, static_cast<uint8_t *>(destination));
		}
		scanlinecallback(callbackdata, -1);
	}
}

void TVPLoadHeaderWEBP(void *, tTJSBinaryStream *source, iTJSDispatch2 **dictionary)
{
	const std::vector<uint8_t> data = ReadRemaining(source);
	WebPBitstreamFeatures features;
	if (data.empty() || WebPGetFeatures(data.data(), data.size(), &features) != VP8_STATUS_OK)
		TVPThrowExceptionMessage(TVPImageLoadError, TJS_W("Invalid WebP image"));

	*dictionary = TJSCreateDictionaryObject();
	tTJSVariant value(static_cast<tjs_int32>(features.width));
	(*dictionary)->PropSet(TJS_MEMBERENSURE, TJS_W("width"), nullptr, &value, *dictionary);
	value = static_cast<tjs_int32>(features.height);
	(*dictionary)->PropSet(TJS_MEMBERENSURE, TJS_W("height"), nullptr, &value, *dictionary);
	value = static_cast<tjs_int32>(features.has_alpha ? 32 : 24);
	(*dictionary)->PropSet(TJS_MEMBERENSURE, TJS_W("bpp"), nullptr, &value, *dictionary);
}
