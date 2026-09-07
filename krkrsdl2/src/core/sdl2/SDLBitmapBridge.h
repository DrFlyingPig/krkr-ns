/* SPDX-License-Identifier: MIT */
#pragma once

#include <SDL.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

// KiriKiri DIBs use positive height for bottom-up storage, negative height
// for top-down storage. SDL surfaces always address their logical first row.
// Keep this boundary independent of TJS so both layouts can be pixel-tested.
inline bool TVPCopyBitmapToSurface(SDL_Surface* surface, const void* bits,
                                  int bitmapWidth, int bitmapHeight,
                                  SDL_Rect source, int x, int y,
                                  SDL_Rect& copied)
{
    copied = {0, 0, 0, 0};
    const int64_t height = bitmapHeight < 0 ? -int64_t(bitmapHeight) : bitmapHeight;
    if (!surface || !bits || !surface->format || surface->format->BytesPerPixel != 4 ||
        bitmapWidth <= 0 || height <= 0 || height > std::numeric_limits<int>::max() ||
        source.x < 0 || source.y < 0 || source.w <= 0 || source.h <= 0 ||
        int64_t(source.x) + source.w > bitmapWidth ||
        int64_t(source.y) + source.h > height)
        return false;

    const SDL_Rect requested = {x, y, source.w, source.h};
    const SDL_Rect bounds = {0, 0, surface->w, surface->h};
    if (!SDL_IntersectRect(&requested, &bounds, &copied))
        return false;
    source.x += copied.x - x;
    source.y += copied.y - y;
    const std::ptrdiff_t stride = std::ptrdiff_t(bitmapWidth) * 4;
    const auto* top = static_cast<const uint8_t*>(bits);
    const std::ptrdiff_t pitch = bitmapHeight < 0 ? stride : -stride;
    if (bitmapHeight > 0)
        top += (height - 1) * stride;

    const bool locked = SDL_MUSTLOCK(surface);
    if (locked && SDL_LockSurface(surface) != 0)
        return false;
    for (int row = 0; row < copied.h; ++row)
    {
        const auto* src = top + std::ptrdiff_t(source.y + row) * pitch +
                          std::ptrdiff_t(source.x) * 4;
        auto* dst = static_cast<uint8_t*>(surface->pixels) +
                    std::ptrdiff_t(copied.y + row) * surface->pitch +
                    std::ptrdiff_t(copied.x) * 4;
        SDL_memcpy(dst, src, size_t(copied.w) * 4);
    }
    if (locked)
        SDL_UnlockSurface(surface);
    return true;
}

// SDL_UpdateTexture's pointer is the first pixel of the UPDATE RECTANGLE,
// not the first pixel of the entire source surface. Retain the full row pitch.
inline int TVPUploadDirtySurface(SDL_Renderer* renderer, SDL_Texture* texture,
                                 SDL_Surface* surface, SDL_Rect dirty)
{
    if (!renderer || !texture || !surface || !surface->format ||
        surface->format->BytesPerPixel != 4)
        return SDL_SetError("Invalid KiriKiri bitmap upload");
    int width = 0, height = 0;
    if (SDL_QueryTexture(texture, nullptr, nullptr, &width, &height) != 0)
        return -1;
    const SDL_Rect bounds = {0, 0, std::min(width, surface->w),
                            std::min(height, surface->h)};
    SDL_Rect clipped;
    if (!SDL_IntersectRect(&dirty, &bounds, &clipped))
        return 0;
    const bool locked = SDL_MUSTLOCK(surface);
    if (locked && SDL_LockSurface(surface) != 0)
        return -1;
    const auto* pixels = static_cast<const uint8_t*>(surface->pixels) +
                        std::ptrdiff_t(clipped.y) * surface->pitch +
                        std::ptrdiff_t(clipped.x) * 4;
    const int result = SDL_UpdateTexture(texture, &clipped, pixels, surface->pitch);
    if (locked)
        SDL_UnlockSurface(surface);
    return result;
}
