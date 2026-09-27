// Compile the production player against a controllable audio stream. A zero
// sample counter must distinguish an unconsumed queue from an exhausted one.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#define tjsCommHeadH
#define MsgIntfH
#define WaveIntfH
#define PortAudioImplH
#define __SOUND_SAMPLES_BUFFER_H__
#define TJS_W(value) value
using tjs_int = int;
using tjs_uint = unsigned;
using tjs_uint8 = uint8_t;
using tjs_uint32 = uint32_t;
using tjs_int16 = int16_t;
using tjs_int64 = int64_t;
using tjs_uint64 = uint64_t;
void TVPThrowExceptionMessage(const char* value) { throw std::runtime_error(value); }
struct tTVPWaveFormat {
    unsigned SamplesPerSec = 48000, Channels = 1, BitsPerSample = 16;
    unsigned BytesPerSample = 2, SpeakerConfig = 0;
    bool IsFloat = false;
};
struct TestCriticalSection {};
struct tTJSCriticalSectionHolder {
    explicit tTJSCriticalSectionHolder(TestCriticalSection&) {}
};
void TVPConvertPCMTo16bits(tjs_int16*, const void*, unsigned, unsigned,
                         unsigned, bool, int, bool) {}
#include "../krkrsdl2/external/krkrz/sound/AudioDevice.h"
class tTVPSoundSamplesBuffer;
struct tTJSNI_QueueSoundBuffer {
    TestCriticalSection CriticalSection;
    TestCriticalSection& GetBufferCS() { return CriticalSection; }
    void ReleasePlayedSamples(tTVPSoundSamplesBuffer*, bool) {}
};
struct TestSegments {
    int FilteredPositionToDecodePosition(int position) const { return position; }
    int GetFilteredLength() const { return 0; }
};
class tTVPSoundSamplesBuffer {
public:
    bool IsEnded() const { return true; }
    int GetDecodePosition() const { return 0; }
    unsigned GetInSamples() const { return 14400; }
    unsigned GetSamplesCount() const { return 24000; }
    void Enqueue(iTVPAudioStream* stream) { stream->Enqueue(nullptr, 48000, true); }
    const TestSegments& GetSegmentQueue() const {
        static TestSegments value;
        return value;
    }
    const uint8_t* GetVisBuffer() const { return nullptr; }
};
#include "../krkrsdl2/external/krkrz/sound/SoundPlayer.h"
#include "../krkrsdl2/external/krkrz/sound/SoundPlayer.cpp"

class TestStream final : public iTVPAudioStream {
public:
    unsigned Queued = 0;
    uint64_t Played = 0;
    int Stops = 0;
    void SetCallback(StreamQueueCallback, void*) override {}
    void Enqueue(void*, size_t, bool) override { ++Queued; }
    void ClearQueue() override { Queued = 0; }
    void StartStream() override {}
    void StopStream() override { ++Stops; }
    void AbortStream() override {}
    unsigned GetQueuedCount() const override { return Queued; }
    uint64_t GetSamplesPlayed() const override { return Played; }
    void SetVolume(int) override {}
    int GetVolume() const override { return 100000; }
    void SetPan(int) override {}
    int GetPan() const override { return 0; }
    void SetFrequency(int) override {}
    int GetFrequency() const override { return 48000; }
};
class TestDevice final : public iTVPAudioDevice {
public:
    TestStream* Stream = nullptr;
    void Initialize(tTVPAudioInitParam&) override {}
    void Uninitialize() override {}
    iTVPAudioStream* CreateAudioStream(tTVPAudioStreamParam&) override {
        Stream = new TestStream();
        return Stream;
    }
    void SetMasterVolume(int) override {}
    int GetMasterVolume() const override { return 100000; }
};

static bool Check(bool value, const char* message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
int main() {
    tTJSNI_QueueSoundBuffer owner;
    tTVPSoundPlayer player(&owner);
    TestDevice device;
    tTVPWaveFormat format;
    tTVPSoundSamplesBuffer samples;
    auto start = [&]() {
        player.Clear();
        player.CreateStream(&device, format, 24000);
        player.Reset();
        player.PushSamplesBuffer(&samples);
        player.Start();
    };
    start();
    if (!Check(player.Update(), "zero counter with queued PCM keeps playing")) return 1;
    if (!Check(device.Stream->Stops == 0, "unconsumed stream is not stopped")) return 1;
    device.Stream->Played = 14399;
    if (!Check(player.Update(), "samples below EOS keep playing")) return 1;
    device.Stream->Played = 14400;
    if (!Check(!player.Update(), "sample position reaching EOS stops")) return 1;
    if (!Check(device.Stream->Stops == 1, "EOS stops stream once")) return 1;
    start();
    if (!Check(player.Update(), "replay zero counter also waits")) return 1;
    device.Stream->Queued = 0;
    if (!Check(!player.Update(), "exhausted queue with EOS counter reset stops")) return 1;
    if (!Check(device.Stream->Stops == 1, "drained EOS stops stream once")) return 1;
    player.Destroy();
    std::cout << "PASS: natural EOS separates startup zero from drained zero\n";
}
