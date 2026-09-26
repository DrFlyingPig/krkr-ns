/* SPDX-License-Identifier: MIT */
#pragma once

// Launcher-only validation before calling the unchanged engine TLG decoder.
// Field/token semantics follow visual/LoadTLG.cpp, tvpgl.c and SaveTLG6.cpp.
#include "tjsCommHead.h"
#include "BinaryStream.h"
#include "tjsError.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace krkrns_launcher_artwork {
namespace tlg_preflight {

constexpr std::uint64_t EncodedLimit = 32ULL * 1024 * 1024;
constexpr std::uint64_t PixelLimit = 16ULL * 1024 * 1024;
constexpr std::uint32_t SideLimit = 16384;

[[noreturn]] inline void Reject(const tjs_char *message)
{
    throw eTJSError(message);
}

struct Reader {
    tTJSBinaryStream &stream;
    std::uint64_t position;
    std::uint64_t end;

    std::uint64_t Remaining() const { return end - position; }
    void Require(std::uint64_t count) const
    {
        if (count > Remaining()) Reject(TJS_W("Truncated launcher artwork TLG"));
    }
    void Read(void *buffer, std::uint32_t count)
    {
        Require(count);
        if (count) stream.ReadBuffer(buffer, count);
        position += count;
    }
    std::uint8_t Byte()
    {
        std::uint8_t value;
        Read(&value, 1);
        return value;
    }
    std::uint32_t U32()
    {
        std::uint8_t b[4];
        Read(b, 4);
        return std::uint32_t(b[0]) | (std::uint32_t(b[1]) << 8) |
            (std::uint32_t(b[2]) << 16) | (std::uint32_t(b[3]) << 24);
    }
    void Skip(std::uint64_t count)
    {
        Require(count);
        stream.SetPosition(position + count);
        position += count;
    }
    std::vector<std::uint8_t> Bytes(std::uint32_t count)
    {
        Require(count); // Do not allocate from a length before checking the stream.
        std::vector<std::uint8_t> bytes(count);
        Read(bytes.data(), count);
        return bytes;
    }
};

inline void Dimensions(std::uint32_t width, std::uint32_t height)
{
    if (!width || !height || width > SideLimit || height > SideLimit ||
        std::uint64_t(width) * height > PixelLimit)
        Reject(TJS_W("Launcher artwork TLG dimensions exceed the limit"));
}

// Validate every slide token and its exact output count. TLG5 pixel channels
// need no output/dictionary copy. TLG6 filters also need their decoded values.
inline void Slide(const std::vector<std::uint8_t> &input,
                  std::uint64_t expected, bool filters)
{
    std::array<std::uint8_t, 4096> dictionary{};
    if (filters) {
        // Same initial byte dictionary as TVPLoadTLG6's paired 0x01010101 words.
        for (unsigned i = 0; i < 32; ++i)
            for (unsigned j = 0; j < 16; ++j)
                for (unsigned k = 0; k < 8; ++k)
                    dictionary[(i * 16 + j) * 8 + k] = k < 4 ? i : j;
    }
    std::size_t cursor = 0;
    std::uint64_t produced = 0;
    unsigned flags = 0, ring = 0;
    auto take = [&]() -> std::uint8_t {
        if (cursor == input.size()) Reject(TJS_W("Truncated launcher artwork TLG slide token"));
        return input[cursor++];
    };
    auto emit = [&](std::uint8_t value) {
        if (filters) {
            if (value >= 32) Reject(TJS_W("Invalid launcher artwork TLG6 filter"));
            dictionary[ring] = value;
            ring = (ring + 1) & 4095;
        }
    };
    while (cursor < input.size()) {
        if (((flags >>= 1) & 256) == 0) flags = take() | 0xff00;
        if (flags & 1) {
            const unsigned low = take(), high = take();
            unsigned match = low | ((high & 15) << 8);
            unsigned length = (high >> 4) + 3;
            if (length == 18) length += take();
            if (length > expected - produced)
                Reject(TJS_W("Launcher artwork TLG slide exceeds its output block"));
            if (filters) for (unsigned i = 0; i < length; ++i) {
                const auto value = dictionary[match];
                match = (match + 1) & 4095;
                emit(value);
            }
            produced += length;
        } else {
            const auto value = take();
            if (produced == expected)
                Reject(TJS_W("Launcher artwork TLG slide exceeds its output block"));
            emit(value);
            ++produced;
        }
    }
    if (produced != expected)
        Reject(TJS_W("Incomplete launcher artwork TLG output block"));
}

inline void TLG5(Reader &reader)
{
    const auto colors = reader.Byte();
    const auto width = reader.U32(), height = reader.U32(), blockHeight = reader.U32();
    Dimensions(width, height);
    if (colors != 3 && colors != 4)
        Reject(TJS_W("Unsupported launcher artwork TLG5 color count"));
    // The upstream division occurs before its dimension callback. Its channel
    // allocations use the full declared height, even for the final short block.
    if (!blockHeight || std::uint64_t(blockHeight) * width > PixelLimit)
        Reject(TJS_W("Invalid launcher artwork TLG5 block height"));
    const std::uint64_t capacity = std::uint64_t(blockHeight) * width;
    const std::uint32_t blocks = (height - 1) / blockHeight + 1;
    // The upstream loader ignores the advisory block-size table; preserve that
    // behavior and derive actual bounds from each channel's serialized length.
    reader.Skip(std::uint64_t(blocks) * 4);
    for (std::uint32_t y = 0; y < height; y += blockHeight) {
        const auto rows = blockHeight < height - y ? blockHeight : height - y;
        const std::uint64_t expected = std::uint64_t(rows) * width;
        for (unsigned channel = 0; channel < colors; ++channel) {
            const auto method = reader.Byte();
            const auto size = reader.U32();
            // Upstream's input/output allocations are capacity + 10 + 16.
            if (size > capacity + 26)
                Reject(TJS_W("Launcher artwork TLG5 channel exceeds its buffer"));
            if (method == 0) Slide(reader.Bytes(size), expected, false);
            else {
                // Any nonzero marker is raw in the original decoder.
                if (size != expected)
                    Reject(TJS_W("Invalid launcher artwork TLG5 raw channel length"));
                reader.Skip(size);
            }
        }
    }
}

struct Bits {
    const std::vector<std::uint8_t> &bytes;
    std::uint32_t length;
    std::uint32_t position = 0;

    unsigned One()
    {
        if (position == length) Reject(TJS_W("Truncated launcher artwork TLG6 bitstream"));
        const auto result = (bytes[position >> 3] >> (position & 7)) & 1;
        ++position;
        return result;
    }
    std::uint32_t Read(unsigned count)
    {
        if (count > 31 || count > length - position)
            Reject(TJS_W("Truncated launcher artwork TLG6 bitstream"));
        std::uint64_t word = 0;
        const std::size_t start = position >> 3;
        for (unsigned i = 0; i < 5 && start + i < bytes.size(); ++i)
            word |= std::uint64_t(bytes[start + i]) << (i * 8);
        const auto value = (word >> (position & 7)) & ((std::uint64_t(1) << count) - 1);
        position += count;
        return static_cast<std::uint32_t>(value);
    }
    std::uint32_t DecoderWord() const
    {
        std::uint32_t word = 0;
        const std::size_t start = position >> 3;
        for (unsigned i = 0; i < 4 && start + i < bytes.size(); ++i)
            word |= std::uint32_t(bytes[start + i]) << (i * 8);
        return word >> (position & 7);
    }
};

inline unsigned GolombK(unsigned sum, unsigned counter)
{
    // Identical cumulative table to tvpgl.c/SaveTLG6.cpp; no dependency on the
    // engine's global table having been initialized when preflight is called.
    static constexpr unsigned compressed[4][9] = {
        {3,7,15,27,63,108,223,448,130}, {3,5,13,24,51,95,192,384,257},
        {2,5,12,21,39,86,155,320,384}, {2,3,9,18,33,61,129,258,511}
    };
    if (sum >= 1024 || counter > 3)
        Reject(TJS_W("Invalid launcher artwork TLG6 Golomb table index"));
    unsigned end = 0;
    for (unsigned k = 0; k < 9; ++k) {
        end += compressed[counter][k];
        if (sum < end) return k;
    }
    Reject(TJS_W("Invalid launcher artwork TLG6 Golomb table index"));
}

inline void Golomb(const std::vector<std::uint8_t> &payload,
                   std::uint32_t bitLength, std::uint32_t pixels)
{
    Bits bits{payload, bitLength};
    bool zero = bits.One() == 0;
    unsigned sum = 0, counter = 3;
    std::uint32_t remaining = pixels;
    while (remaining) {
        unsigned zeros = 0;
        while (!bits.One()) {
            if (++zeros >= 31)
                Reject(TJS_W("Invalid launcher artwork TLG6 gamma run"));
        }
        const std::uint32_t run = (std::uint32_t(1) << zeros) + bits.Read(zeros);
        if (run > remaining)
            Reject(TJS_W("Launcher artwork TLG6 run exceeds its pixel block"));
        if (!zero) for (std::uint32_t i = 0; i < run; ++i) {
            const unsigned k = GolombK(sum, counter);
            unsigned quotient = 0;
            if (!bits.DecoderWord()) {
                // Exact upstream escape: skip the rest of four bytes from the
                // current byte boundary, read one quotient byte, then k bits.
                const auto quotientPosition = (bits.position >> 3) * 8 + 32;
                if (quotientPosition > bits.length)
                    Reject(TJS_W("Truncated launcher artwork TLG6 escape"));
                bits.position = quotientPosition;
                quotient = bits.Read(8);
            } else {
                while (!bits.One()) {
                    if (++quotient >= 32)
                        Reject(TJS_W("Invalid launcher artwork TLG6 Golomb prefix"));
                }
            }
            const auto value = (quotient << k) + bits.Read(k);
            sum += value >> 1;
            // Bounds are checked before every actual table lookup, including
            // runs spanning several adaptive groups. Preserve byte-wrap output
            // semantics for values that remain safe for the original decoder.
            if (counter == 0) { sum >>= 1; counter = 3; }
            else --counter;
        }
        remaining -= run;
        zero = !zero;
    }
    // The original decoder stops at pixel_count; unused declared tail bits are
    // harmless and are not interpreted here either.
}

inline void TLG6(Reader &reader)
{
    const auto colors = reader.Byte();
    if (colors != 1 && colors != 3 && colors != 4)
        Reject(TJS_W("Unsupported launcher artwork TLG6 color count"));
    const auto dataFlag = reader.Byte(), colorType = reader.Byte(), externalTable = reader.Byte();
    if (dataFlag || colorType || externalTable)
        Reject(TJS_W("Unsupported launcher artwork TLG6 flags"));
    const auto width = reader.U32(), height = reader.U32(), maxBits = reader.U32();
    Dimensions(width, height);
    if (!maxBits || maxBits > EncodedLimit * 8)
        Reject(TJS_W("Launcher artwork TLG6 bit pool exceeds the limit"));
    const std::uint64_t filters = std::uint64_t((width + 7) / 8) * ((height + 7) / 8);
    const auto filterBytes = reader.U32();
    if (!filterBytes || filterBytes > EncodedLimit)
        Reject(TJS_W("Launcher artwork TLG6 filter buffer exceeds the limit"));
    Slide(reader.Bytes(filterBytes), filters, true);
    for (std::uint32_t y = 0; y < height; y += 8) {
        const auto rows = height - y < 8 ? height - y : 8;
        const auto pixels = rows * width; // At most 8 * 16384; no signed overflow.
        for (unsigned channel = 0; channel < colors; ++channel) {
            const auto encodedBits = reader.U32();
            if (encodedBits >> 30)
                Reject(TJS_W("Unsupported launcher artwork TLG6 entropy method"));
            const auto bitLength = encodedBits & 0x3fffffff;
            if (!bitLength || bitLength > maxBits)
                Reject(TJS_W("Launcher artwork TLG6 channel exceeds its bit pool"));
            const auto bytes = (bitLength + 7) / 8;
            Golomb(reader.Bytes(bytes), bitLength, pixels);
        }
    }
}

inline void Raw(Reader &reader)
{
    std::uint8_t signature[11];
    reader.Read(signature, sizeof signature);
    if (!std::memcmp(signature, "TLG5.0\x00raw\x1a\x00", 11)) TLG5(reader);
    else if (!std::memcmp(signature, "TLG6.0\x00raw\x1a\x00", 11)) TLG6(reader);
    else Reject(TJS_W("Invalid launcher artwork TLG raw header"));
}

inline void Tags(const std::vector<std::uint8_t> &data)
{
    std::size_t position = 0;
    auto length = [&]() -> std::size_t {
        std::size_t result = 0;
        while (position < data.size() && data[position] >= '0' && data[position] <= '9') {
            const unsigned digit = data[position++] - '0';
            if (result > data.size() / 10 || result * 10 + digit > data.size())
                Reject(TJS_W("Launcher artwork TLG tag length exceeds its chunk"));
            result = result * 10 + digit;
        }
        if (position == data.size() || data[position++] != ':')
            Reject(TJS_W("Malformed launcher artwork TLG tag length"));
        return result;
    };
    auto field = [&](unsigned delimiter) {
        const auto size = length();
        if (size > data.size() - position)
            Reject(TJS_W("Truncated launcher artwork TLG tag field"));
        position += size;
        if (position == data.size() || data[position++] != delimiter)
            Reject(TJS_W("Malformed launcher artwork TLG tag delimiter"));
    };
    while (position < data.size()) { field('='); field(','); }
}

inline void Container(Reader &reader)
{
    std::uint8_t signature[11];
    reader.Read(signature, sizeof signature);
    if (std::memcmp(signature, "TLG0.0\x00sds\x1a\x00", 11)) {
        reader.stream.SetPosition(0);
        reader.position = 0;
        Raw(reader);
        return;
    }
    const auto rawLength = reader.U32();
    reader.Require(rawLength);
    Reader raw{reader.stream, reader.position, reader.position + rawLength};
    Raw(raw); // The nested raw decoder must not borrow bytes from metadata.
    raw.Skip(raw.Remaining()); // Preserve ignored raw padding inside TLG0.
    reader.position = raw.end;
    while (reader.Remaining()) {
        std::uint8_t name[4];
        reader.Read(name, sizeof name);
        const auto size = reader.U32();
        reader.Require(size);
        if (!std::memcmp(name, "tags", 4)) Tags(reader.Bytes(size));
        else reader.Skip(size);
    }
}

} // namespace tlg_preflight

inline void PreflightArtworkTLG(tTJSBinaryStream &input)
{
    const auto savedPosition = input.GetPosition();
    try {
        const auto size = input.GetSize();
        if (size > tlg_preflight::EncodedLimit)
            tlg_preflight::Reject(TJS_W("Launcher artwork TLG exceeds the 32 MiB limit"));
        input.SetPosition(0);
        tlg_preflight::Reader reader{input, 0, size};
        tlg_preflight::Container(reader);
        input.SetPosition(savedPosition);
    } catch (...) {
        try { input.SetPosition(savedPosition); } catch (...) { }
        throw;
    }
}

} // namespace krkrns_launcher_artwork
