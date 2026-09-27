#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace krkrns {

// Pixel semantics from krkrsdl3/plugins/LayerExRaster.cpp. Rows which shift
// entirely outside the destination and uncovered edge pixels stay unchanged.
inline bool CopyRaster(std::uint8_t* destination, std::ptrdiff_t destinationPitch,
                       const std::uint8_t* source, std::ptrdiff_t sourcePitch,
                       int width, int height, int amplitude, int lines,
                       int cycle, std::int64_t time)
{
    if (!destination || !source || width <= 0 || height <= 0 || !lines || !cycle)
        return false;
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    const auto pitchBytes = [](std::ptrdiff_t pitch) -> std::size_t {
        return pitch < 0 ? static_cast<std::size_t>(-(pitch + 1)) + 1
                         : static_cast<std::size_t>(pitch);
    };
    if (pitchBytes(destinationPitch) < rowBytes || pitchBytes(sourcePitch) < rowBytes)
        return false;

    constexpr double pi = 3.14159265358979323846;
    const double omega = 2 * pi / lines;
    // height/2 intentionally uses integer division, as in the reference.
    double phase = -omega * time / cycle * (height / 2);
    for (int y = 0; y < height; ++y, phase += omega) {
        const double displacement = std::sin(phase) * amplitude;
        // Check before converting to int: extreme amplitudes must not cause
        // conversion overflow or construct pointers outside a scanline.
        if (displacement >= width || displacement <= -static_cast<double>(width))
            continue;
        const int shift = static_cast<int>(displacement); // truncation toward zero
        const int sourceX = shift < 0 ? -shift : 0;
        const int destinationX = shift > 0 ? shift : 0;
        const int count = width - (shift < 0 ? -shift : shift);
        const std::uint8_t* sourceRow = source + static_cast<std::ptrdiff_t>(y) * sourcePitch;
        std::uint8_t* destinationRow = destination + static_cast<std::ptrdiff_t>(y) * destinationPitch;
        // memmove also makes an in-place raster copy safe in either direction.
        std::memmove(destinationRow + static_cast<std::size_t>(destinationX) * 4,
                     sourceRow + static_cast<std::size_t>(sourceX) * 4,
                     static_cast<std::size_t>(count) * 4);
    }
    return true;
}

} // namespace krkrns
