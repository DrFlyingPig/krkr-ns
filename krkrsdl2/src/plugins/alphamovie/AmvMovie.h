// AJPM AlphaMovie container and script-driven playback, independent of TJS/SDL.
// Decoder provenance and license: LICENSE.krkrsdl3 in this directory.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <vector>

namespace krkr { namespace amv {

class Input {
public:
    virtual ~Input() = default;
    virtual uint64_t Size() const = 0;
    virtual void Read(uint64_t offset, void* dest, size_t bytes) = 0;
};

struct Info {
    uint32_t width = 0, height = 0, count = 0, rate = 0, alphaMode = 0;
};

struct Frame {
    uint32_t index = 0, left = 0, top = 0, width = 0, height = 0;
    uint32_t screenWidth = 0, screenHeight = 0;
    std::vector<uint8_t> rgba;
};

class Movie {
public:
    explicit Movie(std::unique_ptr<Input> input);
    const Info& GetInfo() const { return info_; }
    std::shared_ptr<const Frame> Decode(uint32_t index);
    size_t FrameBytes(uint32_t index) const;
    void ClearDecoded();
    size_t CachedBytes() const { return cacheBytes_; }
    static constexpr size_t CacheLimit = 32u * 1024u * 1024u;
private:
    struct Record {
        uint64_t offset = 0;
        uint32_t bytes = 0, alphaBytes = 0;
        uint32_t left = 0, top = 0, width = 0, height = 0;
    };
    std::unique_ptr<Input> input_;
    Info info_;
    std::array<std::array<uint8_t, 64>, 3> quant_{};
    std::vector<Record> records_;
    std::list<std::shared_ptr<const Frame>> cache_;
    size_t cacheBytes_ = 0;
};

// The original frame property reports the decoder cursor (including preload),
// while showNextImage returns the zero-based displayed index. Seeking flushes
// the queue; play restarts at zero, stop retains the current file and cursor.
class Player {
public:
    void Open(std::unique_ptr<Input> input);
    void SetNext(std::unique_ptr<Input> input);
    void Play();
    void Stop();
    void Close();
    void ClearDecoded();
    void Seek(int64_t frame);
    std::shared_ptr<const Frame> NextImage();
    Info GetInfo() const { return movie_ ? movie_->GetInfo() : Info{}; }
    int64_t FrameNumber() const { return decoded_; }
    bool IsPlaying() const { return playing_; }
    void SetLoop(bool value) { loop_ = value; }
    bool Loop() const { return loop_; }
    void SetNextLoop(bool value) { nextLoop_ = value; }
    bool NextLoop() const { return nextLoop_; }
    void SetPreload(int value);
    int Preload() const { return preload_; }
    size_t CachedBytes() const;
private:
    std::unique_ptr<Movie> movie_, next_;
    uint32_t cursor_ = 0;
    int64_t decoded_ = 0;
    bool playing_ = true, loop_ = true, nextLoop_ = true;
    int preload_ = 5;
};

// Convert the visible frame rectangle to engine BGRA. Dest points at logical
// row 0 and pitch may be negative. Premultiplication is selected by Layer.face.
void CopyBGRA(const Frame& frame, uint8_t* dest, ptrdiff_t pitch,
              uint32_t width, uint32_t height, bool premultiply);

}} // namespace krkr::amv
