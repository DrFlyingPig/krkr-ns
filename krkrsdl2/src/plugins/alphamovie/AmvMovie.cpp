#include "AmvMovie.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <zlib.h>

namespace krkr { namespace amv {
void DecodePixels(const uint8_t*, size_t, uint8_t[3][64], uint8_t*,
                  uint32_t, uint32_t, const uint8_t*, bool);
namespace {
uint16_t U16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t U32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
constexpr uint64_t MaxPixels = 16u * 1024u * 1024u;
constexpr uint32_t MaxPayload = 64u * 1024u * 1024u;
}

Movie::Movie(std::unique_ptr<Input> input) : input_(std::move(input)) {
    Require(bool(input_), "no AMV storage stream");
    const uint64_t size = input_->Size();
    Require(size >= 40, "truncated AJPM header");
    uint8_t header[40]; input_->Read(0, header, sizeof(header));
    Require(!std::memcmp(header, "AJPM", 4), "not an AJPM AlphaMovie");
    Require(U32(header + 4) == size, "AJPM file size mismatch");
    Require(U32(header + 8) == 0, "unsupported AJPM revision");
    info_.count = U32(header + 20);
    info_.rate = U32(header + 28);
    info_.width = U16(header + 32); info_.height = U16(header + 34);
    info_.alphaMode = U32(header + 36);
    Require(info_.alphaMode == 1 || info_.alphaMode == 2, "unsupported AMV alpha mode");
    Require(info_.width && info_.height && info_.width <= 8192 && info_.height <= 8192 &&
            uint64_t(info_.width) * info_.height <= MaxPixels, "invalid AMV canvas size");
    Require(info_.rate && info_.rate <= 1000, "invalid AMV frame rate");
    const uint32_t tables = info_.alphaMode == 1 ? 3 : 2;
    uint64_t offset = U32(header + 12);
    Require(offset == 40 + tables * 64 && offset <= size, "invalid AMV quantization tables");
    Require(info_.count && info_.count <= 1000000 && info_.count <= (size - offset) / 20,
            "invalid AMV frame count");
    input_->Read(40, quant_.data(), tables * 64);
    records_.reserve(info_.count);
    for (uint32_t i = 0; i < info_.count; ++i) {
        Require(offset <= size && size - offset >= 20, "truncated FRAM header");
        uint8_t frame[24]{}; input_->Read(offset, frame, 20);
        Require(!std::memcmp(frame, "FRAM", 4), "invalid FRAM signature");
        const uint64_t total = uint64_t(U32(frame + 4)) + 8;
        Require(total >= 20 && total <= size - offset, "invalid FRAM span");
        Record record;
        record.left = U16(frame + 12); record.top = U16(frame + 14);
        record.width = U16(frame + 16); record.height = U16(frame + 18);
        Require((record.width == 0) == (record.height == 0), "invalid empty AMV frame");
        uint32_t headerBytes = 20;
        if (info_.alphaMode == 2 && total >= 24) {
            input_->Read(offset + 20, frame + 20, 4);
            record.alphaBytes = U32(frame + 20); headerBytes = 24;
        }
        Require(record.width <= 8192 && record.height <= 8192 &&
                uint64_t(record.width) * record.height <= MaxPixels, "AMV frame too large");
        Require(!record.width || ((record.width % 16) == 0 && (record.height % 16) == 0),
                "AMV coded frame is not macroblock aligned");
        Require(total - headerBytes <= MaxPayload, "AMV frame payload too large");
        record.offset = offset + headerBytes;
        record.bytes = static_cast<uint32_t>(total - headerBytes);
        Require(record.alphaBytes <= record.bytes, "invalid AMV alpha span");
        if (record.width) {
            Require(record.bytes > record.alphaBytes, "missing AMV color stream");
            Require(info_.alphaMode != 2 || record.alphaBytes, "missing AMV alpha stream");
        } else {
            Require(!record.bytes && !record.alphaBytes, "unexpected empty AMV payload");
        }
        records_.push_back(record);
        offset += total;
    }
    Require(offset == size, "trailing or unindexed AMV data");
}

std::shared_ptr<const Frame> Movie::Decode(uint32_t index) {
    Require(index < records_.size(), "AMV frame out of range");
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
        if ((*it)->index == index) {
            auto result = *it;
            cache_.splice(cache_.begin(), cache_, it);
            return result;
        }
    }
    const auto& r = records_[index];
    const size_t rgbaBytes = size_t(r.width) * r.height * 4;
    // Evict before allocation, so decoding does not momentarily keep the entire
    // old cache plus a new large frame. Empty frames are bounded by count too.
    while (!cache_.empty() && (cacheBytes_ + rgbaBytes > CacheLimit || cache_.size() >= 8)) {
        cacheBytes_ -= cache_.back()->rgba.size(); cache_.pop_back();
    }
    auto frame = std::make_shared<Frame>();
    frame->index = index; frame->left = r.left; frame->top = r.top;
    frame->width = r.width; frame->height = r.height;
    frame->screenWidth = info_.width; frame->screenHeight = info_.height;
    if (r.width) {
        std::vector<uint8_t> packed(r.bytes); input_->Read(r.offset, packed.data(), packed.size());
        std::vector<uint8_t> alpha;
        if (info_.alphaMode == 2) {
            alpha.resize(size_t(r.width) * r.height);
            uLongf count = static_cast<uLongf>(alpha.size());
            const int status = uncompress(alpha.data(), &count, packed.data(), r.alphaBytes);
            Require(status == Z_OK && count == alpha.size(), "invalid compressed AMV alpha plane");
        }
        frame->rgba.resize(rgbaBytes);
        DecodePixels(packed.data() + r.alphaBytes, packed.size() - r.alphaBytes,
                     reinterpret_cast<uint8_t (*)[64]>(quant_.data()), frame->rgba.data(),
                     r.width, r.height, alpha.empty() ? nullptr : alpha.data(), info_.alphaMode == 1);
    }
    if (rgbaBytes <= CacheLimit) {
        cacheBytes_ += rgbaBytes; cache_.push_front(frame);
    }
    return frame;
}

void Movie::ClearDecoded() { cache_.clear(); cacheBytes_ = 0; }
size_t Movie::FrameBytes(uint32_t index) const {
    Require(index < records_.size(), "AMV frame out of range");
    return size_t(records_[index].width) * records_[index].height * 4;
}

void Player::Open(std::unique_ptr<Input> input) {
    auto movie = std::make_unique<Movie>(std::move(input));
    movie_ = std::move(movie); next_.reset(); cursor_ = 0; decoded_ = 0;
}
void Player::SetNext(std::unique_ptr<Input> input) {
    if (!input) { next_.reset(); return; }
    next_ = std::make_unique<Movie>(std::move(input));
}
void Player::Play() {
    Require(bool(movie_), "AlphaMovie.play requires an opened movie");
    cursor_ = 0; decoded_ = 0; ClearDecoded();
    playing_ = true;
}
void Player::Stop() {
    playing_ = false; next_.reset();
    if (movie_) movie_->ClearDecoded();
}
void Player::Close() { Stop(); movie_.reset(); cursor_ = 0; decoded_ = 0; }
void Player::ClearDecoded() {
    if (movie_) movie_->ClearDecoded();
    if (next_) next_->ClearDecoded();
}
void Player::Seek(int64_t frame) {
    Require(bool(movie_), "AlphaMovie.frame requires an opened movie");
    Require(frame >= 0 && uint64_t(frame) < movie_->GetInfo().count, "AMV seek out of range");
    cursor_ = static_cast<uint32_t>(frame); decoded_ = frame;
    ClearDecoded();
}
void Player::SetPreload(int value) {
    Require(value >= 0 && value <= 1024, "invalid AMV preloadSamples");
    preload_ = value;
    ClearDecoded();
}
size_t Player::CachedBytes() const {
    return (movie_ ? movie_->CachedBytes() : 0) + (next_ ? next_->CachedBytes() : 0);
}
std::shared_ptr<const Frame> Player::NextImage() {
    if (!playing_ || !movie_) return {};
    const auto info = movie_->GetInfo();
    auto result = movie_->Decode(cursor_);
    decoded_ = cursor_++;
    if (cursor_ == info.count) {
        if (loop_) {
            cursor_ = 0;
        } else if (next_) {
            // KAG observes the next movie's metadata at the old last-frame
            // result, then emits next_alpha_movie before asking for frame 0.
            movie_ = std::move(next_); loop_ = nextLoop_; cursor_ = 0; decoded_ = 0;
        } else {
            // The original plugin holds and returns the last frame until stop.
            cursor_ = info.count - 1;
        }
    }
    // Honor the request under the byte budget. This deterministic synchronous
    // path shares the script's clock and starts no second advancing timer.
    const int ahead = std::min(preload_, 8) - 1;
    size_t queuedBytes = result->rgba.size();
    if (playing_ && ahead > 0 && (cursor_ != result->index || loop_)) {
        for (int n = 0; n < ahead; ++n) {
            uint32_t target = cursor_ + n;
            if (target >= movie_->GetInfo().count) {
                if (!loop_) break;
                target %= movie_->GetInfo().count;
            }
            const size_t bytes = movie_->FrameBytes(target);
            if (bytes > Movie::CacheLimit || queuedBytes > Movie::CacheLimit - bytes) break;
            movie_->Decode(target); decoded_ = target; queuedBytes += bytes;
        }
    }
    if (!preload_) movie_->ClearDecoded();
    return result;
}

void CopyBGRA(const Frame& frame, uint8_t* dest, ptrdiff_t pitch,
              uint32_t width, uint32_t height, bool premultiply) {
    Require(dest && pitch && width && height, "invalid AMV bitmap destination");
    Require(width <= frame.width && height <= frame.height &&
            frame.rgba.size() == size_t(frame.width) * frame.height * 4, "invalid AMV bitmap source");
    Require(pitch >= ptrdiff_t(width) * 4 || pitch <= -ptrdiff_t(width) * 4, "AMV bitmap pitch too small");
    for (uint32_t y = 0; y < height; ++y) {
        const auto* src = frame.rgba.data() + size_t(y) * frame.width * 4;
        auto* row = dest + ptrdiff_t(y) * pitch;
        for (uint32_t x = 0; x < width; ++x, src += 4, row += 4) {
            const unsigned alpha = src[3];
            row[0] = premultiply ? uint8_t((unsigned(src[2]) * alpha) / 255) : src[2];
            row[1] = premultiply ? uint8_t((unsigned(src[1]) * alpha) / 255) : src[1];
            row[2] = premultiply ? uint8_t((unsigned(src[0]) * alpha) / 255) : src[0];
            row[3] = src[3];
        }
    }
}
}} // namespace krkr::amv
