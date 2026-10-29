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
const char* const names[] = {"graphic-load", "image-decode", "xp3-read", "disk-read",
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
    "gpu-canvas-create", "gpu-group-render", "gpu-group-blit", "gpu-source-prepare"};
static_assert(sizeof(names) / sizeof(names[0]) == OpCount, "Every operation needs a report name");
struct Record
{
    std::mutex mutex;
    std::uint64_t n = 0, inclusive = 0, self = 0, bytes = 0;
};
Record records[2][OpCount];
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
