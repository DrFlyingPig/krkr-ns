/* SPDX-License-Identifier: MIT */
#pragma once

#include <atomic>
#include <cstddef>
#include <utility>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <new>

namespace krkrns
{
struct SymbolAllocatorSystemBacking
{
    static void* Allocate(std::size_t bytes) noexcept { return std::malloc(bytes); }
    static void Free(void* allocation) noexcept { std::free(allocation); }
};

// Fixed-size scalar allocations only. The caller retains the object's exact
// layout, construction/destruction, and ordinary new[]/delete[] behavior.
// Page storage is capped; excess requests use independently owned allocations.
template<std::size_t PayloadBytes, std::size_t PayloadAlignment,
         std::size_t PageBytes = 64 * 1024, std::size_t MaximumPages = 8192,
         class Backing = SymbolAllocatorSystemBacking>
class SymbolAllocator
{
    static_assert(PayloadBytes > 0, "An allocation must contain a payload");
    static_assert(PayloadAlignment && !(PayloadAlignment & (PayloadAlignment - 1)),
                  "Alignment must be a power of two");
    struct Page;
    struct Slot
    {
        Page* owner;
        // A page slot needs its link only while free. A standalone slot uses
        // the same space to retain the original, possibly unaligned malloc.
        union { Slot* next; void* allocation; } link;
        alignas(PayloadAlignment) unsigned char payload[PayloadBytes];
    };
    struct Page
    {
        void* allocation;
        Page* previous;
        Page* next;
        Slot* available;
        std::size_t live;
    };
    static constexpr std::size_t PageAlignment =
        alignof(Page) > alignof(Slot) ? alignof(Page) : alignof(Slot);
    static constexpr std::size_t SlotOffset =
        (sizeof(Page) + alignof(Slot) - 1) & ~(alignof(Slot) - 1);
    static_assert(PageBytes >= SlotOffset + sizeof(Slot), "Page must hold a slot");
    static_assert(PageBytes <= (std::numeric_limits<std::size_t>::max)() - PageAlignment + 1,
                  "Page allocation size overflows");
    static constexpr std::size_t SlotsPerPage = (PageBytes - SlotOffset) / sizeof(Slot);
    static constexpr std::size_t PageAllocationBytes = PageBytes + PageAlignment - 1;
    static constexpr std::size_t SlotAllocationBytes = sizeof(Slot) + alignof(Slot) - 1;

    struct State
    {
        std::mutex mutex;
        Page* available = nullptr;
        std::size_t pages = 0;
        std::size_t peakPages = 0;
        std::size_t standaloneLive = 0;
        std::size_t standaloneTotal = 0;
#ifdef KRKRNS_SYMBOL_ALLOCATOR_TEST
        std::size_t liveSlots = 0;
#endif
    };
    static std::atomic<State*>& PublishedState() noexcept
    {
        static std::atomic<State*> state{nullptr};
        return state;
    }
    static State& GetState()
    {
        // State deliberately outlives static destruction; pages do not.
        static State* const state = [] {
            State* result = new State;
            PublishedState().store(result, std::memory_order_release);
            return result;
        }();
        return *state;
    }
    static void* Align(void* allocation, std::size_t alignment) noexcept
    {
        const auto address = reinterpret_cast<std::uintptr_t>(allocation);
        return reinterpret_cast<void*>((address + alignment - 1) & ~(alignment - 1));
    }
    static void Insert(State& state, Page* page) noexcept
    {
        page->previous = nullptr;
        page->next = state.available;
        if (page->next) page->next->previous = page;
        state.available = page;
    }
    static void Remove(State& state, Page* page) noexcept
    {
        if (page->previous) page->previous->next = page->next;
        else state.available = page->next;
        if (page->next) page->next->previous = page->previous;
    }
    static Page* CreatePage() noexcept
    {
        void* allocation = Backing::Allocate(PageAllocationBytes);
        if (!allocation) return nullptr;
        auto* page = ::new (Align(allocation, PageAlignment)) Page;
        page->allocation = allocation;
        page->previous = page->next = nullptr;
        page->available = nullptr;
        page->live = 0;
        auto* slots = reinterpret_cast<unsigned char*>(page) + SlotOffset;
        for (std::size_t index = 0; index < SlotsPerPage; ++index)
        {
            auto* slot = ::new (static_cast<void*>(slots + index * sizeof(Slot))) Slot;
            slot->owner = page;
            slot->link.next = page->available;
            page->available = slot;
        }
        return page;
    }

public:
    // Optional existing-page lease: never creates State or reserves slots.
    // If the first page cannot satisfy the maximum subsequent allocations,
    // normal per-allocation locking remains in use. The caller must not run
    // callbacks or deallocate this specialization while the lease is active.
    class Lease
    {
        State* state_ = nullptr;
        std::unique_lock<std::mutex> lock_;
        std::size_t remaining_ = 0;
    public:
        explicit Lease(std::size_t needed) noexcept
        {
            if (!needed || needed > SlotsPerPage) return;
            State* state = PublishedState().load(std::memory_order_acquire);
            if (!state) return;
            try {
                std::unique_lock<std::mutex> lock(state->mutex, std::try_to_lock);
                if (!lock.owns_lock() || !state->available ||
                    SlotsPerPage - state->available->live < needed) return;
                state_ = state;
                lock_ = std::move(lock);
                remaining_ = needed;
            } catch (...) { state_ = nullptr; }
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        explicit operator bool() const noexcept { return state_ != nullptr; }
        void* Allocate() {
            if (!state_) return SymbolAllocator::Allocate();
            if (!remaining_) {
                state_ = nullptr;
                lock_.unlock();
                return SymbolAllocator::Allocate();
            }
            --remaining_;
            return AllocateLocked(*state_);
        }
    };

    static void* Allocate()
    {
        State& state = GetState();
        std::lock_guard<std::mutex> lock(state.mutex);
        return AllocateLocked(state);
    }

private:
    static void* AllocateLocked(State& state)
    {
        if (!state.available && state.pages < MaximumPages)
        {
            if (Page* page = CreatePage())
            {
                Insert(state, page);
                ++state.pages;
                if (state.peakPages < state.pages) state.peakPages = state.pages;
            }
        }
        if (Page* page = state.available)
        {
            Slot* slot = page->available;
            page->available = slot->link.next;
            ++page->live;
            if (!page->available) Remove(state, page);
#ifdef KRKRNS_SYMBOL_ALLOCATOR_TEST
            ++state.liveSlots;
#endif
            return slot->payload;
        }
        // A page may fail even though one small allocation can still succeed.
        // Its explicit owner marker makes deletion independent of page lists.
        void* allocation = Backing::Allocate(SlotAllocationBytes);
        if (!allocation) throw std::bad_alloc();
        auto* slot = ::new (Align(allocation, alignof(Slot))) Slot;
        slot->owner = nullptr;
        slot->link.allocation = allocation;
        ++state.standaloneLive;
        ++state.standaloneTotal;
        return slot->payload;
    }

public:
    static void Deallocate(void* payload) noexcept
    {
        if (!payload) return;
        // Read only this allocation's prefix. Never infer an owner by rounding
        // to a virtual page boundary, which could read outside mapped memory.
        auto* slot = reinterpret_cast<Slot*>(static_cast<unsigned char*>(payload) - offsetof(Slot, payload));
        State& state = GetState();
        void* release = nullptr;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (Page* page = slot->owner)
            {
                const bool wasFull = !page->available;
                slot->link.next = page->available;
                page->available = slot;
                --page->live;
#ifdef KRKRNS_SYMBOL_ALLOCATOR_TEST
                --state.liveSlots;
#endif
                if (wasFull) Insert(state, page);
                if (!page->live)
                {
                    Remove(state, page);
                    --state.pages;
                    release = page->allocation;
                }
            }
            else
            {
                release = slot->link.allocation;
                --state.standaloneLive;
            }
        }
        // No empty page survives its final deletion; avoid holding the pool
        // mutex during the backing allocator's potentially expensive free.
        if (release) Backing::Free(release);
    }

    // Runtime snapshots expose page pressure and fallback traffic without a
    // per-pooled-slot production counter. The 512 MiB default is a ceiling,
    // not a reservation; no page is kept after its final live slot is freed.
    struct RuntimeStatistics
    {
        std::size_t pages, peakPages, standaloneLive, standaloneTotal;
    };
    static RuntimeStatistics GetRuntimeStatistics() noexcept
    {
        try
        {
            State& state = GetState();
            std::lock_guard<std::mutex> lock(state.mutex);
            return {state.pages, state.peakPages, state.standaloneLive, state.standaloneTotal};
        }
        catch (...) { return {}; } // Diagnostics must not fail a low-memory load.
    }

#ifdef KRKRNS_SYMBOL_ALLOCATOR_TEST
    struct Statistics { std::size_t pages, liveSlots, standalone; };
    static Statistics GetStatistics()
    {
        State& state = GetState();
        std::lock_guard<std::mutex> lock(state.mutex);
        return {state.pages, state.liveSlots, state.standaloneLive};
    }
    static constexpr std::size_t CapacityPerPage() noexcept { return SlotsPerPage; }
    static constexpr std::size_t PageBackingBytes() noexcept { return PageAllocationBytes; }
    static constexpr std::size_t StandaloneBackingBytes() noexcept { return SlotAllocationBytes; }
#endif
};

// Backing for a scalar symbol's BASE TABLE array, selected by the exact byte
// count supplied by the compiler to operator new[] / sized operator delete[].
// That count can include a cookie. This helper neither interprets it nor
// changes the address the compiler uses to construct/destroy the elements.
class SymbolArrayAllocator
{
    template<std::size_t Bytes>
    using Pool = SymbolAllocator<Bytes, alignof(void*), 64 * 1024, 2048>;

public:
    static void* Allocate(std::size_t bytes)
    {
        if (bytes <= 64) return Pool<64>::Allocate();
        if (bytes <= 128) return Pool<128>::Allocate();
        if (bytes <= 256) return Pool<256>::Allocate();
        if (bytes <= 512) return Pool<512>::Allocate();
        if (bytes <= 1024) return Pool<1024>::Allocate();
        if (bytes <= 2048) return Pool<2048>::Allocate();
        if (bytes <= 4096) return Pool<4096>::Allocate();
        return ::operator new[](bytes);
    }
    static void Deallocate(void* allocation, std::size_t bytes) noexcept
    {
        if (!allocation) return;
        if (bytes <= 64) Pool<64>::Deallocate(allocation);
        else if (bytes <= 128) Pool<128>::Deallocate(allocation);
        else if (bytes <= 256) Pool<256>::Deallocate(allocation);
        else if (bytes <= 512) Pool<512>::Deallocate(allocation);
        else if (bytes <= 1024) Pool<1024>::Deallocate(allocation);
        else if (bytes <= 2048) Pool<2048>::Deallocate(allocation);
        else if (bytes <= 4096) Pool<4096>::Deallocate(allocation);
        else ::operator delete[](allocation);
    }
    struct RuntimeStatistics
    {
        // Each pool has its own mutex; this is a diagnostic snapshot, not an
        // atomic cross-pool view. Large/global allocations are not included.
        std::size_t pages, peakPagesSum, standaloneLive, standaloneTotal;
    };
    static RuntimeStatistics GetRuntimeStatistics() noexcept
    {
        RuntimeStatistics total{};
        const auto add = [&total](const auto& statistics) {
            total.pages += statistics.pages;
            // Sum of independent high-water marks, NOT a simultaneous peak.
            total.peakPagesSum += statistics.peakPages;
            total.standaloneLive += statistics.standaloneLive;
            total.standaloneTotal += statistics.standaloneTotal;
        };
        add(Pool<64>::GetRuntimeStatistics());
        add(Pool<128>::GetRuntimeStatistics());
        add(Pool<256>::GetRuntimeStatistics());
        add(Pool<512>::GetRuntimeStatistics());
        add(Pool<1024>::GetRuntimeStatistics());
        add(Pool<2048>::GetRuntimeStatistics());
        add(Pool<4096>::GetRuntimeStatistics());
        return total;
    }
};
}
