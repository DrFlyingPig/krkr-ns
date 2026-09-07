#include "SDLBitmapBridge.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

static int checks = 0;
static void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s (%s)\n", message, SDL_GetError()); std::exit(1); }
}

int main()
{
    SDL_SetMainReady();
    check(SDL_Init(0) == 0, "SDL init");
    constexpr int w = 7, h = 5, pitch = 40;
    std::vector<uint8_t> destination(pitch * h + 32, 0xAD);
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(destination.data(), w, h, 32, pitch, SDL_PIXELFORMAT_ARGB8888);
    check(surface != nullptr, "padded destination surface");
    std::vector<uint32_t> top(w * h), bottom(w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            top[y * w + x] = bottom[(h - y - 1) * w + x] = 0xff000000u | (y * 37u << 16) | (x * 23u << 8) | (x + y);
    for (int sign : {-1, 1})
    {
        SDL_Rect copied;
        check(TVPCopyBitmapToSurface(surface, sign < 0 ? top.data() : bottom.data(), w, sign * h,
                                    {0, 0, w, h}, 0, 0, copied), "DIB full copy");
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                check(reinterpret_cast<uint32_t*>(destination.data() + y * pitch)[x] == top[y * w + x], "DIB orientation and channels");
        for (int y = 0; y < h; ++y)
            for (int i = w * 4; i < pitch; ++i)
                check(destination[y * pitch + i] == 0xAD, "row padding preserved");
        check(TVPCopyBitmapToSurface(surface, sign < 0 ? top.data() : bottom.data(), w, sign * h,
                                    {2, 1, 3, 2}, 5, 4, copied), "destination clipped safely");
        check(copied.w == 2 && copied.h == 1, "reported actual damage");
        check(reinterpret_cast<uint32_t*>(destination.data() + 4 * pitch)[5] == top[w + 2], "source offset preserved");
        check(!TVPCopyBitmapToSurface(surface, top.data(), w, sign * h, {6, 0, 2, 1}, 0, 0, copied), "reject invalid source rectangle");
    }
    for (size_t i = pitch * h; i < destination.size(); ++i)
        check(destination[i] == 0xAD, "allocation guard preserved");

    SDL_Surface* screen = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(screen);
    check(renderer != nullptr, "software renderer");
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    check(texture != nullptr, "streaming texture");
    SDL_FillRect(surface, nullptr, 0xff101010);
    check(TVPUploadDirtySurface(renderer, texture, surface, {0, 0, w, h}) == 0, "initial upload");
    SDL_Rect patch = {3, 2, 3, 2};
    SDL_FillRect(surface, &patch, 0xffe03080);
    check(TVPUploadDirtySurface(renderer, texture, surface, patch) == 0, "offset upload");
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    std::vector<uint32_t> rendered(w * h);
    check(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, rendered.data(), w * 4) == 0, "read back texture");
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            check(rendered[y * w + x] == (x >= 3 && x < 6 && y >= 2 && y < 4 ? 0xffe03080u : 0xff101010u), "local update changes only requested pixels");
    check(TVPUploadDirtySurface(renderer, texture, surface, {6, 4, 100, 100}) == 0, "upload clipped to remaining width and height");
    check(TVPUploadDirtySurface(renderer, texture, surface, {-5, -5, 1, 1}) == 0, "empty upload is harmless");
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_FreeSurface(screen);
    SDL_FreeSurface(surface);
    SDL_Quit();
    std::printf("PASS: %d bitmap boundary checks\n", checks);
}
