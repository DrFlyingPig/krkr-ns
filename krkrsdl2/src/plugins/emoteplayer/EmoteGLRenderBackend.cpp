#include "KrkrNSPaths.h"
#include "EmoteGLRenderBackend.h"
#include "KrkrNSLog.h"
#include "KrkrNSProf.h"
#include "KrkrNSPerf.h"
#include "KrkrNSResourcePolicy.h"
#if defined(KRKRNS_EMOTE_RESIDENT_READBACK)
#include "EmoteReadbackPlan.h"
#endif
#include <SDL.h>
#include <SDL_opengles2.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

extern SDL_Renderer* TVPGetPrimarySDLRenderer();

namespace krkrsdl3
{
namespace
{
// Shader body and blend equations: krkrsdl3/krkrsdl3
// 5a8bd422f82d3758045f403520a64b772a59f40c, GLRenderBackend.cpp.
// E-mote is an extension, not a replacement for the KiriKiri core renderer.
const char* vertexShader = R"(
attribute vec2 aPos;
attribute vec2 aTexCoord;
varying vec2 texCoord;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); texCoord = aTexCoord; }
)";
const char* fragmentShader = R"(
#ifdef GL_ES
precision mediump float;
#endif
varying vec2 texCoord;
uniform sampler2D texture1;
uniform sampler2D maskTexture;
uniform bool enableMask;
uniform vec2 viewportSize;
uniform float opa;
uniform bool enableColor;
uniform bool multiplyColor;
uniform vec4 uniformColor;
void main() {
    if (enableMask && texture2D(maskTexture, gl_FragCoord.xy / viewportSize).a < 0.5)
        discard;
    vec4 color = texture2D(texture1, texCoord);
    if (enableColor) color = vec4(uniformColor.rgb, uniformColor.a * color.a);
    color.a *= multiplyColor ? clamp(opa, 0.0, 1.0) : opa;
    // Mode 19 (PS multiply): RGB must include the effective source alpha
    // before DST_COLOR / ONE_MINUS_SRC_ALPHA blending.
    if (multiplyColor) {
        color.rgb *= color.a;
    }
    gl_FragColor = color;
}
)";

#if defined(KRKRNS_EMOTE_RESIDENT_READBACK)
// Compare the final RGBA8 images, including transparent RGB and pixels erased
// by ClearTarget. A command/mesh estimate cannot determine unchanged pixels.
// Both samplers use nearest filtering and the same texel-center coordinates.
const char* damageFragmentShader = R"(
#ifdef GL_ES
precision highp float;
#define EMOTE_SAMPLE_PRECISION highp
#else
#define EMOTE_SAMPLE_PRECISION
#endif
uniform EMOTE_SAMPLE_PRECISION sampler2D currentImage;
uniform EMOTE_SAMPLE_PRECISION sampler2D previousImage;
uniform vec2 imageSize;
void main() {
    vec2 origin = floor(gl_FragCoord.xy) * 16.0;
    bool changed = false;
    for (int row = 0; row < 16; ++row) {
        for (int col = 0; col < 16; ++col) {
            vec2 pixel = origin + vec2(float(col), float(row));
            if (pixel.x < imageSize.x && pixel.y < imageSize.y) {
                vec2 uv = (pixel + vec2(0.5)) / imageSize;
                if (any(notEqual(texture2D(currentImage, uv),
                                 texture2D(previousImage, uv))))
                    changed = true;
            }
        }
    }
    gl_FragColor = changed ? vec4(1.0, 0.0, 0.0, 1.0)
                           : vec4(0.0, 0.0, 0.0, 1.0);
}
)";
#endif

// Resolve through SDL so the same source exercises GLES2 on Switch and GL on
// Windows. No platform-specific GL loader or SDL renderer internals are used.
#define EMOTE_GL_FUNCTIONS(X) \
    X(PFNGLGETSTRINGPROC, GetString) X(PFNGLGETERRORPROC, GetError) \
    X(PFNGLCREATESHADERPROC, CreateShader) X(PFNGLSHADERSOURCEPROC, ShaderSource) \
    X(PFNGLCOMPILESHADERPROC, CompileShader) X(PFNGLGETSHADERIVPROC, GetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog) X(PFNGLDELETESHADERPROC, DeleteShader) \
    X(PFNGLCREATEPROGRAMPROC, CreateProgram) X(PFNGLATTACHSHADERPROC, AttachShader) \
    X(PFNGLBINDATTRIBLOCATIONPROC, BindAttribLocation) X(PFNGLLINKPROGRAMPROC, LinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, GetProgramiv) X(PFNGLDELETEPROGRAMPROC, DeleteProgram) \
    X(PFNGLUSEPROGRAMPROC, UseProgram) X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, Uniform1i) X(PFNGLUNIFORM1FPROC, Uniform1f) \
    X(PFNGLUNIFORM2FPROC, Uniform2f) X(PFNGLUNIFORM4FVPROC, Uniform4fv) \
    X(PFNGLGENTEXTURESPROC, GenTextures) X(PFNGLBINDTEXTUREPROC, BindTexture) \
    X(PFNGLDELETETEXTURESPROC, DeleteTextures) X(PFNGLTEXIMAGE2DPROC, TexImage2D) \
    X(PFNGLTEXSUBIMAGE2DPROC, TexSubImage2D) X(PFNGLTEXPARAMETERIPROC, TexParameteri) \
    X(PFNGLGENERATEMIPMAPPROC, GenerateMipmap) X(PFNGLACTIVETEXTUREPROC, ActiveTexture) \
    X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers) \
    X(PFNGLGENBUFFERSPROC, GenBuffers) X(PFNGLBINDBUFFERPROC, BindBuffer) \
    X(PFNGLBUFFERDATAPROC, BufferData) X(PFNGLDELETEBUFFERSPROC, DeleteBuffers) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, VertexAttribPointer) X(PFNGLDRAWELEMENTSPROC, DrawElements) \
    X(PFNGLENABLEPROC, Enable) X(PFNGLDISABLEPROC, Disable) X(PFNGLVIEWPORTPROC, Viewport) \
    X(PFNGLBLENDFUNCSEPARATEPROC, BlendFuncSeparate) \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, BlendEquationSeparate) \
    X(PFNGLCLEARCOLORPROC, ClearColor) X(PFNGLCLEARPROC, Clear) \
    X(PFNGLPIXELSTOREIPROC, PixelStorei) X(PFNGLREADPIXELSPROC, ReadPixels) X(PFNGLFLUSHPROC, Flush) \
    X(PFNGLGETINTEGERVPROC, GetIntegerv) X(PFNGLISENABLEDPROC, IsEnabled)
}

struct EmoteGLRenderBackend::Impl
{
#define DECLARE_GL(type, name) type name = nullptr;
    EMOTE_GL_FUNCTIONS(DECLARE_GL)
#undef DECLARE_GL
    struct Image
    {
        int width, height;
        bool target;
        GLuint texture = 0, fbo = 0;
        std::vector<uint8_t> pixels;
        // Region of this render target touched since the last LockTarget.
        // Readback only needs these pixels: the CPU mirror keeps the rest from
        // the previous frame, and the mesh vertices are the only writers.
        // A clear wipes the target, so the mirror is zeroed on the next
        // readback instead of being re-read from the GPU.
        int dirtyX0 = 0, dirtyY0 = 0, dirtyX1 = 0, dirtyY1 = 0;
        bool dirtyValid = false;
        bool cleared = false;
        EmoteLayerDamage layerDamage;
#if defined(KRKRNS_EMOTE_RESIDENT_READBACK)
        GLuint previousTexture = 0, damageTexture = 0, damageFbo = 0;
        std::size_t residentBytes = 0;
        int damageWidth = 0, damageHeight = 0;
        bool previousValid = false, residentUnavailable = false, residentValidated = false;
        std::vector<uint8_t> damagePixels;
#endif
        void markDirty(int x0, int y0, int x1, int y1)
        {
            if (x0 >= x1 || y0 >= y1) return;
            if (!dirtyValid) { dirtyX0 = x0; dirtyY0 = y0; dirtyX1 = x1; dirtyY1 = y1; dirtyValid = true; return; }
            if (x0 < dirtyX0) dirtyX0 = x0;
            if (y0 < dirtyY0) dirtyY0 = y0;
            if (x1 > dirtyX1) dirtyX1 = x1;
            if (y1 > dirtyY1) dirtyY1 = y1;
        }
        void clearDirty()
        {
            dirtyValid = false;
            cleared = false;
            dirtyX0 = dirtyY0 = dirtyX1 = dirtyY1 = 0;
        }
    };
    std::vector<std::unique_ptr<Image>> images;
    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr, savedContext = nullptr;
    KrkrNSGpuResourceLease resourcePolicy;
    SDL_Window* savedWindow = nullptr;
    bool active = false, failed = false;
    GLuint program = 0, vbo = 0, ibo = 0;
    GLint textureLoc, maskLoc, enableMaskLoc, viewportLoc, opacityLoc, colorEnabledLoc, multiplyColorLoc, colorLoc;
    Image* target = nullptr;
    Image* mask = nullptr;
    int blend = 0;
    float color[4] = {};
    bool residentRequested = true;
    EmoteResidentReadbackStats residentStats;
#if defined(KRKRNS_EMOTE_RESIDENT_READBACK)
    PFNGLCOPYTEXSUBIMAGE2DPROC CopyTexSubImage2D = nullptr;
    PFNGLCOLORMASKPROC ColorMask = nullptr;
    GLuint damageProgram = 0, damageVbo = 0, damageIbo = 0;
    GLint damageCurrentLoc = -1, damagePreviousLoc = -1, damageSizeLoc = -1;
    bool residentProgramFailed = false;
    std::uint64_t residentLocks = 0;
    static constexpr std::uint64_t ResidentBudget = 64u * 1024u * 1024u;
#endif
    // 0 means one normal full-rectangle glReadPixels call.  Some Switch
    // compatibility renderers correctly rasterize the whole FBO but cap a
    // single readback row, so IsAvailable() can select a verified tile width.
    int readTileWidth = 0;
    std::vector<uint8_t> readScratch;

    void end()
    {
        if (!active) return;
        if (Flush) Flush();
        const int result = SDL_GL_MakeCurrent(savedWindow ? savedWindow : window, savedContext);
        active = false;
        if (result != 0)
            KRKRNS_LOG("[emote] failed to restore SDL GL context: %s", SDL_GetError());
    }
    /* The SDL window was destroyed (game session ended, launcher rebuilt its
     * own window).  Everything here is bound to the old window's GL context,
     * which is already invalid, so drop it WITHOUT issuing GL calls; the next
     * begin() recreates context/program against the new window. */
    void resetForNewWindow()
    {
        KRKRNS_LOG("[emote] GL backend reset for a new SDL window");
        program = 0; vbo = 0; ibo = 0;
        images.clear();
        target = nullptr; mask = nullptr;
        window = nullptr; context = nullptr;
        resourcePolicy.Release();
        savedWindow = nullptr; savedContext = nullptr;
        // readTileWidth is a property of the RENDERER (the self-test-measured
        // largest full-row readback the driver performs honestly), not of the
        // window -- keep it, or a truncating renderer (the emulator) would
        // silently fall back to corrupting full-width reads after an engine
        // restart.  readScratch is resized on demand.
        readScratch.clear();
        active = false; failed = false; blend = 0;
#if defined(KRKRNS_EMOTE_RESIDENT_READBACK)
        // The old context owns these objects. As with the mesh resources,
        // resetting against an already retired SDL window issues no GL calls.
        damageProgram = damageVbo = damageIbo = 0;
        damageCurrentLoc = damagePreviousLoc = damageSizeLoc = -1;
        residentProgramFailed = false;
        residentStats.liveGPUBytes = 0;
        CopyTexSubImage2D = nullptr;
        ColorMask = nullptr;
#endif
    }
    struct Scope
    {
        Impl& self;
        bool owner;
        explicit Scope(Impl& self) : self(self), owner(!self.active) { self.require(); }
        ~Scope() { if (owner) self.end(); }
    };
    GLuint compile(GLenum type, const char* source)
    {
        GLuint shader = CreateShader(type);
        ShaderSource(shader, 1, &source, nullptr);
        CompileShader(shader);
        GLint good = 0;
        GetShaderiv(shader, GL_COMPILE_STATUS, &good);
        if (!good)
        {
            char message[1024] = {};
            GetShaderInfoLog(shader, sizeof(message), nullptr, message);
            DeleteShader(shader);
            throw std::runtime_error(message);
        }
        return shader;
    }
    bool begin()
    {
        if (active) return true;
        if (failed) return false;
        SDL_Renderer* renderer = TVPGetPrimarySDLRenderer();
        if (!renderer) return false;
        SDL_Window* currentWindow = SDL_RenderGetWindow(renderer);
        if (!currentWindow) return false;
        if (window && currentWindow != window)
        {
            // Game ended and the launcher (or the next game) built a new SDL
            // window.  Without this the cached window/context made begin()
            // fail forever, so E-mote stayed dead after returning to the
            // launcher.
            resetForNewWindow();
        }
        SDL_RenderFlush(renderer);
        savedContext = SDL_GL_GetCurrentContext();
        savedWindow = SDL_GL_GetCurrentWindow();
        window = currentWindow;
        if (!context)
        {
            // FastLoad throttles the GPU. Retire the software-only mode
            // before creating or using this real GPU render backend.
            if (!resourcePolicy.Acquire()) {
                SDL_SetError("Cannot retire software CPU boost for GPU rendering");
                failed = true;
                return false;
            }
            // Preserve SDL's requested context attributes as well as its current
            // context. This private context never shares SDL's state cache.
            int profile = 0, major = 0, minor = 0, share = 0;
            SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile);
            SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
            SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minor);
            SDL_GL_GetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, &share);
#ifdef __SWITCH__
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
            SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
            context = SDL_GL_CreateContext(window);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
            SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, share);
            if (!context)
            {
                SDL_GL_MakeCurrent(savedWindow ? savedWindow : window, savedContext);
                resourcePolicy.Release();
                failed = true;
                return false;
            }
        }
        if (SDL_GL_MakeCurrent(window, context) != 0) return false;
        active = true;
        if (program) return true;
        try
        {
#define LOAD_GL(type, name) name = reinterpret_cast<type>(SDL_GL_GetProcAddress("gl" #name)); if (!name) throw std::runtime_error("Missing gl" #name);
            EMOTE_GL_FUNCTIONS(LOAD_GL)
#undef LOAD_GL
            const char* version = reinterpret_cast<const char*>(GetString(GL_VERSION));
            const char* extensions = reinterpret_cast<const char*>(GetString(GL_EXTENSIONS));
            if (version && std::strstr(version, "OpenGL ES 2.") &&
                (!extensions || !std::strstr(extensions, "GL_EXT_blend_minmax")))
                throw std::runtime_error("E-mote requires GL_EXT_blend_minmax");
            const GLuint vs = compile(GL_VERTEX_SHADER, vertexShader);
            const GLuint fs = compile(GL_FRAGMENT_SHADER, fragmentShader);
            program = CreateProgram();
            AttachShader(program, vs); AttachShader(program, fs);
            BindAttribLocation(program, 0, "aPos");
            BindAttribLocation(program, 1, "aTexCoord");
            LinkProgram(program);
            DeleteShader(vs); DeleteShader(fs);
            GLint linked = 0;
            GetProgramiv(program, GL_LINK_STATUS, &linked);
            if (!linked) throw std::runtime_error("E-mote mesh shader link failed");
            textureLoc = GetUniformLocation(program, "texture1");
            maskLoc = GetUniformLocation(program, "maskTexture");
            enableMaskLoc = GetUniformLocation(program, "enableMask");
            viewportLoc = GetUniformLocation(program, "viewportSize");
            opacityLoc = GetUniformLocation(program, "opa");
            colorEnabledLoc = GetUniformLocation(program, "enableColor");
            multiplyColorLoc = GetUniformLocation(program, "multiplyColor");
            colorLoc = GetUniformLocation(program, "uniformColor");
            GenBuffers(1, &vbo); GenBuffers(1, &ibo);
            Disable(GL_DEPTH_TEST); Disable(GL_SCISSOR_TEST); Disable(GL_CULL_FACE);
            Enable(GL_BLEND);
            KRKRNS_LOG("[emote] isolated GL mesh backend ready: %s", version);
            return true;
        }
        catch (const std::exception& error)
        {
            KRKRNS_LOG("[emote] isolated GL unavailable: %s", error.what());
            SDL_SetError("%s", error.what());
            end();
            SDL_GL_DeleteContext(context); context = nullptr; program = 0;
            resourcePolicy.Release();
            failed = true;
            return false;
        }
    }
    void require() { if (!begin()) throw std::runtime_error(SDL_GetError()); }
    Image* find(void* handle)
    {
        for (const auto& image : images) if (image.get() == handle) return image.get();
        return nullptr;
    }
    void bind(Image* image)
    {
        require();
        BindFramebuffer(GL_FRAMEBUFFER, image->fbo);
        Viewport(0, 0, image->width, image->height);
    }
    void readPixels(Image* image, uint8_t* destination, int tileWidth,
                    bool clearBeforeRead = false)
    {
        KrkrNSPerfScope perf(KrkrNSPerfOp::EmoteReadback);
        if (!image || !image->target || !destination)
            throw std::runtime_error("Invalid E-mote readback target");
        bind(image);
        PixelStorei(GL_PACK_ALIGNMENT, 1);
        const size_t rowBytes = size_t(image->width) * 4;
        if (clearBeforeRead)
            std::memset(destination, 0, rowBytes * image->height);

        // Only the touched region needs to come back from the GPU: the CPU
        // mirror already holds every untouched pixel from the previous frame.
        // Reading a 1920x1080 target in full cost ~8MB per E-mote draw, which
        // the device logs showed as the dominant cost of animated scenes.
        // A clear() is handled on the CPU side (the FBO is cleared to
        // transparent), so it zeroes the mirror instead of forcing a full read.
        if (!clearBeforeRead)
        {
            if (image->cleared)
                std::memset(destination, 0, rowBytes * size_t(image->height));
            if (!image->dirtyValid)
                return; // nothing was drawn: the mirror is already correct
            const int x0 = std::max(0, image->dirtyX0);
            const int y0 = std::max(0, image->dirtyY0);
            const int x1 = std::min(image->width, image->dirtyX1);
            const int y1 = std::min(image->height, image->dirtyY1);
            if (x0 >= x1 || y0 >= y1) return;
            const int w = x1 - x0;
            const int h = y1 - y0;
            const size_t scratchRow = size_t(w) * 4;
            if (tileWidth <= 0 || tileWidth >= w)
            {
                readScratch.resize(scratchRow * size_t(h));
                {
                    KrkrNSPerfScope gpuReadback(KrkrNSPerfOp::GpuReadback);
                    ReadPixels(x0, y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, readScratch.data());
                    gpuReadback.Bytes(std::uint64_t(w) * h * 4);
                }
                for (int r = 0; r < h; ++r)
                    std::memcpy(destination + size_t(y0 + r) * rowBytes + size_t(x0) * 4,
                                readScratch.data() + size_t(r) * scratchRow, scratchRow);
                return;
            }
            // Tiled readback (compatibility renderers that cap a single
            // readback row): read one tight tile at a time and scatter it.
            for (int tx = x0; tx < x1; tx += tileWidth)
            {
                const int tw = std::min(tileWidth, x1 - tx);
                const size_t tileRow = size_t(tw) * 4;
                std::vector<uint8_t> tile(tileRow * size_t(h));
                uint8_t* tilePixels = tile.data();
                {
                    KrkrNSPerfScope gpuReadback(KrkrNSPerfOp::GpuReadback);
                    ReadPixels(tx, y0, tw, h, GL_RGBA, GL_UNSIGNED_BYTE, tilePixels);
                    gpuReadback.Bytes(std::uint64_t(tw) * h * 4);
                }
                for (int r = 0; r < h; ++r)
                    std::memcpy(destination + size_t(y0 + r) * rowBytes + size_t(tx) * 4,
                                tilePixels + size_t(r) * tileRow, tileRow);
            }
            return;
        }

        if (tileWidth <= 0 || tileWidth >= image->width)
        {
            {
                KrkrNSPerfScope gpuReadback(KrkrNSPerfOp::GpuReadback);
                ReadPixels(0, 0, image->width, image->height,
                           GL_RGBA, GL_UNSIGNED_BYTE, destination);
                gpuReadback.Bytes(std::uint64_t(image->width) * image->height * 4);
            }
            return;
        }

        // Do not rely on GL_PACK_ROW_LENGTH (not core in GLES2).  Read each
        // vertical tile into a tightly packed scratch buffer, then scatter its
        // rows into the full-width CPU image.  This also avoids trusting the
        // file extension, window size, or a renderer-specific byte limit.
        for (int x = 0; x < image->width; x += tileWidth)
        {
            const int width = std::min(tileWidth, image->width - x);
            const size_t tileRowBytes = size_t(width) * 4;
            const size_t tileBytes = tileRowBytes * image->height;
            readScratch.resize(tileBytes);
            if (clearBeforeRead)
                std::fill(readScratch.begin(), readScratch.end(), 0);
            {
                KrkrNSPerfScope gpuReadback(KrkrNSPerfOp::GpuReadback);
                ReadPixels(x, 0, width, image->height,
                           GL_RGBA, GL_UNSIGNED_BYTE, readScratch.data());
                gpuReadback.Bytes(std::uint64_t(width) * image->height * 4);
            }
            for (int y = 0; y < image->height; ++y)
                std::memcpy(destination + size_t(y) * rowBytes + size_t(x) * 4,
                            readScratch.data() + size_t(y) * tileRowBytes,
                            tileRowBytes);
        }
    }
#if defined(KRKRNS_EMOTE_RESIDENT_READBACK)
    void checkResidentError()
    {
        const GLenum error = GetError();
        if (error == GL_NO_ERROR) return;
        // A candidate failure must not poison the original full-read fallback.
        for (int i = 0; i < 16 && GetError() != GL_NO_ERROR; ++i) {}
        throw std::runtime_error("E-mote resident readback GL error");
    }
    void releaseResident(Image& image)
    {
        image.previousValid = false;
        image.residentValidated = false;
        if (image.damageFbo) DeleteFramebuffers(1, &image.damageFbo);
        if (image.damageTexture) DeleteTextures(1, &image.damageTexture);
        if (image.previousTexture) DeleteTextures(1, &image.previousTexture);
        image.damageFbo = image.damageTexture = image.previousTexture = 0;
        residentStats.liveGPUBytes -= image.residentBytes;
        image.residentBytes = 0;
        image.damageWidth = image.damageHeight = 0;
        std::vector<uint8_t>().swap(image.damagePixels);
    }
    bool ensureDamageProgram()
    {
        if (damageProgram) return true;
        if (residentProgramFailed) return false;
        GLuint vs = 0, fs = 0, candidate = 0, candidateVbo = 0, candidateIbo = 0;
        try
        {
            CopyTexSubImage2D = reinterpret_cast<PFNGLCOPYTEXSUBIMAGE2DPROC>(
                SDL_GL_GetProcAddress("glCopyTexSubImage2D"));
            ColorMask = reinterpret_cast<PFNGLCOLORMASKPROC>(SDL_GL_GetProcAddress("glColorMask"));
            if (!CopyTexSubImage2D || !ColorMask)
                throw std::runtime_error("E-mote resident readback GL functions unavailable");
            vs = compile(GL_VERTEX_SHADER, vertexShader);
            fs = compile(GL_FRAGMENT_SHADER, damageFragmentShader);
            candidate = CreateProgram();
            if (!candidate) throw std::runtime_error("E-mote damage program allocation failed");
            AttachShader(candidate, vs); AttachShader(candidate, fs);
            BindAttribLocation(candidate, 0, "aPos");
            LinkProgram(candidate);
            GLint linked = 0;
            GetProgramiv(candidate, GL_LINK_STATUS, &linked);
            if (!linked) throw std::runtime_error("E-mote damage shader link failed");
            const GLint currentLoc = GetUniformLocation(candidate, "currentImage");
            const GLint previousLoc = GetUniformLocation(candidate, "previousImage");
            const GLint sizeLoc = GetUniformLocation(candidate, "imageSize");
            if (currentLoc < 0 || previousLoc < 0 || sizeLoc < 0)
                throw std::runtime_error("E-mote damage shader uniforms unavailable");
            const float positions[] = {-1.f, -1.f, 1.f, -1.f, 1.f, 1.f, -1.f, 1.f};
            const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
            GenBuffers(1, &candidateVbo); GenBuffers(1, &candidateIbo);
            if (!candidateVbo || !candidateIbo)
                throw std::runtime_error("E-mote damage geometry allocation failed");
            BindBuffer(GL_ARRAY_BUFFER, candidateVbo);
            BufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
            BindBuffer(GL_ELEMENT_ARRAY_BUFFER, candidateIbo);
            BufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
            checkResidentError();
            DeleteShader(vs); DeleteShader(fs); vs = fs = 0;
            damageProgram = candidate; damageVbo = candidateVbo; damageIbo = candidateIbo;
            damageCurrentLoc = currentLoc; damagePreviousLoc = previousLoc; damageSizeLoc = sizeLoc;
            KRKRNS_LOG("[emote-resident] exact RGBA tile comparison enabled tile=16 budget=64MiB");
            return true;
        }
        catch (...)
        {
            if (candidateIbo) DeleteBuffers(1, &candidateIbo);
            if (candidateVbo) DeleteBuffers(1, &candidateVbo);
            if (candidate) DeleteProgram(candidate);
            if (fs) DeleteShader(fs);
            if (vs) DeleteShader(vs);
            residentProgramFailed = true;
            throw;
        }
    }
    bool ensureResident(Image& image)
    {
        if (image.residentUnavailable) return false;
        if (image.width > 4096 || image.height > 4096)
        {
            image.residentUnavailable = true;
            return false;
        }
        if (image.previousTexture && image.damageTexture && image.damageFbo) return true;
        const int mapW = (image.width + 15) / 16;
        const int mapH = (image.height + 15) / 16;
        const std::uint64_t mapBytes = std::uint64_t(mapW) * mapH * 4;
        const std::uint64_t imageBytes = std::uint64_t(image.width) * image.height * 4;
        const std::uint64_t bytes = imageBytes + mapBytes;
        // The first-use copy/map contract needs a separate same-size target.
        // Include that transient payload before allocating persistent objects.
        const std::uint64_t firstUseBytes = bytes + imageBytes;
        if (firstUseBytes > ResidentBudget || residentStats.liveGPUBytes > ResidentBudget - firstUseBytes)
        {
            image.residentUnavailable = true;
            return false;
        }
        if (!ensureDamageProgram())
        {
            image.residentUnavailable = true;
            return false;
        }
        image.damagePixels.resize(static_cast<std::size_t>(mapBytes));
        ActiveTexture(GL_TEXTURE0);
        GenTextures(1, &image.previousTexture);
        GenTextures(1, &image.damageTexture);
        GenFramebuffers(1, &image.damageFbo);
        if (!image.previousTexture || !image.damageTexture || !image.damageFbo)
            throw std::runtime_error("E-mote resident textures allocation failed");
        BindTexture(GL_TEXTURE_2D, image.previousTexture);
        TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height,
                   0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        BindTexture(GL_TEXTURE_2D, image.damageTexture);
        TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mapW, mapH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        BindFramebuffer(GL_FRAMEBUFFER, image.damageFbo);
        FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image.damageTexture, 0);
        if (CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("E-mote damage framebuffer incomplete");
        checkResidentError();
        image.damageWidth = mapW; image.damageHeight = mapH;
        image.residentBytes = static_cast<std::size_t>(bytes);
        residentStats.liveGPUBytes += bytes;
        residentStats.peakGPUBytes = std::max(residentStats.peakGPUBytes, residentStats.liveGPUBytes);
        return true;
    }
    void copyResidentBaseline(Image& image)
    {
        KrkrNSPerfScope perf(KrkrNSPerfOp::GpuResidentCopy);
        bind(&image);
        ActiveTexture(GL_TEXTURE0);
        BindTexture(GL_TEXTURE_2D, image.previousTexture);
        CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, image.width, image.height);
        checkResidentError();
        image.previousValid = true;
        ++residentStats.baselineCopies;
        // This counter describes GPU-to-GPU payload, not CPU readback bytes.
        perf.Bytes(std::uint64_t(image.width) * image.height * 4);
    }
    void readDamageMap(Image& image, GLuint currentTexture = 0)
    {
        if (!currentTexture) currentTexture = image.texture;
        // Restore the old mesh path even if shader submission/readback fails.
        // All of these objects belong to this same, already-active context.
        struct Restore
        {
            Impl& gl;
            Image& image;
            GLuint comparedTexture;
            GLint activeTexture = GL_TEXTURE0, textures[2] = {}, colorMask[4] = {};
            GLint oldProgram = 0, arrayBuffer = 0, elementBuffer = 0;
            bool blend, depth, scissor, cull, dither;
            Restore(Impl& gl, Image& image, GLuint comparedTexture) :
                gl(gl), image(image), comparedTexture(comparedTexture),
                blend(gl.IsEnabled(GL_BLEND) != 0), depth(gl.IsEnabled(GL_DEPTH_TEST) != 0),
                scissor(gl.IsEnabled(GL_SCISSOR_TEST) != 0), cull(gl.IsEnabled(GL_CULL_FACE) != 0),
                dither(gl.IsEnabled(GL_DITHER) != 0)
            {
                gl.GetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
                gl.GetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
                gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
                gl.GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elementBuffer);
                gl.GetIntegerv(GL_COLOR_WRITEMASK, colorMask);
                for (int i = 0; i < 2; ++i)
                {
                    gl.ActiveTexture(GL_TEXTURE0 + i);
                    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &textures[i]);
                }
            }
            ~Restore()
            {
                gl.ActiveTexture(GL_TEXTURE0);
                gl.BindTexture(GL_TEXTURE_2D, comparedTexture);
                gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                for (int i = 0; i < 2; ++i)
                {
                    gl.ActiveTexture(GL_TEXTURE0 + i);
                    gl.BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(textures[i]));
                }
                gl.ActiveTexture(static_cast<GLenum>(activeTexture));
                gl.UseProgram(static_cast<GLuint>(oldProgram));
                gl.BindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
                gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(elementBuffer));
                gl.ColorMask(colorMask[0] != 0, colorMask[1] != 0, colorMask[2] != 0, colorMask[3] != 0);
                if (blend) gl.Enable(GL_BLEND); else gl.Disable(GL_BLEND);
                if (depth) gl.Enable(GL_DEPTH_TEST); else gl.Disable(GL_DEPTH_TEST);
                if (scissor) gl.Enable(GL_SCISSOR_TEST); else gl.Disable(GL_SCISSOR_TEST);
                if (cull) gl.Enable(GL_CULL_FACE); else gl.Disable(GL_CULL_FACE);
                if (dither) gl.Enable(GL_DITHER); else gl.Disable(GL_DITHER);
                gl.ClearColor(0, 0, 0, 0);
                gl.BindFramebuffer(GL_FRAMEBUFFER, image.fbo);
                gl.Viewport(0, 0, image.width, image.height);
            }
        };
        {
            Restore restore(*this, image, currentTexture);
            {
                KrkrNSPerfScope perf(KrkrNSPerfOp::GpuDamageCompare);
                BindFramebuffer(GL_FRAMEBUFFER, image.damageFbo);
                Viewport(0, 0, image.damageWidth, image.damageHeight);
                Disable(GL_BLEND); Disable(GL_DEPTH_TEST); Disable(GL_SCISSOR_TEST);
                Disable(GL_CULL_FACE); Disable(GL_DITHER);
                ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                // A clipped/partially written compare target must conservatively
                // request pixel reads, never classify undefined pixels as equal.
                ClearColor(1, 0, 0, 1);
                Clear(GL_COLOR_BUFFER_BIT);
                UseProgram(damageProgram);
                ActiveTexture(GL_TEXTURE0); BindTexture(GL_TEXTURE_2D, currentTexture);
                TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                ActiveTexture(GL_TEXTURE1); BindTexture(GL_TEXTURE_2D, image.previousTexture);
                Uniform1i(damageCurrentLoc, 0); Uniform1i(damagePreviousLoc, 1);
                Uniform2f(damageSizeLoc, float(image.width), float(image.height));
                BindBuffer(GL_ARRAY_BUFFER, damageVbo);
                BindBuffer(GL_ELEMENT_ARRAY_BUFFER, damageIbo);
                EnableVertexAttribArray(0);
                VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
                DrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
                checkResidentError();
            }
            {
                KrkrNSPerfScope mapPerf(KrkrNSPerfOp::GpuDamageMap);
                PixelStorei(GL_PACK_ALIGNMENT, 1);
                // The compatibility driver has truncated wide readbacks before.
                // Untouched CPU flag bytes must mean changed, rather than hits.
                std::fill(image.damagePixels.begin(), image.damagePixels.end(), 255);
                const int tile = readTileWidth > 0 ? readTileWidth : image.damageWidth;
                for (int x = 0; x < image.damageWidth; x += tile)
                {
                    const int width = std::min(tile, image.damageWidth - x);
                    const std::size_t row = std::size_t(width) * 4;
                    const std::size_t bytes = row * image.damageHeight;
                    const bool wholeMap = width == image.damageWidth;
                    uint8_t* pixels = image.damagePixels.data();
                    if (!wholeMap)
                    {
                        readScratch.resize(bytes);
                        std::fill(readScratch.begin(), readScratch.end(), 255);
                        pixels = readScratch.data();
                    }
                    {
                        KrkrNSPerfScope readPerf(KrkrNSPerfOp::GpuReadback);
                        ReadPixels(x, 0, width, image.damageHeight,
                                   GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                        checkResidentError();
                        readPerf.Bytes(bytes);
                    }
                    ++residentStats.mapReads;
                    residentStats.mapBytes += bytes;
                    if (!wholeMap)
                        for (int y = 0; y < image.damageHeight; ++y)
                            std::memcpy(image.damagePixels.data() +
                                (std::size_t(y) * image.damageWidth + x) * 4,
                                pixels + std::size_t(y) * row, row);
                }
                mapPerf.Bytes(image.damagePixels.size());
            }
        }
        checkResidentError();
    }
    void validateResidentCopy(Image& image)
    {
        if (image.residentValidated) return;
        const std::uint64_t bytes = std::uint64_t(image.width) * image.height * 4;
        if (bytes > ResidentBudget || residentStats.liveGPUBytes > ResidentBudget - bytes)
            throw std::runtime_error("E-mote resident copy-probe budget unavailable");
        // Probe a separate, same-size framebuffer. Neither the game's target
        // nor its CPU mirror is changed by this compatibility contract.
        struct Probe
        {
            Impl& gl;
            Image& image;
            GLuint texture = 0, fbo = 0;
            const std::uint64_t bytes, mapReadsBefore, mapBytesBefore;
            GLint activeTexture = GL_TEXTURE0, textures[2] = {}, colorMask[4] = {};
            bool scissor;
            Probe(Impl& gl, Image& image, std::uint64_t bytes) : gl(gl), image(image),
                bytes(bytes), mapReadsBefore(gl.residentStats.mapReads),
                mapBytesBefore(gl.residentStats.mapBytes), scissor(gl.IsEnabled(GL_SCISSOR_TEST) != 0)
            {
                gl.GetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
                gl.GetIntegerv(GL_COLOR_WRITEMASK, colorMask);
                for (int i = 0; i < 2; ++i)
                {
                    gl.ActiveTexture(GL_TEXTURE0 + i);
                    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &textures[i]);
                }
                gl.residentStats.liveGPUBytes += bytes;
                gl.residentStats.peakGPUBytes = std::max(gl.residentStats.peakGPUBytes,
                                                       gl.residentStats.liveGPUBytes);
            }
            ~Probe()
            {
                if (fbo) gl.DeleteFramebuffers(1, &fbo);
                if (texture) gl.DeleteTextures(1, &texture);
                gl.residentStats.liveGPUBytes -= bytes;
                gl.residentStats.probeMapReads += gl.residentStats.mapReads - mapReadsBefore;
                gl.residentStats.probeMapBytes += gl.residentStats.mapBytes - mapBytesBefore;
                for (int i = 0; i < 2; ++i)
                {
                    gl.ActiveTexture(GL_TEXTURE0 + i);
                    gl.BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(textures[i]));
                }
                gl.ActiveTexture(static_cast<GLenum>(activeTexture));
                gl.ColorMask(colorMask[0] != 0, colorMask[1] != 0, colorMask[2] != 0, colorMask[3] != 0);
                if (scissor) gl.Enable(GL_SCISSOR_TEST); else gl.Disable(GL_SCISSOR_TEST);
                gl.ClearColor(0, 0, 0, 0);
                gl.BindFramebuffer(GL_FRAMEBUFFER, image.fbo);
                gl.Viewport(0, 0, image.width, image.height);
            }
        } probe(*this, image, bytes);
        try
        {
            ActiveTexture(GL_TEXTURE0);
            GenTextures(1, &probe.texture);
            GenFramebuffers(1, &probe.fbo);
            if (!probe.texture || !probe.fbo)
                throw std::runtime_error("E-mote copy-probe allocation failed");
            BindTexture(GL_TEXTURE_2D, probe.texture);
            TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height,
                       0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            BindFramebuffer(GL_FRAMEBUFFER, probe.fbo);
            Viewport(0, 0, image.width, image.height);
            Disable(GL_SCISSOR_TEST);
            ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            // Start the destination at a known different value. Undefined
            // texture memory that happens to match a probe is not a proof.
            FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                 GL_TEXTURE_2D, image.previousTexture, 0);
            if (CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                throw std::runtime_error("E-mote copy-probe baseline framebuffer incomplete");
            ClearColor(0, 0, 0, 0); Clear(GL_COLOR_BUFFER_BIT);
            FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                 GL_TEXTURE_2D, probe.texture, 0);
            if (CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                throw std::runtime_error("E-mote copy-probe framebuffer incomplete");
            ClearColor(0.25f, 0.5f, 0.75f, 1.f); Clear(GL_COLOR_BUFFER_BIT);
            {
                KrkrNSPerfScope perf(KrkrNSPerfOp::GpuResidentCopy);
                BindTexture(GL_TEXTURE_2D, image.previousTexture);
                CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, image.width, image.height);
                checkResidentError();
                ++residentStats.probeCopies;
                perf.Bytes(bytes);
            }
            readDamageMap(image, probe.texture);
            for (std::size_t p = 0; p < image.damagePixels.size(); p += 4)
                if (image.damagePixels[p] != 0)
                    throw std::runtime_error("E-mote resident full-width copy contract failed");

            // The map helper restores the ACTUAL target framebuffer. Explicitly
            // rebind the temporary probe before clearing; never erase game data.
            BindFramebuffer(GL_FRAMEBUFFER, probe.fbo);
            Viewport(0, 0, image.width, image.height);
            Disable(GL_SCISSOR_TEST); ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            ClearColor(0, 0, 0, 0); Clear(GL_COLOR_BUFFER_BIT);
            checkResidentError();
            readDamageMap(image, probe.texture);
            for (std::size_t p = 0; p < image.damagePixels.size(); p += 4)
                if (image.damagePixels[p] == 0)
                    throw std::runtime_error("E-mote resident clear/map coverage contract failed");
            image.residentValidated = true;
            image.previousValid = false;
            ++residentStats.probePasses;
        }
        catch (...)
        {
            ++residentStats.probeFailures;
            image.residentValidated = false;
            image.previousValid = false;
            throw;
        }
    }
    void readResidentRect(Image& image, const krkrns_emote_readback::Rect& rect)
    {
        bind(&image);
        PixelStorei(GL_PACK_ALIGNMENT, 1);
        const int tile = readTileWidth > 0 ? readTileWidth : rect.w;
        const std::size_t destinationRow = std::size_t(image.width) * 4;
        for (int x = rect.x; x < rect.x + rect.w; x += tile)
        {
            const int width = std::min(tile, rect.x + rect.w - x);
            const std::size_t row = std::size_t(width) * 4;
            const std::size_t bytes = row * rect.h;
            readScratch.resize(bytes);
            {
                KrkrNSPerfScope perf(KrkrNSPerfOp::GpuReadback);
                ReadPixels(x, rect.y, width, rect.h, GL_RGBA, GL_UNSIGNED_BYTE, readScratch.data());
                checkResidentError();
                perf.Bytes(bytes);
            }
            ++residentStats.dataRects;
            residentStats.dataBytes += bytes;
            for (int y = 0; y < rect.h; ++y)
                std::memcpy(image.pixels.data() + std::size_t(rect.y + y) * destinationRow + std::size_t(x) * 4,
                            readScratch.data() + std::size_t(y) * row, row);
        }
    }
    void readResidentFull(Image& image)
    {
        ++residentStats.fullReads;
        const krkrns_emote_readback::Rect whole{0, 0, image.width, image.height};
        readResidentRect(image, whole);
    }
    void readResident(Image& image)
    {
        KrkrNSPerfScope perf(KrkrNSPerfOp::EmoteReadback);
        bool used = false;
        try
        {
            checkResidentError();
            if (ensureResident(image))
            {
                used = true;
                validateResidentCopy(image);
                if (!image.previousValid)
                {
                    // Every pixel of the CPU mirror and GPU baseline now has
                    // one common generation; no old dirty-rectangle assumption.
                    readResidentFull(image);
                    copyResidentBaseline(image);
                }
                else if (!image.dirtyValid && !image.cleared)
                {
                    ++residentStats.cacheHits;
                    image.layerDamage.SetDamage({});
                }
                else
                {
                    readDamageMap(image);
                    krkrns_emote_readback::Plan plan;
                    if (!krkrns_emote_readback::BuildPlan(image.damagePixels.data(),
                        image.damagePixels.size(), std::size_t(image.damageWidth) * 4,
                        image.width, image.height, readTileWidth, plan, 8))
                        throw std::runtime_error("E-mote damage readback plan invalid");
                    if (plan.rects.empty())
                    {
                        ++residentStats.cacheHits;
                        image.layerDamage.SetDamage({});
                    }
                    else
                    {
                        for (const auto& rect : plan.rects)
                        {
                            if (rect.x == 0 && rect.y == 0 && rect.w == image.width && rect.h == image.height)
                                ++residentStats.fullReads;
                            readResidentRect(image, rect);
                        }
                        // Only publish after all CPU rectangles and the whole
                        // GPU baseline have reached the same current frame.
                        copyResidentBaseline(image);
                        EmoteLayerBounds bounds{image.width, image.height, 0, 0};
                        for (const auto& rect : plan.rects)
                        {
                            bounds.left = std::min(bounds.left, rect.x);
                            bounds.top = std::min(bounds.top, rect.y);
                            bounds.right = std::max(bounds.right, rect.x + rect.w);
                            bounds.bottom = std::max(bounds.bottom, rect.y + rect.h);
                        }
                        image.layerDamage.SetDamage(bounds);
                    }
                }
            }
        }
        catch (const std::exception& error)
        {
            ++residentStats.errors;
            image.residentUnavailable = true;
            releaseResident(image);
            KRKRNS_LOG("[emote-resident] full-read fallback: %s", error.what());
            for (int i = 0; i < 16 && GetError() != GL_NO_ERROR; ++i) {}
            used = false;
        }
        catch (...)
        {
            ++residentStats.errors;
            image.residentUnavailable = true;
            releaseResident(image);
            for (int i = 0; i < 16 && GetError() != GL_NO_ERROR; ++i) {}
            used = false;
        }
        if (!used)
        {
            // A failed rectangle transfer may already have written some CPU
            // pixels. Overwrite the complete target, including all old clears.
            image.previousValid = false;
            ++residentStats.fallbacks;
            readResidentFull(image);
        }
        bind(&image);
        UseProgram(program);
        if (++residentLocks == 1 || residentLocks % 60 == 0)
            KRKRNS_LOG("[emote-resident] locks=%llu full=%llu map=%llu mapBytes=%llu hits=%llu rects=%llu dataBytes=%llu errors=%llu fallback=%llu copies=%llu gpuBytes=%llu probePass=%llu probeFail=%llu probeMap=%llu probeMapBytes=%llu probeCopy=%llu peakGPUBytes=%llu",
                (unsigned long long)residentLocks, (unsigned long long)residentStats.fullReads,
                (unsigned long long)residentStats.mapReads, (unsigned long long)residentStats.mapBytes,
                (unsigned long long)residentStats.cacheHits, (unsigned long long)residentStats.dataRects,
                (unsigned long long)residentStats.dataBytes, (unsigned long long)residentStats.errors,
                (unsigned long long)residentStats.fallbacks, (unsigned long long)residentStats.baselineCopies,
                (unsigned long long)residentStats.liveGPUBytes,
                (unsigned long long)residentStats.probePasses, (unsigned long long)residentStats.probeFailures,
                (unsigned long long)residentStats.probeMapReads, (unsigned long long)residentStats.probeMapBytes,
                (unsigned long long)residentStats.probeCopies, (unsigned long long)residentStats.peakGPUBytes);
    }
#endif
    void* create(int width, int height, bool renderTarget)
    {
        if (width <= 0 || height <= 0) return nullptr;
        Scope scope(*this);
        auto image = std::make_unique<Image>();
        image->width = width; image->height = height; image->target = renderTarget;
        image->pixels.resize(size_t(width) * height * 4);
        GenTextures(1, &image->texture);
        BindTexture(GL_TEXTURE_2D, image->texture);
        TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image->pixels.data());
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, renderTarget ? GL_LINEAR : GL_LINEAR_MIPMAP_LINEAR);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        if (renderTarget)
        {
            GenFramebuffers(1, &image->fbo);
            BindFramebuffer(GL_FRAMEBUFFER, image->fbo);
            FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image->texture, 0);
            if (CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            {
                DeleteFramebuffers(1, &image->fbo); DeleteTextures(1, &image->texture);
                throw std::runtime_error("E-mote framebuffer incomplete");
            }
        }
        else GenerateMipmap(GL_TEXTURE_2D);
        auto* result = image.get();
        try { images.push_back(std::move(image)); }
        catch (...)
        {
            // A failed vector growth has not published this object. Retire its
            // GPU allocation while the private context is still current.
            if (image)
            {
                if (image->fbo) DeleteFramebuffers(1, &image->fbo);
                if (image->texture) DeleteTextures(1, &image->texture);
            }
            throw;
        }
        return result;
    }
    void destroy(void* handle)
    {
        auto* image = find(handle);
        if (!image) return;
        Scope scope(*this);
#if defined(KRKRNS_EMOTE_RESIDENT_READBACK)
        releaseResident(*image);
#endif
        if (image->fbo) DeleteFramebuffers(1, &image->fbo);
        DeleteTextures(1, &image->texture);
        if (target == image) target = nullptr;
        if (mask == image) mask = nullptr;
        images.erase(std::remove_if(images.begin(), images.end(),
            [image](const std::unique_ptr<Image>& item) { return item.get() == image; }), images.end());
    }
    void update(void* handle, const uint8_t* pixels, int width, int height, int pitch)
    {
        auto* image = find(handle);
        const std::ptrdiff_t stride = pitch;
        if (!image || !pixels || width <= 0 || height <= 0 || width > image->width || height > image->height ||
            (stride < std::ptrdiff_t(width) * 4 && stride > -std::ptrdiff_t(width) * 4))
            throw std::runtime_error("Invalid E-mote texture update dimensions/pitch");
        Scope scope(*this);
#if defined(KRKRNS_EMOTE_RESIDENT_READBACK)
        // UpdateTargetTexture changes the CPU mirror without a matching GPU
        // baseline generation. Re-establish both with a complete next read.
        if (image->target) image->previousValid = false;
#endif
        for (int y = 0; y < height; ++y)
            std::memcpy(image->pixels.data() + size_t(y) * image->width * 4,
                        pixels + std::ptrdiff_t(y) * stride, size_t(width) * 4);
        BindTexture(GL_TEXTURE_2D, image->texture);
        PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image->width, image->height,
                     GL_RGBA, GL_UNSIGNED_BYTE, image->pixels.data());
        if (!image->target) GenerateMipmap(GL_TEXTURE_2D);
    }
};

EmoteGLRenderBackend::EmoteGLRenderBackend(bool useResidentReadback) : impl(new Impl)
{
    impl->residentRequested = useResidentReadback;
}
EmoteResidentReadbackStats EmoteGLRenderBackend::GetResidentReadbackStats() const noexcept
{
    return impl->residentStats;
}
EmoteGLRenderBackend::~EmoteGLRenderBackend()
{
    // SDL may already have destroyed the window at process shutdown. GL owns
    // the image objects, so deleting the private context releases them together.
    if (SDL_WasInit(SDL_INIT_VIDEO) && impl->context)
    {
        impl->end();
        SDL_GL_DeleteContext(impl->context);
    }
}
void EmoteGLRenderBackend::ResetForEngineRestart()
{
    // The engine restart destroyed the SDL window this context was built
    // against.  begin()'s window-pointer check normally covers a new window,
    // but SDL routinely hands the replacement the SAME address, in which case
    // the dead context is kept and mesh drawing/readback corrupts (observed
    // as E-mote art filling only the left quarter of the screen on the
    // emulator).  Drop unconditionally; the next begin() rebuilds against the
    // current window.  resetForNewWindow() issues no GL calls, which matters
    // because the old context is already gone here.
    impl->resetForNewWindow();
}
bool EmoteGLRenderBackend::IsAvailable()
{
    bool ready = impl->begin();
    if (ready)
    {
        ready = selfTest();
    }
    impl->end();
    return ready;
}

bool EmoteGLRenderBackend::RetireUnusedContext() noexcept
{
    if (!impl->images.empty()) return false;
    impl->end();
    if (impl->context && SDL_WasInit(SDL_INIT_VIDEO))
    {
        // end() normally restores SDL's renderer context. If that restore
        // failed, retry before deletion rather than leaving SDL bound to a
        // dead private context. A self-saved context can only be unbound.
        if (SDL_GL_GetCurrentContext() == impl->context)
        {
            const SDL_GLContext restore = impl->savedContext == impl->context
                ? nullptr : impl->savedContext;
            if (SDL_GL_MakeCurrent(impl->savedWindow ? impl->savedWindow : impl->window,
                                   restore) != 0)
            {
                KRKRNS_LOG("[emote] cannot retire rejected GL context: %s", SDL_GetError());
                return false;
            }
        }
        SDL_GL_DeleteContext(impl->context);
    }
    // The private context is now deleted (or SDL video already retired it).
    // Clear its handles, then release the lease; this is not a live-window
    // shortcut through ResetForEngineRestart().
    impl->resetForNewWindow();
    impl->failed = true;
    return true;
}

bool EmoteGLRenderBackend::selfTest()
{
    // Draw a full-target quad through the same public API the compositor uses.
    // Verify rasterization with independent corner reads, then choose the
    // largest readback width that reconstructs the complete target.  A tiled
    // readback is a correctness workaround for compatibility renderers that
    // return only the first part of each row for a large single call.
    const int W = 1280, H = 720;
    void* target = CreateTarget(W, H);
    void* tex = CreateTexture(1, 1);
    if (!target || !tex)
    {
        DestroyTarget(target);
        DestroyTexture(tex);
        KRKRNS_LOG("[emote] GL probe FAIL: probe resources");
        return false;
    }
    const uint8_t red[4] = {255, 0, 0, 255};
    UpdateTexture(tex, red, 1, 1, 4);
    SetTarget(target);
    const float verts[16] = {
        -1.f, -1.f, 0.f, 0.f,
         1.f, -1.f, 1.f, 0.f,
         1.f,  1.f, 1.f, 1.f,
        -1.f,  1.f, 0.f, 1.f,
    };
    const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    const int savedBlend = impl->blend;
    impl->blend = 21; // normal alpha blend: opaque red quad overwrites
    // blend==21 enables uniformColor (ShaderContract); its default is BLACK,
    // which painted the probe quad opaque black — restore white for the probe.
    float savedColor[4];
    std::memcpy(savedColor, impl->color, sizeof(savedColor));
    const float white[4] = {1.f, 1.f, 1.f, 1.f};
    std::memcpy(impl->color, white, sizeof(white));
    bool cornersOk = true;
    bool readbackOk = false;
    uint8_t corners[4][4] = {};
    double selectedRedFrac = 0.0;
    try
    {
        DrawMesh(verts, 4, idx, 6, tex, 1.0f);
        impl->PixelStorei(GL_PACK_ALIGNMENT, 1);
        const int probes[4][2] = {{0, 0}, {W - 1, 0}, {0, H - 1}, {W - 1, H - 1}};
        for (int p = 0; p < 4; ++p)
        {
            corners[p][0] = corners[p][1] = corners[p][2] = corners[p][3] = 0;
            impl->ReadPixels(probes[p][0], probes[p][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corners[p]);
            if (corners[p][0] < 200 || corners[p][3] < 200) cornersOk = false;
        }
        std::vector<uint8_t> full((size_t)W * H * 4);
        const int candidates[] = {W, 512, 320, 256, 128, 64};
        int previous = -1;
        for (int candidate : candidates)
        {
            candidate = std::min(candidate, W);
            if (candidate == previous) continue;
            previous = candidate;
            const int mode = candidate == W ? 0 : candidate;
            impl->readPixels(impl->find(target), full.data(), mode, true);
            unsigned redPx = 0;
            for (size_t i = 0; i < full.size(); i += 4)
                if (full[i] > 200 && full[i + 3] > 200) redPx++;
            selectedRedFrac = (double)redPx / ((double)W * H);
            KRKRNS_LOG("[emote] GL readback probe %s%d fullRed=%.1f%%",
                       mode ? "tile=" : "full=", mode ? mode : W,
                       selectedRedFrac * 100.0);
            if (selectedRedFrac >= 0.90)
            {
                impl->readTileWidth = mode;
                readbackOk = true;
                break;
            }
        }
        if (!cornersOk || !readbackOk)
            KRKRNS_LOG("[emote] GL probe readback: corner_px=%d%d%d%d bestRed=%.1f%%",
                       corners[0][0], corners[1][0], corners[2][0], corners[3][0],
                       selectedRedFrac * 100.0);
    }
    catch (...)
    {
        cornersOk = false;
        readbackOk = false;
    }
    std::memcpy(impl->color, savedColor, sizeof(savedColor));
    impl->blend = savedBlend;
    SetTarget(nullptr);
    DestroyTarget(target);
    DestroyTexture(tex);
    const char* rend = (const char*)impl->GetString(GL_RENDERER);
    const char* ver = (const char*)impl->GetString(GL_VERSION);
    const bool ok = cornersOk && readbackOk;
    KRKRNS_LOG("[emote] GL probe %s x=%d y=%d corners=%s readback=%s%d %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x renderer=%s version=%s",
               ok ? "PASS" : "FAIL", W, H, cornersOk ? "red" : "MISMATCH",
               impl->readTileWidth ? "tile=" : "full=", impl->readTileWidth ? impl->readTileWidth : W,
               corners[0][0], corners[0][1], corners[0][2], corners[0][3],
               corners[1][0], corners[1][1], corners[1][2], corners[1][3],
               corners[2][0], corners[2][1], corners[2][2], corners[2][3],
               corners[3][0], corners[3][1], corners[3][2], corners[3][3],
               rend ? rend : "?", ver ? ver : "?");
    return ok;
}
void* EmoteGLRenderBackend::CreateTarget(int w, int h) { return impl->create(w, h, true); }
bool EmoteGLRenderBackend::GetTargetSize(void* target, int& width, int& height) const
{
    auto* image = impl->find(target);
    if (!image || !image->target) return false;
    width = image->width;
    height = image->height;
    return true;
}
void* EmoteGLRenderBackend::CreateTexture(int w, int h) { return impl->create(w, h, false); }
void EmoteGLRenderBackend::DestroyTarget(void* target) { impl->destroy(target); }
void EmoteGLRenderBackend::DestroyTexture(void* texture) { impl->destroy(texture); }
void EmoteGLRenderBackend::SetTarget(void* target)
{
    impl->target = impl->find(target);
    if (impl->target && impl->target->target) impl->bind(impl->target);
    else impl->target = nullptr;
}
void EmoteGLRenderBackend::ClearTarget(bool clear)
{
    if (clear && impl->target)
    {
        impl->bind(impl->target);
        impl->ClearColor(0, 0, 0, 0); impl->Clear(GL_COLOR_BUFFER_BIT);
        // The whole target was wiped. The CPU mirror is zeroed on the next
        // readback instead of re-reading the cleared FBO from the GPU.
        impl->target->cleared = true;
        impl->target->dirtyValid = false;
    }
}
uint8_t* EmoteGLRenderBackend::LockTarget(void* handle, int& pitch)
{
    auto* target = impl->find(handle);
    if (!target || !target->target) return nullptr;
    target->layerDamage.BeginLock();
    impl->bind(target);
    pitch = target->width * 4;
    {
        // GPU->CPU readback is the E-mote frame's biggest single cost; timed
        // separately from the RGBA->BGRA convert that follows in the layer
        // bridge so the next optimization (direct/format readback vs
        // passthrough) can be chosen from data.
        const Uint64 t0 = SDL_GetPerformanceCounter();
#if defined(KRKRNS_EMOTE_RESIDENT_READBACK)
        if (impl->residentRequested && !target->residentUnavailable && !impl->residentProgramFailed)
            impl->readResident(*target);
        else
#endif
            impl->readPixels(target, target->pixels.data(), impl->readTileWidth);
        krkrsdl2_prof_emote_lock(
            (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 /
            (double)SDL_GetPerformanceFrequency());
    }
    target->clearDirty();
    // Preserve the upstream framebuffer's RGBA/alpha equations exactly.
    // Row 0 maps to logical row 0 of E-mote's clip-space mesh at the Layer bridge.
    return target->pixels.data();
}
void EmoteGLRenderBackend::UnlockTarget(void*) { impl->end(); }
bool EmoteGLRenderBackend::GetLayerDamage(void* handle, const EmoteLayerIdentity& layer,
                                         EmoteLayerBounds& bounds) const noexcept
{
    auto* target = impl->find(handle);
    return target && target->target && target->layerDamage.GetDamage(layer, bounds);
}
void EmoteGLRenderBackend::CommitLayerDamage(void* handle, const EmoteLayerIdentity& layer) noexcept
{
    auto* target = impl->find(handle);
    if (target && target->target) target->layerDamage.Commit(layer);
}
void* EmoteGLRenderBackend::GetTargetTexture(void* target) { return impl->find(target); }
void EmoteGLRenderBackend::UpdateTargetTexture(void* t, const uint8_t* p, int w, int h, int pitch) { impl->update(t, p, w, h, pitch); }
void EmoteGLRenderBackend::UpdateTexture(void* t, const uint8_t* p, int w, int h, int pitch) { impl->update(t, p, w, h, pitch); }
uint8_t* EmoteGLRenderBackend::LockTexture(void* texture, int& pitch)
{
    auto* image = impl->find(texture);
    if (!image) return nullptr;
    // This API exports a writable CPU mirror. Even a read-only caller cannot
    // promise that a previously obtained pointer was not used to modify it.
    if (image->target) image->layerDamage.Invalidate();
    pitch = image->width * 4;
    return image->pixels.data();
}
void EmoteGLRenderBackend::SetMask(void* mask) { impl->mask = impl->find(mask); }
void EmoteGLRenderBackend::SetBlendMode(int mode, const float* color)
{
    impl->blend = mode;
    if (color) std::memcpy(impl->color, color, sizeof(impl->color));
}
void EmoteGLRenderBackend::DrawMesh(const float* vertices, int count, const uint16_t* indices,
                                   int indexCount, void* texture, float opacity)
{
    auto& gl = *impl;
    auto* image = gl.find(texture);
    if (!image || !gl.target || gl.blend == 6 || !vertices || !indices || count <= 0 || indexCount <= 0) return;
    for (int i = 0; i < indexCount; ++i)
        if (indices[i] >= count) throw std::runtime_error("E-mote mesh index out of bounds");
    gl.bind(gl.target);
    gl.UseProgram(gl.program);
    gl.BindBuffer(GL_ARRAY_BUFFER, gl.vbo);
    gl.BufferData(GL_ARRAY_BUFFER, size_t(count) * 4 * sizeof(float), vertices, GL_STREAM_DRAW);
#if defined(KRKRNS_EMOTE_VERBOSE_DIAGNOSTICS)
    {
        // KRKR-ns diagnostic: actual clip-space vertex bounds per mesh draw.
        static int boundsLogs = 0;
        if (boundsLogs < 60)
        {
            float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
            for (int i = 0; i < count; ++i)
            {
                float x = vertices[i * 4 + 0], y = vertices[i * 4 + 1];
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
            boundsLogs++;
            // KRKR-ns diagnostic: how much of the texture actually holds
            // decoded pixels? A right edge far below image->width means the
            // icon decode/upload stopped early (left strip symptom).
            int rightCol = -1;
            if (!image->pixels.empty())
            {
                const int w = image->width, h = image->height;
                for (int x = w - 1; x >= 0 && rightCol < 0; --x)
                    for (int y = 0; y < h; y += 7)
                    {
                        const size_t o = (size_t(y) * w + x) * 4;
                        if (image->pixels[o] | image->pixels[o + 1] |
                            image->pixels[o + 2] | image->pixels[o + 3])
                        {
                            rightCol = x;
                            break;
                        }
                    }
            }
            int opaqueSamples = 0, seenSamples = 0;
            if (!image->pixels.empty())
            {
                const int w = image->width, h = image->height;
                for (int y = 0; y < h; y += 13)
                    for (int x = 0; x < w; x += 11)
                    {
                        const size_t o = (size_t(y) * w + x) * 4;
                        const uint8_t r = image->pixels[o], g = image->pixels[o + 1],
                                      b = image->pixels[o + 2], a = image->pixels[o + 3];
                        if (r | g | b | a)
                        {
                            seenSamples++;
                            if (a > 128) opaqueSamples++;
                        }
                    }
            }
            KRKRNS_LOG("[emote] mesh bounds x=[%.3f,%.3f] y=[%.3f,%.3f] verts=%d tex=%dx%d contentRight=%d blend=%d alphaOpaque=%d/%d",
                       minX, maxX, minY, maxY, count, image->width, image->height, rightCol, gl.blend,
                       opaqueSamples, seenSamples);
            // KRKR-ns diagnostic: actual GL viewport/scissor at draw time —
            // the backend shares the SDL renderer's context, and stale SDL
            // state (clip rect / viewport) would clip the mesh rasterization.
            GLint vp[4] = {}, sc[4] = {};
            gl.GetIntegerv(GL_VIEWPORT, vp);
            gl.GetIntegerv(GL_SCISSOR_BOX, sc);
            GLboolean scissorOn = gl.IsEnabled(GL_SCISSOR_TEST);
            KRKRNS_LOG("[emote] GL viewport=%d,%d %dx%d scissor(%s)=%d,%d %dx%d targetWH=%dx%d fbo=%u",
                       vp[0], vp[1], vp[2], vp[3], scissorOn ? "on" : "off",
                       sc[0], sc[1], sc[2], sc[3],
                       gl.target->width, gl.target->height, gl.target->fbo);
        }
    }
#endif
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl.ibo);
    gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, size_t(indexCount) * sizeof(uint16_t), indices, GL_STREAM_DRAW);
    gl.EnableVertexAttribArray(0); gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
    gl.ActiveTexture(GL_TEXTURE0); gl.BindTexture(GL_TEXTURE_2D, image->texture);
    gl.Uniform1i(gl.textureLoc, 0);
    gl.Uniform1i(gl.enableMaskLoc, gl.mask != nullptr);
    gl.Uniform1i(gl.maskLoc, 1);
    if (gl.mask) { gl.ActiveTexture(GL_TEXTURE1); gl.BindTexture(GL_TEXTURE_2D, gl.mask->texture); }
    gl.Uniform2f(gl.viewportLoc, float(gl.target->width), float(gl.target->height));
    gl.Uniform1f(gl.opacityLoc, opacity);
    gl.Uniform1i(gl.colorEnabledLoc, gl.blend == 21);
    gl.Uniform1i(gl.multiplyColorLoc, gl.blend == 19);
    gl.Uniform4fv(gl.colorLoc, 1, gl.color);
    switch (gl.blend)
    {
    case 19:
        gl.BlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        break;
    case 1: case 4:
        gl.BlendFuncSeparate(GL_DST_COLOR, GL_ONE, GL_ZERO, GL_ONE);
        gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        break;
    case 21:
        gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        break;
    default:
        gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
        gl.BlendEquationSeparate(GL_FUNC_ADD, 0x8008 /* GL_MAX / GL_MAX_EXT */);
        break;
    }
    gl.DrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, nullptr);
    // Track the drawn region in target pixels so LockTarget can read back only
    // what this frame actually touched (a0 clip space -> framebuffer pixels).
    {
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        for (int i = 0; i < count; ++i)
        {
            const float x = vertices[i * 4 + 0], y = vertices[i * 4 + 1];
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
        if (minX <= maxX && minY <= maxY)
        {
            const float w = float(gl.target->width), h = float(gl.target->height);
            // 1px margin absorbs the linear-filter footprint at the edges.
            const int x0 = std::max(0, int(std::floor((minX + 1.0f) * 0.5f * w)) - 1);
            const int x1 = std::min(gl.target->width, int(std::ceil((maxX + 1.0f) * 0.5f * w)) + 1);
            const int y0 = std::max(0, int(std::floor((minY + 1.0f) * 0.5f * h)) - 1);
            const int y1 = std::min(gl.target->height, int(std::ceil((maxY + 1.0f) * 0.5f * h)) + 1);
            gl.target->markDirty(x0, y0, x1, y1);
        }
    }
#if defined(__SWITCH__) && defined(KRKRNS_EMOTE_CAPTURE_DIAGNOSTICS)
    // KRKR-ns diagnostic: snapshot the target after each of the first draws so
    // the exact call that produces (or destroys) visible content is identified.
    static int fboSnaps = 0;
    if (fboSnaps < 12)
    {
        fboSnaps++;
        gl.Flush();
        std::vector<uint8_t> snap(size_t(gl.target->width) * gl.target->height * 4);
        gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
        gl.ReadPixels(0, 0, gl.target->width, gl.target->height, GL_RGBA,
                      GL_UNSIGNED_BYTE, snap.data());
        char path[128];
        snprintf(path, sizeof(path), KRKRNS_BASE_A "/emote-draw-%02d.bmp", fboSnaps);
        SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormatFrom(
            snap.data(), gl.target->width, gl.target->height, 32,
            gl.target->width * 4, SDL_PIXELFORMAT_ABGR8888);
        if (shot)
        {
            SDL_SaveBMP(shot, path);
            SDL_FreeSurface(shot);
        }
    }
#endif
}
}
