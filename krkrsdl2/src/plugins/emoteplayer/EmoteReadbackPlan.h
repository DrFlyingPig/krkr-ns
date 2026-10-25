/* SPDX-License-Identifier: MIT */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace krkrns_emote_readback
{
constexpr int TileSize = 16;
constexpr int MaximumTargetDimension = 4096;

struct Rect
{
    int x, y, w, h;
};

struct Plan
{
    std::vector<Rect> rects;
    std::size_t readCalls = 0;
    bool boundingFallback = false;
};

// The comparison target has one RGBA8 pixel per 16x16 target-pixel tile.
// Only its red channel signals damage. Coordinates have the same row origin
// as the readback buffer: the GL caller performs any required image flip.
// readTileWidth == 0 means no driver-width restriction. A fragmented map uses
// one bounding rectangle; hardware-width splitting may exceed the soft budget.
// Failure leaves out empty so the caller can use its full-target fallback.
inline bool BuildPlan(const std::uint8_t* flags, std::size_t flagsBytes,
                      std::size_t flagsPitch, int targetWidth, int targetHeight,
                      int readTileWidth, Plan& out, std::size_t maxReadCalls = 8)
{
    out = Plan{};
    if (!flags || targetWidth <= 0 || targetHeight <= 0 ||
        targetWidth > MaximumTargetDimension || targetHeight > MaximumTargetDimension ||
        readTileWidth < 0 || maxReadCalls == 0)
        return false;

    const int columns = (targetWidth + TileSize - 1) / TileSize;
    const int rows = (targetHeight + TileSize - 1) / TileSize;
    const std::size_t rowBytes = std::size_t(columns) * 4;
    if (flagsPitch < rowBytes ||
        (rows > 1 && flagsPitch > (std::numeric_limits<std::size_t>::max() - rowBytes) /
                                   std::size_t(rows - 1)))
        return false;
    const std::size_t required = std::size_t(rows - 1) * flagsPitch + rowBytes;
    if (flagsBytes < required) return false;

    auto callsForWidth = [readTileWidth](int width) -> std::size_t {
        return readTileWidth ? std::size_t((width - 1) / readTileWidth) + 1 : 1;
    };
    struct Run { int first, last; std::size_t rectangle; };
    try
    {
        Plan plan;
        std::vector<Run> previous, current;
        previous.reserve(std::size_t(columns + 1) / 2);
        current.reserve(std::size_t(columns + 1) / 2);
        int minimumX = columns, minimumY = rows, maximumX = 0, maximumY = 0;

        for (int y = 0; y < rows; ++y)
        {
            const auto* row = flags + std::size_t(y) * flagsPitch;
            current.clear();
            std::size_t previousPosition = 0;
            for (int x = 0; x < columns;)
            {
                if (row[std::size_t(x) * 4] == 0) { ++x; continue; }
                const int first = x;
                do { ++x; } while (x < columns && row[std::size_t(x) * 4] != 0);
                const int last = x;
                minimumX = std::min(minimumX, first);
                minimumY = std::min(minimumY, y);
                maximumX = std::max(maximumX, last);
                maximumY = y + 1;
                if (plan.boundingFallback) continue;

                while (previousPosition < previous.size() &&
                       previous[previousPosition].first < first) ++previousPosition;
                if (previousPosition < previous.size() &&
                    previous[previousPosition].first == first &&
                    previous[previousPosition].last == last)
                {
                    const std::size_t index = previous[previousPosition].rectangle;
                    plan.rects[index].h = std::min(targetHeight, (y + 1) * TileSize) -
                                         plan.rects[index].y;
                    current.push_back({first, last, index});
                    continue;
                }

                const int pixelX = first * TileSize;
                const int width = std::min(targetWidth, last * TileSize) - pixelX;
                const std::size_t calls = callsForWidth(width);
                if (calls > maxReadCalls || plan.readCalls > maxReadCalls - calls)
                {
                    plan.boundingFallback = true;
                    plan.rects.clear();
                    previous.clear();
                    current.clear();
                    continue;
                }
                const int pixelY = y * TileSize;
                current.push_back({first, last, plan.rects.size()});
                plan.rects.push_back({pixelX, pixelY, width,
                                     std::min(TileSize, targetHeight - pixelY)});
                plan.readCalls += calls;
            }
            previous.swap(current);
        }
        if (plan.boundingFallback)
        {
            const int x = minimumX * TileSize;
            const int y = minimumY * TileSize;
            const int width = std::min(targetWidth, maximumX * TileSize) - x;
            plan.rects.push_back({x, y, width,
                                  std::min(targetHeight, maximumY * TileSize) - y});
            plan.readCalls = callsForWidth(width);
        }
        out = std::move(plan);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
}
} // namespace krkrns_emote_readback
