#pragma once

#include <cstdint>

// Minimal 2D render-backend contract used by the open-source E-mote
// implementation. KRKR-ns supplies a private GL context plus a CPU fallback;
// the main KiriKiri Layer compositor retains ownership of the SDL renderer.
namespace krkrsdl3
{
class iTVPRenderBackend
{
public:
    virtual ~iTVPRenderBackend() = default;
    virtual void* CreateTarget(int width, int height) = 0;
    virtual void DestroyTarget(void* target) = 0;
    virtual void SetTarget(void* target) = 0;
    virtual void ClearTarget(bool clearColor) = 0;
    virtual uint8_t* LockTarget(void* target, int& pitch) = 0;
    virtual void UnlockTarget(void* target) = 0;
    virtual void* GetTargetTexture(void* target) = 0;
    virtual void UpdateTargetTexture(void* target, const uint8_t* pixels,
                                     int width, int height, int pitch) = 0;
    virtual void* CreateTexture(int width, int height) = 0;
    virtual void UpdateTexture(void* texture, const uint8_t* pixels,
                               int width, int height, int pitch) = 0;
    virtual void DestroyTexture(void* texture) = 0;
    virtual uint8_t* LockTexture(void* texture, int& pitch) = 0;
    virtual void SetMask(void* maskTarget) = 0;
    virtual void SetBlendMode(int mode, const float* uniformColor = nullptr) = 0;
    virtual void DrawMesh(const float* vertices, int vertexCount,
                          const uint16_t* indices, int indexCount,
                          void* texture, float opacity) = 0;

    // Called when the host rebuilds the engine in-process and the SDL renderer
    // this backend cached has been destroyed.  Backends that hold no renderer
    // state keep the default no-op.
    virtual void ResetForEngineRestart() {}
};

iTVPRenderBackend* TVPGetRenderBackend();

// Resets whichever backend is selected; see the definition in TVPCompositor.cpp.
void TVPResetRenderBackendForEngineRestart();
} // namespace krkrsdl3


