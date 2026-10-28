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

// A transition handler may render a GPU temporary only while the direct
// window traversal is processing that exact layer. Nested offscreen layer
// completions must not inherit the permission.
inline const void*& GPUWindowTransitionOwnerSlot()
{
    static thread_local const void* owner = nullptr;
    return owner;
}
inline bool& GPUWindowTransitionProcessSlot()
{
    static thread_local bool enabled = false;
    return enabled;
}
class GPUWindowTransitionOwnerScope
{
    const void* previous_;
public:
    explicit GPUWindowTransitionOwnerScope(const void* owner)
        : previous_(GPUWindowTransitionOwnerSlot()) { GPUWindowTransitionOwnerSlot() = owner; }
    ~GPUWindowTransitionOwnerScope() { GPUWindowTransitionOwnerSlot() = previous_; }
    GPUWindowTransitionOwnerScope(const GPUWindowTransitionOwnerScope&) = delete;
    GPUWindowTransitionOwnerScope& operator=(const GPUWindowTransitionOwnerScope&) = delete;
};
class GPUWindowTransitionProcessScope
{
    bool previous_;
public:
    explicit GPUWindowTransitionProcessScope(bool enabled)
        : previous_(GPUWindowTransitionProcessSlot()) { GPUWindowTransitionProcessSlot() = enabled; }
    ~GPUWindowTransitionProcessScope() { GPUWindowTransitionProcessSlot() = previous_; }
    GPUWindowTransitionProcessScope(const GPUWindowTransitionProcessScope&) = delete;
    GPUWindowTransitionProcessScope& operator=(const GPUWindowTransitionProcessScope&) = delete;
};

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
