/* SPDX-License-Identifier: MIT */
#pragma once
#include <SDL.h>
#include <cstdint>

namespace krkrns {
// Owns the launcher's native SDL pair during game startup. Switch permits
// only one native window, so it must be released before creating the game
// window. No script object or compositor owns this pair after adoption.
class StartupConsoleNativeHost {
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    std::uint64_t generation_ = 0;
    bool releaseAfterDraw_ = false;
    bool releasing_ = false;
public:
    StartupConsoleNativeHost() = default;
    StartupConsoleNativeHost(const StartupConsoleNativeHost&) = delete;
    StartupConsoleNativeHost& operator=(const StartupConsoleNativeHost&) = delete;

    bool Adopt(SDL_Window*& window, SDL_Renderer*& renderer, bool active, bool drawing) noexcept {
        if (!active || drawing || releasing_ || window_ || renderer_ || !window || !renderer) return false;
        window_ = window;
        renderer_ = renderer;
        window = nullptr;
        renderer = nullptr;
        releaseAfterDraw_ = false;
        ++generation_;
        return true;
    }
    bool Release(bool drawing) noexcept {
        if (releasing_) return false;
        if (!window_ && !renderer_) return true;
        if (drawing) { releaseAfterDraw_ = true; return false; }
        releasing_ = true;
        SDL_Renderer* renderer = renderer_;
        SDL_Window* window = window_;
        renderer_ = nullptr;
        window_ = nullptr;
        releaseAfterDraw_ = false;
        ++generation_;
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        releasing_ = false;
        return true;
    }
    void EndDraw() noexcept { if (releaseAfterDraw_) Release(false); }
    SDL_Window* Window() const noexcept { return window_; }
    SDL_Renderer* Renderer() const noexcept { return renderer_; }
    std::uint64_t Generation() const noexcept { return generation_; }
    bool Matches(SDL_Window* window, SDL_Renderer* renderer, std::uint64_t generation) const noexcept {
        return window_ == window && renderer_ == renderer && generation_ == generation && !releaseAfterDraw_;
    }
};
}
