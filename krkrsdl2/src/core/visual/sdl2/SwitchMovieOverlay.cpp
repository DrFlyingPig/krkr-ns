/* SPDX-License-Identifier: MIT */
/* KRKR-ns Phase 4: FFmpeg movie overlay implementation (see header). */

#include "SwitchMovieOverlay.h"

#include <cstring>
#include <limits>

#include "MsgIntf.h"
#include "StorageIntf.h"
#include "KrkrNSLog.h"
#include "NativeEventQueue.h"

#include "AudioDevice.h"

// From QueueSoundBufferImpl.cpp: the engine's global audio output device and
// its mixing format.  May return nullptr before sound is initialized.
iTVPAudioDevice * TVPGetInitializedAudioDevice(tjs_uint32 * rate,
                                               tjs_uint32 * channels);

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace
{
void MovieAvLog(void * /*avcl*/, int level, const char * fmt, va_list args)
{
    if (level > AV_LOG_INFO) return;
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    KRKRNS_LOG("[movie] %s", buf);
}
} // namespace

/* ---- AVIO callbacks over tTJSBinaryStream (class-private via opaque) ---- */
int SwitchMovieOverlay::AvioRead2(void * opaque, uint8_t * dst, int size)
{
    SwitchMovieOverlay * self = static_cast<SwitchMovieOverlay *>(opaque);
    if (!self || !self->stream_ || !dst || size <= 0) return AVERROR(EIO);
    int total = 0;
    try
    {
        // Archive streams may legitimately return a short read at a segment
        // boundary.  Fill the request when possible, and report a real EOF
        // code instead of zero as required by AVIO's stream callback contract.
        while (total < size)
        {
            const tjs_uint got = self->stream_->Read(
                dst + total, static_cast<tjs_uint>(size - total));
            if (got == 0) break;
            total += static_cast<int>(got);
        }
    }
    catch (...)
    {
        KRKRNS_LOG("[movie] storage read failed at %d/%d", total, size);
        return total > 0 ? total : AVERROR(EIO);
    }
    return total > 0 ? total : AVERROR_EOF;
}

int64_t SwitchMovieOverlay::AvioSeek2(void * opaque, int64_t offset, int whence)
{
    SwitchMovieOverlay * self = static_cast<SwitchMovieOverlay *>(opaque);
    if (!self || !self->stream_) return AVERROR(EIO);
    if (whence & AVSEEK_SIZE)
        return self->streamSize_;

    try
    {
        tjs_int base;
        int64_t target = offset;
        switch (whence & 0xFFFF)
        {
        case SEEK_SET:
            base = TJS_BS_SEEK_SET;
            break;
        case SEEK_CUR:
        {
            const tjs_uint64 current = self->stream_->GetPosition();
            if (current > static_cast<tjs_uint64>(
                              std::numeric_limits<int64_t>::max()))
                return AVERROR(EINVAL);
            const int64_t current64 = static_cast<int64_t>(current);
            if ((offset > 0 && current64 >
                 std::numeric_limits<int64_t>::max() - offset) ||
                (offset < 0 && current64 <
                 std::numeric_limits<int64_t>::min() - offset))
                return AVERROR(EINVAL);
            target = current64 + offset;
            base = TJS_BS_SEEK_CUR;
            break;
        }
        case SEEK_END:
            if ((offset > 0 && self->streamSize_ >
                 std::numeric_limits<int64_t>::max() - offset) ||
                (offset < 0 && self->streamSize_ <
                 std::numeric_limits<int64_t>::min() - offset))
                return AVERROR(EINVAL);
            target = self->streamSize_ + offset;
            base = TJS_BS_SEEK_END;
            break;
        default:
            return AVERROR(EINVAL);
        }
        if (target < 0 || target > self->streamSize_)
            return AVERROR(EINVAL);

        const tjs_uint64 pos = self->stream_->Seek(offset, base);
        if (pos > static_cast<tjs_uint64>(
                      std::numeric_limits<int64_t>::max()))
            return AVERROR(EINVAL);
        if (static_cast<int64_t>(pos) != target)
            return AVERROR(EIO);
        return target;
    }
    catch (...)
    {
        KRKRNS_LOG("[movie] storage seek failed offset=%lld whence=%d",
                   static_cast<long long>(offset), whence);
        return AVERROR(EIO);
    }
}

SwitchMovieOverlay::SwitchMovieOverlay(NativeEventQueueImplement *eventQueue)
    : eventQueue_(eventQueue)
{
    for (auto & e : events_) e = Event{};
    av_log_set_callback(MovieAvLog);
}

SwitchMovieOverlay::~SwitchMovieOverlay()
{
    Stop();
    CloseCodecs();
    CloseAudio();
}

bool SwitchMovieOverlay::OpenStream(const ttstr & name,
                                    tTJSBinaryStream * stream,
                                    long & width, long & height)
{
    Stop();
    CloseCodecs();
    // Ownership transfers at this point, including every failure path below.
    stream_ = stream;
    openPath_ = name;
    if (!stream_)
    {
        KRKRNS_LOG("[movie] open failed: null storage stream");
        return false;
    }

    const char * demuxerName = nullptr;
    try
    {
        const tjs_uint64 size = stream_->GetSize();
        if (size == 0 || size > static_cast<tjs_uint64>(
                                    std::numeric_limits<int64_t>::max()))
        {
            KRKRNS_LOG("[movie] invalid storage size=%llu",
                       static_cast<unsigned long long>(size));
            CloseCodecs();
            return false;
        }
        streamSize_ = static_cast<int64_t>(size);
        if (stream_->Seek(0, TJS_BS_SEEK_SET) != 0)
        {
            KRKRNS_LOG("[movie] storage rewind failed");
            CloseCodecs();
            return false;
        }

        // Do not trust the extension: at least one shipped game names an
        // ISO-BMFF/H.264 file "opmovie.wmv".  Select from the real signature,
        // while retaining auto-probe only as a diagnostic fallback.
        uint8_t signature[16] = {};
        const tjs_uint signatureSize = stream_->Read(signature,
                                                     sizeof(signature));
        if (stream_->Seek(0, TJS_BS_SEEK_SET) != 0)
        {
            KRKRNS_LOG("[movie] storage rewind after signature failed");
            CloseCodecs();
            return false;
        }
        static const uint8_t asfGuid[16] = {
            0x30, 0x26, 0xb2, 0x75, 0x8e, 0x66, 0xcf, 0x11,
            0xa6, 0xd9, 0x00, 0xaa, 0x00, 0x62, 0xce, 0x6c
        };
        if (signatureSize >= 12 &&
            std::memcmp(signature + 4, "ftyp", 4) == 0)
            demuxerName = "mov";
        else if (signatureSize >= sizeof(asfGuid) &&
                 std::memcmp(signature, asfGuid, sizeof(asfGuid)) == 0)
            demuxerName = "asf";

        KRKRNS_LOG("[movie] storage ready size=%lld container=%s sig=%02x%02x%02x%02x/%02x%02x%02x%02x",
                   static_cast<long long>(streamSize_),
                   demuxerName ? demuxerName : "auto",
                   signature[0], signature[1], signature[2], signature[3],
                   signature[4], signature[5], signature[6], signature[7]);
    }
    catch (...)
    {
        KRKRNS_LOG("[movie] storage initialization failed");
        CloseCodecs();
        return false;
    }

    // FFmpeg owns this buffer once avio_alloc_context succeeds.  It must be
    // allocated with av_malloc because libavformat may replace/free it.
    constexpr int kAvioBufferSize = 256 * 1024;
    uint8_t * avioBuffer = static_cast<uint8_t *>(av_malloc(kAvioBufferSize));
    if (!avioBuffer)
    {
        KRKRNS_LOG("[movie] AVIO buffer allocation failed");
        CloseCodecs();
        return false;
    }
    avio_ = avio_alloc_context(
        avioBuffer, kAvioBufferSize, 0, this,
        AvioRead2, nullptr, AvioSeek2);
    if (!avio_)
    {
        av_free(avioBuffer);
        CloseCodecs();
        return false;
    }

    format_ = avformat_alloc_context();
    if (!format_)
    {
        CloseCodecs();
        return false;
    }
    format_->pb = avio_;
    format_->flags |= AVFMT_FLAG_CUSTOM_IO;
    const AVInputFormat * ifmt = demuxerName
        ? av_find_input_format(demuxerName) : nullptr;
    if (demuxerName && !ifmt)
    {
        KRKRNS_LOG("[movie] demuxer unavailable: %s", demuxerName);
        CloseCodecs();
        return false;
    }
    const int openResult = avformat_open_input(&format_, "", ifmt, nullptr);
    if (openResult < 0)
    {
        char error[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(openResult, error, sizeof(error));
        KRKRNS_LOG("[movie] avformat_open_input failed code=%d (%s) container=%s",
                   openResult, error, demuxerName ? demuxerName : "auto");
        CloseCodecs();
        return false;
    }
    if (avformat_find_stream_info(format_, nullptr) < 0)
    {
        KRKRNS_LOG("[movie] find_stream_info failed");
        CloseCodecs();
        return false;
    }

    videoStream_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1,
                                       nullptr, 0);
    if (videoStream_ < 0)
    {
        KRKRNS_LOG("[movie] no video stream");
        CloseCodecs();
        return false;
    }
    AVStream * st = format_->streams[videoStream_];
    const AVCodec * codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec)
    {
        KRKRNS_LOG("[movie] no decoder for codec %d", st->codecpar->codec_id);
        CloseCodecs();
        return false;
    }
    videoCodec_ = avcodec_alloc_context3(codec);
    if (!videoCodec_ || avcodec_parameters_to_context(videoCodec_, st->codecpar) < 0 ||
        avcodec_open2(videoCodec_, codec, nullptr) < 0)
    {
        KRKRNS_LOG("[movie] video decoder open failed");
        CloseCodecs();
        return false;
    }

    // Audio track (best effort): when present and decodable, it is decoded
    // alongside the video and streamed through the engine's audio device.
    // Any failure here simply falls back to the previous silent playback.
    audioStream_ = -1;
    {
        const int a = av_find_best_stream(format_, AVMEDIA_TYPE_AUDIO, -1,
                                          videoStream_, nullptr, 0);
        if (a >= 0)
        {
            AVStream * ast = format_->streams[a];
            const AVCodec * acodec =
                avcodec_find_decoder(ast->codecpar->codec_id);
            if (!acodec)
            {
                KRKRNS_LOG("[movie] no built-in audio decoder for codec %d",
                           (int)ast->codecpar->codec_id);
            }
            else
            {
                audioCodec_ = avcodec_alloc_context3(acodec);
                if (!audioCodec_ ||
                    avcodec_parameters_to_context(audioCodec_, ast->codecpar) < 0 ||
                    avcodec_open2(audioCodec_, acodec, nullptr) < 0)
                {
                    KRKRNS_LOG("[movie] audio decoder open failed codec=%d",
                               (int)ast->codecpar->codec_id);
                    avcodec_free_context(&audioCodec_);
                }
            }
            if (audioCodec_)
            {
                audioStream_ = a;
                audioRate_ = audioCodec_->sample_rate;
                audioChannels_ = audioCodec_->ch_layout.nb_channels;
                if (audioChannels_ != 1 && audioChannels_ != 2)
                    audioChannels_ = 2; // FAudio mixes anything wider down
                KRKRNS_LOG("[movie] audio stream idx=%d codec=%d rate=%d ch=%d",
                           audioStream_, (int)ast->codecpar->codec_id,
                           audioRate_, audioChannels_);
            }
        }
        else
        {
            KRKRNS_LOG("[movie] container has no audio stream");
        }
    }

    width_ = videoCodec_->width;
    height_ = videoCodec_->height;
    if (width_ <= 0 || height_ <= 0)
    {
        CloseCodecs();
        return false;
    }
    frameStride_ = width_ * 4;
    width = width_;
    height = height_;

    // FPS from the stream (fallbacks per WA2-ns experience).
    fps_ = 30.0;
    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
        fps_ = av_q2d(st->avg_frame_rate);
    else if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0)
        fps_ = av_q2d(st->r_frame_rate);
    if (fps_ < 1.0) fps_ = 30.0;
    if (fps_ > 120.0) fps_ = 120.0;

    if (format_->duration > 0)
        durationSec_ = (double)format_->duration / AV_TIME_BASE;
    totalFrames_ = st->nb_frames > 0 ? st->nb_frames : 0;
    if (totalFrames_ <= 0 && durationSec_ > 0)
        totalFrames_ = (int64_t)(durationSec_ * fps_ + 0.5);

    KRKRNS_LOG("[movie] ready %ldx%ld fps=%.2f frames=%lld dur=%.2fs",
               width_, height_, fps_, (long long)totalFrames_, durationSec_);
    return true;
}

void SwitchMovieOverlay::CloseCodecs()
{
    if (sws_)
        sws_freeContext(sws_), sws_ = nullptr;
    if (swrResample_)
        swr_free(&swrResample_), swrResample_ = nullptr;
    if (audioCodec_)
        avcodec_free_context(&audioCodec_);
    audioStream_ = -1;
    audioRate_ = 0;
    audioEof_ = false;
    pcmRead_ = pcmWrite_ = 0;
    if (videoCodec_)
        avcodec_free_context(&videoCodec_);
    if (format_)
    {
        // avio is attached manually: detach so avformat_close_input does
        // not free it, then free the context and our buffer separately.
        if (format_->pb == avio_) format_->pb = nullptr;
        avformat_close_input(&format_);
    }
    if (avio_)
        avio_context_free(&avio_);
    if (stream_)
    {
        delete stream_;
        stream_ = nullptr;
    }
    streamSize_ = 0;
    videoStream_ = -1;
    durationSec_ = 0;
    totalFrames_ = 0;
    width_ = height_ = 0;
    frameStride_ = 0;
    frameCount_.store(0);
    status_.store(vsStopped);
}

void SwitchMovieOverlay::Play()
{
    if (decoding_.load())
    {
        // play() after pause resumes the existing decode thread.
        if (status_.load() == vsPaused) status_.store(vsPlaying);
        return;
    }
    if (!videoCodec_ || width_ <= 0 || height_ <= 0 ||
        !buffers_[0] || !buffers_[1] ||
        bufferSize_ < width_ * height_ * 4)
    {
        KRKRNS_LOG("[movie] play rejected codec=%d size=%ldx%ld buffers=%d/%d bytes=%ld",
                   videoCodec_ ? 1 : 0, width_, height_,
                   buffers_[0] ? 1 : 0, buffers_[1] ? 1 : 0, bufferSize_);
        return;
    }

    quit_.store(false);
    frameCount_.store(0);
    frontBuf_.store(0);
    // drain any leftover events from a previous run
    evRead_.store(evWrite_.load(std::memory_order_acquire));
    startedMs_ = SDL_GetTicks();
    // audio output (best effort): created on the first play, restarted on
    // every play; a missing device keeps the previous silent behaviour.
    audioEof_ = false;
    pcmRead_ = pcmWrite_ = 0;
    if (audioStream_ >= 0 && OpenAudioOutput())
    {
        audioFreeBlocks_.store(kAudioBlocks);
        nextAudioBlock_ = 0;
        audioOut_->StartStream();
        KRKRNS_LOG("[movie] audio output started rate=%d ch=%d",
                   audioRate_, audioChannels_);
    }
    status_.store(vsPlaying);
    decoding_.store(true);
    thread_ = SDL_CreateThreadWithStackSize(DecodeThread, "movie",
                                            4u * 1024u * 1024u, this);
    if (!thread_)
    {
        KRKRNS_LOG("[movie] thread create failed: %s", SDL_GetError());
        quit_.store(true);
        decoding_.store(false);
        status_.store(vsStopped);
    }
    else
    {
        KRKRNS_LOG("[movie] decode thread started stack=4MiB");
    }
}

void SwitchMovieOverlay::Stop()
{
    quit_.store(true);
    // The player owns all FFmpeg contexts used by the worker.  Keep the SDL
    // handle joinable so CloseCodecs can never race a detached decoder.
    SDL_Thread *thread = thread_;
    thread_ = nullptr;
    if (thread)
    {
        SDL_WaitThread(thread, nullptr);
    }
    if (audioOut_)
    {
        // stop and drop whatever buffered sound is left over
        audioOut_->StopStream();
    }
    decoding_.store(false);
    status_.store(vsStopped);
}

void SwitchMovieOverlay::Pause()
{
    if (status_.load() == vsPlaying)
        status_.store(vsPaused);
}

void SwitchMovieOverlay::Rewind()
{
    Stop();
    ClearEvents();
    // No seek support by design (WA2-ns: rewind = full reopen). Reopen the
    // same Kirikiri storage and restart.
    long w = 0, h = 0;
    tTJSBinaryStream *stream = nullptr;
    try
    {
        stream = TVPCreateStream(openPath_);
    }
    catch (...)
    {
        KRKRNS_LOG("[movie] rewind storage reopen failed");
        return;
    }
    if (OpenStream(openPath_, stream, w, h))
        Play();
}

void SwitchMovieOverlay::GetStatus(tTVPVideoStatus * status)
{
    if (status) *status = status_.load();
}

void SwitchMovieOverlay::GetEvent(long * evcode, LONG_PTR * param1,
                                  LONG_PTR * param2, bool * got)
{
    if (got) *got = false;
    const uint64_t r = evRead_.load(std::memory_order_relaxed);
    const uint64_t w = evWrite_.load(std::memory_order_acquire);
    if (r >= w)
    {
        // The one coalesced NativeEvent has now drained the SPSC ring.  A
        // producer racing this transition either observes false itself or is
        // caught by the recheck and gets another main-thread notification.
        notifyPending_.store(false, std::memory_order_release);
        if (evRead_.load(std::memory_order_acquire) <
            evWrite_.load(std::memory_order_acquire))
            ArmNotification();
        return;
    }
    const Event & e = events_[r % kEventSlots];
    if (evcode) *evcode = e.evcode;
    if (param1) *param1 = e.p1;
    if (param2) *param2 = e.p2;
    if (got) *got = true;
    evRead_.store(r + 1, std::memory_order_release);
}

void SwitchMovieOverlay::ClearEvents()
{
    evRead_.store(evWrite_.load(std::memory_order_acquire),
                  std::memory_order_release);
    notifyPending_.store(false, std::memory_order_release);
    if (evRead_.load(std::memory_order_acquire) <
        evWrite_.load(std::memory_order_acquire))
        ArmNotification();
}

void SwitchMovieOverlay::ArmNotification()
{
    if (!eventQueue_) return;
    bool expected = false;
    if (notifyPending_.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel))
    {
        NativeEvent ev(WM_GRAPHNOTIFY);
        eventQueue_->PostEvent(ev);
    }
}

bool SwitchMovieOverlay::QueueEvent(long evcode, LONG_PTR p1, LONG_PTR p2,
                                    bool mustDeliver)
{
    for (;;)
    {
        const uint64_t w = evWrite_.load(std::memory_order_relaxed);
        const uint64_t r = evRead_.load(std::memory_order_acquire);
        if (w - r < kEventSlots - 1)
        {
            events_[w % kEventSlots] = Event{evcode, p1, p2};
            evWrite_.store(w + 1, std::memory_order_release);
            ArmNotification();
            return true;
        }
        if (!mustDeliver || quit_.load()) return false;
        SDL_Delay(1);
    }
}

void SwitchMovieOverlay::GetFrame(int * f)
{
    if (f) *f = frameCount_.load();
}

void SwitchMovieOverlay::GetFPS(double * f)
{
    if (f) *f = fps_;
}

void SwitchMovieOverlay::GetNumberOfFrame(int * f)
{
    if (f) *f = (int)totalFrames_;
}

void SwitchMovieOverlay::GetTotalTime(long long * t)
{
    if (t) *t = (long long)(durationSec_ * 1000.0);
}

void SwitchMovieOverlay::GetVideoSize(long * width, long * height)
{
    if (width) *width = width_;
    if (height) *height = height_;
}

void SwitchMovieOverlay::GetFrontBuffer(BYTE ** buff)
{
    if (buff) *buff = buffers_[frontBuf_.load(std::memory_order_acquire)];
}

void SwitchMovieOverlay::SetVideoBuffer(BYTE * buff1, BYTE * buff2, long size)
{
    buffers_[0] = buff1;
    buffers_[1] = buff2;
    bufferSize_ = size;
}

void SwitchMovieOverlay::GetNumberOfAudioStream(unsigned long * streamCount)
{
    if (streamCount) *streamCount = audioStream_ >= 0 ? 1 : 0;
}

void SwitchMovieOverlay::GetNumberOfVideoStream(unsigned long * streamCount)
{
    if (streamCount) *streamCount = 1;
}

/* ---- decode thread ---- */

int SDLCALL SwitchMovieOverlay::DecodeThread(void * opaque)
{
    static_cast<SwitchMovieOverlay *>(opaque)->DecodeLoop();
    return 0;
}

void SwitchMovieOverlay::DecodeLoop()
{
    AVPacket * pkt = av_packet_alloc();
    AVFrame * frame = av_frame_alloc();
    if (!pkt || !frame)
    {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        decoding_.store(false);
        status_.store(vsStopped);
        return;
    }

    sws_ = sws_getContext(width_, height_, videoCodec_->pix_fmt,
                          width_, height_, AV_PIX_FMT_BGRA,
                          SWS_POINT, nullptr, nullptr, nullptr);
    if (!sws_)
    {
        KRKRNS_LOG("[movie] sws_getContext failed");
        status_.store(vsStopped);
        QueueEvent(EC_COMPLETE, 0, 0, true);
        av_packet_free(&pkt);
        av_frame_free(&frame);
        decoding_.store(false);
        return;
    }

    auto receiveFrames = [&]() -> bool
    {
        for (;;)
        {
            const int got = avcodec_receive_frame(videoCodec_, frame);
            if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) return true;
            if (got < 0)
            {
                char err[128];
                av_strerror(got, err, sizeof(err));
                KRKRNS_LOG("[movie] receive_frame failed %d (%s)", got, err);
                return false;
            }
            const bool published = PublishFrame(frame);
            av_frame_unref(frame);
            if (!published) return false;
        }
    };

    bool reachedEnd = false;
    while (!quit_.load())
    {
        const int read = av_read_frame(format_, pkt);
        if (read < 0)
        {
            reachedEnd = true;
            char err[128];
            av_strerror(read, err, sizeof(err));
            KRKRNS_LOG("[movie] demux end frames=%d code=%d (%s)",
                       frameCount_.load(), read, err);
            int sent = avcodec_send_packet(videoCodec_, nullptr);
            if (sent == AVERROR(EAGAIN))
            {
                receiveFrames();
                sent = avcodec_send_packet(videoCodec_, nullptr);
            }
            if (sent >= 0 || sent == AVERROR_EOF) receiveFrames();
            break;
        }
        if (pkt->stream_index == audioStream_)
        {
            DecodeAudioPacket(pkt);
            av_packet_unref(pkt);
            FeedAudio(false);
            continue;
        }
        if (pkt->stream_index != videoStream_)
        {
            av_packet_unref(pkt);
            continue;
        }

        int sent = avcodec_send_packet(videoCodec_, pkt);
        if (sent == AVERROR(EAGAIN))
        {
            if (!receiveFrames())
            {
                av_packet_unref(pkt);
                break;
            }
            sent = avcodec_send_packet(videoCodec_, pkt);
        }
        av_packet_unref(pkt);
        if (sent < 0)
        {
            char err[128];
            av_strerror(sent, err, sizeof(err));
            KRKRNS_LOG("[movie] send_packet skipped %d (%s)", sent, err);
            continue;
        }
        if (!receiveFrames()) break;
    }

    if (reachedEnd && !quit_.load())
    {
        // Keep the final frame for roughly one frame interval, then let the
        // KRKR event handler perform its normal Stop/loop transition.
        if (frameCount_.load() > 0)
            WaitForPlaybackTime((double)frameCount_.load() / fps_);
        // Let the sound finish too (the tail chunk plus whatever is still
        // queued), bounded so a stuck device cannot stall the movie end.
        if (audioOut_)
        {
            FeedAudio(true);
            int waited = 0;
            while (!quit_.load() && audioFreeBlocks_.load() < kAudioBlocks &&
                   waited < 4000)
            {
                SDL_Delay(8);
                waited += 8;
            }
            audioOut_->StopStream();
        }
        if (!quit_.load())
        {
            status_.store(vsEnded);
            QueueEvent(EC_COMPLETE, 0, 0, true);
            KRKRNS_LOG("[movie] complete frames=%d", frameCount_.load());
        }
    }
    else
    {
        status_.store(vsStopped);
    }

    if (sws_)
        sws_freeContext(sws_), sws_ = nullptr;
    av_packet_free(&pkt);
    av_frame_free(&frame);
    decoding_.store(false);
}

bool SwitchMovieOverlay::WaitForPlaybackTime(double seconds)
{
    while (!quit_.load())
    {
        if (status_.load() == vsPaused)
        {
            const Uint32 pausedAt = SDL_GetTicks();
            while (status_.load() == vsPaused && !quit_.load())
                SDL_Delay(8);
            startedMs_ += SDL_GetTicks() - pausedAt;
            continue;
        }
        const double elapsed = (SDL_GetTicks() - startedMs_) / 1000.0;
        const double lead = seconds - elapsed;
        if (lead <= 0.066) return true;
        SDL_Delay(8);
    }
    return false;
}

bool SwitchMovieOverlay::PublishFrame(AVFrame * frame)
{
    if (!frame || quit_.load() || !sws_) return false;
    const int idx = frameCount_.load(std::memory_order_relaxed);
    if (!WaitForPlaybackTime((double)idx / fps_)) return false;

    const int slot = idx & 1;
    BYTE *buffer = buffers_[slot];
    if (!buffer) return false;
    uint8_t *dst[4] = {
        buffer + (height_ - 1) * frameStride_, nullptr, nullptr, nullptr};
    int stride[4] = {-frameStride_, 0, 0, 0};
    const int rows = sws_scale(sws_, frame->data, frame->linesize,
                               0, height_, dst, stride);
    if (rows <= 0)
    {
        KRKRNS_LOG("[movie] sws_scale failed frame=%d rows=%d", idx, rows);
        return false;
    }

    // Publish the exact slot just written.  The previous implementation
    // always wrote slot 0 while advertising 0/1 alternately, exposing an
    // untouched bitmap on every other frame.
    frontBuf_.store(slot, std::memory_order_release);
    frameCount_.store(idx + 1, std::memory_order_release);
    QueueEvent(EC_UPDATE, (LONG_PTR)(idx + 1), 0, false);
    if (idx == 0 || ((idx + 1) % 120) == 0)
        KRKRNS_LOG("[movie] published frame=%d slot=%d", idx + 1, slot);
    return true;
}


/* ---- audio track ---- */

bool SwitchMovieOverlay::OpenAudioOutput()
{
    if (audioOut_) return true;
    if (audioStream_ < 0 || audioRate_ <= 0) return false;
    tjs_uint32 devRate = 48000, devCh = 2;
    iTVPAudioDevice * dev = TVPGetInitializedAudioDevice(&devRate, &devCh);
    if (!dev)
    {
        KRKRNS_LOG("[movie] no audio device; movie plays silent");
        return false;
    }
    tTVPAudioStreamParam p;
    p.Channels = audioChannels_;
    p.SampleRate = audioRate_;
    p.BitsPerSample = 16;
    p.SampleType = astInt16;
    p.FramesPerBuffer = kAudioBlockBytes / (audioChannels_ * 2);
    try
    {
        audioOut_ = dev->CreateAudioStream(p);
    }
    catch (...)
    {
        audioOut_ = nullptr;
    }
    if (!audioOut_)
    {
        KRKRNS_LOG("[movie] audio stream create failed");
        return false;
    }
    audioOut_->SetVolume((tjs_int)audioVolume_);
    audioOut_->SetCallback(AudioQueueCb, this);
    pcmRing_.assign(kPcmRingBytes, 0);
    pcmRead_ = pcmWrite_ = 0;
    audioBlocks_.assign(kAudioBlocks, nullptr);
    for (auto & b : audioBlocks_)
        b = new uint8_t[kAudioBlockBytes];
    KRKRNS_LOG("[movie] audio output ready rate=%d ch=%d", audioRate_,
               audioChannels_);
    return true;
}

void SwitchMovieOverlay::CloseAudio()
{
    if (audioOut_)
    {
        audioOut_->StopStream();
        audioOut_->SetCallback(nullptr, nullptr);
        delete audioOut_;
        audioOut_ = nullptr;
    }
    for (auto & b : audioBlocks_)
        delete[] b;
    audioBlocks_.clear();
    pcmRing_.clear();
    pcmRing_.shrink_to_fit();
    pcmRead_ = pcmWrite_ = 0;
    audioFreeBlocks_.store(0);
}

void SwitchMovieOverlay::AudioQueueCb(iTVPAudioStream * stream, void * user)
{
    (void)stream;
    // FAudio worker thread: one enqueued block has been consumed and its
    // memory is free for reuse again.
    static_cast<SwitchMovieOverlay *>(user)->audioFreeBlocks_.fetch_add(1);
}

// Convert one decoded audio frame to interleaved S16 (downmixing via
// swresample) and append it to the PCM ring.  The ring is touched only by
// the decode thread; when it runs full the decode loop simply waits, which
// throttles the whole decode pipeline against the audio clock.
void SwitchMovieOverlay::ConsumeAudioFrame(AVFrame * frame)
{
    if (quit_.load()) return;
    if (!swrResample_)
    {
        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, audioChannels_);
        if (swr_alloc_set_opts2(&swrResample_, &outLayout, AV_SAMPLE_FMT_S16,
                                audioRate_, &frame->ch_layout,
                                (AVSampleFormat)frame->format,
                                frame->sample_rate, 0, nullptr) < 0 ||
            !swrResample_)
        {
            KRKRNS_LOG("[movie] swr_alloc_set_opts2 failed");
            return;
        }
        if (swr_init(swrResample_) < 0)
        {
            KRKRNS_LOG("[movie] swr_init failed");
            swr_free(&swrResample_), swrResample_ = nullptr;
            return;
        }
    }

    const int bytesPerFrame = audioChannels_ * 2;
    const int dstCap = frame->nb_samples + 256;
    std::vector<uint8_t> converted((size_t)dstCap * bytesPerFrame);
    uint8_t * outPlane[1] = {converted.data()};
    const int got = swr_convert(swrResample_, outPlane, dstCap,
                                (const uint8_t **)frame->extended_data,
                                frame->nb_samples);
    if (got <= 0) return;
    const size_t bytes = (size_t)got * bytesPerFrame;

    size_t written = 0;
    while (written < bytes)
    {
        const size_t cap = pcmRing_.size();
        const size_t used = pcmWrite_ - pcmRead_;
        const size_t space = cap - used;
        if (space == 0 || quit_.load()) return;
        const size_t n = std::min(bytes - written, space);
        const size_t pos = pcmWrite_ % cap;
        const size_t tail = std::min(n, cap - pos);
        std::memcpy(pcmRing_.data() + pos, converted.data() + written, tail);
        if (n > tail)
            std::memcpy(pcmRing_.data(), converted.data() + written + tail,
                        n - tail);
        pcmWrite_ += n;
        written += n;
        FeedAudio(false);
        if (written < bytes) SDL_Delay(4);
    }
}

// Decode one audio packet into the PCM ring.
void SwitchMovieOverlay::DecodeAudioPacket(AVPacket * pkt)
{
    if (!audioCodec_) return;
    int sent = avcodec_send_packet(audioCodec_, pkt);
    if (sent == AVERROR(EAGAIN))
    {
        // codec queue full: pull one frame out and retry once
        AVFrame * f = av_frame_alloc();
        if (f)
        {
            if (avcodec_receive_frame(audioCodec_, f) >= 0)
                ConsumeAudioFrame(f);
            av_frame_free(&f);
        }
        sent = avcodec_send_packet(audioCodec_, pkt);
    }
    if (sent < 0) return;
    for (;;)
    {
        AVFrame * f = av_frame_alloc();
        const int got = avcodec_receive_frame(audioCodec_, f);
        if (got < 0)
        {
            av_frame_free(&f);
            break;
        }
        ConsumeAudioFrame(f);
        av_frame_free(&f);
    }
}

// Hand whole blocks of buffered PCM to the audio device.  flush=true also
// sends a partial tail (marked end-of-stream) when the movie is over.
void SwitchMovieOverlay::FeedAudio(bool flush)
{
    if (!audioOut_ || quit_.load()) return;
    const size_t cap = pcmRing_.size();
    const size_t available = pcmWrite_ - pcmRead_;
    if (available == 0) return;
    const size_t n = (available >= kAudioBlockBytes && !flush)
        ? kAudioBlockBytes : available;
    if (!flush && n < kAudioBlockBytes) return;
    if (audioFreeBlocks_.load() <= 0) return;
    if ((int)audioOut_->GetQueuedCount() >= kAudioBlocks - 1) return;

    uint8_t * block = audioBlocks_[nextAudioBlock_];
    for (size_t done = 0; done < n; )
    {
        const size_t pos = pcmRead_ % cap;
        const size_t t = std::min(n - done, cap - pos);
        std::memcpy(block + done, pcmRing_.data() + pos, t);
        pcmRead_ += t;
        done += t;
    }
    const bool last = flush && n == available;
    audioFreeBlocks_.fetch_sub(1);
    audioOut_->Enqueue(block, n, last);
    nextAudioBlock_ = (nextAudioBlock_ + 1) % audioBlocks_.size();
}

void SwitchMovieOverlay::SetAudioVolume(long volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100000) volume = 100000;
    audioVolume_ = volume;
    if (audioOut_) audioOut_->SetVolume((tjs_int)volume);
}

void SwitchMovieOverlay::GetAudioVolume(long * volume)
{
    if (volume) *volume = audioVolume_;
}
