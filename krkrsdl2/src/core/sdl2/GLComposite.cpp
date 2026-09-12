/* SPDX-License-Identifier: MIT */
/* KRKR-ns Phase 3: GPU layer composite (Stage 3.1).
 *
 * The engine's whole layer tree lands on the layer manager's DrawBuffer
 * through tTVPBaseBitmap::Blt/CopyRect. When enabled (marker file
 * sdmc:/switch/krkrsdl2/gpu-composite.txt + a full-readback driver probe),
 * those final blits are drawn as textured quads into a private ES2 FBO,
 * and the present path renders that FBO texture fullscreen and swaps —
 * bypassing the surface memcpy, SDL_UpdateTexture and RenderCopy stages.
 * Everything else (methods we don't map yet, hda modes, temp bitmaps,
 * non-compose destinations) keeps the proven CPU path.
 *
 * On drivers whose full-rect ReadPixels truncate (the Nextendo emulator's
 * software GLES returns only the left quarter), the probe fails and the
 * module stays disabled — same gate as the E-mote backend. */

#include "GLCompositeBridge.h"
#include "KrkrNSLog.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_opengles2.h>

extern SDL_Renderer* TVPGetPrimarySDLRenderer();
#include <cstring>
#include <cerrno>
#include <vector>
#include <map>
#include <algorithm>

#ifdef __SWITCH__

// Engine bitmaps are B,G,R,A byte order in memory (R at bit16 of the
// 32bpp word). We upload them as GL_RGBA and fix the R/B swap in the
// shader (c.bgr) and again when copying ReadPixels output (RGBA bytes)
// into the SDL surface (BGRA bytes) — the same proven path E-mote uses.
// GL_BGRA_EXT uploads were silently rejected on this driver (probe passed
// because it only clears; textured quads then sampled empty textures).
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#define KRKRNS_GL_FORMAT GL_RGBA

/* ---- ES2 function pointer table (self-typed; SDL GL headers provide the */
/* GL types, the PFNGL typedefs are avoided on purpose) ---- */
struct GLProcs
{
    void (*ActiveTexture)(GLenum) = nullptr;
    void (*AttachShader)(GLuint, GLuint) = nullptr;
    void (*BindAttribLocation)(GLuint, GLuint, const GLchar*) = nullptr;
    void (*BindBuffer)(GLenum, GLuint) = nullptr;
    void (*BindFramebuffer)(GLenum, GLuint) = nullptr;
    void (*BindTexture)(GLenum, GLuint) = nullptr;
    void (*BlendColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void (*BlendEquationSeparate)(GLenum, GLenum) = nullptr;
    void (*BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum) = nullptr;
    void (*BlitFramebuffer)(GLint, GLint, GLint, GLint,
                            GLint, GLint, GLint, GLint,
                            GLbitfield, GLenum) = nullptr;
    void (*BufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
    void (*BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*) = nullptr;
    void (*Clear)(GLbitfield) = nullptr;
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void (*CompileShader)(GLuint) = nullptr;
    GLuint (*CreateProgram)(void) = nullptr;
    GLuint (*CreateShader)(GLenum) = nullptr;
    void (*DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void (*DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void (*DeleteProgram)(GLuint) = nullptr;
    void (*DeleteShader)(GLuint) = nullptr;
    void (*DeleteTextures)(GLsizei, const GLuint*) = nullptr;
    void (*Disable)(GLenum) = nullptr;
    void (*DrawArrays)(GLenum, GLint, GLsizei) = nullptr;
    void (*Enable)(GLenum) = nullptr;
    void (*EnableVertexAttribArray)(GLuint) = nullptr;
    void (*Flush)(void) = nullptr;
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    void (*GenBuffers)(GLsizei, GLuint*) = nullptr;
    void (*GenFramebuffers)(GLsizei, GLuint*) = nullptr;
    void (*GenTextures)(GLsizei, GLuint*) = nullptr;
    void (*GetIntegerv)(GLenum, GLint*) = nullptr;
    void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void (*GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void (*GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    const GLubyte* (*GetString)(GLenum) = nullptr;
    GLenum (*GetError)(void) = nullptr;
    GLint (*GetUniformLocation)(GLuint, const GLchar*) = nullptr;
    GLboolean (*IsEnabled)(GLenum) = nullptr;
    void (*LineWidth)(GLfloat) = nullptr;
    void (*LinkProgram)(GLuint) = nullptr;
    void (*PixelStorei)(GLenum, GLint) = nullptr;
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
    void (*Scissor)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
    void (*TexParameteri)(GLenum, GLenum, GLint) = nullptr;
    void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*) = nullptr;
    void (*Uniform1f)(GLint, GLfloat) = nullptr;
    void (*Uniform1i)(GLint, GLint) = nullptr;
    void (*Uniform2f)(GLint, GLfloat, GLfloat) = nullptr;
    void (*UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;
    void (*UseProgram)(GLuint) = nullptr;
    void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
};

static GLProcs gl;

static bool LoadGL()
{
#define KRKRNS_LOAD(n) gl.n = reinterpret_cast<decltype(gl.n)>(SDL_GL_GetProcAddress("gl" #n)); if (!gl.n) return false;
    KRKRNS_LOAD(ActiveTexture)
    KRKRNS_LOAD(AttachShader)
    KRKRNS_LOAD(BindAttribLocation)
    KRKRNS_LOAD(BindBuffer)
    KRKRNS_LOAD(BindFramebuffer)
    KRKRNS_LOAD(BindTexture)
    KRKRNS_LOAD(BlendColor)
    KRKRNS_LOAD(BlendEquationSeparate)
    KRKRNS_LOAD(BlendFuncSeparate)
    KRKRNS_LOAD(BlitFramebuffer)
    KRKRNS_LOAD(BufferData)
    KRKRNS_LOAD(BufferSubData)
    KRKRNS_LOAD(Clear)
    KRKRNS_LOAD(ClearColor)
    KRKRNS_LOAD(CompileShader)
    KRKRNS_LOAD(CreateProgram)
    KRKRNS_LOAD(CreateShader)
    KRKRNS_LOAD(DeleteBuffers)
    KRKRNS_LOAD(DeleteFramebuffers)
    KRKRNS_LOAD(DeleteProgram)
    KRKRNS_LOAD(DeleteShader)
    KRKRNS_LOAD(DeleteTextures)
    KRKRNS_LOAD(Disable)
    KRKRNS_LOAD(DrawArrays)
    KRKRNS_LOAD(Enable)
    KRKRNS_LOAD(EnableVertexAttribArray)
    KRKRNS_LOAD(Flush)
    KRKRNS_LOAD(FramebufferTexture2D)
    KRKRNS_LOAD(GenBuffers)
    KRKRNS_LOAD(GenFramebuffers)
    KRKRNS_LOAD(GenTextures)
    KRKRNS_LOAD(GetError)
    KRKRNS_LOAD(GetIntegerv)
    KRKRNS_LOAD(GetProgramInfoLog)
    KRKRNS_LOAD(GetProgramiv)
    KRKRNS_LOAD(GetShaderInfoLog)
    KRKRNS_LOAD(GetShaderiv)
    KRKRNS_LOAD(GetString)
    KRKRNS_LOAD(GetUniformLocation)
    KRKRNS_LOAD(IsEnabled)
    KRKRNS_LOAD(LineWidth)
    KRKRNS_LOAD(LinkProgram)
    KRKRNS_LOAD(PixelStorei)
    KRKRNS_LOAD(ReadPixels)
    KRKRNS_LOAD(Scissor)
    KRKRNS_LOAD(ShaderSource)
    KRKRNS_LOAD(TexImage2D)
    KRKRNS_LOAD(TexParameteri)
    KRKRNS_LOAD(TexSubImage2D)
    KRKRNS_LOAD(Uniform1f)
    KRKRNS_LOAD(Uniform1i)
    KRKRNS_LOAD(Uniform2f)
    KRKRNS_LOAD(UniformMatrix4fv)
    KRKRNS_LOAD(UseProgram)
    KRKRNS_LOAD(VertexAttribPointer)
    KRKRNS_LOAD(Viewport)
#undef KRKRNS_LOAD
    return true;
}
/* ---- state ---- */
namespace
{
struct TextureEntry
{
    GLuint tex = 0;
    int w = 0, h = 0, pitch = 0;
    unsigned long long version = 0;
};
}

static SDL_GLContext gContext = nullptr;
static SDL_GLContext gSavedContext = nullptr;
static SDL_Window* gWindow = nullptr;
static bool gProbeFailed = false;
static bool gProbeDone = false;
static bool gMarkerFound = false;
static bool gMarkerChecked = false;
static int gMode = 0;        // 0 off, 1 full, 2 observe, 3 log-only

static void* gComposeBitmap = nullptr;   // LayerManager::DrawBuffer
static GLuint gComposeTex = 0, gComposeFbo = 0;
static int gComposeW = 1280, gComposeH = 720;

static GLuint gProgram = 0;
static GLint gSizeLoc = -1, gOpaLoc = -1, gImageLoc = -1, gSwapLoc = -1;
static GLuint gVbo = 0;
static std::map<const void*, TextureEntry> gTextures;
static std::map<const void*, unsigned long long> gBitmapVersions;
static unsigned long long gVersionCounter = 1;

struct Twin
{
    GLuint tex = 0, fbo = 0;
    int w = 0, h = 0;
};

static unsigned gHandledBlts = 0, gUploadedTex = 0, gTotalBlts = 0;
static bool gPureFrame = true;                 // (legacy, unused now)
static bool gFrameDrew = false;                 // any handled blt this frame
static bool gFrameActive = false;               // begin_frame completed for this frame
static unsigned gReadbackCount = 0;
static unsigned gLayerCount = 0;   // layers recorded this frame
static Uint32 gLayerMs = 0;        // glc_layer upload+quad time this frame
static unsigned gBlitChecks = 0;   // window-present probes this run (<=3)
static bool gBlitBroken = false;   // probe found the FBO empty -> SDL chain
static unsigned gFrameHandled = 0;              // handled blts in the current frame
static tjs_int gFallbackRects[2048][4];        // fallback compose writes (x,y,w,h)
static unsigned gFallbackCount = 0;
static bool gM1 = false, gM2 = false, gM3 = false, gM4 = false, gM5 = false;
static void Milestone(const char* tag)
{
    if (gM1 && gM2 && gM3 && gM4 && gM5) return;
    KRKRNS_LOG("[glc] mstone %s", tag);
}
static std::map<const void*, Twin> gTwins;     // per-frame render twins (temps)
static std::vector<const void*> gFrameTwinKeys; // created this frame
static const size_t kMaxTwins = 16;
static unsigned long long gUploadBytes = 0;
static Uint32 gStatsFrames = 0;

/* report the first GL errors per session (helps catch driver rejections
 * like the BGRA_EXT uploads that made every textured quad sample empty) */
static void GLErr(const char* where)
{
    if (!gl.GetError) return;
    static int logged = 0;
    if (logged >= 6) return;
    int n = 0;
    GLenum e;
    while ((e = gl.GetError()) != GL_NO_ERROR && n < 4)
    {
        KRKRNS_LOG("[glc] GLerr %s: 0x%x", where, (unsigned)e);
        n++;
        logged++;
    }
}

/* upload a bitmap's pixels to its cached texture (full image, v1) */
static bool UploadBitmap(const void* bmp, int w, int h, int pitch,
                         const unsigned char* pixels)
{
    TextureEntry& e = gTextures[bmp];
    if (e.tex == 0)
    {
        gl.GenTextures(1, &e.tex);
        gl.BindTexture(GL_TEXTURE_2D, e.tex);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    else
    {
        gl.BindTexture(GL_TEXTURE_2D, e.tex);
    }
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // Engine bitmaps are bottom-up: the raster pointer (GetScanLine(0))
    // points at the LOGICAL TOP row, which sits at the END of memory.
    // Walking forward (+y) reads past the buffer; rows must be fetched
    // toward lower addresses. With pitch==w*4 the whole block is already
    // top-down from this pointer, so it can be uploaded in one call.
    if (e.w != w || e.h != h)
    {
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        e.w = w; e.h = h;
    }
    if (pitch == w * 4)
    {
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, KRKRNS_GL_FORMAT, GL_UNSIGNED_BYTE, pixels);
    }
    else
    {
        // padded rows: repack into a tight buffer (row 0 = logical top)
        std::vector<unsigned char> tight((size_t)w * h * 4);
        for (int y = 0; y < h; ++y)
            std::memcpy(tight.data() + (size_t)y * w * 4,
                        pixels - (size_t)y * pitch, (size_t)w * 4);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, KRKRNS_GL_FORMAT, GL_UNSIGNED_BYTE, tight.data());
    }
    e.pitch = pitch;
    auto it = gBitmapVersions.find(bmp);
    e.version = it != gBitmapVersions.end() ? it->second : 0;
    gUploadedTex++;
    gUploadBytes += (unsigned long long)w * h * 4;
    GLErr("UploadBitmap");
    return true;
}

/* quad draw shared by copy/alpha/add; swapRGB=1 keeps the layer-upload
 * R/B exchange (engine bitmaps are BGRA in memory, uploaded as GL_RGBA),
 * the window-present quad passes 0 because the compose FBO is already
 * in display order. */
static void DrawQuad(int dstX, int dstY, int dstW, int dstH,
                     int srcX, int srcY, int srcW, int srcH,
                     int texW, int texH, float opa, bool premultiply,
                     int tw = 0, int th = 0, bool swapRGB = true)
{
    if (tw <= 0 || th <= 0) { tw = gComposeW; th = gComposeH; }
    if (tw <= 0 || th <= 0) return;
    gl.UseProgram(gProgram);
    // clip-space quad
    const float x0 = (float)dstX / tw * 2.0f - 1.0f;
    const float y0 = 1.0f - (float)dstY / th * 2.0f;
    const float x1 = (float)(dstX + dstW) / tw * 2.0f - 1.0f;
    const float y1 = 1.0f - (float)(dstY + dstH) / th * 2.0f;
    const float u0 = (float)srcX / texW;
    const float v0 = (float)srcY / texH;
    const float u1 = (float)(srcX + srcW) / texW;
    const float v1 = (float)(srcY + srcH) / texH;
    const float verts[24] = {
        x0, y0, u0, v0,
        x1, y0, u1, v0,
        x1, y1, u1, v1,
        x0, y0, u0, v0,
        x1, y1, u1, v1,
        x0, y1, u0, v1,
    };
    gl.BindBuffer(GL_ARRAY_BUFFER, gVbo);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                           reinterpret_cast<void*>(2 * sizeof(float)));
    gl.Uniform2f(gSizeLoc, (float)tw, (float)th);
    gl.Uniform1f(gOpaLoc, opa / 255.0f);
    gl.Uniform1i(gImageLoc, 0);
    gl.Uniform1i(gSwapLoc, swapRGB ? 1 : 0);
    gl.DrawArrays(GL_TRIANGLES, 0, 6);
    GLErr("DrawQuad");
}

static GLuint CompileShader(GLenum type, const char* src)
{
    GLuint sh = gl.CreateShader(type);
    gl.ShaderSource(sh, 1, &src, nullptr);
    gl.CompileShader(sh);
    GLint ok = 0;
    gl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        gl.GetShaderInfoLog(sh, sizeof(log), nullptr, log);
        KRKRNS_LOG("[glc] shader compile failed: %s", log);
        gl.DeleteShader(sh);
        return 0;
    }
    return sh;
}

static const char* gVert =
    "attribute vec2 aPos;"
    "attribute vec2 aUv;"
    "varying vec2 vUv;"
    "uniform vec2 viewportSize;"
    "void main() { vUv = aUv; gl_Position = vec4(aPos, 0.0, 1.0); }";

static const char* gFrag =
    "precision mediump float;"
    "varying vec2 vUv;"
    "uniform sampler2D image;"
    "uniform float opa;"
    "uniform int swapRGB;"
    "void main() {"
    "  vec4 c = texture2D(image, vUv);"
    "  if (swapRGB == 1) c.rgb = c.bgr;"
    "  gl_FragColor = vec4(c.rgb, c.a * opa);"
    "}";

static bool EnsureProgram()
{
    if (gProgram) return true;
    GLuint vs = CompileShader(GL_VERTEX_SHADER, gVert);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, gFrag);
    if (!vs || !fs) return false;
    gProgram = gl.CreateProgram();
    gl.AttachShader(gProgram, vs);
    gl.AttachShader(gProgram, fs);
    gl.BindAttribLocation(gProgram, 0, "aPos");
    gl.BindAttribLocation(gProgram, 1, "aUv");
    gl.LinkProgram(gProgram);
    GLint linked = 0;
    gl.GetProgramiv(gProgram, GL_LINK_STATUS, &linked);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    if (!linked)
    {
        KRKRNS_LOG("[glc] program link failed");
        return false;
    }
    gSizeLoc = gl.GetUniformLocation(gProgram, "viewportSize");
    gOpaLoc = gl.GetUniformLocation(gProgram, "opa");
    gImageLoc = gl.GetUniformLocation(gProgram, "image");
    gSwapLoc = gl.GetUniformLocation(gProgram, "swapRGB");
    gl.GenBuffers(1, &gVbo);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_CULL_FACE);
    return true;
}

static void ResetGLState()
{
    if (gContext)
    {
        SDL_GL_DeleteContext(gContext);
        gContext = nullptr;
    }
    gComposeTex = 0;
    gComposeFbo = 0;
    gProgram = 0;
    gVbo = 0;
    gBlitChecks = 0; // re-verify the window after a context rebuild
    gTextures.clear();
    gTwins.clear();
    gFrameTwinKeys.clear();
    gBitmapVersions.clear();
    gProbeDone = false;
    gProbeFailed = false;
    gM1 = gM2 = gM3 = gM4 = gM5 = false; // let milestones re-fire after rebuild
    gReadbackCount = 0;
    gSavedContext = nullptr;
    gFrameActive = false;
}

static bool BeginContext()
{
    SDL_Renderer* renderer = TVPGetPrimarySDLRenderer();
    if (!renderer) return false;
    SDL_Window* win = SDL_RenderGetWindow(renderer);
    if (!win) return false;
    if (gContext && win != gWindow)
    {
        // The window changed under us (launcher window -> game window, the
        // launcher window is destroyed when the game starts). Everything GL
        // is owned by the old context and dies with it, so rebuild all state
        // on the new window. Without this, every SDL_GL_MakeCurrent fails
        // and the GPU composite silently no-ops (m5 never fires).
        KRKRNS_LOG("[glc] window switched %p -> %p, rebuilding GL state", gWindow, win);
        ResetGLState();
        gWindow = win;
    }
    else if (gContext)
    {
        // Same window as before.  After an in-process engine restart this is the
        // suspicious case: if SDL handed the new engine a recycled SDL_Window
        // pointer, the context we keep is the one built against the window that
        // was already destroyed, and compositing silently produces nothing --
        // UI that still draws while the background stays black.
        static void *lastLogged = nullptr;
        if (gWindow != lastLogged)
        {
            lastLogged = gWindow;
            KRKRNS_LOG("[glc] reusing GL context for window %p (no switch detected)", gWindow);
        }
    }
    if (gContext)
    {
        if (SDL_GL_MakeCurrent(gWindow, gContext) != 0) return false;
        return true;
    }
    gWindow = win;
    int profile = 0, major = 0, minor = 0;
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minor);
#ifdef __SWITCH__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
    gContext = SDL_GL_CreateContext(win);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    if (!gContext) return false;
    if (SDL_GL_MakeCurrent(win, gContext) != 0) return false;
    if (!LoadGL())
    {
        KRKRNS_LOG("[glc] GLES2 proc load failed");
        return false;
    }
    const char* ver = (const char*)gl.GetString(GL_VERSION);
    KRKRNS_LOG("[glc] context ready: %s", ver ? ver : "?");
    if (!EnsureProgram()) return false;
    return true;
}

static void EndContext()
{
    if (gSavedContext)
        SDL_GL_MakeCurrent(gWindow, gSavedContext);
}

/* full-readback probe: a driver whose whole-rect ReadPixels truncates
 * (the emulator's software GLES returns only 25%) gets disabled here. */
static bool RunProbe()
{
    if (gProbeDone) return !gProbeFailed;
    gProbeDone = true;
    if (!gComposeTex) return false;
    gl.BindFramebuffer(GL_FRAMEBUFFER, gComposeFbo);
    gl.Viewport(0, 0, gComposeW, gComposeH);
    gl.Disable(GL_BLEND);
    gl.ClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    std::vector<unsigned char> full((size_t)gComposeW * gComposeH * 4);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.ReadPixels(0, 0, gComposeW, gComposeH, KRKRNS_GL_FORMAT, GL_UNSIGNED_BYTE, full.data());
    unsigned red = 0;
    for (size_t i = 0; i < full.size(); i += 4)
        if (full[i] > 200 && full[i + 3] > 200) red++; // RGBA: red in byte 0
    const double frac = (double)red / ((double)gComposeW * gComposeH);
    gProbeFailed = frac < 0.90;
    const char* rend = (const char*)gl.GetString(GL_RENDERER);
    KRKRNS_LOG("[glc] probe %s: fullRed=%.1f%% renderer=%s",
               gProbeFailed ? "FAIL (disabled)" : "PASS", frac * 100.0,
               rend ? rend : "?");
    if (gProbeFailed)
    {
        gl.DeleteFramebuffers(1, &gComposeFbo);
        gl.DeleteTextures(1, &gComposeTex);
        gComposeFbo = 0; gComposeTex = 0;
    }
    return !gProbeFailed;
}

/* ---- public interface (C++ linkage, see GLCompositeBridge.h) ---- */

bool krkrsdl2_glc_enabled()
{
    // FBO creation happens lazily inside begin_frame on first use.
    if (!gMarkerChecked)
    {
        gMarkerChecked = true;
        FILE* m = fopen("sdmc:/switch/krkrsdl2/gpu-composite.txt", "rb");
        if (m) { fclose(m); gMarkerFound = true; gMode = 1; }
        FILE* o = fopen("sdmc:/switch/krkrsdl2/gpu-composite-obs.txt", "rb");
        if (o) { fclose(o); gMarkerFound = true; gMode = 2; }
        FILE* l = fopen("sdmc:/switch/krkrsdl2/gpu-composite-log.txt", "rb");
        if (l) { fclose(l); gMarkerFound = true; gMode = 3; }
        FILE* c = fopen("sdmc:/switch/krkrsdl2/gpu-composite-cpuonly.txt", "rb");
        if (c) { fclose(c); gMarkerFound = true; gMode = 4; }
        FILE* g = fopen("sdmc:/switch/krkrsdl2/gpu-composite-gpuonly.txt", "rb");
        if (g) { fclose(g); gMarkerFound = true; gMode = 5; }
        KRKRNS_LOG("[glc] marker %s mode=%d", gMarkerFound ? "found" : "absent (CPU composite)", gMode);
    }
    // Mode 4 (cpuonly): try_blt always declines, so every compose write
    // runs the CPU path and readback's fold uploads the finished bitmap.
    // It is the A/B probe for "is the quad/texture path or the fold/readback
    // path broken?" — a correct full image here proves fold+readback, and
    // a black/missing image here blames them.
    if (gMode == 4) return true;
    return gMarkerFound && !gProbeFailed && gMode != 3;
}

void krkrsdl2_glc_set_compose_target(void* bitmap)
{
    gComposeBitmap = bitmap;
}

static Twin* FindOrCreateTwin(const void* bmp, int w, int h)
{
    auto it = gTwins.find(bmp);
    if (it != gTwins.end())
    {
        Twin& t = it->second;
        if (t.w == w && t.h == h) return &t;
        gl.DeleteTextures(1, &t.tex);
        gl.DeleteFramebuffers(1, &t.fbo);
        gTwins.erase(it);
    }
    if (gTwins.size() >= kMaxTwins || w <= 0 || h <= 0) return nullptr;
    Twin& t = gTwins[bmp];
    gl.GenTextures(1, &t.tex);
    gl.BindTexture(GL_TEXTURE_2D, t.tex);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.GenFramebuffers(1, &t.fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    t.w = w; t.h = h;
    gFrameTwinKeys.push_back(bmp);
    return &t;
}

void krkrsdl2_glc_begin_frame()
{
    gFrameActive = false; // this frame starts on the CPU path until proven
    if (!krkrsdl2_glc_enabled()) return;
    if (gProbeDone && gProbeFailed) return;
    if (!BeginContext()) return;
    gSavedContext = SDL_GL_GetCurrentContext();
    if (!gM1) { gM1 = true; Milestone("m1 context+program ready"); }
    if (!gComposeTex)
    {
        gl.GenTextures(1, &gComposeTex);
        gl.BindTexture(GL_TEXTURE_2D, gComposeTex);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gComposeW, gComposeH, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl.GenFramebuffers(1, &gComposeFbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER, gComposeFbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, gComposeTex, 0);
        KRKRNS_LOG("[glc] compose FBO %dx%d created", gComposeW, gComposeH);
    }
    if (!RunProbe()) return;
    if (!gM2) { gM2 = true; Milestone("m2 probe passed"); }
    gPureFrame = true;
    gFrameDrew = false;
    gFrameHandled = 0;
    gFallbackCount = 0;
    gLayerCount = 0;
    gLayerMs = 0;
    gl.BindFramebuffer(GL_FRAMEBUFFER, gComposeFbo);
    gl.Viewport(0, 0, gComposeW, gComposeH);
    gl.Disable(GL_BLEND);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    if (!gM3) { gM3 = true; Milestone("m3 compose clear ok"); }
    gFrameActive = true; // context current, FBO bound: GPU may claim blits now
}

void krkrsdl2_glc_end_frame()
{
    if (!gMarkerFound || gProbeFailed) return;
    // tear down this frame's temporary twins (the compose target persists)
    for (const void* key : gFrameTwinKeys)
    {
        auto it = gTwins.find(key);
        if (it == gTwins.end()) continue;
        if (it->second.tex == gComposeTex) continue;
        gl.DeleteTextures(1, &it->second.tex);
        gl.DeleteFramebuffers(1, &it->second.fbo);
        gTwins.erase(it);
    }
    gFrameTwinKeys.clear();
    {
        static unsigned frames = 0;
        if (++frames % 30 == 0)
            KRKRNS_LOG("[glc] compose-stats: frames=%u handled=%u/%u pure=%d drew=%d readback=%u",
                       frames, gHandledBlts, gTotalBlts, gPureFrame ? 1 : 0,
                       gFrameDrew ? 1 : 0, gReadbackCount);
    }
    EndContext();
}

/* a compose-target write the GPU did NOT claim: the engine will perform
 * the CPU blit on the compose bitmap, and readback must fold that region
 * into the GL texture on top of any GPU content. Called on every CPU
 * fallback path; never on the GPU-claimed return-true path (the engine
 * skips the CPU blit there, so folding would overwrite fresh GPU pixels
 * with stale ones). */
static void NoteComposeFallback(tjs_int x, tjs_int y, tjs_int w, tjs_int h)
{
    if (w <= 0 || h <= 0) return;
    if (gFallbackCount < 2048)
    {
        tjs_int* r = gFallbackRects[gFallbackCount++];
        r[0] = x; r[1] = y; r[2] = w; r[3] = h;
    }
}

bool krkrsdl2_glc_try_blt(void* dest, tjs_int x, tjs_int y,
                          const void* src, const tTVPRect* srcRect,
                          tjs_int method, tjs_int opa, bool hda)
{
    gTotalBlts++;
    if (!krkrsdl2_glc_enabled()) return false;
    const bool destIsCompose = (dest == gComposeBitmap);
    {
        static unsigned diagCalls = 0;
        if (diagCalls < 6)
        {
            KRKRNS_LOG("[glc] blt#%u destIsCompose=%d hda=%d met=%d xy=%d,%d sz=%dx%d src=%p dest=%p",
                       ++diagCalls, dest == gComposeBitmap ? 1 : 0,
                       hda ? 1 : 0, method, x, y,
                       srcRect->right - srcRect->left,
                       srcRect->bottom - srcRect->top, src, dest);
        }
    }
    if (gMode == 1 || gMode == 5)
    {
        // v2.6 layer-composite mode: per-layer quads are drawn by
        // krkrsdl2_glc_layer from NotifyBitmapCompleted on the full-screen
        // FBO. The old Blt/DrawBuffer interception (8-row strip target) is
        // retired; the engine's CPU compose stays authoritative for texture
        // caches and the readback publishes the GPU frame.
        // Mode 5 (gpuonly) additionally skips the CPU-side compose writes
        // (see BasicDrawDevice::NotifyBitmapCompleted) so the GPU quads are
        // the only composition — the performance target for Phase 3.
        return false;
    }
    if (gMode == 4)
    {
        // cpuonly A/B probe: decline everything so the engine's CPU blit
        // owns the compose bitmap, and readback folds the finished frame.
        // (begin_frame still runs so gComposeFbo exists for the fold.)
        if (destIsCompose)
            NoteComposeFallback(x, y,
                srcRect->right - srcRect->left, srcRect->bottom - srcRect->top);
        return false;
    }
    if (!gFrameActive)
    {
        // begin_frame did not complete (window switch pending rebuild,
        // probe failed, ...): claiming the blit here would make the engine
        // skip the CPU draw while nothing reached the GPU -> black frame.
        // Let the CPU path own the frame instead.
        return false;
    }
    {
        static unsigned diagCalls = 0;
        if (diagCalls < 5)
        {
            KRKRNS_LOG("[glc] full-blt#%u destIsCompose=%d hda=%d met=%d xy=%d,%d sz=%dx%d src=%p dest=%p",
                       ++diagCalls, dest == gComposeBitmap ? 1 : 0,
                       hda ? 1 : 0, method, x, y,
                       srcRect->right - srcRect->left,
                       srcRect->bottom - srcRect->top, src, dest);
        }
    }
    if (hda)
    {
        if (destIsCompose) NoteComposeFallback(x, y,
            srcRect->right - srcRect->left, srcRect->bottom - srcRect->top);
        return false;
    }
    if (method != 0 && method != 1 && method != 6) // copy/alpha/add
    {
        if (destIsCompose) NoteComposeFallback(x, y,
            srcRect->right - srcRect->left, srcRect->bottom - srcRect->top);
        return false;
    }
    tjs_int sw = srcRect->right - srcRect->left;
    tjs_int sh = srcRect->bottom - srcRect->top;
    if (sw <= 0 || sh <= 0) return false;

    Twin* dtwin = nullptr;
    if (destIsCompose)
    {
        if (gComposeFbo)
        {
            Twin& ct = gTwins[gComposeBitmap];
            ct.tex = gComposeTex; ct.fbo = gComposeFbo;
            ct.w = gComposeW; ct.h = gComposeH;
            dtwin = &ct;
        }
    }
    else
    {
        auto it = gTwins.find(dest);
        if (it != gTwins.end() && it->second.tex != 0)
        {
            dtwin = &it->second;
        }
        else
        {
            // first GPU write to this (temp/region) bitmap this frame:
            // create its render twin; later writes and the final blit to
            // the compose target will use it as the source texture.
            int dw = 0, dh = 0, dpitch = 0, dbpp = 0;
            if (!krkrsdl2_glc_get_bitmap_raster(dest, &dw, &dh, &dpitch, &dbpp))
            {
                if (destIsCompose) NoteComposeFallback(x, y, sw, sh);
                return false;
            }
            if (dbpp != 32)
            {
                if (destIsCompose) NoteComposeFallback(x, y, sw, sh);
                return false;
            }
            dtwin = FindOrCreateTwin(dest, dw, dh);
        }
    }
    if (!dtwin)
    {
        if (destIsCompose)
        {
            NoteComposeFallback(x, y, sw, sh);
            gPureFrame = false;
        }
        return false;
    }

    // resolve the source: prefer an existing twin, else upload CPU raster
    GLuint srcTex = 0;
    int sw_tex = 0, sh_tex = 0;
    auto sit = gTwins.find(src);
    if (sit != gTwins.end() && sit->second.tex != 0)
    {
        srcTex = sit->second.tex;
        sw_tex = sit->second.w;
        sh_tex = sit->second.h;
    }
    else
    {
        int w = 0, h = 0, pitch = 0, bpp = 0;
        const unsigned char* pixels = static_cast<const unsigned char*>(
            krkrsdl2_glc_get_bitmap_raster(src, &w, &h, &pitch, &bpp));
        if (!pixels || bpp != 32)
        {
            if (destIsCompose) NoteComposeFallback(x, y, sw, sh);
            return false;
        }
        if (srcRect->left < 0 || srcRect->top < 0 ||
            srcRect->right > w || srcRect->bottom > h)
        {
            if (destIsCompose) NoteComposeFallback(x, y, sw, sh);
            return false;
        }
        auto vit = gBitmapVersions.find(src);
        const unsigned long long ver = vit != gBitmapVersions.end() ? vit->second : 0;
        TextureEntry& e = gTextures[src];
        // Strip-like sources (the draw-pool row bands, <=16 rows) are
        // re-filled with new pixels on almost every Blt while the engine
        // never bumps their version on the GPU-claimed path (the hook's
        // bump_version call sits after try_blt returns false). Caching them
        // freezes the first band on screen. Upload them every time instead.
        const bool stripLike = (sh <= 16);
        if (stripLike || e.tex == 0 || e.w != w || e.h != h || e.version != ver)
        {
            if (!UploadBitmap(src, w, h, pitch, pixels))
            {
                if (destIsCompose) NoteComposeFallback(x, y, sw, sh);
                return false;
            }
        }
        srcTex = e.tex;
        sw_tex = w;
        sh_tex = h;
    }

    gHandledBlts++;
    gFrameHandled++;
    gFrameDrew = true;
    if (!gM4)
    {
        gM4 = true;
        KRKRNS_LOG("[glc] mstone m4 first blt destIsCompose=%d method=%d src=%dx%d",
                   destIsCompose ? 1 : 0, method, sw, sh);
    }
    gl.BindFramebuffer(GL_FRAMEBUFFER, dtwin->fbo);
    gl.Viewport(0, 0, dtwin->w, dtwin->h);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, srcTex);
    if (method == 0)
    {
        gl.Disable(GL_BLEND);
        DrawQuad(x, y, sw, sh, srcRect->left, srcRect->top, sw, sh,
                 sw_tex, sh_tex, 255.0f, false, dtwin->w, dtwin->h);
    }
    else
    {
        gl.Enable(GL_BLEND);
        gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        if (method == 1)
        {
            // engine TVPAlphaBlend: dst = src.rgb*src.a + dst.rgb*(1-src.a)
            // shader outputs non-premultiplied colors, so use SRC_ALPHA
            gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                 GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        else
        {
            // engine TVPAddBlend: dst += src (fully additive, no alpha)
            gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
        }
        DrawQuad(x, y, sw, sh, srcRect->left, srcRect->top, sw, sh,
                 sw_tex, sh_tex, (float)opa, true, dtwin->w, dtwin->h);
    }
    return true;
}

bool krkrsdl2_glc_try_copy(void* dest, tjs_int x, tjs_int y,
                           const void* src, const tTVPRect* srcRect)
{
    return krkrsdl2_glc_try_blt(dest, x, y, src, srcRect, 0, 255, false);
}

bool krkrsdl2_glc_pure_frame()
{
    return krkrsdl2_glc_enabled() && gPureFrame;
}

bool krkrsdl2_glc_gpuonly()
{
    return gMode == 5;
}

/* Per-layer composite (v2.6). Called from BasicDrawDevice::
 * NotifyBitmapCompleted — the engine presents each finished layer through
 * this path, so the *full-screen* compose FBO is the correct target (old
 * gComposeBitmap was the 8-row DrawBuffer, the source of every "one band"
 * artifact). The layer's cliprect is the source region, (x,y) the target,
 * type/opacity the blend. This mirrors the CPU path's semantics exactly. */
void krkrsdl2_glc_layer(tjs_int x, tjs_int y,
                        const void* bits, tjs_int w, tjs_int h,
                        tjs_int pitch,
                        const tTVPRect& cliprect,
                        tjs_int type, tjs_int opacity,
                        bool bottomup)
{
    const Uint32 l_t0 = SDL_GetTicks();
    if (!krkrsdl2_glc_enabled() || !gFrameActive) return;
    if (gComposeFbo == 0 || !bits || w <= 0 || h <= 0 || pitch <= 0) return;
    if (opacity <= 0) return;
    const tjs_int sw = cliprect.right - cliprect.left;
    const tjs_int sh = cliprect.bottom - cliprect.top;
    if (sw <= 0 || sh <= 0) return;
    if (cliprect.left < 0 || cliprect.top < 0 ||
        cliprect.right > w || cliprect.bottom > h) return;
    gLayerCount++;

    // first sessions: layer stream shape (pos/size/type) — the gapless
    // seq lets us see whether the engine hands us whole layers or 8-row
    // strips (the snapshot showed an 8-row repeated pattern => strips)
    {
        static unsigned layDiag = 0;
        if (layDiag < 6 && gReadbackCount <= 2)
        {
            layDiag++;
            KRKRNS_LOG("[glc] layer#%u xy=%d,%d clip=%d,%d %dx%d tex=%dx%d type=%d opa=%d",
                       layDiag, x, y, cliprect.left, cliprect.top, sw, sh,
                       w, h, (int)type, (int)opacity);
        }
    }

    // Layer rasters are usually fresh pointers every frame; cap the texture
    // cache so a long session doesn't accumulate unbounded textures.
    if (gTextures.size() > 256)
    {
        for (auto& kv : gTextures)
            if (kv.second.tex) gl.DeleteTextures(1, &kv.second.tex);
        gTextures.clear();
    }
    // Upload the layer pixels into a transient texture (key = bits+size;
    // layers are usually unique per frame, so keep it cheap and correct).
    TextureEntry& e = gTextures[bits];
    const bool needSize = (e.tex == 0 || e.w != w || e.h != h);
    if (needSize)
    {
        if (e.tex == 0)
        {
            gl.GenTextures(1, &e.tex);
            gl.BindTexture(GL_TEXTURE_2D, e.tex);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        else
        {
            gl.BindTexture(GL_TEXTURE_2D, e.tex);
        }
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                      GL_UNSIGNED_BYTE, nullptr);
        e.w = w; e.h = h;
    }
    else
    {
        gl.BindTexture(GL_TEXTURE_2D, e.tex);
    }
    // bottom-up layer memory: logical row 0 sits at the END of the buffer.
    // Upload top-down (texture row 0 = logical top) so v=0 is the top.
    // Row-by-row repack is mandatory for bottomup buffers: a whole-buffer
    // upload from the logical top reads past the bitmap (each 8-row strip
    // then sampled the SAME neighbouring memory -> the 8-row moire).
    {
        std::vector<unsigned char> tight((size_t)w * h * 4);
        for (int r = 0; r < h; ++r)
        {
            const unsigned char* srcrow = static_cast<const unsigned char*>(bits) +
                (size_t)(bottomup ? (h - 1 - r) : r) * pitch;
            std::memcpy(tight.data() + (size_t)r * w * 4, srcrow, (size_t)w * 4);
        }
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA,
                         GL_UNSIGNED_BYTE, tight.data());
    }

    // Blit onto the compose FBO with the layer's blend type.
    const bool alpha = (type == 2 || type == 12);      // ltAlpha / ltAddAlpha
    const bool additive = (type == 3 || type == 12);   // ltAdditive
    gl.BindFramebuffer(GL_FRAMEBUFFER, gComposeFbo);
    gl.Viewport(0, 0, gComposeW, gComposeH);
    gl.Disable(GL_BLEND);
    if (alpha || additive)
    {
        gl.Enable(GL_BLEND);
        gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        if (additive)
            gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
        else
            gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                 GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    DrawQuad(x, y, sw, sh,
             cliprect.left, cliprect.top, sw, sh,
             w, h, (float)opacity, alpha || additive, gComposeW, gComposeH);
    GLErr("glc_layer");
    gLayerMs += SDL_GetTicks() - l_t0;
}

bool krkrsdl2_glc_readback(void* surface, int w, int h, int pitch)
{
    (void)w; (void)h;
    if (!krkrsdl2_glc_enabled()) return false;
    // Mode 5 (gpuonly): every readback call MUST present — when the engine
    // skipped composition entirely (no layer notifications) the FBO still
    // holds the previous frame, so re-presenting it shows the unchanged
    // picture instead of a blank SDL-screen frame (the black flash). The
    // counted-blts gate below applies to the other modes only.
    if (gMode != 5 && gFrameHandled == 0 && gFallbackCount == 0 && gLayerCount == 0)
        return false;
    if (gComposeFbo == 0 || !surface) return false;
    gReadbackCount++;
    if (gReadbackCount <= 3)
        KRKRNS_LOG("[glc] readback #%u for frame %u handled=%u fallback=%u layers=%u",
                   gReadbackCount, gTotalBlts, gFrameHandled, gFallbackCount, gLayerCount);
    if (gMode == 1 || gMode == 5)
    {
        // v2.6 layer-composite mode: the full-screen FBO holds the per-layer
        // quads. Read it back; publish it to the surface ONLY when the GPU
        // clearly composed a real frame (several layers + substantial
        // non-black content; mode 5 needs just one layer since the CPU side
        // is skipped). Otherwise keep the CPU-composed surface —
        // overwriting a correct CPU frame with a nearly-empty GPU one is the
        // classic black-screen trap (launcher = 1 layer must stay CPU).
        const Uint32 rb_t0 = SDL_GetTicks();
        if (!BeginContext())
        {
            static bool bcFailLogged = false;
            if (!bcFailLogged)
            {
                bcFailLogged = true;
                const char* err = SDL_GetError();
                KRKRNS_LOG("[glc] readback BeginContext FAILED: %s", err ? err : "?");
            }
            return false;
        }
        gSavedContext = SDL_GL_GetCurrentContext();
        const bool gpuOnly = (gMode == 5);
        if (gpuOnly)
        {
            // v2.8b: publish every frame the engine notifies — the FBO holds
            // exactly what the engine drew (or the retained previous frame
            // when nothing changed; no blank-SDL-frame flashes anymore).
            int dw = 0, dh = 0;
            // v2.8g: prefer the renderer's real output size — the window
            // drawable can report the screen resolution while the EGL
            // surface of a game-sized window is actually smaller, which made
            // the full-size quad clip to 2/3 of the screen.
            if (SDL_GetRendererOutputSize(TVPGetPrimarySDLRenderer(), &dw, &dh) != 0 ||
                dw <= 0 || dh <= 0)
                SDL_GL_GetDrawableSize(gWindow, &dw, &dh);
            if (dw > 0 && dh > 0)
            {
                // v2.8h: set the window viewport before the present quad —
                // the stale compose viewport (1280x720) rasterized the
                // full-size quad into the bottom-left corner (probe: center
                // + bottom-left 100%, all other corners 0%).
                gl.Viewport(0, 0, dw, dh);
                // v2.8e: present the compose texture as a full-window quad.
                // glBlitFramebuffer scaling is broken on this software driver
                // (1.5x blit -> black; 1:1 blit -> top-left 2/3 only), while
                // texture-sampled scaling is the proven path (E-mote GL).
                // The FBO texture rows are bottom-up (row 0 = engine bottom),
                // so the source height is negated to flip v.
                gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
                gl.ActiveTexture(GL_TEXTURE0);
                gl.BindTexture(GL_TEXTURE_2D, gComposeTex);
                DrawQuad(0, 0, dw, dh,
                         0, gComposeH, gComposeW, -gComposeH,
                         gComposeW, gComposeH, 255, false, dw, dh, false);
                gl.Flush();
                // First frames after (re)start or a window switch: probe the real
                // display buffer (READ=0) at center + 4 corners so a wrong
                // present size reads out as content only in part of the
                // window (the "picture is 2/3 of the screen" complaint).
                if (gBlitChecks < 3)
                {
                    gBlitChecks++;
                    unsigned char probe[4 * 8 * 8];
                    // Compose FBO content first (distinguishes "black
                    // screen because composition is empty" from "present
                    // dropped the frame").
                    unsigned fbb = 0;
                    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, gComposeFbo);
                    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
                    gl.ReadPixels(gComposeW / 2, gComposeH / 2, 8, 8,
                                  GL_RGBA, GL_UNSIGNED_BYTE, probe);
                    for (int i = 0; i < 8 * 8; ++i)
                        if (probe[i * 4] || probe[i * 4 + 1] || probe[i * 4 + 2]) fbb++;
                    // Window backbuffer corners, GL coords (origin bottom-left).
                    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                    const int pts[4][2] = {
                        {8, 8}, {dw - 8, 8}, {8, dh - 8}, {dw - 8, dh - 8}};
                    unsigned pnb[4] = {0, 0, 0, 0};
                    for (int p = 0; p < 4; ++p)
                    {
                        gl.ReadPixels(pts[p][0], pts[p][1], 8, 8,
                                      GL_RGBA, GL_UNSIGNED_BYTE, probe);
                        for (int i = 0; i < 8 * 8; ++i)
                            if (probe[i * 4] || probe[i * 4 + 1] || probe[i * 4 + 2]) pnb[p]++;
                    }
                    KRKRNS_LOG("[glc] present probe#%u win=%dx%d fbo=%.0f%% LL=%.0f%% RL=%.0f%% LT=%.0f%% RT=%.0f%%",
                               gBlitChecks, dw, dh, fbb * 100.0 / 64.0,
                               pnb[0] * 100.0 / 64.0, pnb[1] * 100.0 / 64.0,
                               pnb[2] * 100.0 / 64.0, pnb[3] * 100.0 / 64.0);
                    if (fbb < 5.0) gBlitBroken = true;
                }
                // v2.8f: swap through the GL API directly (unconditional
                // eglSwapBuffers) instead of relying on SDL_RenderPresent
                // with an empty command queue — some software-driver builds
                // skip or defer the swap, which left the verified backbuffer
                // frame unseen on the console (black launcher).
                gl.Flush();
                SDL_GL_SwapWindow(gWindow);
                EndContext();
                return true; // GPU-presented frame (swap already done)
            }
            EndContext();
            return false; // fall back to the SDL chain (stale surface)
        }
        // mode1: stats readback + CPU surface copy (the validated
        // upload/present chain follows in the caller).
        const size_t bytes = (size_t)gComposeW * 4;
        gl.BindFramebuffer(GL_FRAMEBUFFER, gComposeFbo);
        gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
        std::vector<unsigned char> rows((size_t)gComposeW * gComposeH * 4);
        gl.ReadPixels(0, 0, gComposeW, gComposeH, KRKRNS_GL_FORMAT, GL_UNSIGNED_BYTE, rows.data());
        GLErr("layer.ReadPixels");
        unsigned nonblack = 0;
        for (int y = 0; y < gComposeH; y += 3)
        {
            const unsigned char* src = rows.data() + (size_t)(gComposeH - 1 - y) * bytes;
            for (int x = 0; x < gComposeW; x += 3)
            {
                if (src[x*4+0] || src[x*4+1] || src[x*4+2]) nonblack++;
            }
        }
        const double frac = nonblack * 100.0 /
            (double)((gComposeW + 2) / 3 * (gComposeH + 2) / 3);
        if (gReadbackCount == 1 || gReadbackCount % 15 == 0)
            KRKRNS_LOG("[glc] layer-composite: readback #%u nonblack=%.1f%% layers=%u rb=%.1fms%s",
                       gReadbackCount, frac, gLayerCount,
                       (double)(SDL_GetTicks() - rb_t0),
                       (gLayerCount >= 3 && frac > 15.0) ? " -> PUBLISH" : " -> keep CPU");
        if (!(gLayerCount >= 3 && frac > 15.0))
        {
            EndContext();
            return false; // keep the CPU-composed surface
        }
        unsigned char* dst = static_cast<unsigned char*>(surface);
        for (int y = 0; y < gComposeH; ++y)
        {
            // The present surface is BGRA-in-memory (Rmask 0x00ff0000);
            // rotate the GL RGBA readback bytes in registers instead of a
            // per-byte copy (bytes are 4-aligned: rows + pitch are %4==0).
            const uint32_t* s = reinterpret_cast<const uint32_t*>(
                rows.data() + (size_t)(gComposeH - 1 - y) * bytes);
            uint32_t* d = reinterpret_cast<uint32_t*>(dst + (size_t)y * pitch);
            for (int x = 0; x < gComposeW; ++x)
            {
                const uint32_t v = s[x];
                d[x] = ((v & 0x00ff0000u) >> 16) | (v & 0x0000ff00u) |
                       ((v & 0x000000ffu) << 16) | (v & 0xff000000u);
            }
        }
        // snapshot published frames so the "garbled but visible" output can
        // be pixel-analyzed (color order / row flip / blend issues)
        if (gReadbackCount == 1 || gReadbackCount % 15 == 0)
        {
            const int w = gComposeW, h = gComposeH;
            const int row = w * 3;
            const int pad = (4 - (row % 4)) % 4;
            const int dataSize = (row + pad) * h;
            unsigned char hdr[54] = {0};
            hdr[0] = 'B'; hdr[1] = 'M';
            unsigned int fsz = 54 + dataSize;
            for (int i = 0; i < 4; ++i) hdr[2+i] = (unsigned char)(fsz >> (8*i));
            hdr[10] = 54;
            hdr[14] = 40;
            unsigned int ww = w, hh = h;
            for (int i = 0; i < 4; ++i) { hdr[18+i] = (unsigned char)(ww >> (8*i)); hdr[22+i] = (unsigned char)(hh >> (8*i)); }
            hdr[26] = 1; hdr[28] = 24;
            FILE* bmp = fopen("sdmc:/switch/krkrsdl2/glc-layer.bmp", "wb");
            if (bmp)
            {
                fwrite(hdr, 1, 54, bmp);
                for (int y = h - 1; y >= 0; --y)
                {
                    const unsigned char* srow = dst + (size_t)y * pitch;
                    for (int x = 0; x < w; ++x)
                    {
                        fputc(srow[x*4+0], bmp); // B
                        fputc(srow[x*4+1], bmp); // G
                        fputc(srow[x*4+2], bmp); // R
                    }
                    for (int i = 0; i < pad; ++i) fputc(0, bmp);
                }
                fclose(bmp);
                KRKRNS_LOG("[glc] layer snapshot saved (readback #%u)", gReadbackCount);
            }
        }
        EndContext();
        return true;
    }
    if (gMode == 4)
    {
        // cpuonly A/B probe, phase 2: bypass GL entirely — copy the compose
        // bitmap raster straight into the surface (bottom-up memory, so
        // logical row L lives at cpix - L*pitch; surface row L gets it).
        // If this shows the full image, the CPU compose + raster accessors
        // are proven good and the GL fold/readback path is what is broken.
        int cw = 0, ch = 0, cpitch = 0, cbpp = 0;
        static bool rasterDiag = false;
        if (!rasterDiag && gReadbackCount <= 2)
        {
            rasterDiag = true;
            KRKRNS_LOG("[glc] mode4: gComposeBitmap=%p", gComposeBitmap);
        }
        const unsigned char* cpix = static_cast<const unsigned char*>(
            krkrsdl2_glc_get_bitmap_raster(gComposeBitmap, &cw, &ch, &cpitch, &cbpp));
        if (cpix && cpitch > 0)
        {
            unsigned char* dst = static_cast<unsigned char*>(surface);
            for (int y = 0; y < ch && y < (int)pitch / 4; ++y)
            {
                const unsigned char* src = cpix - (size_t)(ch - 1 - y) * cpitch;
                std::memcpy(dst + (size_t)y * pitch, src, (size_t)cw * 4);
            }
            if (gReadbackCount == 1 || gReadbackCount % 15 == 0)
            {
                unsigned nonblack = 0;
                unsigned char* dst = static_cast<unsigned char*>(surface);
                for (int y = 0; y < ch; y += 3)
                    for (int x = 0; x < cw; x += 3)
                        if (dst[y*pitch + x*4 + 0] || dst[y*pitch + x*4 + 1] ||
                            dst[y*pitch + x*4 + 2]) nonblack++;
                KRKRNS_LOG("[glc] mode4-direct copy: readback #%u nonblack=%.1f%%",
                           gReadbackCount,
                           nonblack * 100.0 / (double)((cw + 2) / 3 * (ch + 2) / 3));
            }
        }
        else
        {
            KRKRNS_LOG("[glc] mode4-direct: raster unavailable");
        }
        return true;
    }
    if (!BeginContext())
    {
        // Once per session, say why. The classic cause is the launcher
        // window being destroyed when the game window opens, which left
        // SDL_GL_MakeCurrent failing on a stale gWindow.
        static bool bcFailLogged = false;
        if (!bcFailLogged)
        {
            bcFailLogged = true;
            const char* err = SDL_GetError();
            KRKRNS_LOG("[glc] readback BeginContext FAILED: %s", err ? err : "?");
        }
        return false;
    }
    gSavedContext = SDL_GL_GetCurrentContext();

    // fold fallback compose writes: upload their (correct, CPU-written)
    // pixels into the GL texture before the readback
    if (gFallbackCount > 0)
    {
        int cw = 0, ch = 0, cpitch = 0, cbpp = 0;
        const unsigned char* cpix = static_cast<const unsigned char*>(
            krkrsdl2_glc_get_bitmap_raster(gComposeBitmap, &cw, &ch, &cpitch, &cbpp));
        if (cpix)
        {
            gl.BindTexture(GL_TEXTURE_2D, gComposeTex);
            gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
            // Process one full frame of fallback fold.
        // IMPORTANT row-order note: the FBO texture is sampled with row 0 at
        // the TOP during DrawQuad (v0 = srcY/texH), and ReadPixels returns
        // bottom-up rows that the readback loop mirrors back into the SDL
        // surface. A raw TexSubImage2D(y=L) therefore lands at GL row L,
        // which after ReadPixels+mirror shows up at surface row H-1-L — i.e.
        // upside down. The GL texture's physical row 0 is the quad's BOTTOM
        // (render-to-texture inverts). To place rect row L at surface row L
        // we must write GL row (ch-1-L). Same for the whole-image fold.
        if (gFallbackCount > 128)
        {
            // many scattered fallback rects: upload the whole compose
            // bitmap once, mirrored (GL row t <- logical row ch-1-t)
            std::vector<unsigned char> tight((size_t)cw * ch * 4);
            for (int t = 0; t < ch; ++t)
                std::memcpy(tight.data() + (size_t)t * cw * 4,
                            cpix - (size_t)(ch - 1 - t) * cpitch, (size_t)cw * 4);
            gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cw, ch,
                             KRKRNS_GL_FORMAT, GL_UNSIGNED_BYTE, tight.data());
        }
        else
        {
            unsigned skipped = 0;
            for (unsigned i = 0; i < gFallbackCount; ++i)
            {
                const tjs_int* r = gFallbackRects[i];
                // safety: no OOB reads (both axes — x was missing and can
                // walk past the raster on side-scrolled/regioned blits)
                if (r[1] < 0 || r[1] + r[3] > ch) { skipped++; continue; }
                if (r[0] < 0 || r[0] + r[2] > cw) { skipped++; continue; }
                static unsigned fbLog = 0;
                if (fbLog < 8)
                {
                    KRKRNS_LOG("[glc] fold rect#%u xy=%d,%d sz=%dx%d (cw=%d ch=%d)",
                               ++fbLog, r[0], r[1], r[2], r[3], cw, ch);
                }
                for (int row = 0; row < r[3]; ++row)
                {
                    const unsigned char* srcrow = cpix -
                        (size_t)(r[1] + row) * cpitch + (size_t)r[0] * 4;
                    gl.TexSubImage2D(GL_TEXTURE_2D, 0, r[0], ch - 1 - (r[1] + row),
                                     r[2], 1, KRKRNS_GL_FORMAT, GL_UNSIGNED_BYTE, srcrow);
                }
            }
            static bool skipLog = false;
            if (!skipLog)
            {
                skipLog = true;
                KRKRNS_LOG("[glc] fold: %u rects, %u skipped by bounds", gFallbackCount, skipped);
            }
        }
        }
        else
        {
            static bool cpLog = false;
            if (!cpLog)
            {
                cpLog = true;
                KRKRNS_LOG("[glc] fold: compose raster unavailable (fallback=%u)",
                           gFallbackCount);
            }
        }
    }

    const size_t bytes = (size_t)gComposeW * 4;
    gl.BindFramebuffer(GL_FRAMEBUFFER, gComposeFbo);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    // ReadPixels returns bottom-up rows; the compose surface is top-down.
    std::vector<unsigned char> rows((size_t)gComposeW * gComposeH * 4);
    gl.ReadPixels(0, 0, gComposeW, gComposeH, KRKRNS_GL_FORMAT, GL_UNSIGNED_BYTE, rows.data());
    GLErr("readback.ReadPixels");
    unsigned char* dst = static_cast<unsigned char*>(surface);
    for (int y = 0; y < gComposeH; ++y)
    {
        const unsigned char* src = rows.data() + (size_t)(gComposeH - 1 - y) * bytes;
        unsigned char* drow = dst + (size_t)y * pitch;
        // GL_RGBA bytes (R,G,B,A) -> SDL surface is BGRA bytes: swap R/B
        for (int x = 0; x < gComposeW; ++x)
        {
            drow[x*4+0] = src[x*4+2];
            drow[x*4+1] = src[x*4+1];
            drow[x*4+2] = src[x*4+0];
            drow[x*4+3] = src[x*4+3];
        }
    }
    // diagnostic: snapshot composed frames as 24bpp BMPs (readback #1, then
    // every 15th), plus a log line quantifying how much of the frame is
    // non-black — visual completeness without pulling the BMP.
    if (gReadbackCount == 1 || gReadbackCount % 15 == 0)
    {
        unsigned nonblack = 0;
        for (int y = 0; y < gComposeH; y += 3)
        {
            const unsigned char* srow = dst + (size_t)y * pitch;
            for (int x = 0; x < gComposeW; x += 3)
            {
                if (srow[x*4+0] || srow[x*4+1] || srow[x*4+2]) nonblack++;
            }
        }
        KRKRNS_LOG("[glc] frame content: readback #%u nonblack=%.1f%%",
                   gReadbackCount,
                   nonblack * 100.0 / (double)((gComposeW + 2) / 3 * (gComposeH + 2) / 3));
        static bool snapFailLogged = false;
        FILE* bmp = fopen("sdmc:/switch/krkrsdl2/glc-frame.bmp", "wb");
        if (bmp)
        {
            const int w = gComposeW, h = gComposeH;
            const int row = w * 3;
            const int pad = (4 - (row % 4)) % 4;
            const int dataSize = (row + pad) * h;
            unsigned char hdr[54] = {0};
            hdr[0] = 'B'; hdr[1] = 'M';
            unsigned int fsz = 54 + dataSize;
            for (int i = 0; i < 4; ++i) hdr[2+i] = (unsigned char)(fsz >> (8*i));
            hdr[10] = 54;
            hdr[14] = 40;
            unsigned int ww = w, hh = h;
            for (int i = 0; i < 4; ++i) { hdr[18+i] = (unsigned char)(ww >> (8*i)); hdr[22+i] = (unsigned char)(hh >> (8*i)); }
            hdr[26] = 1; hdr[28] = 24;
            fwrite(hdr, 1, 54, bmp);
            // BMP stores rows bottom-up: write surface row h-1 first so the
            // file's y axis matches the screen's (bottom = file start).
            for (int y = h - 1; y >= 0; --y)
            {
                const unsigned char* srow = dst + (size_t)y * pitch;
                for (int x = 0; x < w; ++x)
                {
                    // surface is B,G,R,A byte order; BMP wants B,G,R
                    fputc(srow[x*4+0], bmp); // B
                    fputc(srow[x*4+1], bmp); // G
                    fputc(srow[x*4+2], bmp); // R
                }
                for (int i = 0; i < pad; ++i) fputc(0, bmp);
            }
            fclose(bmp);
            KRKRNS_LOG("[glc] frame snapshot saved (readback #%u)", gReadbackCount);
        }
        else if (!snapFailLogged)
        {
            snapFailLogged = true;
            KRKRNS_LOG("[glc] frame snapshot fopen FAILED: %s", strerror(errno));
        }
    }
    EndContext();
    if (!gM5)
    {
        gM5 = true;
        // m5 is the last milestone; Milestone()'s "all five done" guard
        // would swallow it, so print directly.
        KRKRNS_LOG("[glc] mstone m5 readback ok");
    }

    if (++gStatsFrames >= 240)
    {
        gStatsFrames = 0;
        KRKRNS_LOG("[glc] stats: handled=%u/%u blts tex=%u upMB=%.1f",
                   gHandledBlts, gTotalBlts, gUploadedTex,
                   (double)gUploadBytes / (1024.0 * 1024.0));
        gHandledBlts = gTotalBlts = gUploadedTex = 0;
        gUploadBytes = 0;
    }
    return true;
}

void krkrsdl2_glc_bump_version(const void* bitmap)
{
    gBitmapVersions[bitmap] = gVersionCounter++;
}


#else // !__SWITCH__
void krkrsdl2_glc_bump_version(const void*) {}
#endif // __SWITCH__
