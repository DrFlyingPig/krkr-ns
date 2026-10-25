/* SPDX-License-Identifier: MIT */
#pragma once

#ifdef __SWITCH__
void krkrsdl2_resource_policy_begin_session() noexcept;
void krkrsdl2_resource_policy_end_session() noexcept;
bool krkrsdl2_resource_policy_acquire_gpu() noexcept;
void krkrsdl2_resource_policy_release_gpu() noexcept;
#else
inline void krkrsdl2_resource_policy_begin_session() noexcept {}
inline void krkrsdl2_resource_policy_end_session() noexcept {}
inline bool krkrsdl2_resource_policy_acquire_gpu() noexcept { return true; }
inline void krkrsdl2_resource_policy_release_gpu() noexcept {}
#endif

// Kirikiroid2-NX uses FastLoad only with its software render manager. Our
// E-mote GL backend can coexist with CPU Layers, so its live context keeps
// the process in Normal mode until that context is retired.
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
