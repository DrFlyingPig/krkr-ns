/* SPDX-License-Identifier: MIT */
/* KRKR-ns Phase 4: FFmpeg-backed movie overlay player for Switch.
 *
 * Implements the iTVPVideoOverlay contract (krmovie.h) on top of the
 * minimal static FFmpeg built for WA2-ns (out/ffmpeg_switch). The decode
 * pipeline follows the WA2-ns device-proven design:
 *   - one SDL decode thread with a 4 MiB stack (newlib pthread default
 *     128 KB overflows inside FFmpeg),
 *   - custom AVIO over Kirikiri's tTJSBinaryStream, so loose files and
 *     archive/autopath storage names have exactly the same semantics,
 *   - signature-selected ASF or MOV/MP4 demuxer (avoids both fragile generic
 *     probing and games whose movie extension does not match its container),
 *   - frame_index/fps pacing (stream pts time_base differs across
 *     platforms and ran 10x off),
 *   - sws_scale on the decode thread, SWS_POINT, output BGRA with a
 *     negative stride so rows land bottom-up exactly like the 32bpp
 *     engine bitmaps (tTVPBaseBitmap keeps the logical top at the END of
 *     the buffer).
 *
 * The engine-side pump (VideoOvlImpl) polls GetEvent(); we queue
 * EC_UPDATE(curFrame) per frame and EC_COMPLETE at EOF, and
 * GetFrontBuffer() returns the latest finished of the two buffers that
 * SetVideoBuffer provided — the pump hands it to the layer system via
 * AssignMainImage, which feeds the existing GPU compositor unchanged.
 *
 * Audio: the container's first audio track is decoded on the same worker
 * thread (FFmpeg + swresample, downmixed to S16) and streamed through the
 * engine's global audio device (iTVPAudioStream / FAudio) with a small
 * rotated-block queue; the movie reports completion only after the sound has
 * finished.  A container without an audio track plays silent, exactly like
 * before.
 */
#ifndef SWITCH_MOVIE_OVERLAY_H
#define SWITCH_MOVIE_OVERLAY_H

#include "tjsCommHead.h"

#include <SDL.h>

#include <atomic>
#include <cstdint>
#include <vector>

/* DirectShow-style event codes the engine pump switches on; evcode.h only
 * exists on the win32 build, so self-define on other platforms. */
#ifndef EC_COMPLETE
#define EC_COMPLETE 0x01
#endif
#ifndef EC_UPDATE
#define EC_UPDATE 0x8001 /* (EC_USER+1) */
#endif

// krmovie.h pulls Win32-ish typedefs from the compat layer (already used
// by VideoOvlImpl.cpp on Switch). Relative path: both live one level up.
#include "../win32/krmovie.h"

struct AVFormatContext;
struct SwrContext;
class iTVPAudioStream;
struct AVCodecContext;
struct AVCodecParameters;
struct AVIOContext;
struct AVPacket;
struct AVFrame;
struct SwsContext;
class NativeEventQueueImplement;

class SwitchMovieOverlay : public iTVPVideoOverlay
{
public:
    explicit SwitchMovieOverlay(NativeEventQueueImplement *eventQueue);
    ~SwitchMovieOverlay(); // iTVPVideoOverlay has no virtual dtor

    /* Non-interface entry used by VideoOvlImpl::Open (Switch path).
     * Takes ownership of stream unconditionally.  Using the already-resolved
     * Kirikiri stream is essential: name may refer to an XP3/7z entry or an
     * autopath hit rather than a native filesystem path. */
    bool OpenStream(const ttstr &name, tTJSBinaryStream *stream,
                    long &width, long &height);

    /* iTVPVideoOverlay */
    void __stdcall AddRef() override { ++ref_; }
    void __stdcall Release() override
    {
        if (--ref_ == 0) delete this;
    }
    void __stdcall SetWindow(HWND /*window*/) override {}
    void __stdcall SetMessageDrainWindow(HWND /*window*/) override {}
    void __stdcall SetRect(RECT * /*rect*/) override {}
    void __stdcall SetVisible(bool /*b*/) override {}
    void __stdcall Play() override;
    void __stdcall Stop() override;
    void __stdcall Pause() override;
    void __stdcall SetPosition(unsigned long long /*tick*/) override {}
    void __stdcall GetPosition(unsigned long long * /*tick*/) override {}
    void __stdcall GetStatus(tTVPVideoStatus * status) override;
    void __stdcall GetEvent(long * evcode, LONG_PTR * param1,
                            LONG_PTR * param2, bool * got) override;
    void __stdcall FreeEventParams(long /*evcode*/, LONG_PTR /*param1*/,
                                   LONG_PTR /*param2*/) override {}
    void __stdcall Rewind() override;
    void __stdcall SetFrame(int /*f*/) override {}
    void __stdcall GetFrame(int * f) override;
    void __stdcall GetFPS(double * f) override;
    void __stdcall GetNumberOfFrame(int * f) override;
    void __stdcall GetTotalTime(long long * t) override;
    void __stdcall GetVideoSize(long * width, long * height) override;
    void __stdcall GetFrontBuffer(BYTE ** buff) override;
    void __stdcall SetVideoBuffer(BYTE * buff1, BYTE * buff2,
                                  long size) override;
    void __stdcall SetStopFrame(int /*frame*/) override {}
    void __stdcall GetStopFrame(int * /*frame*/) override {}
    void __stdcall SetDefaultStopFrame() override {}
    void __stdcall SetPlayRate(double /*rate*/) override {}
    void __stdcall GetPlayRate(double * /*rate*/) override {}
    void __stdcall SetAudioBalance(long /*balance*/) override {}
    void __stdcall GetAudioBalance(long * /*balance*/) override {}
    void __stdcall SetAudioVolume(long volume) override;
    void __stdcall GetAudioVolume(long * volume) override;
    void __stdcall GetNumberOfAudioStream(unsigned long * streamCount) override;
    void __stdcall SelectAudioStream(unsigned long /*num*/) override {}
    void __stdcall GetEnableAudioStreamNum(long * /*num*/) override {}
    void __stdcall DisableAudioStream() override {}
    void __stdcall GetNumberOfVideoStream(unsigned long * streamCount) override;
    void __stdcall SelectVideoStream(unsigned long /*num*/) override {}
    void __stdcall GetEnableVideoStreamNum(long * /*num*/) override {}
    void __stdcall SetMixingBitmap(HDC /*hdc*/, RECT * /*dest*/,
                                   float /*alpha*/) override {}
    void __stdcall ResetMixingBitmap() override {}
    void __stdcall SetMixingMovieAlpha(float /*a*/) override {}
    void __stdcall GetMixingMovieAlpha(float * /*a*/) override {}
    void __stdcall SetMixingMovieBGColor(unsigned long /*col*/) override {}
    void __stdcall GetMixingMovieBGColor(unsigned long * /*col*/) override {}
    void __stdcall PresentVideoImage() override {}
    void __stdcall GetContrastRangeMin(float * v) override { *v = 0; }
    void __stdcall GetContrastRangeMax(float * v) override { *v = 0; }
    void __stdcall GetContrastDefaultValue(float * v) override { *v = 0; }
    void __stdcall GetContrastStepSize(float * v) override { *v = 0; }
    void __stdcall GetContrast(float * v) override { *v = 0; }
    void __stdcall SetContrast(float /*v*/) override {}
    void __stdcall GetBrightnessRangeMin(float * v) override { *v = 0; }
    void __stdcall GetBrightnessRangeMax(float * v) override { *v = 0; }
    void __stdcall GetBrightnessDefaultValue(float * v) override { *v = 0; }
    void __stdcall GetBrightnessStepSize(float * v) override { *v = 0; }
    void __stdcall GetBrightness(float * v) override { *v = 0; }
    void __stdcall SetBrightness(float /*v*/) override {}
    void __stdcall GetHueRangeMin(float * v) override { *v = 0; }
    void __stdcall GetHueRangeMax(float * v) override { *v = 0; }
    void __stdcall GetHueDefaultValue(float * v) override { *v = 0; }
    void __stdcall GetHueStepSize(float * v) override { *v = 0; }
    void __stdcall GetHue(float * v) override { *v = 0; }
    void __stdcall SetHue(float /*v*/) override {}
    void __stdcall GetSaturationRangeMin(float * v) override { *v = 0; }
    void __stdcall GetSaturationRangeMax(float * v) override { *v = 0; }
    void __stdcall GetSaturationDefaultValue(float * v) override { *v = 0; }
    void __stdcall GetSaturationStepSize(float * v) override { *v = 0; }
    void __stdcall GetSaturation(float * v) override { *v = 0; }
    void __stdcall SetSaturation(float /*v*/) override {}

    // Called by VideoOvlImpl after the decoder has stopped, before its
    // NativeEventQueue is cleared or the player is released.
    void ClearEvents();

private:
    void CloseCodecs();
    static int SDLCALL DecodeThread(void * opaque);
    void DecodeLoop();
    bool PublishFrame(AVFrame * frame);
    bool WaitForPlaybackTime(double seconds);
    bool QueueEvent(long evcode, LONG_PTR p1, LONG_PTR p2,
                    bool mustDeliver = false);
    void ArmNotification();
    /* AVIO callbacks over Kirikiri storage (private members via opaque=this) */
    static int AvioRead2(void * opaque, uint8_t * dst, int size);
    static int64_t AvioSeek2(void * opaque, int64_t offset, int whence);

    /* bounded SPSC event ring (decode thread -> pump thread) */
    struct Event
    {
        long evcode = 0;
        LONG_PTR p1 = 0;
        LONG_PTR p2 = 0;
    };
    static constexpr int kEventSlots = 32;
    alignas(64) Event events_[kEventSlots];
    alignas(64) std::atomic<uint64_t> evRead_{0};
    alignas(64) std::atomic<uint64_t> evWrite_{0};
    std::atomic<bool> notifyPending_{false};

    std::atomic<int> ref_{1};
    std::atomic<tTVPVideoStatus> status_{vsStopped};
    std::atomic<bool> quit_{false};
    std::atomic<bool> decoding_{false};
    std::atomic<int> frontBuf_{0};   // 0/1 -> which SetVideoBuffer buffer
    std::atomic<int> frameCount_{0}; // decoded frame index (0-based)

    SDL_Thread * thread_ = nullptr;
    NativeEventQueueImplement * eventQueue_ = nullptr;
    Uint32 startedMs_ = 0;
    double fps_ = 30.0;
    long width_ = 0;
    long height_ = 0;
    int frameStride_ = 0; // width*4

    BYTE * buffers_[2] = {nullptr, nullptr};
    long bufferSize_ = 0;

    tTJSBinaryStream * stream_ = nullptr;
    int64_t streamSize_ = 0;
    AVIOContext * avio_ = nullptr;
    AVFormatContext * format_ = nullptr;
    AVCodecContext * videoCodec_ = nullptr;
    SwsContext * sws_ = nullptr;
    int videoStream_ = -1;
    double durationSec_ = 0.0;
    int64_t totalFrames_ = 0;
    ttstr openPath_; // storage name for Rewind (full reopen)

    /* ---- audio track (same worker thread decodes, FAudio plays) ---- */
    static const size_t kPcmRingBytes = 1u << 20; // absorb decode-ahead
    static const size_t kAudioBlockBytes = 4096 * 4; // ~21ms @48k stereo
    static const int kAudioBlocks = 4;               // 3 queued + 1 free
    int audioStream_ = -1;
    AVCodecContext * audioCodec_ = nullptr;
    SwrContext * swrResample_ = nullptr;
    iTVPAudioStream * audioOut_ = nullptr;
    int audioRate_ = 0;
    int audioChannels_ = 2;
    long audioVolume_ = 100000;
    std::vector<uint8_t> pcmRing_;
    size_t pcmRead_ = 0;  // monotonic offsets into the ring (decode thread only)
    size_t pcmWrite_ = 0;
    std::vector<uint8_t *> audioBlocks_;
    std::atomic<int> audioFreeBlocks_{0};
    int nextAudioBlock_ = 0;
    bool audioEof_ = false;

    void CloseAudio();
    bool OpenAudioOutput();
    void DecodeAudioPacket(AVPacket * pkt);
    void ConsumeAudioFrame(AVFrame * frame);
    void FeedAudio(bool flush);
    static void AudioQueueCb(iTVPAudioStream * stream, void * user);
};

#endif // SWITCH_MOVIE_OVERLAY_H
