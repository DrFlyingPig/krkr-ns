/* SPDX-License-Identifier: MIT */
#include "KrkrNSResourcePolicy.h"
#ifdef __SWITCH__
#include "KrkrNSLog.h"
#include "ReferenceRendererConfig.h"
#include <switch.h>
#include <atomic>
#include <mutex>
#include <new>

namespace
{
std::atomic<bool> softwareComposition{true};
std::atomic<unsigned> frameRate{60};

struct ResourcePolicy
{
    std::mutex mutex;
    bool session = false;
    unsigned gpuContexts = 0;
    int appliedMode = -1;
    int lastAttempt = -1;
    bool lastAttemptFailed = false;
};

ResourcePolicy* State() noexcept
{
    // Backends may be process-lifetime objects. Keep their policy alive during
    // static destruction too. On allocation failure the entire policy stays
    // disabled, so it cannot enable FastLoad without tracking GPU contexts.
    static ResourcePolicy* const state = new (std::nothrow) ResourcePolicy;
    return state;
}

bool Apply(ResourcePolicy& state) noexcept
{
    const ApmCpuBoostMode mode = state.session &&
        softwareComposition.load(std::memory_order_acquire) && state.gpuContexts == 0
        ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal;
    if (state.appliedMode == static_cast<int>(mode)) return true;
    if (mode == ApmCpuBoostMode_FastLoad &&
        state.lastAttempt == static_cast<int>(mode) && state.lastAttemptFailed)
        return false;
    // Like the reference, request only at renderer/session transitions; a
    // service unavailable in applet mode must not be retried on every frame.
    state.lastAttempt = static_cast<int>(mode);
    const Result result = appletSetCpuBoostMode(mode);
    state.lastAttemptFailed = R_FAILED(result);
    if (R_SUCCEEDED(result)) state.appliedMode = static_cast<int>(mode);
    KRKRNS_LOG("[scheduler] cpu-mode=%s session=%d gpu-contexts=%u rc=%x",
        mode == ApmCpuBoostMode_FastLoad ? "software-fastload" : "normal",
        state.session, state.gpuContexts, result);
    // Older/app-restricted systems may not expose the service at all. GPU
    // use is safe there if this controller never successfully enabled boost.
    // A failed Normal request after successful FastLoad must block GPU use.
    return R_SUCCEEDED(result) ||
        (mode == ApmCpuBoostMode_Normal &&
         state.appliedMode != static_cast<int>(ApmCpuBoostMode_FastLoad));
}
}

bool krkrsdl2_force_software_composition() noexcept
{
    return softwareComposition.load(std::memory_order_acquire);
}

unsigned krkrsdl2_resource_policy_frame_rate() noexcept
{
    return frameRate.load(std::memory_order_acquire);
}

void krkrsdl2_resource_policy_select_renderer(const char* native_game_dir) noexcept
{
    // Resolve once per game before begin_session, as NX does before creating
    // its render manager. Finished SDL texture display remains common to both.
    softwareComposition.store(true, std::memory_order_release);
    frameRate.store(60, std::memory_order_release);
    try {
        const auto settings = ReadReferenceRendererSettings(native_game_dir);
        krkrsdl2_apply_reference_memory_settings(settings.memoryUsage.c_str(), settings.memorySource.c_str());
        krkrsdl2_apply_reference_split_settings(settings.renderer == "software");
        const unsigned selectedRate = settings.fpsLimit > 0
            ? static_cast<unsigned>(settings.fpsLimit) : 60;
        frameRate.store(selectedRate, std::memory_order_release);
        KRKRNS_LOG("[renderer] fps_limit=%u source=%s", selectedRate, settings.fpsSource.c_str());
        if (settings.renderer == "opengl") {
            softwareComposition.store(false, std::memory_order_release);
            KRKRNS_LOG("[renderer] renderer=opengl source=%s; available GPU paths with CPU fallback",
                settings.rendererSource.c_str());
        } else if (settings.renderer == "software") {
            KRKRNS_LOG("[renderer] renderer=software source=%s", settings.rendererSource.c_str());
        } else {
            KRKRNS_LOG("[renderer] unsupported renderer=%s source=%s; using software fallback",
                settings.renderer.c_str(), settings.rendererSource.c_str());
        }
    } catch (...) {
		krkrsdl2_restore_reference_split_settings();
		krkrsdl2_restore_reference_memory_settings();
        KRKRNS_LOG("[renderer] cannot read reference renderer settings; using software fallback");
    }
}

void krkrsdl2_resource_policy_begin_session() noexcept
{
    if (auto* state = State()) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->session) state->lastAttemptFailed = false;
        state->session = true;
        Apply(*state);
    }
}

void krkrsdl2_resource_policy_end_session() noexcept
{
    krkrsdl2_restore_reference_split_settings();
    krkrsdl2_restore_reference_memory_settings();
    if (auto* state = State()) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->session = false;
        Apply(*state);
    }
    softwareComposition.store(true, std::memory_order_release);
}

bool krkrsdl2_resource_policy_acquire_gpu() noexcept
{
    if (krkrsdl2_force_software_composition()) return false;
    if (auto* state = State()) {
        std::lock_guard<std::mutex> lock(state->mutex);
        // This is a real renderer transition, not a per-frame retry. Even
        // if Normal is already applied it ends a previous failed boost try.
        state->lastAttemptFailed = false;
        ++state->gpuContexts;
        if (!Apply(*state)) {
            --state->gpuContexts;
            return false;
        }
    }
    return true;
}

void krkrsdl2_resource_policy_release_gpu() noexcept
{
    if (auto* state = State()) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->gpuContexts) --state->gpuContexts;
        Apply(*state);
    }
}
#endif
