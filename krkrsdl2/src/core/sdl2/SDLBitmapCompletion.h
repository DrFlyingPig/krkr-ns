/* SPDX-License-Identifier: MIT */
/* Copyright (c) Kirikiri SDL2 Developers */

#pragma once
#include "tjsCommHead.h"
#include "WindowImpl.h"
#include <SDL.h>
#ifdef __SWITCH__
#include "SDLBitmapCompositor.h"
#endif

class TVPSDLBitmapCompletion
{
#ifdef __SWITCH__
	krkrns::ResidentFrame residentFrame_;
	bool surfaceStale_ = false;
	bool gpuCycleAllowed_ = true;
	bool forceCPUUpload_ = false;
	TVPSDLBitmapCompletion* next_ = nullptr;
	TVPSDLBitmapCompletion* previous_ = nullptr;
	static TVPSDLBitmapCompletion* first_;
#endif
public:
	SDL_Surface* surface;
	tTVPRect update_rect;
	TVPSDLBitmapCompletion();
	~TVPSDLBitmapCompletion();
	void BeginCompletion();
	bool SyncSurface();
	void UseCPUSurface();
	SDL_Texture* ResidentTexture() const;
	bool ForceCPUUpload() const;
	void DidCPUUpload();
	static bool SyncAllSurfaces();
	static void DiscardAllDerivedFrames();
#ifdef __SWITCH__
	bool NotifyGPUCompleted(iTVPLayerManager* manager, tjs_int x, tjs_int y,
		const krkrns::BitmapView& bitmap, const tTVPRect& cliprect,
		tTVPLayerType type, tjs_int opacity);
#endif
	void NotifyBitmapCompleted(iTVPLayerManager * manager,
		tjs_int x, tjs_int y, const void * bits, const class BitmapInfomation * bmpinfo,
		const tTVPRect &cliprect, tTVPLayerType type, tjs_int opacity);
};
