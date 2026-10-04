// layerExImage.dll: brightness/contrast, hue/saturation and noise operations on
// a Layer's main image.
//
// Adapted from krkrsdl3/plugins/LayerExImage.cpp; see LICENSE.krkrsdl3 for the
// upstream license and the CxImage attribution the operations derive from.  The
// pixel work lives in LayerExImageAlgo.h so the host test can exercise it
// without the engine.
//
// Upstream attaches a native instance to the Layer class and caches the layer's
// geometry through it.  krkrsdl2 reaches the same properties directly, which is
// how the sibling ports in this directory (layerexbtoa, layerexraster) work.
#include "ncbind/ncbind.hpp"

#include <algorithm>
#include <cstdint>

#include "LayerExImageAlgo.h"

#define NCB_MODULE_NAME TJS_W("layerExImage.dll")

// krkrsdl2common is a static archive: the registrations below only exist as
// static initializers, so the linker would drop this unit without an anchor.
// PluginImpl calls this before ncbAutoRegister::LoadModule.
extern "C" void krkrsdl2_link_layereximage_plugin()
{
}

namespace {

iTJSDispatch2* GetLayerClass()
{
    tTJSVariant var;
    TVPExecuteExpression(TJS_W("Layer"), &var);
    return var.AsObjectNoAddRef();
}

// tTVInteger (not tjs_int) so the main image buffer pointer survives on 64 bit
// targets; the clip values are read through the same accessor and fit in int.
tTVInteger ReadInteger(iTJSDispatch2* layerClass, const tjs_char* name, iTJSDispatch2* layer)
{
    tTJSVariant value;
    if (TJS_FAILED(layerClass->PropGet(0, name, nullptr, &value, layer)))
        TVPThrowExceptionMessage(TJS_W("layerExImage: cannot read Layer property"), name);
    return value.AsInteger();
}

// The image area the reference operates on: the layer's clip rectangle, at the
// clip origin inside the main image buffer.
struct LayerImage
{
    krkrns::Channel* pixels;
    int width;
    int height;
    std::ptrdiff_t pitch;
    tjs_int clipLeft;
    tjs_int clipTop;
    tjs_int clipWidth;
    tjs_int clipHeight;
};

void OpenLayerImage(iTJSDispatch2* layer, LayerImage& image)
{
    iTJSDispatch2* layerClass = GetLayerClass();
    if (!layer || TJS_FAILED(layer->IsInstanceOf(0, nullptr, nullptr, TJS_W("Layer"), layer)))
        TVPThrowExceptionMessage(TJS_W("layerExImage: this must be a Layer."));

    const tjs_int imageWidth = static_cast<tjs_int>(ReadInteger(layerClass, TJS_W("imageWidth"), layer));
    const tjs_int imageHeight = static_cast<tjs_int>(ReadInteger(layerClass, TJS_W("imageHeight"), layer));
    if (imageWidth <= 0 || imageHeight <= 0)
        TVPThrowExceptionMessage(TJS_W("layerExImage: this Layer has no image."));

    // Upstream works on the clip rectangle and never validates it.  A layer with
    // a degenerate or out-of-range clip would make the original walk outside the
    // buffer, so clamp to the image instead of reproducing that.
    tjs_int clipLeft = 0;
    tjs_int clipTop = 0;
    tjs_int clipWidth = imageWidth;
    tjs_int clipHeight = imageHeight;
    try
    {
        clipLeft = static_cast<tjs_int>(ReadInteger(layerClass, TJS_W("clipLeft"), layer));
        clipTop = static_cast<tjs_int>(ReadInteger(layerClass, TJS_W("clipTop"), layer));
        clipWidth = static_cast<tjs_int>(ReadInteger(layerClass, TJS_W("clipWidth"), layer));
        clipHeight = static_cast<tjs_int>(ReadInteger(layerClass, TJS_W("clipHeight"), layer));
    }
    catch (...)
    {
        // A Layer without clip properties still gets the whole image processed.
        clipLeft = 0;
        clipTop = 0;
        clipWidth = imageWidth;
        clipHeight = imageHeight;
    }
    clipLeft = std::max<tjs_int>(0, std::min(clipLeft, imageWidth));
    clipTop = std::max<tjs_int>(0, std::min(clipTop, imageHeight));
    clipWidth = std::max<tjs_int>(0, std::min(clipWidth, imageWidth - clipLeft));
    clipHeight = std::max<tjs_int>(0, std::min(clipHeight, imageHeight - clipTop));
    if (clipWidth == 0 || clipHeight == 0)
        TVPThrowExceptionMessage(TJS_W("layerExImage: this Layer has an empty clip area."));

    const tTVInteger pitch = ReadInteger(layerClass, TJS_W("mainImageBufferPitch"), layer);
    const tTVInteger buffer = ReadInteger(layerClass, TJS_W("mainImageBufferForWrite"), layer);
    if (!buffer || !pitch)
        TVPThrowExceptionMessage(TJS_W("layerExImage: this Layer has no main image buffer."));

    image.pixels = reinterpret_cast<krkrns::Channel*>(static_cast<intptr_t>(buffer)) +
                   static_cast<std::ptrdiff_t>(clipTop) * static_cast<std::ptrdiff_t>(pitch) +
                   clipLeft * krkrns::kPixelStride;
    image.width = static_cast<int>(clipWidth);
    image.height = static_cast<int>(clipHeight);
    image.pitch = static_cast<std::ptrdiff_t>(pitch);
    image.clipLeft = clipLeft;
    image.clipTop = clipTop;
    image.clipWidth = clipWidth;
    image.clipHeight = clipHeight;
}

// Mirrors layerExBase::redraw(): update(clipLeft, clipTop, clipWidth, clipHeight).
void Redraw(iTJSDispatch2* layer, const LayerImage& image)
{
    tTJSVariant args[4] = {image.clipLeft, image.clipTop, image.clipWidth, image.clipHeight};
    tTJSVariant* argv[4] = {&args[0], &args[1], &args[2], &args[3]};
    if (TJS_FAILED(layer->FuncCall(0, TJS_W("update"), nullptr, nullptr, 4, argv, layer)))
        TVPThrowExceptionMessage(TJS_W("layerExImage: Layer.update failed."));
}

tjs_error Light(tTJSVariant* result, tjs_int numparams, tTJSVariant** param, iTJSDispatch2* layer)
{
    if (numparams < 2) return TJS_E_BADPARAMCOUNT;
    LayerImage image;
    OpenLayerImage(layer, image);
    krkrns::Light(image.pixels, image.width, image.height, image.pitch,
                  static_cast<int>(param[0]->AsInteger()), static_cast<int>(param[1]->AsInteger()));
    Redraw(layer, image);
    return TJS_S_OK;
}

tjs_error Colorize(tTJSVariant* result, tjs_int numparams, tTJSVariant** param, iTJSDispatch2* layer)
{
    if (numparams < 3) return TJS_E_BADPARAMCOUNT;
    LayerImage image;
    OpenLayerImage(layer, image);
    krkrns::Colorize(image.pixels, image.width, image.height, image.pitch,
                     static_cast<int>(param[0]->AsInteger()), static_cast<int>(param[1]->AsInteger()),
                     param[2]->AsReal());
    Redraw(layer, image);
    return TJS_S_OK;
}

tjs_error Modulate(tTJSVariant* result, tjs_int numparams, tTJSVariant** param, iTJSDispatch2* layer)
{
    if (numparams < 3) return TJS_E_BADPARAMCOUNT;
    LayerImage image;
    OpenLayerImage(layer, image);
    krkrns::Modulate(image.pixels, image.width, image.height, image.pitch,
                     static_cast<int>(param[0]->AsInteger()), static_cast<int>(param[1]->AsInteger()),
                     static_cast<int>(param[2]->AsInteger()));
    Redraw(layer, image);
    return TJS_S_OK;
}

tjs_error Noise(tTJSVariant* result, tjs_int numparams, tTJSVariant** param, iTJSDispatch2* layer)
{
    if (numparams < 1) return TJS_E_BADPARAMCOUNT;
    LayerImage image;
    OpenLayerImage(layer, image);
    krkrns::Noise(image.pixels, image.width, image.height, image.pitch,
                  static_cast<int>(param[0]->AsInteger()));
    Redraw(layer, image);
    return TJS_S_OK;
}

tjs_error GenerateWhiteNoise(tTJSVariant* result, tjs_int numparams, tTJSVariant** param,
                             iTJSDispatch2* layer)
{
    LayerImage image;
    OpenLayerImage(layer, image);
    krkrns::GenerateWhiteNoise(image.pixels, image.width, image.height, image.pitch);
    Redraw(layer, image);
    return TJS_S_OK;
}

}  // namespace

NCB_ATTACH_FUNCTION(light, Layer, Light);
NCB_ATTACH_FUNCTION(colorize, Layer, Colorize);
NCB_ATTACH_FUNCTION(modulate, Layer, Modulate);
NCB_ATTACH_FUNCTION(noise, Layer, Noise);
NCB_ATTACH_FUNCTION(generateWhiteNoise, Layer, GenerateWhiteNoise);
