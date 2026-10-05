#ifndef _DrawBackend_h_
#define _DrawBackend_h_

#include <plutovg.h>
#include <cmath>
#include <cstring>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// BrushType enum (needs to be defined before SoftBrush)
enum BrushType
{
    BrushTypeNone = -1,
    BrushTypeSolidColor = 0,
    BrushTypeHatchFill = 1,
    BrushTypeTextureFill = 2,
    BrushTypePathGradient = 3,
    BrushTypeLinearGradient = 4
};

// ============================================================
// SoftBrush — replaces BLBrush (supports solid/hatch/texture/gradient)
// ============================================================
struct SoftBrush
{
    BrushType type;

    // Solid color (non-premultiplied ARGB)
    tjs_uint32 solidColor;

    // Texture (hatch or image pattern)
    plutovg_surface_t* texSurface; // owned if ownsTexSurface
    bool ownsTexSurface;
    plutovg_texture_type_t texType;
    plutovg_matrix_t texMatrix;
    bool hasTexMatrix;

    // Gradient data
    struct GradientData
    {
        bool isLinear;                               // true=linear, false=radial
        float x1, y1, x2, y2;                       // linear endpoints
        float cx, cy, cr, fx, fy, fr;                // radial params
        plutovg_spread_method_t spread;
        std::vector<plutovg_gradient_stop_t> stops;
    };
    GradientData* gradient;

    SoftBrush()
      : type((BrushType)-1),
        solidColor(0),
        texSurface(nullptr),
        ownsTexSurface(false),
        texType(PLUTOVG_TEXTURE_TYPE_TILED),
        hasTexMatrix(false),
        gradient(nullptr)
    {
    }

    explicit SoftBrush(tjs_uint32 color)
      : type((BrushType)0 /*BrushTypeSolidColor*/),
        solidColor(color),
        texSurface(nullptr),
        ownsTexSurface(false),
        texType(PLUTOVG_TEXTURE_TYPE_TILED),
        hasTexMatrix(false),
        gradient(nullptr)
    {
    }

    // Takes ownership of surface
    explicit SoftBrush(plutovg_surface_t* surf)
      : type((BrushType)1 /*BrushTypeHatchFill*/),
        solidColor(0),
        texSurface(surf),
        ownsTexSurface(true),
        texType(PLUTOVG_TEXTURE_TYPE_TILED),
        hasTexMatrix(false),
        gradient(nullptr)
    {
    }

    // Texture with explicit type
    SoftBrush(plutovg_surface_t* surf, plutovg_texture_type_t tt)
      : type((BrushType)2 /*BrushTypeTextureFill*/),
        solidColor(0),
        texSurface(surf),
        ownsTexSurface(true),
        texType(tt),
        hasTexMatrix(false),
        gradient(nullptr)
    {
    }

    // Gradient brush (takes ownership of GradientData)
    explicit SoftBrush(GradientData* gd)
      : type((BrushType)(gd->isLinear ? 4 : 3) /*LinearGradient or PathGradient*/),
        solidColor(0),
        texSurface(nullptr),
        ownsTexSurface(false),
        texType(PLUTOVG_TEXTURE_TYPE_PLAIN),
        hasTexMatrix(false),
        gradient(gd)
    {
    }

    ~SoftBrush()
    {
        if (texSurface && ownsTexSurface)
            plutovg_surface_destroy(texSurface);
        delete gradient;
    }

    SoftBrush* Clone() const
    {
        SoftBrush* b = new SoftBrush();
        b->type = type;
        b->solidColor = solidColor;
        if (texSurface)
        {
            b->texSurface = plutovg_surface_reference(texSurface);
            b->ownsTexSurface = true;
        }
        b->texType = texType;
        b->texMatrix = texMatrix;
        b->hasTexMatrix = hasTexMatrix;
        if (gradient)
        {
            b->gradient = new GradientData(*gradient);
        }
        return b;
    }

    // Apply this brush as the current paint on a canvas
    void applyToCanvas(plutovg_canvas_t* canvas) const
    {
        switch ((int)type)
        {
            case 0: // BrushTypeSolidColor
            {
                plutovg_color_t c;
                plutovg_color_init_argb32(&c, solidColor);
                plutovg_canvas_set_color(canvas, &c);
                break;
            }
            case 1: // BrushTypeHatchFill
            case 2: // BrushTypeTextureFill
            {
                if (texSurface)
                {
                    const plutovg_matrix_t* mtx = hasTexMatrix ? &texMatrix : nullptr;
                    plutovg_canvas_set_texture(canvas, texSurface, texType, 1.0f, mtx);
                }
                break;
            }
            case 3: // BrushTypePathGradient (radial)
            case 4: // BrushTypeLinearGradient
            {
                if (gradient)
                {
                    const plutovg_gradient_stop_t* stops =
                        gradient->stops.empty() ? nullptr : gradient->stops.data();
                    int nstops = (int)gradient->stops.size();
                    if (gradient->isLinear)
                    {
                        plutovg_canvas_set_linear_gradient(
                            canvas, gradient->x1, gradient->y1, gradient->x2, gradient->y2,
                            gradient->spread, stops, nstops, nullptr);
                    }
                    else
                    {
                        plutovg_canvas_set_radial_gradient(
                            canvas, gradient->cx, gradient->cy, gradient->cr, gradient->fx,
                            gradient->fy, gradient->fr, gradient->spread, stops, nstops, nullptr);
                    }
                }
                break;
            }
            default:
            {
                plutovg_color_t c;
                plutovg_color_init_rgba8(&c, 0, 0, 0, 255);
                plutovg_canvas_set_color(canvas, &c);
                break;
            }
        }
    }

private:
    SoftBrush(const SoftBrush&);
    SoftBrush& operator=(const SoftBrush&);
};

// ============================================================
// SoftPen — replaces BLPen (stroke style + color/brush)
// ============================================================
struct SoftPen
{
    tjs_real strokeWidth;

    // Fill: either brush or solid color
    SoftBrush* brush; // owned, may be nullptr
    tjs_uint32 color; // used when brush==nullptr

    // Stroke options
    plutovg_line_cap_t lineCap;
    plutovg_line_join_t lineJoin;
    float miterLimit;
    std::vector<float> dashArray;
    float dashOffset;

    // Custom caps
    int startCapType; // 0=built-in, 1=custom path
    int endCapType;
    plutovg_path_t* startCapPath;
    plutovg_path_t* endCapPath;

    SoftPen(tjs_uint32 c, tjs_real w)
      : strokeWidth(w),
        brush(nullptr),
        color(c),
        lineCap(PLUTOVG_LINE_CAP_BUTT),
        lineJoin(PLUTOVG_LINE_JOIN_MITER),
        miterLimit(10.0f),
        dashOffset(0.0f),
        startCapType(0),
        endCapType(0),
        startCapPath(nullptr),
        endCapPath(nullptr)
    {
    }

    SoftPen(SoftBrush* b, tjs_real w)
      : strokeWidth(w),
        brush(b),
        color(0),
        lineCap(PLUTOVG_LINE_CAP_BUTT),
        lineJoin(PLUTOVG_LINE_JOIN_MITER),
        miterLimit(10.0f),
        dashOffset(0.0f),
        startCapType(0),
        endCapType(0),
        startCapPath(nullptr),
        endCapPath(nullptr)
    {
    }

    ~SoftPen()
    {
        delete brush;
        if (startCapPath)
            plutovg_path_destroy(startCapPath);
        if (endCapPath)
            plutovg_path_destroy(endCapPath);
    }

    SoftPen* Clone() const
    {
        SoftPen* p;
        if (brush)
            p = new SoftPen(brush->Clone(), strokeWidth);
        else
            p = new SoftPen(color, strokeWidth);
        p->lineCap = lineCap;
        p->lineJoin = lineJoin;
        p->miterLimit = miterLimit;
        p->dashArray = dashArray;
        p->dashOffset = dashOffset;
        p->startCapType = startCapType;
        p->endCapType = endCapType;
        if (startCapPath)
            p->startCapPath = plutovg_path_clone(startCapPath);
        if (endCapPath)
            p->endCapPath = plutovg_path_clone(endCapPath);
        return p;
    }

    // Apply stroke style to canvas
    void applyStrokeStyle(plutovg_canvas_t* canvas) const
    {
        plutovg_canvas_set_line_width(canvas, (float)strokeWidth);
        plutovg_canvas_set_line_cap(canvas, lineCap);
        plutovg_canvas_set_line_join(canvas, lineJoin);
        plutovg_canvas_set_miter_limit(canvas, miterLimit);
        if (!dashArray.empty())
        {
            plutovg_canvas_set_dash(canvas, dashOffset, dashArray.data(), (int)dashArray.size());
        }
    }

    // Apply fill color/paint for stroke
    void applyStrokeColor(plutovg_canvas_t* canvas) const
    {
        if (brush)
        {
            brush->applyToCanvas(canvas);
        }
        else
        {
            plutovg_color_t c;
            plutovg_color_init_argb32(&c, color);
            plutovg_canvas_set_color(canvas, &c);
        }
    }

private:
    SoftPen(const SoftPen&);
    SoftPen& operator=(const SoftPen&);
};

// ============================================================
// Helper: Elliptic arc approximation with cubic beziers
// ============================================================
static inline void pathEllipticArc(plutovg_path_t* path, float cx, float cy, float rx, float ry,
                                   float startAngle, float sweepAngle, bool addMoveTo = false)
{
    if (fabsf(sweepAngle) < 1e-6f)
        return;

    int nsegs = (int)ceilf(fabsf(sweepAngle) / (float)(M_PI / 2.0));
    if (nsegs < 1)
        nsegs = 1;
    float segAngle = sweepAngle / nsegs;

    if (addMoveTo)
    {
        float x0 = cx + rx * cosf(startAngle);
        float y0 = cy + ry * sinf(startAngle);
        plutovg_path_move_to(path, x0, y0);
    }

    float angle = startAngle;
    for (int i = 0; i < nsegs; i++)
    {
        float a1 = angle;
        float da = segAngle;
        float a2 = a1 + da;

        float halfDa = da / 4.0f;
        float kappa = 4.0f / 3.0f * tanf(halfDa);

        float cos1 = cosf(a1), sin1 = sinf(a1);
        float cos2 = cosf(a2), sin2 = sinf(a2);

        // Tangent directions
        float dx1 = -rx * sin1, dy1 = ry * cos1;
        float dx2 = -rx * sin2, dy2 = ry * cos2;

        // Control points
        float cp1x = cx + rx * cos1 + kappa * dx1;
        float cp1y = cy + ry * sin1 + kappa * dy1;
        float cp2x = cx + rx * cos2 - kappa * dx2;
        float cp2y = cy + ry * sin2 - kappa * dy2;
        float x3 = cx + rx * cos2;
        float y3 = cy + ry * sin2;

        plutovg_path_cubic_to(path, cp1x, cp1y, cp2x, cp2y, x3, y3);

        angle = a2;
    }
}

// ============================================================
// Helper: check if a plutovg_path is empty
// ============================================================
static inline bool pathIsEmpty(const plutovg_path_t* path)
{
    if (!path)
        return true;
    const plutovg_path_element_t* elements = nullptr;
    int count = plutovg_path_get_elements(path, &elements);
    return count <= 0;
}

// ============================================================
// Helper: get path bounding box
// ============================================================
static inline void pathGetBoundingBox(const plutovg_path_t* path, float* x0, float* y0, float* x1,
                                      float* y1)
{
    plutovg_rect_t extents;
    plutovg_path_extents(path, &extents, false);
    if (x0)
        *x0 = extents.x;
    if (y0)
        *y0 = extents.y;
    if (x1)
        *x1 = extents.x + extents.w;
    if (y1)
        *y1 = extents.y + extents.h;
}

#endif // _DrawBackend_h_
