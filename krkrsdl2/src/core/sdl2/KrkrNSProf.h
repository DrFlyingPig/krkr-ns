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
/* E-mote frame path (EmotePlayer::progress/draw and the target->layer
 * readback+convert).  On E-mote titles this work sits in the "disp" segment,
 * so without these fields a slow frame cannot be attributed between script,
 * E-mote and composition. */
extern void krkrsdl2_prof_emote_progress(double ms);
extern void krkrsdl2_prof_emote_draw(double ms);
extern void krkrsdl2_prof_emote_readback(double ms);
extern void krkrsdl2_prof_emote_lock(double ms);    // GPU->CPU readback (glReadPixels)
extern void krkrsdl2_prof_emote_convert(double ms); // RGBA->BGRA convert + change compare
extern void krkrsdl2_prof_emote_mesh();
/* One NotifyBitmapCompleted call: the engine's per-layer presentation, inside
 * the compose segment on the CPU path and the glc quad path on the GPU one. */
extern void krkrsdl2_prof_notify(double ms, int blend_type);
/* Continuous-event delivery accounting (KRKR-ns 2026-09-15): how often the
 * engine delivers continuous events, how many TJS closure calls that is, and
 * how many limit-thread ticks fired.  These rates pin down whether a title
 * animating at ~10 fps is the game's own cadence or the engine's pacing. */
extern void krkrsdl2_prof_cont_delivery();
extern void krkrsdl2_prof_cont_call();
extern void krkrsdl2_prof_limit_tick();
/* Window update requests (RequestUpdate -> post) vs actual UpdateContent
 * deliveries.  post >> deliver means the engine coalesces/drops repaint
 * requests — the layer tree asked for 60 fps but the window rendered fewer. */
extern void krkrsdl2_prof_win_update_post();
extern void krkrsdl2_prof_win_update_deliver();
/* E-mote API call rates (progress/draw per second) — splits "the game drives
 * the character at 60 Hz but repaints at 10" from "the game drives it at 10". */
extern void krkrsdl2_prof_emote_prog_call();
extern void krkrsdl2_prof_emote_draw_call();
/* TJS Timer fire accounting: how many timers fire per window and at which
 * requested intervals.  If the game asks for a 16 ms animation timer and the
 * histogram shows it firing far less often, the engine's timer delivery is
 * the cadence problem (rather than the game's own scheduling). */
extern void krkrsdl2_prof_timer_fire(unsigned interval_ms, unsigned pending);
/* One tTVPBaseBitmap::Blt: the layer-tree blend primitive.  Split by whether
 * the destination is the layer manager's compose buffer — that subset is what
 * a GPU-compositing switch would remove from the CPU. */
extern void krkrsdl2_prof_blt(int compose_dest, int method, double ms, unsigned px);
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
static inline void krkrsdl2_prof_emote_progress(double ms) {}
static inline void krkrsdl2_prof_emote_draw(double ms) {}
static inline void krkrsdl2_prof_emote_readback(double ms) {}
static inline void krkrsdl2_prof_emote_lock(double ms) {}
static inline void krkrsdl2_prof_emote_convert(double ms) {}
static inline void krkrsdl2_prof_emote_mesh() {}
static inline void krkrsdl2_prof_notify(double ms, int blend_type) {}
static inline void krkrsdl2_prof_cont_delivery() {}
static inline void krkrsdl2_prof_cont_call() {}
static inline void krkrsdl2_prof_limit_tick() {}
static inline void krkrsdl2_prof_win_update_post() {}
static inline void krkrsdl2_prof_win_update_deliver() {}
static inline void krkrsdl2_prof_emote_prog_call() {}
static inline void krkrsdl2_prof_emote_draw_call() {}
static inline void krkrsdl2_prof_timer_fire(unsigned interval_ms, unsigned pending) {}
static inline void krkrsdl2_prof_blt(int compose_dest, int method, double ms, unsigned px) {}
static inline void krkrsdl2_prof_emit_and_reset(double interval_ms) {}
static inline unsigned krkrsdl2_pool_begins() { return 0; }
static inline unsigned krkrsdl2_pool_bigbegins() { return 0; }
#endif

#endif // KRKRNS_PROF_H