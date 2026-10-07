/* SPDX-License-Identifier: MIT */
#pragma once

#include "SDLBitmapBridge.h"

// Texture contents survive SDL_RenderPresent; the backbuffer does not.
// This state only suppresses an upload when no writer touched the CPU surface.
// It never suppresses rendering or tries a partial upload on its own.
class TVPSDLTextureUploadState
{
    bool contentValid = false;

public:
    void Invalidate() { contentValid = false; }

    int Upload(SDL_Renderer* renderer, SDL_Texture* texture, SDL_Surface* surface,
               SDL_Rect& uploadRect, bool surfaceWritten)
    {
        if (!renderer || !texture || !surface)
        {
            contentValid = false;
            return SDL_SetError("Invalid KiriKiri texture upload state");
        }
        if (contentValid && !surfaceWritten)
        {
            uploadRect = {0, 0, 0, 0};
            return 0;
        }
        if (!contentValid && (uploadRect.w <= 0 || uploadRect.h <= 0))
            uploadRect = {0, 0, surface->w, surface->h};

        // An empty/clipped-away update must not initialize a new texture.
        int width = 0, height = 0;
        if (SDL_QueryTexture(texture, nullptr, nullptr, &width, &height) != 0)
        {
            contentValid = false;
            return -1;
        }
        const SDL_Rect bounds = {0, 0, std::min(width, surface->w),
                                std::min(height, surface->h)};
        SDL_Rect clipped;
        if (!SDL_IntersectRect(&uploadRect, &bounds, &clipped))
        {
            uploadRect = {0, 0, 0, 0};
            return 0;
        }
        uploadRect = clipped;
        const int result = TVPUploadDirtySurface(renderer, texture, surface, uploadRect);
        // An overlay writer can consume its decoded frame before uploading.
        // A failed transfer must be retried even if the next frame has no new
        // engine damage or movie frame notification.
        contentValid = result == 0;
        return result;
    }
};
