/* SPDX-License-Identifier: MIT */
#pragma once

#include "tjsCommHead.h"
#include "BinaryStream.h"
#include <cstdint>
#include <vector>

namespace krkrns_launcher_artwork {
namespace raster_detail {

inline std::uint32_t ReadLE32(const unsigned char *data)
{
    return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) |
        (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
}

inline unsigned ReadLE16(const unsigned char *data)
{
    return unsigned(data[0]) | (unsigned(data[1]) << 8);
}

[[noreturn]] inline void InvalidBMP()
{
    throw eTJSError(TJS_W("Artwork BMP header, palette or pixel data is invalid or excessive"));
}

} // namespace raster_detail

// Validate only the BMP forms understood by the existing native loader. Its
// palette allocation and fixed 256-entry palette precede the size callback.
// This private guard restores the caller's initial stream position. Partial
// palettes additionally require each pixel index to name a supplied colour.
inline void PreflightArtworkBMP(tTJSBinaryStream &source)
{
    using namespace raster_detail;
    const tjs_uint64 origin = source.GetPosition();
    try {
        const tjs_uint64 total = source.GetSize();
        if (origin > total || total - origin < 26 || total - origin > 32ULL * 1024 * 1024)
            InvalidBMP();
        const tjs_uint64 available = total - origin;
        unsigned char header[54] = {};
        source.ReadBuffer(header, 26);
        if (header[0] != 'B' || header[1] != 'M') InvalidBMP();
        const std::uint32_t declaredSize = ReadLE32(header + 2), offset = ReadLE32(header + 10);
        const std::uint32_t dibSize = ReadLE32(header + 14);
        tjs_uint64 width = 0, height = 0;
        unsigned planes = 0, bits = 0, paletteCount = 0, paletteEntryBytes = 0;
        if (dibSize == 12) {
            width = ReadLE16(header + 18);
            height = ReadLE16(header + 20);
            planes = ReadLE16(header + 22);
            bits = ReadLE16(header + 24);
            paletteEntryBytes = 3;
        } else if (dibSize == 40) {
            if (available < sizeof header) InvalidBMP();
            source.ReadBuffer(header + 26, 28);
            width = ReadLE32(header + 18);
            const std::uint32_t signedHeight = ReadLE32(header + 22);
            height = (signedHeight & 0x80000000U) ? 0x100000000ULL - signedHeight : signedHeight;
            planes = ReadLE16(header + 26);
            bits = ReadLE16(header + 28);
            if (ReadLE32(header + 30) != 0) InvalidBMP(); // BI_RGB only
            paletteCount = ReadLE32(header + 46);
            paletteEntryBytes = 4;
        } else InvalidBMP();

        if (planes != 1 || !width || !height || width > 16384 || height > 16384 ||
            width * height > 16ULL * 1024 * 1024 ||
            !(bits == 1 || bits == 4 || bits == 8 || bits == 16 || bits == 24 || bits == 32))
            InvalidBMP();
        unsigned fullPalette = 0;
        if (bits <= 8) {
            fullPalette = 1U << bits;
            if (!paletteCount) paletteCount = fullPalette;
            if (paletteCount > fullPalette || paletteCount > 256) InvalidBMP();
        } else {
            // This launcher accepts true-colour BMPs without an optional
            // optimization palette; the native loader ignores that palette.
            if (paletteCount) InvalidBMP();
            paletteCount = 0;
        }
        const tjs_uint64 paletteEnd = 14ULL + dibSize + tjs_uint64(paletteCount) * paletteEntryBytes;
        const tjs_uint64 pitch = ((width * bits + 31) / 32) * 4;
        const tjs_uint64 pixelBytes = pitch * height;
        if (offset < paletteEnd || offset > available || pixelBytes > available - offset ||
            (declaredSize && (declaredSize > available || declaredSize < tjs_uint64(offset) + pixelBytes)))
            InvalidBMP();

        if (fullPalette && paletteCount < fullPalette) {
            std::vector<unsigned char> row(static_cast<std::size_t>(pitch));
            source.SetPosition(origin + offset);
            for (tjs_uint64 y = 0; y < height; ++y) {
                source.ReadBuffer(row.data(), static_cast<tjs_uint>(row.size()));
                for (tjs_uint64 x = 0; x < width; ++x) {
                    const unsigned index = bits == 8 ? row[static_cast<std::size_t>(x)] :
                        bits == 4 ? (row[static_cast<std::size_t>(x / 2)] >> ((x & 1) ? 0 : 4)) & 15 :
                        (row[static_cast<std::size_t>(x / 8)] >> (7 - (x & 7))) & 1;
                    if (index >= paletteCount) InvalidBMP();
                }
            }
        }
        source.SetPosition(origin);
    } catch (...) {
        try { source.SetPosition(origin); } catch (...) {}
        throw;
    }
}

} // namespace krkrns_launcher_artwork
