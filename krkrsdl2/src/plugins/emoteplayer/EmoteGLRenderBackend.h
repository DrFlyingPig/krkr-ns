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
    // Full-target raster/readback self-test.  It keeps normal full reads on a
    // conforming driver and automatically selects a verified tiled readback
    // on compatibility renderers that truncate large rows.
    bool selfTest();
};
}
