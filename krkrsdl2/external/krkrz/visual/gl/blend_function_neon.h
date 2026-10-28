/* SPDX-License-Identifier: MIT */
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __SWITCH__
#include <arm_neon.h>
#else
#include "simde/arm/neon.h"
#endif

// Kirikiroid2-NX processes eight ARGB pixels per NEON instruction group for
// ConstAlphaBlend_SD. Keep this engine's existing scalar function for tails
// and unusual opacity values, and do not change the order of overlapping rows.
template<typename Functor>
static inline bool TVPConstAlphaBlendSDNEON(tjs_uint32* dest,
    const tjs_uint32* src1, const tjs_uint32* src2, tjs_int len,
    tjs_int opa, const Functor& scalar)
{
    if (len < 8 || opa < 0 || opa > 255) return false;

    const auto dst = reinterpret_cast<std::uintptr_t>(dest);
    const std::size_t bytes = static_cast<std::size_t>(len) * sizeof(*dest);
    const auto overlapsShifted = [dst, bytes](const tjs_uint32* source) {
        const auto address = reinterpret_cast<std::uintptr_t>(source);
        if (address == dst) return false;
        return address < dst ? dst - address < bytes : address - dst < bytes;
    };
    if (overlapsShifted(src1) || overlapsShifted(src2)) return false;

    const uint8x8_t alpha = vdup_n_u8(static_cast<std::uint8_t>(opa));
    while (len >= 8)
    {
        const uint8x8x4_t first = vld4_u8(reinterpret_cast<const std::uint8_t*>(src1));
        const uint8x8x4_t second = vld4_u8(reinterpret_cast<const std::uint8_t*>(src2));
        uint8x8x4_t output;
        for (unsigned channel = 0; channel < 4; ++channel)
            output.val[channel] = vadd_u8(first.val[channel],
                vsubhn_u16(vmull_u8(second.val[channel], alpha),
                           vmull_u8(first.val[channel], alpha)));
        vst4_u8(reinterpret_cast<std::uint8_t*>(dest), output);
        dest += 8;
        src1 += 8;
        src2 += 8;
        len -= 8;
    }
    while (len-- > 0)
    {
        *dest = scalar(*src1, *src2);
        ++dest;
        ++src1;
        ++src2;
    }
    return true;
}

// Retain the SSE2 scalar edges and four-pixel groups: opaque shortcuts and
// opacity rounding in the old scalar path differ from the vector path.
template<bool HoldAlpha, bool UseOpacity, bool SourceBranch, typename Functor>
static inline void TVPAlphaBlendNEON(tjs_uint32* dest, const tjs_uint32* src,
                                    tjs_int len, tjs_int opa, const Functor& func)
{
    if (len <= 0) return;
    while (len > 0 && (reinterpret_cast<size_t>(dest) & 15) != 0)
    {
        *dest = func(*dest, *src);
        ++dest;
        ++src;
        --len;
    }

    const uint8_t alphaIndices[16] = {
        3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15
    };
    const uint8x16_t indices = vld1q_u8(alphaIndices);
    const uint16x8_t opacity = vdupq_n_u16(static_cast<uint16_t>(opa));
    const uint32x4_t alphaMask = vdupq_n_u32(0xff000000);
    while (len >= 4)
    {
        const uint32x4_t source = vld1q_u32(src);
        if (SourceBranch)
        {
            const uint32x4_t alpha = vshrq_n_u32(source, 24);
            if (vminvq_u32(alpha) == 255)
            {
                vst1q_u32(dest, source);
                dest += 4;
                src += 4;
                len -= 4;
                continue;
            }
            if (vmaxvq_u32(alpha) == 0)
            {
                dest += 4;
                src += 4;
                len -= 4;
                continue;
            }
        }
#ifdef KRKRNS_ALPHA_HOT_NEON
        else
        {
            const tjs_uint32 maxSourceAlpha = vmaxvq_u32(vshrq_n_u32(source, 24));
            if ((!UseOpacity && !maxSourceAlpha) ||
                (UseOpacity && maxSourceAlpha * tjs_uint32(opa) < 256))
            {
                dest += 4;
                src += 4;
                len -= 4;
                continue;
            }
        }
#endif

        const uint32x4_t destination = vld1q_u32(dest);
        const uint8x16_t s = vreinterpretq_u8_u32(source);
        const uint8x16_t d = vreinterpretq_u8_u32(destination);
        const uint8x16_t alpha = vqtbl1q_u8(s, indices);
        uint16x8_t a0 = vmovl_u8(vget_low_u8(alpha));
        uint16x8_t a1 = vmovl_u8(vget_high_u8(alpha));
        if (UseOpacity)
        {
            a0 = vshrq_n_u16(vmulq_u16(a0, opacity), 8);
            a1 = vshrq_n_u16(vmulq_u16(a1, opacity), 8);
        }
        const uint16x8_t d0 = vmovl_u8(vget_low_u8(d));
        const uint16x8_t d1 = vmovl_u8(vget_high_u8(d));
        const uint16x8_t s0 = vmovl_u8(vget_low_u8(s));
        const uint16x8_t s1 = vmovl_u8(vget_high_u8(s));
        // Modular 16-bit arithmetic is exact for d * 256 + (s - d) * a.
        const uint16x8_t r0 = vmlaq_u16(vshlq_n_u16(d0, 8), vsubq_u16(s0, d0), a0);
        const uint16x8_t r1 = vmlaq_u16(vshlq_n_u16(d1, 8), vsubq_u16(s1, d1), a1);
        uint32x4_t result = vreinterpretq_u32_u8(
            vcombine_u8(vshrn_n_u16(r0, 8), vshrn_n_u16(r1, 8)));
        if (HoldAlpha)
            result = vbslq_u32(alphaMask, destination, result);
        vst1q_u32(dest, result);
        dest += 4;
        src += 4;
        len -= 4;
    }
    while (len-- > 0)
    {
        *dest = func(*dest, *src);
        ++dest;
        ++src;
    }
}
