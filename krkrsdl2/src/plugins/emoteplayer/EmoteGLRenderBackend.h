#pragma once
#include "TVPCompositor.h"
#include "EmoteLayerDamage.h"
#include <cstdint>
#include <memory>

namespace krkrsdl3
{
// CPU-observed transfers/calls, not GPU hardware timings. The interface stays
// available in control builds so one host test can compare both instance modes.
struct EmoteResidentReadbackStats
{
    std::uint64_t fullReads = 0, mapReads = 0, mapBytes = 0, cacheHits = 0;
    std::uint64_t dataRects = 0, dataBytes = 0, errors = 0, fallbacks = 0;
    std::uint64_t baselineCopies = 0, liveGPUBytes = 0;
    std::uint64_t probePasses = 0, probeFailures = 0, probeMapReads = 0;
    std::uint64_t probeMapBytes = 0, probeCopies = 0, peakGPUBytes = 0;
};
// The E-mote mesh shader/blend contract is ported from the pinned open-source
// plugin backend; a private GL context isolates it from SDL's cached GL state.
class EmoteGLRenderBackend final : public iTVPRenderBackend
{
public:
    explicit EmoteGLRenderBackend(bool useResidentReadback = true);
    ~EmoteGLRenderBackend() override;
    bool IsAvailable();
    // A rejected probe must not keep an unused private context (and GPU
    // resource lease) alive after selection falls back to the CPU backend.
    // Never retire a context while client images still exist.
    bool RetireUnusedContext() noexcept;
    EmoteResidentReadbackStats GetResidentReadbackStats() const noexcept;
    // Damage is valid only for the immediately preceding synchronized Layer
    // generation; unrelated target locks and destination writes invalidate it.
    bool GetLayerDamage(void* target, const EmoteLayerIdentity& layer,
                        EmoteLayerBounds& bounds) const noexcept;
    void CommitLayerDamage(void* target, const EmoteLayerIdentity& layer) noexcept;
    void* CreateTarget(int width, int height) override;
    bool GetTargetSize(void* target, int& width, int& height) const override;
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
    // The in-process engine restart may hand the next game an SDL_Window at
    // the SAME address as the destroyed one; the window-pointer comparison in
    // begin() then keeps the context built against the dead window, and every
    // mesh draw/readback runs through it (observed on the emulator as E-mote
    // art rendered into the left quarter of the screen).  Drop the cached
    // context unconditionally, exactly like the pointer-change path does.
    void ResetForEngineRestart() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    // Full-target raster/readback self-test.  It keeps normal full reads on a
    // conforming driver and automatically selects a verified tiled readback
    // on compatibility renderers that truncate large rows.
    bool selfTest();
};
}
