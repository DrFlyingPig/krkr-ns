/* SPDX-License-Identifier: MIT */
/* Copyright (c) Kirikiri SDL2 Developers */

#include "SDLBitmapCompletion.h"
#include "SDLBitmapBridge.h"
#include "DebugIntf.h"
#include "KrkrNSLog.h"
#include "KrkrNSProf.h"
#include "GLCompositeBridge.h"
#include <SDL.h>

TVPSDLBitmapCompletion::TVPSDLBitmapCompletion()
{
	surface = nullptr;
	update_rect.clear();
}

void TVPSDLBitmapCompletion::NotifyBitmapCompleted(iTVPLayerManager * manager,
	tjs_int x, tjs_int y, const void * bits, const class BitmapInfomation * bmpinfo,
	const tTVPRect &cliprect, tTVPLayerType type, tjs_int opacity)
{
	if (!surface || !manager || !bits || !bmpinfo)
	{
		static void* lastNullSurface = (void*)-1;
		if (lastNullSurface != (void*)surface)
		{
			lastNullSurface = (void*)surface;
			KRKRNS_LOG("[comp] skip null: surface=%p manager=%p bits=%p bmpinfo=%p",
				(void*)surface, (void*)manager, bits, (void*)bmpinfo);
		}
		return;
	}
	const TVPBITMAPINFO *bitmapinfo = bmpinfo->GetBITMAPINFO();
	tjs_int w = 0;
	tjs_int h = 0;
	if (!manager->GetPrimaryLayerSize(w, h))
	{
		return;
	}
	if (x < 0 || y < 0 || int64_t(x) + cliprect.get_width() > w ||
		int64_t(y) + cliprect.get_height() > h || bitmapinfo->bmiHeader.biBitCount != 32)
	{
		static void* lastBoundsSurface = (void*)-1;
		if (lastBoundsSurface != (void*)surface)
		{
			lastBoundsSurface = (void*)surface;
			KRKRNS_LOG("[comp] skip bounds: at %d,%d clip %dx%d primary %dx%d bpp=%d surface=%p %dx%d",
				x, y, cliprect.get_width(), cliprect.get_height(), w, h,
				(int)bitmapinfo->bmiHeader.biBitCount, (void*)surface,
				surface ? surface->w : 0, surface ? surface->h : 0);
		}
		return;
	}
	const SDL_Rect source = {cliprect.left, cliprect.top,
		cliprect.get_width(), cliprect.get_height()};
	SDL_Rect copied;
#ifdef __SWITCH__
	const Uint64 scStart = SDL_GetPerformanceCounter();
#endif
	if (TVPCopyBitmapToSurface(surface, bits, bitmapinfo->bmiHeader.biWidth,
		bitmapinfo->bmiHeader.biHeight, source, x, y, copied))
	{
		static void* lastOkSurface = (void*)-1;
		static unsigned okCount = 0;
		if (lastOkSurface != (void*)surface)
		{
			lastOkSurface = (void*)surface;
			okCount = 0;
		}
		if ((okCount++ % 30u) == 0u)
		{
			// KRKR-ns diagnostic: is the composed source black, or did the copy
			// lose it?  Sample both sides coarsely (raw word scan, flip-agnostic).
			const uint32_t* srcWords = static_cast<const uint32_t*>(bits);
			const size_t srcWordsN =
				size_t(bitmapinfo->bmiHeader.biWidth) *
				size_t(bitmapinfo->bmiHeader.biHeight < 0 ? -bitmapinfo->bmiHeader.biHeight
				                                          : bitmapinfo->bmiHeader.biHeight);
			size_t srcLit = 0;
			for (size_t i = 0; i < srcWordsN; i += 997)
			{
				if (srcWords[i] & 0x00ffffffu) ++srcLit;
			}
			const uint32_t* dstWords = static_cast<const uint32_t*>(surface->pixels);
			const size_t dstWordsN = size_t(surface->w) * size_t(surface->h);
			size_t dstLit = 0;
			for (size_t i = 0; i < dstWordsN; i += 997)
			{
				if (dstWords[i] & 0x00ffffffu) ++dstLit;
			}
			KRKRNS_LOG("[comp] copy ok t=%u: surface=%p %dx%d at %d,%d clip %dx%d -> %dx%d srcLit=%zu/%zu dstLit=%zu/%zu",
				(unsigned)SDL_GetTicks(),
				(void*)surface, surface->w, surface->h, x, y,
				cliprect.get_width(), cliprect.get_height(), copied.w, copied.h,
				srcLit, srcWordsN / 997 + 1, dstLit, dstWordsN / 997 + 1);
		}
#ifdef __SWITCH__
		krkrsdl2_prof_accum_surface_copy(
			(double)(SDL_GetPerformanceCounter() - scStart) * 1000.0 /
			(double)SDL_GetPerformanceFrequency());
#endif
#ifdef __SWITCH__
		// One-shot diagnostic: composition content clipped by the surface
		// means the surface/texture is smaller than the primary layer —
		// the classic stale-scene-fragment / partial-scene generator.
		static bool clipLogged = false;
		if (!clipLogged && (copied.w < source.w || copied.h < source.h))
		{
			clipLogged = true;
			KRKRNS_LOG("[layer] COMPOSITION CLIPPED: want %dx%d at %d,%d got %dx%d (surface %dx%d)",
				source.w, source.h, x, y, copied.w, copied.h,
				surface->w, surface->h);
		}
#endif
		tTVPRect r;
		r.set_offsets(copied.x, copied.y);
		r.set_size(copied.w, copied.h);
		update_rect.do_union(r);
	}
#ifdef __SWITCH__
	else
	{
		static bool dropLogged = false;
		if (!dropLogged)
		{
			dropLogged = true;
			KRKRNS_LOG("[layer] COMPOSITION DROPPED: cliprect %d,%d %dx%d bmp %dx%d primary check w=%d h=%d surface=%dx%d",
				cliprect.left, cliprect.top, cliprect.get_width(), cliprect.get_height(),
				bitmapinfo->bmiHeader.biWidth, bitmapinfo->bmiHeader.biHeight, w, h,
				surface->w, surface->h);
		}
	}
#endif
}

TVPSDLBitmapCompletion::~TVPSDLBitmapCompletion()
{
}
