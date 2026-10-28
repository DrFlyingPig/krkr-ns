/* SPDX-License-Identifier: MIT */
#pragma once
#include <SDL.h>

// Console rendering is allowed only at SDL's default framebuffer boundary.
// Save every public renderer property changed by the temporary overlay.
class SDLStartupConsoleState {
    SDL_Renderer* renderer_;
    SDL_Texture* target_;
    SDL_Rect viewport_{}, clip_{};
    float scaleX_ = 1, scaleY_ = 1;
    int logicalW_ = 0, logicalH_ = 0;
    SDL_bool clipped_, integerScale_;
    Uint8 red_ = 0, green_ = 0, blue_ = 0, alpha_ = 0;
    SDL_BlendMode blend_ = SDL_BLENDMODE_NONE;
    bool restored_ = false;
public:
    explicit SDLStartupConsoleState(SDL_Renderer* renderer) : renderer_(renderer), target_(SDL_GetRenderTarget(renderer)) {
        SDL_RenderGetViewport(renderer_, &viewport_);
        SDL_RenderGetClipRect(renderer_, &clip_);
        clipped_ = SDL_RenderIsClipEnabled(renderer_);
        SDL_RenderGetScale(renderer_, &scaleX_, &scaleY_);
        SDL_RenderGetLogicalSize(renderer_, &logicalW_, &logicalH_);
        integerScale_ = SDL_RenderGetIntegerScale(renderer_);
        SDL_GetRenderDrawColor(renderer_, &red_, &green_, &blue_, &alpha_);
        SDL_GetRenderDrawBlendMode(renderer_, &blend_);
    }
    bool Begin() {
        if (target_) return false;
        return SDL_SetRenderTarget(renderer_, nullptr) == 0 &&
            SDL_RenderSetLogicalSize(renderer_, 0, 0) == 0 &&
            SDL_RenderSetIntegerScale(renderer_, SDL_FALSE) == 0 &&
            SDL_RenderSetScale(renderer_, 1, 1) == 0 &&
            SDL_RenderSetViewport(renderer_, nullptr) == 0 &&
            SDL_RenderSetClipRect(renderer_, nullptr) == 0 &&
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE) == 0 &&
            SDL_SetRenderDrawColor(renderer_, 8, 9, 11, 255) == 0;
    }
    bool Restore() noexcept {
        if (restored_) return true;
        bool good = SDL_SetRenderTarget(renderer_, target_) == 0;
        good = SDL_RenderSetLogicalSize(renderer_, logicalW_, logicalH_) == 0 && good;
        good = SDL_RenderSetIntegerScale(renderer_, integerScale_) == 0 && good;
        good = SDL_RenderSetScale(renderer_, scaleX_, scaleY_) == 0 && good;
        good = SDL_RenderSetViewport(renderer_, &viewport_) == 0 && good;
        good = SDL_RenderSetClipRect(renderer_, clipped_ ? &clip_ : nullptr) == 0 && good;
        good = SDL_SetRenderDrawBlendMode(renderer_, blend_) == 0 && good;
        good = SDL_SetRenderDrawColor(renderer_, red_, green_, blue_, alpha_) == 0 && good;
        restored_ = good;
        return good;
    }
    ~SDLStartupConsoleState() { Restore(); }
};
