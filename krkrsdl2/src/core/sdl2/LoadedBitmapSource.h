/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstdint>
#include <memory>
#include <utility>

namespace krkrns
{
struct LoadedBitmapSourceState;
struct BitmapView;
using AllocateLoadedPixels = void* (*)(std::uint32_t bytes, std::uint32_t width,
                                      std::uint32_t height);
using FreeLoadedPixels = void (*)(void* pixels);

// Immutable loaded pixels own their source texture and exact recovery data.
// The cache and bitmap aliases share this handle; renderer entries only index
// it. GPU budget eviction may remove its texture, never its recovery pixels.
class LoadedBitmapSource
{
    std::shared_ptr<LoadedBitmapSourceState> state_;
    explicit LoadedBitmapSource(std::shared_ptr<LoadedBitmapSourceState> state)
        : state_(std::move(state)) {}
    friend class SDLBitmapCompositor;
public:
    LoadedBitmapSource() = default;
    explicit operator bool() const noexcept { return bool(state_); }
    // CPU restoration uses the supplied non-compacting allocator and performs
    // no renderer, script, file, or VM call. A published address stays stable.
    const void* CPUBuffer() const noexcept;
    const void* PeekCPUBuffer() const noexcept;
    bool Matches(const BitmapView& view) const noexcept;
    // Actual retained CPU pixel/recovery capacity, excluding control metadata.
    std::uint64_t CPUBackingBytes() const noexcept;
    // Only the initial, unexposed decode buffer may be dropped. A mirror
    // returned by CPUBuffer() is permanent for this immutable generation.
    bool ReleaseDecodedCPU() noexcept;
};
}
