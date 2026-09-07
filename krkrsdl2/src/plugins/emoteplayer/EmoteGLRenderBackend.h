#pragma once
#include "TVPCompositor.h"
#include <memory>

namespace krkrsdl3
{
// The E-mote mesh shader/blend contract is ported from the pinned open-source
// plugin backend; a private GL context isolates it from SDL's cached GL state.
class EmoteGLRenderBackend final : public iTVPRenderBackend
{
public:
    EmoteGLRenderBackend();
    ~EmoteGLRenderBackend() override;
    bool IsAvailable();
    void* CreateTarget(int width, int height) override;
    void DestroyTarget(void* target) override;
    void SetTarget(void* target) override;
    void ClearTarget(bool clearColor) override;
    uint8_t* LockTarget(void* target, int& pitch) override;
    void UnlockTarget(void* target) override;
    void* GetTargetTexture(void* target) override;
    void UpdateTargetTexture(void* target, const uint8_t* pixels, int width, int height, int pitch) override;
    void* CreateTexture(int width, int height) override;
    void UpdateTexture(void* texture, const uint8_t* pixels, int width, int height, int pitch) override;
    void DestroyTexture(void* texture) override;
    uint8_t* LockTexture(void* texture, int& pitch) override;
    void SetMask(void* maskTarget) override;
    void SetBlendMode(int mode, const float* uniformColor) override;
    void DrawMesh(const float* vertices, int vertexCount, const uint16_t* indices,
                  int indexCount, void* texture, float opacity) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    // KRKR-ns Phase 2: full-target raster self-test (rejects drivers whose
    // triangle rasterization clamps to a fraction of the FBO, e.g. the
    // Nextendo emulator's software GLES which squeezed meshes into the left
    // 320x720 of a 1280x720 target).
    bool selfTest();
};
}
