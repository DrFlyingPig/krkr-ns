/* SPDX-License-Identifier: MIT */
#ifndef KRKR_SHARED_BYTE_VIEW_H
#define KRKR_SHARED_BYTE_VIEW_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

// Immutable storage with shared lifetime. Opening a resource or a solid-archive
// member takes a view; replacing/evicting its cache cannot invalidate a reader.
struct KrkrSharedBytes
{
    std::shared_ptr<const void> owner;
    const std::uint8_t *data = nullptr;
    size_t size = 0;

    KrkrSharedBytes Slice(size_t offset, size_t length) const
    {
        if (offset > size || length > size - offset)
            throw std::out_of_range("Resource slice exceeds buffer");
        return {owner, data ? data + offset : nullptr, length};
    }
};

class KrkrReadOnlyCursor
{
    KrkrSharedBytes Bytes;
    size_t Position = 0;

public:
    explicit KrkrReadOnlyCursor(KrkrSharedBytes bytes) : Bytes(std::move(bytes)) {}
    size_t Size() const { return Bytes.size; }
    size_t Tell() const { return Position; }

    // Origins match TJS_BS_SEEK_SET/CUR/END. Failed seeks leave the cursor intact.
    size_t Seek(std::int64_t offset, int origin)
    {
        size_t unsignedBase;
        switch (origin)
        {
        case 0: unsignedBase = 0; break;
        case 1: unsignedBase = Position; break;
        case 2: unsignedBase = Bytes.size; break;
        default: return Position;
        }
        if (unsignedBase > static_cast<std::uint64_t>(INT64_MAX)) return Position;
        const auto base = static_cast<std::int64_t>(unsignedBase);
        if ((offset > 0 && base > INT64_MAX - offset) ||
            (offset < 0 && base < INT64_MIN - offset)) return Position;
        const auto next = base + offset;
        if (next < 0 || static_cast<std::uint64_t>(next) > Bytes.size) return Position;
        Position = static_cast<size_t>(next);
        return Position;
    }

    size_t Read(void *destination, size_t size)
    {
        const size_t count = std::min(size, Bytes.size - Position);
        if (count) std::memcpy(destination, Bytes.data + Position, count);
        Position += count;
        return count;
    }
};

#endif
