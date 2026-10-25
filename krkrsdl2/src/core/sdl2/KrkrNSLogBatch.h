/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstddef>
#include <cstring>

template<std::size_t Capacity = 8192>
class KrkrNSLogBatchBuffer
{
    char data_[Capacity];
    std::size_t size_ = 0;
    unsigned depth_ = 0;
public:
    void Begin() { ++depth_; }
    bool Active() const { return depth_ != 0; }
    std::size_t Pending() const { return size_; }

    template<class Writer> bool Flush(Writer write)
    {
        std::size_t sent = 0;
        while (sent < size_)
        {
            const auto count = write(data_ + sent, size_ - sent);
            if (count <= 0 || std::size_t(count) > size_ - sent) break;
            sent += std::size_t(count);
        }
        size_ -= sent;
        if (sent && size_) std::memmove(data_, data_ + sent, size_);
        return size_ == 0;
    }

    template<class Writer> bool Append(const char* data, std::size_t size, Writer write)
    {
        if (size > Capacity) return false;
        if (size > Capacity - size_ && !Flush(write)) return false;
        std::memcpy(data_ + size_, data, size);
        size_ += size;
        return true;
    }

    template<class Writer> bool End(Writer write)
    {
        if (depth_) --depth_;
        return depth_ || Flush(write);
    }

    template<class Writer> bool Immediate(const char* data, std::size_t size, Writer write)
    {
        if (!Append(data, size, write)) return false;
        return Flush(write);
    }
};
