#include "EmoteSWRenderBackend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <cstddef>
#include <stdexcept>

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

static void SwRasterizeBand(SwBandJob& j)
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

        ColorRGBA* row = (ColorRGBA*)(j.dst + (size_t)py * j.pitch);
        const uint32_t* maskRow = j.maskRowBase ? j.maskRowBase + (size_t)py * j.width : nullptr;

        for (int px = j.minX; px <= j.maxX; px++)
        {
            const float e01 = j.ccw ? f01_row : -f01_row;
            const float e12 = j.ccw ? f12_row : -f12_row;
            const float e20 = j.ccw ? f20_row : -f20_row;
            const bool inside = (e01 > 0 || (e01 == 0 && j.include01)) &&
                                (e12 > 0 || (e12 == 0 && j.include12)) &&
                                (e20 > 0 || (e20 == 0 && j.include20));
            if (inside)
            {
                int tx = (int)(tu_row * texW_1 + 0.5f);
                int ty = (int)(tv_row * texH_1 + 0.5f);
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
        }
    }
}

static void SwRasterizeBandEntry(void* p)
{
    SwRasterizeBand(*(SwBandJob*)p);
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
    const bool hasStencil = (maskRowBase != nullptr);
    const int pitch = width;

    ColorRGBA uniformColor = {(uint8_t)(uniformColor_[0] * 255), (uint8_t)(uniformColor_[1] * 255),
                              (uint8_t)(uniformColor_[2] * 255), (uint8_t)(uniformColor_[3] * 255)};

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
        // KRKR-ns Phase 1b diag: does the row-band pool actually engage on
        // the device? (device log showed drawT=4 yet compose barely moved)
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
        if (diagCalls % 240 == 0)
        {
            KRKRNS_LOG("[emote] band diag: calls=%u tall=%u banded=%u rAvg=%.0f bAvg=%.1f",
                       diagCalls, diagTall, diagBanded,
                       (double)diagRows / diagCalls,
                       diagBanded ? (double)diagBands / diagBanded : 0.0);
        }
        (void)f01; (void)f12; (void)f20; (void)tu0; (void)tv0;
        (void)df01_dx; (void)df12_dx; (void)df20_dx; (void)df01_dy; (void)df12_dy; (void)df20_dy;
    }
}
} // namespace krkrsdl3
