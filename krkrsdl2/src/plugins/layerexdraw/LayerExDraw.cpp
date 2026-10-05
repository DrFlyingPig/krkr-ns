#include "ncbind/ncbind.hpp"
#include "LayerExDraw.hpp"
#include "StorageIntf.h"
#include "BinaryStream.h"
#include "CharacterSet.h"
#include "TVPSysFont.h"
#include <vector>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

#define NCB_MODULE_NAME TJS_W("layerExDraw.dll")

// ============================================================
// Global font storage (replaces blend2d font data/face vectors)
// ============================================================
struct FontFaceEntry
{
    plutovg_font_face_t* face;
    ttstr name;
    void* data;     // owned font data buffer
    unsigned int dataSize;
};
static std::vector<FontFaceEntry> g_fontFaces;

void initGdiPlus()
{
}

void deInitGdiPlus()
{
    for (auto& entry : g_fontFaces)
    {
        if (entry.face)
            plutovg_font_face_destroy(entry.face);
            // data is freed by plutovg via destroy_func
    }
    g_fontFaces.clear();
}

// ============================================================
// Image loading
// ============================================================

// krkrsdl2 adaptations: this tree exposes no TVPCreateFontStream, and its
// embedded fallback font is not a Droid face.  Both helpers open a stream
// through the storage system instead.

// plutovg takes UTF-8 paths; TJS strings are UTF-16 in this tree.
static std::string ToUtf8Path(const tjs_char* text)
{
    const tjs_int length = TVPWideCharToUtf8String(text, NULL);
    if (length < 0) return std::string();
    std::string out(static_cast<std::size_t>(length) + 1, '\0');
    TVPWideCharToUtf8String(text, &out[0]);
    out.resize(static_cast<std::size_t>(length));
    return out;
}

static tTJSBinaryStream* TVPCreateFontStream(const ttstr& name)
{
    const ttstr placed = TVPGetPlacedPath(name);
    if (placed.length()) return TVPCreateBinaryStreamForRead(placed, TJS_W(""));
    return NULL;
}

static tTJSBinaryStream* GetResourceStream(const ttstr& filename)
{
    tTJSBinaryStream* stream = TVPCreateFontStream(filename);
    if (stream) return stream;
    // The port ships notosanssc.ttf in its romfs; the reference embedded a Droid
    // fallback face instead.
    return TVPCreateBinaryStreamForRead(TJS_W("file://?/romfs:/notosanssc.ttf"), TJS_W(""));
}

static plutovg_surface_t* loadImage(const tjs_char* name)
{
    ttstr filename = TVPGetPlacedPath(name);
    if (filename.length())
    {
        tTJSBinaryStream* in = TVPCreateBinaryStreamForRead(filename, TJS_W(""));
        if (in)
        {
            tjs_uint size = (tjs_uint)in->GetSize();
            tjs_uint8* fileData = new tjs_uint8[size];
            in->ReadBuffer(fileData, size);
            plutovg_surface_t* surface = plutovg_surface_load_from_image_data(fileData, size);
            delete[] fileData;
            delete in;
            return surface;
        }
    }
    return nullptr;
}

// ============================================================
// GdiPlus static methods
// ============================================================
static void fontDataDestroy(void* closure)
{
    delete[] (tjs_uint8*)closure;
}

void GdiPlus::addPrivateFont(const tjs_char* fontFileName)
{
    ttstr filename = TVPGetPlacedPath(fontFileName);
    tTJSBinaryStream* in = NULL;
    if (filename.length())
    {
        in = TVPCreateBinaryStreamForRead(filename, TJS_W(""));
    }
    else
    {
        std::vector<tjs_string> ret;
        TVPGetAllFontList(ret);
        for (auto& ftN : ret)
        {
            if (ttstr(ftN) == ttstr(fontFileName))
            {
                in = TVPCreateFontStream(fontFileName);
                if (in)
                    break;
            }
        }
        if (!in)
        {
            in = GetResourceStream(TJS_W("DroidSansFallback.ttf"));
        }
    }
    if (in)
    {
        tjs_uint size = (tjs_uint)in->GetSize();
        tjs_uint8* fileData = new tjs_uint8[size];
        in->ReadBuffer(fileData, size);
        delete in;

        // Load font face (plutovg takes ownership of data via destroy callback)
        plutovg_font_face_t* face =
            plutovg_font_face_load_from_data(fileData, size, 0, fontDataDestroy, fileData);
        if (!face)
        {
            delete[] fileData;
            TVPThrowExceptionMessage(TJS_W("plutovg cannot load:%1"), fontFileName);
        }

        FontFaceEntry entry;
        entry.face = face;
        entry.name = ttstr(fontFileName);
        entry.data = fileData;
        entry.dataSize = size;
        g_fontFaces.push_back(entry);
        return;
    }
    TVPThrowExceptionMessage(TJS_W("cannot open:%1"), fontFileName);
}

tTJSVariant GdiPlus::getFontList(bool privateOnly)
{
    iTJSDispatch2* array = TJSCreateArrayObject();

    if (!privateOnly)
    {
        std::vector<tjs_string> ret;
        TVPGetAllFontList(ret);
        for (auto& ftN : ret)
        {
            if (!ftN.empty())
            {
                tTJSVariant vname(ftN), *param = &vname;
                array->FuncCall(0, TJS_W("add"), NULL, 0, 1, &param, array);
            }
        }
    }

    for (auto& entry : g_fontFaces)
    {
        if (entry.name.length())
        {
            tTJSVariant vname(entry.name), *param = &vname;
            array->FuncCall(0, TJS_W("add"), NULL, 0, 1, &param, array);
        }
    }

    tTJSVariant ret(array, array);
    array->Release();
    return ret;
}

// ============================================================
// FontInfo implementation
// ============================================================
FontInfo::FontInfo()
  : emSize(12),
    style(0),
    gdiPlusUnsupportedFont(false),
    forceSelfPathDraw(false),
    propertyModified(true),
    ascent(0),
    descent(0),
    lineSpacing(0),
    ascentLeading(0),
    descentLeading(0)
{
}

FontInfo::FontInfo(const tjs_char* familyName, tjs_real emSize, tjs_int style)
  : gdiPlusUnsupportedFont(false),
    forceSelfPathDraw(false),
    propertyModified(true),
    ascent(0),
    descent(0),
    lineSpacing(0),
    ascentLeading(0),
    descentLeading(0)
{
    setFamilyName(familyName);
    setEmSize(emSize);
    setStyle(style);
}

FontInfo::FontInfo(const FontInfo& orig)
{
    familyName = orig.familyName;
    emSize = orig.emSize;
    style = orig.style;
    gdiPlusUnsupportedFont = orig.gdiPlusUnsupportedFont;
    forceSelfPathDraw = orig.forceSelfPathDraw;
    propertyModified = true;
    ascent = descent = lineSpacing = ascentLeading = descentLeading = 0;
}

FontInfo::~FontInfo()
{
    clear();
}

void FontInfo::clear()
{
    familyName = "";
    gdiPlusUnsupportedFont = false;
    propertyModified = true;
}

void FontInfo::setFamilyName(const tjs_char* name)
{
    propertyModified = true;
    clear();
    if (name)
    {
        this->familyName = name;
    }
    if (!getFontFace())
    {
        GdiPlus::addPrivateFont(name);
    }
}

void FontInfo::setForceSelfPathDraw(bool state)
{
    forceSelfPathDraw = state;
    this->setFamilyName(familyName.c_str());
}

bool FontInfo::getForceSelfPathDraw(void) const
{
    return forceSelfPathDraw;
}

bool FontInfo::getSelfPathDraw(void) const
{
    return forceSelfPathDraw || gdiPlusUnsupportedFont;
}

void FontInfo::updateSizeParams(void) const
{
    if (!propertyModified)
        return;
    propertyModified = false;

    plutovg_font_face_t* face = getFontFace();
    if (face)
    {
        float a, d, lg;
        plutovg_font_face_get_metrics(face, (float)emSize, &a, &d, &lg, nullptr);
        ascent = a;
        descent = -d; // plutovg returns negative descent
        lineSpacing = lg;
        ascentLeading = 0;
        descentLeading = 0;
    }
    else
    {
        ascent = emSize * 0.8f;
        descent = emSize * 0.2f;
        lineSpacing = emSize * 1.2f;
        ascentLeading = 0;
        descentLeading = 0;
    }
}

tjs_real FontInfo::getAscent() const
{
    updateSizeParams();
    return ascent;
}

tjs_real FontInfo::getDescent() const
{
    updateSizeParams();
    return descent;
}

tjs_real FontInfo::getAscentLeading() const
{
    updateSizeParams();
    return ascentLeading;
}

tjs_real FontInfo::getDescentLeading() const
{
    updateSizeParams();
    return descentLeading;
}

tjs_real FontInfo::getLineSpacing() const
{
    updateSizeParams();
    return lineSpacing;
}

plutovg_font_face_t* FontInfo::getFontFace() const
{
    // Search by family name
    for (auto& entry : g_fontFaces)
    {
        if (entry.name == familyName)
            return entry.face;
    }
    // Fallback to first available
    if (!g_fontFaces.empty())
        return g_fontFaces[0].face;
    return nullptr;
}

// ============================================================
// Appearance implementation
// ============================================================
Appearance::Appearance()
{
}

Appearance::~Appearance()
{
    clear();
}

Appearance* Appearance::Clone() const
{
    Appearance* newItm = new Appearance();
    for (const auto& itm : drawInfos)
    {
        DrawInfo newdraw(itm);
        newItm->drawInfos.push_back(newdraw);
    }
    return newItm;
}

void Appearance::clear()
{
    drawInfos.clear();
}

// ============================================================
// Helper functions for brush/pen parameter parsing
// ============================================================
extern bool IsArray(const tTJSVariant& var);
extern PointF getPoint(const tTJSVariant& var);

static void getPoints(const tTJSVariant& var, std::vector<PointF>& points)
{
    ncbPropAccessor info(var);
    int c = info.GetArrayCount();
    for (int i = 0; i < c; i++)
    {
        tTJSVariant p;
        if (info.checkVariant(i, p))
            points.push_back(getPoint(p));
    }
}

static void getPoints(ncbPropAccessor& info, int n, std::vector<PointF>& points)
{
    tTJSVariant var;
    if (info.checkVariant(n, var))
        getPoints(var, points);
}

static void getPoints(ncbPropAccessor& info, const tjs_char* n, std::vector<PointF>& points)
{
    tTJSVariant var;
    if (info.checkVariant(n, var))
        getPoints(var, points);
}

static RectF getRect(tTJSVariant& var);

static void getRects(const tTJSVariant& var, std::vector<RectF>& rects)
{
    ncbPropAccessor info(var);
    int c = info.GetArrayCount();
    for (int i = 0; i < c; i++)
    {
        tTJSVariant p;
        if (info.checkVariant(i, p))
            rects.push_back(getRect(p));
    }
}

static void getReals(const tTJSVariant& var, std::vector<tjs_real>& points)
{
    ncbPropAccessor info(var);
    int c = info.GetArrayCount();
    for (int i = 0; i < c; i++)
        points.push_back((tjs_real)info.getRealValue(i));
}

static void getReals(ncbPropAccessor& info, int n, std::vector<tjs_real>& points)
{
    tTJSVariant var;
    if (info.checkVariant(n, var))
        getReals(var, points);
}

static void getReals(ncbPropAccessor& info, const tjs_char* n, std::vector<tjs_real>& points)
{
    tTJSVariant var;
    if (info.checkVariant(n, var))
        getReals(var, points);
}

static void getColors(const tTJSVariant& var, std::vector<tjs_uint32>& colors)
{
    ncbPropAccessor info(var);
    int c = info.GetArrayCount();
    for (int i = 0; i < c; i++)
        colors.push_back((tjs_uint32)info.getIntValue(i));
}

static void getColors(ncbPropAccessor& info, const tjs_char* n, std::vector<tjs_uint32>& colors)
{
    tTJSVariant var;
    if (info.checkVariant(n, var))
        getColors(var, colors);
}

static RectF calculateBounds(const std::vector<PointF>& points)
{
    RectF bounds;
    if (!points.empty())
    {
        tjs_real x0 = points[0].x, y0 = points[0].y, x1 = x0, y1 = y0;
        for (size_t i = 1; i < points.size(); i++)
        {
            x0 = std::min(x0, points[i].x);
            y0 = std::min(y0, points[i].y);
            x1 = std::max(x1, points[i].x);
            y1 = std::max(y1, points[i].y);
        }
        bounds.reset(x0, y0, x1 - x0, y1 - y0);
    }
    return bounds;
}

// ============================================================
// Hatch pattern generation using plutovg
// ============================================================
static plutovg_surface_t* createHatchPattern(HatchStyle style, tjs_uint32 foreColor,
                                             tjs_uint32 backColor, int size = 8)
{
    plutovg_surface_t* pattern = plutovg_surface_create(size, size);
    plutovg_canvas_t* ctx = plutovg_canvas_create(pattern);

    // Fill background
    plutovg_color_t bg;
    plutovg_color_init_argb32(&bg, backColor);
    plutovg_canvas_set_operator(ctx, PLUTOVG_OPERATOR_SRC);
    plutovg_canvas_set_color(ctx, &bg);
    plutovg_canvas_fill_rect(ctx, 0, 0, (float)size, (float)size);

    // Draw foreground
    plutovg_color_t fg;
    plutovg_color_init_argb32(&fg, foreColor);
    plutovg_canvas_set_operator(ctx, PLUTOVG_OPERATOR_SRC_OVER);
    plutovg_canvas_set_color(ctx, &fg);

    // Helper lambda: stroke a line path on the canvas
    auto strokeLines = [&](plutovg_path_t* lp) {
        plutovg_canvas_set_line_width(ctx, 1);
        plutovg_canvas_stroke_path(ctx, lp);
        plutovg_path_destroy(lp);
    };

    switch (style)
    {
        case HatchStyleHorizontal:
            plutovg_canvas_fill_rect(ctx, 0, size / 2.0f, (float)size, 1);
            break;
        case HatchStyleVertical:
            plutovg_canvas_fill_rect(ctx, size / 2.0f, 0, 1, (float)size);
            break;
        case HatchStyleForwardDiagonal:
        {
            plutovg_path_t* lp = plutovg_path_create();
            for (int i = -size; i < size; i += 2)
            {
                plutovg_path_move_to(lp, (float)i, 0);
                plutovg_path_line_to(lp, (float)(i + size), (float)size);
            }
            strokeLines(lp);
            break;
        }
        case HatchStyleBackwardDiagonal:
        {
            plutovg_path_t* lp = plutovg_path_create();
            for (int i = -size; i < size; i += 2)
            {
                plutovg_path_move_to(lp, (float)(i + size), 0);
                plutovg_path_line_to(lp, (float)i, (float)size);
            }
            strokeLines(lp);
            break;
        }
        case HatchStyleCross:
            plutovg_canvas_fill_rect(ctx, 0, size / 2.0f, (float)size, 1);
            plutovg_canvas_fill_rect(ctx, size / 2.0f, 0, 1, (float)size);
            break;
        case HatchStyleDiagonalCross:
        {
            plutovg_path_t* lp = plutovg_path_create();
            for (int i = -size; i < size; i += 2)
            {
                plutovg_path_move_to(lp, (float)i, 0);
                plutovg_path_line_to(lp, (float)(i + size), (float)size);
                plutovg_path_move_to(lp, (float)(i + size), 0);
                plutovg_path_line_to(lp, (float)i, (float)size);
            }
            strokeLines(lp);
            break;
        }
        case HatchStyle50Percent:
            for (int y = 0; y < size; y += 2)
                for (int x = y % 2; x < size; x += 2)
                    plutovg_canvas_fill_rect(ctx, (float)x, (float)y, 1, 1);
            break;
        case HatchStyleSmallGrid:
            for (int i = 0; i < size; i += 2)
            {
                plutovg_canvas_fill_rect(ctx, 0, (float)i, (float)size, 1);
                plutovg_canvas_fill_rect(ctx, (float)i, 0, 1, (float)size);
            }
            break;
        case HatchStyleDottedGrid:
            for (int y = 0; y < size; y += 4)
                for (int x = 0; x < size; x += 4)
                    plutovg_canvas_fill_rect(ctx, (float)x, (float)y, 1, 1);
            break;
        default:
        {
            plutovg_path_t* lp = plutovg_path_create();
            for (int i = -size; i < size; i += 2)
            {
                plutovg_path_move_to(lp, (float)i, 0);
                plutovg_path_line_to(lp, (float)(i + size), (float)size);
            }
            strokeLines(lp);
            break;
        }
    }

    plutovg_canvas_destroy(ctx);
    return pattern;
}

// ============================================================
// Brush creation
// ============================================================
static SoftBrush* createBrush(const tTJSVariant colorOrBrush)
{
    if (colorOrBrush.Type() != tvtObject)
    {
        return new SoftBrush((tjs_uint32)(tjs_int)colorOrBrush);
    }
    else
    {
        ncbPropAccessor info(colorOrBrush);
        BrushType type = (BrushType)info.getIntValue(TJS_W("type"), BrushTypeSolidColor);
        switch (type)
        {
            case BrushTypeSolidColor:
                return new SoftBrush(
                    (tjs_uint32)info.getIntValue(TJS_W("color"), (tjs_int)0xFFFFFFFF));
            case BrushTypeHatchFill:
            {
                HatchStyle hatchStyle =
                    (HatchStyle)info.getIntValue(TJS_W("hatchStyle"), HatchStyleHorizontal);
                tjs_uint32 foreColor =
                    (tjs_uint32)info.getIntValue(TJS_W("foreColor"), (tjs_int)0xFFFFFFFF);
                tjs_uint32 backColor =
                    (tjs_uint32)info.getIntValue(TJS_W("backColor"), (tjs_int)0xFF000000);
                plutovg_surface_t* pattern =
                    createHatchPattern(hatchStyle, foreColor, backColor);
                return new SoftBrush(pattern);
            }
            case BrushTypeTextureFill:
            {
                ttstr imgname = info.GetValue(TJS_W("image"), ncbTypedefs::Tag<ttstr>());
                plutovg_surface_t* image = loadImage(imgname.c_str());
                if (image)
                {
                    WrapMode wrapMode =
                        (WrapMode)info.getIntValue(TJS_W("wrapMode"), WrapModeTile);
                    plutovg_texture_type_t texType = PLUTOVG_TEXTURE_TYPE_TILED;
                    if (wrapMode == WrapModeClamp)
                        texType = PLUTOVG_TEXTURE_TYPE_PLAIN;

                    SoftBrush* brush = new SoftBrush(image, texType);

                    tTJSVariant dstRectVar;
                    if (info.checkVariant(TJS_W("dstRect"), dstRectVar))
                    {
                        RectF dstRect = getRect(dstRectVar);
                        int iw = plutovg_surface_get_width(image);
                        int ih = plutovg_surface_get_height(image);
                        if (dstRect.x != 0 || dstRect.y != 0 || dstRect.w != iw ||
                            dstRect.h != ih)
                        {
                            plutovg_matrix_init_identity(&brush->texMatrix);
                            plutovg_matrix_scale(&brush->texMatrix, dstRect.w / iw,
                                                 dstRect.h / ih);
                            plutovg_matrix_translate(&brush->texMatrix, dstRect.x, dstRect.y);
                            brush->hasTexMatrix = true;
                        }
                    }
                    return brush;
                }
                break;
            }
            case BrushTypePathGradient:
            {
                std::vector<PointF> points;
                getPoints(info, TJS_W("points"), points);
                if (points.empty())
                    TVPThrowExceptionMessage(TJS_W("must set points"));

                auto* gd = new SoftBrush::GradientData();
                gd->isLinear = false;
                gd->spread = PLUTOVG_SPREAD_METHOD_PAD;

                RectF bounds = calculateBounds(points);
                gd->cx = bounds.x + bounds.w / 2;
                gd->cy = bounds.y + bounds.h / 2;
                gd->cr = std::max(bounds.w, bounds.h) / 2;
                gd->fx = gd->cx;
                gd->fy = gd->cy;
                gd->fr = 0;

                tTJSVariant var;
                if (info.checkVariant(TJS_W("centerColor"), var))
                {
                    plutovg_gradient_stop_t stop;
                    stop.offset = 0.0f;
                    plutovg_color_init_argb32(&stop.color, (tjs_uint32)(tjs_int)var);
                    gd->stops.push_back(stop);
                }
                if (info.checkVariant(TJS_W("surroundColors"), var))
                {
                    std::vector<tjs_uint32> colors;
                    getColors(var, colors);
                    if (!colors.empty())
                    {
                        plutovg_gradient_stop_t stop;
                        stop.offset = 1.0f;
                        plutovg_color_init_argb32(&stop.color, colors[0]);
                        gd->stops.push_back(stop);
                    }
                }
                return new SoftBrush(gd);
            }
            case BrushTypeLinearGradient:
            {
                auto* gd = new SoftBrush::GradientData();
                gd->isLinear = true;
                gd->spread = PLUTOVG_SPREAD_METHOD_PAD;

                tTJSVariant var;
                if (info.checkVariant(TJS_W("point1"), var))
                {
                    PointF p1 = getPoint(var);
                    info.checkVariant(TJS_W("point2"), var);
                    PointF p2 = getPoint(var);
                    gd->x1 = p1.x;
                    gd->y1 = p1.y;
                    gd->x2 = p2.x;
                    gd->y2 = p2.y;
                }
                else if (info.checkVariant(TJS_W("rect"), var))
                {
                    RectF rect = getRect(var);
                    float angle = (float)info.getRealValue(TJS_W("angle"), 0.0f);
                    float rad = angle * (float)M_PI / 180.0f;
                    float cx = rect.x + rect.w / 2;
                    float cy = rect.y + rect.h / 2;
                    float length = sqrtf(rect.w * rect.w + rect.h * rect.h) / 2;
                    gd->x1 = cx - cosf(rad) * length;
                    gd->y1 = cy - sinf(rad) * length;
                    gd->x2 = cx + cosf(rad) * length;
                    gd->y2 = cy + sinf(rad) * length;
                }
                else
                {
                    TVPThrowExceptionMessage(TJS_W("must set point1,2 or rect"));
                }

                plutovg_gradient_stop_t s0, s1;
                s0.offset = 0.0f;
                plutovg_color_init_argb32(&s0.color,
                                          (tjs_uint32)(tjs_int)info.getIntValue(TJS_W("color1"), 0));
                gd->stops.push_back(s0);
                s1.offset = 1.0f;
                plutovg_color_init_argb32(&s1.color,
                                          (tjs_uint32)(tjs_int)info.getIntValue(TJS_W("color2"), 0));
                gd->stops.push_back(s1);

                return new SoftBrush(gd);
            }
            default:
                TVPThrowExceptionMessage(TJS_W("invalid brush type"));
                break;
        }
    }
    return new SoftBrush();
}

// ============================================================
// Appearance::addBrush / addPen
// ============================================================
void Appearance::addBrush(tTJSVariant colorOrBrush, tjs_real ox, tjs_real oy)
{
    drawInfos.push_back(DrawInfo(ox, oy, createBrush(colorOrBrush), 1));
}

void Appearance::addPen(tTJSVariant colorOrBrush, tTJSVariant widthOrOption, tjs_real ox,
                        tjs_real oy)
{
    SoftPen* pen = nullptr;
    tjs_real w = 1.0;
    if (colorOrBrush.Type() == tvtObject)
    {
        SoftBrush* brush = createBrush(colorOrBrush);
        pen = new SoftPen(brush, w);
    }
    else
    {
        pen = new SoftPen((tjs_uint32)(tjs_int)colorOrBrush, w);
    }

    if (widthOrOption.Type() != tvtObject)
    {
        pen->strokeWidth = (tjs_real)widthOrOption;
    }
    else
    {
        ncbPropAccessor info(widthOrOption);
        tTJSVariant var;

        if (info.checkVariant(TJS_W("width"), var))
            pen->strokeWidth = (tjs_real)var;

        if (info.checkVariant(TJS_W("dashOffset"), var))
            pen->dashOffset = (float)(tjs_real)var;

        if (info.checkVariant(TJS_W("dashStyle"), var))
        {
            if (IsArray(var))
            {
                std::vector<tjs_real> reals;
                getReals(var, reals);
                pen->dashArray.clear();
                for (auto v : reals)
                    pen->dashArray.push_back((float)v);
            }
            else
            {
                DashStyle dashStyle = (DashStyle)(tjs_int)var;
                pen->dashArray.clear();
                switch (dashStyle)
                {
                    case DashStyleDash:
                        pen->dashArray = {5.0f, 3.0f};
                        break;
                    case DashStyleDot:
                        pen->dashArray = {1.0f, 3.0f};
                        break;
                    case DashStyleDashDot:
                        pen->dashArray = {5.0f, 3.0f, 1.0f, 3.0f};
                        break;
                    case DashStyleDashDotDot:
                        pen->dashArray = {5.0f, 3.0f, 1.0f, 3.0f, 1.0f, 3.0f};
                        break;
                    default:
                        break;
                }
            }
        }

        if (info.checkVariant(TJS_W("startCap"), var))
        {
            plutovg_line_cap_t cap;
            plutovg_path_t* custom = nullptr;
            if (getLineCap(var, cap, custom, pen->strokeWidth))
            {
                if (!custom)
                    pen->lineCap = cap;
                else
                {
                    pen->startCapType = 1;
                    pen->startCapPath = custom;
                }
            }
        }

        if (info.checkVariant(TJS_W("endCap"), var))
        {
            plutovg_line_cap_t cap;
            plutovg_path_t* custom = nullptr;
            if (getLineCap(var, cap, custom, pen->strokeWidth))
            {
                if (!custom)
                    pen->lineCap = cap;
                else
                {
                    pen->endCapType = 1;
                    pen->endCapPath = custom;
                }
            }
        }

        if (info.checkVariant(TJS_W("lineJoin"), var))
        {
            LineJoin lineJoin = (LineJoin)(tjs_int)var;
            switch (lineJoin)
            {
                case LineJoinMiter:
                case LineJoinMiterClipped:
                    pen->lineJoin = PLUTOVG_LINE_JOIN_MITER;
                    break;
                case LineJoinBevel:
                    pen->lineJoin = PLUTOVG_LINE_JOIN_BEVEL;
                    break;
                case LineJoinRound:
                    pen->lineJoin = PLUTOVG_LINE_JOIN_ROUND;
                    break;
                default:
                    pen->lineJoin = PLUTOVG_LINE_JOIN_MITER;
            }
        }

        if (info.checkVariant(TJS_W("miterLimit"), var))
            pen->miterLimit = (float)(tjs_real)var;
    }
    drawInfos.push_back(DrawInfo(ox, oy, pen, 0));
}

bool Appearance::getLineCap(tTJSVariant& in, plutovg_line_cap_t& cap, plutovg_path_t*& custom,
                            tjs_real pw)
{
    custom = nullptr;
    switch (in.Type())
    {
        case tvtVoid:
        case tvtInteger:
        {
            LineCap lcap = (LineCap)(tjs_int)in;
            switch (lcap)
            {
                case LineCapFlat:
                    cap = PLUTOVG_LINE_CAP_BUTT;
                    break;
                case LineCapRound:
                    cap = PLUTOVG_LINE_CAP_ROUND;
                    break;
                case LineCapSquare:
                    cap = PLUTOVG_LINE_CAP_SQUARE;
                    break;
                default:
                    cap = PLUTOVG_LINE_CAP_BUTT;
                    break;
            }
            break;
        }
        case tvtObject:
        {
            ncbPropAccessor info(in);
            tjs_real width = pw, height = pw;
            tTJSVariant var;
            if (info.checkVariant(TJS_W("width"), var))
                width = ((tjs_real)var) * pw;
            if (info.checkVariant(TJS_W("height"), var))
                height = ((tjs_real)var) * pw;

            custom = plutovg_path_create();
            plutovg_path_move_to(custom, (float)(-width / 2), (float)(-height));
            plutovg_path_line_to(custom, 0, 0);
            plutovg_path_line_to(custom, (float)(width / 2), (float)(-height));

            bool filled = (bool)info.getIntValue(TJS_W("filled"), 1);
            if (filled)
                plutovg_path_close(custom);
            break;
        }
        default:
            return false;
    }
    return true;
}

// ============================================================
// Path implementation
// ============================================================
Path::Path()
{
    path = plutovg_path_create();
}

Path::~Path()
{
    if (path)
        plutovg_path_destroy(path);
}

void Path::startFigure()
{
    figureStarted = true;
}

void Path::closeFigure()
{
    plutovg_path_close(path);
}

void Path::drawArc(tjs_real x, tjs_real y, tjs_real width, tjs_real height, tjs_real startAngle,
                   tjs_real sweepAngle)
{
    float startRad = (float)(startAngle * M_PI / 180.0);
    float sweepRad = (float)(sweepAngle * M_PI / 180.0);
    float rx = (float)(width / 2.0);
    float ry = (float)(height / 2.0);
    float cx = (float)(x + rx);
    float cy = (float)(y + ry);
    pathEllipticArc(path, cx, cy, rx, ry, startRad, sweepRad, !figureStarted);
    figureStarted = true;
}

void Path::drawBezier(tjs_real x1, tjs_real y1, tjs_real x2, tjs_real y2, tjs_real x3,
                      tjs_real y3, tjs_real x4, tjs_real y4)
{
    if (!figureStarted)
    {
        plutovg_path_move_to(path, (float)x1, (float)y1);
        figureStarted = true;
    }
    plutovg_path_cubic_to(path, (float)x2, (float)y2, (float)x3, (float)y3, (float)x4, (float)y4);
}

void Path::drawBeziers(tTJSVariant points)
{
    std::vector<PointF> ps;
    getPoints(points, ps);
    if (ps.size() < 4 || (ps.size() - 1) % 3 != 0)
        return;

    if (!figureStarted && !ps.empty())
    {
        plutovg_path_move_to(path, (float)ps[0].x, (float)ps[0].y);
        figureStarted = true;
    }

    for (size_t i = 1; i + 2 < ps.size(); i += 3)
    {
        plutovg_path_cubic_to(path, (float)ps[i].x, (float)ps[i].y, (float)ps[i + 1].x,
                              (float)ps[i + 1].y, (float)ps[i + 2].x, (float)ps[i + 2].y);
    }
}

void Path::drawClosedCurve(tTJSVariant points)
{
    drawClosedCurve2(points, 0.5);
}

void Path::drawClosedCurve2(tTJSVariant points, tjs_real tension)
{
    std::vector<PointF> ps;
    getPoints(points, ps);
    if (ps.size() < 2)
        return;

    for (size_t i = 0; i < ps.size(); i++)
    {
        PointF p0 = ps[(i + ps.size() - 1) % ps.size()];
        PointF p1 = ps[i];
        PointF p2 = ps[(i + 1) % ps.size()];
        PointF p3 = ps[(i + 2) % ps.size()];

        PointF cp1 = p1 + (p2 - p0) * tension / 3.0;
        PointF cp2 = p2 - (p3 - p1) * tension / 3.0;

        if (i == 0)
        {
            plutovg_path_move_to(path, (float)p1.x, (float)p1.y);
            figureStarted = true;
        }
        plutovg_path_cubic_to(path, (float)cp1.x, (float)cp1.y, (float)cp2.x, (float)cp2.y,
                              (float)p2.x, (float)p2.y);
    }
    plutovg_path_close(path);
}

void Path::drawCurve(tTJSVariant points)
{
    drawCurve3(points, 0, -1, 0.5);
}

void Path::drawCurve2(tTJSVariant points, tjs_real tension)
{
    drawCurve3(points, 0, -1, tension);
}

void Path::drawCurve3(tTJSVariant points, int offset, int numberOfSegments, tjs_real tension)
{
    std::vector<PointF> ps;
    getPoints(points, ps);
    if (ps.size() < 2)
        return;

    if (numberOfSegments < 0)
        numberOfSegments = (int)ps.size() - 1;
    if (offset < 0 || offset + numberOfSegments >= (int)ps.size())
        return;

    if (!figureStarted && offset < (int)ps.size())
    {
        plutovg_path_move_to(path, (float)ps[offset].x, (float)ps[offset].y);
        figureStarted = true;
    }

    for (int i = offset; i < offset + numberOfSegments; i++)
    {
        PointF p0 = (i > 0) ? ps[i - 1] : ps[i];
        PointF p1 = ps[i];
        PointF p2 = ps[i + 1];
        PointF p3 = (i + 2 < (int)ps.size()) ? ps[i + 2] : ps[i + 1];

        PointF cp1 = p1 + (p2 - p0) * tension / 3.0f;
        PointF cp2 = p2 - (p3 - p1) * tension / 3.0f;

        plutovg_path_cubic_to(path, (float)cp1.x, (float)cp1.y, (float)cp2.x, (float)cp2.y,
                              (float)p2.x, (float)p2.y);
    }
}

void Path::drawPie(tjs_real x, tjs_real y, tjs_real width, tjs_real height, tjs_real startAngle,
                   tjs_real sweepAngle)
{
    float rx = (float)(width / 2.0);
    float ry = (float)(height / 2.0);
    float cx = (float)(x + rx);
    float cy = (float)(y + ry);
    float startRad = (float)(startAngle * M_PI / 180.0);
    float sweepRad = (float)(sweepAngle * M_PI / 180.0);

    plutovg_path_move_to(path, cx, cy);
    figureStarted = true;

    float startX = cx + rx * cosf(startRad);
    float startY = cy + ry * sinf(startRad);
    plutovg_path_line_to(path, startX, startY);
    pathEllipticArc(path, cx, cy, rx, ry, startRad, sweepRad);
    plutovg_path_line_to(path, cx, cy);
    plutovg_path_close(path);
}

void Path::drawEllipse(tjs_real x, tjs_real y, tjs_real width, tjs_real height)
{
    float cx = (float)(x + width / 2.0);
    float cy = (float)(y + height / 2.0);
    float rx = (float)(width / 2.0);
    float ry = (float)(height / 2.0);
    plutovg_path_add_ellipse(path, cx, cy, rx, ry);
    figureStarted = false;
}

void Path::drawLine(tjs_real x1, tjs_real y1, tjs_real x2, tjs_real y2)
{
    if (!figureStarted)
    {
        plutovg_path_move_to(path, (float)x1, (float)y1);
        figureStarted = true;
    }
    else
    {
        plutovg_path_line_to(path, (float)x1, (float)y1);
    }
    plutovg_path_line_to(path, (float)x2, (float)y2);
}

void Path::drawLines(tTJSVariant points)
{
    std::vector<PointF> ps;
    getPoints(points, ps);
    if (ps.empty())
        return;

    if (!figureStarted)
    {
        plutovg_path_move_to(path, (float)ps[0].x, (float)ps[0].y);
        figureStarted = true;
    }

    for (size_t i = 1; i < ps.size(); i++)
        plutovg_path_line_to(path, (float)ps[i].x, (float)ps[i].y);
}

void Path::drawPolygon(tTJSVariant points)
{
    std::vector<PointF> ps;
    getPoints(points, ps);
    if (ps.empty())
        return;

    plutovg_path_move_to(path, (float)ps[0].x, (float)ps[0].y);
    figureStarted = true;

    for (size_t i = 1; i < ps.size(); i++)
        plutovg_path_line_to(path, (float)ps[i].x, (float)ps[i].y);
    plutovg_path_close(path);
}

void Path::drawRectangle(tjs_real x, tjs_real y, tjs_real width, tjs_real height)
{
    plutovg_path_add_rect(path, (float)x, (float)y, (float)width, (float)height);
    figureStarted = false;
}

void Path::drawRectangles(tTJSVariant rects)
{
    std::vector<RectF> rs;
    getRects(rects, rs);
    for (const auto& rect : rs)
        plutovg_path_add_rect(path, (float)rect.x, (float)rect.y, (float)rect.w, (float)rect.h);
    figureStarted = false;
}

// ============================================================
// GdipImage implementation
// ============================================================
GdipImage::~GdipImage()
{
    if (_surface)
        plutovg_surface_destroy(_surface);
    for (auto& info : vectorGraph)
    {
        delete info.app;
        if (info.path)
            plutovg_path_destroy(info.path);
    }
}

GdipImage* GdipImage::Clone()
{
    GdipImage* cloned = nullptr;
    if (type == 0)
    {
        if (_surface)
        {
            int w = plutovg_surface_get_width(_surface);
            int h = plutovg_surface_get_height(_surface);
            plutovg_surface_t* newSurf = plutovg_surface_create(w, h);
            // Copy pixel data
            unsigned char* src = plutovg_surface_get_data(_surface);
            unsigned char* dst = plutovg_surface_get_data(newSurf);
            int stride = plutovg_surface_get_stride(_surface);
            memcpy(dst, src, stride * h);
            cloned = new GdipImage(newSurf);
            plutovg_surface_destroy(newSurf); // GdipImage constructor references it
        }
    }
    else if (type == 1)
    {
        cloned = new GdipImage(width, height);
        for (auto& info : vectorGraph)
        {
            GdipImage::Info newInfo;
            newInfo.app = info.app ? info.app->Clone() : nullptr;
            newInfo.path = info.path ? plutovg_path_clone(info.path) : nullptr;
            cloned->vectorGraph.push_back(newInfo);
        }
        cloned->transMtx = transMtx;
    }
    return cloned;
}

RectF GdipImage::GetBounds()
{
    if (type == 0)
    {
        if (!_surface)
            return RectF(0, 0, 0, 0);
        return RectF(0, 0, (tjs_real)width, (tjs_real)height);
    }
    else if (type == 1)
    {
        RectF last(0, 0, 0, 0);
        for (auto& itm : vectorGraph)
        {
            if (itm.path && !pathIsEmpty(itm.path))
            {
                plutovg_rect_t ext;
                plutovg_path_extents(itm.path, &ext, false);
                RectF org = last;
                RectF::Union(last, org, RectF(ext.x, ext.y, ext.w, ext.h));
            }
        }
        return last;
    }
    return RectF(0, 0, 0, 0);
}

tjs_uint GdipImage::GetFlags()
{
    if (type == 0)
    {
        if (_surface)
            return ImageFlagsHasAlpha | ImageFlagsColorSpaceRGB;
    }
    else if (type == 1)
        return ImageFlagsReadOnly | ImageFlagsColorSpaceRGB;
    return ImageFlagsNone;
}

Status GdipImage::RotateFlip(RotateFlipType rftype)
{
    switch (rftype)
    {
        case RotateNoneFlipNone:
            return Ok;
        case Rotate90FlipNone:
            plutovg_matrix_init_rotate(&transMtx, (float)(M_PI * 0.5));
            break;
        case Rotate180FlipNone:
            plutovg_matrix_init_rotate(&transMtx, (float)M_PI);
            break;
        case Rotate270FlipNone:
            plutovg_matrix_init_rotate(&transMtx, (float)(M_PI * 1.5));
            break;
        case RotateNoneFlipX:
            plutovg_matrix_init_scale(&transMtx, -1, 1);
            break;
        case RotateNoneFlipY:
            plutovg_matrix_init_scale(&transMtx, 1, -1);
            break;
        default:
            return NotImplemented;
    }
    return Ok;
}

// ============================================================
// LayerExDraw core implementation
// ============================================================
void LayerExDraw::updateRect(RectF& rect)
{
    if (updateWhenDraw)
    {
        tTVPRect rc((tjs_int)rect.x, (tjs_int)rect.y, (tjs_int)(rect.x + rect.w),
                    (tjs_int)(rect.y + rect.h));
        _this->Update(rc);
    }
}

LayerExDraw::LayerExDraw(DispatchT obj)
  : layerExBase_GL(obj),
    width(-1),
    height(-1),
    pitch(0),
    buffer(NULL),
    surface(NULL),
    canvas(NULL),
    metaGraphics(NULL),
    clipLeft(-1),
    clipTop(-1),
    clipWidth(-1),
    clipHeight(-1),
    smoothingMode(SmoothingModeAntiAlias),
    textRenderingHint(TextRenderingHintAntiAlias),
    updateWhenDraw(true)
{
    plutovg_matrix_init_identity(&viewTransform);
    plutovg_matrix_init_identity(&transform);
    plutovg_matrix_init_identity(&calcTransform);
}

LayerExDraw::~LayerExDraw()
{
    destroyRecord();
    if (canvas)
        plutovg_canvas_destroy(canvas);
    if (surface)
        plutovg_surface_destroy(surface);
}

void LayerExDraw::reset()
{
    layerExBase_GL::reset();
    if (!(canvas && width == _width && height == _height && pitch == _pitch && buffer == _buffer))
    {
        if (canvas)
        {
            plutovg_canvas_destroy(canvas);
            canvas = NULL;
        }
        if (surface)
        {
            plutovg_surface_destroy(surface);
            surface = NULL;
        }
        width = _width;
        height = _height;
        pitch = _pitch;
        buffer = _buffer;
        if (buffer && width > 0 && height > 0)
        {
            surface = plutovg_surface_create_for_data(buffer, width, height, pitch);
            canvas = plutovg_canvas_create(surface);
            plutovg_canvas_set_operator(canvas, PLUTOVG_OPERATOR_SRC_OVER);
        }
        clipLeft = clipTop = clipWidth = clipHeight = -1;
    }
    if (canvas && (_clipLeft != clipLeft || _clipTop != clipTop || _clipWidth != clipWidth ||
                   _clipHeight != clipHeight))
    {
        clipLeft = _clipLeft;
        clipTop = _clipTop;
        clipWidth = _clipWidth;
        clipHeight = _clipHeight;
        plutovg_canvas_clip_rect(canvas, (float)clipLeft, (float)clipTop, (float)clipWidth,
                                 (float)clipHeight);
    }
}

void LayerExDraw::updateViewTransform()
{
    plutovg_matrix_init_identity(&calcTransform);
    plutovg_matrix_t temp = calcTransform;
    plutovg_matrix_multiply(&calcTransform, &transform, &viewTransform);
    redrawRecord();
}

void LayerExDraw::setViewTransform(const GdipMatrix* trans)
{
    if (memcmp(&viewTransform, &trans->_core, sizeof(plutovg_matrix_t)) != 0)
    {
        viewTransform = trans->_core;
        updateViewTransform();
    }
}

void LayerExDraw::resetViewTransform()
{
    plutovg_matrix_init_identity(&viewTransform);
    updateViewTransform();
}

void LayerExDraw::rotateViewTransform(tjs_real angle)
{
    plutovg_matrix_init_rotate(&viewTransform, (float)angle);
    updateViewTransform();
}

void LayerExDraw::scaleViewTransform(tjs_real sx, tjs_real sy)
{
    plutovg_matrix_init_scale(&viewTransform, (float)sx, (float)sy);
    updateViewTransform();
}

void LayerExDraw::translateViewTransform(tjs_real dx, tjs_real dy)
{
    plutovg_matrix_init_translate(&viewTransform, (float)dx, (float)dy);
    updateViewTransform();
}

void LayerExDraw::updateTransform()
{
    plutovg_matrix_multiply(&calcTransform, &transform, &viewTransform);
}

void LayerExDraw::setTransform(const GdipMatrix* trans)
{
    if (memcmp(&transform, &trans->_core, sizeof(plutovg_matrix_t)) != 0)
    {
        transform = trans->_core;
        updateTransform();
    }
}

void LayerExDraw::resetTransform()
{
    plutovg_matrix_init_identity(&transform);
    updateTransform();
}

void LayerExDraw::rotateTransform(tjs_real angle)
{
    plutovg_matrix_init_rotate(&transform, (float)angle);
    updateTransform();
}

void LayerExDraw::scaleTransform(tjs_real sx, tjs_real sy)
{
    plutovg_matrix_init_scale(&transform, (float)sx, (float)sy);
    updateTransform();
}

void LayerExDraw::translateTransform(tjs_real dx, tjs_real dy)
{
    plutovg_matrix_init_translate(&transform, (float)dx, (float)dy);
    updateTransform();
}

// ============================================================
// Core drawing: clear, draw, fill, _drawPath
// ============================================================
void LayerExDraw::clear(tjs_uint32 argb)
{
    if (!canvas)
        return;
    plutovg_canvas_save(canvas);
    plutovg_canvas_reset_matrix(canvas);
    plutovg_color_t color;
    plutovg_color_init_argb32(&color, argb);
    plutovg_canvas_set_color(canvas, &color);
    plutovg_canvas_set_operator(canvas, PLUTOVG_OPERATOR_SRC);
    plutovg_canvas_fill_rect(canvas, 0, 0, (float)width, (float)height);
    plutovg_canvas_restore(canvas);

    if (metaGraphics)
    {
        createRecord();
        metaGraphics->bgColor = argb;
    }
    _this->Update();
}

RectF LayerExDraw::drawPath(const Appearance* app, const Path* path)
{
    return _drawPath(app, path->path);
}

RectF LayerExDraw::getPathExtents(const Appearance* app, const plutovg_path_t* path)
{
    return RectF();
}

void LayerExDraw::draw(const SoftPen* pen, const plutovg_matrix_t* matrix,
                       const plutovg_path_t* path)
{
    if (!canvas || !pen || !path)
        return;

    plutovg_canvas_save(canvas);
    plutovg_canvas_set_operator(canvas, PLUTOVG_OPERATOR_SRC_OVER);

    // Set transform
    plutovg_canvas_set_matrix(canvas, &calcTransform);
    if (matrix)
    {
        plutovg_canvas_transform(canvas, matrix);
    }

    // Set stroke style
    pen->applyStrokeStyle(canvas);
    pen->applyStrokeColor(canvas);

    // Stroke
    plutovg_canvas_stroke_path(canvas, path);

    // Custom end cap
    if (pen->endCapType == 1 && pen->endCapPath)
    {
        // Get last point of path
        float px, py;
        plutovg_path_get_current_point(path, &px, &py);
        plutovg_matrix_t endPose;
        plutovg_matrix_init_translate(&endPose, px, py);
        plutovg_canvas_transform(canvas, &endPose);
        plutovg_canvas_stroke_path(canvas, pen->endCapPath);
    }

    plutovg_canvas_restore(canvas);
}

void LayerExDraw::fill(const SoftBrush* brush, const plutovg_matrix_t* matrix,
                       const plutovg_path_t* path)
{
    if (!canvas || !brush || !path)
        return;

    plutovg_canvas_save(canvas);
    plutovg_canvas_set_operator(canvas, PLUTOVG_OPERATOR_SRC_OVER);

    // Set transform
    plutovg_canvas_set_matrix(canvas, &calcTransform);
    if (matrix)
    {
        plutovg_canvas_transform(canvas, matrix);
    }

    // Set fill style
    brush->applyToCanvas(canvas);

    // Fill
    plutovg_canvas_fill_path(canvas, path);

    plutovg_canvas_restore(canvas);
}

RectF LayerExDraw::_drawPath(const Appearance* app, const plutovg_path_t* path)
{
    RectF bounds;
    if (!canvas || !app || !path || pathIsEmpty(path))
        return bounds;

    for (const auto& drawInfo : app->drawInfos)
    {
        if (!drawInfo.info)
            continue;

        if (metaGraphics)
        {
            GdipImage::Info info;
            info.app = app->Clone();
            info.path = plutovg_path_clone(path);
            metaGraphics->vectorGraph.push_back(info);
            metaGraphics->transMtx = transform;
        }

        plutovg_matrix_t drawMatrix;
        plutovg_matrix_init_translate(&drawMatrix, (float)drawInfo.ox, (float)drawInfo.oy);

        if (drawInfo.type == 0)
        {
            SoftPen* pen = static_cast<SoftPen*>(drawInfo.info);
            draw(pen, &drawMatrix, path);
        }
        else
        {
            SoftBrush* brush = static_cast<SoftBrush*>(drawInfo.info);
            fill(brush, &drawMatrix, path);
        }
    }

    updateRect(bounds);
    return bounds;
}

// ============================================================
// Shape drawing methods
// ============================================================
RectF LayerExDraw::drawArc(const Appearance* app, tjs_real x, tjs_real y, tjs_real w, tjs_real h,
                           tjs_real startAngle, tjs_real sweepAngle)
{
    plutovg_path_t* p = plutovg_path_create();
    float rx = (float)(w / 2.0), ry = (float)(h / 2.0);
    float cx = (float)(x + rx), cy = (float)(y + ry);
    float startRad = (float)(startAngle * M_PI / 180.0);
    float sweepRad = (float)(sweepAngle * M_PI / 180.0);
    float sx = cx + rx * cosf(startRad), sy = cy + ry * sinf(startRad);
    plutovg_path_move_to(p, sx, sy);
    pathEllipticArc(p, cx, cy, rx, ry, startRad, sweepRad);
    RectF ret = _drawPath(app, p);
    plutovg_path_destroy(p);
    return ret;
}

RectF LayerExDraw::drawBezier(const Appearance* app, tjs_real x1, tjs_real y1, tjs_real x2,
                              tjs_real y2, tjs_real x3, tjs_real y3, tjs_real x4, tjs_real y4)
{
    plutovg_path_t* p = plutovg_path_create();
    plutovg_path_move_to(p, (float)x1, (float)y1);
    plutovg_path_cubic_to(p, (float)x2, (float)y2, (float)x3, (float)y3, (float)x4, (float)y4);
    RectF ret = _drawPath(app, p);
    plutovg_path_destroy(p);
    return ret;
}

RectF LayerExDraw::drawBeziers(const Appearance* app, tTJSVariant points)
{
    std::vector<PointF> ps;
    getPoints(points, ps);
    if (ps.size() < 4 || (ps.size() - 1) % 3 != 0)
        return RectF();
    plutovg_path_t* p = plutovg_path_create();
    plutovg_path_move_to(p, (float)ps[0].x, (float)ps[0].y);
    for (size_t i = 1; i + 2 < ps.size(); i += 3)
        plutovg_path_cubic_to(p, (float)ps[i].x, (float)ps[i].y, (float)ps[i + 1].x,
                              (float)ps[i + 1].y, (float)ps[i + 2].x, (float)ps[i + 2].y);
    RectF ret = _drawPath(app, p);
    plutovg_path_destroy(p);
    return ret;
}

RectF LayerExDraw::drawClosedCurve(const Appearance* app, tTJSVariant points)
{
    return drawClosedCurve2(app, points, 0.5);
}

RectF LayerExDraw::drawClosedCurve2(const Appearance* app, tTJSVariant points, tjs_real tension)
{
    std::vector<PointF> ps;
    getPoints(points, ps);
    if (ps.size() < 2)
        return RectF();

    plutovg_path_t* p = plutovg_path_create();
    for (size_t i = 0; i < ps.size(); i++)
    {
        PointF p0 = ps[(i + ps.size() - 1) % ps.size()];
        PointF p1 = ps[i];
        PointF p2 = ps[(i + 1) % ps.size()];
        PointF p3 = ps[(i + 2) % ps.size()];
        PointF cp1 = p1 + (p2 - p0) * tension / 3.0;
        PointF cp2 = p2 - (p3 - p1) * tension / 3.0;
        if (i == 0)
            plutovg_path_move_to(p, (float)p1.x, (float)p1.y);
        plutovg_path_cubic_to(p, (float)cp1.x, (float)cp1.y, (float)cp2.x, (float)cp2.y,
                              (float)p2.x, (float)p2.y);
    }
    plutovg_path_close(p);
    RectF ret = _drawPath(app, p);
    plutovg_path_destroy(p);
    return ret;
}

RectF LayerExDraw::drawCurve(const Appearance* app, tTJSVariant points)
{
    return drawCurve3(app, points, 0, -1, 0.5);
}

RectF LayerExDraw::drawCurve2(const Appearance* app, tTJSVariant points, tjs_real tension)
{
    return drawCurve3(app, points, 0, -1, tension);
}

RectF LayerExDraw::drawCurve3(const Appearance* app, tTJSVariant points, int offset,
                              int numberOfSegments, tjs_real tension)
{
    std::vector<PointF> ps;
    getPoints(points, ps);
    if (ps.size() < 2)
        return RectF();
    if (numberOfSegments < 0)
        numberOfSegments = (int)ps.size() - 1;
    if (offset < 0 || offset + numberOfSegments >= (int)ps.size())
        return RectF();

    plutovg_path_t* p = plutovg_path_create();
    plutovg_path_move_to(p, (float)ps[offset].x, (float)ps[offset].y);
    for (int i = offset; i < offset + numberOfSegments; i++)
    {
        PointF p0 = (i > 0) ? ps[i - 1] : ps[i];
        PointF p1 = ps[i];
        PointF p2 = ps[i + 1];
        PointF p3 = (i + 2 < (int)ps.size()) ? ps[i + 2] : ps[i + 1];
        PointF cp1 = p1 + (p2 - p0) * tension / 3.0f;
        PointF cp2 = p2 - (p3 - p1) * tension / 3.0f;
        plutovg_path_cubic_to(p, (float)cp1.x, (float)cp1.y, (float)cp2.x, (float)cp2.y,
                              (float)p2.x, (float)p2.y);
    }
    RectF ret = _drawPath(app, p);
    plutovg_path_destroy(p);
    return ret;
}

RectF LayerExDraw::drawPie(const Appearance* app, tjs_real x, tjs_real y, tjs_real w, tjs_real h,
                           tjs_real startAngle, tjs_real sweepAngle)
{
    plutovg_path_t* p = plutovg_path_create();
    float rx = (float)(w / 2.0), ry = (float)(h / 2.0);
    float cx = (float)(x + rx), cy = (float)(y + ry);
    float startRad = (float)(startAngle * M_PI / 180.0);
    float sweepRad = (float)(sweepAngle * M_PI / 180.0);

    plutovg_path_move_to(p, cx, cy);
    float sx = cx + rx * cosf(startRad), sy = cy + ry * sinf(startRad);
    plutovg_path_line_to(p, sx, sy);
    pathEllipticArc(p, cx, cy, rx, ry, startRad, sweepRad);
    plutovg_path_line_to(p, cx, cy);
    plutovg_path_close(p);
    RectF ret = _drawPath(app, p);
    plutovg_path_destroy(p);
    return ret;
}

RectF LayerExDraw::drawEllipse(const Appearance* app, tjs_real x, tjs_real y, tjs_real w,
                               tjs_real h)
{
    plutovg_path_t* p = plutovg_path_create();
    float cx = (float)(x + w / 2.0), cy = (float)(y + h / 2.0);
    float rx = (float)(w / 2.0), ry = (float)(h / 2.0);
    plutovg_path_add_ellipse(p, cx, cy, rx, ry);
    RectF ret = _drawPath(app, p);
    plutovg_path_destroy(p);
    return ret;
}

RectF LayerExDraw::drawLine(const Appearance* app, tjs_real x1, tjs_real y1, tjs_real x2,
                            tjs_real y2)
{
    plutovg_path_t* p = plutovg_path_create();
    plutovg_path_move_to(p, (float)x1, (float)y1);
    plutovg_path_line_to(p, (float)x2, (float)y2);
    RectF ret = _drawPath(app, p);
    plutovg_path_destroy(p);
    return ret;
}

RectF LayerExDraw::drawLines(const Appearance* app, tTJSVariant points)
{
    std::vector<PointF> ps;
    getPoints(points, ps);
    if (ps.empty())
        return RectF();
    plutovg_path_t* p = plutovg_path_create();
    plutovg_path_move_to(p, (float)ps[0].x, (float)ps[0].y);
    for (size_t i = 1; i < ps.size(); i++)
        plutovg_path_line_to(p, (float)ps[i].x, (float)ps[i].y);
    RectF ret = _drawPath(app, p);
    plutovg_path_destroy(p);
    return ret;
}

RectF LayerExDraw::drawPolygon(const Appearance* app, tTJSVariant points)
{
    std::vector<PointF> ps;
    getPoints(points, ps);
    if (ps.empty())
        return RectF();
    plutovg_path_t* p = plutovg_path_create();
    plutovg_path_move_to(p, (float)ps[0].x, (float)ps[0].y);
    for (size_t i = 1; i < ps.size(); i++)
        plutovg_path_line_to(p, (float)ps[i].x, (float)ps[i].y);
    plutovg_path_close(p);
    RectF ret = _drawPath(app, p);
    plutovg_path_destroy(p);
    return ret;
}

RectF LayerExDraw::drawRectangle(const Appearance* app, tjs_real x, tjs_real y, tjs_real w,
                                 tjs_real h)
{
    plutovg_path_t* p = plutovg_path_create();
    plutovg_path_add_rect(p, (float)x, (float)y, (float)w, (float)h);
    RectF ret = _drawPath(app, p);
    plutovg_path_destroy(p);
    return ret;
}

RectF LayerExDraw::drawRectangles(const Appearance* app, tTJSVariant rects)
{
    std::vector<RectF> rs;
    getRects(rects, rs);
    plutovg_path_t* p = plutovg_path_create();
    for (const auto& rect : rs)
        plutovg_path_add_rect(p, (float)rect.x, (float)rect.y, (float)rect.w, (float)rect.h);
    RectF ret = _drawPath(app, p);
    plutovg_path_destroy(p);
    return ret;
}

// ============================================================
// Text drawing
// ============================================================
RectF LayerExDraw::drawPathString(const FontInfo* font, const Appearance* app, tjs_real x,
                                  tjs_real y, const tjs_char* text)
{
    plutovg_font_face_t* face = font->getFontFace();
    if (!face)
        return RectF();

    plutovg_path_t* textPath = plutovg_path_create();
    float emSize = (float)font->getEmSize();
    float xpos  = (float)x;
    float yBase = (float)y + (float)font->getAscent() - (float)font->getDescent();
    size_t len = TJS_strlen(text);

    plutovg_text_iterator_t it;
    plutovg_text_iterator_init(&it, text, (int)len, PLUTOVG_TEXT_ENCODING_UTF8);
    float aw = 0, lsb = 0;
    plutovg_rect_t ext = {0, 0, 0, 0};
    while (plutovg_text_iterator_has_next(&it))
    {
        plutovg_codepoint_t cp = plutovg_text_iterator_next(&it);
        plutovg_font_face_get_glyph_metrics(face, emSize, cp, &aw, &lsb, &ext);
        float adv = plutovg_font_face_get_glyph_path(face, emSize, xpos, yBase + ext.y, cp, textPath);
        xpos += adv;
    }

    RectF ret = _drawPath(app, textPath);
    plutovg_path_destroy(textPath);
    return ret;
}

RectF LayerExDraw::drawString(const FontInfo* font, const Appearance* app, tjs_real x, tjs_real y,
                              const tjs_char* text)
{
    RectF bounds;
    plutovg_font_face_t* face = font->getFontFace();
    if (!face || !canvas)
        return RectF();

    plutovg_canvas_save(canvas);
    plutovg_canvas_set_operator(canvas, PLUTOVG_OPERATOR_SRC_OVER);
    plutovg_canvas_set_matrix(canvas, &calcTransform);
    plutovg_canvas_set_font(canvas, face, (float)font->getEmSize());

    size_t len = TJS_strlen(text);

    for (const auto& drawInfo : app->drawInfos)
    {
        if (!drawInfo.info)
            continue;

        if (drawInfo.type == 1) // brush only
        {
            SoftBrush* brush = static_cast<SoftBrush*>(drawInfo.info);
            if (!brush)
                continue;
            brush->applyToCanvas(canvas);
            plutovg_canvas_fill_text(canvas, text, (int)len, PLUTOVG_TEXT_ENCODING_UTF8,
                                     (float)(x + drawInfo.ox),
                                     (float)(y + drawInfo.oy));
        }
    }

    plutovg_canvas_restore(canvas);

    updateRect(bounds);
    return bounds;
}

RectF LayerExDraw::measureString(const FontInfo* font, const tjs_char* text)
{
    plutovg_font_face_t* face = font->getFontFace();
    if (!face)
        return RectF();

    plutovg_rect_t extents;
    size_t len = TJS_strlen(text);
    plutovg_font_face_text_extents(face, (float)font->getEmSize(), text, (int)len,
                                    PLUTOVG_TEXT_ENCODING_UTF8, &extents);
    return RectF(extents.x, extents.y, extents.w, extents.h);
}

RectF LayerExDraw::measureStringInternal(const FontInfo* font, const tjs_char* text)
{
    return measureString(font, text);
}

RectF LayerExDraw::drawPathString2(const FontInfo* font, const Appearance* app, tjs_real x,
                                   tjs_real y, const tjs_char* text)
{
    return RectF(); // TODO
}

RectF LayerExDraw::measureString2(const FontInfo* font, const tjs_char* text)
{
    return RectF(); // TODO
}

RectF LayerExDraw::measureStringInternal2(const FontInfo* font, const tjs_char* text)
{
    return RectF(); // TODO
}

void LayerExDraw::getGlyphOutline(const FontInfo* fontInfo, PointF& offset, plutovg_path_t* path,
                                  tjs_uint glyph)
{
    // TODO
}

void LayerExDraw::getTextOutline(const FontInfo* fontInfo, PointF& offset, plutovg_path_t* path,
                                 ttstr text)
{
    // TODO
}

// ============================================================
// Image drawing
// ============================================================
RectF LayerExDraw::drawImage(tjs_real x, tjs_real y, GdipImage* src)
{
    if (!src)
        return RectF();
    return drawImageRect(x, y, src, 0, 0, (tjs_real)src->width, (tjs_real)src->height);
}

RectF LayerExDraw::drawImageRect(tjs_real dleft, tjs_real dtop, GdipImage* src, tjs_real sleft,
                                 tjs_real stop, tjs_real swidth, tjs_real sheight)
{
    return drawImageAffine(src, sleft, stop, swidth, sheight, true, 1, 0, 0, 1, dleft, dtop);
}

RectF LayerExDraw::drawImageStretch(tjs_real dleft, tjs_real dtop, tjs_real dwidth,
                                    tjs_real dheight, GdipImage* src, tjs_real sleft, tjs_real stop,
                                    tjs_real swidth, tjs_real sheight)
{
    return drawImageAffine(src, sleft, stop, swidth, sheight, true, dwidth / swidth, 0, 0,
                           dheight / sheight, dleft, dtop);
}

RectF LayerExDraw::drawImageAffine(GdipImage* src, tjs_real sleft, tjs_real stop, tjs_real swidth,
                                   tjs_real sheight, bool affine, tjs_real A, tjs_real B,
                                   tjs_real C, tjs_real D, tjs_real E, tjs_real F)
{
    RectF bounds;
    if (!canvas || !src)
        return bounds;

    plutovg_matrix_t matrix;
    if (affine)
    {
        plutovg_matrix_init(&matrix, (float)A, (float)B, (float)C, (float)D, (float)E, (float)F);
    }
    else
    {
        tjs_real a = C - A, b = D - B, c = E - A, d = F - B, e = A, f = B;
        plutovg_matrix_init(&matrix, (float)a, (float)b, (float)c, (float)d, (float)e, (float)f);
    }

    if (src->type == 0)
    {
        if (!src->_surface)
            return bounds;

        const int srcLeft = (int)sleft;
        const int srcTop = (int)stop;
        const int srcWidth = (int)swidth;
        const int srcHeight = (int)sheight;
        if (srcWidth <= 0 || srcHeight <= 0)
            return bounds;

        plutovg_canvas_save(canvas);
        plutovg_canvas_set_operator(canvas, PLUTOVG_OPERATOR_SRC_OVER);
        plutovg_canvas_set_matrix(canvas, &matrix);
        plutovg_matrix_t textureMatrix;
        plutovg_matrix_init_translate(&textureMatrix, (float)(-srcLeft), (float)(-srcTop));
        plutovg_canvas_set_texture(canvas, src->_surface, PLUTOVG_TEXTURE_TYPE_PLAIN, 1.0f,
                                   &textureMatrix);
        plutovg_canvas_fill_rect(canvas, 0.0f, 0.0f, (float)srcWidth, (float)srcHeight);
        plutovg_canvas_restore(canvas);
    }
    else if (src->type == 1)
    {
        // Vector: replay
        plutovg_matrix_t savedMtx = calcTransform;
        calcTransform = matrix;
        plutovg_matrix_t srcRectTransform;
        plutovg_matrix_init_scale(&srcRectTransform, (float)(swidth / src->GetWidth()),
                                  (float)(sheight / src->GetHeight()));
        plutovg_matrix_translate(&srcRectTransform, (float)(-sleft), (float)(-stop));

        plutovg_matrix_t temp = calcTransform;
        plutovg_matrix_multiply(&calcTransform, &temp, &srcRectTransform);
        temp = calcTransform;
        plutovg_matrix_multiply(&calcTransform, &temp, &src->transMtx);

        for (auto& infoItm : src->vectorGraph)
        {
            if (!canvas || !infoItm.app || !infoItm.path || pathIsEmpty(infoItm.path))
                continue;
            for (const auto& drawInfo : infoItm.app->drawInfos)
            {
                if (!drawInfo.info)
                    continue;
                plutovg_matrix_t drawMatrix;
                plutovg_matrix_init_translate(&drawMatrix, (float)drawInfo.ox,
                                              (float)drawInfo.oy);
                if (drawInfo.type == 0)
                    draw(static_cast<SoftPen*>(drawInfo.info), &drawMatrix, infoItm.path);
                else
                    fill(static_cast<SoftBrush*>(drawInfo.info), &drawMatrix, infoItm.path);
            }
        }
        calcTransform = savedMtx;
    }

    updateRect(bounds);
    return bounds;
}

// ============================================================
// Record (metafile) operations
// ============================================================
void LayerExDraw::createRecord()
{
    destroyRecord();
    metaGraphics = new GdipImage(width, height);
}

void LayerExDraw::recreateRecord()
{
    createRecord();
}

void LayerExDraw::destroyRecord()
{
    if (metaGraphics)
    {
        delete metaGraphics;
        metaGraphics = NULL;
    }
}

void LayerExDraw::setRecord(bool record)
{
    if (record)
    {
        if (!metaGraphics)
            createRecord();
    }
    else
    {
        if (metaGraphics)
            destroyRecord();
    }
}

bool LayerExDraw::redraw(GdipImage* image)
{
    if (!image || !canvas)
        return false;
    // Clear and redraw
    plutovg_canvas_save(canvas);
    plutovg_canvas_reset_matrix(canvas);
    plutovg_color_t clearColor;
    plutovg_color_init_argb32(&clearColor, 0);
    plutovg_canvas_set_color(canvas, &clearColor);
    plutovg_canvas_set_operator(canvas, PLUTOVG_OPERATOR_SRC);
    plutovg_canvas_fill_rect(canvas, 0, 0, (float)width, (float)height);
    plutovg_canvas_restore(canvas);

    if (image->_surface)
    {
        plutovg_canvas_save(canvas);
        plutovg_canvas_set_operator(canvas, PLUTOVG_OPERATOR_SRC_OVER);
        plutovg_canvas_set_matrix(canvas, &viewTransform);
        plutovg_canvas_set_texture(canvas, image->_surface, PLUTOVG_TEXTURE_TYPE_PLAIN, 1.0f, nullptr);
        plutovg_canvas_fill_rect(canvas, 0, 0, (float)image->width, (float)image->height);
        plutovg_canvas_set_matrix(canvas, &calcTransform);
        plutovg_canvas_restore(canvas);
    }
    _this->Update();
    return true;
}

GdipImage* LayerExDraw::getRecordImage()
{
    GdipImage* image = NULL;
    if (metaGraphics)
    {
        image = metaGraphics->Clone();
        if (image)
            redraw(image);
    }
    return image;
}

bool LayerExDraw::redrawRecord()
{
    GdipImage* image = getRecordImage();
    if (image)
    {
        delete image;
        return true;
    }
    return false;
}

bool LayerExDraw::saveRecord(const tjs_char* filename)
{
    if (!surface)
        return false;
    // Save to PNG via plutovg
    // Note: TJS uses UTF-8 char paths
    ttstr fn(filename);
    bool ret = plutovg_surface_write_to_png(surface, ToUtf8Path(fn.c_str()).c_str());
    GdipImage* image = getRecordImage();
    if (image)
        delete image;
    return ret;
}

bool LayerExDraw::loadRecord(const tjs_char* filename)
{
    return false; // TODO
}

// ============================================================
// NCB Registration
// ============================================================

#define NCB_SET_CONVERTOR_BOTH(type, convertor)            \
    NCB_TYPECONV_SRCMAP_SET(type, convertor<type>, true);  \
    NCB_TYPECONV_DSTMAP_SET(type, convertor<type>, true)

#define NCB_SET_CONVERTOR_SRC(type, convertor)                          \
    NCB_TYPECONV_SRCMAP_SET(type, convertor<type>, true);               \
    NCB_TYPECONV_DSTMAP_SET(type, ncbNativeObjectBoxing::Unboxing, true)

#define NCB_SET_CONVERTOR_DST(type, convertor)                          \
    NCB_TYPECONV_SRCMAP_SET(type, ncbNativeObjectBoxing::Boxing, true);  \
    NCB_TYPECONV_DSTMAP_SET(type, convertor<type>, true)

bool IsArray(const tTJSVariant& var)
{
    if (var.Type() == tvtObject)
    {
        iTJSDispatch2* obj = var.AsObjectNoAddRef();
        return obj->IsInstanceOf(0, NULL, NULL, TJS_W("Array"), obj) == TJS_S_TRUE;
    }
    return false;
}

#define NCB_MEMBER_PROPERTY(name, type, membername)                            \
    struct AutoProp_##name                                                     \
    {                                                                          \
        static void ProxySet(Class* inst, type value) { inst->membername = value; } \
        static type ProxyGet(Class* inst) { return inst->membername; }          \
    };                                                                         \
    NCB_PROPERTY_PROXY(name, AutoProp_##name::ProxyGet, AutoProp_##name::ProxySet)

#define NCB_ARG_PROPERTY_RO(name, type, methodname)             \
    struct AutoProp_##name                                      \
    {                                                           \
        static type ProxyGet(Class* inst)                       \
        {                                                       \
            type var;                                           \
            inst->methodname(var);                              \
            return var;                                         \
        }                                                       \
    };                                                          \
    Property(TJS_W(#name), &AutoProp_##name::ProxyGet, (int)0, Proxy)

NCB_TYPECONV_CAST_INTEGER(Status);
NCB_TYPECONV_CAST_INTEGER(MatrixOrder);
NCB_TYPECONV_CAST_INTEGER(ImageType);
NCB_TYPECONV_CAST_INTEGER(RotateFlipType);
NCB_TYPECONV_CAST_INTEGER(SmoothingMode);
NCB_TYPECONV_CAST_INTEGER(TextRenderingHint);

// ------------------------------------------------------- PointF
template<class T>
struct PointFConvertor
{
    typedef ncbInstanceAdaptor<T> AdaptorT;
    template<typename ANYT>
    void operator()(ANYT& adst, const tTJSVariant& src)
    {
        if (src.Type() == tvtObject)
        {
            T* obj = AdaptorT::GetNativeInstance(src.AsObjectNoAddRef());
            if (obj)
            {
                dst = *obj;
            }
            else
            {
                ncbPropAccessor info(src);
                if (IsArray(src))
                    dst = PointF((tjs_real)info.getRealValue(0), (tjs_real)info.getRealValue(1));
                else
                    dst = PointF((tjs_real)info.getRealValue(TJS_W("x")),
                                 (tjs_real)info.getRealValue(TJS_W("y")));
            }
        }
        else
        {
            dst = T();
        }
        adst = ncbTypeConvertor::ToTarget<ANYT>::Get(&dst);
    }

private:
    T dst;
};

NCB_SET_CONVERTOR_DST(PointF, PointFConvertor);
NCB_REGISTER_SUBCLASS_DELAY(PointF)
{
    NCB_CONSTRUCTOR((tjs_real, tjs_real));
    NCB_MEMBER_PROPERTY(x, tjs_real, x);
    NCB_MEMBER_PROPERTY(y, tjs_real, y);
    NCB_METHOD(Equals);
};

PointF getPoint(const tTJSVariant& var)
{
    PointFConvertor<PointF> conv;
    PointF ret;
    conv(ret, var);
    return ret;
}

// ------------------------------------------------------- RectF
template<class T>
struct RectFConvertor
{
    typedef ncbInstanceAdaptor<T> AdaptorT;
    template<typename ANYT>
    void operator()(ANYT& adst, const tTJSVariant& src)
    {
        if (src.Type() == tvtObject)
        {
            T* obj = AdaptorT::GetNativeInstance(src.AsObjectNoAddRef());
            if (obj)
            {
                dst = *obj;
            }
            else
            {
                ncbPropAccessor info(src);
                if (IsArray(src))
                    dst = RectF((tjs_real)info.getRealValue(0), (tjs_real)info.getRealValue(1),
                                (tjs_real)info.getRealValue(2), (tjs_real)info.getRealValue(3));
                else
                    dst = RectF((tjs_real)info.getRealValue(TJS_W("x")),
                                (tjs_real)info.getRealValue(TJS_W("y")),
                                (tjs_real)info.getRealValue(TJS_W("width")),
                                (tjs_real)info.getRealValue(TJS_W("height")));
            }
        }
        else
        {
            dst = T();
        }
        adst = ncbTypeConvertor::ToTarget<ANYT>::Get(&dst);
    }

private:
    T dst;
};

static RectF getRect(tTJSVariant& var)
{
    RectFConvertor<RectF> conv;
    RectF ret;
    conv(ret, var);
    return ret;
}

NCB_SET_CONVERTOR_DST(RectF, RectFConvertor);
NCB_REGISTER_SUBCLASS_DELAY(RectF)
{
    NCB_CONSTRUCTOR((tjs_real, tjs_real, tjs_real, tjs_real));
    NCB_MEMBER_PROPERTY(x, tjs_real, x);
    NCB_MEMBER_PROPERTY(y, tjs_real, y);
    NCB_MEMBER_PROPERTY(width, tjs_real, w);
    NCB_MEMBER_PROPERTY(height, tjs_real, h);
    NCB_PROPERTY_RO(left, GetLeft);
    NCB_PROPERTY_RO(top, GetTop);
    NCB_PROPERTY_RO(right, GetRight);
    NCB_PROPERTY_RO(bottom, GetBottom);
    NCB_ARG_PROPERTY_RO(location, PointF, GetLocation);
    NCB_ARG_PROPERTY_RO(bounds, RectF, GetBounds);
    NCB_METHOD(Clone);
    NCB_METHOD(Equals);
    NCB_METHOD_DETAIL(Inflate, Class, void, Class::Inflate, (tjs_real, tjs_real));
    NCB_METHOD_DETAIL(InflatePoint, Class, void, Class::Inflate, (const PointF&));
    NCB_METHOD(IntersectsWith);
    NCB_METHOD(IsEmptyArea);
    NCB_METHOD_DETAIL(Offset, Class, void, Class::Offset, (tjs_real, tjs_real));
    NCB_METHOD(Union);
};

// ------------------------------------------------------- GdipWrapper
template<class T>
class GdipWrapper
{
    typedef T GdipClassT;
    typedef GdipWrapper<GdipClassT> WrapperT;

protected:
    GdipClassT* obj;

public:
    GdipWrapper() : obj(NULL) {}
    GdipWrapper(GdipClassT* obj) : obj(obj) {}
    GdipWrapper(const GdipWrapper& orig) : obj(NULL)
    {
        if (orig.obj)
            obj = orig.obj->Clone();
    }
    ~GdipWrapper()
    {
        if (obj)
            delete obj;
    }
    GdipClassT* getGdipObject() { return obj; }
    void setGdipObject(GdipClassT* src)
    {
        if (obj)
            delete obj;
        obj = src;
    }
    struct BridgeFunctor
    {
        GdipClassT* operator()(WrapperT* p) const { return p->getGdipObject(); }
    };
    template<class CastT>
    struct CastBridgeFunctor
    {
        CastT* operator()(WrapperT* p) const { return (CastT*)p->getGdipObject(); }
    };
};

template<class T>
struct GdipTypeConvertor
{
    typedef typename ncbTypeConvertor::Stripper<T>::Type GdipClassT;
    typedef T* GdipClassP;
    typedef GdipWrapper<GdipClassT> WrapperT;
    typedef ncbInstanceAdaptor<WrapperT> AdaptorT;

protected:
    GdipClassT* result;

public:
    GdipTypeConvertor() : result(NULL) {}
    ~GdipTypeConvertor() { delete result; }

    void operator()(GdipClassP& dst, const tTJSVariant& src)
    {
        WrapperT* obj;
        if (src.Type() == tvtObject && (obj = AdaptorT::GetNativeInstance(src.AsObjectNoAddRef())))
            dst = obj->getGdipObject();
    }

    void operator()(tTJSVariant& dst, const GdipClassP& src)
    {
        if (src != NULL)
        {
            iTJSDispatch2* adpobj = AdaptorT::CreateAdaptor(new WrapperT(src));
            if (adpobj)
            {
                dst = tTJSVariant(adpobj, adpobj);
                adpobj->Release();
            }
        }
        else
        {
            dst.Clear();
        }
    }
};

#define NCB_GDIP_CONVERTOR(type)                                          \
    NCB_SET_CONVERTOR(type*, GdipTypeConvertor<type>);                     \
    NCB_SET_CONVERTOR(const type*, GdipTypeConvertor<const type>)

#define NCB_GDIP_CONVERTOR2(type, convertor)                              \
    NCB_SET_CONVERTOR(type*, convertor<type>);                             \
    NCB_SET_CONVERTOR(const type*, convertor<const type>)

#define NCB_REGISTER_GDIP_SUBCLASS(Class)                                  \
    NCB_GDIP_CONVERTOR(Class);                                             \
    NCB_REGISTER_SUBCLASS(GdipWrapper<Class>)                              \
    {                                                                      \
        typedef Class GdipClass;
#define NCB_REGISTER_GDIP_SUBCLASS2(Class, Convertor)                      \
    NCB_GDIP_CONVERTOR2(Class, Convertor);                                 \
    NCB_REGISTER_SUBCLASS(GdipWrapper<Class>)                              \
    {                                                                      \
        typedef Class GdipClass;
#define NCB_GDIP_METHOD(name) \
    Method(TJS_W(#name), &GdipClass::name, Bridge<GdipWrapper<GdipClass>::BridgeFunctor>())
#define NCB_GDIP_MCAST(ret, method, args) static_cast<ret (GdipClass::*) args>(&GdipClass::method)
#define NCB_GDIP_METHOD2(name, ret, method, args) \
    Method(TJS_W(#name), NCB_GDIP_MCAST(ret, method, args), \
           Bridge<GdipWrapper<GdipClass>::BridgeFunctor>())
#define NCB_GDIP_PROPERTY(name, get, set)   \
    Property(TJS_W(#name), &GdipClass::get, &GdipClass::set, \
             Bridge<GdipWrapper<GdipClass>::BridgeFunctor>())
#define NCB_GDIP_PROPERTY_RO(name, get)     \
    Property(TJS_W(#name), &GdipClass::get, (int)0, Bridge<GdipWrapper<GdipClass>::BridgeFunctor>())
#define NCB_GDIP_MEMBER_PROPERTY(name, type, membername)                       \
    struct AutoProp_##name                                                      \
    {                                                                           \
        static void ProxySet(GdipClass* inst, type value) { inst->membername = value; } \
        static type ProxyGet(GdipClass* inst) { return inst->membername; }       \
    };                                                                          \
    Property(TJS_W(#name), AutoProp_##name::ProxyGet, AutoProp_##name::ProxySet, \
             Bridge<GdipWrapper<GdipClass>::BridgeFunctor>())

// ------------------------------------------------------- Matrix
template<class T>
struct MatrixConvertor : public GdipTypeConvertor<T>
{
    void operator()(T*& dst, const tTJSVariant& src)
    {
        typename MatrixConvertor::WrapperT* obj;
        if (src.Type() == tvtObject)
        {
            if ((obj = MatrixConvertor::AdaptorT::GetNativeInstance(src.AsObjectNoAddRef())))
            {
                dst = obj->getGdipObject();
            }
            else
            {
                ncbPropAccessor info(src);
                plutovg_matrix_t m;
                if (IsArray(src))
                {
                    plutovg_matrix_init(&m, (float)info.getRealValue(0),
                                        (float)info.getRealValue(1), (float)info.getRealValue(2),
                                        (float)info.getRealValue(3), (float)info.getRealValue(4),
                                        (float)info.getRealValue(5));
                }
                else
                {
                    plutovg_matrix_init(&m, (float)info.getRealValue(TJS_W("m11")),
                                        (float)info.getRealValue(TJS_W("m12")),
                                        (float)info.getRealValue(TJS_W("m21")),
                                        (float)info.getRealValue(TJS_W("m22")),
                                        (float)info.getRealValue(TJS_W("dx")),
                                        (float)info.getRealValue(TJS_W("dy")));
                }
                this->result = new GdipMatrix(m);
                dst = this->result;
            }
        }
    }
};

static tjs_error MatrixFactory(GdipWrapper<GdipMatrix>** result, tjs_int numparams,
                               tTJSVariant** params, iTJSDispatch2* objthis)
{
    plutovg_matrix_t m;
    if (numparams == 0)
    {
        plutovg_matrix_init_identity(&m);
    }
    else if (numparams == 6)
    {
        plutovg_matrix_init(&m, (float)params[0]->AsReal(), (float)params[1]->AsReal(),
                            (float)params[2]->AsReal(), (float)params[3]->AsReal(),
                            (float)params[4]->AsReal(), (float)params[5]->AsReal());
    }
    else
    {
        return TJS_E_INVALIDPARAM;
    }
    *result = new GdipWrapper<GdipMatrix>(new GdipMatrix(m));
    return TJS_S_OK;
}

NCB_REGISTER_GDIP_SUBCLASS2(GdipMatrix, MatrixConvertor)
    Factory(MatrixFactory);
    NCB_GDIP_METHOD(OffsetX);
    NCB_GDIP_METHOD(OffsetY);
    NCB_GDIP_METHOD(Equals);
    NCB_GDIP_METHOD(SetElements);
    NCB_GDIP_METHOD(GetLastStatus);
    NCB_GDIP_METHOD(Invert);
    NCB_GDIP_METHOD(IsIdentity);
    NCB_GDIP_METHOD(IsInvertible);
    NCB_GDIP_METHOD(Multiply);
    NCB_GDIP_METHOD(Reset);
    NCB_GDIP_METHOD(Rotate);
    NCB_GDIP_METHOD(RotateAt);
    NCB_GDIP_METHOD(Scale);
    NCB_GDIP_METHOD(Shear);
    NCB_GDIP_METHOD(Translate);
};

// ------------------------------------------------------- Image
template<class T>
struct ImageConvertor : public GdipTypeConvertor<T>
{
    void operator()(T*& dst, const tTJSVariant& src)
    {
        if (src.Type() == tvtObject)
        {
            typename ImageConvertor::WrapperT* obj;
            if ((obj = ImageConvertor::AdaptorT::GetNativeInstance(src.AsObjectNoAddRef())))
            {
                dst = obj->getGdipObject();
            }
            else
            {
                LayerExDraw* layer =
                    ncbInstanceAdaptor<LayerExDraw>::GetNativeInstance(src.AsObjectNoAddRef());
                if (layer)
                    dst = *layer;
            }
        }
        else if (src.Type() == tvtString)
        {
            plutovg_surface_t* surf = loadImage(src.GetString());
            if (surf)
                dst = this->result = new GdipImage(surf);
        }
    }
};

static tjs_error ImageFactory(GdipWrapper<GdipImage>** result, tjs_int numparams,
                              tTJSVariant** params, iTJSDispatch2* objthis)
{
    if (numparams == 0)
    {
        *result = new GdipWrapper<GdipImage>();
        return TJS_S_OK;
    }
    else if (numparams > 0 && params[0]->Type() == tvtString)
    {
        plutovg_surface_t* surf = loadImage(params[0]->GetString());
        if (surf)
        {
            *result = new GdipWrapper<GdipImage>(new GdipImage(surf));
            plutovg_surface_destroy(surf); // GdipImage takes a reference
            return TJS_S_OK;
        }
        else
        {
            TVPThrowExceptionMessage(TJS_W("cannot open:%1"), *params[0]);
        }
    }
    return TJS_E_INVALIDPARAM;
}

static void ImageLoad(GdipWrapper<GdipImage>* obj, const tjs_char* filename)
{
    plutovg_surface_t* surf = loadImage(filename);
    if (surf)
    {
        GdipImage* image = new GdipImage(surf);
        plutovg_surface_destroy(surf);
        obj->setGdipObject(image);
    }
    else
    {
        TVPThrowExceptionMessage(TJS_W("cannot open:%1"), ttstr(filename));
    }
}

static tTJSVariant ImageClone(GdipWrapper<GdipImage>* obj)
{
    typedef GdipWrapper<GdipImage> WrapperT;
    typedef ncbInstanceAdaptor<WrapperT> AdaptorT;
    tTJSVariant ret;
    GdipImage* src = obj->getGdipObject()->Clone();
    if (src)
    {
        iTJSDispatch2* adpobj = AdaptorT::CreateAdaptor(new WrapperT(src));
        if (adpobj)
        {
            ret = tTJSVariant(adpobj, adpobj);
            adpobj->Release();
        }
        else
        {
            delete src;
        }
    }
    return ret;
}

static tTJSVariant ImageBounds(GdipWrapper<GdipImage>* obj)
{
    typedef ncbInstanceAdaptor<RectF> AdaptorT;
    tTJSVariant ret;
    RectF src = obj->getGdipObject()->GetBounds();
    RectF* bounds = new RectF(src);
    iTJSDispatch2* adpobj = AdaptorT::CreateAdaptor(bounds);
    if (adpobj)
    {
        ret = tTJSVariant(adpobj, adpobj);
        adpobj->Release();
    }
    else
    {
        delete bounds;
    }
    return ret;
}

NCB_REGISTER_GDIP_SUBCLASS2(GdipImage, ImageConvertor)
    Factory(ImageFactory);
    NCB_METHOD_PROXY(load, ImageLoad);
    NCB_METHOD_PROXY(Clone, ImageClone);
    NCB_METHOD_PROXY(GetBounds, ImageBounds);
    NCB_GDIP_METHOD(GetFlags);
    NCB_GDIP_METHOD(GetHeight);
    NCB_GDIP_METHOD(GetHorizontalResolution);
    NCB_GDIP_METHOD(GetLastStatus);
    NCB_GDIP_METHOD(GetPixelFormat);
    NCB_GDIP_METHOD(GetType);
    NCB_GDIP_METHOD(GetVerticalResolution);
    NCB_GDIP_METHOD(GetWidth);
    NCB_GDIP_METHOD(RotateFlip);
};

// ------------------------------------------------------- Subclass registrations
NCB_REGISTER_SUBCLASS(FontInfo)
{
    NCB_CONSTRUCTOR((const tjs_char*, tjs_real, tjs_int));
    NCB_PROPERTY(familyName, getFamilyName, setFamilyName);
    NCB_PROPERTY(emSize, getEmSize, setEmSize);
    NCB_PROPERTY(style, getStyle, setStyle);
    NCB_PROPERTY(forceSelfPathDraw, getForceSelfPathDraw, setForceSelfPathDraw);
    NCB_PROPERTY_RO(ascent, getAscent);
    NCB_PROPERTY_RO(descent, getDescent);
    NCB_PROPERTY_RO(ascentLeading, getAscentLeading);
    NCB_PROPERTY_RO(descentLeading, getDescentLeading);
    NCB_PROPERTY_RO(lineSpacing, getLineSpacing);
};

NCB_REGISTER_SUBCLASS(Appearance)
{
    NCB_CONSTRUCTOR(());
    NCB_METHOD(clear);
    NCB_METHOD(addBrush);
    NCB_METHOD(addPen);
};

NCB_REGISTER_SUBCLASS(Path)
{
    NCB_CONSTRUCTOR(());
    NCB_METHOD(startFigure);
    NCB_METHOD(closeFigure);
    NCB_METHOD(drawArc);
    NCB_METHOD(drawPie);
    NCB_METHOD(drawBezier);
    NCB_METHOD(drawBeziers);
    NCB_METHOD(drawClosedCurve);
    NCB_METHOD(drawClosedCurve2);
    NCB_METHOD(drawCurve);
    NCB_METHOD(drawCurve2);
    NCB_METHOD(drawCurve3);
    NCB_METHOD(drawEllipse);
    NCB_METHOD(drawLine);
    NCB_METHOD(drawLines);
    NCB_METHOD(drawPolygon);
    NCB_METHOD(drawRectangle);
    NCB_METHOD(drawRectangles);
};

// ------------------------------------------------------- GdiPlus namespace
#define ENUM(n) Variant(#n, (int)n)
#define NCB_SUBCLASS_NAME(name) NCB_SUBCLASS(name, name)
#define NCB_GDIP_SUBCLASS(Class) NCB_SUBCLASS(Class, GdipWrapper<Class>)

NCB_REGISTER_CLASS(GdiPlus)
{
    // Status
    ENUM(Ok); ENUM(GenericError); ENUM(InvalidParameter); ENUM(OutOfMemory);
    ENUM(ObjectBusy); ENUM(InsufficientBuffer); ENUM(NotImplemented);
    ENUM(Win32Error); ENUM(WrongState); ENUM(Aborted); ENUM(FileNotFound);
    ENUM(ValueOverflow); ENUM(AccessDenied); ENUM(UnknownImageFormat);
    ENUM(FontFamilyNotFound); ENUM(FontStyleNotFound); ENUM(NotTrueTypeFont);
    ENUM(UnsupportedGdiplusVersion); ENUM(GdiplusNotInitialized);
    ENUM(PropertyNotFound); ENUM(PropertyNotSupported); ENUM(ProfileNotFound);

    // FontStyle
    ENUM(FontStyleRegular); ENUM(FontStyleBold); ENUM(FontStyleItalic);
    ENUM(FontStyleBoldItalic); ENUM(FontStyleUnderline); ENUM(FontStyleStrikeout);

    // BrushType
    ENUM(BrushTypeSolidColor); ENUM(BrushTypeHatchFill); ENUM(BrushTypeTextureFill);
    ENUM(BrushTypePathGradient); ENUM(BrushTypeLinearGradient);

    // DashCap
    ENUM(DashCapFlat); ENUM(DashCapRound); ENUM(DashCapTriangle);

    // DashStyle
    ENUM(DashStyleSolid); ENUM(DashStyleDash); ENUM(DashStyleDot);
    ENUM(DashStyleDashDot); ENUM(DashStyleDashDotDot);

    // HatchStyle
    ENUM(HatchStyleHorizontal); ENUM(HatchStyleVertical);
    ENUM(HatchStyleForwardDiagonal); ENUM(HatchStyleBackwardDiagonal);
    ENUM(HatchStyleCross); ENUM(HatchStyleDiagonalCross);
    ENUM(HatchStyle05Percent); ENUM(HatchStyle10Percent); ENUM(HatchStyle20Percent);
    ENUM(HatchStyle25Percent); ENUM(HatchStyle30Percent); ENUM(HatchStyle40Percent);
    ENUM(HatchStyle50Percent); ENUM(HatchStyle60Percent); ENUM(HatchStyle70Percent);
    ENUM(HatchStyle75Percent); ENUM(HatchStyle80Percent); ENUM(HatchStyle90Percent);
    ENUM(HatchStyleLightDownwardDiagonal); ENUM(HatchStyleLightUpwardDiagonal);
    ENUM(HatchStyleDarkDownwardDiagonal); ENUM(HatchStyleDarkUpwardDiagonal);
    ENUM(HatchStyleWideDownwardDiagonal); ENUM(HatchStyleWideUpwardDiagonal);
    ENUM(HatchStyleLightVertical); ENUM(HatchStyleLightHorizontal);
    ENUM(HatchStyleNarrowVertical); ENUM(HatchStyleNarrowHorizontal);
    ENUM(HatchStyleDarkVertical); ENUM(HatchStyleDarkHorizontal);
    ENUM(HatchStyleDashedDownwardDiagonal); ENUM(HatchStyleDashedUpwardDiagonal);
    ENUM(HatchStyleDashedHorizontal); ENUM(HatchStyleDashedVertical);
    ENUM(HatchStyleSmallConfetti); ENUM(HatchStyleLargeConfetti);
    ENUM(HatchStyleZigZag); ENUM(HatchStyleWave);
    ENUM(HatchStyleDiagonalBrick); ENUM(HatchStyleHorizontalBrick);
    ENUM(HatchStyleWeave); ENUM(HatchStylePlaid); ENUM(HatchStyleDivot);
    ENUM(HatchStyleDottedGrid); ENUM(HatchStyleDottedDiamond);
    ENUM(HatchStyleShingle); ENUM(HatchStyleTrellis); ENUM(HatchStyleSphere);
    ENUM(HatchStyleSmallGrid); ENUM(HatchStyleSmallCheckerBoard);
    ENUM(HatchStyleLargeCheckerBoard); ENUM(HatchStyleOutlinedDiamond);
    ENUM(HatchStyleSolidDiamond); ENUM(HatchStyleTotal);
    ENUM(HatchStyleLargeGrid); ENUM(HatchStyleMin); ENUM(HatchStyleMax);

    // LinearGradientMode
    ENUM(LinearGradientModeHorizontal); ENUM(LinearGradientModeVertical);
    ENUM(LinearGradientModeForwardDiagonal); ENUM(LinearGradientModeBackwardDiagonal);

    // LineCap
    ENUM(LineCapFlat); ENUM(LineCapSquare); ENUM(LineCapRound); ENUM(LineCapTriangle);
    ENUM(LineCapNoAnchor); ENUM(LineCapSquareAnchor); ENUM(LineCapRoundAnchor);
    ENUM(LineCapDiamondAnchor); ENUM(LineCapArrowAnchor);

    // LineJoin
    ENUM(LineJoinMiter); ENUM(LineJoinBevel); ENUM(LineJoinRound); ENUM(LineJoinMiterClipped);

    // PenAlignment
    ENUM(PenAlignmentCenter); ENUM(PenAlignmentInset);

    // WrapMode
    ENUM(WrapModeTile); ENUM(WrapModeTileFlipX); ENUM(WrapModeTileFlipY);
    ENUM(WrapModeTileFlipXY); ENUM(WrapModeClamp);

    // MatrixOrder
    ENUM(MatrixOrderPrepend); ENUM(MatrixOrderAppend);

    // ImageType
    ENUM(ImageTypeUnknown); ENUM(ImageTypeBitmap); ENUM(ImageTypeMetafile);

    // RotateFlipType
    ENUM(RotateNoneFlipNone); ENUM(Rotate90FlipNone); ENUM(Rotate180FlipNone);
    ENUM(Rotate270FlipNone); ENUM(RotateNoneFlipX); ENUM(Rotate90FlipX);
    ENUM(Rotate180FlipX); ENUM(Rotate270FlipX);
    ENUM(RotateNoneFlipY); ENUM(Rotate90FlipY); ENUM(Rotate180FlipY); ENUM(Rotate270FlipY);
    ENUM(RotateNoneFlipXY); ENUM(Rotate90FlipXY); ENUM(Rotate180FlipXY); ENUM(Rotate270FlipXY);

    // SmoothingMode
    ENUM(SmoothingModeInvalid); ENUM(SmoothingModeDefault);
    ENUM(SmoothingModeHighSpeed); ENUM(SmoothingModeHighQuality);
    ENUM(SmoothingModeNone); ENUM(SmoothingModeAntiAlias);

    // TextRenderingHint
    ENUM(TextRenderingHintSystemDefault); ENUM(TextRenderingHintSingleBitPerPixelGridFit);
    ENUM(TextRenderingHintSingleBitPerPixel); ENUM(TextRenderingHintAntiAliasGridFit);
    ENUM(TextRenderingHintAntiAlias); ENUM(TextRenderingHintClearTypeGridFit);

    // Static methods
    NCB_METHOD(addPrivateFont);
    NCB_METHOD(getFontList);

    // Subclasses
    NCB_SUBCLASS_NAME(PointF);
    NCB_SUBCLASS_NAME(RectF);
    SubClass(TJS_W("Image"), TypeWrap<GdipWrapper<GdipImage>>());
    SubClass(TJS_W("Matrix"), TypeWrap<GdipWrapper<GdipMatrix>>());
    NCB_SUBCLASS(Font, FontInfo);
    NCB_SUBCLASS(Appearance, Appearance);
    NCB_SUBCLASS(Path, Path);
}

// ------------------------------------------------------- LayerExDraw hook
NCB_GET_INSTANCE_HOOK(LayerExDraw)
{
    NCB_INSTANCE_GETTER(objthis)
    {
        ClassT* obj = GetNativeInstance(objthis);
        if (!obj)
        {
            obj = new ClassT(objthis);
            SetNativeInstance(objthis, obj);
        }
        obj->reset();
        return obj;
    }
    ~NCB_GET_INSTANCE_HOOK_CLASS() {}
};

#define LAYEREX_METHOD(type, name) \
    Method(TJS_W(#name), &Type::name, Bridge<LayerExDraw::BridgeFunctor<type>>())

static tjs_error GetRecordImage(tTJSVariant* result, tjs_int numparams, tTJSVariant** param,
                                iTJSDispatch2* objthis)
{
    LayerExDraw* obj = ncbInstanceAdaptor<LayerExDraw>::GetNativeInstance(objthis, true);
    if (result)
        result->Clear();
    if (obj)
    {
        GdipImage* image = obj->getRecordImage();
        if (image)
        {
            typedef GdipWrapper<GdipImage> WrapperT;
            WrapperT* wrap = new WrapperT(image);
            iTJSDispatch2* adpobj = ncbInstanceAdaptor<WrapperT>::CreateAdaptor(wrap);
            if (adpobj)
            {
                if (result)
                    *result = tTJSVariant(adpobj, adpobj);
                adpobj->Release();
            }
            else
            {
                delete wrap;
                delete image;
            }
        }
    }
    return TJS_S_OK;
}

NCB_ATTACH_CLASS_WITH_HOOK(LayerExDraw, Layer)
{
    NCB_PROPERTY(updateWhenDraw, getUpdateWhenDraw, setUpdateWhenDraw);
    NCB_PROPERTY(smoothingMode, getSmoothingMode, setSmoothingMode);
    NCB_PROPERTY(textRenderingHint, getTextRenderingHint, setTextRenderingHint);

    NCB_METHOD(setViewTransform);
    NCB_METHOD(resetViewTransform);
    NCB_METHOD(rotateViewTransform);
    NCB_METHOD(scaleViewTransform);
    NCB_METHOD(translateViewTransform);

    NCB_METHOD(setTransform);
    NCB_METHOD(resetTransform);
    NCB_METHOD(rotateTransform);
    NCB_METHOD(scaleTransform);
    NCB_METHOD(translateTransform);

    NCB_METHOD(clear);
    NCB_METHOD(drawPath);
    NCB_METHOD(drawArc);
    NCB_METHOD(drawPie);
    NCB_METHOD(drawBezier);
    NCB_METHOD(drawBeziers);
    NCB_METHOD(drawClosedCurve);
    NCB_METHOD(drawClosedCurve2);
    NCB_METHOD(drawCurve);
    NCB_METHOD(drawCurve2);
    NCB_METHOD(drawCurve3);
    NCB_METHOD(drawEllipse);
    NCB_METHOD(drawLine);
    NCB_METHOD(drawLines);
    NCB_METHOD(drawPolygon);
    NCB_METHOD(drawRectangle);
    NCB_METHOD(drawRectangles);
    NCB_METHOD(drawPathString);
    NCB_METHOD(drawString);
    NCB_METHOD(measureString);
    NCB_METHOD(measureStringInternal);

    NCB_METHOD(drawImage);
    NCB_METHOD(drawImageRect);
    NCB_METHOD(drawImageStretch);
    NCB_METHOD(drawImageAffine);

    NCB_PROPERTY(record, getRecord, setRecord);
    NCB_METHOD_RAW_CALLBACK(getRecordImage, GetRecordImage, 0);
    NCB_METHOD(redrawRecord);
    NCB_METHOD(saveRecord);
    NCB_METHOD(loadRecord);
}

NCB_PRE_REGIST_CALLBACK(initGdiPlus);
NCB_POST_UNREGIST_CALLBACK(deInitGdiPlus);

// krkrsdl2common is a static archive: the registrations above only exist as
// static initializers, so the linker would drop this unit without an anchor.
// PluginImpl calls this before ncbAutoRegister::LoadModule.
extern "C" void krkrsdl2_link_layerexdraw_plugin()
{
}
