/* SPDX-License-Identifier: MIT */
#pragma once

#include <cstdint>

namespace krkrsdl3
{
// Identifies exactly the portion of a Layer last synchronized by a target.
// Volatile or GPU-written bitmaps must never be passed as valid identities.
struct EmoteLayerIdentity
{
    const void* storage = nullptr;
    const void* row0 = nullptr;
    std::uint64_t serial = 0, version = 0;
    int width = 0, height = 0, pitch = 0;

    bool operator==(const EmoteLayerIdentity& other) const noexcept
    {
        return storage == other.storage && row0 == other.row0 &&
            serial == other.serial && version == other.version &&
            width == other.width && height == other.height && pitch == other.pitch;
    }
    explicit operator bool() const noexcept
        { return storage && row0 && serial && version && width > 0 && height > 0; }
};

struct EmoteLayerBounds { int left = 0, top = 0, right = 0, bottom = 0; };

// Owned by the render target, so target destruction and address reuse cannot
// accidentally resurrect an earlier layer synchronization.
class EmoteLayerDamage
{
    std::uint64_t lock_ = 0, synchronizedLock_ = 0;
    EmoteLayerIdentity synchronized_{};
    EmoteLayerBounds bounds_{};
    bool damageKnown_ = false;
public:
    void BeginLock() noexcept
    {
        if (++lock_ == 0) { lock_ = 1; synchronizedLock_ = 0; }
        damageKnown_ = false;
        bounds_ = {};
    }
    void SetDamage(const EmoteLayerBounds& bounds) noexcept
        { bounds_ = bounds; damageKnown_ = true; }
    bool GetDamage(const EmoteLayerIdentity& identity, EmoteLayerBounds& bounds) const noexcept
    {
        if (!identity || !damageKnown_ || !synchronizedLock_ ||
            synchronizedLock_ != lock_ - 1 || !(identity == synchronized_)) return false;
        bounds = bounds_;
        return true;
    }
    void Commit(const EmoteLayerIdentity& identity) noexcept
    {
        synchronized_ = identity;
        synchronizedLock_ = identity ? lock_ : 0;
    }
    void Invalidate() noexcept { synchronizedLock_ = 0; damageKnown_ = false; }
};
} // namespace krkrsdl3
