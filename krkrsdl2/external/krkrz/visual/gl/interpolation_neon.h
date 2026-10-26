/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstddef>
#include <cstdint>

#ifdef __SWITCH__
#include <arm_neon.h>
#else
#include "simde/arm/neon.h"
#endif

static inline uint32x4_t TVPBlendARGB4NEON(uint32x4_t b, uint32x4_t a,
                                         uint32x4_t ratio)
{
    const uint32x4_t mask = vdupq_n_u32(0x00ff00ff);
    const uint32x4_t b0 = vandq_u32(b, mask);
    const uint32x4_t t = vandq_u32(vaddq_u32(b0, vshrq_n_u32(
        vmulq_u32(vsubq_u32(vandq_u32(a, mask), b0), ratio), 8)), mask);
    const uint32x4_t b1 = vandq_u32(vshrq_n_u32(b, 8), mask);
    const uint32x4_t u = vaddq_u32(b1, vshrq_n_u32(vmulq_u32(
        vsubq_u32(vandq_u32(vshrq_n_u32(a, 8), mask), b1), ratio), 8));
    return vorrq_u32(t, vbicq_u32(vshlq_n_u32(u, 8), mask));
}

static inline void TVPInterpolationLoadPairsNEON(
    const tjs_uint32* p0, const tjs_uint32* p1,
    const tjs_uint32* p2, const tjs_uint32* p3,
    uint32x4_t& left, uint32x4_t& right)
{
    // Each sample has exactly two valid neighbors, including at the right edge.
    const uint32x4_t a = vcombine_u32(vld1_u32(p0), vld1_u32(p1));
    const uint32x4_t b = vcombine_u32(vld1_u32(p2), vld1_u32(p3));
    left = vuzp1q_u32(a, b);
    right = vuzp2q_u32(a, b);
}

static inline bool TVPInterpolationOverlapNEON(
    const tjs_uint32* dest, tjs_int len, const tjs_uint32* src,
    std::intptr_t firstByte, std::intptr_t lastByte)
{
    const std::uintptr_t d = reinterpret_cast<std::uintptr_t>(dest);
    const std::uintptr_t s = reinterpret_cast<std::uintptr_t>(src);
    return d < s + lastByte && s + firstByte < d + std::size_t(len) * 4;
}

static inline void TVPInterpStretchCopyNEON(
    tjs_uint32* dest, tjs_int len, const tjs_uint32* src1,
    const tjs_uint32* src2, tjs_int blend_y, tjs_int sx, tjs_int stepx)
{
    if (len <= 0) return;
    const std::int64_t last = std::int64_t(sx) + std::int64_t(len - 1) * stepx;
    const std::intptr_t x0 = (sx < last ? sx : last) >> 16;
    const std::intptr_t x1 = (sx > last ? sx : last) >> 16;
    const bool overlap = TVPInterpolationOverlapNEON(dest, len, src1, x0 * 4, (x1 + 2) * 4) ||
        TVPInterpolationOverlapNEON(dest, len, src2, x0 * 4, (x1 + 2) * 4);
    blend_y += blend_y >> 7;
    const uint32x4_t by = vdupq_n_u32(static_cast<tjs_uint32>(blend_y));
    while (!overlap && len >= 4)
    {
        const tjs_uint32* top[4];
        const tjs_uint32* bottom[4];
        tjs_uint32 bx[4];
        for (int i = 0; i < 4; ++i)
        {
            const tjs_int x = sx >> 16;
            top[i] = src1 + x;
            bottom[i] = src2 + x;
            bx[i] = (sx & 0xffff) >> 8;
            sx += stepx;
        }
        uint32x4_t tl, tr, bl, br;
        TVPInterpolationLoadPairsNEON(top[0], top[1], top[2], top[3], tl, tr);
        TVPInterpolationLoadPairsNEON(bottom[0], bottom[1], bottom[2], bottom[3], bl, br);
        const uint32x4_t x = vld1q_u32(bx);
        // Keep the horizontal truncation before the vertical interpolation.
        vst1q_u32(dest, TVPBlendARGB4NEON(TVPBlendARGB4NEON(tl, tr, x),
                                         TVPBlendARGB4NEON(bl, br, x), by));
        dest += 4;
        len -= 4;
    }
    while (len-- > 0)
    {
        const tjs_int x = sx >> 16;
        const tjs_int bx = (sx & 0xffff) >> 8;
        *dest++ = TVPBlendARGB(TVPBlendARGB(src1[x], src1[x + 1], bx),
                              TVPBlendARGB(src2[x], src2[x + 1], bx), blend_y);
        sx += stepx;
    }
}

static inline void TVPInterpLinTransCopyNEON(
    tjs_uint32* dest, tjs_int len, const tjs_uint32* src,
    tjs_int sx, tjs_int sy, tjs_int stepx, tjs_int stepy, tjs_int srcpitch)
{
    if (len <= 0) return;
    const std::int64_t lastx = std::int64_t(sx) + std::int64_t(len - 1) * stepx;
    const std::int64_t lasty = std::int64_t(sy) + std::int64_t(len - 1) * stepy;
    const std::intptr_t x0 = (sx < lastx ? sx : lastx) >> 16;
    const std::intptr_t x1 = (sx > lastx ? sx : lastx) >> 16;
    const std::intptr_t y0 = (sy < lasty ? sy : lasty) >> 16;
    const std::intptr_t y1 = (sy > lasty ? sy : lasty) >> 16;
    const std::intptr_t firstRow = (srcpitch < 0 ? y1 + 1 : y0) * srcpitch;
    const std::intptr_t lastRow = (srcpitch < 0 ? y0 : y1 + 1) * srcpitch;
    const bool overlap = TVPInterpolationOverlapNEON(dest, len, src,
        firstRow + x0 * 4, lastRow + (x1 + 2) * 4);
    while (!overlap && len >= 4)
    {
        const tjs_uint32* top[4];
        const tjs_uint32* bottom[4];
        tjs_uint32 bx[4], by[4];
        for (int i = 0; i < 4; ++i)
        {
            const auto* row = reinterpret_cast<const tjs_uint8*>(src) +
                std::intptr_t(sy >> 16) * srcpitch;
            top[i] = reinterpret_cast<const tjs_uint32*>(row) + (sx >> 16);
            bottom[i] = reinterpret_cast<const tjs_uint32*>(
                reinterpret_cast<const tjs_uint8*>(top[i]) + srcpitch);
            bx[i] = (sx & 0xffff) >> 8;
            by[i] = (sy & 0xffff) >> 8;
            bx[i] += bx[i] >> 7;
            by[i] += by[i] >> 7;
            sx += stepx;
            sy += stepy;
        }
        uint32x4_t tl, tr, bl, br;
        TVPInterpolationLoadPairsNEON(top[0], top[1], top[2], top[3], tl, tr);
        TVPInterpolationLoadPairsNEON(bottom[0], bottom[1], bottom[2], bottom[3], bl, br);
        const uint32x4_t x = vld1q_u32(bx);
        vst1q_u32(dest, TVPBlendARGB4NEON(TVPBlendARGB4NEON(tl, tr, x),
                                         TVPBlendARGB4NEON(bl, br, x), vld1q_u32(by)));
        dest += 4;
        len -= 4;
    }
    while (len-- > 0)
    {
        tjs_int bx = (sx & 0xffff) >> 8;
        tjs_int by = (sy & 0xffff) >> 8;
        bx += bx >> 7;
        by += by >> 7;
        const auto* row = reinterpret_cast<const tjs_uint8*>(src) +
            std::intptr_t(sy >> 16) * srcpitch;
        const auto* p0 = reinterpret_cast<const tjs_uint32*>(row) + (sx >> 16);
        const auto* p1 = reinterpret_cast<const tjs_uint32*>(
            reinterpret_cast<const tjs_uint8*>(p0) + srcpitch);
        *dest++ = TVPBlendARGB(TVPBlendARGB(p0[0], p0[1], bx),
                              TVPBlendARGB(p1[0], p1[1], bx), by);
        sx += stepx;
        sy += stepy;
    }
}

// The reference ARM renderer also vectorizes the two interpolating blend
// operations below. Reuse the bounded two-neighbor gathers, retaining this
// engine's packed-channel rounding and source-alpha result exactly.
static inline uint32x4_t TVPAdditiveARGB4NEON(uint32x4_t dest, uint32x4_t source)
{
    const uint32x4_t inverse = vshrq_n_u32(vmvnq_u32(source), 24);
    const uint32x4_t rb = vdupq_n_u32(0x00ff00ff);
    const uint32x4_t g = vdupq_n_u32(0x0000ff00);
    const uint32x4_t scaled = vorrq_u32(
        vandq_u32(vshrq_n_u32(vmulq_u32(vandq_u32(dest, rb), inverse), 8), rb),
        vandq_u32(vshrq_n_u32(vmulq_u32(vandq_u32(dest, g), inverse), 8), g));
    return vreinterpretq_u32_u8(vqaddq_u8(vreinterpretq_u8_u32(scaled),
                                         vreinterpretq_u8_u32(source)));
}

static inline void TVPInterpStretchAdditiveAlphaBlendNEON(
    tjs_uint32* dest, tjs_int len, const tjs_uint32* src1,
    const tjs_uint32* src2, tjs_int blend_y, tjs_int sx, tjs_int stepx)
{
    if (len <= 0) return;
    const std::int64_t last = std::int64_t(sx) + std::int64_t(len - 1) * stepx;
    const std::intptr_t x0 = (sx < last ? sx : last) >> 16;
    const std::intptr_t x1 = (sx > last ? sx : last) >> 16;
    const bool overlap = TVPInterpolationOverlapNEON(dest, len, src1, x0 * 4, (x1 + 2) * 4) ||
        TVPInterpolationOverlapNEON(dest, len, src2, x0 * 4, (x1 + 2) * 4);
    blend_y += blend_y >> 7;
    const uint32x4_t by = vdupq_n_u32(static_cast<tjs_uint32>(blend_y));
    while (!overlap && len >= 4)
    {
        const tjs_uint32* top[4];
        const tjs_uint32* bottom[4];
        tjs_uint32 bx[4];
        for (int i = 0; i < 4; ++i)
        {
            const tjs_int x = sx >> 16;
            top[i] = src1 + x;
            bottom[i] = src2 + x;
            bx[i] = (sx & 0xffff) >> 8;
            sx += stepx;
        }
        uint32x4_t tl, tr, bl, br;
        TVPInterpolationLoadPairsNEON(top[0], top[1], top[2], top[3], tl, tr);
        TVPInterpolationLoadPairsNEON(bottom[0], bottom[1], bottom[2], bottom[3], bl, br);
        const uint32x4_t x = vld1q_u32(bx);
        const uint32x4_t source = TVPBlendARGB4NEON(TVPBlendARGB4NEON(tl, tr, x),
                                                   TVPBlendARGB4NEON(bl, br, x), by);
        vst1q_u32(dest, TVPAdditiveARGB4NEON(vld1q_u32(dest), source));
        dest += 4;
        len -= 4;
    }
    while (len-- > 0)
    {
        const tjs_int x = sx >> 16;
        const tjs_int bx = (sx & 0xffff) >> 8;
        *dest = TVPAddAlphaBlend_n_a(*dest,
            TVPBlendARGB(TVPBlendARGB(src1[x], src1[x + 1], bx),
                         TVPBlendARGB(src2[x], src2[x + 1], bx), blend_y));
        ++dest;
        sx += stepx;
    }
}

static inline void TVPInterpLinTransConstAlphaBlendNEON(
    tjs_uint32* dest, tjs_int len, const tjs_uint32* src,
    tjs_int sx, tjs_int sy, tjs_int stepx, tjs_int stepy, tjs_int srcpitch, tjs_int opacity)
{
    if (len <= 0) return;
    const std::int64_t lastx = std::int64_t(sx) + std::int64_t(len - 1) * stepx;
    const std::int64_t lasty = std::int64_t(sy) + std::int64_t(len - 1) * stepy;
    const std::intptr_t x0 = (sx < lastx ? sx : lastx) >> 16;
    const std::intptr_t x1 = (sx > lastx ? sx : lastx) >> 16;
    const std::intptr_t y0 = (sy < lasty ? sy : lasty) >> 16;
    const std::intptr_t y1 = (sy > lasty ? sy : lasty) >> 16;
    const std::intptr_t firstRow = (srcpitch < 0 ? y1 + 1 : y0) * srcpitch;
    const std::intptr_t lastRow = (srcpitch < 0 ? y0 : y1 + 1) * srcpitch;
    const bool overlap = TVPInterpolationOverlapNEON(dest, len, src,
        firstRow + x0 * 4, lastRow + (x1 + 2) * 4);
    const bool vectorize = !overlap && opacity >= 0 && opacity <= 255;
    opacity += opacity >> 7;
    const uint32x4_t opa = vdupq_n_u32(static_cast<tjs_uint32>(opacity));
    while (vectorize && len >= 4)
    {
        const tjs_uint32* top[4];
        const tjs_uint32* bottom[4];
        tjs_uint32 bx[4], by[4];
        for (int i = 0; i < 4; ++i)
        {
            const auto* row = reinterpret_cast<const tjs_uint8*>(src) +
                std::intptr_t(sy >> 16) * srcpitch;
            top[i] = reinterpret_cast<const tjs_uint32*>(row) + (sx >> 16);
            bottom[i] = reinterpret_cast<const tjs_uint32*>(
                reinterpret_cast<const tjs_uint8*>(top[i]) + srcpitch);
            bx[i] = (sx & 0xffff) >> 8;
            by[i] = (sy & 0xffff) >> 8;
            bx[i] += bx[i] >> 7;
            by[i] += by[i] >> 7;
            sx += stepx;
            sy += stepy;
        }
        uint32x4_t tl, tr, bl, br;
        TVPInterpolationLoadPairsNEON(top[0], top[1], top[2], top[3], tl, tr);
        TVPInterpolationLoadPairsNEON(bottom[0], bottom[1], bottom[2], bottom[3], bl, br);
        const uint32x4_t x = vld1q_u32(bx);
        const uint32x4_t source = TVPBlendARGB4NEON(TVPBlendARGB4NEON(tl, tr, x),
            TVPBlendARGB4NEON(bl, br, x), vld1q_u32(by));
        vst1q_u32(dest, TVPBlendARGB4NEON(vld1q_u32(dest), source, opa));
        dest += 4;
        len -= 4;
    }
    while (len-- > 0)
    {
        tjs_int bx = (sx & 0xffff) >> 8;
        tjs_int by = (sy & 0xffff) >> 8;
        bx += bx >> 7;
        by += by >> 7;
        const auto* row = reinterpret_cast<const tjs_uint8*>(src) +
            std::intptr_t(sy >> 16) * srcpitch;
        const auto* p0 = reinterpret_cast<const tjs_uint32*>(row) + (sx >> 16);
        const auto* p1 = reinterpret_cast<const tjs_uint32*>(
            reinterpret_cast<const tjs_uint8*>(p0) + srcpitch);
        *dest = TVPBlendARGB(*dest, TVPBlendARGB(TVPBlendARGB(p0[0], p0[1], bx),
                             TVPBlendARGB(p1[0], p1[1], bx), by), opacity);
        ++dest;
        sx += stepx;
        sy += stepy;
    }
}
