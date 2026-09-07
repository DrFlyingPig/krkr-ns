#include "tjsCommHead.h"
#include "GraphicsLoaderIntf.h"
#include "MsgIntf.h"
#include "tjsDictionary.h"

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
	std::vector<uint8_t> decoded(stride * static_cast<size_t>(height));
	uint8_t *decodeResult = nullptr;
	if (mode == glmNormalRGBA)
		decodeResult = WebPDecodeRGBAInto(data.data(), data.size(), decoded.data(), decoded.size(), static_cast<int>(stride));
	else
		decodeResult = WebPDecodeBGRAInto(data.data(), data.size(), decoded.data(), decoded.size(), static_cast<int>(stride));
	if (!decodeResult)
		TVPThrowExceptionMessage(TVPImageLoadError, TJS_W("WebP decode failed"));

	for (int y = 0; y < height; ++y)
	{
		void *destination = scanlinecallback(callbackdata, y);
		if (!destination) break;
		const uint8_t *row = decoded.data() + static_cast<size_t>(y) * stride;
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
