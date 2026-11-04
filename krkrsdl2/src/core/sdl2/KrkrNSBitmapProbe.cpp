/* SPDX-License-Identifier: MIT */
#include "KrkrNSBitmapProbe.h"

#ifdef __SWITCH__
#include <SDL.h>

namespace
{
thread_local KrkrNSBitmapProbeTicks tlsTicks;

std::uint64_t Now() noexcept { return SDL_GetPerformanceCounter(); }
}

namespace krkrns
{
KrkrNSBitmapProbeTicks TakeBitmapProbe() noexcept
{
    const KrkrNSBitmapProbeTicks taken = tlsTicks;
    tlsTicks = KrkrNSBitmapProbeTicks{};
    return taken;
}

void AddBitmapProbeDetach(std::uint64_t ticks, bool /*deepCopy*/) noexcept
{
    tlsTicks.detach += ticks;
}

void AddBitmapProbeCPUWrite(std::uint64_t ticks) noexcept
{
    tlsTicks.cpuWrite += ticks;
}

void AddBitmapProbeAlloc(std::uint64_t ticks) noexcept
{
    tlsTicks.alloc += ticks;
}

void AddBitmapProbeScanline() noexcept
{
    ++tlsTicks.scanlines;
}

void AddBitmapProbeStreamRead(std::uint64_t ticks, std::uint64_t bytes) noexcept
{
    tlsTicks.streamRead += ticks;
    tlsTicks.streamBytes += bytes;
}

void AddBitmapProbeGolomb(std::uint64_t ticks) noexcept
{
    tlsTicks.golomb += ticks;
}

void AddBitmapProbeTlgLine(std::uint64_t ticks) noexcept
{
    tlsTicks.tlgLine += ticks;
}

void AddBitmapProbeLzss(std::uint64_t ticks) noexcept
{
    tlsTicks.tlgLzss += ticks;
}

void AddBitmapProbeCompose(std::uint64_t ticks) noexcept
{
    tlsTicks.tlgCompose += ticks;
}

// Called by the probe site that cannot take a counter itself.
std::uint64_t BitmapProbeCounter() noexcept
{
    return Now();
}

double BitmapProbeMilliseconds(std::uint64_t ticks) noexcept
{
    const double frequency = static_cast<double>(SDL_GetPerformanceFrequency());
    return frequency > 0.0 ? static_cast<double>(ticks) * 1000.0 / frequency : 0.0;
}
}
#endif
