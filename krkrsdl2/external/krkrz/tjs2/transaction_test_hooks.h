#pragma once
// Proposed test-only API. No implementation exists in current production or
// experimental candidates. Never include it in production engine builds.
#include "tjsInterface.h"
#include <cstddef>
#include <cstdint>

namespace krkr_lazy_transaction_test {
using TJS::iTJSDispatch2;
using TJS::tTJSVariant;
enum class Point { BeforeStage, AfterInsert, BeforeCommit, BeforeStageRelease, CleanupStep };
using Hook = void (*)(Point, iTJSDispatch2 *target, iTJSDispatch2 *stage,
                      std::size_t inserted, void *context);
struct RawState {
    bool pending, expanding, cleanupBlocked;
    std::size_t liveEntries;
    tjs_int count, hashSize, hashMask;
    tjs_uint rebuildMagic;
    std::uintptr_t symbols;
    std::size_t arrayItems;
    std::uintptr_t arrayData;
};

// Arm a single target without allocating from or touching the target itself.
// A hook that throws is a failed transaction; the candidate must dispose the
// stage and report whether a retry is safe. Same-target external reentry must
// be rejected; `ReentryRejections` counts the actual internal guard branch.
bool Arm(iTJSDispatch2 *target, Hook hook, void *context);
void Disarm() noexcept;
std::size_t HookCalls() noexcept;
std::size_t ReentryRejections() noexcept;
// Attempt deferred private-stage cleanup with fault injection disabled. Returns
// true only when cleanup completed and a normal retry is safe. A candidate may
// return false and keep the receiver permanently fail-closed instead.
bool RetryCleanup(iTJSDispatch2 *target);

// RawState/RawMember must not dispatch or materialize anything. They may only
// use the private test build's raw class fields and locked registry lookup.
RawState ReadRawState(iTJSDispatch2 *target);
bool ReadRawMember(iTJSDispatch2 *target, const tjs_char *name, tTJSVariant &out);
// Testing only; inject a legacy native instance on a live pending Array,
// bypassing the new REGISTER barrier. Ownership transfers only on success.
bool InjectPendingArrayNative(iTJSDispatch2 *target, tjs_int32 id, TJS::iTJSNativeInstance *native);
TJS::iTJSNativeInstance *ReadRawNative(iTJSDispatch2 *target, tjs_int32 id);

// Test-only lifetime observer for any exact native target or staging object,
// keyed by a fresh generation, not address alone, to avoid address-reuse errors.
std::uint64_t LifetimeToken(iTJSDispatch2 *target);
std::size_t Destructions(std::uint64_t token) noexcept;
}
