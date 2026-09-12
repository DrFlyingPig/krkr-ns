#include "PagedCharacterCache.h"
#include "SharedByteView.h"
#include "BufferedWrite.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

static unsigned checks = 0;
static void check(bool ok, const char *message)
{
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

struct Metrics
{
    int index;
    int values[24]{}; // Same 100-byte layout size as the engine's glyph metrics.
};

static void glyphs()
{
    tTVPPagedCharacterCache<Metrics> cache;
    Metrics *first = &cache[0];
    check(first->index == -1024, "unseen glyph is unknown");
    first->index = -1;
    cache[0x4e2d].index = 3;
    cache[0x4e2d].values[7] = 82;
    for (unsigned code = 0; code <= 0xffff; ++code)
    {
        if (code != 0 && code != 0x4e2d)
            check(cache[static_cast<uint16_t>(code)].index == -1024, "all BMP glyphs start unknown");
    }
    check(&cache[0] == first && first->index == -1, "page growth preserves references and missing-glyph result");
    check(cache[0x4e2d].values[7] == 82, "CJK metrics survive other page allocations");
    cache[0xffff].index = 4;
    check(cache[0xffff].index == 4, "U+FFFF does not access beyond the cache");
    tTVPPagedCharacterCache<Metrics> otherSize;
    check(otherSize[0x4e2d].index == -1024, "font sizes do not share glyph results");

    constexpr unsigned repeats = 120;
    volatile int sink = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (unsigned j = 0; j < repeats; ++j)
    {
        auto old = std::make_unique<Metrics[]>(65535);
        for (unsigned i = 0; i < 65535; ++i) old[i].index = -1024;
        sink += old[j].index;
    }
    const auto middle = std::chrono::steady_clock::now();
    for (unsigned j = 0; j < repeats; ++j)
    {
        tTVPPagedCharacterCache<Metrics> sparse;
        sink += sparse[static_cast<uint16_t>(j)].index;
    }
    const auto end = std::chrono::steady_clock::now();
    std::printf("font-size cold allocation model: dense=%.3fms paged=%.3fms; first-page bytes=%zu vs %zu (%d)\n",
        std::chrono::duration<double, std::milli>(middle - begin).count() / repeats,
        std::chrono::duration<double, std::milli>(end - middle).count() / repeats,
        sizeof(tTVPPagedCharacterCache<Metrics>) + 256 * sizeof(Metrics), 65535 * sizeof(Metrics), sink);
}

static void views()
{
    auto allocation = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0,1,2,3,4,5,6,7});
    std::weak_ptr<const void> lifetime = allocation;
    KrkrSharedBytes cache{allocation, allocation->data(), allocation->size()};
    auto part = cache.Slice(2, 4);
    check(part.data == allocation->data() + 2, "resource opening does not copy payload");
    {
        KrkrReadOnlyCursor a(part), b(part);
        allocation.reset(); cache = {}; part = {};
        check(!lifetime.expired(), "live reader retains evicted/replaced resource");
        uint8_t bytes[8]{};
        check(a.Read(bytes, 2) == 2 && bytes[0] == 2 && bytes[1] == 3, "slice reads expected bytes");
        check(b.Tell() == 0, "concurrent streams have independent positions");
        check(a.Seek(-1, 2) == 3 && a.Read(bytes, 8) == 1 && bytes[0] == 5, "end-relative seek and bounded read");
        check(a.Read(bytes, 1) == 0, "EOF is a short read");
        check(a.Seek(INT64_MIN, 1) == 4 && a.Seek(INT64_MAX, 1) == 4, "overflow seeks leave position unchanged");
        check(a.Seek(-1, 0) == 4 && a.Seek(0, 77) == 4, "invalid seeks leave position unchanged");
        check(b.Read(bytes, 8) == 4 && bytes[3] == 5, "old resource survives cache replacement");
    }
    check(lifetime.expired(), "last reader frees resource");
    KrkrReadOnlyCursor empty({});
    check(empty.Read(nullptr, 0) == 0 && empty.Seek(0, 2) == 0, "empty resource is valid");
    bool threw = false;
    try { KrkrSharedBytes{}.Slice(1, 0); } catch (const std::out_of_range &) { threw = true; }
    check(threw, "out-of-range resource metadata is rejected");
}

static void writes()
{
    std::vector<uint8_t> expected, actual;
    size_t calls = 0;
    auto writer = [&](const void *p, size_t n) {
        const auto *bytes = static_cast<const uint8_t *>(p);
        actual.insert(actual.end(), bytes, bytes + n); ++calls;
    };
    KrkrBufferedWrite<> output;
    // Many saveStruct tokens, then values crossing/exceeding the buffer size.
    for (unsigned i = 0; i < 20000; ++i)
    {
        const uint8_t bytes[] = {static_cast<uint8_t>(i), 0, static_cast<uint8_t>(i >> 8)};
        output.Append(bytes, sizeof(bytes), writer);
        expected.insert(expected.end(), bytes, bytes + sizeof(bytes));
    }
    for (size_t size : {size_t(16383), size_t(16384), size_t(16385), size_t(70000)})
    {
        std::vector<uint8_t> value(size, 0xba);
        output.Append(value.data(), size, writer);
        expected.insert(expected.end(), value.begin(), value.end());
    }
    output.Append(nullptr, 0, writer);
    output.Flush(writer); output.Flush(writer);
    check(actual == expected, "coalescing preserves every byte across fragment boundaries");
    check(calls < 20, "many small save tokens become a few sink calls");
    std::printf("save buffering: 20004 writes -> %zu sink calls, %zu bytes identical\n", calls, actual.size());
    KrkrBufferedWrite<4> failing;
    failing.Append("ab", 2, writer);
    bool threw = false;
    try { failing.Flush([](const void *, size_t) { throw std::runtime_error("disk full"); }); }
    catch (const std::runtime_error &) { threw = true; }
    check(threw, "write failure reaches caller");
    unsigned replayed = 0;
    failing.Flush([&](const void *, size_t) { ++replayed; });
    check(replayed == 0, "failed output is not replayed during cleanup");
}

int main()
{
    glyphs(); views(); writes();
    std::printf("PASS: %u checks\n", checks);
}
