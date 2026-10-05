#ifndef _layerExText_hpp_
#define _layerExText_hpp_

#include <vector>

#include "LayerExBase.hpp"
#include "DrawBackend.h"

// gdiplus enum
#define PixelFormatIndexed 0x00010000
#define PixelFormatGDI 0x00020000
#define PixelFormatAlpha 0x00040000
#define PixelFormatPAlpha 0x00080000
#define PixelFormatExtended 0x00100000
#define PixelFormatCanonical 0x00200000
#define PixelFormatUndefined 0
#define PixelFormatDontCare 0
#define PixelFormat1bppIndexed (1 | (1 << 8) | PixelFormatIndexed | PixelFormatGDI)
#define PixelFormat4bppIndexed (2 | (4 << 8) | PixelFormatIndexed | PixelFormatGDI)
#define PixelFormat8bppIndexed (3 | (8 << 8) | PixelFormatIndexed | PixelFormatGDI)
#define PixelFormat16bppGrayScale (4 | (16 << 8) | PixelFormatExtended)
#define PixelFormat16bppRGB555 (5 | (16 << 8) | PixelFormatGDI)
#define PixelFormat16bppRGB565 (6 | (16 << 8) | PixelFormatGDI)
#define PixelFormat16bppARGB1555 (7 | (16 << 8) | PixelFormatAlpha | PixelFormatGDI)
#define PixelFormat24bppRGB (8 | (24 << 8) | PixelFormatGDI)
#define PixelFormat32bppRGB (9 | (32 << 8) | PixelFormatGDI)
#define PixelFormat32bppARGB \
    (10 | (32 << 8) | PixelFormatAlpha | PixelFormatGDI | PixelFormatCanonical)
#define PixelFormat32bppPARGB \
    (11 | (32 << 8) | PixelFormatAlpha | PixelFormatPAlpha | PixelFormatGDI)
#define PixelFormat48bppRGB (12 | (48 << 8) | PixelFormatExtended)
#define PixelFormat64bppARGB \
    (13 | (64 << 8) | PixelFormatAlpha | PixelFormatCanonical | PixelFormatExtended)
#define PixelFormat64bppPARGB \
    (14 | (64 << 8) | PixelFormatAlpha | PixelFormatPAlpha | PixelFormatExtended)
#define PixelFormat32bppCMYK (15 | (32 << 8))
#define PixelFormatMax 16
typedef enum
{
    ImageFlagsNone = 0,
    ImageFlagsScalable = 0x0001,
    ImageFlagsHasAlpha = 0x0002,
    ImageFlagsHasTranslucent = 0x0004,
    ImageFlagsPartiallyScalable = 0x0008,
    ImageFlagsColorSpaceRGB = 0x0010,
    ImageFlagsColorSpaceCMYK = 0x0020,
    ImageFlagsColorSpaceGRAY = 0x0040,
    ImageFlagsColorSpaceYCBCR = 0x0080,
    ImageFlagsColorSpaceYCCK = 0x0100,
    ImageFlagsHasRealDPI = 0x1000,
    ImageFlagsHasRealPixelSize = 0x2000,
    ImageFlagsReadOnly = 0x00010000,
    ImageFlagsCaching = 0x00020000,
    ImageFlagsUndocumented = 0x00040000
} ImageFlags;
enum Status
{
    Ok = 0,
    GenericError = 1,
    InvalidParameter = 2,
    OutOfMemory = 3,
    ObjectBusy = 4,
    InsufficientBuffer = 5,
    NotImplemented = 6,
    Win32Error = 7,
    WrongState = 8,
    Aborted = 9,
    FileNotFound = 10,
    ValueOverflow = 11,
    AccessDenied = 12,
    UnknownImageFormat = 13,
    FontFamilyNotFound = 14,
    FontStyleNotFound = 15,
    NotTrueTypeFont = 16,
    UnsupportedGdiplusVersion = 17,
    GdiplusNotInitialized = 18,
    PropertyNotFound = 19,
    PropertyNotSupported = 20,
    ProfileNotFound = 21
};
enum FontStyle
{
    FontStyleRegular = 0,
    FontStyleBold = 1,
    FontStyleItalic = 2,
    FontStyleBoldItalic = 3,
    FontStyleUnderline = 4,
    FontStyleStrikeout = 8
};
enum DashCap
{
    DashCapFlat = 0,
    DashCapRound = 2,
    DashCapTriangle = 3
};
enum DashStyle
{
    DashStyleSolid,
    DashStyleDash,
    DashStyleDot,
    DashStyleDashDot,
    DashStyleDashDotDot,
    DashStyleCustom
};
// BrushType is defined in DrawBackend.h
enum HatchStyle
{
    HatchStyleHorizontal,
    HatchStyleVertical,
    HatchStyleForwardDiagonal,
    HatchStyleBackwardDiagonal,
    HatchStyleCross,
    HatchStyleDiagonalCross,
    HatchStyle05Percent,
    HatchStyle10Percent,
    HatchStyle20Percent,
    HatchStyle25Percent,
    HatchStyle30Percent,
    HatchStyle40Percent,
    HatchStyle50Percent,
    HatchStyle60Percent,
    HatchStyle70Percent,
    HatchStyle75Percent,
    HatchStyle80Percent,
    HatchStyle90Percent,
    HatchStyleLightDownwardDiagonal,
    HatchStyleLightUpwardDiagonal,
    HatchStyleDarkDownwardDiagonal,
    HatchStyleDarkUpwardDiagonal,
    HatchStyleWideDownwardDiagonal,
    HatchStyleWideUpwardDiagonal,
    HatchStyleLightVertical,
    HatchStyleLightHorizontal,
    HatchStyleNarrowVertical,
    HatchStyleNarrowHorizontal,
    HatchStyleDarkVertical,
    HatchStyleDarkHorizontal,
    HatchStyleDashedDownwardDiagonal,
    HatchStyleDashedUpwardDiagonal,
    HatchStyleDashedHorizontal,
    HatchStyleDashedVertical,
    HatchStyleSmallConfetti,
    HatchStyleLargeConfetti,
    HatchStyleZigZag,
    HatchStyleWave,
    HatchStyleDiagonalBrick,
    HatchStyleHorizontalBrick,
    HatchStyleWeave,
    HatchStylePlaid,
    HatchStyleDivot,
    HatchStyleDottedGrid,
    HatchStyleDottedDiamond,
    HatchStyleShingle,
    HatchStyleTrellis,
    HatchStyleSphere,
    HatchStyleSmallGrid,
    HatchStyleSmallCheckerBoard,
    HatchStyleLargeCheckerBoard,
    HatchStyleOutlinedDiamond,
    HatchStyleSolidDiamond,
    HatchStyleTotal,
    HatchStyleLargeGrid = HatchStyleCross,
    HatchStyleMin = HatchStyleHorizontal,
    HatchStyleMax = HatchStyleTotal - 1,
};
enum WrapMode
{
    WrapModeTile,
    WrapModeTileFlipX,
    WrapModeTileFlipY,
    WrapModeTileFlipXY,
    WrapModeClamp
};
enum LinearGradientMode
{
    LinearGradientModeHorizontal,
    LinearGradientModeVertical,
    LinearGradientModeForwardDiagonal,
    LinearGradientModeBackwardDiagonal
};
enum LineCap
{
    LineCapFlat = 0,
    LineCapSquare = 1,
    LineCapRound = 2,
    LineCapTriangle = 3,
    LineCapNoAnchor = 0x10,
    LineCapSquareAnchor = 0x11,
    LineCapRoundAnchor = 0x12,
    LineCapDiamondAnchor = 0x13,
    LineCapArrowAnchor = 0x14,
    LineCapCustom = 0xff,
    LineCapAnchorMask = 0xf0
};
enum LineJoin
{
    LineJoinMiter = 0,
    LineJoinBevel = 1,
    LineJoinRound = 2,
    LineJoinMiterClipped = 3
};
enum PenAlignment
{
    PenAlignmentCenter = 0,
    PenAlignmentInset = 1
};
enum MatrixOrder
{
    MatrixOrderPrepend = 0,
    MatrixOrderAppend = 1
};
enum ImageType
{
    ImageTypeUnknown,
    ImageTypeBitmap,
    ImageTypeMetafile
};
enum RotateFlipType
{
    RotateNoneFlipNone = 0,
    Rotate90FlipNone = 1,
    Rotate180FlipNone = 2,
    Rotate270FlipNone = 3,
    RotateNoneFlipX = 4,
    Rotate90FlipX = 5,
    Rotate180FlipX = 6,
    Rotate270FlipX = 7,
    RotateNoneFlipY = Rotate180FlipX,
    Rotate90FlipY = Rotate270FlipX,
    Rotate180FlipY = RotateNoneFlipX,
    Rotate270FlipY = Rotate90FlipX,
    RotateNoneFlipXY = Rotate180FlipNone,
    Rotate90FlipXY = Rotate270FlipNone,
    Rotate180FlipXY = RotateNoneFlipNone,
    Rotate270FlipXY = Rotate90FlipNone
};
enum QualityMode
{
    QualityModeInvalid = -1,
    QualityModeDefault = 0,
    QualityModeLow = 1,
    QualityModeHigh = 2
};
enum SmoothingMode
{
    SmoothingModeInvalid = QualityModeInvalid,
    SmoothingModeDefault = QualityModeDefault,
    SmoothingModeHighSpeed = QualityModeLow,
    SmoothingModeHighQuality = QualityModeHigh,
    SmoothingModeNone,
    SmoothingModeAntiAlias,
    SmoothingModeAntiAlias8x4 = SmoothingModeAntiAlias,
    SmoothingModeAntiAlias8x8
};
enum TextRenderingHint
{
    TextRenderingHintSystemDefault = 0,
    TextRenderingHintSingleBitPerPixelGridFit,
    TextRenderingHintSingleBitPerPixel,
    TextRenderingHintAntiAliasGridFit,
    TextRenderingHintAntiAlias,
    TextRenderingHintClearTypeGridFit
};

// ============================================================
// GDI+ struct types
// ============================================================

class PointF
{
public:
    tjs_real x;
    tjs_real y;
    PointF() : x(0), y(0) {};
    PointF(tjs_real _x, tjs_real _y) : x(_x), y(_y) {}
    bool Equals(PointF tgt) { return x == tgt.x && y == tgt.y; };
    PointF operator+(PointF tgt) { return PointF(x + tgt.x, y + tgt.y); }
    PointF operator-(PointF tgt) { return PointF(x - tgt.x, y - tgt.y); }
    PointF operator*(tjs_real tgt) { return PointF(x * tgt, y * tgt); }
    PointF operator/(tjs_real tgt) { return PointF(x / tgt, y / tgt); }
};

class RectF
{
public:
    tjs_real x = 0, y = 0, w = 0, h = 0;
    RectF() {};
    RectF(tjs_real _x, tjs_real _y, tjs_real _w, tjs_real _h) { x = _x, y = _y, w = _w, h = _h; }
    void reset(tjs_real _x, tjs_real _y, tjs_real _w, tjs_real _h)
    {
        x = _x, y = _y, w = _w, h = _h;
    }
    tjs_real GetLeft() { return x; }
    tjs_real GetTop() { return y; }
    tjs_real GetRight() { return x + w; }
    tjs_real GetBottom() { return y + h; }
    void GetLocation(PointF& p) { p.x = x, p.y = y; }
    void GetBounds(RectF& p) { p.x = x, p.y = y, p.w = x + w, p.h = x + h; }
    RectF Clone() { return RectF(x, y, w, h); }
    bool Equals(RectF p) { return x == p.x && y == p.y && w == p.w && h == p.h; };
    void Inflate(tjs_real width, tjs_real height)
    {
        x -= width;
        y -= height;
        w += width * 2;
        h += height * 2;
    }
    void Inflate(const PointF& point) { Inflate(point.x, point.y); }
    bool IntersectsWith(const RectF& rect)
    {
        return !(rect.x > x + w || rect.x + rect.w < x || rect.y > y + h || rect.y + rect.h < y);
    }
    bool IsEmptyArea() const { return w <= 0 || h <= 0; }
    void Offset(tjs_real dx, tjs_real dy)
    {
        x += dx;
        y += dy;
    }
    static bool Union(RectF& ret, const RectF& a, const RectF& b)
    {
        float minX = std::min(a.x, b.x);
        float minY = std::min(a.y, b.y);
        float maxX = std::max(a.x + a.w, b.x + b.w);
        float maxY = std::max(a.y + a.h, b.y + b.h);
        ret = RectF(minX, minY, maxX - minX, maxY - minY);
        return true;
    }
    static bool Intersect(RectF& ret, const RectF& a, const RectF& b)
    {
        tjs_real left = std::max(a.x, b.x);
        tjs_real top = std::max(a.y, b.y);
        tjs_real right = std::min(a.x + a.w, b.x + b.w);
        tjs_real bottom = std::min(a.y + a.h, b.y + b.h);
        if (right > left && bottom > top)
        {
            ret = RectF(left, top, right - left, bottom - top);
            return true;
        }
        ret = RectF();
        return false;
    }
    bool Contains(tjs_real px, tjs_real py)
    {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }
    bool Contains(const RectF& rect)
    {
        return rect.x >= x && rect.x + rect.w <= x + w && rect.y >= y &&
               rect.y + rect.h <= y + h;
    }
};

// ============================================================
// GdipMatrix — wraps plutovg_matrix_t
// ============================================================
class GdipMatrix
{
public:
    plutovg_matrix_t _core;
    tjs_real offsetx = 0, offsety = 0;
    tjs_real rotation = 0;
    tjs_real scale = 1;
    tjs_real shearx = 0, sheary = 0;

    GdipMatrix() { plutovg_matrix_init_identity(&_core); }
    GdipMatrix(plutovg_matrix_t m) : _core(m) { updateProperties(); }
    GdipMatrix* Clone() { return new GdipMatrix(_core); }

    tjs_real OffsetX() { return offsetx; }
    tjs_real OffsetY() { return offsety; }
    bool Equals(const GdipMatrix& other)
    {
        return memcmp(&_core, &other._core, sizeof(plutovg_matrix_t)) == 0;
    }
    bool getElements(std::vector<tjs_real>& elements)
    {
        elements.resize(6);
        elements[0] = _core.a;
        elements[1] = _core.b;
        elements[2] = _core.c;
        elements[3] = _core.d;
        elements[4] = _core.e;
        elements[5] = _core.f;
        return true;
    }
    Status SetElements(tjs_real m11, tjs_real m12, tjs_real m21, tjs_real m22, tjs_real dx,
                       tjs_real dy)
    {
        plutovg_matrix_init(&_core, m11, m12, m21, m22, dx, dy);
        updateProperties();
        return Ok;
    }
    Status GetLastStatus() { return Ok; }
    Status Invert()
    {
        plutovg_matrix_t inverse;
        if (plutovg_matrix_invert(&_core, &inverse))
        {
            _core = inverse;
            updateProperties();
            return Ok;
        }
        return InvalidParameter;
    }
    bool IsIdentity()
    {
        plutovg_matrix_t identity;
        plutovg_matrix_init_identity(&identity);
        return memcmp(&_core, &identity, sizeof(plutovg_matrix_t)) == 0;
    }
    bool IsInvertible()
    {
        plutovg_matrix_t inverse;
        return plutovg_matrix_invert(&_core, &inverse);
    }
    Status Multiply(GdipMatrix* matrix, MatrixOrder order = MatrixOrderPrepend)
    {
        if (!matrix)
            return InvalidParameter;
        if (order == MatrixOrderPrepend)
        {
            plutovg_matrix_t temp = _core;
            plutovg_matrix_multiply(&_core, &temp, &matrix->_core);
        }
        else
        {
            plutovg_matrix_t temp = _core;
            plutovg_matrix_multiply(&_core, &matrix->_core, &temp);
        }
        updateProperties();
        return Ok;
    }
    void Reset()
    {
        plutovg_matrix_init_identity(&_core);
        offsetx = offsety = 0;
        rotation = 0;
        scale = 1;
        shearx = sheary = 0;
    }
    void Rotate(tjs_real angle, MatrixOrder order = MatrixOrderPrepend)
    {
        float rad = angle * M_PI / 180.0f;
        rotation += rad;
        if (order == MatrixOrderPrepend)
        {
            plutovg_matrix_rotate(&_core, rad);
        }
        else
        {
            plutovg_matrix_t rot;
            plutovg_matrix_init_rotate(&rot, rad);
            plutovg_matrix_t temp = _core;
            plutovg_matrix_multiply(&_core, &rot, &temp);
        }
        updateOffset();
    }
    void RotateAt(tjs_real angle, const PointF& center, MatrixOrder order = MatrixOrderPrepend)
    {
        float rad = angle * M_PI / 180.0f;
        if (order == MatrixOrderPrepend)
        {
            plutovg_matrix_translate(&_core, -center.x, -center.y);
            plutovg_matrix_rotate(&_core, rad);
            plutovg_matrix_translate(&_core, center.x, center.y);
        }
        else
        {
            plutovg_matrix_t mat;
            plutovg_matrix_init_translate(&mat, -center.x, -center.y);
            plutovg_matrix_rotate(&mat, rad);
            plutovg_matrix_translate(&mat, center.x, center.y);
            plutovg_matrix_t temp = _core;
            plutovg_matrix_multiply(&_core, &mat, &temp);
        }
        rotation += rad;
        updateProperties();
    }
    void Scale(tjs_real sx, tjs_real sy, MatrixOrder order = MatrixOrderPrepend)
    {
        scale *= std::max(fabs(sx), fabs(sy));
        if (order == MatrixOrderPrepend)
        {
            plutovg_matrix_scale(&_core, sx, sy);
        }
        else
        {
            plutovg_matrix_t scl;
            plutovg_matrix_init_scale(&scl, sx, sy);
            plutovg_matrix_t temp = _core;
            plutovg_matrix_multiply(&_core, &scl, &temp);
        }
        updateOffset();
    }
    void Shear(tjs_real shx, tjs_real shy, MatrixOrder order = MatrixOrderPrepend)
    {
        shearx += shx;
        sheary += shy;
        if (order == MatrixOrderPrepend)
        {
            plutovg_matrix_shear(&_core, shx, shy);
        }
        else
        {
            plutovg_matrix_t shearMat;
            plutovg_matrix_init(&shearMat, 1, shy, shx, 1, 0, 0);
            plutovg_matrix_t temp = _core;
            plutovg_matrix_multiply(&_core, &shearMat, &temp);
        }
        updateOffset();
    }
    void Translate(tjs_real dx, tjs_real dy, MatrixOrder order = MatrixOrderPrepend)
    {
        offsetx += dx;
        offsety += dy;
        if (order == MatrixOrderPrepend)
        {
            plutovg_matrix_translate(&_core, dx, dy);
        }
        else
        {
            plutovg_matrix_t trans;
            plutovg_matrix_init_translate(&trans, dx, dy);
            plutovg_matrix_t temp = _core;
            plutovg_matrix_multiply(&_core, &trans, &temp);
        }
    }
    PointF TransformPoint(const PointF& point)
    {
        float xx, yy;
        plutovg_matrix_map(&_core, (float)point.x, (float)point.y, &xx, &yy);
        return PointF(xx, yy);
    }
    void TransformPoints(std::vector<PointF>& points)
    {
        for (auto& p : points)
        {
            float xx, yy;
            plutovg_matrix_map(&_core, (float)p.x, (float)p.y, &xx, &yy);
            p.x = xx;
            p.y = yy;
        }
    }
    PointF TransformVector(const PointF& vector)
    {
        plutovg_matrix_t m = _core;
        m.e = m.f = 0;
        float xx, yy;
        plutovg_matrix_map(&m, (float)vector.x, (float)vector.y, &xx, &yy);
        return PointF(xx, yy);
    }

private:
    void updateProperties()
    {
        offsetx = _core.e;
        offsety = _core.f;
        float m00 = _core.a;
        float m01 = _core.b;
        float m10 = _core.c;
        float m11 = _core.d;
        scale = sqrt(m00 * m00 + m01 * m01);
        if (fabs(m00) > 1e-6)
            rotation = atan2(m01, m00);
        else
            rotation = atan2(-m10, m11);
        shearx = m10 / (scale > 1e-6 ? scale : 1.0);
        sheary = m01 / (scale > 1e-6 ? scale : 1.0);
    }
    void updateOffset()
    {
        offsetx = _core.e;
        offsety = _core.f;
    }
};

// ============================================================
// GdipImage — wraps plutovg_surface_t* or vector graph
// ============================================================
class Appearance;
class GdipImage
{
public:
    int type = 0; // 0:bitmap 1:vector
    plutovg_matrix_t transMtx;
    tjs_int width = 0, height = 0;

    // Bitmap data
    plutovg_surface_t* _surface; // owned (refcounted)

    // Vector graph data
    struct Info
    {
        Appearance* app;
        plutovg_path_t* path;
    };
    tjs_uint32 bgColor;
    std::vector<Info> vectorGraph;

    GdipImage(tjs_int w, tjs_int h) : width(w), height(h), _surface(nullptr), bgColor(0)
    {
        type = 1;
        plutovg_matrix_init_identity(&transMtx);
    }
    GdipImage(plutovg_surface_t* surf) : _surface(surf), bgColor(0)
    {
        type = 0;
        if (surf)
        {
            width = plutovg_surface_get_width(surf);
            height = plutovg_surface_get_height(surf);
            plutovg_surface_reference(surf);
        }
        plutovg_matrix_init_identity(&transMtx);
    }
    GdipImage* Clone();
    ~GdipImage();
    RectF GetBounds();
    tjs_uint GetFlags();
    tjs_int GetHeight() { return height; }
    tjs_int GetWidth() { return width; }
    tjs_real GetHorizontalResolution() { return 96.0f; }
    Status GetLastStatus() { return (_surface || type == 1) ? Ok : GenericError; }
    tjs_int GetPixelFormat() { return PixelFormat32bppARGB; }
    ImageType GetType()
    {
        if (type == 0)
            return _surface ? ImageTypeBitmap : ImageTypeUnknown;
        else if (type == 1)
            return ImageTypeMetafile;
        return ImageTypeUnknown;
    }
    tjs_real GetVerticalResolution() { return 96.0f; }
    Status RotateFlip(RotateFlipType rftype);
};

// ============================================================
// GdiPlus — static methods
// ============================================================
struct GdiPlus
{
    static void addPrivateFont(const tjs_char* fontFileName);
    static tTJSVariant getFontList(bool privateOnly);
};

// ============================================================
// FontInfo — font descriptor using plutovg_font_face_t
// ============================================================
class FontInfo
{
    friend class LayerExDraw;

protected:
    ttstr familyName;
    tjs_real emSize;
    tjs_int style;
    bool gdiPlusUnsupportedFont;
    bool forceSelfPathDraw;
    mutable bool propertyModified;
    mutable tjs_real ascent;
    mutable tjs_real descent;
    mutable tjs_real lineSpacing;
    mutable tjs_real ascentLeading;
    mutable tjs_real descentLeading;

    void clear();

public:
    FontInfo();
    FontInfo(const tjs_char* familyName, tjs_real emSize, tjs_int style);
    FontInfo(const FontInfo& orig);
    virtual ~FontInfo();

    void setFamilyName(const tjs_char* familyName);
    const tjs_char* getFamilyName() const { return familyName.c_str(); }
    void setEmSize(tjs_real emSize)
    {
        this->emSize = emSize;
        propertyModified = true;
    }
    tjs_real getEmSize() const { return emSize; }
    void setStyle(tjs_int style)
    {
        this->style = style;
        propertyModified = true;
    }
    tjs_int getStyle() const { return style; }
    void setForceSelfPathDraw(bool state);
    bool getForceSelfPathDraw(void) const;
    bool getSelfPathDraw(void) const;

    void updateSizeParams(void) const;
    tjs_real getAscent() const;
    tjs_real getDescent() const;
    tjs_real getAscentLeading() const;
    tjs_real getDescentLeading() const;
    tjs_real getLineSpacing() const;

    plutovg_font_face_t* getFontFace() const;
};

// ============================================================
// Appearance — drawing style info
// ============================================================
class Appearance
{
    friend class LayerExDraw;

public:
    struct DrawInfo
    {
        int type;    // 0:pen 1:brush
        void* info;  // SoftPen* or SoftBrush*
        tjs_real ox;
        tjs_real oy;
        DrawInfo() : ox(0), oy(0), type(0), info(NULL) {}
        DrawInfo(tjs_real ox, tjs_real oy, void* data, int ty) : ox(ox), oy(oy), type(ty), info(data)
        {
        }
        DrawInfo(const DrawInfo& orig)
        {
            ox = orig.ox;
            oy = orig.oy;
            type = orig.type;
            if (orig.info)
            {
                switch (type)
                {
                    case 0:
                        info = (void*)((SoftPen*)orig.info)->Clone();
                        break;
                    case 1:
                        info = (void*)((SoftBrush*)orig.info)->Clone();
                        break;
                    default:
                        info = NULL;
                        break;
                }
            }
            else
            {
                info = NULL;
            }
        }
        virtual ~DrawInfo()
        {
            if (info)
            {
                switch (type)
                {
                    case 0:
                        delete (SoftPen*)info;
                        break;
                    case 1:
                        delete (SoftBrush*)info;
                        break;
                }
            }
        }
    };
    std::vector<DrawInfo> drawInfos;

public:
    Appearance();
    virtual ~Appearance();
    Appearance* Clone() const;
    void clear();
    void addBrush(tTJSVariant colorOrBrush, tjs_real ox = 0, tjs_real oy = 0);
    void addPen(tTJSVariant colorOrBrush, tTJSVariant widthOrOption, tjs_real ox = 0,
                tjs_real oy = 0);

protected:
    bool getLineCap(tTJSVariant& in, plutovg_line_cap_t& cap, plutovg_path_t*& custom,
                    tjs_real pw);
};

// ============================================================
// Path — wraps plutovg_path_t*
// ============================================================
class Path
{
    friend class LayerExDraw;
    bool figureStarted = false;

public:
    Path();
    virtual ~Path();
    void startFigure();
    void closeFigure();
    void drawArc(tjs_real x, tjs_real y, tjs_real width, tjs_real height, tjs_real startAngle,
                 tjs_real sweepAngle);
    void drawBezier(tjs_real x1, tjs_real y1, tjs_real x2, tjs_real y2, tjs_real x3, tjs_real y3,
                    tjs_real x4, tjs_real y4);
    void drawBeziers(tTJSVariant points);
    void drawClosedCurve(tTJSVariant points);
    void drawClosedCurve2(tTJSVariant points, tjs_real tension);
    void drawCurve(tTJSVariant points);
    void drawCurve2(tTJSVariant points, tjs_real tension);
    void drawCurve3(tTJSVariant points, int offset, int numberOfSegments, tjs_real tension);
    void drawPie(tjs_real x, tjs_real y, tjs_real width, tjs_real height, tjs_real startAngle,
                 tjs_real sweepAngle);
    void drawEllipse(tjs_real x, tjs_real y, tjs_real width, tjs_real height);
    void drawLine(tjs_real x1, tjs_real y1, tjs_real x2, tjs_real y2);
    void drawLines(tTJSVariant points);
    void drawPolygon(tTJSVariant points);
    void drawRectangle(tjs_real x, tjs_real y, tjs_real width, tjs_real height);
    void drawRectangles(tTJSVariant rects);

protected:
    plutovg_path_t* path;
};

// ============================================================
// LayerExDraw — main drawing class attached to Layer
// ============================================================
class LayerExDraw : public layerExBase_GL
{
protected:
    GeometryT width, height;
    BufferT buffer;
    PitchT pitch;
    GeometryT clipLeft, clipTop, clipWidth, clipHeight;

    plutovg_surface_t* surface;
    plutovg_canvas_t* canvas;

    // Transform
    plutovg_matrix_t transform;
    plutovg_matrix_t viewTransform;
    plutovg_matrix_t calcTransform;

protected:
    SmoothingMode smoothingMode;
    TextRenderingHint textRenderingHint;

public:
    int getSmoothingMode() { return (int)smoothingMode; }
    void setSmoothingMode(int mode) { smoothingMode = (SmoothingMode)mode; }
    int getTextRenderingHint() { return (int)textRenderingHint; }
    void setTextRenderingHint(int hint) { textRenderingHint = (TextRenderingHint)hint; }

protected:
    GdipImage* metaGraphics;
    std::string cvName;
    bool updateWhenDraw;
    void updateRect(RectF& rect);

public:
    void setUpdateWhenDraw(int updateWhenDraw) { this->updateWhenDraw = updateWhenDraw != 0; }
    int getUpdateWhenDraw() { return updateWhenDraw ? 1 : 0; }

    inline operator GdipImage*() const { return new GdipImage(surface); }
    inline operator const GdipImage*() const { return new GdipImage(surface); }

    template<class T>
    struct BridgeFunctor
    {
        T* operator()(LayerExDraw* p) const { return (T*)*p; }
    };

public:
    LayerExDraw(DispatchT obj);
    ~LayerExDraw();
    virtual void reset();

protected:
    void updateViewTransform();
    void updateTransform();

public:
    void setViewTransform(const GdipMatrix* transform);
    void resetViewTransform();
    void rotateViewTransform(tjs_real angle);
    void scaleViewTransform(tjs_real sx, tjs_real sy);
    void translateViewTransform(tjs_real dx, tjs_real dy);

    void setTransform(const GdipMatrix* transform);
    void resetTransform();
    void rotateTransform(tjs_real angle);
    void scaleTransform(tjs_real sx, tjs_real sy);
    void translateTransform(tjs_real dx, tjs_real dy);

protected:
    RectF getPathExtents(const Appearance* app, const plutovg_path_t* path);
    void draw(const SoftPen* pen, const plutovg_matrix_t* matrix, const plutovg_path_t* path);
    void fill(const SoftBrush* brush, const plutovg_matrix_t* matrix, const plutovg_path_t* path);
    RectF _drawPath(const Appearance* app, const plutovg_path_t* path);
    void getGlyphOutline(const FontInfo* font, PointF& offset, plutovg_path_t* path,
                         tjs_uint glyph);
    void getTextOutline(const FontInfo* font, PointF& offset, plutovg_path_t* path, ttstr text);

public:
    void clear(tjs_uint32 argb);
    RectF drawPath(const Appearance* app, const Path* path);
    RectF drawArc(const Appearance* app, tjs_real x, tjs_real y, tjs_real width, tjs_real height,
                  tjs_real startAngle, tjs_real sweepAngle);
    RectF drawPie(const Appearance* app, tjs_real x, tjs_real y, tjs_real width, tjs_real height,
                  tjs_real startAngle, tjs_real sweepAngle);
    RectF drawBezier(const Appearance* app, tjs_real x1, tjs_real y1, tjs_real x2, tjs_real y2,
                     tjs_real x3, tjs_real y3, tjs_real x4, tjs_real y4);
    RectF drawBeziers(const Appearance* app, tTJSVariant points);
    RectF drawClosedCurve(const Appearance* app, tTJSVariant points);
    RectF drawClosedCurve2(const Appearance* app, tTJSVariant points, tjs_real tension);
    RectF drawCurve(const Appearance* app, tTJSVariant points);
    RectF drawCurve2(const Appearance* app, tTJSVariant points, tjs_real tension);
    RectF drawCurve3(const Appearance* app, tTJSVariant points, int offset, int numberOfSegments,
                     tjs_real tension);
    RectF drawEllipse(const Appearance* app, tjs_real x, tjs_real y, tjs_real width,
                      tjs_real height);
    RectF drawLine(const Appearance* app, tjs_real x1, tjs_real y1, tjs_real x2, tjs_real y2);
    RectF drawLines(const Appearance* app, tTJSVariant points);
    RectF drawPolygon(const Appearance* app, tTJSVariant points);
    RectF drawRectangle(const Appearance* app, tjs_real x, tjs_real y, tjs_real width,
                        tjs_real height);
    RectF drawRectangles(const Appearance* app, tTJSVariant rects);
    RectF drawPathString(const FontInfo* font, const Appearance* app, tjs_real x, tjs_real y,
                         const tjs_char* text);
    RectF drawPathString2(const FontInfo* font, const Appearance* app, tjs_real x, tjs_real y,
                          const tjs_char* text);
    RectF drawString(const FontInfo* font, const Appearance* app, tjs_real x, tjs_real y,
                     const tjs_char* text);
    RectF measureString(const FontInfo* font, const tjs_char* text);
    RectF measureStringInternal(const FontInfo* font, const tjs_char* text);
    RectF measureString2(const FontInfo* font, const tjs_char* text);
    RectF measureStringInternal2(const FontInfo* font, const tjs_char* text);

    RectF drawImage(tjs_real x, tjs_real y, GdipImage* src);
    RectF drawImageRect(tjs_real dleft, tjs_real dtop, GdipImage* src, tjs_real sleft,
                        tjs_real stop, tjs_real swidth, tjs_real sheight);
    RectF drawImageStretch(tjs_real dleft, tjs_real dtop, tjs_real dwidth, tjs_real dheight,
                           GdipImage* src, tjs_real sleft, tjs_real stop, tjs_real swidth,
                           tjs_real sheight);
    RectF drawImageAffine(GdipImage* src, tjs_real sleft, tjs_real stop, tjs_real swidth,
                          tjs_real sheight, bool affine, tjs_real A, tjs_real B, tjs_real C,
                          tjs_real D, tjs_real E, tjs_real F);

protected:
    void createRecord();
    void recreateRecord();
    void destroyRecord();
    bool redraw(GdipImage* image);

public:
    void setRecord(bool record);
    bool getRecord() { return metaGraphics != NULL; }
    GdipImage* getRecordImage();
    bool redrawRecord();
    bool saveRecord(const tjs_char* filename);
    bool loadRecord(const tjs_char* filename);
};

#endif
