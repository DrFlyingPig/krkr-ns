/* SPDX-License-Identifier: MIT */
/* KRKR-ns Phase 3: GPU layer-composite bridge (engine side).
 *
 * The whole KiriKiri layer tree finally reaches the screen through
 * tTVPBaseBitmap::Blt / CopyRect onto the layer manager's DrawBuffer.
 * These hooks route that last composite onto a GL path when the module
 * is enabled (sdmc:/switch/krkrsdl2/gpu-composite.txt + driver probe).
 *
 * Off-Switch (or when the module is off) every call is a cheap no-op, so
 * the embedded krkrz engine can call them unconditionally. */
#ifndef KRKRNS_GLCOMPOSITE_H
#define KRKRNS_GLCOMPOSITE_H

#include "tjsTypes.h"
#include "ComplexRect.h"

#ifdef __SWITCH__
extern bool krkrsdl2_glc_enabled();
/* registered by tTVPLayerManager::GetDrawTargetBitmap (DrawBuffer) */
extern void krkrsdl2_glc_set_compose_target(void* bitmap);
/* frame bracketing inside UpdateToDrawDevice (before/after CompleteForWindow) */
extern void krkrsdl2_glc_begin_frame();
extern void krkrsdl2_glc_end_frame();
/* Blt / CopyRect / StretchBlt / Fill interception; return true if handled */
extern bool krkrsdl2_glc_try_blt(void* dest, tjs_int x, tjs_int y,
                                 const void* src, const tTVPRect* srcRect,
                                 tjs_int method, tjs_int opa, bool hda);
extern bool krkrsdl2_glc_try_copy(void* dest, tjs_int x, tjs_int y,
                                  const void* src, const tTVPRect* srcRect);
/* copy the composed GL texture into the (top-down, 32bpp) compose surface;
 * returns true when the frame was composed on the GPU */
extern bool krkrsdl2_glc_readback(void* surface, int w, int h, int pitch);
/* true while the current frame was composed entirely on the GPU */
extern bool krkrsdl2_glc_pure_frame();
/* true in mode 5 (gpu-composite-gpuonly.txt): the CPU-side layer
 * composition is skipped — the GPU quads are the only frame source */
extern bool krkrsdl2_glc_gpuonly();
/* pixel-version bump for tracked source bitmaps */
extern void krkrsdl2_glc_bump_version(const void* bitmap);
/* engine-side raster accessor (implemented in LayerBitmapIntf.cpp where the
 * full bitmap type exists); returns row-0 pixels or nullptr */
extern const void* krkrsdl2_glc_get_bitmap_raster(const void* bitmap,
                                                  int* w, int* h, int* pitch,
                                                  int* bpp);
/* per-layer composite from BasicDrawDevice::NotifyBitmapCompleted (v2.6+).
 * bits = layer pixel base, bpp = 32, pitch in bytes; bottomup mirrors
 * bitmapinfo height sign (positive = bottom-up memory). The layer is drawn
 * onto the full-screen compose FBO using its cliprect as the source and
 * (x,y) as the destination, with the layer's type/opacity. */
extern void krkrsdl2_glc_layer(tjs_int x, tjs_int y,
                               const void* bits, tjs_int w, tjs_int h,
                               tjs_int pitch,
                               const tTVPRect& cliprect,
                               tjs_int type, tjs_int opacity,
                               bool bottomup);
#else
static inline bool krkrsdl2_glc_enabled() { return false; }
static inline void krkrsdl2_glc_set_compose_target(void*) {}
static inline void krkrsdl2_glc_begin_frame() {}
static inline void krkrsdl2_glc_end_frame() {}
static inline bool krkrsdl2_glc_try_blt(void*, tjs_int, tjs_int, const void*,
                                        const tTVPRect*, tjs_int, tjs_int, bool) { return false; }
static inline bool krkrsdl2_glc_try_copy(void*, tjs_int, tjs_int, const void*,
                                         const tTVPRect*) { return false; }
static inline bool krkrsdl2_glc_readback(void*, int, int, int) { return false; }
static inline bool krkrsdl2_glc_pure_frame() { return false; }
static inline bool krkrsdl2_glc_gpuonly() { return false; }
static inline void krkrsdl2_glc_bump_version(const void*) {}
static inline const void* krkrsdl2_glc_get_bitmap_raster(const void*, int*, int*, int*, int*) { return nullptr; }
static inline void krkrsdl2_glc_layer(tjs_int, tjs_int, const void*, tjs_int,
                                      tjs_int, tjs_int, const tTVPRect&,
                                      tjs_int, tjs_int, bool) {}
#endif

#endif // KRKRNS_GLCOMPOSITE_H