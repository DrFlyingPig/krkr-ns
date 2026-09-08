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
		return;
	const SDL_Rect source = {cliprect.left, cliprect.top,
		cliprect.get_width(), cliprect.get_height()};
	SDL_Rect copied;
#ifdef __SWITCH__
	const Uint64 scStart = SDL_GetPerformanceCounter();
#endif
	if (TVPCopyBitmapToSurface(surface, bits, bitmapinfo->bmiHeader.biWidth,
		bitmapinfo->bmiHeader.biHeight, source, x, y, copied))
	{
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
