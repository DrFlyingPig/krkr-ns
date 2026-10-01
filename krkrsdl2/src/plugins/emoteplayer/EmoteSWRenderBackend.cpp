#include "EmoteSWRenderBackend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <cstddef>
#include <stdexcept>

#ifdef __SWITCH__
#include <arm_neon.h>
#include <switch.h>
#elif defined(KRKRNS_EMOTE_TEST_NEON)
// Exercise the Switch pixel loop on the host with the same NEON operations.
#define SIMDE_ENABLE_NATIVE_ALIASES
#include "../../../external/simde/simde/arm/neon.h"
#endif

#include <SDL.h>

#include "KrkrNSLog.h"
extern SDL_Renderer* TVPGetPrimarySDLRenderer();

#include "ThreadIntf.h"

namespace krkrsdl3
{
namespace
{
struct ColorRGBA
{
    uint8_t r, g, b, a;
};

static inline uint8_t Clampf(float v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

// Match the pinned GL mesh shader and fixed-function blend equations.
// Target bytes are raw framebuffer RGBA; they are not straight-alpha colours.
static ColorRGBA BlendPixels(ColorRGBA src, ColorRGBA dst, int mode,
                             float opa, ColorRGBA uniformColor = {0, 0, 0, 0})
{
    if (mode == 6) return dst;
    float sa = src.a / 255.0f * std::max(0.0f, std::min(1.0f, opa));
    if (mode == 21)
    {
        sa *= uniformColor.a / 255.0f;
        src.r = uniformColor.r;
        src.g = uniformColor.g;
        src.b = uniformColor.b;
    }
    // Multiply-add modes deliberately ignore sa; even a transparent source
    // can alter their RGB. Other modes leave every destination byte unchanged.
    if (sa == 0.0f && mode != 1 && mode != 4) return dst;
    if (sa >= 1.0f)
    {
        // Fully opaque source: plain overwrite (matches SRC_ALPHA blending).
        ColorRGBA out = src;
        out.a = 255;
        return out;
    }
    ColorRGBA out;
    if (mode == 1 || mode == 4)
    {
        out.r = Clampf(src.r * dst.r / 255.0f + dst.r + 0.5f);
        out.g = Clampf(src.g * dst.g / 255.0f + dst.g + 0.5f);
        out.b = Clampf(src.b * dst.b / 255.0f + dst.b + 0.5f);
        out.a = dst.a;
    }
    else
    {
        out.r = Clampf(src.r * sa + dst.r * (1 - sa) + 0.5f);
        out.g = Clampf(src.g * sa + dst.g * (1 - sa) + 0.5f);
        out.b = Clampf(src.b * sa + dst.b * (1 - sa) + 0.5f);
        out.a = Clampf((mode == 21 ? sa * sa * 255 + dst.a * (1 - sa)
                                  : std::max(sa * 255, float(dst.a))) + 0.5f);
    }
    return out;
}

static uint8_t ToByte(float value)
{
    value = std::max(0.0f, std::min(1.0f, value));
    return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

// The SDL target contains premultiplied RGB after source-over blending.  This
// mode composites such a target without multiplying its RGB by alpha twice.
static SDL_BlendMode PremultipliedSourceOver()
{
    static const SDL_BlendMode mode = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD);
    return mode;
}

// Draw the mask over a premultiplied scratch target.  Source RGB is ignored;
// destination RGBA is multiplied by the mask's alpha.
static SDL_BlendMode MultiplyDestinationBySourceAlpha()
{
    static const SDL_BlendMode mode = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD);
    return mode;
}

// E-mote modes 1 and 4 use destination-colour multiplication followed by an
// additive destination term.  Keep destination alpha untouched.
static SDL_BlendMode EmoteMultiplyAdd()
{
    static const SDL_BlendMode mode = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_DST_COLOR, SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);
    return mode;
}
} // namespace

bool EmoteSWRenderBackend::EnsureGpuRenderer()
{
    if (gpuDisabled_)
        return false;

    SDL_Renderer* renderer = TVPGetPrimarySDLRenderer();
    if (!renderer)
        return false;
    if (renderer == gpuRenderer_)
        return true;

    // A recreated SDL window owns a new renderer.  Handles from the old one
    // are no longer safe to destroy or reuse; abandon them and recreate lazily.
    if (gpuRenderer_)
        ReleaseGpuResources(false);

    SDL_RendererInfo info = {};
    if (SDL_GetRendererInfo(renderer, &info) != 0 ||
        (info.flags & SDL_RENDERER_TARGETTEXTURE) == 0)
    {
        if (!gpuFailureReported_)
        {
            KRKRNS_LOG("[emote] SDL renderer cannot provide target textures: %s",
                       SDL_GetError());
            gpuFailureReported_ = true;
        }
        gpuDisabled_ = true;
        return false;
    }

    gpuRenderer_ = renderer;
    if (!gpuReported_)
    {
        KRKRNS_LOG("[emote] GPU mesh renderer ready backend=%s flags=0x%x",
                   info.name ? info.name : "unknown", info.flags);
        gpuReported_ = true;
    }
    return true;
}

bool EmoteSWRenderBackend::EnsureTargetGpu(Target* target)
{
    if (!target || !EnsureGpuRenderer())
        return false;
    if (target->gpuTexture)
        return true;

    target->gpuTexture = SDL_CreateTexture(gpuRenderer_, SDL_PIXELFORMAT_RGBA32,
                                            SDL_TEXTUREACCESS_TARGET,
                                            target->width, target->height);
    if (!target->gpuTexture)
    {
        if (!gpuFailureReported_)
        {
            KRKRNS_LOG("[emote] GPU target creation failed %dx%d: %s",
                       target->width, target->height, SDL_GetError());
            gpuFailureReported_ = true;
        }
        return false;
    }
    SDL_SetTextureScaleMode(target->gpuTexture, SDL_ScaleModeLinear);
    SDL_SetTextureBlendMode(target->gpuTexture, SDL_BLENDMODE_BLEND);
    return true;
}

bool EmoteSWRenderBackend::EnsureTextureGpu(Texture* texture)
{
    if (!texture || !EnsureGpuRenderer())
        return false;
    if (texture->gpuTexture)
        return true;

    texture->gpuTexture = SDL_CreateTexture(gpuRenderer_, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_STATIC,
                                             texture->width, texture->height);
    if (!texture->gpuTexture)
    {
        if (!gpuFailureReported_)
        {
            KRKRNS_LOG("[emote] GPU texture creation failed %dx%d: %s",
                       texture->width, texture->height, SDL_GetError());
            gpuFailureReported_ = true;
        }
        return false;
    }
    SDL_SetTextureScaleMode(texture->gpuTexture, SDL_ScaleModeLinear);
    SDL_SetTextureBlendMode(texture->gpuTexture, SDL_BLENDMODE_BLEND);
    if (!texture->pixels.empty())
    {
        SDL_UpdateTexture(texture->gpuTexture, nullptr, texture->pixels.data(),
                          texture->width * 4);
    }
    return true;
}

SDL_Texture* EmoteSWRenderBackend::EnsureAlphaTextureGpu(Texture* texture)
{
    if (!EnsureTextureGpu(texture))
        return nullptr;
    if (!texture->gpuAlphaTexture)
    {
        texture->gpuAlphaTexture = SDL_CreateTexture(
            gpuRenderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
            texture->width, texture->height);
        if (!texture->gpuAlphaTexture)
            return nullptr;
        SDL_SetTextureScaleMode(texture->gpuAlphaTexture, SDL_ScaleModeLinear);
        texture->gpuAlphaDirty = true;
    }
    if (texture->gpuAlphaDirty)
    {
        gpuAlphaPixels_.resize(texture->pixels.size());
        for (size_t i = 0; i + 3 < texture->pixels.size(); i += 4)
        {
            gpuAlphaPixels_[i + 0] = 255;
            gpuAlphaPixels_[i + 1] = 255;
            gpuAlphaPixels_[i + 2] = 255;
            gpuAlphaPixels_[i + 3] = texture->pixels[i + 3];
        }
        if (SDL_UpdateTexture(texture->gpuAlphaTexture, nullptr,
                              gpuAlphaPixels_.data(), texture->width * 4) != 0)
            return nullptr;
        texture->gpuAlphaDirty = false;
    }
    return texture->gpuAlphaTexture;
}

bool EmoteSWRenderBackend::BeginGpuSession()
{
    if (gpuSessionActive_)
        return true;
    if (!EnsureGpuRenderer())
        return false;

    savedRendererState_ = SavedRendererState{};
    savedRendererState_.target = SDL_GetRenderTarget(gpuRenderer_);
    SDL_RenderGetLogicalSize(gpuRenderer_, &savedRendererState_.logicalWidth,
                            &savedRendererState_.logicalHeight);
    savedRendererState_.integerScale =
        SDL_RenderGetIntegerScale(gpuRenderer_) == SDL_TRUE;
    SDL_RenderGetScale(gpuRenderer_, &savedRendererState_.scaleX,
                       &savedRendererState_.scaleY);
    SDL_Rect viewport = {};
    SDL_RenderGetViewport(gpuRenderer_, &viewport);
    savedRendererState_.viewportX = viewport.x;
    savedRendererState_.viewportY = viewport.y;
    savedRendererState_.viewportWidth = viewport.w;
    savedRendererState_.viewportHeight = viewport.h;
    savedRendererState_.clipEnabled =
        SDL_RenderIsClipEnabled(gpuRenderer_) == SDL_TRUE;
    SDL_Rect clip = {};
    SDL_RenderGetClipRect(gpuRenderer_, &clip);
    savedRendererState_.clipX = clip.x;
    savedRendererState_.clipY = clip.y;
    savedRendererState_.clipWidth = clip.w;
    savedRendererState_.clipHeight = clip.h;
    SDL_GetRenderDrawColor(gpuRenderer_, &savedRendererState_.drawR,
                           &savedRendererState_.drawG,
                           &savedRendererState_.drawB,
                           &savedRendererState_.drawA);
    SDL_BlendMode drawBlend = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(gpuRenderer_, &drawBlend);
    savedRendererState_.drawBlendMode = static_cast<int>(drawBlend);
    savedRendererState_.valid = true;
    gpuSessionActive_ = true;
    return true;
}

bool EmoteSWRenderBackend::BindGpuTarget(SDL_Texture* texture, int width, int height)
{
    if (!texture || width <= 0 || height <= 0 || !BeginGpuSession())
        return false;
    if (SDL_SetRenderTarget(gpuRenderer_, texture) != 0)
        return false;
    SDL_RenderSetLogicalSize(gpuRenderer_, 0, 0);
    SDL_RenderSetIntegerScale(gpuRenderer_, SDL_FALSE);
    SDL_RenderSetViewport(gpuRenderer_, nullptr);
    SDL_RenderSetScale(gpuRenderer_, 1.0f, 1.0f);
    SDL_RenderSetClipRect(gpuRenderer_, nullptr);
    return true;
}

void EmoteSWRenderBackend::EndGpuSession()
{
    if (!gpuSessionActive_ || !gpuRenderer_)
        return;

    if (savedRendererState_.valid)
    {
        SDL_SetRenderTarget(gpuRenderer_, savedRendererState_.target);
        SDL_RenderSetLogicalSize(gpuRenderer_, savedRendererState_.logicalWidth,
                                 savedRendererState_.logicalHeight);
        SDL_RenderSetIntegerScale(gpuRenderer_,
            savedRendererState_.integerScale ? SDL_TRUE : SDL_FALSE);
        SDL_RenderSetScale(gpuRenderer_, savedRendererState_.scaleX,
                           savedRendererState_.scaleY);
        SDL_Rect viewport = {savedRendererState_.viewportX,
                             savedRendererState_.viewportY,
                             savedRendererState_.viewportWidth,
                             savedRendererState_.viewportHeight};
        SDL_RenderSetViewport(gpuRenderer_, &viewport);
        if (savedRendererState_.clipEnabled)
        {
            SDL_Rect clip = {savedRendererState_.clipX,
                             savedRendererState_.clipY,
                             savedRendererState_.clipWidth,
                             savedRendererState_.clipHeight};
            SDL_RenderSetClipRect(gpuRenderer_, &clip);
        }
        else
        {
            SDL_RenderSetClipRect(gpuRenderer_, nullptr);
        }
        SDL_SetRenderDrawColor(gpuRenderer_, savedRendererState_.drawR,
                               savedRendererState_.drawG,
                               savedRendererState_.drawB,
                               savedRendererState_.drawA);
        SDL_SetRenderDrawBlendMode(
            gpuRenderer_, static_cast<SDL_BlendMode>(savedRendererState_.drawBlendMode));
    }
    savedRendererState_.valid = false;
    gpuSessionActive_ = false;
}

bool EmoteSWRenderBackend::EnsureScratchGpu(int width, int height)
{
    if (!EnsureGpuRenderer())
        return false;
    if (gpuScratch_ && gpuScratchWidth_ == width && gpuScratchHeight_ == height)
        return true;
    if (gpuScratch_)
        SDL_DestroyTexture(gpuScratch_);
    gpuScratch_ = SDL_CreateTexture(gpuRenderer_, SDL_PIXELFORMAT_RGBA32,
                                    SDL_TEXTUREACCESS_TARGET, width, height);
    if (!gpuScratch_)
        return false;
    gpuScratchWidth_ = width;
    gpuScratchHeight_ = height;
    SDL_SetTextureScaleMode(gpuScratch_, SDL_ScaleModeLinear);
    return true;
}

void EmoteSWRenderBackend::ReleaseGpuResources(bool rendererIsAlive)
{
    if (rendererIsAlive)
        EndGpuSession();
    else
    {
        gpuSessionActive_ = false;
        savedRendererState_.valid = false;
    }

    for (Target* target : targets_)
    {
        if (rendererIsAlive && target->gpuTexture)
            SDL_DestroyTexture(target->gpuTexture);
        target->gpuTexture = nullptr;
    }
    for (Texture* texture : textures_)
    {
        if (rendererIsAlive && texture->gpuTexture)
            SDL_DestroyTexture(texture->gpuTexture);
        if (rendererIsAlive && texture->gpuAlphaTexture)
            SDL_DestroyTexture(texture->gpuAlphaTexture);
        texture->gpuTexture = nullptr;
        texture->gpuAlphaTexture = nullptr;
        texture->gpuAlphaDirty = true;
    }
    if (rendererIsAlive && gpuScratch_)
        SDL_DestroyTexture(gpuScratch_);
    gpuScratch_ = nullptr;
    gpuScratchWidth_ = 0;
    gpuScratchHeight_ = 0;
    gpuRenderer_ = nullptr;
}

EmoteSWRenderBackend::~EmoteSWRenderBackend()
{
    const bool rendererIsAlive = gpuRenderer_ &&
        SDL_WasInit(SDL_INIT_VIDEO) != 0 &&
        TVPGetPrimarySDLRenderer() == gpuRenderer_;
    ReleaseGpuResources(rendererIsAlive);
    for (Target* t : targets_)
        delete t;
    targets_.clear();
    for (Texture* t : textures_)
        delete t;
    textures_.clear();
}

// KRKR-ns: drop every GPU handle when a game session ends and the engine is
// rebuilt in-process.
//
// EnsureGpuRenderer() re-creates the cached handles when the SDL renderer
// POINTER changes, but that test cannot detect the dangerous case: the engine
// restart destroys the SDL window and renderer, and the allocator routinely
// hands the replacement the SAME address.  `renderer == gpuRenderer_` then
// compares equal, the stale target/texture handles are reused, and the new
// engine composites into textures owned by a renderer that no longer exists.
// That shows up as a title screen whose UI draws while the background and
// E-mote logo animation stay empty, and as a freeze on a later restart.
//
// The renderer is already gone at this point, so pass rendererIsAlive=false:
// the old handles must be abandoned, not destroyed.
void EmoteSWRenderBackend::ResetForEngineRestart()
{
    if (!gpuRenderer_ && !gpuScratch_)
        return;
    KRKRNS_LOG("[emote] CPU backend: releasing %d target(s) and %d texture(s) "
               "for engine restart", (int)targets_.size(), (int)textures_.size());
    ReleaseGpuResources(false);
}

EmoteSWRenderBackend::Target* EmoteSWRenderBackend::FindTarget(void* handle) const
{
    for (Target* t : targets_)
    {
        if (t == handle)
            return t;
    }
    return nullptr;
}

EmoteSWRenderBackend::Texture* EmoteSWRenderBackend::FindTexture(void* handle) const
{
    for (Texture* t : textures_)
    {
        if (t == handle)
            return t;
    }
    return nullptr;
}

void* EmoteSWRenderBackend::CreateTarget(int width, int height)
{
    if (width <= 0 || height <= 0)
        return nullptr;
    KRKRNS_LOG("[emote] CreateTarget begin %dx%d targets=%zu", width, height, targets_.size());
    Target* target = new Target();
    target->width = width;
    target->height = height;
    target->pixels.resize((size_t)width * height * 4, 0);
    targets_.push_back(target);
    const bool gpuReady = EnsureTargetGpu(target);
    KRKRNS_LOG("[emote] CreateTarget ready %p bytes=%zu gpu=%d", target,
               target->pixels.size(), gpuReady ? 1 : 0);
    return target;
}

void EmoteSWRenderBackend::DestroyTarget(void* handle)
{
    Target* target = FindTarget(handle);
    if (!target)
        return;
    if (gpuSessionActive_)
        EndGpuSession();
    if (currentTarget_ == target)
        currentTarget_ = nullptr;
    if (maskTarget_ == target)
        maskTarget_ = nullptr;
    for (size_t i = 0; i < targets_.size(); i++)
    {
        if (targets_[i] == target)
        {
            targets_.erase(targets_.begin() + i);
            break;
        }
    }
    if (target->gpuTexture && gpuRenderer_ && SDL_WasInit(SDL_INIT_VIDEO) != 0)
        SDL_DestroyTexture(target->gpuTexture);
    target->gpuTexture = nullptr;
    delete target;
}

void EmoteSWRenderBackend::SetTarget(void* handle)
{
    currentTarget_ = FindTarget(handle);
    if (currentTarget_ && EnsureTargetGpu(currentTarget_))
        BindGpuTarget(currentTarget_->gpuTexture, currentTarget_->width, currentTarget_->height);
}

void EmoteSWRenderBackend::ClearTarget(bool clearColor)
{
    // There is no depth buffer; clearColor=false intentionally preserves the
    // previous frame so script-controlled multi-player composition still works.
    if (!currentTarget_ || !clearColor)
        return;
    if (currentTarget_->gpuTexture &&
        BindGpuTarget(currentTarget_->gpuTexture, currentTarget_->width, currentTarget_->height))
    {
        SDL_SetRenderDrawBlendMode(gpuRenderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(gpuRenderer_, 0, 0, 0, 0);
        if (SDL_RenderClear(gpuRenderer_) == 0)
            return;
    }
    std::memset(currentTarget_->pixels.data(), 0, currentTarget_->pixels.size());
}

uint8_t* EmoteSWRenderBackend::LockTarget(void* handle, int& pitch)
{
    Target* target = FindTarget(handle);
    if (!target)
        return nullptr;
    pitch = target->width * 4;
    if (EnsureTargetGpu(target) &&
        BindGpuTarget(target->gpuTexture, target->width, target->height))
    {
        const uint64_t readbackStart = SDL_GetPerformanceCounter();
        const int readbackResult = SDL_RenderReadPixels(
            gpuRenderer_, nullptr, SDL_PIXELFORMAT_RGBA32,
            target->pixels.data(), pitch);
        const uint64_t unpremultiplyStart = SDL_GetPerformanceCounter();
        profileReadbackTicks_ += unpremultiplyStart - readbackStart;
        if (readbackResult == 0)
        {
            // SDL's conventional source-over blend leaves premultiplied RGB in
            // a transparent target.  Kirikiri Layer pixels are straight RGBA,
            // so undo that representation at the single readback boundary.
            for (size_t i = 0; i + 3 < target->pixels.size(); i += 4)
            {
                const unsigned alpha = target->pixels[i + 3];
                if (alpha == 0)
                {
                    target->pixels[i + 0] = 0;
                    target->pixels[i + 1] = 0;
                    target->pixels[i + 2] = 0;
                }
                else if (alpha < 255)
                {
                    for (int channel = 0; channel < 3; ++channel)
                    {
                        const unsigned value = target->pixels[i + channel];
                        target->pixels[i + channel] = static_cast<uint8_t>(
                            std::min(255u, (value * 255u + alpha / 2u) / alpha));
                    }
                }
            }
            profileUnpremultiplyTicks_ +=
                SDL_GetPerformanceCounter() - unpremultiplyStart;
            ++profileFrames_;
            if (profileFrames_ >= 30)
            {
                const double frequency =
                    static_cast<double>(SDL_GetPerformanceFrequency());
                const double readbackMs = profileReadbackTicks_ * 1000.0 /
                    frequency / profileFrames_;
                const double unpremultiplyMs = profileUnpremultiplyTicks_ * 1000.0 /
                    frequency / profileFrames_;
                const double drawMs = profileDrawTicks_ * 1000.0 /
                    frequency / profileFrames_;
                KRKRNS_LOG("[emote] GPU profile frames=%u draw=%.2fms readback=%.2fms unpremul=%.2fms calls=%.1f masked=%.1f",
                           profileFrames_, drawMs, readbackMs, unpremultiplyMs,
                           static_cast<double>(profileDrawCalls_) / profileFrames_,
                           static_cast<double>(profileMaskedDrawCalls_) / profileFrames_);
                profileDrawTicks_ = 0;
                profileReadbackTicks_ = 0;
                profileUnpremultiplyTicks_ = 0;
                profileDrawCalls_ = 0;
                profileMaskedDrawCalls_ = 0;
                profileFrames_ = 0;
            }
        }
        else if (!gpuFailureReported_)
        {
            KRKRNS_LOG("[emote] GPU target readback failed: %s", SDL_GetError());
            gpuFailureReported_ = true;
        }
    }
    return target->pixels.data();
}

void EmoteSWRenderBackend::UnlockTarget(void* handle)
{
    (void)handle;
    EndGpuSession();
}

void* EmoteSWRenderBackend::GetTargetTexture(void* handle)
{
    // The public handle remains the Target object; DrawMesh resolves its GPU
    // texture internally and the CPU fallback uses the same pixel vector.
    return FindTarget(handle) ? handle : nullptr;
}

void EmoteSWRenderBackend::UpdateTargetTexture(void* handle,
                                          const uint8_t* pixels,
                                          int width,
                                          int height,
                                          int pitch)
{
    Target* target = FindTarget(handle);
    if (!target || !pixels || width <= 0 || height <= 0 ||
        width > target->width || height > target->height ||
        (std::ptrdiff_t(pitch) < std::ptrdiff_t(width) * 4 &&
         std::ptrdiff_t(pitch) > -std::ptrdiff_t(width) * 4))
        throw std::runtime_error("Invalid E-mote target update dimensions/pitch");
    for (int y = 0; y < height; y++)
    {
        std::memcpy(target->pixels.data() + (size_t)y * target->width * 4,
                    pixels + std::ptrdiff_t(y) * pitch, (size_t)width * 4);
    }
    if (EnsureTargetGpu(target))
    {
        // SDL permits updating many target textures directly.  This interface
        // is not used by the current E-mote path; retain the CPU copy if a
        // platform renderer rejects it.
        SDL_UpdateTexture(target->gpuTexture, nullptr, target->pixels.data(),
                          target->width * 4);
    }
}

//---------------------------------------------------------------------------
// 一般贴图：内部持有 CPU 像素副本
//---------------------------------------------------------------------------
void* EmoteSWRenderBackend::CreateTexture(int width, int height)
{
    if (width <= 0 || height <= 0)
        return nullptr;
    KRKRNS_LOG("[emote] CreateTexture begin %dx%d textures=%zu", width, height, textures_.size());
    Texture* texture = new Texture();
    texture->width = width;
    texture->height = height;
    texture->pixels.resize((size_t)width * height * 4, 0);
    textures_.push_back(texture);
    const bool gpuReady = EnsureTextureGpu(texture);
    KRKRNS_LOG("[emote] CreateTexture ready %p bytes=%zu gpu=%d", texture,
               texture->pixels.size(), gpuReady ? 1 : 0);
    return texture;
}

void EmoteSWRenderBackend::UpdateTexture(void* handle, const uint8_t* pixels, int width, int height, int pitch)
{
    Texture* texture = FindTexture(handle);
    if (!texture || !pixels || width <= 0 || height <= 0 ||
        width > texture->width || height > texture->height ||
        (std::ptrdiff_t(pitch) < std::ptrdiff_t(width) * 4 &&
         std::ptrdiff_t(pitch) > -std::ptrdiff_t(width) * 4))
        throw std::runtime_error("Invalid E-mote texture update dimensions/pitch");
    // 按行拷贝（源 pitch 可能含对齐填充）
    for (int y = 0; y < height; y++)
    {
        std::memcpy(texture->pixels.data() + (size_t)y * texture->width * 4,
                    pixels + std::ptrdiff_t(y) * pitch, (size_t)width * 4);
    }
    // Inspect the actual complete image, including any untouched part of a
    // partial update. A retained LockTexture pointer can mutate pixels later;
    // once exposed, never cache an opacity assertion for this texture again.
    texture->opaquePixels = !texture->pixelAccessExposed;
    if (texture->opaquePixels)
        for (size_t i = 3; i < texture->pixels.size(); i += 4)
            if (texture->pixels[i] != 255)
            {
                texture->opaquePixels = false;
                break;
            }
    texture->gpuAlphaDirty = true;
    if (EnsureTextureGpu(texture))
    {
        SDL_UpdateTexture(texture->gpuTexture, nullptr, texture->pixels.data(),
                          texture->width * 4);
    }
}

uint8_t* EmoteSWRenderBackend::LockTexture(void* handle, int& pitch)
{
    Texture* texture = FindTexture(handle);
    if (!texture)
        return nullptr;
    texture->pixelAccessExposed = true;
    texture->opaquePixels = false;
    pitch = texture->width * 4;
    return texture->pixels.data();
}

void EmoteSWRenderBackend::DestroyTexture(void* handle)
{
    Texture* texture = FindTexture(handle);
    if (!texture)
        return;
    for (size_t i = 0; i < textures_.size(); i++)
    {
        if (textures_[i] == texture)
        {
            textures_.erase(textures_.begin() + i);
            break;
        }
    }
    if (gpuRenderer_ && SDL_WasInit(SDL_INIT_VIDEO) != 0)
    {
        if (texture->gpuTexture)
            SDL_DestroyTexture(texture->gpuTexture);
        if (texture->gpuAlphaTexture)
            SDL_DestroyTexture(texture->gpuAlphaTexture);
    }
    texture->gpuTexture = nullptr;
    texture->gpuAlphaTexture = nullptr;
    delete texture;
}

//---------------------------------------------------------------------------
// 绘制状态
//---------------------------------------------------------------------------
void EmoteSWRenderBackend::SetMask(void* handle)
{
    maskTarget_ = FindTarget(handle);
}

void EmoteSWRenderBackend::SetBlendMode(int mode, const float* uniformColor)
{
    blendMode_ = mode;
    skipDraw_ = (mode == 6);
    if (mode == 21 && uniformColor)
    {
        uniformColor_[0] = uniformColor[0];
        uniformColor_[1] = uniformColor[1];
        uniformColor_[2] = uniformColor[2];
        uniformColor_[3] = uniformColor[3];
    }
}

bool EmoteSWRenderBackend::DrawMeshGpu(const float* vertices,
                                       int vertexCount,
                                       const uint16_t* indices,
                                       int indexCount,
                                       Texture* texture,
                                       Target* targetAsTexture,
                                       float opacity)
{
    if (!currentTarget_ || !EnsureTargetGpu(currentTarget_))
        return false;

    SDL_Texture* source = nullptr;
    if (texture)
    {
        if (!EnsureTextureGpu(texture))
            return false;
        source = blendMode_ == 21 ? EnsureAlphaTextureGpu(texture)
                                  : texture->gpuTexture;
    }
    else if (targetAsTexture && EnsureTargetGpu(targetAsTexture))
    {
        source = targetAsTexture->gpuTexture;
    }
    if (!source)
        return false;

    gpuPositions_.resize(static_cast<size_t>(vertexCount) * 2u);
    gpuColors_.resize(static_cast<size_t>(vertexCount) * 4u);
    const uint8_t colorR = blendMode_ == 21 ? ToByte(uniformColor_[0]) : 255;
    const uint8_t colorG = blendMode_ == 21 ? ToByte(uniformColor_[1]) : 255;
    const uint8_t colorB = blendMode_ == 21 ? ToByte(uniformColor_[2]) : 255;
    const float colorAlpha = opacity * (blendMode_ == 21 ? uniformColor_[3] : 1.0f);
    const uint8_t colorA = ToByte(colorAlpha);

    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();
    for (int i = 0; i < vertexCount; ++i)
    {
        const float* vertex = vertices + static_cast<size_t>(i) * 4u;
        const float x = (vertex[0] + 1.0f) * 0.5f * currentTarget_->width;
        const float y = (vertex[1] + 1.0f) * 0.5f * currentTarget_->height;
        gpuPositions_[static_cast<size_t>(i) * 2u + 0] = x;
        gpuPositions_[static_cast<size_t>(i) * 2u + 1] = y;
        gpuColors_[static_cast<size_t>(i) * 4u + 0] = colorR;
        gpuColors_[static_cast<size_t>(i) * 4u + 1] = colorG;
        gpuColors_[static_cast<size_t>(i) * 4u + 2] = colorB;
        gpuColors_[static_cast<size_t>(i) * 4u + 3] = colorA;
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
    }

    auto renderGeometry = [&]() {
        return SDL_RenderGeometryRaw(
            gpuRenderer_, source,
            gpuPositions_.data(), static_cast<int>(sizeof(float) * 2),
            reinterpret_cast<const SDL_Color*>(gpuColors_.data()), 4,
            vertices + 2, static_cast<int>(sizeof(float) * 4), vertexCount,
            indices, indexCount, 2);
    };

    if (!maskTarget_)
    {
        if (!BindGpuTarget(currentTarget_->gpuTexture,
                           currentTarget_->width, currentTarget_->height))
            return false;
        const SDL_BlendMode blend = (blendMode_ == 1 || blendMode_ == 4)
            ? EmoteMultiplyAdd()
            : SDL_BLENDMODE_BLEND;
        SDL_SetTextureBlendMode(source, blend);
        if (renderGeometry() == 0)
            return true;
    }
    else
    {
        if (!EnsureTargetGpu(maskTarget_) ||
            !EnsureScratchGpu(currentTarget_->width, currentTarget_->height))
            return false;

        SDL_Rect bounds = {};
        bounds.x = std::max(0, static_cast<int>(std::floor(minX)) - 1);
        bounds.y = std::max(0, static_cast<int>(std::floor(minY)) - 1);
        const int right = std::min(currentTarget_->width,
                                   static_cast<int>(std::ceil(maxX)) + 1);
        const int bottom = std::min(currentTarget_->height,
                                    static_cast<int>(std::ceil(maxY)) + 1);
        bounds.w = right - bounds.x;
        bounds.h = bottom - bounds.y;
        if (bounds.w <= 0 || bounds.h <= 0)
            return true;

        if (!BindGpuTarget(gpuScratch_, currentTarget_->width, currentTarget_->height))
            return false;
        SDL_SetRenderDrawBlendMode(gpuRenderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(gpuRenderer_, 0, 0, 0, 0);
        if (SDL_RenderFillRect(gpuRenderer_, &bounds) != 0)
            return false;

        // First create a premultiplied version of this mesh in scratch, then
        // multiply it by the independently rendered E-mote mask.
        SDL_SetTextureBlendMode(source, SDL_BLENDMODE_BLEND);
        if (renderGeometry() != 0)
            return false;
        SDL_SetTextureBlendMode(maskTarget_->gpuTexture,
                                MultiplyDestinationBySourceAlpha());
        if (SDL_RenderCopy(gpuRenderer_, maskTarget_->gpuTexture,
                           &bounds, &bounds) != 0)
            return false;

        if (!BindGpuTarget(currentTarget_->gpuTexture,
                           currentTarget_->width, currentTarget_->height))
            return false;
        SDL_SetTextureBlendMode(
            gpuScratch_, (blendMode_ == 1 || blendMode_ == 4)
                ? EmoteMultiplyAdd()
                : PremultipliedSourceOver());
        if (SDL_RenderCopy(gpuRenderer_, gpuScratch_, &bounds, &bounds) == 0)
            return true;
    }

    if (!gpuFailureReported_)
    {
        KRKRNS_LOG("[emote] SDL geometry rendering failed; using CPU fallback: %s",
                   SDL_GetError());
        gpuFailureReported_ = true;
    }
    EndGpuSession();
    gpuDisabled_ = true;
    return false;
}

void EmoteSWRenderBackend::DrawMesh(const float* vertices,
                                    int vertexCount,
                                    const uint16_t* indices,
                                    int indexCount,
                                    void* handle,
                                    float opacity)
{
    if (skipDraw_ || !currentTarget_ || !vertices || !indices || vertexCount <= 0 || indexCount <= 0)
        return;
    Texture* texture = FindTexture(handle);
    Target* targetAsTexture = texture ? nullptr : FindTarget(handle);
    if (!texture && !targetAsTexture)
        return;

    // Summarize invisible work before discarding it, so a static/hidden scene
    // can be distinguished from expensive visible meshes on the real runtime.
    const bool opacityNoOp = opacity <= 0.0f && blendMode_ != 1 && blendMode_ != 4;
    static unsigned opacityCalls = 0, opacityZero = 0, opacityFull = 0,
                    opacityMultiply = 0, opacitySkipped = 0;
    ++opacityCalls;
    opacityZero += opacity <= 0.0f;
    opacityFull += opacity >= 1.0f;
    opacityMultiply += blendMode_ == 1 || blendMode_ == 4;
    opacitySkipped += opacityNoOp;
    if (opacityCalls == 240)
    {
        KRKRNS_LOG("[emote] mesh opacity: calls=%u zero=%u full=%u multiply=%u skipped=%u",
                   opacityCalls, opacityZero, opacityFull, opacityMultiply, opacitySkipped);
        opacityCalls = opacityZero = opacityFull = opacityMultiply = opacitySkipped = 0;
    }
    // Stencil sources reach this API with opacity=1. Multiply-add modes retain
    // their opacity-independent RGB equation, including transparent inputs.
    if (opacityNoOp) return;

    const uint64_t drawStart = SDL_GetPerformanceCounter();
    if (DrawMeshGpu(vertices, vertexCount, indices, indexCount,
                    texture, targetAsTexture, opacity))
    {
        profileDrawTicks_ += SDL_GetPerformanceCounter() - drawStart;
        ++profileDrawCalls_;
        if (maskTarget_)
            ++profileMaskedDrawCalls_;
        return;
    }
    DrawMeshCpu(vertices, vertexCount, indices, indexCount,
                texture, targetAsTexture, opacity);
    profileDrawTicks_ += SDL_GetPerformanceCounter() - drawStart;
    ++profileDrawCalls_;
    static Uint32 swProfFrames = 0;
    swProfFrames++;
    if (swProfFrames % 240 == 0 && profileDrawCalls_ > 0)
    {
        const double freq = (double)SDL_GetPerformanceFrequency();
        KRKRNS_LOG("[emote] SW draw profile: avg=%.2fms/call calls=%u",
                   (profileDrawTicks_ * 1000.0 / freq) / profileDrawCalls_,
                   profileDrawCalls_);
    }
}


// KRKR-ns: row-band parallel rasterization. Rows are disjoint pixel sets, and
// each band walks the triangles in original order, so blending order stays
// deterministic while the pixel loop scales across cores.
namespace
{
struct SwBandJob
{
    const int32_t* textureXs = nullptr;
    bool sourceAliased = false;
    bool fullCoverage = false;
    bool splitUVs = false;
    float splitA = 0, splitB = 0, splitC = 0;
    bool splitInclusive = false;
    bool opaqueTexture = false;
    float secondAu = 0, secondBu = 0, secondCu = 0;
    float secondAv = 0, secondBv = 0, secondCv = 0;
    float sx;
    float a01, b01, c01, a12, b12, c12, a20, b20, c20;
    float A_u, B_u, C_u, A_v, B_v, C_v;
    bool ccw, include01, include12, include20;
    uint32_t* dst;
    const uint32_t* maskRowBase;
    bool hasStencil;
    int pitch;
    int width;
    int minX, maxX;
    int minY, rows;
    int texW, texH;
    int blendMode;
    float opacity;
    ColorRGBA uniformColor;
    const ColorRGBA* texData;
};
}

template<bool vectorTexelCoordinates, bool fullCoverage = false, bool splitUVs = false,
         bool rowTexels = false, bool cachedX = false>
static void SwRasterizeBandImpl(SwBandJob& j)
{
    const int texW_1 = j.texW - 1, texH_1 = j.texH - 1;
    const float sx = j.sx;
    for (int py = j.minY; py < j.minY + j.rows; py++)
    {
        const float fy = py + 0.5f;
        float f01_row = j.a01 * sx + j.b01 * fy + j.c01;
        float f12_row = j.a12 * sx + j.b12 * fy + j.c12;
        float f20_row = j.a20 * sx + j.b20 * fy + j.c20;
        float tu_row = j.A_u * sx + j.B_u * fy + j.C_u;
        float tv_row = j.A_v * sx + j.B_v * fy + j.C_v;
        float split_row = 0, secondU_row = 0, secondV_row = 0;
        if constexpr (splitUVs)
        {
            split_row = j.splitA * sx + j.splitB * fy + j.splitC;
            secondU_row = j.secondAu * sx + j.secondBu * fy + j.secondCu;
            secondV_row = j.secondAv * sx + j.secondBv * fy + j.secondCv;
        }

        ColorRGBA* row = (ColorRGBA*)(j.dst + (size_t)py * j.pitch);
        const uint32_t* maskRow = j.maskRowBase ? j.maskRowBase + (size_t)py * j.width : nullptr;

        int px = j.minX;
#if defined(__SWITCH__) || defined(KRKRNS_EMOTE_TEST_NEON)
        // ---- 4-pixel NEON fast path (general alpha blend only) -------------
        // The edge functions, UVs and the blend are all per-pixel float work;
        // on the emulated/CPU path this loop is the E-mote's dominant cost
        // (measured ~6 ms per draw call).  Vectorize 4 pixels at a time for
        // the common alpha mode; every float op keeps BlendPixels' exact
        // order so the output is bit-identical.  Other modes and the row
        // tail keep the scalar path below.
        const bool neonBlend = (j.blendMode != 6 && j.blendMode != 21 &&
                                j.blendMode != 1 && j.blendMode != 4);
        if (neonBlend)
        {
            const float32x4_t kLane = {0.f, 1.f, 2.f, 3.f};
            const float32x4_t vZero = vdupq_n_f32(0.f);
            const float32x4_t vOne = vdupq_n_f32(1.f);
            const float32x4_t vHalf = vdupq_n_f32(0.5f);
            const float32x4_t vInv255 = vdupq_n_f32(1.0f / 255.0f);
            const float32x4_t v255 = vdupq_n_f32(255.0f);
            const float32x4_t vOpa = vdupq_n_f32(
                std::max(0.0f, std::min(1.0f, j.opacity)));
            const float32x4_t vA01 = vdupq_n_f32(j.a01);
            const float32x4_t vA12 = vdupq_n_f32(j.a12);
            const float32x4_t vA20 = vdupq_n_f32(j.a20);
            const float32x4_t vAU = vdupq_n_f32(j.A_u);
            const float32x4_t vAV = vdupq_n_f32(j.A_v);
            const uint32x4_t mIncl01 = vdupq_n_u32(j.include01 ? 0xffffffffu : 0u);
            const uint32x4_t mIncl12 = vdupq_n_u32(j.include12 ? 0xffffffffu : 0u);
            const uint32x4_t mIncl20 = vdupq_n_u32(j.include20 ? 0xffffffffu : 0u);
            const uint32x4_t vByte = vdupq_n_u32(0xffu);
            const uint32x4_t vAlphaKeep = vdupq_n_u32(0xff000000u);
            const float32x4_t vTexWidth = vdupq_n_f32(static_cast<float>(texW_1));
            const float32x4_t vTexHeight = vdupq_n_f32(static_cast<float>(texH_1));
            const int32x4_t vIntZero = vdupq_n_s32(0);
            const int32x4_t vMaxTexX = vdupq_n_s32(texW_1);
            const int32x4_t vMaxTexY = vdupq_n_s32(texH_1);
            const ColorRGBA* textureRow = nullptr;
            if constexpr (rowTexels)
            {
                // A_v is exactly zero. Match lane zero of the original
                // FMLA, fused texel rounding and saturating conversion once
                // per row; covered groups retain their original U stepping.
                const float rowV = vgetq_lane_f32(
                    vmlaq_f32(vdupq_n_f32(tv_row), kLane, vAV), 0);
#if defined(__SWITCH__) || defined(KRKRNS_EMOTE_TEST_FMA)
                const float texY = std::fma(rowV, static_cast<float>(texH_1), 0.5f);
#else
                const float texY = rowV * texH_1 + 0.5f;
#endif
                const int ty = vgetq_lane_s32(vminq_s32(vmaxq_s32(
                    vcvtq_s32_f32(vdupq_n_f32(texY)), vIntZero), vMaxTexY), 0);
                textureRow = j.texData + static_cast<size_t>(ty) * j.texW;
            }
            for (; px + 4 <= j.maxX + 1; px += 4)
            {
                uint32x4_t inside = vdupq_n_u32(0xffffffffu);
                if constexpr (!fullCoverage)
                {
                    const float32x4_t f01 = vmlaq_f32(vdupq_n_f32(f01_row), kLane, vA01);
                    const float32x4_t f12 = vmlaq_f32(vdupq_n_f32(f12_row), kLane, vA12);
                    const float32x4_t f20 = vmlaq_f32(vdupq_n_f32(f20_row), kLane, vA20);
                    uint32x4_t m01, m12, m20;
                    if (j.ccw)
                    {
                        m01 = vorrq_u32(vcgtq_f32(f01, vZero),
                                        vandq_u32(vceqq_f32(f01, vZero), mIncl01));
                        m12 = vorrq_u32(vcgtq_f32(f12, vZero),
                                        vandq_u32(vceqq_f32(f12, vZero), mIncl12));
                        m20 = vorrq_u32(vcgtq_f32(f20, vZero),
                                        vandq_u32(vceqq_f32(f20, vZero), mIncl20));
                    }
                    else
                    {
                        m01 = vorrq_u32(vcltq_f32(f01, vZero),
                                        vandq_u32(vceqq_f32(f01, vZero), mIncl01));
                        m12 = vorrq_u32(vcltq_f32(f12, vZero),
                                        vandq_u32(vceqq_f32(f12, vZero), mIncl12));
                        m20 = vorrq_u32(vcltq_f32(f20, vZero),
                                        vandq_u32(vceqq_f32(f20, vZero), mIncl20));
                    }
                    inside = vandq_u32(vandq_u32(m01, m12), m20);
                }
                if (j.hasStencil)
                {
                    const uint32x4_t st = vld1q_u32(maskRow + px);
                    inside = vandq_u32(inside,
                        vcgeq_u32(vshrq_n_u32(st, 24), vdupq_n_u32(128u)));
                }
                if (vmaxvq_u32(inside) != 0)
                {
                    float32x4_t tu = vZero;
                    if constexpr (!cachedX)
                        tu = vmlaq_f32(vdupq_n_f32(tu_row), kLane, vAU);
                    float32x4_t tv = vZero;
                    if constexpr (!rowTexels)
                        tv = vmlaq_f32(vdupq_n_f32(tv_row), kLane, vAV);
                    if constexpr (splitUVs)
                    {
                        const float32x4_t edge = vmlaq_f32(vdupq_n_f32(split_row),
                                                         kLane, vdupq_n_f32(j.splitA));
                        const uint32x4_t primary = vorrq_u32(
                            j.ccw ? vcgtq_f32(edge, vZero) : vcltq_f32(edge, vZero),
                            vandq_u32(vceqq_f32(edge, vZero),
                                      vdupq_n_u32(j.splitInclusive ? 0xffffffffu : 0u)));
                        tu = vbslq_f32(primary, tu,
                            vmlaq_f32(vdupq_n_f32(secondU_row), kLane, vdupq_n_f32(j.secondAu)));
                        tv = vbslq_f32(primary, tv,
                            vmlaq_f32(vdupq_n_f32(secondV_row), kLane, vdupq_n_f32(j.secondAv)));
                    }
                    uint32_t insideLane[4];
                    vst1q_u32(insideLane, inside);
                    uint32_t texel[4] = {0u, 0u, 0u, 0u};
                    if constexpr (cachedX)
                    {
                        const int32_t* x = j.textureXs + px - j.minX;
                        if (vminvq_u32(inside) != 0 && x[1] == x[0] + 1 &&
                            x[2] == x[0] + 2 && x[3] == x[0] + 3)
                            vst1q_u32(texel, vld1q_u32(reinterpret_cast<const uint32_t*>(textureRow + x[0])));
                        else
                            for (int l = 0; l < 4; ++l)
                                if (insideLane[l])
                                    texel[l] = *reinterpret_cast<const uint32_t*>(textureRow + x[l]);
                    }
                    else if constexpr (vectorTexelCoordinates)
                    {
                        // Switch GCC contracts the former scalar multiply/add
                        // to FMADD; retain its half-texel boundary rounding.
#if defined(__SWITCH__)
                        const float32x4_t texX = vfmaq_f32(vHalf, tu, vTexWidth);
                        float32x4_t texY = vZero;
                        if constexpr (!rowTexels)
                            texY = vfmaq_f32(vHalf, tv, vTexHeight);
#elif defined(KRKRNS_EMOTE_TEST_FMA)
                        // SIMDe may emulate vfmaq with unfused multiply/add.
                        // This host reference forces the ARM fused rounding.
                        float fusedX[4], fusedY[4];
                        vst1q_f32(fusedX, tu);
                        vst1q_f32(fusedY, tv);
                        for (int lane = 0; lane < 4; ++lane)
                        {
                            fusedX[lane] = std::fma(fusedX[lane], vgetq_lane_f32(vTexWidth, 0), 0.5f);
                            if constexpr (!rowTexels)
                                fusedY[lane] = std::fma(fusedY[lane], vgetq_lane_f32(vTexHeight, 0), 0.5f);
                        }
                        const float32x4_t texX = vld1q_f32(fusedX);
                        const float32x4_t texY = vld1q_f32(fusedY);
#else
                        const float32x4_t texX = vaddq_f32(vmulq_f32(tu, vTexWidth), vHalf);
                        float32x4_t texY = vZero;
                        if constexpr (!rowTexels)
                            texY = vaddq_f32(vmulq_f32(tv, vTexHeight), vHalf);
#endif
                        int32_t txa[4], tya[4];
                        vst1q_s32(txa, vminq_s32(vmaxq_s32(vcvtq_s32_f32(texX), vIntZero), vMaxTexX));
                        if constexpr (rowTexels)
                        {
                            if (vminvq_u32(inside) != 0 && txa[1] == txa[0] + 1 &&
                                txa[2] == txa[0] + 2 && txa[3] == txa[0] + 3)
                                vst1q_u32(texel, vld1q_u32(reinterpret_cast<const uint32_t*>(textureRow + txa[0])));
                            else
                                for (int l = 0; l < 4; ++l)
                                    if (insideLane[l])
                                        texel[l] = *reinterpret_cast<const uint32_t*>(textureRow + txa[l]);
                        }
                        else
                        {
                            vst1q_s32(tya, vminq_s32(vmaxq_s32(vcvtq_s32_f32(texY), vIntZero), vMaxTexY));
                            for (int l = 0; l < 4; ++l)
                            {
                                if (!insideLane[l]) continue;
                                texel[l] = *reinterpret_cast<const uint32_t*>(
                                    &j.texData[(size_t)tya[l] * j.texW + txa[l]]);
                            }
                        }
                    }
                    else
                    {
                        float txa[4], tya[4];
                        vst1q_f32(txa, tu);
                        vst1q_f32(tya, tv);
                        for (int l = 0; l < 4; ++l)
                        {
                            if (!insideLane[l])
                                continue;
#if defined(KRKRNS_EMOTE_TEST_FMA) && !defined(__SWITCH__)
                            int tx = (int)std::fma(txa[l], static_cast<float>(texW_1), 0.5f);
                            int ty = (int)std::fma(tya[l], static_cast<float>(texH_1), 0.5f);
#else
                            int tx = (int)(txa[l] * texW_1 + 0.5f);
                            int ty = (int)(tya[l] * texH_1 + 0.5f);
#endif
                            if (tx < 0)
                                tx = 0;
                            else if (tx > texW_1)
                                tx = texW_1;
                            if (ty < 0)
                                ty = 0;
                            else if (ty > texH_1)
                                ty = texH_1;
                            texel[l] = *reinterpret_cast<const uint32_t*>(
                                &j.texData[(size_t)ty * j.texW + tx]);
                        }
                    }
                    const uint32x4_t srcv = vld1q_u32(texel);
                    uint32_t* dstRow = reinterpret_cast<uint32_t*>(row) + px;
                    const uint32x4_t sourceAlpha = vshrq_n_u32(srcv, 24);
                    if (vmaxvq_u32(vandq_u32(inside, sourceAlpha)) == 0)
                    {
                        // Transparent lanes do not touch the destination.
                    }
                    else if (j.opacity >= 1.0f &&
                             vmaxvq_u32(vandq_u32(inside,
                                 vmvnq_u32(vceqq_u32(sourceAlpha, vdupq_n_u32(255u))))) == 0)
                    {
                        // Fully opaque covered lanes only copy their source;
                        // keep pixels outside the triangle/stencil unchanged.
                        const uint32x4_t dstv = vld1q_u32(dstRow);
                        vst1q_u32(dstRow, vbslq_u32(inside, srcv, dstv));
                    }
                    else
                    {
                        const uint32x4_t dstv = vld1q_u32(dstRow);
                        // sa = (src.a / 255) * opa  (same order as BlendPixels)
                        const float32x4_t sa = vmulq_f32(
                            vmulq_f32(vcvtq_f32_u32(vshrq_n_u32(srcv, 24)), vInv255), vOpa);
                        const float32x4_t inv = vsubq_f32(vOne, sa);
                        const float32x4_t sr = vcvtq_f32_u32(vandq_u32(srcv, vByte));
                        const float32x4_t sg = vcvtq_f32_u32(
                            vandq_u32(vshrq_n_u32(srcv, 8), vByte));
                        const float32x4_t sb = vcvtq_f32_u32(
                            vandq_u32(vshrq_n_u32(srcv, 16), vByte));
                        const float32x4_t dr = vcvtq_f32_u32(vandq_u32(dstv, vByte));
                        const float32x4_t dg = vcvtq_f32_u32(
                            vandq_u32(vshrq_n_u32(dstv, 8), vByte));
                        const float32x4_t db = vcvtq_f32_u32(
                            vandq_u32(vshrq_n_u32(dstv, 16), vByte));
                        const float32x4_t da = vcvtq_f32_u32(vshrq_n_u32(dstv, 24));
                        float32x4_t oR = vaddq_f32(vaddq_f32(vmulq_f32(sr, sa),
                                                             vmulq_f32(dr, inv)), vHalf);
                        float32x4_t oG = vaddq_f32(vaddq_f32(vmulq_f32(sg, sa),
                                                             vmulq_f32(dg, inv)), vHalf);
                        float32x4_t oB = vaddq_f32(vaddq_f32(vmulq_f32(sb, sa),
                                                             vmulq_f32(db, inv)), vHalf);
                        float32x4_t oA = vaddq_f32(vmaxq_f32(vmulq_f32(sa, v255), da), vHalf);
                        oR = vminq_f32(vmaxq_f32(oR, vZero), v255);
                        oG = vminq_f32(vmaxq_f32(oG, vZero), v255);
                        oB = vminq_f32(vmaxq_f32(oB, vZero), v255);
                        oA = vminq_f32(vmaxq_f32(oA, vZero), v255);
                        uint32x4_t outv = vorrq_u32(
                            vorrq_u32(vcvtq_u32_f32(oR), vshlq_n_u32(vcvtq_u32_f32(oG), 8)),
                            vorrq_u32(vshlq_n_u32(vcvtq_u32_f32(oB), 16),
                                      vshlq_n_u32(vcvtq_u32_f32(oA), 24)));
                        // fully-opaque source: plain overwrite with a = 255
                        outv = vbslq_u32(vcgeq_f32(sa, vOne),
                                         vorrq_u32(srcv, vAlphaKeep), outv);
                        outv = vbslq_u32(inside, outv, dstv);
                        vst1q_u32(dstRow, outv);
                    }
                }
                f01_row += j.a01 * 4.0f;
                f12_row += j.a12 * 4.0f;
                f20_row += j.a20 * 4.0f;
                tu_row += j.A_u * 4.0f;
                tv_row += j.A_v * 4.0f;
                if constexpr (splitUVs)
                {
                    split_row += j.splitA * 4.0f;
                    secondU_row += j.secondAu * 4.0f;
                    secondV_row += j.secondAv * 4.0f;
                }
            }
        }
#endif
        for (; px <= j.maxX; px++)
        {
            const float e01 = j.ccw ? f01_row : -f01_row;
            const float e12 = j.ccw ? f12_row : -f12_row;
            const float e20 = j.ccw ? f20_row : -f20_row;
            const bool inside = fullCoverage ||
                               ((e01 > 0 || (e01 == 0 && j.include01)) &&
                                (e12 > 0 || (e12 == 0 && j.include12)) &&
                                (e20 > 0 || (e20 == 0 && j.include20)));
            if (inside)
            {
                float tu = tu_row, tv = tv_row;
                if constexpr (splitUVs)
                {
                    const float edge = j.ccw ? split_row : -split_row;
                    if (!(edge > 0 || (edge == 0 && j.splitInclusive)))
                    {
                        tu = secondU_row;
                        tv = secondV_row;
                    }
                }
                int tx = (int)(tu * texW_1 + 0.5f);
                int ty = (int)(tv * texH_1 + 0.5f);
                if (tx < 0)
                    tx = 0;
                else if (tx > texW_1)
                    tx = texW_1;
                if (ty < 0)
                    ty = 0;
                else if (ty > texH_1)
                    ty = texH_1;

                if (!j.hasStencil || ((maskRow[px] >> 24) & 0xFF) >= 128)
                {
                    row[px] = BlendPixels(j.texData[(size_t)ty * j.texW + tx], row[px],
                                          j.blendMode, j.opacity, j.uniformColor);
                }
            }

            f01_row += j.a01;
            f12_row += j.a12;
            f20_row += j.a20;
            tu_row += j.A_u;
            tv_row += j.A_v;
            if constexpr (splitUVs)
            {
                split_row += j.splitA;
                secondU_row += j.secondAu;
                secondV_row += j.secondAv;
            }
        }
    }
}

#if defined(__SWITCH__) || defined(KRKRNS_EMOTE_TEST_NEON)
static bool SwCanDrawRectangleRows(const SwBandJob& j)
{
    return j.fullCoverage && !j.hasStencil &&
        j.blendMode != 1 && j.blendMode != 4 && j.blendMode != 6 && j.blendMode != 21 &&
        j.maxX - j.minX + 1 >= 64 && j.A_v == 0.f &&
        (!j.splitUVs || j.secondAv == 0.f);
}

static bool SwBuildTextureXs(SwBandJob& j, std::vector<int32_t>& coordinates)
{
    const int width = j.maxX - j.minX + 1;
    if (j.sourceAliased || j.hasStencil || j.B_u != 0.f || j.A_v != 0.f ||
        j.blendMode == 1 || j.blendMode == 4 || j.blendMode == 6 || j.blendMode == 21 ||
        width < 64 || j.rows < 96 || (uint64_t)width * j.rows < 66 * 500 ||
        (j.splitUVs && (j.secondBu != 0.f || j.secondAv != 0.f)))
        return false;

    // Reuse the original four-lane recurrence, not a newly calculated affine
    // mapping. With B_u exactly zero these texel X values are row-invariant.
    // A split rectangle is eligible only if both original UV maps quantize
    // to the same X at every vector lane; its per-row Y check stays in place.
    const float32x4_t lanes = {0.f, 1.f, 2.f, 3.f};
    const float32x4_t half = vdupq_n_f32(0.5f);
    const float32x4_t texWidth = vdupq_n_f32(float(j.texW - 1));
    const int32x4_t zero = vdupq_n_s32(0), maxX = vdupq_n_s32(j.texW - 1);
    const auto quantize = [&](float u, float step) {
        const float32x4_t values = vmlaq_f32(vdupq_n_f32(u), lanes, vdupq_n_f32(step));
#if defined(__SWITCH__)
        const float32x4_t x = vfmaq_f32(half, values, texWidth);
#elif defined(KRKRNS_EMOTE_TEST_FMA)
        float fused[4];
        vst1q_f32(fused, values);
        for (int lane = 0; lane < 4; ++lane)
            fused[lane] = std::fma(fused[lane], float(j.texW - 1), 0.5f);
        const float32x4_t x = vld1q_f32(fused);
#else
        const float32x4_t x = vaddq_f32(vmulq_f32(values, texWidth), half);
#endif
        return vminq_s32(vmaxq_s32(vcvtq_s32_f32(x), zero), maxX);
    };
    const float fy = j.minY + 0.5f;
    float u = j.A_u * j.sx + j.B_u * fy + j.C_u;
    float secondU = j.secondAu * j.sx + j.secondBu * fy + j.secondCu;
    coordinates.resize(width / 4 * 4);
    for (int offset = 0; offset < (int)coordinates.size(); offset += 4)
    {
        const int32x4_t x = quantize(u, j.A_u);
        if (j.splitUVs && vminvq_u32(vceqq_s32(x, quantize(secondU, j.secondAu))) == 0)
        {
            coordinates.clear();
            return false;
        }
        vst1q_s32(coordinates.data() + offset, x);
        u += j.A_u * 4.f;
        secondU += j.secondAu * 4.f;
    }
    j.textureXs = coordinates.data();
    return true;
}

static uint32x4_t SwBlendRectanglePixels(uint32x4_t src, uint32x4_t dst, float opacity)
{
    // The same source-alpha/GL_MAX-alpha equation and operation order as
    // SwRasterizeBandImpl, with all four pixels known to be covered.
    const float32x4_t zero = vdupq_n_f32(0.f), one = vdupq_n_f32(1.f);
    const float32x4_t half = vdupq_n_f32(0.5f), max = vdupq_n_f32(255.f);
    const uint32x4_t byte = vdupq_n_u32(255u);
    const float32x4_t sa = vmulq_f32(
        vmulq_f32(vcvtq_f32_u32(vshrq_n_u32(src, 24)), vdupq_n_f32(1.f / 255.f)),
        vdupq_n_f32(std::max(0.f, std::min(1.f, opacity))));
    const float32x4_t inv = vsubq_f32(one, sa);
    const float32x4_t sr = vcvtq_f32_u32(vandq_u32(src, byte));
    const float32x4_t sg = vcvtq_f32_u32(vandq_u32(vshrq_n_u32(src, 8), byte));
    const float32x4_t sb = vcvtq_f32_u32(vandq_u32(vshrq_n_u32(src, 16), byte));
    const float32x4_t dr = vcvtq_f32_u32(vandq_u32(dst, byte));
    const float32x4_t dg = vcvtq_f32_u32(vandq_u32(vshrq_n_u32(dst, 8), byte));
    const float32x4_t db = vcvtq_f32_u32(vandq_u32(vshrq_n_u32(dst, 16), byte));
    const float32x4_t da = vcvtq_f32_u32(vshrq_n_u32(dst, 24));
    const float32x4_t r = vminq_f32(vmaxq_f32(vaddq_f32(vaddq_f32(vmulq_f32(sr, sa), vmulq_f32(dr, inv)), half), zero), max);
    const float32x4_t g = vminq_f32(vmaxq_f32(vaddq_f32(vaddq_f32(vmulq_f32(sg, sa), vmulq_f32(dg, inv)), half), zero), max);
    const float32x4_t b = vminq_f32(vmaxq_f32(vaddq_f32(vaddq_f32(vmulq_f32(sb, sa), vmulq_f32(db, inv)), half), zero), max);
    const float32x4_t a = vminq_f32(vmaxq_f32(vaddq_f32(vmaxq_f32(vmulq_f32(sa, max), da), half), zero), max);
    const uint32x4_t blended = vorrq_u32(
        vorrq_u32(vcvtq_u32_f32(r), vshlq_n_u32(vcvtq_u32_f32(g), 8)),
        vorrq_u32(vshlq_n_u32(vcvtq_u32_f32(b), 16), vshlq_n_u32(vcvtq_u32_f32(a), 24)));
    return vbslq_u32(vcgeq_f32(sa, one), vorrq_u32(src, vdupq_n_u32(0xff000000u)), blended);
}

template<bool splitUVs, bool cachedX = false>
static void SwDrawRectangleRows(SwBandJob& j)
{
    // Kirikiroid2 sends opaque rectangular operations to its copy/stretch
    // kernels. Retain our nearest-texel float recurrence, but copy the row
    // directly where opaque, using the original blend for transparent pixels.
    const float32x4_t lanes = {0.f, 1.f, 2.f, 3.f};
    const float32x4_t zero = vdupq_n_f32(0.f);
    const float32x4_t half = vdupq_n_f32(0.5f);
    const float32x4_t texWidth = vdupq_n_f32(float(j.texW - 1));
    const int32x4_t intZero = vdupq_n_s32(0);
    const int32x4_t maxX = vdupq_n_s32(j.texW - 1);
    for (int py = j.minY; py < j.minY + j.rows; ++py)
    {
        const float fy = py + 0.5f;
        float u = j.A_u * j.sx + j.B_u * fy + j.C_u;
        const float v = j.A_v * j.sx + j.B_v * fy + j.C_v;
#if defined(__SWITCH__) || defined(KRKRNS_EMOTE_TEST_FMA)
        int ty = static_cast<int>(std::fma(v, float(j.texH - 1), 0.5f));
#else
        int ty = static_cast<int>(v * (j.texH - 1) + 0.5f);
#endif
        ty = std::max(0, std::min(ty, j.texH - 1));
        float secondU = 0, secondV = 0, edge = 0;
        if constexpr (splitUVs)
        {
            secondU = j.secondAu * j.sx + j.secondBu * fy + j.secondCu;
            secondV = j.secondAv * j.sx + j.secondBv * fy + j.secondCv;
#if defined(__SWITCH__) || defined(KRKRNS_EMOTE_TEST_FMA)
            int secondY = static_cast<int>(std::fma(secondV, float(j.texH - 1), 0.5f));
#else
            int secondY = static_cast<int>(secondV * (j.texH - 1) + 0.5f);
#endif
            secondY = std::max(0, std::min(secondY, j.texH - 1));
            if (secondY != ty)
            {
                // A rounding boundary can select different source rows for
                // the two halves. Keep the general, exact path for this row.
                SwBandJob row = j;
                row.minY = py; row.rows = 1;
                SwRasterizeBandImpl<true, true, true>(row);
                continue;
            }
            edge = j.splitA * j.sx + j.splitB * fy + j.splitC;
        }
        const auto* src = reinterpret_cast<const uint32_t*>(j.texData) + size_t(ty) * j.texW;
        auto* dst = j.dst + size_t(py) * j.pitch;
        int px = j.minX;
        for (; px + 4 <= j.maxX + 1; px += 4)
        {
            int32_t computedX[4];
            const int32_t* x;
            if constexpr (cachedX)
                x = j.textureXs + px - j.minX;
            else
            {
                float32x4_t tu = vmlaq_f32(vdupq_n_f32(u), lanes, vdupq_n_f32(j.A_u));
                if constexpr (splitUVs)
                {
                    const float32x4_t values = vmlaq_f32(vdupq_n_f32(edge), lanes,
                                                        vdupq_n_f32(j.splitA));
                    const uint32x4_t primary = vorrq_u32(
                        j.ccw ? vcgtq_f32(values, zero) : vcltq_f32(values, zero),
                        vandq_u32(vceqq_f32(values, zero),
                                  vdupq_n_u32(j.splitInclusive ? 0xffffffffu : 0u)));
                    tu = vbslq_f32(primary, tu,
                        vmlaq_f32(vdupq_n_f32(secondU), lanes, vdupq_n_f32(j.secondAu)));
                }
#if defined(__SWITCH__)
                const float32x4_t texX = vfmaq_f32(half, tu, texWidth);
#elif defined(KRKRNS_EMOTE_TEST_FMA)
                float values[4];
                vst1q_f32(values, tu);
                for (int l = 0; l < 4; ++l)
                    values[l] = std::fma(values[l], float(j.texW - 1), 0.5f);
                const float32x4_t texX = vld1q_f32(values);
#else
                const float32x4_t texX = vaddq_f32(vmulq_f32(tu, texWidth), half);
#endif
                vst1q_s32(computedX, vminq_s32(vmaxq_s32(vcvtq_s32_f32(texX), intZero), maxX));
                x = computedX;
            }
            uint32x4_t pixels;
            if (x[1] == x[0] + 1 && x[2] == x[0] + 2 && x[3] == x[0] + 3)
                pixels = vld1q_u32(src + x[0]);
            else
            {
                const uint32_t values[4] = {src[x[0]], src[x[1]], src[x[2]], src[x[3]]};
                pixels = vld1q_u32(values);
            }
            const uint32x4_t alpha = vshrq_n_u32(pixels, 24);
            if (j.opacity >= 1.f && (j.opaqueTexture || vminvq_u32(alpha) == 255u))
                vst1q_u32(dst + px, pixels);
            else if (vmaxvq_u32(alpha) != 0)
                vst1q_u32(dst + px, SwBlendRectanglePixels(pixels, vld1q_u32(dst + px), j.opacity));
            u += j.A_u * 4.f;
            if constexpr (splitUVs)
            {
                secondU += j.secondAu * 4.f;
                edge += j.splitA * 4.f;
            }
        }
        for (; px <= j.maxX; ++px)
        {
            float tu = u, tv = v;
            if constexpr (splitUVs)
            {
                const float value = j.ccw ? edge : -edge;
                if (!(value > 0 || (value == 0 && j.splitInclusive)))
                {
                    tu = secondU;
                    tv = secondV;
                }
            }
            int tx = static_cast<int>(tu * (j.texW - 1) + 0.5f);
            tx = std::max(0, std::min(tx, j.texW - 1));
            int tailY = static_cast<int>(tv * (j.texH - 1) + 0.5f);
            tailY = std::max(0, std::min(tailY, j.texH - 1));
            const ColorRGBA source = j.texData[size_t(tailY) * j.texW + tx];
            auto* row = reinterpret_cast<ColorRGBA*>(dst);
            row[px] = BlendPixels(source, row[px], j.blendMode, j.opacity, j.uniformColor);
            u += j.A_u;
            if constexpr (splitUVs)
            {
                secondU += j.secondAu;
                edge += j.splitA;
            }
        }
    }
}
#endif

static void SwRasterizeBand(SwBandJob& j)
{
#if defined(__SWITCH__) || defined(KRKRNS_EMOTE_TEST_NEON)
    if (SwCanDrawRectangleRows(j))
    {
        if (j.textureXs)
        {
            if (j.splitUVs) SwDrawRectangleRows<true, true>(j);
            else SwDrawRectangleRows<false, true>(j);
        }
        else if (j.splitUVs) SwDrawRectangleRows<true>(j);
        else SwDrawRectangleRows<false>(j);
        return;
    }
#endif
    if (j.fullCoverage)
    {
        if (j.splitUVs)
        {
            if (!j.hasStencil && j.maxX - j.minX + 1 >= 64)
                SwRasterizeBandImpl<true, true, true>(j);
            else
                SwRasterizeBandImpl<false, true, true>(j);
            return;
        }
        if (!j.hasStencil && j.maxX - j.minX + 1 >= 64)
            SwRasterizeBandImpl<true, true>(j);
        else
            SwRasterizeBandImpl<false, true>(j);
        return;
    }
    // Limit coordinate vectorization to wide, unmasked scan bands; keep
    // the original lane conversion for stencil-heavy and narrow geometry.
    if (!j.hasStencil && j.maxX - j.minX + 1 >= 64)
    {
#if defined(__SWITCH__) || defined(KRKRNS_EMOTE_TEST_NEON)
        if (!j.sourceAliased && j.A_v == 0.f && j.blendMode != 1 && j.blendMode != 4 &&
            j.blendMode != 6 && j.blendMode != 21)
        {
            if (j.textureXs) SwRasterizeBandImpl<true, false, false, true, true>(j);
            else SwRasterizeBandImpl<true, false, false, true>(j);
        }
        else
#endif
            SwRasterizeBandImpl<true>(j);
    }
    else
        SwRasterizeBandImpl<false>(j);
}

static void SwRasterizeBandEntry(void* p)
{
    SwRasterizeBand(*(SwBandJob*)p);
}

struct SwMeshBandJob
{
    const SwBandJob* triangles;
    size_t triangleCount;
    int minY, endY;
#ifdef __SWITCH__
    bool measure = false;
    Uint64 start = 0, finish = 0;
    unsigned core = 0;
#endif
};

static void SwRasterizeMeshBandEntry(void* p)
{
    auto& mesh = *static_cast<SwMeshBandJob*>(p);
#ifdef __SWITCH__
    if (mesh.measure)
    {
        mesh.start = SDL_GetPerformanceCounter();
        mesh.core = svcGetCurrentProcessorNumber();
    }
#endif
    // Every worker owns the same rows for the whole mesh. Within those rows,
    // triangles still blend in index order, with the original row/UV setup.
    for (size_t i = 0; i < mesh.triangleCount; ++i)
    {
        SwBandJob job = mesh.triangles[i];
        const int endY = std::min(mesh.endY, job.minY + job.rows);
        job.minY = std::max(mesh.minY, job.minY);
        job.rows = endY - job.minY;
        if (job.rows > 0)
            SwRasterizeBand(job);
    }
#ifdef __SWITCH__
    if (mesh.measure)
        mesh.finish = SDL_GetPerformanceCounter();
#endif
}

static bool SwTryMergeRectangle(SwBandJob& first, const SwBandJob& second)
{
    // Kirikiroid2's OperateTriangles recognizes rectangular quad pairs and
    // dispatches them through OperateRect/OperateStretch. Here we retain the
    // existing float UV stepping and exactly complementary shared edges.
    // Differing maps retain both recurrences, selected by the original
    // shared-edge rule, rather than approximating them by one rectangle map.
    if (first.ccw != second.ccw || first.minX != second.minX ||
        first.maxX != second.maxX || first.minY != second.minY ||
        first.rows != second.rows)
        return false;

    struct Edge { float a, b, c; bool inclusive; };
    const Edge edges[2][3] = {
        {{first.a01, first.b01, first.c01, first.include01},
         {first.a12, first.b12, first.c12, first.include12},
         {first.a20, first.b20, first.c20, first.include20}},
        {{second.a01, second.b01, second.c01, second.include01},
         {second.a12, second.b12, second.c12, second.include12},
         {second.a20, second.b20, second.c20, second.include20}}};
    int shared[2] = {-1, -1};
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k)
            if (edges[0][i].a != 0 && edges[0][i].b != 0 &&
                edges[0][i].a == -edges[1][k].a &&
                edges[0][i].b == -edges[1][k].b &&
                edges[0][i].c == -edges[1][k].c &&
                edges[0][i].inclusive != edges[1][k].inclusive)
                shared[0] = i, shared[1] = k;
    if (shared[0] < 0)
        return false;

    const bool neonBlend = first.blendMode != 6 && first.blendMode != 21 &&
                           first.blendMode != 1 && first.blendMode != 4;
    for (int t = 0; t < 2; ++t)
        for (int i = 0; i < 3; ++i)
        {
            if (i == shared[t]) continue;
            const Edge& edge = edges[t][i];
            if ((edge.a == 0) == (edge.b == 0)) return false;
            const auto accepted = [&](float value) {
                return std::isfinite(value) && (first.ccw ? value > 0 : value < 0);
            };
            if (edge.a == 0)
            {
                // Horizontal edges are constant along a row and monotonic
                // over rows. Require both extreme pixel centres to be inside.
                const float fy0 = first.minY + 0.5f;
                const float fy1 = first.minY + first.rows - 1 + 0.5f;
                if (!accepted(edge.a * first.sx + edge.b * fy0 + edge.c) ||
                    !accepted(edge.a * first.sx + edge.b * fy1 + edge.c))
                    return false;
            }
            else
            {
                // Vertical edges are constant over rows. Walk the original
                // rounded recurrence to its last group/tail; a multiply by
                // the span would change boundary rounding on large targets.
                const float fy = first.minY + 0.5f;
                float value = edge.a * first.sx + edge.b * fy + edge.c;
                if (!accepted(value)) return false;
                int x = first.minX;
#if defined(__SWITCH__) || defined(KRKRNS_EMOTE_TEST_NEON)
                if (neonBlend)
                {
                    for (; x + 4 <= first.maxX + 1; x += 4)
                    {
                        const float32x4_t lanes = {0.f, 1.f, 2.f, 3.f};
                        const float32x4_t values = vmlaq_f32(vdupq_n_f32(value), lanes,
                                                           vdupq_n_f32(edge.a));
                        if (!accepted(vminvq_f32(values)) || !accepted(vmaxvq_f32(values)))
                            return false;
                        value += edge.a * 4.0f;
                    }
                }
#else
                (void)neonBlend;
#endif
                for (; x <= first.maxX; ++x)
                {
                    if (!accepted(value)) return false;
                    value += edge.a;
                }
            }
        }
    first.splitUVs = first.A_u != second.A_u || first.B_u != second.B_u ||
        first.C_u != second.C_u || first.A_v != second.A_v ||
        first.B_v != second.B_v || first.C_v != second.C_v;
    if (first.splitUVs)
    {
        const Edge& edge = edges[0][shared[0]];
        first.splitA = edge.a; first.splitB = edge.b; first.splitC = edge.c;
        first.splitInclusive = edge.inclusive;
        first.secondAu = second.A_u; first.secondBu = second.B_u; first.secondCu = second.C_u;
        first.secondAv = second.A_v; first.secondBv = second.B_v; first.secondCv = second.C_v;
    }
    first.fullCoverage = true;
    return true;
}

void EmoteSWRenderBackend::DrawMeshCpu(const float* vertices,
                                       int vertexCount,
                                       const uint16_t* indices,
                                       int indexCount,
                                       Texture* texture,
                                       Target* targetAsTexture,
                                       float opacity)
{
    // CPU fallback kept for renderers without target-texture/geometry support.
    const int width = currentTarget_->width;
    const int height = currentTarget_->height;
    uint32_t* dst = (uint32_t*)currentTarget_->pixels.data();
    const uint32_t* maskRowBase = maskTarget_ ? (uint32_t*)maskTarget_->pixels.data() : nullptr;
    const ColorRGBA* texData = (const ColorRGBA*)(texture ? texture->pixels.data()
                                                          : targetAsTexture->pixels.data());
    const int texW = texture ? texture->width : targetAsTexture->width;
    const int texH = texture ? texture->height : targetAsTexture->height;
    if (!texData)
        return;
#ifdef __SWITCH__
    static bool coordinatePathLogged = false;
    if (!coordinatePathLogged)
    {
        coordinatePathLogged = true;
        KRKRNS_LOG("[emote] CPU raster: vector texel coordinates (FMA)");
    }
#endif
    const bool hasStencil = (maskRowBase != nullptr);
    const int pitch = width;

    ColorRGBA uniformColor = {(uint8_t)(uniformColor_[0] * 255), (uint8_t)(uniformColor_[1] * 255),
                              (uint8_t)(uniformColor_[2] * 255), (uint8_t)(uniformColor_[3] * 255)};

    // Kirikiroid2's software RenderManager batches OperateTriangles and
    // divides rectangular pixel operations into rows with an area cutoff.
    // Use that task scope here, keeping arbitrary overlapping triangles in
    // index order within every row. Complementary quad halves also balance
    // across the same workers instead of waiting at a barrier per triangle.
    // Keep the existing path when sampling/masking the destination itself:
    // those reads can depend on triangles completed in other row bands.
    const bool batchMesh = indexCount > 3 && TVPGetThreadNum() > 1 &&
                           targetAsTexture != currentTarget_ &&
                           maskTarget_ != currentTarget_;
    std::vector<SwBandJob> triangles;
    if (batchMesh)
        triangles.reserve(static_cast<size_t>(indexCount / 3));
    int meshMinY = height, meshEndY = 0;
    uint64_t meshPixels = 0;

    for (int idx = 0; idx + 2 < indexCount; idx += 3)
    {
        uint16_t i0 = indices[idx];
        uint16_t i1 = indices[idx + 1];
        uint16_t i2 = indices[idx + 2];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
            continue;
        const float* v0 = vertices + (size_t)i0 * 4;
        const float* v1 = vertices + (size_t)i1 * 4;
        const float* v2 = vertices + (size_t)i2 * 4;
        float x0 = (v0[0] + 1.0f) * 0.5f * width;
        float y0 = (v0[1] + 1.0f) * 0.5f * height;
        float x1 = (v1[0] + 1.0f) * 0.5f * width;
        float y1 = (v1[1] + 1.0f) * 0.5f * height;
        float x2 = (v2[0] + 1.0f) * 0.5f * width;
        float y2 = (v2[1] + 1.0f) * 0.5f * height;

        int minX = (int)std::max(0.0f, std::min(x0, std::min(x1, x2)));
        int maxX = (int)std::min((float)width - 1, std::max(x0, std::max(x1, x2)));
        int minY = (int)std::max(0.0f, std::min(y0, std::min(y1, y2)));
        int maxY = (int)std::min((float)height - 1, std::max(y0, std::max(y1, y2)));
        if (minX > maxX || minY > maxY)
            continue;

        // Edge function: f_ij(x,y) = a*x + b*y + c
        float a01 = y0 - y1, b01 = x1 - x0, c01 = x0 * y1 - x1 * y0;
        float a12 = y1 - y2, b12 = x2 - x1, c12 = x1 * y2 - x2 * y1;
        float a20 = y2 - y0, b20 = x0 - x2, c20 = x2 * y0 - x0 * y2;

        float area = a12 * x0 + b12 * y0 + c12; // = 2*有符号面积
        if (std::abs(area) < 1e-6f)
            continue;
        float invArea = 1.0f / area;
        bool ccw = area > 0;
        auto inclusiveEdge = [ccw](float ax, float ay, float bx, float by) {
            const float dx = ccw ? bx - ax : ax - bx;
            const float dy = ccw ? by - ay : ay - by;
            return dy < 0 || (dy == 0 && dx > 0);
        };
        const bool include01 = inclusiveEdge(x0, y0, x1, y1);
        const bool include12 = inclusiveEdge(x1, y1, x2, y2);
        const bool include20 = inclusiveEdge(x2, y2, x0, y0);

        // 仿射纹理步进参数
        float A_u = v0[2] * a12 + v1[2] * a20 + v2[2] * a01;
        float B_u = v0[2] * b12 + v1[2] * b20 + v2[2] * b01;
        float A_v = v0[3] * a12 + v1[3] * a20 + v2[3] * a01;
        float B_v = v0[3] * b12 + v1[3] * b20 + v2[3] * b01;

        const float sampleX = minX + 0.5f, sampleY = minY + 0.5f;
        float tu0 = (A_u * sampleX + B_u * sampleY + (v0[2] * c12 + v1[2] * c20 + v2[2] * c01)) * invArea;
        float tv0 = (A_v * sampleX + B_v * sampleY + (v0[3] * c12 + v1[3] * c20 + v2[3] * c01)) * invArea;
        float du_dx = A_u * invArea, dv_dx = A_v * invArea;
        float du_dy = B_u * invArea, dv_dy = B_v * invArea;

        float f01 = a01 * sampleX + b01 * sampleY + c01;
        float f12 = a12 * sampleX + b12 * sampleY + c12;
        float f20 = a20 * sampleX + b20 * sampleY + c20;
        float df01_dx = a01, df12_dx = a12, df20_dx = a20;
        float df01_dy = b01, df12_dy = b12, df20_dy = b20;

        SwBandJob job;
        job.sourceAliased = targetAsTexture == currentTarget_ || maskTarget_ == currentTarget_;
        job.sx = sampleX;
        job.a01 = a01; job.b01 = b01; job.c01 = c01;
        job.a12 = a12; job.b12 = b12; job.c12 = c12;
        job.a20 = a20; job.b20 = b20; job.c20 = c20;
        job.A_u = du_dx; job.B_u = B_u * invArea;
        job.C_u = (v0[2] * c12 + v1[2] * c20 + v2[2] * c01) * invArea;
        job.A_v = dv_dx; job.B_v = B_v * invArea;
        job.C_v = (v0[3] * c12 + v1[3] * c20 + v2[3] * c01) * invArea;
        job.ccw = ccw;
        job.include01 = include01; job.include12 = include12; job.include20 = include20;
        job.dst = dst;
        job.maskRowBase = maskRowBase;
        job.hasStencil = hasStencil;
        job.pitch = pitch;
        job.width = width;
        job.minX = minX; job.maxX = maxX;
        job.minY = minY;
        job.rows = maxY - minY + 1;
        job.texW = texW; job.texH = texH;
        job.blendMode = blendMode_;
        job.opacity = opacity;
        job.uniformColor = uniformColor;
        job.texData = texData;
        job.opaqueTexture = texture && texture->opaquePixels;

        if (batchMesh)
        {
            triangles.push_back(job);
            meshMinY = std::min(meshMinY, minY);
            meshEndY = std::max(meshEndY, maxY + 1);
            meshPixels += static_cast<uint64_t>(maxX - minX + 1) * job.rows;
            continue;
        }

        const int totalRows = job.rows;
        int bandCount = 1;
        if (totalRows >= 96)
        {
            bandCount = TVPGetThreadNum();
            if (bandCount > TVPMaxThreadNum) bandCount = TVPMaxThreadNum;
            if (bandCount > 1)
            {
                const int minBand = (totalRows + bandCount - 1) / bandCount;
                if (minBand < 48) bandCount = std::max(1, totalRows / 48);
            }
        }
        #if defined(KRKRNS_EMOTE_VERBOSE_DIAGNOSTICS)
        // Optional row-band diagnostics for profiling the CPU fallback.
        static Uint32 diagCalls = 0, diagTall = 0, diagBanded = 0;
        static Uint64 diagRows = 0, diagBands = 0;
        static bool diagFirstLogged = false;
        diagCalls++; diagRows += (Uint64)totalRows;
        if (totalRows >= 96) { diagTall++; if (bandCount > 1) { diagBanded++; diagBands += (Uint64)bandCount; } }
        if (!diagFirstLogged)
        {
            diagFirstLogged = true;
            KRKRNS_LOG("[emote] band first: rows=%d bandCount=%d threadNum=%d",
                       totalRows, bandCount, TVPGetThreadNum());
        }
        #endif
        if (bandCount <= 1)
        {
            SwRasterizeBand(job);
        }
        else
        {
            SwBandJob jobs[TVPMaxThreadNum];
            TVPBeginThreadTask(bandCount);
            for (int b = 0; b < bandCount; b++)
            {
                jobs[b] = job;
                jobs[b].minY = minY + totalRows * b / bandCount;
                jobs[b].rows = totalRows * (b + 1) / bandCount - totalRows * b / bandCount;
                TVPExecThreadTask(SwRasterizeBandEntry, &jobs[b]);
            }
            TVPEndThreadTask();
        }
        #if defined(KRKRNS_EMOTE_VERBOSE_DIAGNOSTICS)
        if (diagCalls % 240 == 0)
        {
            KRKRNS_LOG("[emote] band diag: calls=%u tall=%u banded=%u rAvg=%.0f bAvg=%.1f",
                       diagCalls, diagTall, diagBanded,
                       (double)diagRows / diagCalls,
                       diagBanded ? (double)diagBands / diagBanded : 0.0);
        }
        #endif
        (void)f01; (void)f12; (void)f20; (void)tu0; (void)tv0;
        (void)df01_dx; (void)df12_dx; (void)df20_dx; (void)df01_dy; (void)df12_dy; (void)df20_dy;
    }

    if (triangles.empty())
        return;

    if (meshPixels >= 66 * 500 && triangles.size() == 2 &&
        SwTryMergeRectangle(triangles[0], triangles[1]))
    {
        triangles.resize(1);
#ifdef __SWITCH__
        static bool rectangleLogged = false;
        if (!rectangleLogged)
        {
            rectangleLogged = true;
            KRKRNS_LOG("[emote] CPU raster: exact rectangular mesh splitUVs=%d",
                       triangles[0].splitUVs);
        }
#endif
    }

    const int totalRows = meshEndY - meshMinY;
#if defined(__SWITCH__) || defined(KRKRNS_EMOTE_TEST_NEON)
    std::vector<std::vector<int32_t>> textureCoordinateRows;
    size_t coordinateCount = 0;
    constexpr size_t coordinateBudget = 16384; // at most 64 KiB per mesh
    for (auto& job : triangles)
    {
        if (job.B_u != 0.f || job.A_v != 0.f || job.hasStencil || job.sourceAliased ||
            job.rows < 96 || job.maxX - job.minX + 1 < 64) continue;
        const size_t count = size_t(job.maxX - job.minX + 1) / 4 * 4;
        if (count > coordinateBudget - coordinateCount) continue;
        textureCoordinateRows.emplace_back();
        SwBuildTextureXs(job, textureCoordinateRows.back());
        coordinateCount += count;
    }
#endif
    int bandCount = 1;
    if (totalRows >= 96 && meshPixels >= 66 * 500)
    {
        bandCount = std::min<int>(TVPGetThreadNum(), TVPMaxThreadNum);
        bandCount = std::min(bandCount, totalRows / 48);
    }
    if (bandCount <= 1)
    {
        for (auto& job : triangles)
            SwRasterizeBand(job);
        return;
    }
#ifdef __SWITCH__
    static bool meshBandsLogged = false;
    if (!meshBandsLogged)
    {
        meshBandsLogged = true;
        KRKRNS_LOG("[emote] CPU raster: ordered mesh row bands triangles=%zu bands=%d",
                   triangles.size(), bandCount);
    }
#endif
    SwMeshBandJob bands[TVPMaxThreadNum];
#ifdef __SWITCH__
    // Bounded samples expose actual worker execution, without logging from
    // pixel loops or changing the render target or game update interval.
    static unsigned meshSamples = 0;
    ++meshSamples;
    const bool measureBands = (texture && !texture->cpuMeshProfiled) || meshSamples <= 8 ||
        (meshSamples <= 2048 && meshSamples % 128 == 0);
    if (texture && measureBands) texture->cpuMeshProfiled = true;
    const Uint64 meshStart = measureBands ? SDL_GetPerformanceCounter() : 0;
#endif
    TVPBeginThreadTask(bandCount);
    for (int b = 0; b < bandCount; ++b)
    {
        bands[b] = {triangles.data(), triangles.size(),
                    meshMinY + totalRows * b / bandCount,
                    meshMinY + totalRows * (b + 1) / bandCount};
#ifdef __SWITCH__
        bands[b].measure = measureBands;
#endif
        TVPExecThreadTask(SwRasterizeMeshBandEntry, &bands[b]);
    }
    TVPEndThreadTask();
#ifdef __SWITCH__
    if (measureBands)
    {
        const Uint64 finish = SDL_GetPerformanceCounter();
        const double msPerTick = 1000.0 / SDL_GetPerformanceFrequency();
        const auto& first = triangles[0];
        const bool rowSample = SwCanDrawRectangleRows(first) || (!first.fullCoverage &&
            !first.sourceAliased && !first.hasStencil && first.maxX - first.minX + 1 >= 64 &&
            first.A_v == 0.f && first.blendMode != 1 && first.blendMode != 4 &&
            first.blendMode != 6 && first.blendMode != 21);
        KRKRNS_LOG("[emote] mesh sample=%u target=%dx%d texture=%dx%d mode=%d stencil=%d vertices=%d indices=%d boundsPx=%llu bands=%d rect=%d splitUVs=%d opaque=%d rowCopy=%d rowSample=%d xMap=%d opacity=%.3f wall=%.2fms",
                   meshSamples, width, height, texW, texH, blendMode_, hasStencil,
                   vertexCount, indexCount, (unsigned long long)meshPixels,
                   bandCount, triangles[0].fullCoverage, triangles[0].splitUVs,
                   triangles[0].opaqueTexture, SwCanDrawRectangleRows(triangles[0]),
                   rowSample, first.textureXs != nullptr, opacity, (finish - meshStart) * msPerTick);
        for (int b = 0; b < bandCount; ++b)
            KRKRNS_LOG("[emote] band sample=%u band=%d core=%u start=%.2fms work=%.2fms rows=%d",
                       meshSamples, b, bands[b].core,
                       (bands[b].start - meshStart) * msPerTick,
                       (bands[b].finish - bands[b].start) * msPerTick,
                       bands[b].endY - bands[b].minY);
    }
#endif
}
} // namespace krkrsdl3
