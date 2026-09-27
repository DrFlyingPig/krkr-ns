// CPU platform adaptation of krkrsdl3/plugins/LayerExRaster.cpp.
// See LICENSE.krkrsdl3 for the upstream license.
#include "ncbind/ncbind.hpp"
#include "RasterCopy.h"

#define NCB_MODULE_NAME TJS_W("layerExRaster.dll")

// Keep the ncbind registration unit when linked from krkrsdl2common's archive.
extern "C" void krkrsdl2_link_layerexraster_plugin() {}

namespace {

tTJSVariant LayerProperty(iTJSDispatch2* layer, const tjs_char* name)
{
    tTJSVariant value;
    if (!layer || TJS_FAILED(layer->PropGet(0, name, nullptr, &value, layer)))
        TVPThrowExceptionMessage(TJS_W("copyRaster: cannot read Layer property"), name);
    return value;
}

void RequireLayer(iTJSDispatch2* layer)
{
    if (!layer || layer->IsInstanceOf(0, nullptr, nullptr, TJS_W("Layer"), layer) != TJS_S_TRUE)
        TVPThrowExceptionMessage(TJS_W("copyRaster: source and destination must be Layer"));
}

tjs_error copyRaster(tTJSVariant* result, tjs_int numparams,
                     tTJSVariant** param, iTJSDispatch2* destination)
{
    if (numparams < 5) return TJS_E_BADPARAMCOUNT;
    iTJSDispatch2* source = param[0]->AsObjectNoAddRef();
    RequireLayer(destination);
    RequireLayer(source);
    const int width = static_cast<tjs_int>(LayerProperty(source, TJS_W("imageWidth")));
    const int height = static_cast<tjs_int>(LayerProperty(source, TJS_W("imageHeight")));
    // The reference quietly leaves the destination alone on size mismatch.
    if (width != static_cast<tjs_int>(LayerProperty(destination, TJS_W("imageWidth"))) ||
        height != static_cast<tjs_int>(LayerProperty(destination, TJS_W("imageHeight"))))
        return TJS_S_OK;

    const int amplitude = static_cast<tjs_int>(*param[1]);
    const int lines = static_cast<tjs_int>(*param[2]);
    const int cycle = static_cast<tjs_int>(*param[3]);
    const tjs_int64 time = static_cast<tjs_int64>(*param[4]);
    if (!lines || !cycle)
        TVPThrowExceptionMessage(TJS_W("copyRaster: lines and cycle must be nonzero"));

    // Request writable storage before reading the source. This ensures that
    // copy-on-write has completed, including when both objects are identical.
    auto* destinationPixels = reinterpret_cast<std::uint8_t*>(static_cast<tjs_int64>(
        LayerProperty(destination, TJS_W("mainImageBufferForWrite"))));
    const auto* sourcePixels = reinterpret_cast<const std::uint8_t*>(static_cast<tjs_int64>(
        LayerProperty(source, TJS_W("mainImageBuffer"))));
    const tjs_int destinationPitch = static_cast<tjs_int>(LayerProperty(destination, TJS_W("mainImageBufferPitch")));
    const tjs_int sourcePitch = static_cast<tjs_int>(LayerProperty(source, TJS_W("mainImageBufferPitch")));
    if (!krkrns::CopyRaster(destinationPixels, destinationPitch, sourcePixels, sourcePitch,
                           width, height, amplitude, lines, cycle, time))
        TVPThrowExceptionMessage(TJS_W("copyRaster: invalid Layer image buffer"));
    // Upstream copyRaster does not request update(); the caller owns redraw.
    return TJS_S_OK;
}

} // namespace

NCB_ATTACH_FUNCTION(copyRaster, Layer, copyRaster);
