/* SPDX-License-Identifier: MIT */
#include "KrkrNSResourcePolicy.h"
#ifdef __SWITCH__
#include "KrkrNSLog.h"
#include "KrkrNSPaths.h"
#include "ReferenceRendererConfig.h"
#include <switch.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <new>

namespace
{
std::atomic<bool> softwareComposition{true};
// Hybrid mode: composition stays on the CPU, the E-mote raster may use the GPU.
std::atomic<bool> emoteGpuAllowed{false};
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

bool krkrsdl2_emote_gpu_allowed() noexcept
{
    return emoteGpuAllowed.load(std::memory_order_acquire);
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
    emoteGpuAllowed.store(false, std::memory_order_release);
    frameRate.store(60, std::memory_order_release);
    try {
        const auto settings = ReadReferenceRendererSettings(native_game_dir);
        krkrsdl2_apply_reference_memory_settings(settings.memoryUsage.c_str(), settings.memorySource.c_str());
        const unsigned selectedRate = settings.fpsLimit > 0
            ? static_cast<unsigned>(settings.fpsLimit) : 60;
        frameRate.store(selectedRate, std::memory_order_release);
        KRKRNS_LOG("[renderer] fps_limit=%u source=%s", selectedRate, settings.fpsSource.c_str());

        // KRKR-ns (2026-11-04): the hybrid E-mote GPU raster is the default, so
        // no marker is needed.  Two markers remain for the whole console:
        //   sdmc:/switch/KRKR-ns/renderer-cpu.txt  pure software (no GPU at all)
        //   sdmc:/switch/KRKR-ns/renderer-gl.txt   full GPU composition
        static const char *const markerCpu = KRKRNS_BASE_A "/renderer-cpu.txt";
        static const char *const markerGl = KRKRNS_BASE_A "/renderer-gl.txt";
        std::string renderer = settings.renderer;
        std::string rendererSource = settings.rendererSource;
        FILE* marker = nullptr;
        if ((marker = fopen(markerCpu, "rb")) != nullptr)
        {
            fclose(marker);
            renderer = "software";
            rendererSource = "renderer-cpu.txt";
        }
        else if ((marker = fopen(markerGl, "rb")) != nullptr)
        {
            fclose(marker);
            renderer = "opengl";
            rendererSource = "renderer-gl.txt";
        }

        // Stripe splitting matches the reference per-renderer, so it must follow
        // the resolved renderer, not the raw preference.
        krkrsdl2_apply_reference_split_settings(renderer == "software");

        if (renderer == "opengl") {
            softwareComposition.store(false, std::memory_order_release);
            emoteGpuAllowed.store(true, std::memory_order_release);
            KRKRNS_LOG("[renderer] renderer=opengl source=%s; available GPU paths with CPU fallback",
                rendererSource.c_str());
        } else if (renderer == "software") {
            // Hybrid mode (KRKR-ns 2026-11-04) is the default: keep the CPU
            // compositor -- dialogue frames are many small layer updates, which
            // the GPU direct path does not win, and GPU composition loses layer
            // content on some titles -- but let the E-mote raster use the GPU,
            // where the 1080p mesh rasterization is the dominant cost.
            // An explicit software choice (game preference or renderer-cpu.txt)
            // still means pure CPU; emote-gl.txt forces the hybrid back on.
            FILE* hybrid = fopen(KRKRNS_BASE_A "/emote-gl.txt", "rb");
            bool explicitSoftware = rendererSource != "default";
            if (hybrid != nullptr)
            {
                fclose(hybrid);
                explicitSoftware = false;
            }
            if (!explicitSoftware)
            {
                emoteGpuAllowed.store(true, std::memory_order_release);
                KRKRNS_LOG("[renderer] renderer=software source=%s; E-mote GPU raster enabled (hybrid default)",
                    rendererSource.c_str());
            }
            else
            {
                KRKRNS_LOG("[renderer] renderer=software source=%s; E-mote CPU raster",
                    rendererSource.c_str());
            }
        } else {
            KRKRNS_LOG("[renderer] unsupported renderer=%s source=%s; using software fallback",
                renderer.c_str(), rendererSource.c_str());
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
    // Software composition denies GPU composition, but not the hybrid E-mote
    // raster: in that mode the GPU owns only the character targets.
    if (krkrsdl2_force_software_composition() && !krkrsdl2_emote_gpu_allowed()) return false;
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
