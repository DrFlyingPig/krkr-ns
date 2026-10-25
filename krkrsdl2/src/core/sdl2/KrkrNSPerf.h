/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstdint>

enum class KrkrNSPerfOp : unsigned
{
    GraphicLoad, ImageDecode, XP3Read, DiskRead, Inflate, BitmapDetach,
    ThreadWait, LayerPrepare, LayerRaster, LayerFinish, Dispatch, Present,
    BitmapBlt, BlendPixels, BitmapCopy, CopyPixels,
    TextureUpload, PsbParse, PsbCommit, PsbExpand,
    AlphaPixels, AlphaHdaPixels, AlphaOpacityPixels, AlphaHdaOpacityPixels,
    PsbRelease,
    LayerOnPaint, LayerTransitionStart, LayerTransitionChildren, LayerPrepareTree,
    EmoteReadback, GpuReadback, EmoteLayerBridge,
    EmoteConvert, BitmapAffine, BitmapAffineRaster,
    PsbRead, PsbTables, PsbBuild,
    SurfaceDamageBounds, SurfaceCopy,
    ScriptEvent, InputEvent, ContinuousEvent, WindowUpdate,
    IdleCompact, IdleRehash,
    GpuTextureInit,
    AutoPathValidate, AutoPathRebuild,
    GpuDamageCompare, GpuDamageMap, GpuResidentCopy,
    Count
};

#if defined(__SWITCH__) || defined(KRKRNS_PERF_TEST)
void krkrsdl2_perf_init() noexcept;
bool krkrsdl2_perf_enabled() noexcept;
void krkrsdl2_perf_loop(double ms, bool presented) noexcept;
void krkrsdl2_perf_input() noexcept;
void krkrsdl2_perf_present() noexcept;
void krkrsdl2_perf_report() noexcept;
void krkrsdl2_perf_cache(bool hit) noexcept;
void krkrsdl2_perf_bytes(KrkrNSPerfOp operation, std::uint64_t bytes) noexcept;

class KrkrNSPerfScope
{
    KrkrNSPerfOp operation_;
    KrkrNSPerfScope* parent_ = nullptr;
    std::uint64_t start_ = 0, children_ = 0, bytes_ = 0;
    unsigned domain_ = 0;
    bool linked_ = false;
public:
    explicit KrkrNSPerfScope(KrkrNSPerfOp operation, std::uint64_t bytes = 0) noexcept;
    KrkrNSPerfScope(const KrkrNSPerfScope&) = delete;
    ~KrkrNSPerfScope() noexcept;
    void Finish() noexcept;
    void Bytes(std::uint64_t bytes) noexcept { bytes_ = bytes; }
};

#ifdef KRKRNS_PERF_TEST
struct KrkrNSPerfTestRecord { std::uint64_t n, inclusive, self, bytes; };
void krkrsdl2_perf_test_enable(bool) noexcept;
void krkrsdl2_perf_test_clock(std::uint64_t) noexcept;
KrkrNSPerfTestRecord krkrsdl2_perf_test_record(KrkrNSPerfOp) noexcept;
#endif
#else
inline void krkrsdl2_perf_init() noexcept {}
inline bool krkrsdl2_perf_enabled() noexcept { return false; }
inline void krkrsdl2_perf_loop(double, bool) noexcept {}
inline void krkrsdl2_perf_input() noexcept {}
inline void krkrsdl2_perf_present() noexcept {}
inline void krkrsdl2_perf_report() noexcept {}
inline void krkrsdl2_perf_cache(bool) noexcept {}
inline void krkrsdl2_perf_bytes(KrkrNSPerfOp, std::uint64_t) noexcept {}
class KrkrNSPerfScope
{
public:
    explicit KrkrNSPerfScope(KrkrNSPerfOp, std::uint64_t = 0) noexcept {}
    void Finish() noexcept {}
    void Bytes(std::uint64_t) noexcept {}
};
#endif
