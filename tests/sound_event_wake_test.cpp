// Run the production sound worker with an auto-reset event whose wake timing
// is controlled explicitly. No OS scheduling or wall-clock sleep is involved.
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

#define tjsCommHeadH
#define ThreadIntfH
#define __NATIVE_EVENT_QUEUE_H__
#define PortAudioImplH
#define TickCountH
#define RandomH
#define __USER_EVENT_H__
using tjs_int = int;
using tjs_int32 = int32_t;
using tjs_int64 = int64_t;
using tjs_uint = unsigned;
using tjs_uint32 = uint32_t;
constexpr int TVP_TIMEOFS_INVALID_VALUE = INT32_MIN;
constexpr int TVP_EV_WAVE_SND_BUF_THREAD = 1026;
constexpr int ttpHighest = 1, ttpNormal = 0;

static uint32_t Tick = 0;
uint32_t TVPGetRoughTickCount32() { return Tick; }
int64_t TVPGetTickCount() { return Tick; }
void TVPPushEnvironNoise(const void*, int) {}
struct tTJSCriticalSection {};
struct tTJSCriticalSectionHolder {
    explicit tTJSCriticalSectionHolder(tTJSCriticalSection&) {}
};
struct NativeEvent {
    int Message;
    explicit NativeEvent(int message) : Message(message) {}
};
template<class T> class NativeEventQueue {
public:
    NativeEventQueue(T*, void (T::*)(NativeEvent&)) {}
    void Allocate() {}
    void Deallocate() {}
    void HandlerDefault(NativeEvent&) {}
    void PostEvent(NativeEvent) {}
};
class tTVPThread {
    bool Terminated = false;
protected:
    virtual void Execute() = 0;
public:
    void SetPriority(int) {}
    void StartTread() {}
    void Terminate() { Terminated = true; }
    bool GetTerminated() const { return Terminated; }
    void WaitFor() {}
};
class tTVPThreadEvent {
    bool Ready = false;
public:
    inline static std::function<void()> TimedWait;
    inline static std::function<void()> InfiniteWait;
    inline static int InfiniteWaits = 0;
    void Set() { Ready = true; }
    void WaitFor(int timeout) {
        if (timeout) {
            if (TimedWait) TimedWait();
            Tick += static_cast<unsigned>(timeout);
        } else {
            ++InfiniteWaits;
            if (!Ready && InfiniteWait) InfiniteWait();
            if (!Ready)
                throw std::runtime_error("worker entered an infinite wait after its timed wait consumed Start");
        }
        // The production SDL and std::condition_variable backends both consume
        // a Set here, including when a finite wait receives the signal.
        Ready = false;
    }
};
class tTJSNI_QueueSoundBuffer {
public:
    bool ThreadCallbackEnabled = false;
    int Updates = 0;
    std::function<void()> OnUpdate;
    void Update() { ++Updates; if (OnUpdate) OnUpdate(); }
    int FireLabelEventsAndGetNearestLabelEventStep(int64_t) {
        return TVP_TIMEOFS_INVALID_VALUE;
    }
    void ReleaseSoundBuffer(bool) {}
    void SetVolumeToStream() {}
};
#include "../krkrsdl2/external/krkrz/sound/SoundEventThread.h"
#ifndef SOUND_EVENT_THREAD_SOURCE
#define SOUND_EVENT_THREAD_SOURCE "../krkrsdl2/external/krkrz/sound/SoundEventThread.cpp"
#endif
#include SOUND_EVENT_THREAD_SOURCE

class TestWorker : public tTVPSoundEventThread {
public:
    explicit TestWorker(tTVPSoundBuffers* buffers) : tTVPSoundEventThread(buffers) {}
    void Run() { Execute(); }
};
static void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void ResetEvent() {
    Tick = 0;
    tTVPThreadEvent::TimedWait = {};
    tTVPThreadEvent::InfiniteWait = {};
    tTVPThreadEvent::InfiniteWaits = 0;
}
static void WakeDuringTimedWait() {
    ResetEvent();
    tTVPSoundBuffers buffers;
    tTJSNI_QueueSoundBuffer sound;
    buffers.AddBuffer(&sound);
    TestWorker worker(&buffers);
    worker.CheckBufferSleep();
    int wakes = 0;
    tTVPThreadEvent::TimedWait = [&]() {
        if (wakes++ == 0) {
            sound.ThreadCallbackEnabled = true;
            worker.Start();
            worker.Start(); // auto-reset events may coalesce multiple wakes
        }
    };
    sound.OnUpdate = [&]() { worker.Terminate(); };
    worker.Run();
    Require(sound.Updates == 1, "new work must receive Update after a timed wake");
    Require(tTVPThreadEvent::InfiniteWaits == 0, "Start must clear the old suspension request");
}
static void WakeFromInfiniteWait() {
    ResetEvent();
    tTVPSoundBuffers buffers;
    tTJSNI_QueueSoundBuffer sound;
    buffers.AddBuffer(&sound);
    TestWorker worker(&buffers);
    worker.CheckBufferSleep();
    tTVPThreadEvent::InfiniteWait = [&]() {
        sound.ThreadCallbackEnabled = true;
        worker.Start();
    };
    sound.OnUpdate = [&]() { worker.Terminate(); };
    worker.Run();
    Require(sound.Updates == 1, "a fully sleeping worker must wake for new work");
    Require(tTVPThreadEvent::InfiniteWaits == 1, "case must exercise the indefinite wait");
}
static void RepeatedIdleAndPlay() {
    ResetEvent();
    tTVPSoundBuffers buffers;
    tTJSNI_QueueSoundBuffer sound;
    buffers.AddBuffer(&sound);
    TestWorker worker(&buffers);
    worker.CheckBufferSleep();
    tTVPThreadEvent::TimedWait = [&]() {
        sound.ThreadCallbackEnabled = true;
        worker.Start();
    };
    sound.OnUpdate = [&]() {
        if (sound.Updates == 3) worker.Terminate();
        else {
            sound.ThreadCallbackEnabled = false;
            worker.CheckBufferSleep();
        }
    };
    worker.Run();
    Require(sound.Updates == 3, "each idle/play cycle must resume the worker");
    Require(tTVPThreadEvent::InfiniteWaits == 0, "repeated timed wakes must not lose work");
}
int main() {
    try {
        WakeDuringTimedWait();
        WakeFromInfiniteWait();
        RepeatedIdleAndPlay();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS: timed wake, indefinite wake, and repeated idle/play cycles\n";
}
