/* SPDX-License-Identifier: MIT */
/* KRKR-ns: shared SD-card debug log (Switch only, debug builds)
 * Single implementation lives in SDLApplication.cpp (krkrsdl2_logf).
 * Uses raw fd writes (no FILE stream, no heap) and a dedicated file name so
 * it never contends with the engine's own UTF-16 file log (which caused heap
 * corruption on the emulator's sdmc driver). */
#ifndef KRKRNS_LOG_H
#define KRKRNS_LOG_H

#ifdef __SWITCH__
extern void krkrsdl2_logf_impl(const char *fmt, ...);
extern void krkrsdl2_set_stage(const char *stage);
extern const char * krkrsdl2_get_stage();
extern void krkrsdl2_heartbeat_main_progress();
#define KRKRNS_LOG(...) krkrsdl2_logf_impl(__VA_ARGS__)
// Publish what the main thread is currently doing; the heartbeat thread
// reports it periodically, so a hang pinpoints its own stage.
#define KRKRNS_STAGE(msg) krkrsdl2_set_stage(msg)
#else
#define KRKRNS_LOG(...)
#define KRKRNS_STAGE(msg)
#endif

#endif // KRKRNS_LOG_H