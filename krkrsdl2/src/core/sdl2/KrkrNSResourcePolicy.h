/* SPDX-License-Identifier: MIT */
#pragma once

#ifdef __SWITCH__
// Resolve the reference's per-game/global renderer before starting a session.
// The launcher and sessions with no configured renderer use software.
void krkrsdl2_resource_policy_select_renderer(const char* native_game_dir) noexcept;
bool krkrsdl2_force_software_composition() noexcept;
unsigned krkrsdl2_resource_policy_frame_rate() noexcept;
void krkrsdl2_resource_policy_begin_session() noexcept;
void krkrsdl2_resource_policy_end_session() noexcept;
bool krkrsdl2_resource_policy_acquire_gpu() noexcept;
void krkrsdl2_resource_policy_release_gpu() noexcept;
#else
inline void krkrsdl2_resource_policy_select_renderer(const char*) noexcept {}
inline bool krkrsdl2_force_software_composition() noexcept { return false; }
inline unsigned krkrsdl2_resource_policy_frame_rate() noexcept { return 60; }
inline void krkrsdl2_resource_policy_begin_session() noexcept {}
inline void krkrsdl2_resource_policy_end_session() noexcept {}
inline bool krkrsdl2_resource_policy_acquire_gpu() noexcept { return true; }
inline void krkrsdl2_resource_policy_release_gpu() noexcept {}
#endif

// Kirikiroid2-NX uses FastLoad only with its software render manager. GPU
// composition is allowed only by the selected OpenGL mode, and live resources
// also prevent lowering the GPU clock during renderer/session transitions.
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
