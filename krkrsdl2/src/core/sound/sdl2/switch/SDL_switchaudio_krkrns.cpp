/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2018 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty. In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:
  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  KRKR-ns altered version of devkitPro SDL's src/audio/switch/SDL_switchaudio.c
  at commit 0738d3c9f6993875e2f3dd0e8cc0bb4ae4440b4e (switch-sdl-2.28).
  Keep the existing audren/S16/two-buffer output. Wait for reusable terminal
  buffer states, rather than requiring observation of transient Playing.
  Link this object before libSDL2.a to replace only SWITCHAUDIO_bootstrap.
*/

#ifdef KRKRNS_SDL_AUDIO_TEST
#include "tests/fake_backend.h"
#else
#include "SDL_sysaudio_2_28_5.h"
#include "KrkrNSLog.h"
#include <switch.h>
#include <malloc.h>
#endif

struct SDL_PrivateAudioData {
    AudioDriver driver;
    AudioDriverWaveBuf buffer[2];
    Uint8 *buffer_tmp;
    void *pool;
    bool audr_device;
    bool audr_driver;
    bool renderer_started;
    bool in_flight[2];
    bool observed_playing[2];
    Uint32 last_played;
    Uint32 last_diagnostic;
    Uint64 mix_blocks;
    Uint64 submitted_blocks;
    Uint64 completed_blocks;
    Uint64 played_samples;
    Uint64 skipped_playing;
    Uint64 voice_restarts;
};

static const AudioRendererConfig krkrns_ar_config = {
    AudioRendererOutputRate_48kHz, 24, 0, 1, 1, 2
};

static void KRKRNS_SWITCHAUDIO_Log(SDL_AudioDevice *device, const char *reason)
{
    SDL_PrivateAudioData *state = device->hidden;
    state->last_diagnostic = SDL_GetTicks();
    KRKRNS_LOG("[switch-audio] %s t=%u mix=%llu submitted=%llu completed=%llu "
               "played-samples=%llu missed-playing=%llu restarts=%llu state=%u,%u",
               reason, state->last_diagnostic,
               (unsigned long long)state->mix_blocks,
               (unsigned long long)state->submitted_blocks,
               (unsigned long long)state->completed_blocks,
               (unsigned long long)state->played_samples,
               (unsigned long long)state->skipped_playing,
               (unsigned long long)state->voice_restarts,
               (unsigned)state->buffer[0].state,
               (unsigned)state->buffer[1].state);
}

static bool KRKRNS_SWITCHAUDIO_Fail(SDL_AudioDevice *device,
                                   const char *operation, Result result)
{
    KRKRNS_LOG("[switch-audio] %s failed result=0x%x", operation,
               (unsigned)result);
    SDL_SetError("%s failed (0x%x)", operation, (unsigned)result);
    SDL_OpenedAudioDeviceDisconnected(device);
    return false;
}

static bool KRKRNS_SWITCHAUDIO_Update(SDL_AudioDevice *device)
{
    SDL_PrivateAudioData *state = device->hidden;
    const Result result = audrvUpdate(&state->driver);
    if (R_FAILED(result))
        return KRKRNS_SWITCHAUDIO_Fail(device, "audrvUpdate", result);

    const Uint32 played = audrvVoiceGetPlayedSampleCount(&state->driver, 0);
    state->played_samples += (Uint32)(played - state->last_played);
    state->last_played = played;
    for (int i = 0; i < 2; ++i) {
        if (!state->in_flight[i]) continue;
        if (state->buffer[i].state == AudioDriverWaveBufState_Playing)
            state->observed_playing[i] = true;
        if (state->buffer[i].state == AudioDriverWaveBufState_Done) {
            state->in_flight[i] = false;
            ++state->completed_blocks;
            if (!state->observed_playing[i]) {
                ++state->skipped_playing;
                // Record the first few transitions, then exponentially, so
                // prolonged heavy rendering cannot flood the runtime log.
                const Uint64 n = state->skipped_playing;
                if (n <= 4 || (n & (n - 1)) == 0)
                    KRKRNS_SWITCHAUDIO_Log(device, "missed-playing recovered");
            }
        }
    }
    if (SDL_GetTicks() - state->last_diagnostic >= 5000)
        KRKRNS_SWITCHAUDIO_Log(device, "heartbeat");
    return true;
}

static int KRKRNS_SWITCHAUDIO_FindReusable(SDL_AudioDevice *device)
{
    for (int i = 0; i < 2; ++i) {
        const AudioDriverWaveBufState state = device->hidden->buffer[i].state;
        if (state == AudioDriverWaveBufState_Free ||
            state == AudioDriverWaveBufState_Done)
            return i;
    }
    return -1;
}

static int KRKRNS_SWITCHAUDIO_WaitReusable(SDL_AudioDevice *device)
{
    while (!SDL_AtomicGet(&device->shutdown) && SDL_AtomicGet(&device->enabled)) {
        if (!KRKRNS_SWITCHAUDIO_Update(device)) return -1;
        const int current = KRKRNS_SWITCHAUDIO_FindReusable(device);
        if (current >= 0) return current;

        // An underrun may have stopped the voice while buffers remain queued.
        // Restart it without throwing away either pending PCM block.
        if (!audrvVoiceIsPlaying(&device->hidden->driver, 0)) {
            audrvVoiceStart(&device->hidden->driver, 0);
            ++device->hidden->voice_restarts;
        }

        // Equivalent to audrenWaitFrame, with a bounded wait so closing the
        // device can still end the SDL thread when the renderer stops firing.
        const Result result = eventWait(audrenGetFrameEvent(), 50000000ULL);
        if (R_FAILED(result) && result != KERNELRESULT(TimedOut)) {
            KRKRNS_SWITCHAUDIO_Fail(device, "audren frame wait", result);
            return -1;
        }
    }
    return -1;
}

static void KRKRNS_SWITCHAUDIO_CloseDevice(SDL_AudioDevice *device)
{
    SDL_PrivateAudioData *state = device->hidden;
    if (!state) return;
    KRKRNS_SWITCHAUDIO_Log(device, "close");
    if (state->renderer_started) audrenStopAudioRenderer();
    if (state->audr_driver) audrvClose(&state->driver);
    if (state->audr_device) audrenExit();
    SDL_free(state->buffer_tmp);
    free(state->pool);
    SDL_free(state);
    device->hidden = nullptr;
}

static int KRKRNS_SWITCHAUDIO_OpenDevice(SDL_AudioDevice *device,
                                        const char *devname)
{
    (void)devname;
    static const u8 sink_channels[] = {0, 1};
    device->hidden = (SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*device->hidden));
    if (!device->hidden) return SDL_OutOfMemory();
    SDL_PrivateAudioData *state = device->hidden;

    Result result = audrenInitialize(&krkrns_ar_config);
    if (R_FAILED(result)) {
        SDL_SetError("audrenInitialize failed (0x%x)", (unsigned)result);
        KRKRNS_SWITCHAUDIO_CloseDevice(device);
        return -1;
    }
    state->audr_device = true;
    result = audrvCreate(&state->driver, &krkrns_ar_config, 2);
    if (R_FAILED(result)) {
        SDL_SetError("audrvCreate failed (0x%x)", (unsigned)result);
        KRKRNS_SWITCHAUDIO_CloseDevice(device);
        return -1;
    }
    state->audr_driver = true;

    device->spec.format = AUDIO_S16SYS;
    SDL_CalculateAudioSpec(&device->spec);
    const size_t size = ((size_t)device->spec.size * 2 + 0xfff) & ~(size_t)0xfff;
    state->pool = memalign(0x1000, size);
    state->buffer_tmp = (Uint8 *)SDL_malloc(device->spec.size);
    if (!state->pool || !state->buffer_tmp) {
        SDL_OutOfMemory();
        KRKRNS_SWITCHAUDIO_CloseDevice(device);
        return -1;
    }
    SDL_memset(state->pool, 0, size);
    for (int i = 0; i < 2; ++i) {
        state->buffer[i].data_raw = state->pool;
        state->buffer[i].size = (u64)device->spec.size * 2;
        state->buffer[i].start_sample_offset = i * device->spec.samples;
        state->buffer[i].end_sample_offset = (i + 1) * device->spec.samples;
    }
    const int pool_id = audrvMemPoolAdd(&state->driver, state->pool, size);
    if (pool_id < 0 || !audrvMemPoolAttach(&state->driver, pool_id) ||
        audrvDeviceSinkAdd(&state->driver, AUDREN_DEFAULT_DEVICE_NAME, 2,
                          sink_channels) < 0 ||
        !audrvVoiceInit(&state->driver, 0, device->spec.channels,
                        PcmFormat_Int16, device->spec.freq)) {
        SDL_SetError("Failed to prepare Switch renderer PCM buffers");
        KRKRNS_SWITCHAUDIO_CloseDevice(device);
        return -1;
    }
    audrvVoiceSetDestinationMix(&state->driver, 0, AUDREN_FINAL_MIX_ID);
    if (device->spec.channels == 1) {
        audrvVoiceSetMixFactor(&state->driver, 0, 1.0f, 0, 0);
        audrvVoiceSetMixFactor(&state->driver, 0, 1.0f, 0, 1);
    } else {
        audrvVoiceSetMixFactor(&state->driver, 0, 1.0f, 0, 0);
        audrvVoiceSetMixFactor(&state->driver, 0, 0.0f, 0, 1);
        audrvVoiceSetMixFactor(&state->driver, 0, 0.0f, 1, 0);
        audrvVoiceSetMixFactor(&state->driver, 0, 1.0f, 1, 1);
    }
    result = audrenStartAudioRenderer();
    if (R_FAILED(result)) {
        SDL_SetError("audrenStartAudioRenderer failed (0x%x)", (unsigned)result);
        KRKRNS_SWITCHAUDIO_CloseDevice(device);
        return -1;
    }
    state->renderer_started = true;
    audrvVoiceStart(&state->driver, 0);
    KRKRNS_LOG("[switch-audio] project audren backend SDL 2.28.5: %u Hz %u ch %u frames",
               (unsigned)device->spec.freq, (unsigned)device->spec.channels,
               (unsigned)device->spec.samples);
    KRKRNS_SWITCHAUDIO_Log(device, "open");
    return 0;
}

static void KRKRNS_SWITCHAUDIO_PlayDevice(SDL_AudioDevice *device)
{
    SDL_PrivateAudioData *state = device->hidden;
    ++state->mix_blocks;
    const int current = KRKRNS_SWITCHAUDIO_WaitReusable(device);
    if (current < 0) return;

    // Only Free/Done buffers may be overwritten. The staging block remains
    // intact while both hardware buffers are still busy.
    Uint8 *ptr = (Uint8 *)state->pool + current * device->spec.size;
    SDL_memcpy(ptr, state->buffer_tmp, device->spec.size);
    armDCacheFlush(ptr, device->spec.size);
    state->observed_playing[current] = false;
    if (!audrvVoiceAddWaveBuf(&state->driver, 0, &state->buffer[current])) {
        KRKRNS_SWITCHAUDIO_Fail(device, "audrvVoiceAddWaveBuf", 0);
        return;
    }
    state->in_flight[current] = true;
    ++state->submitted_blocks;

    // Keep at most two blocks ahead. Done is stable until we reuse the slot;
    // it is safe even if an entire block finished between renderer updates.
    // There is deliberately no wait for a particular slot to become Playing.
    KRKRNS_SWITCHAUDIO_WaitReusable(device);
}

static void KRKRNS_SWITCHAUDIO_WaitDevice(SDL_AudioDevice *) {}
static void KRKRNS_SWITCHAUDIO_ThreadInit(SDL_AudioDevice *) {}
static Uint8 *KRKRNS_SWITCHAUDIO_GetDeviceBuf(SDL_AudioDevice *device)
{
    return device->hidden->buffer_tmp;
}

static SDL_bool KRKRNS_SWITCHAUDIO_Init(SDL_AudioDriverImpl *impl)
{
    impl->OpenDevice = KRKRNS_SWITCHAUDIO_OpenDevice;
    impl->PlayDevice = KRKRNS_SWITCHAUDIO_PlayDevice;
    impl->WaitDevice = KRKRNS_SWITCHAUDIO_WaitDevice;
    impl->GetDeviceBuf = KRKRNS_SWITCHAUDIO_GetDeviceBuf;
    impl->CloseDevice = KRKRNS_SWITCHAUDIO_CloseDevice;
    impl->ThreadInit = KRKRNS_SWITCHAUDIO_ThreadInit;
    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;
    return SDL_TRUE;
}

extern "C" {
AudioBootStrap SWITCHAUDIO_bootstrap = {
    "switch", "Nintendo Switch audio driver (KRKR-ns queue fix)",
    KRKRNS_SWITCHAUDIO_Init, SDL_FALSE
};
}
