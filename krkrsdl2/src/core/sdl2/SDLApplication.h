/* SPDX-License-Identifier: MIT */
/* Copyright (c) Kirikiri SDL2 Developers */

#pragma once
#include "tjsCommHead.h"

struct SDL_Renderer;

extern void krkrsdl2_pre_init_platform(void);
extern void krkrsdl2_set_args(int argc, tjs_char **argv);
extern void krkrsdl2_convert_set_args(int argc, char **argv);
extern bool krkrsdl2_init_platform(void);
extern void krkrsdl2_run_main_loop(void);
extern void krkrsdl2_cleanup(void);

// Borrow the active window's renderer for short-lived, main-thread plugin
// rendering. Ownership remains with TVPWindowWindow.
extern SDL_Renderer *TVPGetPrimarySDLRenderer(void);
