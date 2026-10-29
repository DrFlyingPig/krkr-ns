/* SPDX-License-Identifier: MIT */
#pragma once
#include "SDLBitmapCompositor.h"

namespace krkrns
{
inline bool& GPUCompletionReceiverSlot()
{
    static thread_local bool supported = false;
    return supported;
}

class BitmapCompletionReceiverScope
{
    bool previous_;
public:
    BitmapCompletionReceiverScope() : previous_(GPUCompletionReceiverSlot())
        { GPUCompletionReceiverSlot() = false; }
    ~BitmapCompletionReceiverScope() { GPUCompletionReceiverSlot() = previous_; }
};

inline const BitmapView*& CompletionBitmapSlot()
{
    static thread_local const BitmapView* bitmap = nullptr;
    return bitmap;
}

// Only valid during the synchronous manager -> drawdevice notification.
// The consumer obtains a ResidentFrame, never keeps this borrowed CPU view.
class CompletionBitmapScope
{
    const BitmapView* previous_;
public:
    explicit CompletionBitmapScope(const BitmapView& bitmap)
        : previous_(CompletionBitmapSlot()) { CompletionBitmapSlot() = &bitmap; }
    explicit CompletionBitmapScope(const BitmapView* bitmap)
        : previous_(CompletionBitmapSlot()) { CompletionBitmapSlot() = bitmap; }
    ~CompletionBitmapScope() { CompletionBitmapSlot() = previous_; }
    CompletionBitmapScope(const CompletionBitmapScope&) = delete;
    CompletionBitmapScope& operator=(const CompletionBitmapScope&) = delete;
};
}
