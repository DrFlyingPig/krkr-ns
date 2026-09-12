#pragma once

#include <cstdint>
#include <vector>

#include "TVPCompositor.h"

struct SDL_Renderer;
struct SDL_Texture;

namespace krkrsdl3
{
class EmoteSWRenderBackend final : public iTVPRenderBackend
{
public:
    // The SDL geometry experiment does not implement the source shader's
    // binary stencil and GL_MAX alpha. Use this class only as a CPU fallback.
    explicit EmoteSWRenderBackend(bool enableSdlGeometry = false)
        : gpuDisabled_(!enableSdlGeometry) {}
    ~EmoteSWRenderBackend() override;

    // Drop every GPU handle because the SDL renderer was destroyed with the
    // previous engine (in-process engine restart).  See the definition.
    void ResetForEngineRestart() override;

    void* CreateTarget(int width, int height) override;
    void DestroyTarget(void* target) override;
    void SetTarget(void* target) override;
    void ClearTarget(bool clearColor) override;
    uint8_t* LockTarget(void* target, int& pitch) override;
    void UnlockTarget(void* target) override;
    void* GetTargetTexture(void* target) override;
    void UpdateTargetTexture(void* target, const uint8_t* pixels,
                             int width, int height, int pitch) override;
    void* CreateTexture(int width, int height) override;
    void UpdateTexture(void* texture, const uint8_t* pixels,
                       int width, int height, int pitch) override;
    void DestroyTexture(void* texture) override;
    uint8_t* LockTexture(void* texture, int& pitch) override;
    void SetMask(void* maskTarget) override;
    void SetBlendMode(int mode, const float* uniformColor) override;
    void DrawMesh(const float* vertices, int vertexCount,
                  const uint16_t* indices, int indexCount,
                  void* texture, float opacity) override;

private:
    struct Target
    {
        std::vector<uint8_t> pixels;
        SDL_Texture* gpuTexture = nullptr;
        int width = 0;
        int height = 0;
    };
    struct Texture
    {
        std::vector<uint8_t> pixels;
        SDL_Texture* gpuTexture = nullptr;
        SDL_Texture* gpuAlphaTexture = nullptr;
        bool gpuAlphaDirty = true;
        int width = 0;
        int height = 0;
    };

    struct SavedRendererState
    {
        SDL_Texture* target = nullptr;
        int logicalWidth = 0;
        int logicalHeight = 0;
        bool integerScale = false;
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        int viewportX = 0;
        int viewportY = 0;
        int viewportWidth = 0;
        int viewportHeight = 0;
        bool clipEnabled = false;
        int clipX = 0;
        int clipY = 0;
        int clipWidth = 0;
        int clipHeight = 0;
        uint8_t drawR = 0;
        uint8_t drawG = 0;
        uint8_t drawB = 0;
        uint8_t drawA = 0;
        int drawBlendMode = 0;
        bool valid = false;
    };

    Target* FindTarget(void* handle) const;
    Texture* FindTexture(void* handle) const;
    bool EnsureGpuRenderer();
    bool EnsureTargetGpu(Target* target);
    bool EnsureTextureGpu(Texture* texture);
    SDL_Texture* EnsureAlphaTextureGpu(Texture* texture);
    bool BeginGpuSession();
    bool BindGpuTarget(SDL_Texture* texture, int width, int height);
    void EndGpuSession();
    bool EnsureScratchGpu(int width, int height);
    bool DrawMeshGpu(const float* vertices, int vertexCount,
                     const uint16_t* indices, int indexCount,
                     Texture* texture, Target* targetAsTexture, float opacity);
    void DrawMeshCpu(const float* vertices, int vertexCount,
                     const uint16_t* indices, int indexCount,
                     Texture* texture, Target* targetAsTexture, float opacity);
    void ReleaseGpuResources(bool rendererIsAlive);

    Target* currentTarget_ = nullptr;
    Target* maskTarget_ = nullptr;
    int blendMode_ = 0;
    bool skipDraw_ = false;
    float uniformColor_[4] = {0, 0, 0, 0};
    std::vector<Target*> targets_;
    std::vector<Texture*> textures_;
    SDL_Renderer* gpuRenderer_ = nullptr;
    SDL_Texture* gpuScratch_ = nullptr;
    int gpuScratchWidth_ = 0;
    int gpuScratchHeight_ = 0;
    bool gpuSessionActive_ = false;
    bool gpuDisabled_ = false;
    bool gpuReported_ = false;
    bool gpuFailureReported_ = false;
    SavedRendererState savedRendererState_;
    std::vector<float> gpuPositions_;
    std::vector<uint8_t> gpuColors_;
    std::vector<uint8_t> gpuAlphaPixels_;
    uint64_t profileDrawTicks_ = 0;
    uint64_t profileReadbackTicks_ = 0;
    uint64_t profileUnpremultiplyTicks_ = 0;
    uint64_t profileDrawCalls_ = 0;
    uint64_t profileMaskedDrawCalls_ = 0;
    uint32_t profileFrames_ = 0;
};
} // namespace krkrsdl3
