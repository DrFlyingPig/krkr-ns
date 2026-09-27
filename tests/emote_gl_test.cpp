#include "EmoteGLRenderBackend.h"
#include "EmoteSWRenderBackend.h"
#include "ThreadIntf.h"
#include <SDL.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

static SDL_Renderer* primary;
SDL_Renderer* TVPGetPrimarySDLRenderer() { return primary; }
tjs_int TVPGetThreadNum() { return 1; }
void TVPBeginThreadTask(tjs_int) {}
void TVPExecThreadTask(TVP_THREAD_TASK_FUNC func, TVP_THREAD_PARAM param) { func(param); }
void TVPEndThreadTask() {}
static unsigned checks;
static void check(bool value, const char* message)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s (%s)\n", message, SDL_GetError()); std::exit(1); }
}
static const float quad[] = {-1,-1,0,0, 1,-1,1,0, 1,1,1,1, -1,1,0,1};
static const uint16_t indices[] = {0,1,2, 0,2,3};
using Pixel = std::array<uint8_t,4>;

// Nonuniform pixels and odd widths exercise transparent/opaque SIMD groups,
// mixed groups, the scalar tail, both triangle windings and stencil cutouts.
// Expected colours follow the pinned blend equations independently of the
// rasterizer's edge and UV implementation.
static void checkAlphaFastPaths(krkrsdl3::iTVPRenderBackend& backend)
{
    constexpr int w = 17, h = 9;
    const Pixel background = {37, 79, 121, 63};
    const uint8_t alphas[] = {0, 255, 1, 128, 254};
    std::vector<Pixel> source(w * h), maskPixels(w * h), seedPixels(w * h, background);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            source[y * w + x] = {uint8_t(17 + x * 11), uint8_t(31 + y * 23),
                                 uint8_t(211 - x * 7), alphas[((x / 4) + y) % 5]};
            maskPixels[y * w + x] = {0, 0, 0, uint8_t((x + y) % 4 < 2 ? 127 : 128)};
        }
    void* target = backend.CreateTarget(w, h);
    void* texture = backend.CreateTexture(w, h);
    void* mask = backend.CreateTarget(w, h);
    backend.UpdateTexture(texture, source[0].data(), w, h, w * 4);
    backend.UpdateTargetTexture(mask, maskPixels[0].data(), w, h, w * 4);
    const float color[] = {0.25f, 0.75f, 0.5f, 0.5f};
    const Pixel uniform = {63, 191, 127, 127};
    const uint16_t reverseIndices[] = {2, 1, 0, 3, 2, 0};
    for (bool clipped : {false, true})
        for (bool stencil : {false, true})
            for (bool reversed : {false, true})
                for (int mode : {0, 3, 1, 4, 21})
                    for (float opacity : {-1.0f, 0.0f, 0.5f, 1.0f, 2.0f})
                    {
                        const int left = clipped ? 3 : 0, right = clipped ? 14 : w;
                        const int top = clipped ? 1 : 0, bottom = clipped ? 8 : h;
                        const float x0 = 2.0f * left / w - 1, x1 = 2.0f * right / w - 1;
                        const float y0 = 2.0f * top / h - 1, y1 = 2.0f * bottom / h - 1;
                        const float vertices[] = {x0,y0,0,0, x1,y0,1,0,
                                                   x1,y1,1,1, x0,y1,0,1};
                        backend.UpdateTargetTexture(target, seedPixels[0].data(), w, h, w * 4);
                        backend.SetTarget(target); backend.SetMask(stencil ? mask : nullptr);
                        backend.SetBlendMode(mode, mode == 21 ? color : nullptr);
                        backend.DrawMesh(vertices, 4, reversed ? reverseIndices : indices, 6,
                                         texture, opacity);
                        int pitch = 0;
                        const uint8_t* pixels = backend.LockTarget(target, pitch);
                        check(pixels && pitch == w * 4, "odd-width alpha target dimensions");
                        for (int y = 0; y < h; ++y)
                            for (int x = 0; x < w; ++x)
                            {
                                Pixel expected = background;
                                bool exact = true;
                                if (x >= left && x < right && y >= top && y < bottom &&
                                    (!stencil || maskPixels[y * w + x][3] >= 128))
                                {
                                    const int tx = int((x - left + 0.5) / (right - left) * (w - 1) + 0.5);
                                    const int ty = int((y - top + 0.5) / (bottom - top) * (h - 1) + 0.5);
                                    const Pixel src = source[ty * w + tx];
                                    const double opa = std::max(0.0, std::min(1.0, double(opacity)));
                                    const double sa = src[3] / 255.0 * opa *
                                        (mode == 21 ? uniform[3] / 255.0 : 1.0);
                                    if (sa >= 1.0) expected = src;
                                    else if (sa != 0 || mode == 1 || mode == 4)
                                    {
                                        exact = false;
                                        for (int c = 0; c < 3; ++c)
                                        {
                                            const double out = mode == 1 || mode == 4
                                                ? src[c] * background[c] / 255.0 + background[c]
                                                : (mode == 21 ? uniform[c] : src[c]) * sa + background[c] * (1 - sa);
                                            expected[c] = uint8_t(std::min(255.0, std::round(out)));
                                        }
                                        if (mode != 1 && mode != 4)
                                            expected[3] = uint8_t(std::round(mode == 21
                                                ? sa * sa * 255 + background[3] * (1 - sa)
                                                : std::max(sa * 255, double(background[3]))));
                                    }
                                }
                                const uint8_t* actual = pixels + y * pitch + x * 4;
                                for (int c = 0; c < 4; ++c)
                                {
                                    const int error = std::abs(int(actual[c]) - expected[c]);
                                    if (error > (exact ? 0 : 1))
                                        std::fprintf(stderr, "alpha regression mode=%d opa=%g clip=%d mask=%d reverse=%d pos=%d,%d channel=%d actual=%d expected=%d\n",
                                                     mode, opacity, clipped, stencil, reversed, x, y, c,
                                                     actual[c], expected[c]);
                                    check(error <= (exact ? 0 : 1), "alpha fast path preserves pixels and blend equations");
                                }
                            }
                        backend.UnlockTarget(target);
                    }
    backend.SetMask(nullptr);
    backend.DestroyTexture(texture); backend.DestroyTarget(mask); backend.DestroyTarget(target);
}

int main(int argc, char** argv)
{
    SDL_SetMainReady();
    check(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL video init");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_Window* window = SDL_CreateWindow("KRKR port regression", 0, 0, 320, 200, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    check(window != nullptr, "GL window");
    primary = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    check(primary != nullptr, "primary SDL renderer");
    SDL_RendererInfo info;
    SDL_GetRendererInfo(primary, &info);
    std::printf("SDL renderer: %s\n", info.name);
    SDL_GLContext originalContext = SDL_GL_GetCurrentContext();
    SDL_RenderSetLogicalSize(primary, 160, 100);
    SDL_RenderSetScale(primary, 1.5f, 1.5f);
    SDL_Rect viewport = {4, 6, 112, 74}, clip = {3, 5, 62, 46};
    SDL_RenderSetViewport(primary, &viewport);
    SDL_RenderSetClipRect(primary, &clip);
    SDL_SetRenderDrawColor(primary, 31, 71, 191, 255);
    SDL_SetRenderDrawBlendMode(primary, SDL_BLENDMODE_ADD);
    auto screen = [&]() {
        SDL_RenderClear(primary);
        SDL_SetRenderDrawColor(primary, 213, 42, 19, 255);
        SDL_RenderFillRect(primary, nullptr);
        SDL_SetRenderDrawColor(primary, 31, 71, 191, 255);
        std::vector<uint8_t> pixels(320 * 200 * 4);
        check(SDL_RenderReadPixels(primary, nullptr, SDL_PIXELFORMAT_RGBA32, pixels.data(), 320 * 4) == 0, "window readback");
        return pixels;
    };
    const auto before = screen();
    {
        krkrsdl3::EmoteGLRenderBackend gpu;
        krkrsdl3::EmoteSWRenderBackend cpu;
        const bool useCpu = argc > 1 && std::strcmp(argv[1], "--cpu") == 0;
        check(useCpu || gpu.IsAvailable(), "private GL backend initializes");
        krkrsdl3::iTVPRenderBackend& gl = useCpu ? static_cast<krkrsdl3::iTVPRenderBackend&>(cpu)
                                              : static_cast<krkrsdl3::iTVPRenderBackend&>(gpu);
        check(SDL_GL_GetCurrentContext() == originalContext, "initialization restores current context");
        auto* target = gl.CreateTarget(8, 8);
        auto* texture = gl.CreateTexture(1, 1);
        auto* mask = gl.CreateTarget(8, 8);
        const Pixel source = {200, 60, 20, 128}, background = {40, 80, 120, 64};
        gl.UpdateTexture(texture, source.data(), 1, 1, 4);
        auto seed = [&]() {
            std::vector<Pixel> pixels(64, background);
            gl.UpdateTargetTexture(target, pixels[0].data(), 8, 8, 8 * 4);
            gl.SetTarget(target); gl.SetMask(nullptr);
        };
        auto read = [&]() {
            int pitch = 0;
            const uint8_t* pixels = gl.LockTarget(target, pitch);
            check(pixels && pitch == 32, "target readback dimensions");
            Pixel result;
            std::memcpy(result.data(), pixels + 3 * pitch + 4 * 4, 4);
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    check(std::memcmp(pixels + y * pitch + x * 4, result.data(), 4) == 0,
                          "uniform quad has no double-blended triangle seam");
            gl.UnlockTarget(target);
            check(SDL_GL_GetCurrentContext() == originalContext, "draw restores current context");
            return result;
        };
        // Independent analytical expectations from the pinned GL blend state:
        // 0/3: src*sa+dst*(1-sa), max(sa,da); 1/4: src*dst+dst, da.
        for (int mode : {0, 3, 1, 4, 21, 6})
        {
            seed();
            const float color[] = {0.25f, 0.75f, 0.5f, 0.5f};
            gl.SetBlendMode(mode, color);
            gl.DrawMesh(quad, 4, indices, 6, texture, 0.5f);
            const Pixel actual = read();
            Pixel expected = background;
            const double sa = 128.0 / 255 * 0.5 * (mode == 21 ? 0.5 : 1.0);
            for (int c = 0; c < 3; ++c)
            {
                double out = background[c];
                if (mode == 1 || mode == 4) out += source[c] * background[c] / 255.0;
                else if (mode != 6) out = (mode == 21 ? color[c] * 255 : source[c]) * sa + background[c] * (1 - sa);
                expected[c] = uint8_t(std::min(255.0, std::round(out)));
            }
            if (mode == 0 || mode == 3) expected[3] = uint8_t(std::round(std::max(sa * 255, double(background[3]))));
            if (mode == 21) expected[3] = uint8_t(std::round(sa * sa * 255 + background[3] * (1 - sa)));
            for (int c = 0; c < 4; ++c)
            {
                if (std::abs(int(actual[c]) - expected[c]) > 2)
                    std::fprintf(stderr, "mode=%d channel=%d actual=%d expected=%d\n", mode, c, actual[c], expected[c]);
                check(std::abs(int(actual[c]) - expected[c]) <= 2, "upstream blend equation");
            }
        }
        for (int alpha : {0, 127, 128, 255})
        {
            std::vector<Pixel> maskPixels(64, Pixel{0, 0, 0, uint8_t(alpha)});
            gl.UpdateTargetTexture(mask, maskPixels[0].data(), 8, 8, 32);
            seed(); gl.SetMask(mask); gl.SetBlendMode(0, nullptr);
            gl.DrawMesh(quad, 4, indices, 6, texture, 1);
            Pixel actual = read();
            check(alpha < 128 ? actual == background : actual[0] >= 119 && actual[0] <= 121,
                  "binary mask threshold, never opacity multiplication");
        }
        const Pixel corners[] = {{255,0,0,255}, {0,255,0,255}, {0,0,255,255}, {255,255,255,255}};
        auto* small = gl.CreateTexture(2, 2);
        gl.UpdateTexture(small, corners[2].data(), 2, 2, -8);
        int pitch = 0;
        uint8_t* stored = gl.LockTexture(small, pitch);
        check(pitch == 8 && stored[2] == 255 && stored[8] == 255, "negative source pitch");
        gl.UpdateTexture(small, corners[0].data(), 2, 2, 8);
        gl.SetTarget(target); gl.ClearTarget(true); gl.SetMask(nullptr); gl.SetBlendMode(0, nullptr);
        gl.DrawMesh(quad, 4, indices, 6, small, 1);
        uint8_t* pixels = gl.LockTarget(target, pitch);
        check(pixels[0] == 255 && pixels[2] == 0, "logical first row is red, no vertical inversion");
        check(pixels[7 * pitch + 2] == 255, "logical last row is blue");
        gl.UnlockTarget(target);
        bool rejected = false;
        try { gl.UpdateTexture(small, corners[0].data(), 3, 2, 12); }
        catch (const std::exception&) { rejected = true; }
        check(rejected, "oversized upload rejected before touching GL state");
        gl.DestroyTexture(small); gl.DestroyTexture(texture); gl.DestroyTarget(mask); gl.DestroyTarget(target);
        // CPU sampling is pinned to nearest texels; GL uses filtered sampling.
        if (useCpu) checkAlphaFastPaths(gl);
    }
    check(SDL_GL_GetCurrentContext() == originalContext, "destruction restores context");
    int lw, lh; float sx, sy; SDL_Rect vp, cr;
    SDL_RenderGetLogicalSize(primary, &lw, &lh); SDL_RenderGetScale(primary, &sx, &sy);
    SDL_RenderGetViewport(primary, &vp); SDL_RenderGetClipRect(primary, &cr);
    check(lw == 160 && lh == 100 && sx == 1.5f && sy == 1.5f, "SDL logical size and scale unchanged");
    check(std::memcmp(&vp, &viewport, sizeof(vp)) == 0 && std::memcmp(&cr, &clip, sizeof(cr)) == 0, "SDL viewport and clip unchanged");
    check(screen() == before, "primary renderer pixels identical after E-mote use");
    SDL_DestroyRenderer(primary); primary = nullptr; SDL_DestroyWindow(window); SDL_Quit();
    std::printf("PASS: %u GL port checks\n", checks);
}
