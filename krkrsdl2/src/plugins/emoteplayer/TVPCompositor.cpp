#include "TVPCompositor.h"
#include "EmoteSWRenderBackend.h"
#include "EmoteGLRenderBackend.h"
#include "KrkrNSLog.h"

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
        if (wantGl && gpu.IsAvailable())
        {
            selected = &gpu;
        }
        else
        {
            selected = &cpu;
        }
        KRKRNS_LOG("[emote] selected %s backend", selected == &gpu ? "isolated GL" : "CPU fallback");
    }
    return selected;
}
} // namespace krkrsdl3

