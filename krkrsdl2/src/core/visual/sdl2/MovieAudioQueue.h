/* SPDX-License-Identifier: MIT */
#ifndef KRKRNS_MOVIE_AUDIO_QUEUE_H
#define KRKRNS_MOVIE_AUDIO_QUEUE_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

struct MovieAudioQueueResult
{
    unsigned queuedBefore = 0;
    size_t submittedBlocks = 0;
    size_t submittedBytes = 0;
};

struct MovieAudioDecodeResult
{
    size_t frames = 0;
    bool eof = false;
    int error = 0;
};

// FFmpeg send/receive backpressure also applies when sending the null EOF
// packet. Receive all pending frames before retrying an unaccepted packet.
template<class Send, class Receive, class Consume, class ShouldQuit>
MovieAudioDecodeResult PumpMovieAudioDecoder(
    Send send, Receive receive, Consume consume, ShouldQuit shouldQuit,
    int again, int eof)
{
    MovieAudioDecodeResult result;
    auto receiveAll = [&] {
        while (!shouldQuit())
        {
            const int status = receive();
            if (status < 0) return status;
            consume();
            ++result.frames;
        }
        return again;
    };
    if (shouldQuit()) return result;
    int sent = send();
    while (sent == again && !shouldQuit())
    {
        const size_t before = result.frames;
        const int status = receiveAll();
        if (status == eof) { result.eof = true; return result; }
        if (status != again || result.frames == before)
        {
            result.error = status; // A codec that cannot progress must not spin.
            return result;
        }
        if (shouldQuit()) return result;
        sent = send();
    }
    if (shouldQuit()) return result;
    if (sent < 0 && sent != eof) { result.error = sent; return result; }
    const int status = receiveAll();
    result.eof = sent == eof || status == eof;
    if (status < 0 && status != again && status != eof) result.error = status;
    return result;
}

template<class Convert, class Append, class ShouldQuit>
MovieAudioDecodeResult DrainMovieAudioResampler(
    Convert convert, Append append, ShouldQuit shouldQuit)
{
    MovieAudioDecodeResult result;
    while (!shouldQuit())
    {
        const int frames = convert(); // Production uses swr_convert(NULL, 0).
        if (frames < 0) { result.error = frames; return result; }
        if (frames == 0) { result.eof = true; return result; }
        append(frames);
        result.frames += (size_t)frames;
    }
    return result;
}

// DestroyVoice synchronizes any in-flight completion callbacks. Resetting
// callback credits or freeing their PCM buffers must happen after deletion.
template<class Stream, class Reset>
void ReleaseMovieAudioOutput(Stream *&stream, Reset reset)
{
    if (stream)
    {
        stream->StopStream();
        delete stream;
        stream = nullptr;
    }
    reset();
}

// The movie decoder owns the PCM ring and rotated block index. The audio
// callback only returns blocks through freeBlocks. Keep one rotated block out
// of the device queue so an in-flight block is never overwritten.
template<class Stream, class ShouldQuit>
MovieAudioQueueResult RefillMovieAudioQueue(
    Stream &stream, const std::vector<uint8_t> &ring, size_t &read,
    size_t write, const std::vector<uint8_t *> &blocks,
    std::atomic<int> &freeBlocks, int &nextBlock, size_t blockBytes,
    bool flush, ShouldQuit shouldQuit, bool allowPartialTail = false,
    size_t sampleFrameBytes = 1)
{
    MovieAudioQueueResult result;
    result.queuedBefore = stream.GetQueuedCount();
    if (ring.empty() || blocks.size() < 2 || sampleFrameBytes == 0 ||
        blockBytes == 0 || blockBytes % sampleFrameBytes != 0) return result;
    unsigned queued = result.queuedBefore;
    while (!shouldQuit())
    {
        const size_t available = write - read;
        const size_t bytes = std::min(available, blockBytes) /
                             sampleFrameBytes * sampleFrameBytes;
        // A video packet run may delay the next decoded audio frame. Submit
        // the partial PCM already available before the last queued block ends;
        // this does not imply EOF and must not set an end-of-stream marker.
        if (bytes == 0 || (!flush && bytes < blockBytes &&
                          (!allowPartialTail || queued > 1)) ||
            freeBlocks.load() <= 0 || queued >= blocks.size() - 1)
            break;

        uint8_t *block = blocks[nextBlock];
        const size_t position = read % ring.size();
        const size_t tail = std::min(bytes, ring.size() - position);
        std::memcpy(block, ring.data() + position, tail);
        if (bytes > tail)
            std::memcpy(block + tail, ring.data(), bytes - tail);
        read += bytes;
        freeBlocks.fetch_sub(1);
        stream.Enqueue(block, bytes, flush && bytes == available);
        nextBlock = (nextBlock + 1) % blocks.size();
        ++result.submittedBlocks;
        result.submittedBytes += bytes;
        queued = stream.GetQueuedCount();
    }
    return result;
}

#endif
