#include "BitmapBufferPool.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

static void check(bool ok, const char *message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main()
{
    {
        KrkrBitmapBufferPool arena;
        size_t aSize = 16 * 1024 * 1024, bSize = aSize, cSize = aSize, dSize = aSize;
        void *a = arena.Allocate(aSize), *b = arena.Allocate(bSize);
        void *c = arena.Allocate(cSize), *d = arena.Allocate(dSize);
        check(a && b && c && d, "one arena holds four adjacent images");
        check(arena.GetStats().reserved == arena.ArenaSize, "large images share a segregated arena");
        std::memset(a, 0x17, aSize);
        std::memset(d, 0x39, dSize);
        arena.Free(b, bSize); arena.Free(c, cSize);
        const auto before = arena.GetStats();
        size_t largerSize = 32 * 1024 * 1024;
        void *larger = arena.Allocate(largerSize);
        check(larger == b && arena.GetStats().reserved == before.reserved,
            "adjacent freed images combine without growing the heap");
        std::memset(larger, 0xab, largerSize);
        arena.Clear();
        check(static_cast<unsigned char *>(a)[aSize - 1] == 0x17 &&
              static_cast<unsigned char *>(d)[0] == 0x39,
              "larger allocation and compaction preserve neighboring live images");
        arena.Free(a, aSize); arena.Free(larger, largerSize); arena.Free(d, dSize);
        arena.Clear();
        check(arena.GetStats().reserved == 0, "fully freed arenas return to the system heap");
        for (size_t requested : {size_t(4096), size_t(33 * 1024 * 1024)})
        {
            size_t capacity = requested;
            void *data = arena.Allocate(capacity);
            check(data && capacity == requested, "small and oversized images use the fallback allocator");
            arena.Free(data, capacity);
        }
    }
    KrkrBitmapBufferPool pool;
    size_t capacity = 1920 * 1440 * 4 + 56;
    const size_t requested = capacity;
    void *first = pool.Allocate(capacity);
    check(first && capacity >= requested, "frame allocation fits the pixels and guards");
    std::memset(first, 0x5a, capacity);
    pool.Free(first, capacity);
    size_t next = requested;
    void *second = pool.Allocate(next);
    check(second == first && next == capacity, "freed frame stays reusable by the next frame");
    check(static_cast<unsigned char *>(second)[next - 1] == 0x5a, "pooled capacity remains accessible");
    pool.Clear();
    check(static_cast<unsigned char *>(second)[0] == 0x5a, "compaction preserves live pixels");
    pool.Free(second, next);

    // Exceed the idle arena budget; live allocations remain distinct.
    std::vector<std::pair<void *, size_t>> live;
    for (unsigned i = 0; i < 48; ++i)
    {
        size_t bytes = i < 24 ? 12 * 1024 * 1024 : 256 * 1024;
        void *data = pool.Allocate(bytes);
        check(data != nullptr, "mixed-size allocation succeeds");
        for (const auto &other : live) check(data != other.first, "live buffers never alias");
        static_cast<unsigned char *>(data)[bytes - 1] = static_cast<unsigned char>(i);
        live.emplace_back(data, bytes);
    }
    for (unsigned i = 0; i < live.size(); ++i)
    {
        const auto block = live[i];
        check(static_cast<unsigned char *>(block.first)[block.second - 1] == i, "eviction preserves other live buffers");
        pool.Free(block.first, block.second);
        const auto stats = pool.GetStats();
        check(stats.bytes <= pool.Budget && stats.count <= pool.Budget / pool.ArenaSize, "idle arenas stay bounded");
    }
    pool.Clear();
    check(pool.GetStats().bytes == 0 && pool.GetStats().count == 0, "compaction returns all idle memory");

    // Decode workers and the main renderer may free/allocate concurrently.
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 4; ++worker) workers.emplace_back([&, worker] {
        for (unsigned frame = 0; frame < 1500; ++frame)
        {
            size_t bytes = 262144 + ((frame + worker) % 12) * 65536;
            void *data = pool.Allocate(bytes);
            check(data != nullptr, "concurrent allocation succeeds");
            std::memset(data, worker + 1, bytes);
            const auto *pixels = static_cast<const unsigned char *>(data);
            check(pixels[0] == worker + 1 && pixels[bytes - 1] == worker + 1, "concurrent live buffers stay independent");
            pool.Free(data, bytes);
            if (frame % 257 == 0) pool.Clear();
        }
    });
    for (auto &worker : workers) worker.join();
    const auto stats = pool.GetStats();
    check(stats.hits > 5000, "sustained rendering primarily reuses large allocations");
    check(stats.bytes <= pool.Budget, "concurrent release preserves the memory budget");
    pool.Clear();
    std::printf("PASS: 6000 concurrent bitmap cycles, bounded eviction, live-data preservation, hits=%llu\n",
        static_cast<unsigned long long>(stats.hits));
}
