#include "KrkrNSPaths.h"
#include "EmoteGLRenderBackend.h"
#include "KrkrNSLog.h"
#include "KrkrNSProf.h"
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
uniform vec4 uniformColor;
void main() {
    if (enableMask && texture2D(maskTexture, gl_FragCoord.xy / viewportSize).a < 0.5)
        discard;
    vec4 color = texture2D(texture1, texCoord);
    if (enableColor) color = vec4(uniformColor.rgb, uniformColor.a * color.a);
    color.a *= opa;
    gl_FragColor = color;
}
)";

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
    SDL_Window* savedWindow = nullptr;
    bool active = false, failed = false;
    GLuint program = 0, vbo = 0, ibo = 0;
    GLint textureLoc, maskLoc, enableMaskLoc, viewportLoc, opacityLoc, colorEnabledLoc, colorLoc;
    Image* target = nullptr;
    Image* mask = nullptr;
    int blend = 0;
    float color[4] = {};
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
        savedWindow = nullptr; savedContext = nullptr;
        // readTileWidth is a property of the RENDERER (the self-test-measured
        // largest full-row readback the driver performs honestly), not of the
        // window -- keep it, or a truncating renderer (the emulator) would
        // silently fall back to corrupting full-width reads after an engine
        // restart.  readScratch is resized on demand.
        readScratch.clear();
        active = false; failed = false; blend = 0;
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
                ReadPixels(x0, y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, readScratch.data());
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
                ReadPixels(tx, y0, tw, h, GL_RGBA, GL_UNSIGNED_BYTE, tile.data());
                for (int r = 0; r < h; ++r)
                    std::memcpy(destination + size_t(y0 + r) * rowBytes + size_t(tx) * 4,
                                tile.data() + size_t(r) * tileRow, tileRow);
            }
            return;
        }

        if (tileWidth <= 0 || tileWidth >= image->width)
        {
            ReadPixels(0, 0, image->width, image->height,
                       GL_RGBA, GL_UNSIGNED_BYTE, destination);
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
            ReadPixels(x, 0, width, image->height,
                       GL_RGBA, GL_UNSIGNED_BYTE, readScratch.data());
            for (int y = 0; y < image->height; ++y)
                std::memcpy(destination + size_t(y) * rowBytes + size_t(x) * 4,
                            readScratch.data() + size_t(y) * tileRowBytes,
                            tileRowBytes);
        }
    }
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
        images.push_back(std::move(image));
        return result;
    }
    void destroy(void* handle)
    {
        auto* image = find(handle);
        if (!image) return;
        Scope scope(*this);
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

EmoteGLRenderBackend::EmoteGLRenderBackend() : impl(new Impl) {}
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
    impl->bind(target);
    pitch = target->width * 4;
    {
        // GPU->CPU readback is the E-mote frame's biggest single cost; timed
        // separately from the RGBA->BGRA convert that follows in the layer
        // bridge so the next optimization (direct/format readback vs
        // passthrough) can be chosen from data.
        const Uint64 t0 = SDL_GetPerformanceCounter();
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
void* EmoteGLRenderBackend::GetTargetTexture(void* target) { return impl->find(target); }
void EmoteGLRenderBackend::UpdateTargetTexture(void* t, const uint8_t* p, int w, int h, int pitch) { impl->update(t, p, w, h, pitch); }
void EmoteGLRenderBackend::UpdateTexture(void* t, const uint8_t* p, int w, int h, int pitch) { impl->update(t, p, w, h, pitch); }
uint8_t* EmoteGLRenderBackend::LockTexture(void* texture, int& pitch)
{
    auto* image = impl->find(texture);
    if (!image) return nullptr;
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
    gl.Uniform4fv(gl.colorLoc, 1, gl.color);
    switch (gl.blend)
    {
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
