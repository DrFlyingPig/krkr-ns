// Exercise the production sound timer registry with deterministic timer and
// native-object lifetimes. Audio decoding is irrelevant to the bug: EOS has
// already drained the player, and the timer must publish the stop status.
#include <algorithm>
#include <iostream>
#include <vector>

#define tjsCommHeadH
#define SoundBufferBaseImplH
#define __TVP_TIMER_H__
#undef WIN32
#define TJS_INTF_METHOD
using tjs_error = int;
using tjs_int = int;
struct tTJSVariant {};
struct iTJSDispatch2 {};
constexpr tjs_error TJS_S_OK = 0;
#define TJS_FAILED(value) ((value) < 0)
#define TVP_SB_BEAT_INTERVAL 60
void TVPWaveSoundBufferCommitSettings() {}

class TVPTimer {
public:
    inline static std::vector<TVPTimer*> Registered;
    void* Owner = nullptr;
    void (*Handler)(void*) = nullptr;
    bool Enabled = false;
    TVPTimer() { Registered.push_back(this); }
    ~TVPTimer() {
        Registered.erase(std::remove(Registered.begin(), Registered.end(), this),
                         Registered.end());
    }
    void SetInterval(int) {}
    void SetEnabled(bool enabled) { Enabled = enabled; }
    template<class T>
    void SetOnTimerHandler(T* owner, void (T::*)()) {
        Owner = owner;
        Handler = [](void* value) { static_cast<T*>(value)->Handler(); };
    }
    static void UninitThread() { Registered.clear(); }
    static void Tick() {
        for (auto* timer : Registered)
            if (timer->Enabled && timer->Handler) timer->Handler(timer->Owner);
    }
};

class BaseSoundBuffer {
public:
    tjs_error Construct(tjs_int, tTJSVariant**, iTJSDispatch2*) { return TJS_S_OK; }
    void Invalidate() {}
};
class tTJSNI_SoundBuffer : public BaseSoundBuffer {
    using inherited = BaseSoundBuffer;
public:
    bool Playing = true;
    bool BufferPlaying = true;
    int StopEvents = 0;
    int Beats = 0;
    tTJSNI_SoundBuffer();
    tjs_error Construct(tjs_int, tTJSVariant**, iTJSDispatch2*);
    void Invalidate();
    void TimerBeatHandler() {
        ++Beats;
        if (Playing && !BufferPlaying) {
            Playing = false;
            ++StopEvents;
        }
    }
};

#include "../krkrsdl2/external/krkrz/sound/SoundBufferBaseImpl.cpp"

static bool Check(bool value, const char* message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}

int main() {
    tTJSNI_SoundBuffer oldSound;
    oldSound.Construct(0, nullptr, nullptr);
    oldSound.BufferPlaying = false;
    TVPTimer::Tick();
    if (!Check(oldSound.StopEvents == 1, "first session EOS reports stop")) return 1;

    // A native object retained by an old engine outlives the timer thread.
    TVPResetSoundBufferTimerForEngineRestart();
    TVPTimer::UninitThread();
    tTJSNI_SoundBuffer nextSound;
    nextSound.Construct(0, nullptr, nullptr);
    nextSound.BufferPlaying = false;
    TVPTimer::Tick();
    if (!Check(nextSound.StopEvents == 1,
               "second session EOS reports stop while old object lives")) return 1;
    if (!Check(oldSound.Beats == 1, "old session receives no new beats")) return 1;

    oldSound.Invalidate();
    TVPTimer::Tick();
    if (!Check(nextSound.Beats == 2,
               "late old invalidation preserves the new session timer")) return 1;
    nextSound.Invalidate();
    if (!Check(TVPTimer::Registered.empty(), "final buffer removes timer")) return 1;
    nextSound.Invalidate();
    if (!Check(TVPTimer::Registered.empty(), "repeated invalidation is harmless")) return 1;
    std::cout << "PASS: sound timer restart, EOS, and late invalidation\n";
}
