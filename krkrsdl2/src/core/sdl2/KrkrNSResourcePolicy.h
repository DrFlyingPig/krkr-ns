/* SPDX-License-Identifier: MIT */
#pragma once

#include <cstdint>

#ifdef __SWITCH__
// Resolve the reference's per-game/global renderer before starting a session.
// The launcher and sessions with no configured renderer use software.
void krkrsdl2_resource_policy_select_renderer(const char* native_game_dir) noexcept;
void krkrsdl2_apply_reference_memory_settings(const char* usage, const char* source);
void krkrsdl2_restore_reference_memory_settings() noexcept;
void krkrsdl2_apply_reference_split_settings(bool software);
void krkrsdl2_restore_reference_split_settings() noexcept;
bool krkrsdl2_force_software_composition() noexcept;
// Retained decoded-pixel budget for the GPU compositor's loaded sources, in
// bytes.  Zero keeps every CPU mirror (the behaviour before 2026-11-07).
std::uint64_t krkrsdl2_gpu_cpu_mirror_budget() noexcept;
// KRKR-ns hybrid mode: software composition stays in charge of the final frame,
// but the E-mote raster may use the GPU (see emote-gl.txt / renderer-gl.txt).
bool krkrsdl2_emote_gpu_allowed() noexcept;
unsigned krkrsdl2_resource_policy_frame_rate() noexcept;
void krkrsdl2_resource_policy_begin_session() noexcept;
void krkrsdl2_resource_policy_end_session() noexcept;
bool krkrsdl2_resource_policy_acquire_gpu() noexcept;
void krkrsdl2_resource_policy_release_gpu() noexcept;
#else
inline void krkrsdl2_resource_policy_select_renderer(const char*) noexcept {}
inline bool krkrsdl2_force_software_composition() noexcept { return false; }
inline std::uint64_t krkrsdl2_gpu_cpu_mirror_budget() noexcept { return 0; }
inline bool krkrsdl2_emote_gpu_allowed() noexcept { return false; }
inline unsigned krkrsdl2_resource_policy_frame_rate() noexcept { return 60; }
inline void krkrsdl2_resource_policy_begin_session() noexcept {}
inline void krkrsdl2_resource_policy_end_session() noexcept {}
inline bool krkrsdl2_resource_policy_acquire_gpu() noexcept { return true; }
inline void krkrsdl2_resource_policy_release_gpu() noexcept {}
#endif

// FastLoad applies only with the software render manager. GPU composition is
// allowed only by the selected OpenGL mode, and live resources also prevent
// lowering the GPU clock during renderer/session transitions.
class KrkrNSGpuResourceLease
{
    bool held_ = false;
public:
    KrkrNSGpuResourceLease() = default;
    KrkrNSGpuResourceLease(const KrkrNSGpuResourceLease&) = delete;
    KrkrNSGpuResourceLease& operator=(const KrkrNSGpuResourceLease&) = delete;
    ~KrkrNSGpuResourceLease() { Release(); }
    bool Acquire() noexcept
    {
        if (held_) return true;
        if (!krkrsdl2_resource_policy_acquire_gpu()) return false;
        held_ = true;
        return true;
    }
    void Release() noexcept
    {
        if (!held_) return;
        held_ = false;
        krkrsdl2_resource_policy_release_gpu();
    }
};
