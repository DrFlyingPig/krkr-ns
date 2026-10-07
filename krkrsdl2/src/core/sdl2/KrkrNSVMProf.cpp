/* SPDX-License-Identifier: MIT */
#include "KrkrNSVMProf.h"

#if defined(__SWITCH__) || defined(KRKRNS_VM_PROF_TEST)
#include "KrkrNSLog.h"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

#ifdef KRKRNS_VM_PROF_TEST
// The host contract test captures the same report format without creating logs.
extern void krkrsdl2_vmprof_test_log(const char *, ...) noexcept;
#undef KRKRNS_LOG
#define KRKRNS_LOG(...) krkrsdl2_vmprof_test_log(__VA_ARGS__)
#endif

namespace
{
constexpr unsigned MaxRecords = 2048, BucketCount = 4096;
struct Record
{
    KrkrNSVMProfLabel label;
    std::uint64_t inclusive = 0, self = 0, entries = 0;
};
struct State
{
    std::array<Record, MaxRecords> records;
    std::array<unsigned short, BucketCount> buckets = {};
    KrkrNSVMProfScope *top = nullptr;
    unsigned count = 0;
    std::uint64_t start = 0, elapsed = 0, roots = 0;
    bool active = false, capturing = false, pending = false, failed = false;
    const char *failure = nullptr;
};
std::atomic<SDL_threadID> owner{0};
thread_local State *state = nullptr;
// Only the claimed ApplicationIdle thread reads or writes these fields.
bool markerChecked = false, requested = false;
#ifdef KRKRNS_VM_PROF_TEST
thread_local std::uint64_t testCounter = 0;
thread_local std::uint64_t testCounterReads = 0;
bool failAllocation = false;
#endif

std::uint64_t Counter() noexcept
{
#ifdef KRKRNS_VM_PROF_TEST
    ++testCounterReads;
    return testCounter;
#else
    return SDL_GetPerformanceCounter();
#endif
}
double Scale() noexcept
{
#ifdef KRKRNS_VM_PROF_TEST
    return 1.0; // deterministic test counter is in milliseconds
#else
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    return frequency ? 1000.0 / static_cast<double>(frequency) : 0.0;
#endif
}
bool ClaimOwner() noexcept
{
    const SDL_threadID current = SDL_ThreadID();
    if (!current) return false;
    SDL_threadID expected = 0;
    owner.compare_exchange_strong(expected, current, std::memory_order_relaxed);
    return owner.load(std::memory_order_relaxed) == current;
}
void Fail(const char *reason) noexcept
{
    requested = false;
    if (state) { state->active = false; state->failed = true; state->failure = reason; }
}
bool SameKey(const KrkrNSVMProfLabel &a, const KrkrNSVMProfLabel &b) noexcept
{
    return a.hash == b.hash && a.type == b.type && a.line == b.line && a.ip == b.ip &&
        std::strcmp(a.name, b.name) == 0 && std::strcmp(a.script, b.script) == 0;
}
void Begin(bool enabled) noexcept
{
    if (!enabled) return;
    if (!state)
    {
#ifdef KRKRNS_VM_PROF_TEST
        state = failAllocation ? nullptr : new (std::nothrow) State;
#else
        state = new (std::nothrow) State;
#endif
        if (!state)
        {
            requested = false;
            KRKRNS_LOG("[vm-prof] disabled: allocation failed");
            return;
        }
    }
    if (state->capturing || state->top) { Fail("nested dispatch"); return; }
    // An exception can bypass the normal outer-loop report site.
    krkrsdl2_vmprof_report();
    state->buckets.fill(0);
    state->count = 0;
    state->roots = 0;
    state->elapsed = 0;
    state->failed = false;
    state->failure = nullptr;
    state->pending = false;
    state->active = true;
    state->capturing = true;
    state->start = Counter();
}
}

bool krkrsdl2_vmprof_active() noexcept { return state && state->active; }

bool krkrsdl2_vmprof_native_begin(std::uint64_t &start) noexcept
{
    if (!krkrsdl2_vmprof_active()) return false;
    start = Counter();
    return true;
}

void krkrsdl2_vmprof_native_end(const char *name, std::uint64_t start) noexcept
{
    if (!krkrsdl2_vmprof_active()) return;
    const std::uint64_t end = Counter();
    const double elapsed = (end >= start ? end - start : 0) * Scale();
    if (elapsed > 20.0) KRKRNS_LOG("[vm-native] %s=%.2fms", name, elapsed);
}

void krkrsdl2_vmprof_begin_dispatch() noexcept
{
    if (!ClaimOwner()) return;
    if (!markerChecked)
    {
        markerChecked = true;
        // This opt-in is sampled once, outside TJS. Add/remove the marker
        // before launching the process; ordinary gameplay performs no VM I/O.
        if (FILE *marker = std::fopen("sdmc:/switch/KRKR-ns/vm-profile.txt", "rb"))
        {
            std::fclose(marker);
            requested = true;
            KRKRNS_LOG("[vm-prof] enabled: owner dispatch thread, max contexts=%u", MaxRecords);
        }
    }
    Begin(requested);
}

void krkrsdl2_vmprof_enter(KrkrNSVMProfScope *scope, const KrkrNSVMProfLabel &label) noexcept
{
    if (!krkrsdl2_vmprof_active() || scope->linked_) return;
    unsigned bucket = static_cast<unsigned>(label.hash ^ static_cast<unsigned>(label.line) ^
        static_cast<unsigned>(label.ip * 33u) ^ static_cast<unsigned>(label.type * 65537u)) & (BucketCount - 1);
    unsigned slot = 0;
    for (unsigned probe = 0; probe < BucketCount; ++probe)
    {
        const unsigned entry = state->buckets[bucket];
        if (!entry)
        {
            if (state->count == MaxRecords) { Fail("record capacity exceeded"); return; }
            slot = state->count++;
            state->records[slot] = Record{};
            state->records[slot].label = label;
            state->buckets[bucket] = static_cast<unsigned short>(slot + 1);
            break;
        }
        slot = entry - 1;
        if (SameKey(state->records[slot].label, label)) break;
        bucket = (bucket + 1) & (BucketCount - 1);
    }
    scope->parent_ = state->top;
    scope->slot_ = slot;
    scope->children_ = 0;
    scope->start_ = Counter();
    scope->linked_ = true;
    state->top = scope;
}

void krkrsdl2_vmprof_leave(KrkrNSVMProfScope *scope) noexcept
{
    if (!scope->linked_) return;
    scope->linked_ = false;
    if (!state) return;
    if (state->top != scope)
    {
        // A rejected scope must never leave a pointer to its ended lifetime.
        state->top = nullptr;
        Fail("scope order mismatch");
        return;
    }
    state->top = scope->parent_;
    if (!state->active) return;
    const std::uint64_t end = Counter();
    const std::uint64_t elapsed = end >= scope->start_ ? end - scope->start_ : 0;
    Record &record = state->records[scope->slot_];
    record.inclusive += elapsed;
    record.self += elapsed >= scope->children_ ? elapsed - scope->children_ : 0;
    ++record.entries;
    if (scope->parent_) scope->parent_->children_ += elapsed;
    else state->roots += elapsed;
}

void krkrsdl2_vmprof_end_dispatch() noexcept
{
    if (!state || !state->capturing) return;
    state->capturing = false;
    const std::uint64_t end = Counter();
    state->elapsed = end >= state->start ? end - state->start : 0;
    if (state->top) Fail("dispatch ended with active VM scopes");
    state->active = false;
    state->pending = true;
}

void krkrsdl2_vmprof_report() noexcept
{
    if (!state || !state->pending || state->top) return;
    state->pending = false;
    if (state->failed)
    {
        KRKRNS_LOG("[vm-prof] disabled: %s", state->failure);
        return;
    }
    const double scale = Scale();
    if (state->elapsed * scale <= 100.0) return;
    std::array<unsigned, 16> top{};
    unsigned size = 0;
    std::uint64_t entries = 0;
    for (unsigned i = 0; i < state->count; ++i)
    {
        entries += state->records[i].entries;
        unsigned pos = 0;
        while (pos < size && state->records[top[pos]].self >= state->records[i].self) ++pos;
        if (pos >= top.size()) continue;
        if (size < top.size()) ++size;
        for (unsigned j = size - 1; j > pos; --j) top[j] = top[j - 1];
        top[pos] = i;
    }
    KRKRNS_LOG("[vm-prof] dispatch=%.2f vm-roots=%.2f residual=%.2fms contexts=%u entries=%llu",
        state->elapsed * scale, state->roots * scale,
        (state->elapsed >= state->roots ? state->elapsed - state->roots : 0) * scale,
        state->count, static_cast<unsigned long long>(entries));
    for (unsigned i = 0; i < size; ++i)
    {
        const Record &record = state->records[top[i]];
        KRKRNS_LOG("[vm-prof] self=%.2f inclusive=%.2f n=%llu type=%d %s @ %s:%d ip=%d%s",
            record.self * scale, record.inclusive * scale,
            static_cast<unsigned long long>(record.entries), record.label.type,
            record.label.name, record.label.script, record.label.line, record.label.ip,
            record.label.truncated ? " [label truncated]" : "");
    }
}

#ifdef KRKRNS_VM_PROF_TEST
void krkrsdl2_vmprof_test_begin(bool enabled) noexcept
{
    if (!ClaimOwner()) return;
    requested = enabled;
    Begin(enabled);
}
void krkrsdl2_vmprof_test_counter(std::uint64_t value) noexcept { testCounter = value; }
bool krkrsdl2_vmprof_test_find(const char *name, KrkrNSVMProfTestRecord &out) noexcept
{
    if (!state) return false;
    for (unsigned i = 0; i < state->count; ++i)
        if (std::strcmp(state->records[i].label.name, name) == 0)
        {
            const Record &r = state->records[i];
            out.label = r.label; out.inclusive = r.inclusive;
            out.self = r.self; out.entries = r.entries;
            return true;
        }
    return false;
}
std::uint64_t krkrsdl2_vmprof_test_roots() noexcept { return state ? state->roots : 0; }
bool krkrsdl2_vmprof_test_pending() noexcept { return state && state->pending; }
void krkrsdl2_vmprof_test_fail_allocation(bool enabled) noexcept { failAllocation = enabled; }
std::uint64_t krkrsdl2_vmprof_test_counter_reads() noexcept { return testCounterReads; }
#endif
#endif
