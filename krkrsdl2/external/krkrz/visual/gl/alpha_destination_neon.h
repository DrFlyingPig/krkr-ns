/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstddef>
#include <cstdint>

#ifdef __SWITCH__
#include <arm_neon.h>
#else
#include "simde/arm/neon.h"
#endif
#include "blend_function_neon.h"

static inline tjs_uint32 TVPAlphaBlendRGBPixelNEON(tjs_uint32 d, tjs_uint32 s, tjs_uint32 a)
{
    tjs_uint32 rb = d & 0xff00ff;
    rb = (rb + (((s & 0xff00ff) - rb) * a >> 8)) & 0xff00ff;
    const tjs_uint32 g = d & 0xff00;
    return rb + ((g + (((s & 0xff00) - g) * a >> 8)) & 0xff00);
}

static inline tjs_uint32 TVPAlphaBlendDoPixelNEON(tjs_uint32 d, tjs_uint32 s, tjs_int opa)
{
    const tjs_uint32 index = (((s >> 24) * opa) & 0xff00) + (d >> 24);
    return TVPAlphaBlendRGBPixelNEON(d, s, TVPOpacityOnOpacityTable[index]) +
        (tjs_uint32(TVPNegativeMulTable[index]) << 24);
}

static inline uint32x4_t TVPAlphaBlendPacked4NEON(
    uint32x4_t d, uint32x4_t s, uint32x4_t a)
{
    const uint32x4_t mask = vdupq_n_u32(0x00ff00ff);
    const uint32x4_t rb = vandq_u32(d, mask);
    const uint32x4_t g = vandq_u32(vshrq_n_u32(d, 8), vdupq_n_u32(255));
    const uint32x4_t outRB = vandq_u32(vaddq_u32(rb,
        vshrq_n_u32(vmulq_u32(vsubq_u32(vandq_u32(s, mask), rb), a), 8)), mask);
    const uint32x4_t outG = vandq_u32(vaddq_u32(g,
        vshrq_n_u32(vmulq_u32(vsubq_u32(vandq_u32(vshrq_n_u32(s, 8),
            vdupq_n_u32(255)), g), a), 8)), vdupq_n_u32(255));
    return vorrq_u32(outRB, vshlq_n_u32(outG, 8));
}

template<bool UseOpacity, typename Scalar>
static inline void TVPAlphaBlendDestinationNEON(tjs_uint32* dest, const tjs_uint32* src,
                                               tjs_int len, tjs_int opa, const Scalar& scalar)
{
    if (len <= 0) return;
    // The existing _d dispatcher has scalar alignment edges and four-pixel
    // shortcuts; their alpha rounding must not be widened to eight pixels.
    if (!UseOpacity)
        while (len && (reinterpret_cast<std::size_t>(dest) & 15))
        {
            *dest = scalar(*dest, *src);
            ++dest; ++src; --len;
        }
    const uint32x4_t maxAlpha = vdupq_n_u32(255);
    while (len >= 4)
    {
        const uint32x4_t s = vld1q_u32(src);
        uint32x4_t sa = vshrq_n_u32(s, 24);
        if (!UseOpacity)
        {
            if (vminvq_u32(sa) == 255)
            {
                vst1q_u32(dest, s);
                dest += 4; src += 4; len -= 4;
                continue;
            }
            if (!vmaxvq_u32(sa))
            {
                dest += 4; src += 4; len -= 4;
                continue;
            }
        }
        else
            sa = vshrq_n_u32(vmulq_u32(sa, vdupq_n_u32(tjs_uint32(opa))), 8);
        const uint32x4_t d = vld1q_u32(dest);
        const uint32x4_t da = vshrq_n_u32(d, 24);
        tjs_uint32 index[4], opacity[4], alpha[4];
        vst1q_u32(index, vaddq_u32(vshlq_n_u32(sa, 8), da));
        for (int i = 0; i < 4; ++i)
        {
            opacity[i] = TVPOpacityOnOpacityTable[index[i]];
            if (UseOpacity) alpha[i] = TVPNegativeMulTable[index[i]];
        }
        const uint32x4_t outAlpha = UseOpacity ? vld1q_u32(alpha) :
            vsubq_u32(maxAlpha, vshrq_n_u32(vmulq_u32(vsubq_u32(maxAlpha, sa),
                vsubq_u32(maxAlpha, da)), 8));
        vst1q_u32(dest, vorrq_u32(TVPAlphaBlendPacked4NEON(d, s, vld1q_u32(opacity)),
                                  vshlq_n_u32(outAlpha, 24)));
        dest += 4; src += 4; len -= 4;
    }
    while (len-- > 0)
    {
        *dest = scalar(*dest, *src);
        ++dest; ++src;
    }
}

struct TVPAlphaBlendDoScalarNEON
{
    tjs_int opa;
    tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        return TVPAlphaBlendDoPixelNEON(d, s, opa);
    }
};

static inline bool TVPAlphaBlendSpanOverlapsNEON(
    tjs_uint32* dest, const tjs_uint32* src, tjs_int len)
{
    const auto d = reinterpret_cast<std::uintptr_t>(dest);
    const auto s = reinterpret_cast<std::uintptr_t>(src);
    const std::size_t bytes = std::size_t(len) * sizeof(tjs_uint32);
    return d < s + bytes && s < d + bytes;
}

struct TVPAlphaBlendHDAOpacityScalarNEON
{
    tjs_int opa;
    tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        // Unlike the SSE2 edge functor, the currently dispatched C routine
        // computes opacity from source alpha only, never the packed RGB bits.
        const tjs_uint32 a = ((s >> 24) * tjs_uint32(opa)) >> 8;
        return TVPAlphaBlendRGBPixelNEON(d, s, a) | (d & 0xff000000);
    }
};

static inline void TVPAlphaBlendHDAOpacityNEON(tjs_uint32* dest, const tjs_uint32* src,
                                             tjs_int len, tjs_int opa)
{
    if (len <= 0) return;
    const TVPAlphaBlendHDAOpacityScalarNEON scalar{opa};
    if (len >= 16 && opa >= 0 && opa <= 255 && !TVPAlphaBlendSpanOverlapsNEON(dest, src, len))
        TVPAlphaBlendNEON<true, true, false>(dest, src, len, opa, scalar);
    else
        while (len-- > 0)
        {
            *dest = scalar(*dest, *src);
            ++dest; ++src;
        }
}

static inline void TVPAlphaBlendDoNEON(tjs_uint32* dest, const tjs_uint32* src,
                                      tjs_int len, tjs_int opa)
{
    if (len <= 0) return;
    const TVPAlphaBlendDoScalarNEON scalar{opa};
    if (len >= 16 && opa >= 0 && opa <= 255 && !TVPAlphaBlendSpanOverlapsNEON(dest, src, len))
        TVPAlphaBlendDestinationNEON<true>(dest, src, len, opa, scalar);
    else
        while (len-- > 0)
        {
            *dest = scalar(*dest, *src);
            ++dest; ++src;
        }
}
