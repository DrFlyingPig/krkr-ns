// layerExSave.dll: save a Layer's image (PNG / TLG5) and the small helpers the
// KAG layer code uses to prepare it.
//
// There is no reference source for this plugin anywhere in the workspace (the
// copy in krkrsdl3 is a zero byte placeholder), so the contract comes from the
// 1.3.9 register table plus the titles' call sites.  The original
// layerExSave.dll ships in the game folders and is available for a later
// behaviour comparison.
//
// Members a title reaches once this plugin is linked (the call sites are in the
// titles' own system scripts, e.g. system/BMPBaseAffineSourceLayer.tjs calls
// clearAlpha and oozeColor unguarded right after linking):
//
//   saveLayerImagePng(name)      save as PNG   (MainWindow.tjs, psdlayer.tjs, standview.tjs)
//   saveLayerImageTlg5(name)     save as TLG5  (psdlayer.tjs)
//   getCropRect()                content bounding box      (world.tjs, exroll.tjs)
//   getCropRectZero()            content bounding box      (psdlayer.tjs trimming)
//   getDiffRect(other)           bounding box of the changed area (StandAffineSourceLayer.tjs)
//   oozeColor(level)             spread colour into transparent pixels (exroll.tjs, StandAffineSourceLayer.tjs)
//   copyBlueToAlpha(src)         alpha channel from a blue channel (psdlayer.tjs, world.tjs)
//   isBlank(l, t, w, h)          transparent-only rect test (psdlayer.tjs)
//   clearAlpha()                 alpha = 0 everywhere       (BMPBaseAffineSourceLayer.tjs)
//
// The two savers go through the layer's own saveLayerImage(name, mode) so the
// engine's meta handling (offs_x / offs_y) stays the one the core already uses.
// The helpers' semantics are inferred from those call sites and are documented
// per member; oozeColor is the one with a real algorithmic choice (how far and
// how the colour spreads), and it is marked as such.
#include "ncbind/ncbind.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#define NCB_MODULE_NAME TJS_W("layerExSave.dll")

extern "C" void krkrsdl2_link_layerexsave_plugin()
{
}

namespace {

iTJSDispatch2* GetLayerClass()
{
    tTJSVariant var;
    TVPExecuteExpression(TJS_W("Layer"), &var);
    return var.AsObjectNoAddRef();
}

bool ReadInteger(iTJSDispatch2* layerClass, const tjs_char* name, iTJSDispatch2* layer,
                 tTVInteger& out)
{
    tTJSVariant value;
    if (TJS_FAILED(layerClass->PropGet(0, name, nullptr, &value, layer)))
        return false;
    out = value.AsInteger();
    return true;
}

// The clip rectangle of the layer, falling back to the whole image.
struct LayerImage
{
    std::uint8_t* pixels;
    int width;
    int height;
    std::ptrdiff_t pitch;
};

bool OpenLayer(iTJSDispatch2* layer, LayerImage& image, bool forWrite)
{
    iTJSDispatch2* layerClass = GetLayerClass();
    if (!layer || TJS_FAILED(layer->IsInstanceOf(0, nullptr, nullptr, TJS_W("Layer"), layer)))
        return false;

    tTVInteger width = 0, height = 0, pitch = 0, buffer = 0;
    if (!ReadInteger(layerClass, TJS_W("imageWidth"), layer, width) ||
        !ReadInteger(layerClass, TJS_W("imageHeight"), layer, height) ||
        !ReadInteger(layerClass, TJS_W("mainImageBufferPitch"), layer, pitch) ||
        !ReadInteger(layerClass, forWrite ? TJS_W("mainImageBufferForWrite")
                                          : TJS_W("mainImageBuffer"), layer, buffer))
        return false;
    if (width <= 0 || height <= 0 || pitch == 0 || buffer == 0)
        return false;

    image.pixels = reinterpret_cast<std::uint8_t*>(static_cast<intptr_t>(buffer));
    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    image.pitch = static_cast<std::ptrdiff_t>(pitch);
    return true;
}

std::uint8_t* PixelAt(const LayerImage& image, int x, int y)
{
    return image.pixels + static_cast<std::ptrdiff_t>(y) * image.pitch + x * 4;
}

tTJSVariant MakeRect(int x, int y, int w, int h)
{
    tTJSVariant result;
    iTJSDispatch2* rect = TJSCreateDictionaryObject();
    const tjs_char* names[4] = {TJS_W("x"), TJS_W("y"), TJS_W("w"), TJS_W("h")};
    const tjs_int values[4] = {x, y, w, h};
    for (int i = 0; i < 4; ++i)
    {
        tTJSVariant value(values[i]);
        rect->PropSet(TJS_MEMBERENSURE, names[i], nullptr, &value, rect);
    }
    result = tTJSVariant(rect, rect);
    rect->Release();
    return result;
}

// Bounding box of every pixel whose alpha is not zero; void when the layer is
// empty.
bool ContentBounds(const LayerImage& image, int& left, int& top, int& right, int& bottom)
{
    left = image.width;
    top = image.height;
    right = -1;
    bottom = -1;
    for (int y = 0; y < image.height; ++y)
    {
        const std::uint8_t* row = PixelAt(image, 0, y);
        for (int x = 0; x < image.width; ++x, row += 4)
        {
            if (row[3] == 0) continue;
            if (x < left) left = x;
            if (x > right) right = x;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    }
    return right >= left && bottom >= top;
}

// --- savers ------------------------------------------------------------------

tjs_error SaveAs(tTJSVariant* result, tjs_int numparams, tTJSVariant** param,
                 iTJSDispatch2* layer, const tjs_char* mode)
{
    if (numparams < 1) return TJS_E_BADPARAMCOUNT;
    if (!layer || TJS_FAILED(layer->IsInstanceOf(0, nullptr, nullptr, TJS_W("Layer"), layer)))
        return TJS_E_INVALIDPARAM;

    // Reuse the core method: it owns the meta dictionary (offs_x / offs_y) and
    // the format dispatch, exactly like the desktop plugin's PNG/TLG5 entry
    // points do.
    tTJSVariant args[2] = {*param[0], tTJSVariant(mode)};
    tTJSVariant* argv[2] = {&args[0], &args[1]};
    if (TJS_FAILED(layer->FuncCall(0, TJS_W("saveLayerImage"), nullptr, nullptr, 2, argv, layer)))
        return TJS_E_FAIL;
    return TJS_S_OK;
}

tjs_error SaveLayerImagePng(tTJSVariant* result, tjs_int numparams, tTJSVariant** param,
                            iTJSDispatch2* layer)
{
    return SaveAs(result, numparams, param, layer, TJS_W("png"));
}

tjs_error SaveLayerImageTlg5(tTJSVariant* result, tjs_int numparams, tTJSVariant** param,
                             iTJSDispatch2* layer)
{
    return SaveAs(result, numparams, param, layer, TJS_W("tlg5"));
}

// --- mask helpers ------------------------------------------------------------

// alpha = 0 for every pixel; the colour channels are left alone.
tjs_error ClearAlpha(tTJSVariant* result, tjs_int numparams, tTJSVariant** param,
                     iTJSDispatch2* layer)
{
    LayerImage image;
    if (!OpenLayer(layer, image, true)) return TJS_E_INVALIDPARAM;
    for (int y = 0; y < image.height; ++y)
    {
        std::uint8_t* pixel = PixelAt(image, 0, y);
        for (int x = 0; x < image.width; ++x, pixel += 4) pixel[3] = 0;
    }
    return TJS_S_OK;
}

// dst.alpha = src.blue, per pixel; both layers must have the same image size.
tjs_error CopyBlueToAlpha(tTJSVariant* result, tjs_int numparams, tTJSVariant** param,
                          iTJSDispatch2* layer)
{
    if (numparams < 1) return TJS_E_BADPARAMCOUNT;
    LayerImage dst, src;
    if (!OpenLayer(layer, dst, true)) return TJS_E_INVALIDPARAM;
    if (!OpenLayer(param[0]->AsObjectNoAddRef(), src, false))
        TVPThrowExceptionMessage(TJS_W("Different layer size."));
    if (dst.width != src.width || dst.height != src.height)
        TVPThrowExceptionMessage(TJS_W("Different layer size."));

    for (int y = 0; y < dst.height; ++y)
    {
        std::uint8_t* d = PixelAt(dst, 0, y);
        const std::uint8_t* s = PixelAt(src, 0, y);
        for (int x = 0; x < dst.width; ++x, d += 4, s += 4) d[3] = s[0];
    }
    return TJS_S_OK;
}

// Every alpha inside the rect is zero.  The rect is clipped to the image.
tjs_error IsBlank(tTJSVariant* result, tjs_int numparams, tTJSVariant** param,
                  iTJSDispatch2* layer)
{
    if (numparams < 4) return TJS_E_BADPARAMCOUNT;
    LayerImage image;
    if (!OpenLayer(layer, image, false)) return TJS_E_INVALIDPARAM;

    int left = static_cast<int>(param[0]->AsInteger());
    int top = static_cast<int>(param[1]->AsInteger());
    int width = static_cast<int>(param[2]->AsInteger());
    int height = static_cast<int>(param[3]->AsInteger());
    if (width <= 0 || height <= 0 || left >= image.width || top >= image.height)
    {
        if (result) *result = true;  // an empty rect is blank
        return TJS_S_OK;
    }

    left = std::max(0, left);
    top = std::max(0, top);
    const int right = std::min(image.width, left + width);
    const int bottom = std::min(image.height, top + height);
    for (int y = top; y < bottom; ++y)
    {
        const std::uint8_t* pixel = PixelAt(image, left, y);
        for (int x = left; x < right; ++x, pixel += 4)
        {
            if (pixel[3] != 0)
            {
                if (result) *result = false;
                return TJS_S_OK;
            }
        }
    }
    if (result) *result = true;
    return TJS_S_OK;
}

// Bounding box of the non transparent area, as %[x, y, w, h]; void when the
// layer has no visible pixel.  registerRedraw asserts the rect is used as a
// "has content" test (world.tjs) and for trimming (psdlayer.tjs).
tjs_error GetCropRect(tTJSVariant* result, tjs_int numparams, tTJSVariant** param,
                      iTJSDispatch2* layer)
{
    if (!result) return TJS_S_OK;
    LayerImage image;
    if (!OpenLayer(layer, image, false)) return TJS_E_INVALIDPARAM;

    int left, top, right, bottom;
    if (!ContentBounds(image, left, top, right, bottom))
    {
        result->Clear();
        return TJS_S_OK;
    }
    *result = MakeRect(left, top, right - left + 1, bottom - top + 1);
    return TJS_S_OK;
}

// Bounding box of the pixels that differ between this layer and another, as
// %[x, y, w, h]; void when they are pixel identical.  Both layers must have the
// same image size (the register table carries the "Different layer size."
// message for this plugin).
tjs_error GetDiffRect(tTJSVariant* result, tjs_int numparams, tTJSVariant** param,
                      iTJSDispatch2* layer)
{
    if (numparams < 1) return TJS_E_BADPARAMCOUNT;
    if (!result) return TJS_S_OK;
    LayerImage a, b;
    if (!OpenLayer(layer, a, false)) return TJS_E_INVALIDPARAM;
    if (!OpenLayer(param[0]->AsObjectNoAddRef(), b, false))
        TVPThrowExceptionMessage(TJS_W("Different layer size."));
    if (a.width != b.width || a.height != b.height)
        TVPThrowExceptionMessage(TJS_W("Different layer size."));

    int left = a.width, top = a.height, right = -1, bottom = -1;
    for (int y = 0; y < a.height; ++y)
    {
        const std::uint8_t* pa = PixelAt(a, 0, y);
        const std::uint8_t* pb = PixelAt(b, 0, y);
        for (int x = 0; x < a.width; ++x, pa += 4, pb += 4)
        {
            if (pa[0] == pb[0] && pa[1] == pb[1] && pa[2] == pb[2] && pa[3] == pb[3])
                continue;
            if (x < left) left = x;
            if (x > right) right = x;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    }
    if (right < left || bottom < top)
    {
        result->Clear();
        return TJS_S_OK;
    }
    *result = MakeRect(left, top, right - left + 1, bottom - top + 1);
    return TJS_S_OK;
}

// Spreads colour outwards into fully transparent pixels so that scaling or
// filtering the layer does not pull dark fringe colours in from the empty area.
// "level" is the number of dilation passes, which is how the titles call it
// (oozeColor(4)).  Alpha is never changed.  The exact spreading rule of the
// original DLL is not documented anywhere in this workspace, so this is the
// port's own definition: one pass copies the average of the opaque orthogonal
// neighbours into each transparent pixel that has one.
tjs_error OozeColor(tTJSVariant* result, tjs_int numparams, tTJSVariant** param,
                    iTJSDispatch2* layer)
{
    int level = 1;
    if (numparams >= 1 && param[0]->Type() != tvtVoid)
        level = static_cast<int>(param[0]->AsInteger());
    if (level <= 0) return TJS_S_OK;
    level = std::min(level, 64);

    LayerImage image;
    if (!OpenLayer(layer, image, true)) return TJS_E_INVALIDPARAM;

    const int width = image.width, height = image.height;
    std::vector<std::uint8_t> filled(static_cast<std::size_t>(width) * height, 0);
    for (int pass = 0; pass < level; ++pass)
    {
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                std::uint8_t* pixel = PixelAt(image, x, y);
                if (pixel[3] != 0)
                {
                    filled[static_cast<std::size_t>(y) * width + x] = 1;
                    continue;
                }
                if (filled[static_cast<std::size_t>(y) * width + x]) continue;
                int sum[3] = {0, 0, 0};
                int count = 0;
                const int dx[4] = {-1, 1, 0, 0};
                const int dy[4] = {0, 0, -1, 1};
                for (int d = 0; d < 4; ++d)
                {
                    const int nx = x + dx[d], ny = y + dy[d];
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                    const std::uint8_t* nb = PixelAt(image, nx, ny);
                    if (nb[3] == 0) continue;
                    sum[0] += nb[0];
                    sum[1] += nb[1];
                    sum[2] += nb[2];
                    ++count;
                }
                if (!count) continue;
                pixel[0] = static_cast<std::uint8_t>(sum[0] / count);
                pixel[1] = static_cast<std::uint8_t>(sum[1] / count);
                pixel[2] = static_cast<std::uint8_t>(sum[2] / count);
                filled[static_cast<std::size_t>(y) * width + x] = 1;
            }
        }
    }
    return TJS_S_OK;
}

}  // namespace

NCB_ATTACH_FUNCTION(saveLayerImagePng, Layer, SaveLayerImagePng);
NCB_ATTACH_FUNCTION(saveLayerImageTlg5, Layer, SaveLayerImageTlg5);
NCB_ATTACH_FUNCTION(getCropRect, Layer, GetCropRect);
NCB_ATTACH_FUNCTION(getCropRectZero, Layer, GetCropRect);
NCB_ATTACH_FUNCTION(getDiffRect, Layer, GetDiffRect);
NCB_ATTACH_FUNCTION(isBlank, Layer, IsBlank);
NCB_ATTACH_FUNCTION(copyBlueToAlpha, Layer, CopyBlueToAlpha);
NCB_ATTACH_FUNCTION(clearAlpha, Layer, ClearAlpha);
NCB_ATTACH_FUNCTION(oozeColor, Layer, OozeColor);
