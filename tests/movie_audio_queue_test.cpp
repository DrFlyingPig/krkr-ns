#include "../krkrsdl2/src/core/visual/sdl2/MovieAudioQueue.h"

#include <array>
#include <deque>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

struct FakeStream
{
    struct Buffer
    {
        uint8_t *memory;
        std::vector<uint8_t> expected;
        bool last;
    };
    std::deque<Buffer> pending;
    std::vector<uint8_t> played;
    std::atomic<int> &freeBlocks;
    int eos = 0;
    explicit FakeStream(std::atomic<int> &free) : freeBlocks(free) {}
    unsigned GetQueuedCount() const { return (unsigned)pending.size(); }
    void Enqueue(void *data, size_t bytes, bool last)
    {
        auto *memory = static_cast<uint8_t *>(data);
        Check(pending.size() < 3, "device queue exceeded three blocks");
        for (const auto &buffer : pending)
            Check(memory != buffer.memory, "reused an in-flight audio block");
        pending.push_back({memory, std::vector<uint8_t>(memory, memory + bytes), last});
    }
    void Complete()
    {
        Check(!pending.empty(), "audio queue underflow");
        const auto &buffer = pending.front();
        Check(std::equal(buffer.expected.begin(), buffer.expected.end(), buffer.memory),
              "queued audio memory was overwritten");
        played.insert(played.end(), buffer.memory, buffer.memory + buffer.expected.size());
        if (buffer.last) ++eos;
        pending.pop_front();
        freeBlocks.fetch_add(1); // Same handoff as the production callback.
    }
};

struct Fixture
{
    static constexpr size_t blockBytes = 16;
    std::vector<uint8_t> ring = std::vector<uint8_t>(128);
    std::array<std::array<uint8_t, blockBytes>, 4> storage{};
    std::vector<uint8_t *> blocks;
    size_t read = 0;
    size_t write = 0;
    int next = 0;
    std::atomic<int> free{4};
    FakeStream stream{free};
    bool quit = false;
    bool paused = false;
    Fixture() { for (auto &block : storage) blocks.push_back(block.data()); }
    void Append(size_t bytes)
    {
        Check(write - read + bytes <= ring.size(), "test PCM ring overflow");
        for (size_t n = 0; n < bytes; ++n)
            ring[(write + n) % ring.size()] = uint8_t(write + n);
        write += bytes;
    }
    MovieAudioQueueResult Pump(bool flush = false)
    {
        return RefillMovieAudioQueue(stream, ring, read, write, blocks, free,
                                    next, blockBytes, flush,
                                    [this] { return quit || paused; });
    }
};

struct TimedStream : FakeStream
{
    size_t frontFrames = 0;
    size_t silentFrames = 0;
    explicit TimedStream(std::atomic<int> &free) : FakeStream(free) {}
    void Advance(size_t frames)
    {
        while (frames)
        {
            if (pending.empty())
            {
                silentFrames += frames;
                return;
            }
            const size_t remaining = pending.front().expected.size() / 4 - frontFrames;
            const size_t played = std::min(frames, remaining);
            frontFrames += played;
            frames -= played;
            if (frontFrames == pending.front().expected.size() / 4)
            {
                Complete();
                frontFrames = 0;
            }
        }
    }
};

void ActualInterleaving()
{
    // Actual movie packet layout: 22 AAC packets, then 15 video packets
    // (~500 ms); following audio runs have 21 AAC packets. Each AAC packet
    // produces 1024 stereo frames at 44.1 kHz. The original three-block device
    // queue held only 279 ms even though the PCM ring held the whole audio run.
    constexpr size_t blockBytes = 16384;
    constexpr size_t sampleFrameBytes = 4;
    std::vector<uint8_t> ring(1u << 20);
    std::array<std::array<uint8_t, blockBytes>, 4> memory{};
    std::vector<uint8_t *> blocks;
    for (auto &block : memory) blocks.push_back(block.data());
    std::atomic<int> free{4};
    TimedStream stream(free);
    size_t read = 0, write = 0;
    int next = 0;
    auto append = [&](size_t aacPackets) {
        const size_t bytes = aacPackets * 1024 * sampleFrameBytes;
        for (size_t n = 0; n < bytes; ++n) ring[(write + n) % ring.size()] = uint8_t(write + n);
        write += bytes;
    };
    auto pump = [&](bool duringVideoWait) {
        return RefillMovieAudioQueue(stream, ring, read, write, blocks, free,
                                    next, blockBytes, false, [] { return false; },
                                    duringVideoWait, sampleFrameBytes);
    };
    auto videoWait = [&](unsigned milliseconds) {
        size_t elapsedFrames = 0;
        for (unsigned time = 0; time < milliseconds; )
        {
            pump(true);
            time = std::min(milliseconds, time + 8); // Production wait period.
            const size_t target = size_t(time) * 44100 / 1000;
            stream.Advance(target - elapsedFrames);
            elapsedFrames = target;
        }
    };
    append(22);
    Check(pump(false).submittedBlocks == 3, "real AAC burst did not fill three blocks");
    videoWait(500);
    Check(stream.silentFrames == 0, "500 ms video run starved despite buffered audio");
    append(21);
    pump(false);
    videoWait(480); // Inside the next ~500 ms run, longer than queue capacity.
    Check(stream.silentFrames == 0, "21-packet AAC burst starved during video wait");
    Check(write - read < blockBytes, "video wait did not service buffered full blocks");
    for (const auto &buffer : stream.pending)
        Check(!buffer.last, "non-EOF partial PCM was marked end-of-stream");

    while (stream.GetQueuedCount()) stream.Complete();
    // This test intentionally stops before the next demux audio run. The
    // separate movie-clock/startup offset is not changed by the refill fix.
    Check(stream.eos == 0 && free.load() == 4, "real interleave ownership or EOS");
}

void DecoderDrain()
{
    constexpr int again = -11, eof = -12;
    std::deque<int> received{0, 0, again, 0, eof};
    unsigned sends = 0, consumed = 0;
    auto result = PumpMovieAudioDecoder(
        [&] { return ++sends == 1 ? again : 0; },
        [&] { Check(!received.empty(), "unexpected decoder receive");
              const int value = received.front(); received.pop_front(); return value; },
        [&] { ++consumed; }, [] { return false; }, again, eof);
    Check(sends == 2 && consumed == 3 && result.frames == 3 && result.eof && !result.error,
          "EOF send did not retry after draining pending decoder frames");

    received = {0, again}; sends = consumed = 0;
    result = PumpMovieAudioDecoder(
        [&] { ++sends; return 0; },
        [&] { const int value = received.front(); received.pop_front(); return value; },
        [&] { ++consumed; }, [] { return false; }, again, eof);
    Check(sends == 1 && consumed == 1 && !result.eof && !result.error,
          "normal decoder EAGAIN was treated as EOF/error");

    sends = 0;
    result = PumpMovieAudioDecoder(
        [&] { ++sends; return again; }, [again] { return again; },
        [] { throw std::runtime_error("nonexistent decoded frame"); },
        [] { return false; }, again, eof);
    Check(sends == 1 && result.error == again, "non-progressing decoder spun forever");

    bool quit = false; sends = consumed = 0;
    result = PumpMovieAudioDecoder(
        [&] { ++sends; return again; }, [] { return 0; },
        [&] { ++consumed; quit = true; }, [&] { return quit; }, again, eof);
    Check(sends == 1 && consumed == 1 && !result.eof,
          "stop retried or consumed more delayed decoder frames");

    sends = 0;
    result = PumpMovieAudioDecoder(
        [&] { ++sends; return -99; }, [] { return 0; }, [] {},
        [] { return false; }, again, eof);
    Check(sends == 1 && result.error == -99, "decoder send error was discarded");

    std::deque<int> converted{3, 2, 0};
    unsigned appended = 0;
    result = DrainMovieAudioResampler(
        [&] { const int value = converted.front(); converted.pop_front(); return value; },
        [&](int frames) { appended += frames; }, [] { return false; });
    Check(appended == 5 && result.frames == 5 && result.eof && !result.error,
          "resampler delayed PCM was dropped or flushed only once");
    result = DrainMovieAudioResampler(
        [] { return -99; }, [](int) {}, [] { return false; });
    Check(result.error == -99 && !result.eof, "resampler error was discarded");
    result = DrainMovieAudioResampler(
        [] { throw std::runtime_error("resampler ran after stop"); return 0; },
        [](int) {}, [] { return true; });
    Check(result.frames == 0 && !result.eof, "stopped resampler falsely reported drained");
}

void OutputLifecycle()
{
    struct Output
    {
        std::atomic<int> &credits;
        bool &destroyed;
        int rate, channels;
        bool stopped = false;
        void StopStream() { stopped = true; }
        ~Output() noexcept(false)
        {
            Check(stopped, "source destroyed without stopping playback");
            // Flush completions may still happen during synchronized voice
            // destruction. They belong to the old source, before reset.
            credits.fetch_add(2);
            destroyed = true;
        }
    };
    std::atomic<int> credits{2};
    bool destroyed = false, reset = false;
    Output *output = new Output{credits, destroyed, 44100, 2};
    ReleaseMovieAudioOutput(output, [&] {
        Check(destroyed && credits.load() == 4,
              "reset happened before old flush callbacks finished");
        credits.store(0);
        reset = true;
    });
    Check(!output && reset && credits.load() == 0, "old source or credits survived reopen");
    destroyed = false;
    output = new Output{credits, destroyed, 48000, 1};
    Check(output->rate == 48000 && output->channels == 1 && credits.load() == 0,
          "reopen retained previous audio format or credits");
    ReleaseMovieAudioOutput(output, [&] { credits.store(0); });
}

void Tests()
{
    Fixture burst;
    burst.Append(112);
    auto result = burst.Pump();
    Check(result.submittedBlocks == 3 && result.submittedBytes == 48,
          "one pump must fill all three queue slots after an audio packet burst");
    Check(burst.free.load() == 1 && burst.read == 48, "initial queue ownership");
    Check(burst.Pump().submittedBlocks == 0, "full queue consumed extra PCM");
    // No further audio packets arrive while video waits. Service the already
    // decoded ring after each callback, just as WaitForPlaybackTime does.
    while (burst.read != burst.write || burst.stream.GetQueuedCount())
    {
        burst.stream.Complete();
        burst.Pump();
    }
    Check(burst.stream.played.size() == 112 && burst.free.load() == 4,
          "video wait lost buffered audio or callback credits");
    for (size_t n = 0; n < burst.stream.played.size(); ++n)
        Check(burst.stream.played[n] == uint8_t(n), "audio FIFO order");

    Fixture wrap;
    wrap.read = wrap.write = 120;
    wrap.Append(37); // A whole block crosses the ring boundary; 5-byte tail.
    result = wrap.Pump();
    Check(result.submittedBlocks == 2 && wrap.write - wrap.read == 5,
          "normal playback submitted a partial block");
    Check(wrap.stream.pending[0].expected.front() == 120 &&
          wrap.stream.pending[0].expected.back() == 135, "PCM ring wrap copy");
    result = wrap.Pump(true);
    Check(result.submittedBlocks == 1 && result.submittedBytes == 5 &&
          wrap.stream.pending.back().last, "EOF partial tail or marker");
    Check(wrap.Pump(true).submittedBlocks == 0, "EOF submitted an empty duplicate");
    while (wrap.stream.GetQueuedCount()) wrap.stream.Complete();
    Check(wrap.stream.played.size() == 37 && wrap.stream.eos == 1,
          "EOF bytes or EOS marker duplicated");

    Fixture end;
    end.Append(64); // EOF exactly on a full-block boundary, larger than queue.
    end.Pump(true);
    Check(!end.stream.pending.back().last, "EOS marked before all buffered PCM");
    end.stream.Complete();
    end.Pump(true);
    Check(end.stream.pending.back().last, "full final block needs EOS marker");
    while (end.stream.GetQueuedCount()) end.stream.Complete();
    Check(end.stream.eos == 1 && end.stream.played.size() == 64,
          "exact-boundary EOF accounting");

    Fixture held;
    held.Append(64);
    held.paused = true;
    Check(held.Pump().submittedBlocks == 0 && held.read == 0,
          "paused video consumed buffered PCM");
    held.paused = false;
    held.quit = true;
    Check(held.Pump(true).submittedBlocks == 0 && held.read == 0,
          "stop consumed buffered PCM");
    held.quit = false;
    held.free.store(0);
    Check(held.Pump().submittedBlocks == 0 && held.read == 0,
          "refill ignored callback ownership");

    Fixture alignment;
    alignment.Append(3);
    result = RefillMovieAudioQueue(alignment.stream, alignment.ring,
                                  alignment.read, alignment.write, alignment.blocks,
                                  alignment.free, alignment.next, Fixture::blockBytes,
                                  false, [] { return false; }, true, 4);
    Check(result.submittedBlocks == 0 && alignment.read == 0,
          "partial tail submitted an incomplete sample frame");
    alignment.Append(1);
    result = RefillMovieAudioQueue(alignment.stream, alignment.ring,
                                  alignment.read, alignment.write, alignment.blocks,
                                  alignment.free, alignment.next, Fixture::blockBytes,
                                  false, [] { return false; }, true, 4);
    Check(result.submittedBytes == 4 && !alignment.stream.pending.back().last,
          "non-EOF partial sample frame or EOS flag");

    ActualInterleaving();
    DecoderDrain();
    OutputLifecycle();
}
} // namespace

int main()
{
    try
    {
        Tests();
        std::cout << "movie_audio_queue_test: PASS\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "movie_audio_queue_test: " << error.what() << '\n';
        return 1;
    }
}
