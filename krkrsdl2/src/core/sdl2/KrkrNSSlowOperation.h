/* SPDX-License-Identifier: MIT */
#ifndef KRKR_NS_SLOW_OPERATION_H
#define KRKR_NS_SLOW_OPERATION_H

#ifdef __SWITCH__
#include "KrkrNSLog.h"
#include "tjsString.h"
#include <SDL_timer.h>

// Log individual stalls, which disappear inside a 60-frame average. Names are
// borrowed from the enclosing call and converted only when a call is slow.
class KrkrNSSlowOperation
{
    const char *Kind;
    const TJS::tTJSString *Name;
    Uint64 Start;
public:
    explicit KrkrNSSlowOperation(const char *kind, const TJS::tTJSString *name = nullptr)
        : Kind(kind), Name(name), Start(SDL_GetPerformanceCounter()) {}
    ~KrkrNSSlowOperation() noexcept
    {
        const double ms = (SDL_GetPerformanceCounter() - Start) * 1000.0 / SDL_GetPerformanceFrequency();
        if (ms < 16.0) return;
        try
        {
            KRKRNS_LOG("[slow] %s %.2fms %s", Kind, ms,
                Name ? Name->AsNarrowStdString().c_str() : "");
        }
        catch (...) {} // Diagnostics must not change script/stream error handling.
    }
};
#else
class KrkrNSSlowOperation
{
public:
    explicit KrkrNSSlowOperation(const char *, const void * = nullptr) {}
};
#endif

#endif
