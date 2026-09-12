#include "TVPCompositor.h"
#include "EmoteSWRenderBackend.h"
#include "EmoteGLRenderBackend.h"
#include "KrkrNSLog.h"

extern SDL_Renderer* TVPGetPrimarySDLRenderer();

namespace krkrsdl3
{
iTVPRenderBackend* TVPGetRenderBackend()
{
    static EmoteGLRenderBackend gpu;
    static EmoteSWRenderBackend cpu;
    static iTVPRenderBackend* selected = nullptr;
    if (!selected)
    {
        // Backend preference (KRKR-ns Phase 2): the isolated-GL backend was
        // verified on the real Switch (2026-09-07 — correct sprites, mesh
        // raster moved off the CPU), and IsAvailable() now self-tests the
        // driver's full-target rasterization so the Nextendo emulator's
        // software GLES (which clamps drawn triangles to a quarter of the
        // FBO) is detected and rejected automatically. Override markers:
        //   sdmc:/switch/krkrsdl2/emote-cpu.txt  force the CPU backend
        //   sdmc:/switch/krkrsdl2/emote-gl.txt   force the GL backend
        bool wantGl = true;
#ifdef __SWITCH__
        if (FILE* f = fopen("sdmc:/switch/krkrsdl2/emote-gl.txt", "rb"))
        {
            fclose(f);
            wantGl = true;
        }
        if (FILE* f = fopen("sdmc:/switch/krkrsdl2/emote-cpu.txt", "rb"))
        {
            fclose(f);
            wantGl = false;
        }
#else
        wantGl = false; // other platforms keep the old opt-in behavior
#endif
        if (wantGl)
        {
            // The GL backend builds its private context against the primary
            // SDL renderer/window.  The FIRST E-mote call of a process can
            // land inside the launcher->game window transition, before that
            // exists; failing there used to latch the CPU backend for the
            // WHOLE process, and on the emulator the CPU path is 2-3x slower
            // on 1080p E-mote content -- the second game's opening animation
            // then read as "very laggy" while the same game booted directly
            // was smooth.  An absent primary renderer is a TEMPORARY
            // condition: serve the CPU backend for this call and leave the
            // selection open so a later call retries GL.  A genuine self-test
            // failure (renderer present, test still fails) latches the CPU
            // backend exactly as before.
            if (!TVPGetPrimarySDLRenderer())
            {
                KRKRNS_LOG("[emote] GL not ready (no primary renderer yet); serving CPU, will retry GL");
                return &cpu;
            }
            if (gpu.IsAvailable())
            {
                selected = &gpu;
            }
            else
            {
                selected = &cpu;
                KRKRNS_LOG("[emote] GL self-test failed; CPU backend latched");
            }
        }
        else
        {
            selected = &cpu;
        }
        KRKRNS_LOG("[emote] selected %s backend", selected == &gpu ? "isolated GL" : "CPU fallback");
    }
    return selected;
}

// KRKR-ns: tell the selected backend that the SDL renderer it cached is gone,
// because the host is rebuilding the engine in-process.
//
// The CPU backend normally notices a NEW renderer pointer by itself, but after
// an engine restart the allocator frequently hands the replacement renderer the
// same address -- the comparison then cannot see the change and stale GPU
// handles (targets, textures, scratch) are reused against a renderer that no
// longer exists.  That shows up as a title screen whose UI draws while the
// background and E-mote logo animation stay empty, and as a freeze on a later
// restart.  This reset is unconditional.
void TVPResetRenderBackendForEngineRestart()
{
    if (iTVPRenderBackend* backend = TVPGetRenderBackend())
        backend->ResetForEngineRestart();
}
} // namespace krkrsdl3

