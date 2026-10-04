// CPU implementations of the layerExImage.dll pixel operations.
//
// Adapted from krkrsdl3/plugins/LayerExImage.cpp (see LICENSE.krkrsdl3 in this
// directory for the upstream license).  The arithmetic, clamping, rounding and
// channel order follow that source; nothing here is approximated.
//
// One upstream statement is not reproducible as written: its LUT pass uses
// `*p++ = pLut[*p]`, whose read of `*p` is unsequenced against the `p++` side
// effect.  MSVC happens to evaluate the read first, so the intended operation is
// "replace this channel byte through the table, then move to the next channel".
// This port spells that out explicitly, which also keeps clang (the Switch
// toolchain) from choosing the other order.
#ifndef KRKRNS_LAYER_EX_IMAGE_ALGO_H
#define KRKRNS_LAYER_EX_IMAGE_ALGO_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace krkrns {

using Channel = std::uint8_t;

// Byte offsets of the four channels of one pixel.  Layers are BGRA on both the
// reference platform and this port.
enum : int { kBlue = 0, kGreen = 1, kRed = 2, kAlpha = 3, kPixelStride = 4 };

// Returns a value in [0, 255].
inline Channel ClampChannel(int value)
{
    return static_cast<Channel>(std::max(0, std::min(255, value)));
}

// Random source used by the two noise operations.  The reference calls the C
// library's rand(); the parameter exists so the host test can drive the two
// generators deterministically.
using RandFn = int (*)();

// Applies a 256 entry table to the B, G and R channels.  Alpha is untouched.
inline void ApplyLut(Channel* buffer, int width, int height, std::ptrdiff_t pitch,
                     const Channel* table)
{
    for (int y = 0; y < height; ++y)
    {
        Channel* p = buffer + static_cast<std::ptrdiff_t>(y) * pitch;
        for (int x = 0; x < width; ++x, p += kPixelStride)
        {
            p[kBlue] = table[p[kBlue]];
            p[kGreen] = table[p[kGreen]];
            p[kRed] = table[p[kRed]];
        }
    }
}

// Brightness/contrast.  brightness is -255..255, contrast -100..100 where 0 means
// unchanged.  The table keeps the reference's float expression, so its truncation
// matches.
inline void Light(Channel* buffer, int width, int height, std::ptrdiff_t pitch,
                  int brightness, int contrast)
{
    const float c = (100 + contrast) / 100.0f;
    brightness += 128;
    Channel table[256];
    for (int i = 0; i < 256; ++i)
        table[i] = ClampChannel(static_cast<int>((i - 128) * c + brightness));
    ApplyLut(buffer, width, height, pitch, table);
}

// --- CxImage style integer HSL, used by Colorize -----------------------------
constexpr int kHslMax = 255;
constexpr int kRgbMax = 255;
constexpr int kHslUndefined = kHslMax * 2 / 3;

struct Rgb8
{
    Channel b, g, r;
};

struct Hsl8
{
    Channel l, s, h;
};

inline Hsl8 RgbToHsl(Rgb8 color)
{
    const Channel r = color.r, g = color.g, b = color.b;
    const Channel max = std::max(std::max(r, g), b);
    const Channel min = std::min(std::min(r, g), b);

    Hsl8 out;
    out.l = static_cast<Channel>((((max + min) * kHslMax) + kRgbMax) / (2 * kRgbMax));

    if (max == min)
    {
        out.s = 0;
        out.h = static_cast<Channel>(kHslUndefined);
        return out;
    }

    if (out.l <= (kHslMax / 2))
        out.s = static_cast<Channel>((((max - min) * kHslMax) + ((max + min) / 2)) / (max + min));
    else
        out.s = static_cast<Channel>(
            (((max - min) * kHslMax) + ((2 * kRgbMax - max - min) / 2)) / (2 * kRgbMax - max - min));

    // Hue deltas are computed in 16 bit, as in the reference.
    const unsigned short rDelta = static_cast<unsigned short>(
        (((max - r) * (kHslMax / 6)) + ((max - min) / 2)) / (max - min));
    const unsigned short gDelta = static_cast<unsigned short>(
        (((max - g) * (kHslMax / 6)) + ((max - min) / 2)) / (max - min));
    const unsigned short bDelta = static_cast<unsigned short>(
        (((max - b) * (kHslMax / 6)) + ((max - min) / 2)) / (max - min));

    if (r == max)
        out.h = static_cast<Channel>(bDelta - gDelta);
    else if (g == max)
        out.h = static_cast<Channel>((kHslMax / 3) + rDelta - bDelta);
    else
        out.h = static_cast<Channel>(((2 * kHslMax) / 3) + gDelta - rDelta);
    if (out.h > kHslMax) out.h -= kHslMax;
    return out;
}

inline float HueToRgbFloat(float n1, float n2, float hue)
{
    if (hue > 360)
        hue -= 360;
    else if (hue < 0)
        hue += 360;

    if (hue < 60)
        return n1 + (n2 - n1) * hue / 60.0f;
    if (hue < 180)
        return n2;
    if (hue < 240)
        return n1 + (n2 - n1) * (240 - hue) / 60;
    return n1;
}

inline Rgb8 HslToRgb(Hsl8 color)
{
    const float h = static_cast<float>(color.h) * 360.0f / 255.0f;
    const float s = static_cast<float>(color.s) / 255.0f;
    const float l = static_cast<float>(color.l) / 255.0f;

    const float m2 = (l <= 0.5f) ? l * (1 + s) : l + s - l * s;
    const float m1 = 2 * l - m2;

    Rgb8 out;
    if (s == 0)
    {
        out.r = out.g = out.b = static_cast<Channel>(l * 255.0f);
    }
    else
    {
        out.r = static_cast<Channel>(HueToRgbFloat(m1, m2, h + 120) * 255.0f);
        out.g = static_cast<Channel>(HueToRgbFloat(m1, m2, h) * 255.0f);
        out.b = static_cast<Channel>(HueToRgbFloat(m1, m2, h - 120) * 255.0f);
    }
    return out;
}

// Replaces hue and saturation, blending the converted colour back onto the
// original in 8.8 fixed point.  hue and sat use the same 0..255 units as the HSL
// helpers; blend is clamped to 0..1.  Alpha is untouched.
inline void Colorize(Channel* buffer, int width, int height, std::ptrdiff_t pitch,
                     int hue, int sat, double blend)
{
    if (blend < 0.0f) blend = 0.0f;
    if (blend > 1.0f) blend = 1.0f;
    const int a0 = static_cast<int>(256 * blend);
    const int a1 = 256 - a0;
    const bool fullBlend = blend > 0.999f;

    for (int y = 0; y < height; ++y)
    {
        Channel* p = buffer + static_cast<std::ptrdiff_t>(y) * pitch;
        for (int x = 0; x < width; ++x, p += kPixelStride)
        {
            Rgb8 color{p[kBlue], p[kGreen], p[kRed]};
            Hsl8 hsl = RgbToHsl(color);
            hsl.h = static_cast<Channel>(hue);
            hsl.s = static_cast<Channel>(sat);
            const Rgb8 converted = HslToRgb(hsl);
            if (fullBlend)
            {
                color = converted;
            }
            else
            {
                color.r = static_cast<Channel>((converted.r * a0 + color.r * a1) >> 8);
                color.b = static_cast<Channel>((converted.b * a0 + color.b * a1) >> 8);
                color.g = static_cast<Channel>((converted.g * a0 + color.g * a1) >> 8);
            }
            p[kBlue] = color.b;
            p[kGreen] = color.g;
            p[kRed] = color.r;
        }
    }
}

// --- Double precision HSL, used by Modulate ----------------------------------
inline int HueToRgb8(double n1, double n2, double hue)
{
    if (hue < 0)
        hue += 1.0;
    else if (hue > 1.0)
        hue -= 1.0;

    double color;
    if (hue < 1.0 / 6.0)
        color = n1 + (n2 - n1) * hue * 6.0;
    else if (hue < 1.0 / 2.0)
        color = n2;
    else if (hue < 2.0 / 3.0)
        color = n1 + (n2 - n1) * (2.0 / 3.0 - hue) * 6.0;
    else
        color = n1;
    return static_cast<int>(color * 255.0);
}

// hue, saturation and luminance shifts in place.  h is a fraction of the colour
// circle (-0.5..0.5), s and l are fractions (-1..1).
inline void ModulatePixel(int& b, int& g, int& r, double h, double s, double l)
{
    const double red = r / 255.0;
    const double green = g / 255.0;
    const double blue = b / 255.0;

    const double max = std::max(std::max(red, green), blue);
    const double min = std::min(std::min(red, green), blue);
    const double delta = max - min;
    const double sum = max + min;
    double luminance = sum / 2.0;
    double hue;
    double saturation;
    if (delta == 0)
    {
        saturation = 0;
        hue = 0;
    }
    else
    {
        saturation = (luminance < 0.5) ? delta / sum : delta / (2.0 - sum);
        if (red == max)
            hue = (green - blue) / delta;
        else if (green == max)
            hue = 2.0 + (blue - red) / delta;
        else
            hue = 4.0 + (red - green) / delta;
        hue /= 6.0;
    }

    hue += h;
    while (hue < 0) hue += 1.0;
    while (hue > 1.0) hue -= 1.0;
    if (s > 0)
        saturation += (1.0 - saturation) * s;
    else
        saturation += saturation * s;
    if (l > 0)
        luminance += (1.0 - luminance) * l;
    else
        luminance += luminance * l;

    if (saturation == 0.0)
    {
        r = g = b = static_cast<int>(luminance * 255.0);
        return;
    }

    const double m2 = (luminance <= 0.5) ? luminance * (1 + saturation)
                                         : luminance + saturation - luminance * saturation;
    const double m1 = 2.0 * luminance - m2;
    r = HueToRgb8(m1, m2, hue + 1.0 / 3.0);
    g = HueToRgb8(m1, m2, hue);
    b = HueToRgb8(m1, m2, hue - 1.0 / 3.0);
}

// hue -180..180 (degrees), saturation and luminance -100..100 (percent).
inline void Modulate(Channel* buffer, int width, int height, std::ptrdiff_t pitch,
                     int hue, int saturation, int luminance)
{
    const double h = hue / 360.0f;
    const double s = saturation / 100.0f;
    const double l = luminance / 100.0f;

    for (int y = 0; y < height; ++y)
    {
        Channel* p = buffer + static_cast<std::ptrdiff_t>(y) * pitch;
        for (int x = 0; x < width; ++x, p += kPixelStride)
        {
            int b = p[kBlue];
            int g = p[kGreen];
            int r = p[kRed];
            ModulatePixel(b, g, r, h, s, l);
            p[kBlue] = static_cast<Channel>(b);
            p[kGreen] = static_cast<Channel>(g);
            p[kRed] = static_cast<Channel>(r);
        }
    }
}

// Adds uniform noise of +/- level/2 to B, G and R; alpha is untouched.  level is
// 0 (no noise) .. 255 (a lot).
inline void Noise(Channel* buffer, int width, int height, std::ptrdiff_t pitch, int level,
                  RandFn rnd = &std::rand)
{
    for (int y = 0; y < height; ++y)
    {
        Channel* p = buffer + static_cast<std::ptrdiff_t>(y) * pitch;
        for (int x = 0; x < width; ++x, p += kPixelStride)
        {
            for (int channel = kBlue; channel <= kRed; ++channel)
            {
                const int n = static_cast<int>((rnd() / static_cast<float>(RAND_MAX) - 0.5f) * level);
                p[channel] = ClampChannel(static_cast<int>(p[channel]) + n);
            }
        }
    }
}

// Replaces B, G and R with the same random grey value; alpha is untouched.
inline void GenerateWhiteNoise(Channel* buffer, int width, int height, std::ptrdiff_t pitch,
                               RandFn rnd = &std::rand)
{
    for (int y = 0; y < height; ++y)
    {
        Channel* p = buffer + static_cast<std::ptrdiff_t>(y) * pitch;
        for (int x = 0; x < width; ++x, p += kPixelStride)
        {
            const Channel n = static_cast<Channel>(rnd() / (RAND_MAX / 255));
            p[kBlue] = p[kGreen] = p[kRed] = n;
        }
    }
}

}  // namespace krkrns

#endif  // KRKRNS_LAYER_EX_IMAGE_ALGO_H
