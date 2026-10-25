/* SPDX-License-Identifier: MIT */
#include "KrkrNSResourcePolicy.h"
#ifdef __SWITCH__
#include "KrkrNSLog.h"
#include <switch.h>
#include <mutex>
#include <new>

namespace
{
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
    const ApmCpuBoostMode mode = state.session && state.gpuContexts == 0
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
    if (auto* state = State()) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->session = false;
        Apply(*state);
    }
}

bool krkrsdl2_resource_policy_acquire_gpu() noexcept
{
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
