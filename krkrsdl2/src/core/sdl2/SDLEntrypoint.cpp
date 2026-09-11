/* SPDX-License-Identifier: MIT */
/* Copyright (c) Kirikiri SDL2 Developers */

#include "SDLApplication.h"
#include "SysInitImpl.h"
#ifdef USE_SDL_MAIN
#include <SDL_main.h>
#endif
#ifdef __SWITCH__
extern void krkrsdl2_set_own_path(const char* path);
#endif

#if defined(USE_SDL_MAIN)
extern "C" int SDL_main(int argc, char **argv)
#elif defined(_WIN32) && defined(_UNICODE)
extern "C" int wmain(int argc, wchar_t **argv)
#else
extern "C" int main(int argc, char **argv)
#endif
{
	try
	{
		krkrsdl2_pre_init_platform();

#ifdef __SWITCH__
		// Remember where this NRO was launched from: ending a game restarts the
		// application (envSetNextLoad) so every session starts from a pristine
		// engine and the launcher is the first screen again.  Call this even
		// when the host supplies no argv, so the log always records whether a
		// restart is possible (knowing the NRO path and next-load support).
		krkrsdl2_set_own_path((argc > 0 && argv) ? argv[0] : NULL);
#endif

#if defined(_WIN32) && defined(_UNICODE)
		krkrsdl2_set_args(argc, argv);
#else
		krkrsdl2_convert_set_args(argc, argv);
#endif

		if (krkrsdl2_init_platform())
		{
			TVPTerminateCode = 0;
			return TVPTerminateCode;
		}

		krkrsdl2_run_main_loop();

#ifndef __EMSCRIPTEN__
		krkrsdl2_cleanup();
#endif
	}
	catch (...)
	{
		TVPTerminateCode = 2;
		return TVPTerminateCode;
	}
#ifdef _WIN32
	::TerminateProcess(::GetCurrentProcess(), (UINT)TVPTerminateCode);
#endif
	return TVPTerminateCode;
}

