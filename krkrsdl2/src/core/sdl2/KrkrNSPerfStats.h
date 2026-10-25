/* SPDX-License-Identifier: MIT */
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

template<unsigned Capacity = 512>
class KrkrNSPerfSamples
{
    static_assert(Capacity > 0, "Samples need a nonzero capacity");
    std::array<double, Capacity> values_{};
    unsigned count_ = 0, cursor_ = 0;
    std::uint64_t total_ = 0, long33_ = 0, long100_ = 0;
    double maximum_ = 0;
public:
    void Add(double ms) noexcept
    {
        if (!std::isfinite(ms) || ms < 0) return;
        values_[cursor_] = ms;
        cursor_ = (cursor_ + 1) % Capacity;
        count_ = std::min(count_ + 1, Capacity);
        ++total_;
        maximum_ = std::max(maximum_, ms);
        long33_ += ms > 33.333;
        long100_ += ms > 100.0;
    }
    unsigned Count() const noexcept { return count_; }
    std::uint64_t Total() const noexcept { return total_; }
    std::uint64_t Long33() const noexcept { return long33_; }
    std::uint64_t Long100() const noexcept { return long100_; }
    double Maximum() const noexcept { return maximum_; }
    double Percentile(unsigned percent) const noexcept
    {
        if (!count_) return 0;
        auto sorted = values_;
        std::sort(sorted.begin(), sorted.begin() + count_);
        const unsigned rank = std::max(1u, (std::min(percent, 100u) * count_ + 99) / 100);
        return sorted[rank - 1];
    }
    void Reset() noexcept { count_ = cursor_ = 0; total_ = long33_ = long100_ = 0; maximum_ = 0; }
};
