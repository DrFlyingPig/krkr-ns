/* SPDX-License-Identifier: MIT */
#ifndef TVP_PAGED_CHARACTER_CACHE_H
#define TVP_PAGED_CHARACTER_CACHE_H

#include <array>
#include <cstdint>
#include <memory>

// A new font size normally uses only a small part of the BMP. Allocate metrics
// in 256-character pages instead of constructing 65,535 entries on the first
// glyph. Entries never move, and U+FFFF is covered as well.
template<class Entry>
class tTVPPagedCharacterCache
{
    std::array<std::unique_ptr<Entry[]>, 256> Pages{};

public:
    Entry &operator[](std::uint16_t code)
    {
        auto &page = Pages[code >> 8];
        if (!page)
        {
            auto fresh = std::make_unique<Entry[]>(256);
            for (unsigned i = 0; i < 256; ++i) fresh[i].index = -1024;
            page = std::move(fresh);
        }
        return page[code & 255];
    }
};

#endif
