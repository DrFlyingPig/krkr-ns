/* SPDX-License-Identifier: MIT */
#pragma once
#include <SDL.h>
#include <cstdint>
#include <memory>
#include <mutex>

namespace krkrns
{
// Pixels are native KiriKiri BGRA words. row0 is the logical top row and
// pitch may be negative; these raw fields never invoke engine accessors.
struct BitmapView
{
    const void* storage = nullptr;
    std::uint64_t serial = 0;
    std::uint64_t version = 0;
    std::uint8_t* row0 = nullptr;
    int width = 0, height = 0, pitch = 0, bpp = 32;
    bool volatileCPU = false;
};

enum class BitmapBlend { Copy, Alpha, AddAlpha };
// On the opaque internal target, Alpha/AddAlpha and translucent Copy follow
// NX's GL methods: RGB is blended, destination alpha is retained. Ordinary
// script-visible alpha bitmaps are never accepted as GPU write targets.
struct ResidentFrameState;
struct ResidentFrameSnapshot;

// A frame owns its SDL texture, not its engine bitmap. Storage destruction or
// COW cannot dangle a queued frame. ResetRenderer invalidates every frame.
class ResidentFrame
{
    std::shared_ptr<ResidentFrameState> state_;
    std::shared_ptr<ResidentFrameSnapshot> snapshot_;
    explicit ResidentFrame(std::shared_ptr<ResidentFrameState> state, bool snapshot);
    friend class SDLBitmapCompositor;
public:
    ResidentFrame() = default;
    SDL_Texture* Texture() const noexcept;
    int Width() const noexcept;
    int Height() const noexcept;
    explicit operator bool() const noexcept { return Texture() != nullptr; }
};

class SDLBitmapCompositor
{
    struct Impl;
    mutable std::recursive_mutex mutex_;
    std::unique_ptr<Impl> impl_;
public:
    SDLBitmapCompositor();
    ~SDLBitmapCompositor();
    SDLBitmapCompositor(const SDLBitmapCompositor&) = delete;
    SDLBitmapCompositor& operator=(const SDLBitmapCompositor&) = delete;

    // Bind/reset run on the renderer owner thread. Reset synchronizes live
    // GPU-only targets before destroying textures; false leaves them alive.
    bool BindRenderer(SDL_Renderer* renderer);
    bool ResetRenderer();
    // Device-loss teardown only: discard derived target data without pretending
    // it was synchronized. The caller must invalidate/repaint the primary tree.
    void DiscardRenderer() noexcept;
    SDL_Renderer* Renderer() const noexcept;
    std::uint64_t LogicalTextureBytes() const noexcept;
    bool SetOpaqueTarget(const BitmapView& target);
    bool IsOpaqueTarget(const BitmapView& target) const noexcept;
    bool HasGPUWrites(const BitmapView& bitmap) const noexcept;

    // Group draws to the same canvas without rebinding SDL's framebuffer for
    // every primitive. EndBatch restores the caller's complete renderer state.
    // A failed restore throws; already submitted draws must never be replayed.
    bool BeginBatch();
    void EndBatch();

    bool TryCopy(const BitmapView& target, int x, int y,
                 const BitmapView& source, const SDL_Rect& sourceRect);
    // False means safe CPU fallback before submission. A post-submit driver
    // or state-restore failure throws: replaying an alpha blend is unsafe.
    bool TryBlt(const BitmapView& target, int x, int y,
                const BitmapView& source, const SDL_Rect& sourceRect,
                BitmapBlend blend, int opacity, bool holdDestinationAlpha);
    bool TryFill(const BitmapView& target, const SDL_Rect& rect,
                 std::uint32_t bgra);

    // SyncCPU never invalidates a correct GPU texture. Before any CPU writer
    // the engine must SyncCPU, increment its version, then call CPUWritten.
    bool SyncCPU(const BitmapView& bitmap);
    void CPUWritten(const BitmapView& bitmap) noexcept;
    void Forget(const void* storage, std::uint64_t serial) noexcept;
    // Immutable snapshots trigger COW. A live canvas is for same-thread window
    // presentation and follows later writes to that texture without COW.
    ResidentFrame GetResidentFrame(const BitmapView& bitmap);
    ResidentFrame GetResidentCanvas(const BitmapView& bitmap);
    bool ReadFrame(const ResidentFrame& frame, SDL_Surface* surface);
};

SDLBitmapCompositor& GetBitmapCompositor();
}
