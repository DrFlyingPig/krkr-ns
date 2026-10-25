/* SPDX-License-Identifier: MIT */
/* KRKR-ns: shared SD-card diagnostic log (Switch only)
 * Single implementation lives in SDLApplication.cpp (krkrsdl2_logf).
 * Uses raw fd writes and bounded per-thread report buffers with a dedicated file name so
 * it never contends with the engine's own UTF-16 file log (which caused heap
 * corruption on the emulator's sdmc driver). */
#ifndef KRKRNS_LOG_H
#define KRKRNS_LOG_H

#ifdef __SWITCH__
extern void krkrsdl2_logf_impl(const char *fmt, ...);
extern void krkrsdl2_logf_deferred_impl(const char *fmt, ...);
#ifdef KRKRNS_GAME_LOG_BATCH
extern void krkrsdl2_logf_game_impl(const char *fmt, ...);
extern void krkrsdl2_log_poll();
#define KRKRNS_LOG_GAME(...) krkrsdl2_logf_game_impl(__VA_ARGS__)
#else
#define KRKRNS_LOG_GAME(...) krkrsdl2_logf_impl(__VA_ARGS__)
#endif
extern void krkrsdl2_log_batch_begin();
extern void krkrsdl2_log_batch_end();
extern void krkrsdl2_log_flush();
extern void krkrsdl2_set_stage(const char *stage);
extern const char * krkrsdl2_get_stage();
extern void krkrsdl2_heartbeat_main_progress();
#define KRKRNS_LOG(...) krkrsdl2_logf_impl(__VA_ARGS__)
#define KRKRNS_LOG_DEFERRED(...) krkrsdl2_logf_deferred_impl(__VA_ARGS__)
class KrkrNSLogBatch
{
public:
    KrkrNSLogBatch() { krkrsdl2_log_batch_begin(); }
    ~KrkrNSLogBatch() { krkrsdl2_log_batch_end(); }
    KrkrNSLogBatch(const KrkrNSLogBatch&) = delete;
    KrkrNSLogBatch& operator=(const KrkrNSLogBatch&) = delete;
};
// Publish what the main thread is currently doing; the heartbeat thread
// reports it periodically, so a hang pinpoints its own stage.
#define KRKRNS_STAGE(msg) krkrsdl2_set_stage(msg)
#else
#define KRKRNS_LOG(...)
#define KRKRNS_LOG_DEFERRED(...)
#define KRKRNS_LOG_GAME(...)
#define KRKRNS_STAGE(msg)
class KrkrNSLogBatch {};
#endif

#endif // KRKRNS_LOG_H
