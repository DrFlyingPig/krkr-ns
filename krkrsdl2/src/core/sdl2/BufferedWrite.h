/* SPDX-License-Identifier: MIT */
#ifndef KRKR_BUFFERED_WRITE_H
#define KRKR_BUFFERED_WRITE_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

// Coalesce the small fragments produced by Dictionary/Array.saveStruct before
// entering zlib or the filesystem. Flush explicitly so failures reach the caller.
template<size_t Capacity = 16384>
class KrkrBufferedWrite
{
    static_assert(Capacity > 0, "A write buffer cannot be empty");
    std::array<std::uint8_t, Capacity> Bytes;
    size_t Used = 0;
public:
    template<class Writer>
    void Flush(Writer write)
    {
        const size_t count = Used;
        Used = 0; // A failed sink write must not be replayed during destruction.
        if (count) write(Bytes.data(), count);
    }

    template<class Writer>
    void Append(const void *source, size_t size, Writer write)
    {
        auto *data = static_cast<const std::uint8_t *>(source);
        while (size)
        {
            if (!Used && size >= Capacity)
            {
                write(data, size);
                return;
            }
            const size_t count = std::min(size, Capacity - Used);
            std::memcpy(Bytes.data() + Used, data, count);
            Used += count;
            data += count;
            size -= count;
            if (Used == Capacity) Flush(write);
        }
    }
};

#endif
