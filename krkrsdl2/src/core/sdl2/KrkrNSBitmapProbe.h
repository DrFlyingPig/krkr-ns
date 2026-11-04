/* SPDX-License-Identifier: MIT */
/* KRKR-ns: per-load bitmap write-path probe (Switch only)
 *
 * Decoding writes one scanline at a time, and every write passes through
 * tTVPNativeBaseBitmap::Independ, tTVPBitmap::BeforeCPUWrite and possibly a
 * fresh allocation. When a decode is slow, these counters say whether that
 * path or the codec itself spent the time. Accumulate on the thread that
 * decodes, then read and reset once per load; nothing here is shared across
 * threads.
 */
#ifndef KRKRNS_BITMAP_PROBE_H
#define KRKRNS_BITMAP_PROBE_H

#include <cstdint>

struct KrkrNSBitmapProbeTicks
{
    std::uint64_t detach = 0;    // tTVPNativeBaseBitmap::Independ
    std::uint64_t cpuWrite = 0;  // tTVPBitmap::BeforeCPUWrite
    std::uint64_t alloc = 0;     // tTVPBitmap allocation inside detach
    std::uint64_t scanlines = 0; // scanline writes observed
    std::uint64_t streamRead = 0; // compressed bytes pulled through the stream
    std::uint64_t streamBytes = 0;
    std::uint64_t golomb = 0;     // TLG6 entropy (bit unpacking)
    std::uint64_t tlgLine = 0;    // TLG6 chroma/luminosity prediction
    std::uint64_t tlgLzss = 0;    // TLG5 LZSS decompression
    std::uint64_t tlgCompose = 0; // TLG5 per-channel colour composition
};

namespace krkrns
{
// Returns the calling thread's counters, which the caller then resets.
KrkrNSBitmapProbeTicks TakeBitmapProbe() noexcept;
// Monotonic tick source, so probe sites can bracket their own work.
std::uint64_t BitmapProbeCounter() noexcept;
// Converts a tick delta from BitmapProbeCounter() into milliseconds.
double BitmapProbeMilliseconds(std::uint64_t ticks) noexcept;
void AddBitmapProbeDetach(std::uint64_t ticks, bool deepCopy) noexcept;
void AddBitmapProbeCPUWrite(std::uint64_t ticks) noexcept;
void AddBitmapProbeAlloc(std::uint64_t ticks) noexcept;
void AddBitmapProbeScanline() noexcept;
void AddBitmapProbeStreamRead(std::uint64_t ticks, std::uint64_t bytes) noexcept;
void AddBitmapProbeGolomb(std::uint64_t ticks) noexcept;
void AddBitmapProbeTlgLine(std::uint64_t ticks) noexcept;
void AddBitmapProbeLzss(std::uint64_t ticks) noexcept;
void AddBitmapProbeCompose(std::uint64_t ticks) noexcept;
}

#endif // KRKRNS_BITMAP_PROBE_H
