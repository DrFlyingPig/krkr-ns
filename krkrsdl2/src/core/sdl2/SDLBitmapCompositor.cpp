/* SPDX-License-Identifier: MIT */
#include "SDLBitmapCompositor.h"
#include "KrkrNSResourcePolicy.h"
#include "KrkrNSPerf.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace krkrns
{
namespace
{
#ifdef KRKRNS_BITMAP_GPU_TEST_BUDGET
constexpr std::uint64_t TextureBudget = KRKRNS_BITMAP_GPU_TEST_BUDGET;
#else
constexpr std::uint64_t TextureBudget = 128ull * 1024 * 1024;
#endif
constexpr int ReadTile = 512;
struct RendererEpoch;
class SubmittedGPUFailure final : public std::exception
{
public:
    const char* what() const noexcept override
        { return "GPU bitmap submission failed after drawing; CPU replay is unsafe"; }
};
}

struct ResidentFrameState
{
    std::atomic<SDL_Texture*> texture{nullptr};
    std::atomic<unsigned> snapshots{0};
    std::shared_ptr<RendererEpoch> epoch;
    int width = 0, height = 0;
    std::uint64_t bytes = 0;
    ResidentFrameState* allPrevious = nullptr;
    ResidentFrameState* allNext = nullptr;
    ResidentFrameState* retiredNext = nullptr;
};

// Copies of one immutable frame share a pin. Live window canvases retain the
// resource lifetime but deliberately do not pin its pixels against later draws.
struct ResidentFrameSnapshot
{
    std::shared_ptr<ResidentFrameState> state;
    explicit ResidentFrameSnapshot(std::shared_ptr<ResidentFrameState> value)
        : state(std::move(value)) { state->snapshots.fetch_add(1, std::memory_order_relaxed); }
    ~ResidentFrameSnapshot() { state->snapshots.fetch_sub(1, std::memory_order_relaxed); }
};

namespace
{
struct RendererEpoch
{
    SDL_Renderer* renderer = nullptr;
    SDL_threadID owner = 0;
    std::atomic<bool> alive{true};
    std::atomic<std::uint64_t> bytes{0};
    std::mutex mutex;
    ResidentFrameState* resources = nullptr;
    ResidentFrameState* retired = nullptr;
};

void Unlink(RendererEpoch& epoch, ResidentFrameState* state)
{
    if (state->allPrevious) state->allPrevious->allNext = state->allNext;
    else epoch.resources = state->allNext;
    if (state->allNext) state->allNext->allPrevious = state->allPrevious;
}

void DestroyTexture(RendererEpoch& epoch, ResidentFrameState& state)
{
    if (auto* texture = state.texture.exchange(nullptr)) {
        SDL_DestroyTexture(texture);
        epoch.bytes.fetch_sub(state.bytes, std::memory_order_relaxed);
    }
}

void RetireResource(ResidentFrameState* state) noexcept
{
    const auto epoch = state->epoch;
    bool remove = false;
    {
        std::lock_guard<std::mutex> lock(epoch->mutex);
        if (!epoch->alive.load(std::memory_order_acquire)) {
            // ResetRenderer already destroyed every tracked texture, even
            // ones retained solely by a frame or awaiting worker retirement.
            Unlink(*epoch, state);
            remove = true;
        } else if (SDL_ThreadID() == epoch->owner) {
            DestroyTexture(*epoch, *state);
            Unlink(*epoch, state);
            remove = true;
        } else {
            // No allocation and no renderer call from bitmap destruction on
            // a worker. The owner drains this intrusive retirement list.
            state->retiredNext = epoch->retired;
            epoch->retired = state;
        }
    }
    if (remove) delete state;
}

void DrainRetired(const std::shared_ptr<RendererEpoch>& epoch)
{
    ResidentFrameState* retired = nullptr;
    {
        std::lock_guard<std::mutex> lock(epoch->mutex);
        retired = epoch->retired;
        epoch->retired = nullptr;
        for (auto* state = retired; state; state = state->retiredNext) {
            DestroyTexture(*epoch, *state);
            Unlink(*epoch, state);
        }
    }
    while (retired) {
        auto* next = retired->retiredNext;
        delete retired;
        retired = next;
    }
}

bool ValidView(const BitmapView& view)
{
    const auto pitch = view.pitch < 0 ? -std::int64_t(view.pitch) : std::int64_t(view.pitch);
    return view.storage && view.serial && view.row0 && view.bpp == 32 &&
        view.width > 0 && view.height > 0 &&
        std::int64_t(view.width) * 4 <= pitch &&
        pitch <= (std::numeric_limits<int>::max)() &&
        std::uint64_t(pitch) * std::uint64_t(view.height) <=
            std::uint64_t((std::numeric_limits<std::ptrdiff_t>::max)());
}

struct Key
{
    const void* storage;
    std::uint64_t serial;
    bool operator==(const Key& other) const noexcept
        { return storage == other.storage && serial == other.serial; }
};
struct KeyHash
{
    std::size_t operator()(const Key& key) const noexcept
    {
        return std::hash<const void*>{}(key.storage) ^
            (std::hash<std::uint64_t>{}(key.serial) + std::size_t(0x9e3779b9));
    }
};
Key Identity(const BitmapView& view) { return {view.storage, view.serial}; }

// SDL owns the GL state cache. Use its target/viewport APIs exclusively and
// restore the complete public renderer state before returning to the engine.
class RendererScope
{
    SDL_Renderer* renderer;
    SDL_Texture* target;
    SDL_Rect viewport{}, clip{};
    float scaleX = 1, scaleY = 1;
    int logicalWidth = 0, logicalHeight = 0;
    SDL_bool integerScale = SDL_FALSE, clipped = SDL_FALSE;
    Uint8 red = 0, green = 0, blue = 0, alpha = 0;
    SDL_BlendMode blend = SDL_BLENDMODE_NONE;
    bool restored = false;
public:
    explicit RendererScope(SDL_Renderer* value) : renderer(value), target(SDL_GetRenderTarget(value))
    {
        SDL_RenderGetViewport(renderer, &viewport);
        SDL_RenderGetClipRect(renderer, &clip);
        clipped = SDL_RenderIsClipEnabled(renderer);
        SDL_RenderGetScale(renderer, &scaleX, &scaleY);
        SDL_RenderGetLogicalSize(renderer, &logicalWidth, &logicalHeight);
        integerScale = SDL_RenderGetIntegerScale(renderer);
        SDL_GetRenderDrawColor(renderer, &red, &green, &blue, &alpha);
        SDL_GetRenderDrawBlendMode(renderer, &blend);
    }
    bool Begin(SDL_Texture* texture)
    {
        return SDL_SetRenderTarget(renderer, texture) == 0 &&
            SDL_RenderSetLogicalSize(renderer, 0, 0) == 0 &&
            SDL_RenderSetIntegerScale(renderer, SDL_FALSE) == 0 &&
            SDL_RenderSetScale(renderer, 1, 1) == 0 &&
            SDL_RenderSetViewport(renderer, nullptr) == 0 &&
            SDL_RenderSetClipRect(renderer, nullptr) == 0;
    }
    bool Restore()
    {
        if (restored) return true;
        bool good = SDL_SetRenderTarget(renderer, target) == 0;
        good = SDL_RenderSetLogicalSize(renderer, logicalWidth, logicalHeight) == 0 && good;
        good = SDL_RenderSetIntegerScale(renderer, integerScale) == 0 && good;
        good = SDL_RenderSetScale(renderer, scaleX, scaleY) == 0 && good;
        good = SDL_RenderSetViewport(renderer, &viewport) == 0 && good;
        good = SDL_RenderSetClipRect(renderer, clipped ? &clip : nullptr) == 0 && good;
        good = SDL_SetRenderDrawColor(renderer, red, green, blue, alpha) == 0 && good;
        good = SDL_SetRenderDrawBlendMode(renderer, blend) == 0 && good;
        restored = good;
        return good;
    }
    void Abandon() noexcept { restored = true; }
    ~RendererScope() { Restore(); }
};

bool ClipCopy(const BitmapView& target, int x, int y, const BitmapView& source,
              const SDL_Rect& original, SDL_Rect& from, SDL_Rect& to)
{
    std::int64_t sx = original.x, sy = original.y, dx = x, dy = y;
    std::int64_t width = original.w, height = original.h;
    if (width <= 0 || height <= 0) return false;
    if (sx < 0) { dx -= sx; width += sx; sx = 0; }
    if (sy < 0) { dy -= sy; height += sy; sy = 0; }
    if (dx < 0) { sx -= dx; width += dx; dx = 0; }
    if (dy < 0) { sy -= dy; height += dy; dy = 0; }
    width = (std::min)(width, (std::min)(std::int64_t(source.width) - sx,
                                      std::int64_t(target.width) - dx));
    height = (std::min)(height, (std::min)(std::int64_t(source.height) - sy,
                                        std::int64_t(target.height) - dy));
    if (width <= 0 || height <= 0) return false;
    from = {int(sx), int(sy), int(width), int(height)};
    to = {int(dx), int(dy), int(width), int(height)};
    return true;
}
}

struct SDLBitmapCompositor::Impl
{
    struct Entry
    {
        BitmapView view;
        bool opaqueTarget = false, targetValid = false, cpuStale = false;
        bool rgbaValid = false, opaqueValid = false;
        std::uint64_t rgbaVersion = 0, opaqueVersion = 0, used = 0;
        std::shared_ptr<ResidentFrameState> target, rgba, opaque;
    };
    mutable std::mutex mutex;
    SDL_Renderer* renderer = nullptr;
    SDL_RendererInfo info{};
    std::shared_ptr<RendererEpoch> epoch;
    std::unordered_map<Key, Entry, KeyHash> entries;
    std::vector<std::uint8_t> scratch;
    KrkrNSGpuResourceLease lease;
    std::uint64_t clock = 0;
    unsigned batchDepth = 0;
    std::unique_ptr<RendererScope> drawScope;
    std::shared_ptr<ResidentFrameState> drawResource;
    SDL_Texture* drawTarget = nullptr;

    void EndTarget()
    {
        if (drawScope) {
            const bool good = drawScope->Restore();
            // Keep the original state available so teardown or a subsequent
            // synchronization can retry a transient driver restore failure.
            if (!good) throw SubmittedGPUFailure();
            drawScope.reset();
            drawTarget = nullptr;
            drawResource.reset();
        }
    }
    bool BeginTarget(const std::shared_ptr<ResidentFrameState>& resource)
    {
        auto* target = resource->texture.load();
        if (drawScope && drawTarget == target) return true;
        EndTarget();
        KrkrNSPerfScope perf(KrkrNSPerfOp::GpuCanvasTargetSwitch);
        drawScope = std::make_unique<RendererScope>(renderer);
        // A worker may retire the engine bitmap while this batch owns its
        // renderer target. Keep the resource alive until the target is unbound.
        drawResource = resource;
        if (!drawScope->Begin(target)) {
            EndTarget();
            return false;
        }
        drawTarget = target;
        return true;
    }
    void EndPrimitive() { if (!batchDepth) EndTarget(); }

    bool Owner() const noexcept
        { return epoch && epoch->alive.load() && SDL_ThreadID() == epoch->owner; }
    bool Fits(const BitmapView& view) const
    {
        return ValidView(view) &&
            view.width <= (info.max_texture_width ? info.max_texture_width : 4096) &&
            view.height <= (info.max_texture_height ? info.max_texture_height : 4096);
    }
    Entry* Find(const BitmapView& view)
    {
        auto found = entries.find(Identity(view));
        return found == entries.end() ? nullptr : &found->second;
    }
    Entry* Get(const BitmapView& view)
    {
        if (!Fits(view)) return nullptr;
        auto result = entries.emplace(Identity(view), Entry{});
        auto& entry = result.first->second;
        if (!result.second && entry.cpuStale &&
            (entry.view.width != view.width || entry.view.height != view.height ||
             entry.view.row0 != view.row0 || entry.view.version != view.version)) {
            SDL_SetError("GPU bitmap changed without CPU synchronization");
            return nullptr;
        }
        if (entry.view.width != view.width || entry.view.height != view.height) {
            entry.target.reset(); entry.rgba.reset(); entry.opaque.reset();
            entry.targetValid = entry.rgbaValid = entry.opaqueValid = false;
        }
        entry.view = view;
        entry.used = ++clock;
        return &entry;
    }
    bool Reserve(std::uint64_t bytes, const Entry* protectedEntry)
    {
        if (bytes > TextureBudget) return false;
        DrainRetired(epoch);
        while (epoch->bytes.load() > TextureBudget - bytes) {
            Entry* victim = nullptr;
            for (auto& item : entries) {
                auto& entry = item.second;
                if (&entry == protectedEntry) continue;
                const bool disposable = (entry.rgba && entry.rgba.use_count() == 1) ||
                    (entry.opaque && entry.opaque.use_count() == 1);
                if (disposable && (!victim || entry.used < victim->used)) victim = &entry;
            }
            if (!victim) return false;
            if (victim->rgba && victim->rgba.use_count() == 1) {
                victim->rgba.reset(); victim->rgbaValid = false;
            }
            if (victim->opaque && victim->opaque.use_count() == 1) {
                victim->opaque.reset(); victim->opaqueValid = false;
            }
        }
        return true;
    }
    std::shared_ptr<ResidentFrameState> Create(const BitmapView& view, bool target,
                                               bool opaque, const Entry* protect)
    {
        struct CreationFailure
        {
            Impl& owner;
            bool succeeded = false;
            ~CreationFailure() {
                if (!succeeded && !owner.epoch->bytes.load(std::memory_order_relaxed))
                    owner.lease.Release();
            }
        } failure{*this};
        const std::uint64_t bytes = std::uint64_t(view.width) * view.height * 4;
        if (!Reserve(bytes, protect)) return {};
        if (!lease.Acquire()) return {};
        KrkrNSPerfScope perf(KrkrNSPerfOp::GpuCanvasCreate, bytes);
        SDL_Texture* texture = SDL_CreateTexture(renderer,
            opaque ? SDL_PIXELFORMAT_RGB888 : SDL_PIXELFORMAT_ARGB8888,
            target ? SDL_TEXTUREACCESS_TARGET : SDL_TEXTUREACCESS_STREAMING,
            view.width, view.height);
        if (!texture) {
            if (!epoch->bytes.load()) lease.Release();
            return {};
        }
        auto* state = new (std::nothrow) ResidentFrameState;
        if (!state) { SDL_DestroyTexture(texture); return {}; }
        state->texture.store(texture);
        state->epoch = epoch; state->width = view.width; state->height = view.height;
        state->bytes = bytes;
        {
            std::lock_guard<std::mutex> lock(epoch->mutex);
            state->allNext = epoch->resources;
            if (state->allNext) state->allNext->allPrevious = state;
            epoch->resources = state;
            epoch->bytes.fetch_add(bytes);
        }
        std::shared_ptr<ResidentFrameState> result;
        try { result = std::shared_ptr<ResidentFrameState>(state, RetireResource); }
        catch (...) { return {}; } // shared_ptr invokes its deleter on failure.
        if (SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE) != 0 ||
            SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest) != 0) return {};
        failure.succeeded = true;
        return result;
    }
    bool Upload(const std::shared_ptr<ResidentFrameState>& resource, const BitmapView& view)
    {
        KrkrNSPerfScope perf(KrkrNSPerfOp::GpuCanvasUpload,
            std::uint64_t(view.width) * view.height * 4);
        const void* pixels = view.row0;
        int pitch = view.pitch;
        if (pitch < 0) {
            const std::size_t rowBytes = std::size_t(view.width) * 4;
            scratch.resize(rowBytes * view.height);
            for (int y = 0; y < view.height; ++y)
                std::memcpy(scratch.data() + std::size_t(y) * rowBytes,
                    view.row0 + std::ptrdiff_t(y) * view.pitch, rowBytes);
            pixels = scratch.data(); pitch = int(rowBytes);
        }
        return SDL_UpdateTexture(resource->texture.load(), nullptr, pixels, pitch) == 0;
    }
    bool Read(const std::shared_ptr<ResidentFrameState>& resource,
              std::uint8_t* row0, int pitch, Uint32 format)
    {
        if (!Owner() || !resource || !resource->texture.load()) return false;
        EndTarget();
        KrkrNSPerfScope perf(KrkrNSPerfOp::GpuCanvasReadback,
            std::uint64_t(resource->width) * resource->height * 4);
        RendererScope scope(renderer);
        if (!scope.Begin(resource->texture.load())) return false;
        // SDL handles its own GLES framebuffer orientation and format. The
        // bounded read rectangles avoid drivers truncating a full large row.
        scratch.resize(std::size_t(ReadTile) * ReadTile * 4);
        for (int y = 0; y < resource->height; y += ReadTile) {
            for (int x = 0; x < resource->width; x += ReadTile) {
                SDL_Rect rect{x, y, (std::min)(ReadTile, resource->width - x),
                                   (std::min)(ReadTile, resource->height - y)};
                if (SDL_RenderReadPixels(renderer, &rect, format,
                    scratch.data(), rect.w * 4) != 0) return false;
                for (int row = 0; row < rect.h; ++row)
                    std::memcpy(row0 + std::ptrdiff_t(y + row) * pitch + x * 4,
                        scratch.data() + std::size_t(row) * rect.w * 4,
                        std::size_t(rect.w) * 4);
            }
        }
        return scope.Restore();
    }
    bool Sync(Entry& entry)
    {
        if (!entry.cpuStale) return true;
        if (!ValidView(entry.view) || !entry.targetValid ||
            !Read(entry.target, entry.view.row0, entry.view.pitch, SDL_PIXELFORMAT_ARGB8888))
            return false;
        entry.cpuStale = false;
        return true;
    }
    bool Target(Entry& entry, bool writing)
    {
        if (!entry.opaqueTarget || entry.view.volatileCPU) return false;
        if (!entry.target || (!entry.targetValid && entry.target->snapshots.load(std::memory_order_relaxed)))
            entry.target = Create(entry.view, true, false, &entry);
        if (!entry.target) return false;
        if (!entry.targetValid) {
            if (entry.cpuStale || !Upload(entry.target, entry.view)) return false;
            entry.targetValid = true;
        }
        if (writing && entry.target->snapshots.load(std::memory_order_relaxed)) {
            // Completed frames own GPU resources, not engine storage. Preserve
            // a queued frame when its bitmap is written again before display.
            auto copy = Create(entry.view, true, false, &entry);
            if (!copy) return false;
            EndTarget();
            KrkrNSPerfScope perf(KrkrNSPerfOp::GpuCanvasCopy,
                std::uint64_t(entry.view.width) * entry.view.height * 4);
            RendererScope scope(renderer);
            auto* source = entry.target->texture.load();
            if (!scope.Begin(copy->texture.load()) ||
                SDL_SetTextureColorMod(source, 255, 255, 255) != 0 ||
                SDL_SetTextureAlphaMod(source, 255) != 0 ||
                SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE) != 0)
                return false;
            const bool copied = SDL_RenderCopy(renderer, source, nullptr, nullptr) == 0;
            const bool restored = scope.Restore();
            if (!copied || !restored) throw SubmittedGPUFailure();
            entry.target = std::move(copy);
        }
        return true;
    }
    std::shared_ptr<ResidentFrameState> Source(Entry& entry, bool opaque)
    {
        if (!opaque && entry.opaqueTarget && entry.targetValid) return entry.target;
        if (entry.cpuStale && !Sync(entry)) return {};
        auto& resource = opaque ? entry.opaque : entry.rgba;
        auto& valid = opaque ? entry.opaqueValid : entry.rgbaValid;
        auto& version = opaque ? entry.opaqueVersion : entry.rgbaVersion;
        const bool upload = !valid || entry.view.volatileCPU || version != entry.view.version;
        if (!resource || (upload && resource.use_count() > 1))
            resource = Create(entry.view, false, opaque, &entry);
        if (!resource) return {};
        if (upload) {
            if (!Upload(resource, entry.view)) { valid = false; return {}; }
            valid = true; version = entry.view.version;
        }
        return resource;
    }
    bool Draw(Entry& target, const std::shared_ptr<ResidentFrameState>& source,
              const SDL_Rect& from, const SDL_Rect& to, BitmapBlend method,
              int opacity, bool holdAlpha)
    {
        if (!Target(target, true)) return false;
        auto* texture = source->texture.load();
        SDL_BlendMode blend = SDL_BLENDMODE_NONE;
        Uint8 color = 255;
        if (method == BitmapBlend::Copy) {
            if (opacity != 255) blend = SDL_ComposeCustomBlendMode(
                SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
                SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
            else if (holdAlpha) blend = SDL_ComposeCustomBlendMode(
                SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ZERO, SDL_BLENDOPERATION_ADD,
                SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
        } else if (method == BitmapBlend::Alpha) {
            // NX RenderManager_ogl AlphaBlend uses exactly these factors,
            // preserving destination alpha on an opaque render target.
            blend = SDL_ComposeCustomBlendMode(
                SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
                SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
        } else {
            // NX AdditiveAlphaBlend emits s*opacity, ONE / ONE_MINUS_SRC_ALPHA
            // for RGB and ZERO / ONE for alpha (premultiplied source).
            blend = SDL_ComposeCustomBlendMode(
                SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
                SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
            color = Uint8(opacity);
        }
        if (SDL_SetTextureBlendMode(texture, blend) != 0 ||
            SDL_SetTextureColorMod(texture, color, color, color) != 0 ||
            SDL_SetTextureAlphaMod(texture, Uint8(opacity)) != 0) return false;
        if (!BeginTarget(target.target)) return false;
        // Mark before executing: even a failed partially executed submission
        // must synchronize its current GPU pixels before any CPU fallback.
        target.cpuStale = true;
        target.rgbaValid = target.opaqueValid = false;
        KrkrNSPerfScope perf(KrkrNSPerfOp::GpuCanvasDraw,
            std::uint64_t(to.w) * to.h * 4);
        const bool good = SDL_RenderCopy(renderer, texture, &from, &to) == 0;
        if (!good) { EndTarget(); throw SubmittedGPUFailure(); }
        EndPrimitive();
        // Once a draw was attempted, neither a driver error nor a failure to
        // restore SDL state proves that no pixels were written. Returning
        // false here would replay Alpha/AddAlpha over those pixels on CPU.
        return true;
    }
};

ResidentFrame::ResidentFrame(std::shared_ptr<ResidentFrameState> state, bool snapshot)
    : state_(std::move(state))
{
    if (snapshot) snapshot_ = std::make_shared<ResidentFrameSnapshot>(state_);
}
SDL_Texture* ResidentFrame::Texture() const noexcept
{
    return state_ && state_->epoch->alive.load(std::memory_order_acquire) &&
        SDL_ThreadID() == state_->epoch->owner ? state_->texture.load() : nullptr;
}
int ResidentFrame::Width() const noexcept { return state_ ? state_->width : 0; }
int ResidentFrame::Height() const noexcept { return state_ ? state_->height : 0; }

SDLBitmapCompositor::SDLBitmapCompositor() = default;
SDLBitmapCompositor::~SDLBitmapCompositor()
{
    // Normal teardown calls ResetRenderer before SDL destroys its context.
    // Never issue late SDL calls from static destruction after that context.
    if (impl_ && impl_->epoch) {
        if (impl_->drawScope) impl_->drawScope->Abandon();
        std::lock_guard<std::mutex> lock(impl_->epoch->mutex);
        impl_->epoch->alive.store(false);
        for (auto* state = impl_->epoch->resources; state; state = state->allNext)
            state->texture.store(nullptr);
    }
}

bool SDLBitmapCompositor::BindRenderer(SDL_Renderer* renderer)
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    if (!renderer) return ResetRenderer();
    if (impl_ && impl_->renderer == renderer) return impl_->Owner();
    if (impl_ && !ResetRenderer()) return false;
    try {
        auto next = std::make_unique<Impl>();
        if (SDL_GetRendererInfo(renderer, &next->info) != 0 ||
            !(next->info.flags & SDL_RENDERER_TARGETTEXTURE) ||
            !(next->info.flags & SDL_RENDERER_ACCELERATED)) return false;
        next->renderer = renderer;
        next->epoch = std::make_shared<RendererEpoch>();
        next->epoch->renderer = renderer; next->epoch->owner = SDL_ThreadID();
        impl_ = std::move(next);
        return true;
    } catch (...) { return false; }
}

bool SDLBitmapCompositor::ResetRenderer()
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    if (!impl_) return true;
    if (!impl_->Owner()) return false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        try { impl_->EndTarget(); } catch (...) { return false; }
        for (auto& item : impl_->entries) if (!impl_->Sync(item.second)) return false;
        DrainRetired(impl_->epoch);
        {
            std::lock_guard<std::mutex> epochLock(impl_->epoch->mutex);
            for (auto* state = impl_->epoch->resources; state; state = state->allNext)
                DestroyTexture(*impl_->epoch, *state);
            impl_->epoch->alive.store(false, std::memory_order_release);
        }
        // A worker may have retired a frame between the first drain and the
        // epoch lock. Once alive=false no new deferred retirement is possible.
        DrainRetired(impl_->epoch);
    }
    impl_.reset();
    return true;
}

void SDLBitmapCompositor::DiscardRenderer() noexcept
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const bool owner = SDL_ThreadID() == impl_->epoch->owner;
        if (owner) { try { impl_->EndTarget(); } catch (...) {} }
        // Lost-device/foreign-thread teardown cannot restore a live renderer.
        // Invalidate the retained scope before its destructor can call SDL.
        if (impl_->drawScope) impl_->drawScope->Abandon();
        if (owner) DrainRetired(impl_->epoch);
        {
            std::lock_guard<std::mutex> epochLock(impl_->epoch->mutex);
            for (auto* state = impl_->epoch->resources; state; state = state->allNext) {
                if (owner) DestroyTexture(*impl_->epoch, *state);
                else state->texture.store(nullptr);
            }
            impl_->epoch->alive.store(false, std::memory_order_release);
        }
        // All retired textures have been destroyed (or left owned by SDL for
        // SDL_DestroyRenderer on its thread). This drain only unlinks nodes.
        DrainRetired(impl_->epoch);
    }
    impl_.reset();
}

SDL_Renderer* SDLBitmapCompositor::Renderer() const noexcept
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    return impl_ ? impl_->renderer : nullptr;
}

std::uint64_t SDLBitmapCompositor::LogicalTextureBytes() const noexcept
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    return impl_ ? impl_->epoch->bytes.load(std::memory_order_relaxed) : 0;
}

bool SDLBitmapCompositor::SetOpaqueTarget(const BitmapView& view)
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    if (!impl_ || !impl_->Owner() || view.volatileCPU) return false;
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto* entry = impl_->Get(view);
        if (!entry) return false;
        entry->opaqueTarget = true;
        return true;
    } catch (...) { return false; }
}

bool SDLBitmapCompositor::IsOpaqueTarget(const BitmapView& view) const noexcept
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto* entry = impl_->Find(view);
    return entry && entry->opaqueTarget && !view.volatileCPU;
}

bool SDLBitmapCompositor::HasGPUWrites(const BitmapView& view) const noexcept
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto* entry = impl_->Find(view);
    return entry && entry->cpuStale;
}

bool SDLBitmapCompositor::BeginBatch()
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    if (!impl_ || !impl_->Owner()) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->EndTarget();
    ++impl_->batchDepth;
    return true;
}

void SDLBitmapCompositor::EndBatch()
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    if (!impl_ || !impl_->Owner()) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->batchDepth) --impl_->batchDepth;
    impl_->EndTarget();
}

bool SDLBitmapCompositor::TryCopy(const BitmapView& target, int x, int y,
    const BitmapView& source, const SDL_Rect& rect)
{
    return TryBlt(target, x, y, source, rect, BitmapBlend::Copy, 255, false);
}

bool SDLBitmapCompositor::TryBlt(const BitmapView& target, int x, int y,
    const BitmapView& source, const SDL_Rect& rect, BitmapBlend method, int opacity, bool holdAlpha)
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    if (!impl_ || !impl_->Owner() || !ValidView(target) || !ValidView(source) ||
        Identity(target) == Identity(source) || opacity <= 0 || opacity > 255 ||
        (method != BitmapBlend::Copy && method != BitmapBlend::Alpha &&
         method != BitmapBlend::AddAlpha)) return false;
    SDL_Rect from{}, to{};
    if (!ClipCopy(target, x, y, source, rect, from, to)) return false;
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto* destination = impl_->Find(target);
        if (!destination || !destination->opaqueTarget) return false;
        destination = impl_->Get(target);
        auto* input = impl_->Get(source);
        if (!destination || !input) return false;
        auto texture = impl_->Source(*input, method == BitmapBlend::Copy && opacity < 255);
        return texture && impl_->Draw(*destination, texture, from, to, method, opacity, holdAlpha);
    } catch (const SubmittedGPUFailure&) { throw; }
      catch (...) { return false; }
}

bool SDLBitmapCompositor::TryFill(const BitmapView& target, const SDL_Rect& rect, std::uint32_t color)
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    if (!impl_ || !impl_->Owner() || !ValidView(target)) return false;
    SDL_Rect clipped{}, bounds{0, 0, target.width, target.height};
    if (!SDL_IntersectRect(&rect, &bounds, &clipped)) return false;
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto* entry = impl_->Find(target);
        if (!entry || !entry->opaqueTarget) return false;
        entry = impl_->Get(target);
        if (!entry || !impl_->Target(*entry, true)) return false;
        if (!impl_->BeginTarget(entry->target) ||
            SDL_SetRenderDrawBlendMode(impl_->renderer, SDL_BLENDMODE_NONE) != 0 ||
            SDL_SetRenderDrawColor(impl_->renderer, Uint8(color >> 16), Uint8(color >> 8),
                                  Uint8(color), Uint8(color >> 24)) != 0) {
            impl_->EndTarget();
            return false;
        }
        entry->cpuStale = true;
        entry->rgbaValid = entry->opaqueValid = false;
        KrkrNSPerfScope perf(KrkrNSPerfOp::GpuCanvasDraw,
            std::uint64_t(clipped.w) * clipped.h * 4);
        const bool good = SDL_RenderFillRect(impl_->renderer, &clipped) == 0;
        if (!good) { impl_->EndTarget(); throw SubmittedGPUFailure(); }
        impl_->EndPrimitive();
        return true;
    } catch (const SubmittedGPUFailure&) { throw; }
      catch (...) { return false; }
}

bool SDLBitmapCompositor::SyncCPU(const BitmapView& view)
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    if (!impl_) return true;
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto* entry = impl_->Find(view);
        if (!entry || !entry->cpuStale) return true;
        if (!impl_->Owner() || entry->view.row0 != view.row0 ||
            entry->view.width != view.width || entry->view.height != view.height) return false;
        impl_->EndTarget();
        return impl_->Sync(*entry);
    } catch (...) { return false; }
}

void SDLBitmapCompositor::CPUWritten(const BitmapView& view) noexcept
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (auto* entry = impl_->Find(view)) {
        entry->view = view;
        entry->cpuStale = false;
        entry->targetValid = entry->rgbaValid = entry->opaqueValid = false;
    }
}

void SDLBitmapCompositor::Forget(const void* storage, std::uint64_t serial) noexcept
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->Owner()) {
        auto found = impl_->entries.find(Key{storage, serial});
        if (found != impl_->entries.end() && found->second.target &&
            impl_->drawTarget == found->second.target->texture.load())
            try { impl_->EndTarget(); } catch (...) {}
    }
    impl_->entries.erase(Key{storage, serial});
}

ResidentFrame SDLBitmapCompositor::GetResidentFrame(const BitmapView& view)
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    if (!impl_ || !impl_->Owner()) return {};
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->EndTarget();
        auto* entry = impl_->Find(view);
        if (!entry || !entry->opaqueTarget) return {};
        entry = impl_->Get(view);
        if (!entry || !impl_->Target(*entry, false)) return {};
        auto* texture = entry->target->texture.load();
        if (SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE) != 0 ||
            SDL_SetTextureColorMod(texture, 255, 255, 255) != 0 ||
            SDL_SetTextureAlphaMod(texture, 255) != 0) return {};
        return ResidentFrame(entry->target, true);
    } catch (const SubmittedGPUFailure&) { throw; }
      catch (...) { return {}; }
}

ResidentFrame SDLBitmapCompositor::GetResidentCanvas(const BitmapView& view)
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    if (!impl_ || !impl_->Owner()) return {};
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto* entry = impl_->Find(view);
        if (!entry || !entry->opaqueTarget) return {};
        entry = impl_->Get(view);
        if (!entry || !impl_->Target(*entry, false)) return {};
        return ResidentFrame(entry->target, false);
    } catch (const SubmittedGPUFailure&) { throw; }
      catch (...) { return {}; }
}

bool SDLBitmapCompositor::ReadFrame(const ResidentFrame& frame, SDL_Surface* surface)
{
    std::lock_guard<std::recursive_mutex> lifetimeLock(mutex_);
    if (!impl_ || !impl_->Owner() || !frame.state_ || !surface ||
        frame.state_->epoch != impl_->epoch || frame.Width() != surface->w ||
        frame.Height() != surface->h || surface->format->BytesPerPixel != 4 ||
        (surface->format->format != SDL_PIXELFORMAT_RGB888 &&
         surface->format->format != SDL_PIXELFORMAT_ARGB8888)) return false;
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const bool locked = SDL_MUSTLOCK(surface);
        if (locked && SDL_LockSurface(surface) != 0) return false;
        struct Unlock { SDL_Surface* surface; bool locked;
            ~Unlock() { if (locked) SDL_UnlockSurface(surface); } } unlock{surface, locked};
        return impl_->Read(frame.state_, static_cast<std::uint8_t*>(surface->pixels),
            surface->pitch, surface->format->format);
    } catch (...) { return false; }
}

SDLBitmapCompositor& GetBitmapCompositor()
{
    // Global bitmap caches can release storage during static destruction.
    // Keep this small facade/mutex alive; real GPU allocations are released
    // by the renderer owner's explicit ResetRenderer/DiscardRenderer calls.
    static SDLBitmapCompositor* const compositor = new SDLBitmapCompositor;
    return *compositor;
}
}
