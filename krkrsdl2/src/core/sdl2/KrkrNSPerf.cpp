/* SPDX-License-Identifier: MIT */
#include "KrkrNSPerf.h"
#if defined(__SWITCH__) || defined(KRKRNS_PERF_TEST)
#include "KrkrNSPerfStats.h"
#include "KrkrNSLog.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#ifndef KRKRNS_PERF_TEST
#include <SDL.h>
#include <switch.h>
#else
#include <thread>
extern void krkrsdl2_perf_test_log(const char*, ...) noexcept;
#undef KRKRNS_LOG
#define KRKRNS_LOG(...) krkrsdl2_perf_test_log(__VA_ARGS__)
#undef KRKRNS_LOG_DEFERRED
#define KRKRNS_LOG_DEFERRED(...) krkrsdl2_perf_test_log(__VA_ARGS__)
#endif

namespace
{
constexpr unsigned OpCount = static_cast<unsigned>(KrkrNSPerfOp::Count);
const char* const names[] = {"graphic-load", "image-decode", "xp3-read", "disk-read", "disk-write",
    "inflate", "bitmap-detach", "thread-wait", "layer-prepare", "layer-raster",
    "layer-finish", "dispatch", "present", "bitmap-blt", "blend-pixels", "bitmap-copy", "copy-pixels",
    "texture-upload", "psb-parse", "psb-commit", "psb-expand",
    "alpha-pixels", "alpha-hda-pixels", "alpha-opacity-pixels", "alpha-hda-opacity-pixels", "psb-release",
    "layer-on-paint", "layer-transition-start", "layer-transition-children", "layer-prepare-tree",
    "emote-readback", "gpu-readback", "emote-layer-bridge",
    "emote-convert", "bitmap-affine", "bitmap-affine-raster",
    "psb-read", "psb-tables", "psb-build", "surface-damage-bounds", "surface-copy",
    "script-event", "input-event", "continuous-event", "window-update",
    "idle-compact", "idle-rehash", "gpu-texture-init", "autopath-validate", "autopath-rebuild",
    "gpu-damage-compare", "gpu-damage-map", "gpu-resident-copy",
    "gpu-canvas-upload", "gpu-canvas-copy", "gpu-canvas-readback", "gpu-canvas-draw", "gpu-canvas-target-switch",
    "gpu-canvas-create", "gpu-group-render", "gpu-group-blit", "gpu-source-prepare",
    "graphic-cache-hit", "graphic-cache-store", "graphic-meta-dict",
    "gpu-canvas-upload-flip"};
static_assert(sizeof(names) / sizeof(names[0]) == OpCount, "Every operation needs a report name");
struct Record
{
    std::mutex mutex;
    std::uint64_t n = 0, inclusive = 0, self = 0, bytes = 0;
};
Record records[2][OpCount];
// Frame-entry snapshot used only to attribute a stalled frame. A report drains
// every record, so a base above the live value means the frame crossed a report
// boundary and the earlier part of its work cannot be attributed.
std::uint64_t frameBaseN[2][OpCount];
std::uint64_t frameBaseSelf[2][OpCount];
std::atomic<bool> enabled{false};
std::atomic<std::uint64_t> cacheHit{0}, cacheMiss{0};
thread_local KrkrNSPerfScope* top = nullptr;
KrkrNSPerfSamples<> loopSamples, frameSamples, gapSamples, inputSamples;
std::uint64_t lastPresent = 0, inputStart = 0;
#ifdef KRKRNS_PERF_TEST
std::thread::id owner;
unsigned Domain() noexcept { return std::this_thread::get_id() == owner ? 0 : 1; }
thread_local std::uint64_t clockValue = 0;
std::uint64_t Counter() noexcept { return clockValue; }
double Scale() noexcept { return 1.0; }
#else
std::atomic<SDL_threadID> owner{0};
unsigned Domain() noexcept { return SDL_ThreadID() == owner.load(std::memory_order_relaxed) ? 0 : 1; }
std::uint64_t Counter() noexcept { return SDL_GetPerformanceCounter(); }
double Scale() noexcept { return 1000.0 / SDL_GetPerformanceFrequency(); }
#endif
void Samples(const char* name, KrkrNSPerfSamples<>& samples) noexcept
{
    if (samples.Total())
        KRKRNS_LOG_DEFERRED("[perf-time] kind=%s n=%llu retained=%u p50=%.3f p95=%.3f p99=%.3f max=%.3f long33=%llu long100=%llu",
            name, static_cast<unsigned long long>(samples.Total()), samples.Count(),
            samples.Percentile(50), samples.Percentile(95), samples.Percentile(99),
            samples.Maximum(), static_cast<unsigned long long>(samples.Long33()),
            static_cast<unsigned long long>(samples.Long100()));
    samples.Reset();
}
}

bool krkrsdl2_perf_enabled() noexcept { return enabled.load(std::memory_order_relaxed); }
void krkrsdl2_perf_init() noexcept
{
#ifndef KRKRNS_PERF_TEST
    static bool checked = false;
    if (checked) return;
    checked = true;
    owner = SDL_ThreadID();
    if (FILE* file = std::fopen("sdmc:/switch/KRKR-ns/perf-profile.txt", "rb"))
    {
        std::fclose(file);
        enabled.store(true, std::memory_order_relaxed);
        u64 mask = 0;
        const Result coreRc = svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
        u32 configuration = 0;
        const ApmPerformanceMode mode = appletGetPerformanceMode();
        const Result apmRc = apmGetPerformanceConfiguration(mode, &configuration);
        KRKRNS_LOG("[perf] enabled bounded-samples=512 applet=%d operation=%d performance=%d configuration=%08x apmRc=%x cores=%llx coreRc=%x",
            static_cast<int>(appletGetAppletType()), static_cast<int>(appletGetOperationMode()),
            static_cast<int>(mode), configuration, apmRc,
            static_cast<unsigned long long>(mask), coreRc);
    }
#endif
}

KrkrNSPerfScope::KrkrNSPerfScope(KrkrNSPerfOp operation, std::uint64_t bytes) noexcept
    : operation_(operation), bytes_(bytes)
{
    if (!krkrsdl2_perf_enabled()) return;
    parent_ = top;
    domain_ = Domain();
    start_ = Counter();
    linked_ = true;
    top = this;
}
KrkrNSPerfScope::~KrkrNSPerfScope() noexcept { Finish(); }
void KrkrNSPerfScope::Finish() noexcept
{
    if (!linked_) return;
    const std::uint64_t now = Counter();
    const std::uint64_t elapsed = now >= start_ ? now - start_ : 0;
    if (top == this) top = parent_;
    if (parent_) parent_->children_ += elapsed;
    Record& record = records[domain_][static_cast<unsigned>(operation_)];
    {
        std::lock_guard<std::mutex> lock(record.mutex);
        ++record.n;
        record.inclusive += elapsed;
        record.self += elapsed >= children_ ? elapsed - children_ : 0;
        record.bytes += bytes_;
    }
    linked_ = false;
}
void krkrsdl2_perf_cache(bool hit) noexcept
{
    if (krkrsdl2_perf_enabled()) (hit ? cacheHit : cacheMiss).fetch_add(1, std::memory_order_relaxed);
}
void krkrsdl2_perf_bytes(KrkrNSPerfOp operation, std::uint64_t bytes) noexcept
{
    if (!krkrsdl2_perf_enabled()) return;
    auto& record = records[Domain()][static_cast<unsigned>(operation)];
    std::lock_guard<std::mutex> lock(record.mutex);
    ++record.n;
    record.bytes += bytes;
}
void krkrsdl2_perf_loop(double ms, bool presented) noexcept
{
    if (!krkrsdl2_perf_enabled()) return;
    loopSamples.Add(ms);
    if (presented)
    {
        frameSamples.Add(ms);
        KRKRNS_LOG_DEFERRED("[perf-frame] active=%.3fms", ms);
    }
}
void krkrsdl2_perf_input() noexcept
{
    if (krkrsdl2_perf_enabled() && !inputStart) inputStart = Counter();
}
void krkrsdl2_perf_present() noexcept
{
    if (!krkrsdl2_perf_enabled()) return;
    const auto now = Counter();
    if (lastPresent && now >= lastPresent) gapSamples.Add((now - lastPresent) * Scale());
    lastPresent = now;
    if (inputStart && now >= inputStart)
    {
        const double ms = (now - inputStart) * Scale();
        inputSamples.Add(ms);
        KRKRNS_LOG_DEFERRED("[perf-input] next-present=%.3fms", ms);
        inputStart = 0;
    }
}
#ifdef __SWITCH__
// Defined by the bundled zlib's inflate wrapper.
extern "C" void krkrns_zlib_inflate_stats(unsigned long long *calls, unsigned long long *in,
                                          unsigned long long *out, unsigned long long *ticks);
#endif
#ifdef __SWITCH__
// Defined by the TJS interpreter; prints this window's opcode histogram when
// the opt-in marker file is present.
extern "C" void krkrns_vm_opcode_dump();
#endif
void krkrsdl2_perf_report() noexcept
{
    if (!krkrsdl2_perf_enabled()) return;
    KrkrNSLogBatch logBatch;
#ifndef KRKRNS_PERF_TEST
    static char previousPhase[96] = "boot";
    static bool phaseKnown = false;
    char phase[96] = "boot";
    if (FILE* file = std::fopen("sdmc:/switch/KRKR-ns/perf-phase.txt", "rb"))
    {
        if (std::fgets(phase, sizeof(phase), file))
            phase[std::strcspn(phase, "\r\n")] = 0;
        std::fclose(file);
    }
    const bool changed = phaseKnown && std::strcmp(phase, previousPhase) != 0;
    KRKRNS_LOG_DEFERRED("[perf-report] tick=%u phase=%s boundary=%d", SDL_GetTicks(), phase, changed ? 1 : 0);
    std::memcpy(previousPhase, phase, sizeof(phase));
    phaseKnown = true;
#ifdef __SWITCH__
    // Opt-in opcode histogram (marker file checked by the interpreter). Emitted
    // next to the phase report so each window's distribution can be subtracted.
    krkrns_vm_opcode_dump();
    {
        // Share of the decode path taken by the zlib inflate itself, counted
        // inside the bundled zlib so libpng's per-call window is visible.
        static unsigned long long prevCalls = 0, prevIn = 0, prevOut = 0, prevTicks = 0;
        unsigned long long calls = 0, in = 0, out = 0, ticks = 0;
        krkrns_zlib_inflate_stats(&calls, &in, &out, &ticks);
        if (calls != prevCalls)
        {
            const double hz = 19200000.0; // Switch system tick rate
            const double ms = double(ticks - prevTicks) * 1000.0 / hz;
            const double outMb = double(out - prevOut) / (1024.0 * 1024.0);
            const double inKb = double(in - prevIn) / 1024.0;
            KRKRNS_LOG_DEFERRED("[zlib-inflate] calls=%llu inKB=%.0f outMB=%.1f ms=%.1f MBps=%.1f",
                calls - prevCalls, inKb, outMb, ms, ms > 0.0 ? outMb * 1000.0 / ms : 0.0);
        }
        prevCalls = calls; prevIn = in; prevOut = out; prevTicks = ticks;
    }
#endif
#endif
    // Worker elapsed time is separate work, not main-thread wall time. Nested scopes
    // subtract children on their own thread only; do not sum it into frame time.
    const double scale = Scale();
    for (unsigned domain = 0; domain < 2; ++domain)
    for (unsigned i = 0; i < OpCount; ++i)
    {
        auto& record = records[domain][i];
        std::uint64_t n, inclusive, self, bytes;
        {
            // A completed operation belongs to one window, including its count
            // and bytes; worker completions must not split across exchanges.
            std::lock_guard<std::mutex> lock(record.mutex);
            n = record.n; inclusive = record.inclusive;
            self = record.self; bytes = record.bytes;
            record.n = record.inclusive = record.self = record.bytes = 0;
        }
        if (n || inclusive || bytes)
            KRKRNS_LOG_DEFERRED("[perf-op] domain=%s name=%s n=%llu inclusive=%.3f self=%.3f bytes=%llu",
                domain ? "worker" : "main", names[i], static_cast<unsigned long long>(n), inclusive * scale, self * scale,
                static_cast<unsigned long long>(bytes));
    }
    KRKRNS_LOG_DEFERRED("[perf-cache] hit=%llu miss=%llu",
        static_cast<unsigned long long>(cacheHit.exchange(0, std::memory_order_relaxed)),
        static_cast<unsigned long long>(cacheMiss.exchange(0, std::memory_order_relaxed)));
    Samples("active-loop", loopSamples);
    Samples("presenting-loop", frameSamples);
    Samples("present-gap-including-wait", gapSamples);
    Samples("input-to-next-present", inputSamples);
}
void krkrsdl2_perf_frame_mark() noexcept
{
    if (!krkrsdl2_perf_enabled()) return;
    for (unsigned domain = 0; domain < 2; ++domain)
    for (unsigned i = 0; i < OpCount; ++i)
    {
        auto& record = records[domain][i];
        std::lock_guard<std::mutex> lock(record.mutex);
        frameBaseN[domain][i] = record.n;
        frameBaseSelf[domain][i] = record.self;
    }
}
void krkrsdl2_perf_stall_dump() noexcept
{
    if (!krkrsdl2_perf_enabled()) return;
    struct Row { double ms; unsigned long long n; unsigned domain; unsigned op; };
    Row rows[OpCount * 2];
    unsigned total = 0;
    const double scale = Scale();
    for (unsigned domain = 0; domain < 2; ++domain)
    for (unsigned i = 0; i < OpCount; ++i)
    {
        auto& record = records[domain][i];
        std::uint64_t n, self;
        {
            std::lock_guard<std::mutex> lock(record.mutex);
            n = record.n;
            self = record.self;
        }
        if (n < frameBaseN[domain][i] || self < frameBaseSelf[domain][i]) continue;
        const unsigned long long dn = n - frameBaseN[domain][i];
        const double ms = static_cast<double>(self - frameBaseSelf[domain][i]) * scale;
        if (dn == 0 || ms < 0.5) continue;
        rows[total++] = Row{ms, dn, domain, i};
    }
    constexpr unsigned Shown = 8;
    const unsigned shown = total < Shown ? total : Shown;
    for (unsigned a = 0; a < shown; ++a)
        for (unsigned b = a + 1; b < total; ++b)
            if (rows[b].ms > rows[a].ms) { const Row swap = rows[a]; rows[a] = rows[b]; rows[b] = swap; }
    char text[900];
    int used = 0;
    for (unsigned r = 0; r < shown; ++r)
    {
        const int wrote = std::snprintf(text + used, sizeof(text) - static_cast<size_t>(used),
            "%s%s:%s=%.1f/%llu", r ? " " : "", rows[r].domain ? "worker" : "main",
            names[rows[r].op], rows[r].ms, rows[r].n);
        if (wrote <= 0 || static_cast<size_t>(used + wrote) >= sizeof(text)) { text[used] = 0; break; }
        used += wrote;
    }
    if (!shown) text[0] = 0;
    KRKRNS_LOG("[stall-ops] %s", shown ? text : "none");
}
#ifdef KRKRNS_PERF_TEST
void krkrsdl2_perf_test_enable(bool value) noexcept { owner = std::this_thread::get_id(); enabled.store(value); }
void krkrsdl2_perf_test_clock(std::uint64_t value) noexcept { clockValue = value; }
KrkrNSPerfTestRecord krkrsdl2_perf_test_record(KrkrNSPerfOp op) noexcept
{
    auto& main = records[0][static_cast<unsigned>(op)];
    auto& worker = records[1][static_cast<unsigned>(op)];
    std::lock_guard<std::mutex> mainLock(main.mutex);
    std::lock_guard<std::mutex> workerLock(worker.mutex);
    return {main.n + worker.n, main.inclusive + worker.inclusive,
        main.self + worker.self, main.bytes + worker.bytes};
}
#endif
#endif
