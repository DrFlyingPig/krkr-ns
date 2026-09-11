/* SPDX-License-Identifier: MIT */
/* KRKR-ns: lightweight per-frame profiling accumulators (Phase 0 baseline).
 *
 * Counters are fed from four places and printed as one [prof] line every N
 * update-frames by the emit site in SDLApplication.cpp (TickBeat):
 *   - main-loop segments         (src/core/environ/sdl2/Application.cpp)
 *   - engine software compose    (external/krkrz/visual/LayerManager.cpp)
 *   - compose -> surface memcpy  (src/core/sdl2/SDLBitmapCompletion.cpp)
 *   - texture upload + present   (src/core/sdl2/SDLApplication.cpp)
 *
 * Off-Switch every call is a static inline no-op, so any translation unit
 * (including the embedded krkrz engine) may call them unconditionally. */
#ifndef KRKRNS_PROF_H
#define KRKRNS_PROF_H

#ifdef __SWITCH__
/* main-loop segment ids, see Application::Run() */
#define KRKRNS_PROF_SEG_EVENTS   0
#define KRKRNS_PROF_SEG_DISPATCH 1
#define KRKRNS_PROF_SEG_TICKBEAT 2
#define KRKRNS_PROF_SEG_WAIT     3

extern void krkrsdl2_prof_seg(int which, double ms);
extern void krkrsdl2_prof_accum_frame();
extern void krkrsdl2_prof_begin_compose();
extern void krkrsdl2_prof_end_compose();
extern void krkrsdl2_prof_accum_surface_copy(double ms);
/* per-layer composition volume (called from NotifyBitmapCompleted): feeds the
 * per-frame "layers/lpxM" fields that decompose the compose segment — helps
 * decide whether a slow animated scene is dominated by the number of layers,
 * the blended pixel volume, or the tree traversal. */
extern void krkrsdl2_prof_accum_layer(unsigned w, unsigned h);
extern void krkrsdl2_prof_accum_upload(double ms, unsigned bytes);
extern void krkrsdl2_prof_accum_present(double ms);
extern void krkrsdl2_prof_emit_and_reset(double interval_ms);
/* pool-driven task batches (ThreadIntf.cpp); deltas per emit window */
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned krkrsdl2_pool_begins();
extern unsigned krkrsdl2_pool_bigbegins();
#ifdef __cplusplus
}
#endif
#else
#define KRKRNS_PROF_SEG_EVENTS   0
#define KRKRNS_PROF_SEG_DISPATCH 1
#define KRKRNS_PROF_SEG_TICKBEAT 2
#define KRKRNS_PROF_SEG_WAIT     3
static inline void krkrsdl2_prof_seg(int which, double ms) {}
static inline void krkrsdl2_prof_accum_frame() {}
static inline void krkrsdl2_prof_begin_compose() {}
static inline void krkrsdl2_prof_end_compose() {}
static inline void krkrsdl2_prof_accum_surface_copy(double ms) {}
static inline void krkrsdl2_prof_accum_layer(unsigned w, unsigned h) {}
static inline void krkrsdl2_prof_accum_upload(double ms, unsigned bytes) {}
static inline void krkrsdl2_prof_accum_present(double ms) {}
static inline void krkrsdl2_prof_emit_and_reset(double interval_ms) {}
static inline unsigned krkrsdl2_pool_begins() { return 0; }
static inline unsigned krkrsdl2_pool_bigbegins() { return 0; }
#endif

#endif // KRKRNS_PROF_H