#include "KrkrNSPaths.h"
#include "ncbind/ncbind.hpp"
#include "emoteplayerclass.h"
#include "EmotePlaybackCompletion.h"
#include "tjsArray.h"
#include "StorageIntf.h"
#include "CharacterSet.h"
#include "Platform.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <set>
#ifdef __SWITCH__
#include <arm_neon.h>
#endif

#include "tjsCommHead.h"
#include "LayerIntf.h"
#include "EventIntf.h"
#include "KrkrNSProf.h"
#include "KrkrNSPerf.h"
#include <SDL.h>
#if defined(__SWITCH__) && defined(KRKRNS_EMOTE_RESIDENT_READBACK)
#include "EmoteGLRenderBackend.h"
#include "SDLBitmapCompositor.h"
#endif

// Set while the engine tears a session down (SDLApplication.cpp): the window and
// its GL context are already gone, so calling into a GPU backend there hangs.
extern volatile bool krkrsdl2_engine_teardown_active;

namespace emoteplayer
{
// Scoped per-call timer for the [prof] frame attribution (KrkrNSProf.h).
// E-mote progress/draw run inside the KAG continuous handler, so their cost
// previously hid inside the "disp" segment with no way to separate it from
// script execution.
struct EmoteProfTimer
{
    Uint64 start;
    void (*sink)(double);
    explicit EmoteProfTimer(void (*s)(double))
        : start(SDL_GetPerformanceCounter()), sink(s) {}
    ~EmoteProfTimer()
    {
        const double ms = (double)(SDL_GetPerformanceCounter() - start) * 1000.0 /
            (double)SDL_GetPerformanceFrequency();
        sink(ms);
    }
};

static std::string EmoteToUtf8(const tTJSString& value)
{
    std::string result;
    TVPUtf16ToUtf8(result, value.AsStdString());
    return result;
}

// KRKR-ns (2026-11-04): reuse E-mote render targets instead of churning malloc.//
// A 1920x1440 E-mote target is an 11 MB allocation and this game creates and
// destroys whole E-mote players constantly (428 targets in a 9-minute session).
// Returning those to malloc and asking for them again fragments the heap until
// the arena sits at the process ceiling (3.3 GB with under 1 GB live) and an
// unrelated allocation -- the pre-rendered font atlas, in the crash that
// prompted this -- fails with std::bad_alloc.
//
// Keep the most recently released targets per size and hand them back out.  The
// pool is bounded, flushed whenever the render backend is rebuilt (engine
// restart) and never mixes targets from two different renderers.
namespace
{
struct EmotePooledTarget
{
    krkrsdl3::iTVPRenderBackend* renderer;
    void* target;
    tjs_int width;
    tjs_int height;
};
const size_t EMOTE_TARGET_POOL_LIMIT = 6; // ~66 MB of 1920x1440 targets
std::vector<EmotePooledTarget> _targetPool;

void EmoteDiscardTarget(const EmotePooledTarget& entry)
{
    if (!entry.renderer || !entry.target) return;
    // TVPGetRenderBackend() serves the CPU backend while teardown is active, so
    // a mismatch identifies targets whose GPU context has been destroyed.
    krkrsdl3::iTVPRenderBackend* live = krkrsdl3::TVPGetRenderBackend();
    if (krkrsdl2_engine_teardown_active && entry.renderer != live) return;
    entry.renderer->DestroyTarget(entry.target);
}

void EmoteFlushTargetPool(const char* reason)
{
    if (_targetPool.empty()) return;
    TVPConsoleLog("emote: dropping %u pooled render target(s) for %s",
                  (unsigned)_targetPool.size(), reason);
    for (size_t i = 0; i < _targetPool.size(); ++i)
        EmoteDiscardTarget(_targetPool[i]);
    _targetPool.clear();
}

void* EmoteTakePooledTarget(krkrsdl3::iTVPRenderBackend* renderer,
                            tjs_int width, tjs_int height)
{
    for (size_t i = 0; i < _targetPool.size(); ++i)
    {
        EmotePooledTarget& entry = _targetPool[i];
        if (entry.renderer != renderer) continue;
        if (entry.width != width || entry.height != height) continue;
        void* target = entry.target;
        _targetPool.erase(_targetPool.begin() + i);
        return target;
    }
    return nullptr;
}

void EmoteReturnPooledTarget(krkrsdl3::iTVPRenderBackend* renderer,
                             void* target, tjs_int width, tjs_int height)
{
    if (!renderer || !target || width <= 0 || height <= 0) return;
    if (!_targetPool.empty() && _targetPool.front().renderer != renderer)
        EmoteFlushTargetPool("renderer change");
    if (_targetPool.size() >= EMOTE_TARGET_POOL_LIMIT)
    {
        EmoteDiscardTarget(_targetPool.front());
        _targetPool.erase(_targetPool.begin());
    }
    EmotePooledTarget entry;
    entry.renderer = renderer;
    entry.target = target;
    entry.width = width;
    entry.height = height;
    _targetPool.push_back(entry);
}

// Allocate a render target, reusing a pooled one and surviving allocation
// failure: collect once and retry, then let the caller skip the frame instead of
// letting std::bad_alloc become a fatal script error.
void* CreateRenderTargetSafe(krkrsdl3::iTVPRenderBackend* renderer,
                             tjs_int width, tjs_int height)
{
    if (!renderer || width <= 0 || height <= 0) return nullptr;
    if (void* pooled = EmoteTakePooledTarget(renderer, width, height)) return pooled;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        try
        {
            void* target = renderer->CreateTarget(width, height);
            if (target) return target;
        }
        catch (const std::bad_alloc&)
        {
        }
        catch (...)
        {
        }
        if (attempt != 0) break;
        TVPConsoleLog("[emote] render target %dx%d allocation failed; compacting and retrying",
                      width, height);
        TVPDeliverCompactEvent(TVP_COMPACT_LEVEL_CRITICAL);
        if (void* pooled = EmoteTakePooledTarget(renderer, width, height)) return pooled;
    }
    TVPConsoleLog("[emote] render target %dx%d unavailable; skipping this draw", width, height);
    return nullptr;
}

// Give a target back for reuse (when the renderer still owns the pointer).
void DestroyRenderTargetSafe(krkrsdl3::iTVPRenderBackend* renderer,
                             void* target, tjs_int width, tjs_int height)
{
    if (!target) return;
    if (!renderer)
    {
        // No owner recorded: releasing during teardown would hit a backend whose
        // context is gone.  Leaving it to that backend's reset is the safe exit.
        if (krkrsdl2_engine_teardown_active) return;
        if (krkrsdl3::iTVPRenderBackend* current = krkrsdl3::TVPGetRenderBackend())
            current->DestroyTarget(target);
        return;
    }
    EmoteReturnPooledTarget(renderer, target, width, height);
}
} // namespace

static ttstr EmoteFromUtf8(const std::string& value)
{
    tjs_string result;
    if (!TVPUtf8ToUtf16(result, value))
        return ttstr();
    return ttstr(result);
}

#if defined(__SWITCH__) && defined(KRKRNS_EMOTE_RESIDENT_READBACK)
static krkrsdl3::EmoteLayerIdentity EmoteLayerIdentityForView(
    const krkrns::BitmapView& view, int width, int height)
{
    if (!view.storage || !view.row0 || !view.serial || !view.version ||
        view.volatileCPU || view.sourceHandle || view.bpp != 32 ||
        width <= 0 || height <= 0 || width > view.width || height > view.height ||
        (view.gpuWriteTracked && view.gpuWriteTracked->load(std::memory_order_acquire)))
        return {};
    return {view.storage, view.row0, view.serial, view.version, width, height, view.pitch};
}

static void RecordEmoteLayerDamage(bool partial, bool unchanged,
                                   std::uint64_t scanned, std::uint64_t full)
{
    static std::uint64_t frames = 0, partialFrames = 0, unchangedFrames = 0;
    static std::uint64_t scannedPixels = 0, fullPixels = 0;
    ++frames;
    partialFrames += partial;
    unchangedFrames += unchanged;
    scannedPixels += scanned;
    fullPixels += full;
    if (frames == 1 || frames % 120 == 0)
        KRKRNS_LOG("[emote-layer-damage] frames=%llu partial=%llu unchanged=%llu pixels=%llu fullPixels=%llu",
            (unsigned long long)frames, (unsigned long long)partialFrames,
            (unsigned long long)unchangedFrames, (unsigned long long)scannedPixels,
            (unsigned long long)fullPixels);
}
#endif

// A Kirikiri layer bitmap is commonly bottom-up: scan line 0 points at the
// logical top row while GetMainImagePixelBufferPitch() is negative.  Copying
// the whole render target forward from that pointer corrupts the guard area
// preceding the bitmap.  Always copy logical rows using both signed pitches.
static bool CopyRenderTargetToLayer(krkrsdl3::iTVPRenderBackend* renderer,
                                    void* target,
                                    tTJSNI_BaseLayer* layer,
                                    int sourceWidth,
                                    int sourceHeight,
                                    tTVPRect* dirtyRect)
{
    KrkrNSPerfScope perf(KrkrNSPerfOp::EmoteLayerBridge);
    EmoteProfTimer profTimer(krkrsdl2_prof_emote_readback);
    (void)profTimer;
    if (dirtyRect)
        dirtyRect->clear();
    if (!renderer || !target || !layer || sourceWidth <= 0 || sourceHeight <= 0)
    {
        // KRKR-ns diagnostic: the E-mote frame is copied onto the KAG layer
        // here, so a title screen that loads its .mtn correctly yet stays empty
        // is explained by this early-out.  Logged (bounded) because the reason
        // matters: a missing renderer/target means the backend produced
        // nothing, whereas a missing layer means the copy target is gone.
        static int logged = 0;
        if (logged < 20)
        {
            ++logged;
            KRKRNS_LOG("[emote] draw: cannot copy to layer (renderer=%p target=%p "
                       "layer=%p src=%dx%d)", (void*)renderer, target, (void*)layer,
                       sourceWidth, sourceHeight);
        }
        return false;
    }

    // Capture the destination generation before the transient writer advances
    // it or detaches a shared bitmap. The GL target owns the matching record.
#if defined(__SWITCH__) && defined(KRKRNS_EMOTE_RESIDENT_READBACK)
    auto* damageBackend = dynamic_cast<krkrsdl3::EmoteGLRenderBackend*>(renderer);
    auto* layerBitmap = damageBackend ? layer->GetMainImage() : nullptr;
    const auto beforeView = layerBitmap ? layerBitmap->GetBitmap()->GetGPUView()
                                        : krkrns::BitmapView{};
#endif
    int sourcePitch = 0;
    uint8_t* source = renderer->LockTarget(target, sourcePitch);
#if defined(__SWITCH__) && defined(KRKRNS_EMOTE_RESIDENT_READBACK)
    const int predictedWidth = std::min(sourceWidth, static_cast<int>(layer->GetImageWidth()));
    const int predictedHeight = std::min(sourceHeight, static_cast<int>(layer->GetImageHeight()));
    const auto previousIdentity = EmoteLayerIdentityForView(beforeView, predictedWidth, predictedHeight);
    krkrsdl3::EmoteLayerBounds damage{};
    bool partial = damageBackend && previousIdentity &&
        damageBackend->GetLayerDamage(target, previousIdentity, damage);
    int scanLeft = 0, scanTop = 0, scanRight = predictedWidth, scanBottom = predictedHeight;
    if (partial)
    {
        scanLeft = std::clamp(damage.left, 0, predictedWidth);
        scanTop = std::clamp(damage.top, 0, predictedHeight);
        scanRight = std::clamp(damage.right, scanLeft, predictedWidth);
        scanBottom = std::clamp(damage.bottom, scanTop, predictedHeight);
    }
    // No source pixel in this Layer changed. Keep the observable modification
    // flag, but do not manufacture a new CPU generation or force a GPU upload.
    const std::ptrdiff_t predictedBytes = std::ptrdiff_t(predictedWidth) * 4;
    if (partial && (scanLeft == scanRight || scanTop == scanBottom) && source &&
        (std::ptrdiff_t(sourcePitch) >= predictedBytes || std::ptrdiff_t(sourcePitch) <= -predictedBytes) &&
        (std::ptrdiff_t(beforeView.pitch) >= predictedBytes || std::ptrdiff_t(beforeView.pitch) <= -predictedBytes) &&
        layerBitmap->GetBitmap()->IsIndependent())
    {
        layer->SetImageModified(true);
        RecordEmoteLayerDamage(true, true, 0,
            std::uint64_t(predictedWidth) * predictedHeight);
        damageBackend->CommitLayerDamage(target, previousIdentity);
        renderer->UnlockTarget(target);
        return true;
    }
#endif
    uint8_t* destination =
        static_cast<uint8_t*>(layer->GetMainImagePixelBufferForTransientWrite());
    const int destinationPitch = layer->GetMainImagePixelBufferPitch();
    const int copyWidth = std::min(sourceWidth, static_cast<int>(layer->GetImageWidth()));
    const int copyHeight = std::min(sourceHeight, static_cast<int>(layer->GetImageHeight()));
    const std::size_t rowBytes = copyWidth > 0
        ? static_cast<std::size_t>(copyWidth) * 4u
        : 0u;
    const std::ptrdiff_t sourceStride = sourcePitch;
    const std::ptrdiff_t destinationStride = destinationPitch;
    const bool sourcePitchValid =
        sourceStride >= static_cast<std::ptrdiff_t>(rowBytes) ||
        sourceStride <= -static_cast<std::ptrdiff_t>(rowBytes);
    const bool destinationPitchValid =
        destinationStride >= static_cast<std::ptrdiff_t>(rowBytes) ||
        destinationStride <= -static_cast<std::ptrdiff_t>(rowBytes);
    const bool canCopy = source && destination && copyHeight > 0 && rowBytes > 0 &&
                         sourcePitchValid && destinationPitchValid;

    static bool loggedLayout = false;
    static bool loggedFailure = false;
    static int lastLoggedW = -1, lastLoggedH = -1;
    if (canCopy)
    {
#if !defined(__SWITCH__) || !defined(KRKRNS_EMOTE_RESIDENT_READBACK)
        const int scanLeft = 0, scanTop = 0, scanRight = copyWidth, scanBottom = copyHeight;
#else
        // If layout unexpectedly changed while obtaining the writer, do a
        // complete compare rather than trusting the earlier clipped bounds.
        if (copyWidth != predictedWidth || copyHeight != predictedHeight)
        {
            partial = false;
            scanLeft = scanTop = 0;
            scanRight = copyWidth;
            scanBottom = copyHeight;
        }
        if (damageBackend)
            RecordEmoteLayerDamage(partial, false,
                std::uint64_t(scanRight - scanLeft) * (scanBottom - scanTop),
                std::uint64_t(copyWidth) * copyHeight);
#endif
        if (!loggedLayout || sourceWidth != lastLoggedW || sourceHeight != lastLoggedH)
        {
            TVPConsoleLog("target-to-layer copy src=%dx%d pitch=%d dst=%ux%u pitch=%d copy=%dx%d",
                          sourceWidth, sourceHeight, sourcePitch,
                          layer->GetImageWidth(), layer->GetImageHeight(), destinationPitch,
                          copyWidth, copyHeight);
            loggedLayout = true;
            lastLoggedW = sourceWidth;
            lastLoggedH = sourceHeight;
        }
        #if defined(__SWITCH__) && defined(KRKRNS_EMOTE_CAPTURE_DIAGNOSTICS)
        static int dumpCount = 0;
        if (dumpCount < 5)
        {
            dumpCount++;
            SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormatFrom(
                source, sourceWidth, sourceHeight, 32, sourcePitch, SDL_PIXELFORMAT_ABGR8888);
            if (shot)
            {
                char path[128];
                snprintf(path, sizeof(path), KRKRNS_BASE_A "/emote-src-%d.bmp", dumpCount);
                SDL_SaveBMP(shot, path);
                SDL_FreeSurface(shot);
            }
        }
        #endif
        int dirtyLeft = copyWidth;
        int dirtyTop = copyHeight;
        int dirtyRight = 0;
        int dirtyBottom = 0;
        const Uint64 cvtStart = SDL_GetPerformanceCounter();
        {
            KrkrNSPerfScope perfConvert(KrkrNSPerfOp::EmoteConvert);
#if defined(__SWITCH__) && defined(KRKRNS_EMOTE_RESIDENT_READBACK)
            perfConvert.Bytes(std::uint64_t(scanRight - scanLeft) * (scanBottom - scanTop) * 4);
#endif
            for (int y = scanTop; y < scanBottom; ++y)
            {
                uint8_t* destinationRow =
                    destination + static_cast<std::ptrdiff_t>(y) * destinationStride;
                const uint8_t* sourceRow =
                    source + static_cast<std::ptrdiff_t>(y) * sourceStride;
                // The E-mote backend is RGBA, while a 32-bpp Kirikiri Layer is
                // BGRA in memory.  Preserve G/A and exchange R/B at the boundary.
                // Compare against the previous layer at the same time so Kirikiri
                // only recomposes the pixels that actually changed.  This also
                // catches transparent pixels left behind when a sprite moves.
                int rowLeft = copyWidth;
                int rowRight = 0;
                int x = scanLeft;
#ifdef __SWITCH__
                // Convert four RGBA pixels to Kirikiri BGRA at once.  The prior
                // memcpy-per-pixel loop was measurable after GPU rendering became
                // fast, especially when several full-stage E-mote layers coexist.
                static const uint8_t swizzleBytes[16] = {
                    2, 1, 0, 3, 6, 5, 4, 7,
                    10, 9, 8, 11, 14, 13, 12, 15};
                const uint8x16_t swizzle = vld1q_u8(swizzleBytes);
                uint32x4_t lastChanged = vdupq_n_u32(0);
                int lastChangedX = -1;
                for (; x + 4 <= scanRight; x += 4)
                {
                    const uint8x16_t rgba = vld1q_u8(sourceRow + static_cast<size_t>(x) * 4u);
                    const uint8x16_t bgra = vqtbl1q_u8(rgba, swizzle);
                    const uint8x16_t old = vld1q_u8(destinationRow + static_cast<size_t>(x) * 4u);
                    const uint32x4_t changed = vmvnq_u32(vceqq_u32(
                        vreinterpretq_u32_u8(old), vreinterpretq_u32_u8(bgra)));
                    if (vmaxvq_u32(changed) != 0)
                    {
                        if (rowLeft == copyWidth)
                            rowLeft = x + (vgetq_lane_u32(changed, 0) ? 0 :
                                           vgetq_lane_u32(changed, 1) ? 1 :
                                           vgetq_lane_u32(changed, 2) ? 2 : 3);
                        rowRight = x + 4;
                        lastChangedX = x;
                        lastChanged = changed;
                        // Equal groups need no write. Determine the precise right
                        // edge once per row rather than extracting four lanes for
                        // every converted group; transparent erasure still counts.
                        vst1q_u8(destinationRow + static_cast<size_t>(x) * 4u, bgra);
                    }
                }
                if (lastChangedX >= 0)
                    rowRight = lastChangedX + (vgetq_lane_u32(lastChanged, 3) ? 4 :
                                              vgetq_lane_u32(lastChanged, 2) ? 3 :
                                              vgetq_lane_u32(lastChanged, 1) ? 2 : 1);
#endif
                auto* destinationPixels = reinterpret_cast<std::uint32_t*>(destinationRow);
                const auto* sourcePixels = reinterpret_cast<const std::uint32_t*>(sourceRow);
                for (; x < scanRight; ++x)
                {
                    const std::uint32_t sourcePixel = sourcePixels[x];
                    const std::uint32_t convertedPixel =
                        (sourcePixel & 0xff00ff00u) |
                        ((sourcePixel & 0x000000ffu) << 16) |
                        ((sourcePixel & 0x00ff0000u) >> 16);
                    if (destinationPixels[x] != convertedPixel)
                    {
                        destinationPixels[x] = convertedPixel;
                        if (rowLeft == copyWidth)
                            rowLeft = x;
                        rowRight = x + 1;
                    }
                }
                if (rowLeft < rowRight)
                {
                    dirtyLeft = std::min(dirtyLeft, rowLeft);
                    dirtyTop = std::min(dirtyTop, y);
                    dirtyRight = std::max(dirtyRight, rowRight);
                    dirtyBottom = y + 1;
                }
            }
            if (dirtyRect && dirtyLeft < dirtyRight && dirtyTop < dirtyBottom)
                *dirtyRect = tTVPRect(dirtyLeft, dirtyTop, dirtyRight, dirtyBottom);
        }
        krkrsdl2_prof_emote_convert(
            (double)(SDL_GetPerformanceCounter() - cvtStart) * 1000.0 /
            (double)SDL_GetPerformanceFrequency());
#if defined(__SWITCH__) && defined(KRKRNS_EMOTE_RESIDENT_READBACK)
        if (damageBackend && layerBitmap)
        {
            const auto afterView = layerBitmap->GetBitmap()->GetGPUView();
            auto identity = EmoteLayerIdentityForView(afterView, copyWidth, copyHeight);
            if (afterView.row0 != destination || afterView.pitch != destinationPitch)
                identity = {};
            damageBackend->CommitLayerDamage(target, identity);
        }
#endif
    }
    else if (!loggedFailure)
    {
        TVPConsoleLog("target-to-layer copy rejected src=%p %dx%d pitch=%d dst=%p %ux%u pitch=%d",
                      source, sourceWidth, sourceHeight, sourcePitch, destination,
                      layer->GetImageWidth(), layer->GetImageHeight(), destinationPitch);
        loggedFailure = true;
    }

    renderer->UnlockTarget(target);
    return canCopy;
}

iTJSDispatch2* ResourceManager::_kagWindow = nullptr;
static SeparateLayerAdaptor* _motionWorkLayer = nullptr;

// KRKR-ns (2026-11-04): hand parsed motion files from a dead ResourceManager to
// the next one that asks for the same path.
//
// Several KAG E-mote integrations (the Yuzusoft titles among them) build a NEW
// ResourceManager for every motion they show, so one instance parses one .mtn
// and dies with it.  The per-instance cache can never hit: an 11-hour session
// logged 109 ResourceManager.load calls, 108 parses and 0 cache hits, and every
// motion switch paid for another multi-megabyte parse.  That churn is what
// shredded the heap into holes too small for the next parse in the crash this
// fix addresses.
//
// Only files released by a DESTROYED manager are parked here, and a parked file
// is handed over only when no manager holds it, so this never shares a live
// emotefile between two readers -- emotefile carries per-play state
// (_metadata->_varList, isMirror, ...) and must stay single-owner.  Explicit
// unloads (unload / unloadAll / clearCache) still free immediately: those are
// the game asking for memory back.
namespace
{
struct EmoteParkedFile
{
    ttstr path;
    emotefile* file;
    tjs_int seed;
};
// Enough to cover a motion cycle without holding much memory back.
const size_t EMOTE_PARKED_FILE_LIMIT = 4;
std::vector<EmoteParkedFile> _parkedFiles;

void EmoteParkFile(const ttstr& path, emotefile* file, tjs_int seed)
{
    if (file == nullptr) return;
    for (size_t i = 0; i < _parkedFiles.size(); i++)
    {
        if (_parkedFiles[i].path == path)
        {
            // An identical parse is already reserved; keep that one.
            delete file;
            return;
        }
    }
    if (_parkedFiles.size() >= EMOTE_PARKED_FILE_LIMIT)
    {
        delete _parkedFiles.front().file;
        _parkedFiles.erase(_parkedFiles.begin());
    }
    EmoteParkedFile parked;
    parked.path = path;
    parked.file = file;
    parked.seed = seed;
    _parkedFiles.push_back(parked);
}

emotefile* EmoteTakeParkedFile(const ttstr& path, tjs_int seed)
{
    for (size_t i = 0; i < _parkedFiles.size(); i++)
    {
        if (_parkedFiles[i].path != path) continue;
        if (_parkedFiles[i].seed != seed) continue; // parsed under another seed
        emotefile* file = _parkedFiles[i].file;
        _parkedFiles.erase(_parkedFiles.begin() + i);
        return file;
    }
    return nullptr;
}

void EmoteDropParkedFile(const ttstr& path)
{
    for (size_t i = 0; i < _parkedFiles.size(); i++)
    {
        if (_parkedFiles[i].path != path) continue;
        delete _parkedFiles[i].file;
        _parkedFiles.erase(_parkedFiles.begin() + i);
        return;
    }
}

void EmoteClearParkedFiles(const char* reason)
{
    if (_parkedFiles.empty()) return;
    TVPConsoleLog("emote: dropping %u parked motion file(s) for %s",
                  (unsigned)_parkedFiles.size(), reason);
    for (size_t i = 0; i < _parkedFiles.size(); i++)
        delete _parkedFiles[i].file;
    _parkedFiles.clear();
}
} // namespace

// KRKR-ns: drop the cached work layer when the script engine is torn down.
//
// _motionWorkLayer is built once, from the KAG window's poolLayer, and cached in
// this static for the lifetime of the process -- the `== nullptr` guard above
// means it is never rebuilt.  That is fine while one engine lives forever, but
// when a game ends and the engine is rebuilt (the in-process restart for hosts
// without next-load support), the cached adaptor still references a layer that
// died with the previous engine.  The next game's ResourceManager creation then
// touches that dangling object and faults inside
// tTJSObjectProxy::PropGet -> tTJSCustomObject::Find with a null `this`.
//
// Clearing the pointer makes the next ResourceManager build a fresh work layer
// against the current engine.  The global `motionWorkLayer` goes away with the
// old global object, so there is nothing else to unregister here.
void krkrsdl2_emote_reset_motion_work_layer()
{
    if (_motionWorkLayer)
    {
        TVPConsoleLog("emote: clearing cached motionWorkLayer %p for engine restart",
                      _motionWorkLayer);
        _motionWorkLayer = nullptr;
    }
    // ResourceManager::_kagWindow is the other half of the same problem: it is a
    // static copy of the KAG window's dispatch object (comment at its
    // declaration: "本身就是唯一的，所以直接static"), written once by
    // ResourceManager::init and read back by ResourceManager::load.  After an
    // engine restart it still names the PREVIOUS game's window, which died with
    // that engine -- reading it constructs a variant from a dead object and the
    // next property access faults in tTJSObjectProxy::PropGet ->
    // tTJSCustomObject::Find with a null `this`.  The next init() re-caches the
    // window that actually belongs to the current engine.
    //
    // No Release() here on purpose: this mirror is a borrowed pointer (the
    // script engine owns the window form), matching how init() stores it.
    if (ResourceManager::_kagWindow)
    {
        TVPConsoleLog("emote: clearing cached kagWindow %p for engine restart",
                      ResourceManager::_kagWindow);
        ResourceManager::_kagWindow = nullptr;
    }
    // The PSB decrypt seed + closure (ResourceManager statics, installed by a
    // game's patch script) are the SAME class of stale script reference and
    // DID hit in practice: after an engine restart the cached closure still
    // belongs to the previous game's patch script, and every .mtn parse of
    // the next game died inside it -- the second game silently lost its
    // E-mote opening animation and title art while loads kept succeeding.
    ResourceManager::resetDecryptStateForEngineRestart();
    // Parked parses belong to the game whose decrypt state produced them; the
    // next game must not pick them up.
    EmoteClearParkedFiles("engine restart");
    // Pooled render targets belong to the renderer that is being torn down.
    EmoteFlushTargetPool("engine restart");
}

// 递归查找 shapeNodeAreas 中指定名称的区域
// 也作为后备: 直接检查节点的frame中的shape信息
static bool findShapeAreaRecursive(emotemotionref* mtnRef, const char* name, emoterect& outArea)
{
    if (mtnRef == nullptr) return false;

    // 查找当前motion的预计算shapeNodeAreas
    for (auto& area : mtnRef->shapeNodeAreas)
    {
        if (std::strcmp(area.label.c_str(), name) == 0)
        {
            outArea = area;
            return true;
        }
    }

    // 递归查找子motion的shapeNodeAreas
    for (auto& n : mtnRef->_nodeCache)
    {
        if (n.currentMtn && n.currentMtnRef)
        {
            if (findShapeAreaRecursive(n.currentMtnRef, name, outArea))
                return true;
        }
    }

    return false;
}

// 递归查找子motion ref
static emotemotionref* findMotionRefRecursive(emotemotionref* mtnRef, const char* name)
{
    if (mtnRef == nullptr) return nullptr;

    for (auto& node : mtnRef->_nodeCache)
    {
        if (node.currentMtn && std::strcmp(node.currentNode->label.c_str(), name) == 0)
        {
            return node.currentMtnRef;
        }
    }

    // 递归查找子motion
    for (auto& node : mtnRef->_nodeCache)
    {
        if (node.currentMtn && node.currentMtnRef)
        {
            emotemotionref* found = findMotionRefRecursive(node.currentMtnRef, name);
            if (found) return found;
        }
    }

    return nullptr;
}

ResourceManager::ResourceManager(iTJSDispatch2* kagWindow, tjs_int cacheSize)
{
    // window info
    tjs_int sWidth = 1280, sHeight = 720;
    if (kagWindow != nullptr)
    {
        tTJSVariant val;
        kagWindow->PropGet(0, TJS_W("width"), NULL, &val, kagWindow);
        sWidth = (tjs_int)val;
        kagWindow->PropGet(0, TJS_W("height"), NULL, &val, kagWindow);
        sHeight = (tjs_int)val;
        _kagWindow = kagWindow;
    }
    TVPConsoleLog("ResourceManager created window=%p cache=%d size=%dx%d",
                  kagWindow, (int)cacheSize, (int)sWidth, (int)sHeight);
    // 放这里来吧，省得init时啥也没有
    if (_motionWorkLayer == nullptr)
    {
        // kag.poolLayer作为父类
        tTJSVariant baseLayer;
        iTJSDispatch2* kag = kagWindow;
        if (TJS_FAILED(kag->PropGet(0, TJS_W("poolLayer"), NULL, &baseLayer, kag)) ||
            baseLayer.Type() != tvtObject)
        {
            TVPConsoleLog("create motionWorkLayer failed");
            return;
        }
        // 创建motionWorkLayer实例
        _motionWorkLayer = new SeparateLayerAdaptor(baseLayer.AsObjectThisNoAddRef());
        // 置入全局变量
        tTJSVariant val = tTJSVariant(_motionWorkLayer);
        _motionWorkLayer->Release();
        iTJSDispatch2* global = TVPGetScriptDispatch();
        if (global)
        {
            global->PropSet(TJS_MEMBERENSURE, TJS_W("motionWorkLayer"), NULL, &val, global);
            global->Release();
        }
    }
}
ResourceManager::~ResourceManager()
{
    // The manager is going away but nothing asked for the memory back, so park
    // what it parsed for the next manager of this game (see _parkedFiles).
    unloadAllInternal(true);
}
tTJSVariant ResourceManager::load(tTJSString path)
{
    ttstr trimPath;
    if (path.StartsWith(TJS_W("lzfs://./")))
        trimPath = ttstr(path.c_str() + 9, path.GetLen() - 9);
    else
        trimPath = path;
    const ttstr placedPath = TVPGetPlacedPath(trimPath);
    const std::string requested8 = EmoteToUtf8(path);
    const std::string trimmed8 = EmoteToUtf8(trimPath);
    const std::string placed8 = EmoteToUtf8(placedPath);
    TVPConsoleLog("ResourceManager.load requested=%s trimmed=%s placed=%s",
                  requested8.c_str(), trimmed8.c_str(), placed8.c_str());
    auto rst = cacheData.find(placedPath);
    if (rst != cacheData.end())
    {
        TVPConsoleLog("ResourceManager.load cache hit: %s", placed8.c_str());
        return rst->second->root();
    }

    // KRKR-ns: the manager built for this motion replaces one that parsed the
    // same path a moment ago; reuse that parse instead of paying for another
    // multi-megabyte allocation.
    if (emotefile* parked = EmoteTakeParkedFile(placedPath, _decryptkey))
    {
        cacheData.insert(std::pair<ttstr, emotefile*>(placedPath, parked));
        TVPConsoleLog("ResourceManager.load parked hit: %s", placed8.c_str());
        return parked->root();
    }

    emotefile* file = new emotefile();
    file->setSeed(_decryptkey);
    file->setFun(_decryptClo);
    const bool loaded = file->load(trimPath);
    TVPConsoleLog("ResourceManager.load parsed=%d: %s", loaded ? 1 : 0, placed8.c_str());
    if (!loaded)
    {
        delete file;
        return tTJSVariant();
    }

    // motionKey是唯一可区分的表示符，我们用其作为标志
    cacheData.insert(std::pair<ttstr, emotefile*>(placedPath, file));
    return file->root();
}
void ResourceManager::unload(tTJSString path)
{
    ttstr trimPath;
    if (path.StartsWith(TJS_W("lzfs://./")))
        trimPath = ttstr(path.c_str() + 9, path.GetLen() - 9);
    else
        trimPath = path;
    auto it = cacheData.find(TVPGetPlacedPath(trimPath));
    if (it != cacheData.end())
    {
        if (it->second != nullptr)
            delete it->second;
        cacheData.erase(it);
    }
    // An explicit unload means the game wants the file gone, including any
    // reserved parse of it.
    EmoteDropParkedFile(TVPGetPlacedPath(trimPath));
}
void ResourceManager::unloadAll()
{
    unloadAllInternal(false);
}
void ResourceManager::unloadAllInternal(bool park)
{
    for (auto& item : cacheData)
    {
        if (item.second == nullptr) continue;
        if (park)
            EmoteParkFile(item.first, item.second, _decryptkey);
        else
            delete item.second;
    }
    cacheData.clear();
}
void ResourceManager::clearCache()
{
    // The original emoteplayer.dll frees every cached motion here; the port used
    // to leave this empty, so a game that clears its motion cache got nothing.
    if (!cacheData.empty())
        TVPConsoleLog("ResourceManager.clearCache dropped %u file(s)",
                      (unsigned)cacheData.size());
    unloadAllInternal(false);
}
emotefile* ResourceManager::GetPlayerByName(const tTJSString& name)
{
    auto it = cacheData.find(name);
    if (it != cacheData.end())
    {
        return it->second;
    }
    return nullptr;
}
void ResourceManager::setEmotePSBDecryptSeed(tjs_int decryptkey)
{
    _decryptkey = decryptkey;
}
void ResourceManager::setEmotePSBDecryptFunc(tTJSVariant funclosure)
{
    _decryptClo = funclosure.AsObjectClosure();
}

SeparateLayerAdaptor::SeparateLayerAdaptor(iTJSDispatch2* targetLayer)
{
    // 创建实例
    _this = new tTJSNI_Layer();
    tTJSVariant kag(ResourceManager::_kagWindow);
    tTJSVariant layer(targetLayer);
    tTJSVariant* params[] = {&kag, &layer};
    if (TJS_FAILED(_this->Construct(2, params, this)))
        TVPThrowExceptionMessage(TVPSpecifyLayer);
    // 获取父类实例
    tTJSNI_Layer* ths = NULL;
    if (targetLayer->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
                                           (iTJSNativeInstance**)&ths) < 0 ||
        ths == NULL)
        TVPThrowExceptionMessage(TVPSpecifyLayer);
    // 设置参数
    _this->SetSize(ths->GetWidth(), ths->GetHeight());
    _this->SetImageSize(ths->GetWidth(), ths->GetHeight());
    _this->SetVisible(true);
    _this->SetType(ltAlpha);
    _this->SetHitType(htProvince);
}
SeparateLayerAdaptor::~SeparateLayerAdaptor()
{
    clear();
}
void SeparateLayerAdaptor::assign(iTJSDispatch2* anotherAdaptor)
{
    // TODO
}
void SeparateLayerAdaptor::clear()
{
    // 渲染目标由 core/render 的 2D 渲染器统一管理
    // The getter selects and self-tests GL lazily. An empty adaptor must not
    // create a render context just because it is cleared or collected.
    krkrsdl3::iTVPRenderBackend* renderer = (target || maskTarget)
        ? krkrsdl3::TVPGetRenderBackend() : nullptr;
    if (renderer)
    {
        if (target)
        {
            DestroyRenderTargetSafe(renderer, target, _targetWidth, _targetHeight);
            target = nullptr;
        }
        if (maskTarget)
        {
            DestroyRenderTargetSafe(renderer, maskTarget, _targetWidth, _targetHeight);
            maskTarget = nullptr;
        }
    }
    _targetWidth = 0;
    _targetHeight = 0;
    if (_this != nullptr)
    {
        _this->Invalidate();
        delete _this;
        _this = nullptr;
    }
}
tjs_int SeparateLayerAdaptor::get_absolute()
{
    if (_this != nullptr)
    {
        return _this->GetAbsoluteOrderIndex();
    }
    return 0;
}
void SeparateLayerAdaptor::set_absolute(tjs_int v)
{
    if (_this != nullptr)
    {
        _this->SetAbsoluteOrderIndex(v);
    }
}
bool SeparateLayerAdaptor::get_isPrimary()
{
    if (_this != nullptr)
    {
        return _this->IsPrimary();
    }
    return false;
}
void SeparateLayerAdaptor::set_isPrimary(bool v)
{
    //
}
tTJSVariant SeparateLayerAdaptor::get_parent()
{
    if (_this != nullptr)
    {
        return _this->GetParent();
    }
    return tTJSVariant();
}
void SeparateLayerAdaptor::set_parent(tTJSVariant v)
{
    //
}
void SeparateLayerAdaptor::checkDrawArea(tjs_int width, tjs_int height)
{
    // KRKR-ns (2026-11-04): reuse the targets whenever the requested stage area
    // still fits.  Rebuilding two 11 MB targets on every size change is what
    // shredded the heap: a 15-minute tianshi session allocated 507 targets and
    // drove the malloc arena to 3.2 GB with only ~1.5 GB live.
    _width = width;
    _height = height;
    if (target != nullptr && maskTarget != nullptr &&
        _targetWidth >= width && _targetHeight >= height)
        return;

    krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
    if (!renderer)
        return;
    if (target)
    {
        DestroyRenderTargetSafe(renderer, target, _targetWidth, _targetHeight);
        target = nullptr;
    }
    if (maskTarget)
    {
        DestroyRenderTargetSafe(renderer, maskTarget, _targetWidth, _targetHeight);
        maskTarget = nullptr;
    }
    _targetWidth = 0;
    _targetHeight = 0;
    target = CreateRenderTargetSafe(renderer, width, height);
    maskTarget = target ? CreateRenderTargetSafe(renderer, width, height) : nullptr;
    if (!target || !maskTarget)
    {
        // Out of memory for this size: leave the adaptor empty so the caller
        // skips the frame instead of dying on std::bad_alloc.
        if (maskTarget)
        {
            DestroyRenderTargetSafe(renderer, maskTarget, width, height);
            maskTarget = nullptr;
        }
        if (target)
        {
            DestroyRenderTargetSafe(renderer, target, width, height);
            target = nullptr;
        }
        return;
    }
    _targetWidth = width;
    _targetHeight = height;
}

D3DAdaptor::D3DAdaptor(
    iTJSDispatch2* winRef, tjs_int width, tjs_int height, tjs_int orgX, tjs_int orgY)
{
    krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
    if (renderer)
    {
        _target = renderer->CreateTarget(width, height);
        _maskTarget = renderer->CreateTarget(width, height);
        _width = width;
        _height = height;
        _orgX = orgX;
        _orgY = orgY;
        TVPConsoleLog("[emote] D3DAdaptor created w=%d h=%d orgX=%d orgY=%d",
                      width, height, _orgX, _orgY);
    }
}
D3DAdaptor::~D3DAdaptor()
{
    krkrsdl3::iTVPRenderBackend* renderer = (_target || _maskTarget)
        ? krkrsdl3::TVPGetRenderBackend() : nullptr;
    if (renderer)
    {
        if (_target)
        {
            DestroyRenderTargetSafe(renderer, _target, _width, _height);
            _target = nullptr;
        }
        if (_maskTarget)
        {
            DestroyRenderTargetSafe(renderer, _maskTarget, _width, _height);
            _maskTarget = nullptr;
        }
    }
}
void D3DAdaptor::setClearColor(tjs_uint32 color)
{
    _clearColor = color;
}
void D3DAdaptor::captureCanvas(iTJSDispatch2* targetLayer)
{
    tTJSNI_BaseLayer* ths = NULL;
    if (targetLayer->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
                                       (iTJSNativeInstance**)&ths) < 0)
        return;
    krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
    if (!renderer)
        return;
    tTVPRect dirtyRect;
    const bool copied =
        CopyRenderTargetToLayer(renderer, _target, ths, _width, _height, &dirtyRect);
    if (!copied || dirtyRect.is_empty())
    {
        // Reached the copy but produced nothing: the render target held no
        // pixels, or the layer had no writable buffer.  Distinguished from the
        // early-out above, which means the target/renderer itself was missing.
        static int logged = 0;
        if (logged < 20)
        {
            ++logged;
            KRKRNS_LOG("[emote] draw: copy produced nothing (copied=%d empty=%d "
                       "render=%dx%d)", (int)copied, (int)(dirtyRect.is_empty()),
                       _width, _height);
        }
    }
    else
    {
        static int logged = 0;
        if (logged < 20)
        {
            ++logged;
            KRKRNS_LOG("[emote] draw: copied %dx%d onto layer (rect %d,%d %dx%d)",
                       _width, _height, (int)dirtyRect.left, (int)dirtyRect.top,
                       (int)dirtyRect.get_width(), (int)dirtyRect.get_height());
        }
    }
    if (copied && !dirtyRect.is_empty())
        ths->Update(dirtyRect);
}
void D3DAdaptor::unloadUnusedTextures()
{
    // 做一下清屏操作
    // D3D自己清理
    krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
    if (renderer)
    {
        renderer->SetTarget(_target);
        renderer->ClearTarget(true);
        renderer->UnlockTarget(_target);
    }
}

// 专门用来保存contain信息 两类节点mtn和shape
tTJSNativeClass* TVPCreateNativeClass_TmpMotionObj(EmotePlayer* ptr, emotemotionref* obj);
class TmpMotionObj : public tTJSNativeClass
{
    typedef tTJSNativeClass inherited;

public:
    TmpMotionObj(EmotePlayer* ptr, emotemotionref* obj)
      : tTJSNativeClass(TJS_W("MotionObj"))
    {
        _ptr = ptr;
        _obj = obj;
    }
    tjs_error FuncCall(tjs_uint32 flag,
        const tjs_char* membername,
        tjs_uint32* hint,
        tTJSVariant* result,
        tjs_int numparams,
        tTJSVariant** param,
        iTJSDispatch2* objthis)
    {
        // 古法找函数
        if (TJS_strcmp(membername, TJS_W("contains")) == 0)
        {
            if (numparams < 2)
                return TJS_E_BADPARAMCOUNT;
            tjs_real x = *param[0];
            tjs_real y = *param[1];
            if (result)
            {
                if (_obj)
                {
                    *result = _obj->contains(x, y);
                }
                else
                {
                    // 没有_obj: 这是shape节点，检查存储的l/t/w/h和shapeType进行碰撞检测
                    tTJSVariant vl, vt, vw, vh, vst;
                    bool hasL = TJS_SUCCEEDED(PropGet(0, TJS_W("l"), nullptr, &vl, this));
                    bool hasT = TJS_SUCCEEDED(PropGet(0, TJS_W("t"), nullptr, &vt, this));
                    bool hasW = TJS_SUCCEEDED(PropGet(0, TJS_W("w"), nullptr, &vw, this));
                    bool hasH = TJS_SUCCEEDED(PropGet(0, TJS_W("h"), nullptr, &vh, this));
                    bool hasST = TJS_SUCCEEDED(PropGet(0, TJS_W("shapeType"), nullptr, &vst, this));
                    bool res = false;
                    if (hasL && hasT && hasW && hasH)
                    {
                        double l = (double)vl, t = (double)vt;
                        double w = (double)vw, h = (double)vh;
                        int st = hasST ? (int)vst : 2; // 默认rect
                        switch (st)
                        {
                        case 1: // circle: 检查点到圆心距离是否 <= 半径
                        {
                            double cx = l + w / 2.0, cy = t + h / 2.0;
                            double r = w / 2.0; // 取宽的一半为半径
                            double dx = x - cx, dy = y - cy;
                            res = (dx * dx + dy * dy <= r * r);
                            break;
                        }
                        case 0: // point: 检测一个很小的范围(即点到圆心距离<1)
                        {
                            double cx = l, cy = t;
                            double dx = x - cx, dy = y - cy;
                            res = (dx * dx + dy * dy <= 1.0);
                            break;
                        }
                        case 3: // quad: 简化为矩形检测(精确quad需要存储4个顶点坐标)
                        default: // rect
                            res = (x >= l && x <= l + w && y >= t && y <= t + h);
                            break;
                        }
                        *result = res;
                    }
                    else
                    {
                        *result = false;
                    }
                }
            }
            return TJS_S_OK;
        }
        else if (TJS_strcmp(membername, TJS_W("getLayerGetter")) == 0)
        {
            if (numparams < 1)
                return TJS_E_BADPARAMCOUNT;

            ttstr name = *param[0];
            iTJSDispatch2* dsp = TJSCreateDictionaryObject();

            // 递归寻找子motion ref
            const std::string name8 = EmoteToUtf8(name);
            emotemotionref* emtObj = findMotionRefRecursive(_obj, name8.c_str());

            if (emtObj)
            {
                // 子motion: 同时设置motion和shape，使路径解析(getLayerMotion/getLayerShape)都能正常工作
                iTJSDispatch2* mtn = TVPCreateNativeClass_TmpMotionObj(_ptr, emtObj);
                if (mtn)
                {
                    tTJSVariant mtnVar(mtn);
                    dsp->PropSet(TJS_MEMBERENSURE, TJS_W("motion"), nullptr, &mtnVar, dsp);
                    dsp->PropSet(TJS_MEMBERENSURE, TJS_W("shape"), nullptr, &mtnVar, dsp);
                    mtn->Release();
                }
            }
            else
            {
                // 没找到子motion，在预计算的shapeNodeAreas中递归查找(后备则直接从frame提取)
                emoterect foundArea = {};
                if (_obj != nullptr && findShapeAreaRecursive(_obj, name8.c_str(), foundArea))
                {
                    iTJSDispatch2* shapeObj = TVPCreateNativeClass_TmpMotionObj(nullptr, nullptr);
                    if (shapeObj)
                    {
                        tTJSVariant vL(foundArea.left), vT(foundArea.top);
                        tTJSVariant vW(foundArea.width), vH(foundArea.height);
                        tTJSVariant vST(foundArea.shapeType);
                        shapeObj->PropSet(TJS_MEMBERENSURE, TJS_W("l"), nullptr, &vL, shapeObj);
                        shapeObj->PropSet(TJS_MEMBERENSURE, TJS_W("t"), nullptr, &vT, shapeObj);
                        shapeObj->PropSet(TJS_MEMBERENSURE, TJS_W("w"), nullptr, &vW, shapeObj);
                        shapeObj->PropSet(TJS_MEMBERENSURE, TJS_W("h"), nullptr, &vH, shapeObj);
                        shapeObj->PropSet(TJS_MEMBERENSURE, TJS_W("shapeType"), nullptr, &vST, shapeObj);
                        tTJSVariant shapeVar(shapeObj);
                        dsp->PropSet(TJS_MEMBERENSURE, TJS_W("shape"), nullptr, &shapeVar, dsp);
                        shapeObj->Release();
                    }
                }
            }

            tTJSVariant var(dsp);
            dsp->Release();
            if (result)
                *result = var;
            return TJS_S_OK;
        }
        else if (TJS_strcmp(membername, TJS_W("getLayerMotion")) == 0)
        {
            if (numparams < 1)
                return TJS_E_BADPARAMCOUNT;
            ttstr name = *param[0];

            // 递归查找指定名称的子motion
            const std::string name8 = EmoteToUtf8(name);
            emotemotionref* emtObj = findMotionRefRecursive(_obj, name8.c_str());

            if (emtObj && result)
            {
                iTJSDispatch2* mtn = TVPCreateNativeClass_TmpMotionObj(_ptr, emtObj);
                if (mtn)
                {
                    tTJSVariant mtnVar(mtn);
                    *result = mtnVar;
                    mtn->Release();
                }
            }
            return TJS_S_OK;
        }
        else if (TJS_strcmp(membername, TJS_W("setVariable")) == 0)
        {
            if (numparams < 2)
                return TJS_E_BADPARAMCOUNT;
            ttstr name = *param[0];
            tjs_real value = *param[1];
            if (_ptr)
            {
                _ptr->setVariable(name, value);
            }
            return TJS_S_OK;
        }
        else
            return TJS_E_MEMBERNOTFOUND;
    }

    ~TmpMotionObj()
    {
        _ptr = NULL;
        _obj = NULL;
    }
    static tjs_uint32 ClassID;

protected:
    tTJSNativeInstance* CreateNativeInstance() { return NULL; }

private:
    EmotePlayer* _ptr = NULL;
    emotemotionref* _obj = NULL;
};
tTJSNativeClass* TVPCreateNativeClass_TmpMotionObj(EmotePlayer* ptr, emotemotionref* obj)
{
    return new TmpMotionObj(ptr, obj);
}
tjs_uint32 TmpMotionObj::ClassID = (tjs_uint32)-1;

#define setprop_t(d, p, ty) \
    { \
        tTJSVariant v(ty(p)); \
        d->PropSet(TJS_MEMBERENSURE, TJS_W(#p), nullptr, &v, d); \
    }
#define setprop(d, p) setprop_t(d, p, )
#define getprop_t(d, p, ty) \
    { \
        tTJSVariant v; \
        if (TJS_SUCCEEDED(d->PropGet(0, TJS_W(#p), nullptr, &v, d)) && v.Type() != tvtVoid) \
        { \
            p = ty(v); \
        } \
    }
#define getprop(d, p) getprop_t(d, p, )
EmotePlayer::~EmotePlayer()
{
    ReleaseDrawTargets();
}
void EmotePlayer::ReleaseDrawTargets()
{
    // 渲染目标由 core/render 的 2D 渲染器统一管理
    krkrsdl3::iTVPRenderBackend* renderer = (_target || _maskTarget)
        ? krkrsdl3::TVPGetRenderBackend() : nullptr;
    if (renderer)
    {
        if (_target)
        {
            DestroyRenderTargetSafe(renderer, _target, _targetWidth, _targetHeight);
            _target = nullptr;
        }
        if (_maskTarget)
        {
            DestroyRenderTargetSafe(renderer, _maskTarget, _targetWidth, _targetHeight);
            _maskTarget = nullptr;
        }
    }
    _target = nullptr;
    _maskTarget = nullptr;
    _targetWidth = 0;
    _targetHeight = 0;
}
int32_t EmotePlayer::get_loopTime()
{
    if (emtEngine._mainfile != nullptr && emtEngine._mainfile->_objects.size() > 0 &&
        emtEngine._mainfile->_objects.begin()->second->motion.size() > 0)
    {
        return emtEngine._mainfile->_objects.begin()->second->motion.begin()->second->loopTime;
    }
    return 0;
}
tTJSVariant EmotePlayer::get_variableKeys()
{
    iTJSDispatch2* array = TJSCreateArrayObject();
    if (emtEngine._mainfile != nullptr)
    {
        std::set<std::string> varList;
        for (auto varItm : emtEngine._mainfile->_metadata->_varList)
        {
            tTJSVariant tmp(EmoteFromUtf8(varItm.first));
            tTJSVariant* args[] = {&tmp};
            static tjs_uint addHint = 0;
            array->FuncCall(0, TJS_W("add"), &addHint, nullptr, 1, args, array);
        }
    }
    tTJSVariant result(array, array);
    array->Release();
    return result;
}
tTJSVariant EmotePlayer::serialize()
{
    auto dict = TJSCreateDictionaryObject();

    setprop(dict, currCoordx);
    setprop(dict, currCoordy);
    setprop(dict, currCoordz);
    setprop(dict, currAngle);
    setprop(dict, currZx);
    setprop(dict, currZy);

    auto res = tTJSVariant(dict, dict);
    dict->Release();
    return res;
}
void EmotePlayer::unserialize(tTJSVariant data)
{
    auto dict = data.AsObjectNoAddRef();
    if (!dict)
    {
        return;
    }

    getprop_t(dict, currCoordx, static_cast<tjs_real>);
    getprop_t(dict, currCoordy, static_cast<tjs_real>);
    getprop_t(dict, currCoordz, static_cast<tjs_real>);
    getprop_t(dict, currAngle, static_cast<tjs_real>);
    getprop_t(dict, currZx, static_cast<tjs_real>);
    getprop_t(dict, currZy, static_cast<tjs_real>);
}
void EmotePlayer::play(tTJSString name, int flag)
{
    const std::string requestedMotion8 = EmoteToUtf8(name);
    const std::string requestedChara8 = EmoteToUtf8(_chara);
    TVPConsoleLog("EmotePlayer.play chara=%s motion=%s flag=%d cached=%u mode=%s",
                  requestedChara8.c_str(), requestedMotion8.c_str(), flag,
                  (unsigned)_resourceManager->cacheData.size(), isMotion ? "motion" : "key");
    if (emtEngine._mainfile != nullptr && !isMotion) // motionKey的启动模式
    {
        // motion
        auto it = emtEngine._mainfile->_objects.find(emtEngine._mainfile->_metadata->chara.c_str());
        if (it != emtEngine._mainfile->_objects.end())
        {
            auto it1 = it->second->motion.find(emtEngine._mainfile->_metadata->motion.c_str());
            if (it1 != it->second->motion.end())
            {
                emtEngine._mainmotion = it1->second;
            }
        }
        // start
        clockPassed = 0.0;
        _motion = name;
        _playing = true;
        _allplaying = true;
        isSelfClear = true;
    }
    else if (_resourceManager->cacheData.size() > 0 && isMotion) // chara+motion启动方案
    {
        _motion = name;
        emtEngine._mainmotion = nullptr;
        // motion
        for (auto tmpFile : _resourceManager->cacheData)
        {
            const std::string chara8 = EmoteToUtf8(_chara);
            const std::string motion8 = EmoteToUtf8(_motion);
            auto it = tmpFile.second->_objects.find(chara8);
            if (it != tmpFile.second->_objects.end())
            {
                auto it1 = it->second->motion.find(motion8);
                if (it1 != it->second->motion.end())
                {
                    emtEngine._mainmotion = it1->second;
                }
            }
            if (emtEngine._mainmotion)
            {
                emtEngine._mainfile = tmpFile.second;
                break;
            }
                
        }
        // start
        clockPassed = 0.0;
        _playing = true;
        _allplaying = true;
        isSelfClear = true;
    }
    else // 群体启动模式，即对manager的所有file进行拼好件(其会存在互相索引的情况，结构可能得改改了)
    {
        emtEngine._mainfile = nullptr;
        emtEngine._mainmotion = nullptr;
        for (auto itm : _resourceManager->cacheData)
        {
            // 查询
            auto it = itm.second->_objects.find(itm.second->_metadata->chara.c_str());
            if (it != itm.second->_objects.end())
            {
                auto it1 = it->second->motion.find(itm.second->_metadata->motion.c_str());
                if (it1 != it->second->motion.end())
                {
                    emtEngine._mainfile = itm.second;
                    emtEngine._mainmotion = it1->second;
                    break;
                }
            }
        }
        // 对剩下文件进行相互并连(通过引擎管理附属文件)
        for (auto itm : _resourceManager->cacheData)
        {
            if (itm.second != emtEngine._mainfile)
                emtEngine.addEmoteFile(itm.second);
        }
        // start
        clockPassed = 0.0;
        _motion = name;
        _playing = true;
        _allplaying = true;
        isSelfClear = false;
    }
    _isStop = false;
    // A new motion must reach the layer even if the previous one was stopped
    // before it ever painted (title backgrounds start the player stopped).
    _finitePlaybackGraph = IsFinitePlaybackGraph(emtEngine._mainmotion,
        [](emotemotion* motion) {
            return IsFiniteMotion(motion);
        }, [this](emotemotion* motion) {
            std::vector<emotemotion*> children;
            for (auto* node : motion->nodeList) {
                if (!node) continue;
                for (auto* frame : node->frameList)
                    if (frame && frame->hasContent && frame->src.compare(0, 7, "motion/") == 0)
                        children.push_back(emtEngine.findmotionByName(frame->src));
            }
            return children;
        });
    TVPConsoleLog("EmotePlayer.play selected file=%p motion=%p playing=%d finite=%d vars=%u sync=%.3f selfSync=%.3f last=%.3f loop=%.3f",
                  emtEngine._mainfile, emtEngine._mainmotion, _playing ? 1 : 0,
                  _finitePlaybackGraph ? 1 : 0,
                  emtEngine._mainfile ? (unsigned)emtEngine._mainfile->_metadata->_varList.size() : 0,
                  emtEngine._mainmotion ? (double)emtEngine._mainmotion->syncTime : 0.0,
                  emtEngine._mainmotion ? (double)emtEngine._mainmotion->selfSyncTime : 0.0,
                  emtEngine._mainmotion ? emtEngine._mainmotion->lastTime : 0.0,
                  emtEngine._mainmotion ? emtEngine._mainmotion->loopTime : 0.0);
}
void EmotePlayer::initPhysics(tTJSVariant metadata)
{
    TVPConsoleLog("EmotePlayer::initPhysics TODO");
}
void EmotePlayer::clear(iTJSDispatch2* layer, tjs_uint32 neutralColor)
{
    auto* self = ncbInstanceAdaptor<SeparateLayerAdaptor>::GetNativeInstance(layer);
    tTJSNI_BaseLayer* ths = NULL;
    if (self != nullptr)
    {
        ths = self->GetLayer();
    }
    else
    {
        if (layer->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
                                         (iTJSNativeInstance**)&ths) < 0)
            return;
        withoutAdaptor = true;
    }
    if (ths == NULL)
        return;

    void* target = withoutAdaptor ? _target : (self ? self->target : nullptr);
    if (!target) return;

    // 清屏统一走 2D 渲染器（GL 后端清 FBO，软渲染后端清 CPU 缓冲）
    krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
    if (renderer)
    {
        renderer->SetTarget(target);
        renderer->ClearTarget(true);
        renderer->UnlockTarget(target);
    }
}
void EmotePlayer::progress(tjs_real mstime)
{
    EmoteProfTimer profTimer(krkrsdl2_prof_emote_progress);
    (void)profTimer;
    krkrsdl2_prof_emote_prog_call(); // KRKR-ns: progress call rate
    if (_isStop)
        return;
    if (emtEngine._mainfile != nullptr && emtEngine._mainmotion != nullptr && clockPassed > -1.0 &&
        _limitArea.width != _limitArea.originX && _limitArea.height != _limitArea.originY)
    {
        if (_playing)
        {
            const tjs_real divisor = speedRatio > 0.0 ? speedRatio : 20.0;
#if defined(KRKRNS_EMOTE_VERBOSE_DIAGNOSTICS)
            if (progressLogCount < 8)
            {
                TVPConsoleLog("progress dtMs=%.3f divisor=%.3f frameBefore=%.3f",
                              static_cast<double>(mstime), static_cast<double>(divisor),
                              static_cast<double>(clockPassed));
                ++progressLogCount;
            }
#endif
            clockPassed += mstime / divisor;
        }
        std::vector<emoteRender> empty;
        empty.push_back(_renderMethod);
        // mirror
        if (emtEngine._mainfile->isMirror)
        {
            empty.at(0).attachMat = glm::scale(empty.at(0).attachMat, glm::vec3(-1.0f, 1.0f, 1.0f));
        }
        // condition
        if (emtEngine._mainfile->_metadata->_varList.size() > 0)
        {
            // 更新控制参数(通过引擎调用)
            emtEngine.updateEyeControl(clockPassed, true);
            emtEngine.updateTimelineControl(clockPassed, true);
            // 使用emoteengine::progress构建独立ref树
            emtEngine.progress(0, empty, _limitArea);
        }
        else
        {
            // 是否结束
            if (!isMotion && clockPassed > emtEngine._mainmotion->lastTime)
            {
                _playing = false;
            }
            // 对于motion限制最后时间并结束
            // 参考ref逻辑: syncTime优先, 其次是selfSyncTime, 最后用lastTime作为兜底
            if (isMotion && emtEngine._mainmotion->loopTime < 0)
            {
                tjs_real endTime = emtEngine._mainmotion->syncTime;
                if (endTime <= 0.0) endTime = emtEngine._mainmotion->selfSyncTime;
                if (endTime <= 0.0) endTime = emtEngine._mainmotion->lastTime;
                if (clockPassed > endTime)
                {
                    clockPassed = endTime;
                    _playing = false;
                }
            }
            // 使用emoteengine::progress构建独立ref树
            emtEngine.progress(clockPassed, empty, _limitArea);
        }
        if (_allplaying && HasCompletedFinitePlayback(_playing, _finitePlaybackGraph,
                !emtEngine._mainfile->_metadata->_varList.empty(),
                !emtEngine.currTimeline.empty()))
        {
            _allplaying = false;
            TVPConsoleLog("EmotePlayer completed motion=%s frame=%.3f",
                          EmoteToUtf8(_motion).c_str(), (double)clockPassed);
        }
        // ping-pong触发更新draw，位置暂时选这里，让它频繁点
        if (_pipoVal == 0)
            _pipoVal = 1;
        else
            _pipoVal = 0;
    }
}
void EmotePlayer::draw(iTJSDispatch2* objthis)
{
    EmoteProfTimer profTimer(krkrsdl2_prof_emote_draw);
    (void)profTimer;
    krkrsdl2_prof_emote_draw_call(); // KRKR-ns: draw call rate
    // Upstream draws unconditionally; only progress() stops advancing when
    // _isStop.  An earlier "skip drawing once stopped" guard here made a title
    // whose background is an E-mote motion go black as soon as its render target
    // was recreated (page switch or Clear), because the new target starts empty
    // and nothing ever painted into it.
    auto* self = ncbInstanceAdaptor<SeparateLayerAdaptor>::GetNativeInstance(objthis);
    tTJSNI_BaseLayer* ths = NULL;
    D3DAdaptor* d3dAdaptor = NULL;
    if (self != nullptr)
    {
        ths = self->GetLayer();
        if (ths == NULL)
            return;
    }
    if (ths == NULL)
    {
        d3dAdaptor = ncbInstanceAdaptor<D3DAdaptor>::GetNativeInstance(objthis);
        if (d3dAdaptor != nullptr)
            withD3DAdaptor = true;
    }
    if (d3dAdaptor == NULL && ths == NULL)
    {
        if (objthis->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
                                           (iTJSNativeInstance**)&ths) < 0)
            return;
        withoutAdaptor = true;
    }

    if (emtEngine._mainfile != nullptr && emtEngine._mainmotion != nullptr)
    {
        // 渲染统一走 core/render 的 2D 渲染抽象（GL/软渲染选择在 core 内部完成）
        krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
        if (!renderer)
            return;
        if (!loggedDrawReady)
        {
            TVPConsoleLog("EmotePlayer.draw ready target=%p d3d=%d withoutAdaptor=%d",
                          objthis, withD3DAdaptor ? 1 : 0, withoutAdaptor ? 1 : 0);
            loggedDrawReady = true;
        }
        void* target = nullptr;
        void* maskTarget = nullptr;
        if (withD3DAdaptor)
        {
            target = d3dAdaptor->_target;
            maskTarget = d3dAdaptor->_maskTarget;
            _limitArea.width = d3dAdaptor->_width;
            _limitArea.height = d3dAdaptor->_height;
            // TODO 关于位置的锚定，后续再研究研究，如何进行全面的统一
            _limitArea.originX = d3dAdaptor->_orgX - d3dAdaptor->_width / 2;
            _limitArea.originY = d3dAdaptor->_orgY - d3dAdaptor->_height / 2;
            _width = d3dAdaptor->_width;
            _height = d3dAdaptor->_height;
            if (emtEngine._mainfile != nullptr)
            {
                _limitArea.zMax = emtEngine.getZMax() * 2;
            }
            if (_limitArea.zMax < 30.0f)
                _limitArea.zMax = 30.0f;
            updateTransMat();
        }
        else
        {
            ResetDrawArea(ths->GetWidth(), ths->GetHeight());
            if (withoutAdaptor)
            {
                target = _target;
                maskTarget = _maskTarget;
            }
            else
            {
                self->checkDrawArea(ths->GetWidth(), ths->GetHeight());
                target = self->target;
                maskTarget = self->maskTarget;
            }
            if (!target || !maskTarget)
                return;
            // 启用
            renderer->SetTarget(target);
            // isSelfClear: 自主清屏模式；否则由脚本 clear() 完成清屏
            renderer->ClearTarget(isSelfClear);
        }
        // 使用emoteengine::draw进行绘制(使用progress阶段缓存的独立ref树)
        try
        {
            emtEngine.draw(renderer, target, _limitArea, maskTarget);
            if (!withD3DAdaptor)
            {
                // 回读 CPU 像素并交给图层（GL 后端经 glReadPixels，软渲染后端零拷贝）
                tTVPRect dirtyRect;
                if (CopyRenderTargetToLayer(renderer, target, ths, _width, _height,
                                            &dirtyRect) &&
                    !dirtyRect.is_empty())
                    ths->Update(dirtyRect);
            }
            else
            {
                // Drawing borrowed the window's SDL renderer.  Restore its target
                // and viewport before returning to the normal Kirikiri compositor;
                // captureCanvas starts its own short readback session later.
                renderer->UnlockTarget(target);
            }
        }
        catch (const std::bad_alloc&)
        {
            // KRKR-ns (2026-11-04): the engine's scratch/mask surfaces are also
            // multi-megabyte.  A failed frame must not become a fatal script
            // error: release what we can, ask for a compaction and drop the frame.
            TVPConsoleLog("[emote] draw allocation failed; skipping this frame");
            TVPDeliverCompactEvent(TVP_COMPACT_LEVEL_CRITICAL);
            ReleaseDrawTargets();
            return;
        }
        // The layer now holds this player's pixels, so a later stop() can leave it
        // alone instead of redrawing the same frame every call.
    }
    else if (!loggedDrawMissingState)
    {
        TVPConsoleLog("EmotePlayer.draw skipped file=%p motion=%p", emtEngine._mainfile,
                      emtEngine._mainmotion);
        loggedDrawMissingState = true;
    }
}

// GPU 直通绘制：直接绘制到给定后端离屏目标（不回读、不经引擎 Layer）
void EmotePlayer::drawToTarget(krkrsdl3::iTVPRenderBackend* renderer,
                               void* target,
                               void* maskTarget,
                               bool selfClear,
                               tjs_int width,
                               tjs_int height,
                               tjs_int originX,
                               tjs_int originY)
{
    if (emtEngine._mainfile == nullptr || emtEngine._mainmotion == nullptr)
        return;
    if (!renderer || !target)
        return;
    // D3D 直通路径不经 draw()/ResetDrawArea()：_limitArea 必须在这里初始化，
    // 否则 progress() 因"区域为零"提前返回（动画不推进）、updateTransMat()
    // 投影矩阵退化（什么都不画）。只补区域/变换，不创建软渲染目标。
    if (width > 0 && height > 0 &&
        (_limitArea.width == _limitArea.originX || _limitArea.height == _limitArea.originY ||
         _width != width || _height != height || _limitArea.originX != originX ||
         _limitArea.originY != originY))
    {
        _width = width;
        _height = height;
        _limitArea.originX = originX;
        _limitArea.originY = originY;
        _limitArea.width = width;
        _limitArea.height = height;
        if (emtEngine._mainfile != nullptr)
            _limitArea.zMax = emtEngine.getZMax() * 2;
        if (_limitArea.zMax < 30.0f)
            _limitArea.zMax = 30.0f;
        updateTransMat();
    }
    renderer->SetTarget(target);
    renderer->ClearTarget(selfClear);
    emtEngine.draw(renderer, target, _limitArea, maskTarget);
    renderer->UnlockTarget(target);
}
void EmotePlayer::assign(iTJSDispatch2* anotherAdaptor)
{
    TVPConsoleLog("EmotePlayer::assign TODO");
}
void EmotePlayer::setCoord(tjs_real x, tjs_real y)
{
    currCoordx = x;
    currCoordy = y;
    updateTransMat();
}
void EmotePlayer::setScale(tjs_real scale)
{
    currZx = scale;
    currZy = scale;
    updateTransMat();
}
void EmotePlayer::setRotate(tjs_real rotate)
{
    currAngle = rotate;
    updateTransMat();
}
void EmotePlayer::setColor(tjs_uint32 color)
{
    TVPConsoleLog("EmotePlayer::setColor TODO");
}
void EmotePlayer::setVariable(tTJSString name, tjs_real value)
{
    if (emtEngine._mainfile != nullptr)
    {
        std::string tmpName = EmoteToUtf8(name);
        tmpName.append(1, '\0'); // 终有一天，我会把这sb字符串给优化掉
        emtEngine.setVariable(tmpName, value); // 管你有没有，设了再说
    }
}
tjs_real EmotePlayer::getVariable(tTJSString name)
{
    if (emtEngine._mainfile != nullptr)
    {
        std::string tmpName = EmoteToUtf8(name);
        tmpName.append(1, '\0'); // 终有一天，我会把这sb字符串给优化掉
        return emtEngine.getVariable(tmpName);
    }
    return 0.0;
}
void EmotePlayer::setOuterForce(tTJSString name, tjs_real ofx, tjs_real ofy)
{
    TVPConsoleLog("EmotePlayer::setOuterForce TODO");
}
void EmotePlayer::setDrawAffineTranslateMatrix(
    tjs_real a, tjs_real b, tjs_real c, tjs_real d, tjs_int tx, tjs_int ty)
{
    if (emtEngine._mainfile != nullptr)
    {
        _affineTrans = glm::mat4(a, -c, 0.0f, 0.0f, -b, d, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, tx,
                                 ty, 0.0f, 1.0f);
        updateTransMat();
    }
}
void EmotePlayer::setCameraOffset(tjs_int w, tjs_int h)
{
    currCamX = w;
    currCamY = h;
}
void EmotePlayer::startWind(
    tjs_real start, tjs_real goal, tjs_real speed, tjs_real powMin, tjs_real powMax)
{
    TVPConsoleLog("EmotePlayer::startWind TODO");
}
void EmotePlayer::stopWind()
{
    TVPConsoleLog("EmotePlayer::stopWind TODO");
}
bool EmotePlayer::contains(tjs_real x, tjs_real y)
{
    if (emtEngine._mainMotionRef != nullptr)
    {
        return emtEngine._mainMotionRef->contains(x, y);
    }
    return false;
}
void EmotePlayer::skip()
{
    // Reaching the last frame of the current motion is the same state as a
    // natural completion: stop advancing and let scripts polling the playing
    // flags proceed.  Leaving them set made a title that calls skip() wait for
    // an animation that would never finish, so the screen froze.
    if (emtEngine._mainmotion != nullptr)
    {
        tjs_real last = emtEngine._mainmotion->lastTime;
        if (last > 0.0)
        {
            clockPassed = last;
            TVPConsoleLog("EmotePlayer::skip frame=%.3f", (double)last);
        }
    }
    _playing = false;
    _allplaying = false;
    _pipoVal ^= 1;
}
void EmotePlayer::skipToSync()
{
    if (emtEngine._mainmotion != nullptr)
    {
        const tjs_real sync = emtEngine._mainfile->getSyncTime();
        clockPassed = sync;
        TVPConsoleLog("EmotePlayer::skipToSync frame=%.3f", (double)sync);
    }
    // Same completion contract as skip(): the pose at the sync frame is final
    // for this motion, so clear the playing flags.  This used to be a bare TODO
    // that only moved the clock, which left the title script waiting forever.
    _playing = false;
    _allplaying = false;
    _pipoVal ^= 1;
}
void EmotePlayer::pass()
{
    TVPConsoleLog("EmotePlayer::pass TODO");
}
void EmotePlayer::stop()
{
    _isStop = true;
    _playing = false;
    _allplaying = false;
    // KRKR-ns (2026-11-05): keep the targets.  Releasing them here made the next
    // draw() fetch a pooled target again while the title script still round-robins
    // stop()/draw(), and that combination left the emulator main loop without
    // updates (frozen picture, live heartbeats).  stop() is a state change, not a
    // lifetime end: the pool already removes the allocation churn it was meant for.
    TVPConsoleLog("EmotePlayer::stop");
}
void EmotePlayer::playTimeline(tTJSString name, tjs_int flags)
{
    if (emtEngine._mainfile != nullptr)
    {
        emtEngine.startTimeline(-10000.0f, EmoteToUtf8(name),
                                true); // 管你有没有，设了再说
    }
}
void EmotePlayer::stopTimeline(tTJSString name)
{
    if (emtEngine._mainfile != nullptr)
    {
        emtEngine.stopTimeline(EmoteToUtf8(name), true); // 管你有没有，设了再说
    }
}
bool EmotePlayer::getTimelinePlaying(tTJSString name)
{
    if (emtEngine._mainfile != nullptr)
    {
        bool ret = false;
        if (emtEngine.checkTimline(EmoteToUtf8(name), ret, true))
            return ret;
    }
    return false;
}
bool EmotePlayer::getLoopTimeline(tTJSString name)
{
    if (emtEngine._mainfile != nullptr)
    {
        for (auto itm : emtEngine._mainfile->_metadata->_timelineControl)
        {
            if (itm->label == EmoteToUtf8(name))
            {
                return itm->lastTime < 0;
            }
        }
    }
    return false;
}
tjs_real EmotePlayer::getTimelineTotalFrameCount(tTJSString name)
{
    if (emtEngine._mainfile != nullptr)
    {
        for (auto itm : emtEngine._mainfile->_metadata->_timelineControl)
        {
            if (itm->label == EmoteToUtf8(name))
            {
                return itm->loopEnd - itm->loopBegin + 1;
            }
        }
    }
    return 0;
}
tTJSVariant EmotePlayer::getMainTimelineLabelList()
{
    iTJSDispatch2* array = TJSCreateArrayObject();
    if (emtEngine._mainfile != nullptr)
    {
        for (auto itm : emtEngine._mainfile->_metadata->_timelineControl)
        {
            if (itm->diff == 0)
            {
                tTJSVariant tmp(EmoteFromUtf8(itm->label));
                tTJSVariant* args[] = {&tmp};
                static tjs_uint addHint = 0;
                array->FuncCall(0, TJS_W("add"), &addHint, nullptr, 1, args, array);
            }
        }
    }
    tTJSVariant result(array, array);
    array->Release();
    return result;
}
tTJSVariant EmotePlayer::getDiffTimelineLabelList()
{
    iTJSDispatch2* array = TJSCreateArrayObject();
    if (emtEngine._mainfile != nullptr)
    {
        for (auto itm : emtEngine._mainfile->_metadata->_timelineControl)
        {
            if (itm->diff == 1)
            {
                tTJSVariant tmp(EmoteFromUtf8(itm->label));
                tTJSVariant* args[] = {&tmp};
                static tjs_uint addHint = 0;
                array->FuncCall(0, TJS_W("add"), &addHint, nullptr, 1, args, array);
            }
        }
    }
    tTJSVariant result(array, array);
    array->Release();
    return result;
}
void EmotePlayer::setTimelineBlendRatio(tTJSString name,
                                        tjs_real ratio,
                                        tjs_real time,
                                        tjs_real easing)
{
    TVPConsoleLog("EmotePlayer::setTimelineBlendRatio TODO");
}
void EmotePlayer::fadeInTimeline(tTJSString name, tjs_real time, tjs_real easing)
{
    playTimeline(name);
}
void EmotePlayer::fadeOutTimeline(tTJSString name, tjs_real time, tjs_real easing)
{
    stopTimeline(name);
}
tTJSVariant EmotePlayer::getPlayingTimelineInfoList()
{
    iTJSDispatch2* array = TJSCreateArrayObject();
    tTJSVariant result(array, array);
    if (emtEngine._mainfile != nullptr)
    {
        for (auto playingTimeline : emtEngine.currTimeline)
        {
            iTJSDispatch2* obj = TJSCreateDictionaryObject();
            tTJSVariant val(EmoteFromUtf8(playingTimeline->label));
            obj->PropSet(TJS_MEMBERENSURE, TJS_W("label"), NULL, &val, obj);
            tTJSVariant objItm(obj, obj);
            obj->Release();
            tTJSVariant tmp(objItm);
            tTJSVariant* args[] = {&tmp};
            static tjs_uint addHint = 0;
            array->FuncCall(0, TJS_W("add"), &addHint, nullptr, 1, args, array);
        }
    }
    array->Release();
    return result;
}
tTJSVariant EmotePlayer::getVariableFrameList(tTJSString name)
{
    if (emtEngine._mainfile != nullptr)
    {
        iTJSDispatch2* root = emtEngine._mainfile->root().AsObject();
        tTJSVariant itm;
        if (TJS_FAILED(root->PropGet(0, TJS_W("metadata"), NULL, &itm, root)))
        {
            TVPConsoleLog("emotefile donot contain metadata");
            return tTJSVariant();
        }
        root->Release();
        root = itm.AsObject();
        if (TJS_FAILED(root->PropGet(0, TJS_W("variableList"), NULL, &itm, root)))
        {
            TVPConsoleLog("emotefile donot contain variableList");
            return tTJSVariant();
        }
        root->Release();
        root = itm.AsObjectThis();

        tTJSVariant retNeed;
        for (tjs_uint32 i = 0; i < emtEngine._mainfile->_metadata->_varList.size(); i++)
        {
            tTJSVariant varItem;
            if (TJS_FAILED(root->PropGetByNum(TJS_MEMBERMUSTEXIST, i, &varItem, root)))
                break;
            iTJSDispatch2* rev = varItem.AsObjectThisNoAddRef();
            tTJSVariant labelname;
            if (TJS_FAILED(rev->PropGet(0, TJS_W("label"), NULL, &labelname, rev)))
                continue;
            if (labelname.Type() != tvtString || ttstr(labelname) != name)
                continue;
            if (TJS_FAILED(rev->PropGet(0, TJS_W("frameList"), NULL, &retNeed, rev)))
                continue;
            break;
        }

        root->Release();
        return retNeed;
    }
    else
    {
        iTJSDispatch2* array = TJSCreateArrayObject();
        TVPConsoleLog("EmotePlayer::getVariableFrameList TODO");
        tTJSVariant result(array, array);
        array->Release();
        return result;
    }
}
tTJSVariant EmotePlayer::getCommandList()
{
    iTJSDispatch2* dsp = TJSCreateArrayObject();

    // 让它能触发更新就行了
    tTJSVariant tmp(_pipoVal);
    tTJSVariant* args[] = {&tmp};
    static tjs_uint addHint = 0;
    dsp->FuncCall(0, TJS_W("add"), &addHint, nullptr, 1, args, dsp);

    tTJSVariant var(dsp);
    dsp->Release();
    return var;
}
tTJSVariant EmotePlayer::getLayerGetter(tTJSString name)
{
    iTJSDispatch2* dsp = TJSCreateDictionaryObject();

    // 递归寻找子motion ref
    const std::string name8 = EmoteToUtf8(name);
    emotemotionref* emtObj = findMotionRefRecursive(emtEngine._mainMotionRef, name8.c_str());

    if (emtObj)
    {
        // motion
        iTJSDispatch2* mtn = TVPCreateNativeClass_TmpMotionObj(this, emtObj);
        if (mtn)
        {
            tTJSVariant mtnVar(mtn);
            dsp->PropSet(TJS_MEMBERENSURE, TJS_W("motion"), nullptr, &mtnVar, dsp);
            mtn->Release();
        }
    }
    else if (emtEngine._mainMotionRef != nullptr)
    {
        // 在预计算的shapeNodeAreas中递归查找(后备则直接从frame提取)
        emoterect foundArea = {};
        if (findShapeAreaRecursive(emtEngine._mainMotionRef, name8.c_str(), foundArea))
        {
            iTJSDispatch2* shapeObj = TVPCreateNativeClass_TmpMotionObj(nullptr, nullptr);
            if (shapeObj)
            {
                tTJSVariant vL(foundArea.left), vT(foundArea.top), vW(foundArea.width), vH(foundArea.height);
                tTJSVariant vST(foundArea.shapeType);
                shapeObj->PropSet(TJS_MEMBERENSURE, TJS_W("l"), nullptr, &vL, shapeObj);
                shapeObj->PropSet(TJS_MEMBERENSURE, TJS_W("t"), nullptr, &vT, shapeObj);
                shapeObj->PropSet(TJS_MEMBERENSURE, TJS_W("w"), nullptr, &vW, shapeObj);
                shapeObj->PropSet(TJS_MEMBERENSURE, TJS_W("h"), nullptr, &vH, shapeObj);
                shapeObj->PropSet(TJS_MEMBERENSURE, TJS_W("shapeType"), nullptr, &vST, shapeObj);
                tTJSVariant shapeVar(shapeObj);
                dsp->PropSet(TJS_MEMBERENSURE, TJS_W("shape"), nullptr, &shapeVar, dsp);
                shapeObj->Release();
            }
        }
    }
    
    tTJSVariant var(dsp);
    dsp->Release();
    return var;
}
tTJSVariant EmotePlayer::getLayerMotion(tTJSString name)
{
    // 调用getLayerGetter后返回motion属性
    tTJSVariant getter = getLayerGetter(name);
    if (getter.Type() == tvtObject)
    {
        iTJSDispatch2* dsp = getter.AsObjectNoAddRef();
        if (dsp)
        {
            tTJSVariant motion;
            if (TJS_SUCCEEDED(dsp->PropGet(0, TJS_W("motion"), NULL, &motion, dsp)))
            {
                return motion;
            }
        }
    }
    return tTJSVariant();
}
void EmotePlayer::setFlip(bool isFlip)
{
    if (isFlip)
        currZy = -currZy;
}
void EmotePlayer::setSlant(tjs_real x, tjs_real y)
{
    // unknow
}
void EmotePlayer::setZoom(tjs_real x, tjs_real y)
{
    currZx = x;
    currZy = y;
    updateTransMat();
}
void EmotePlayer::updateTransMat()
{
    _renderMethod.type = 3;
    _renderMethod.opa = 1.0f;
    // 构建变换矩阵
    glm::mat4 projection = glm::ortho(-_limitArea.originX, _limitArea.width - _limitArea.originX,
                                      _limitArea.height - _limitArea.originY, -_limitArea.originY,
                                      _limitArea.zMax, -_limitArea.zMax);
    _renderMethod.attachMat = projection * _affineTrans;
    _renderMethod.currCoordx = currCoordx + currCamX;
    _renderMethod.currCoordy = currCoordy + currCamY;
    _renderMethod.currAngle = currAngle;
    _renderMethod.currZx = currZx;
    _renderMethod.currZy = currZy;
}
void EmotePlayer::ResetDrawArea(tjs_int width, tjs_int height)
{
    if (_width == width && _height == height && _target != nullptr && _maskTarget != nullptr)
        return;
    // limit
    _limitArea.originX = 0;
    _limitArea.originY = 0;
    _width = width;
    _height = height;
    _limitArea.width = width;
    _limitArea.height = height;
    if (emtEngine._mainfile != nullptr)
    {
        _limitArea.zMax = emtEngine.getZMax() * 2;
    }
    if (_limitArea.zMax < 30.0f)
        _limitArea.zMax = 30.0f;
    // transForm
    updateTransMat();
    // KRKR-ns (2026-11-04): only (re)allocate when the current targets cannot
    // serve the new area.  Destroying and recreating two 11 MB surfaces on any
    // size wobble was the main source of heap fragmentation.
    if (_target != nullptr && _maskTarget != nullptr &&
        _targetWidth >= width && _targetHeight >= height)
        return;
    // 渲染目标（主体 + 蒙版）由 core/render 的 2D 渲染器统一管理
    krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
    if (renderer)
    {
        ReleaseDrawTargets();
        _target = CreateRenderTargetSafe(renderer, width, height);
        _maskTarget = _target ? CreateRenderTargetSafe(renderer, width, height) : nullptr;
        if (!_target || !_maskTarget)
        {
            // Allocation failed even after a compaction: drop what we got and
            // let draw() skip this frame rather than ending the session.
            if (_maskTarget)
            {
                DestroyRenderTargetSafe(renderer, _maskTarget, width, height);
                _maskTarget = nullptr;
            }
            if (_target)
            {
                DestroyRenderTargetSafe(renderer, _target, width, height);
                _target = nullptr;
            }
            return;
        }
        _targetWidth = width;
        _targetHeight = height;
    }
}

} // namespace emoteplayer
