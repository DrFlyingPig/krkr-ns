/* SPDX-License-Identifier: MIT */
/* Copyright (c) Kirikiri SDL2 Developers */

#include "tjsCommHead.h"
#include "tjsDebug.h"
#include <thread>
#include "WindowImpl.h"
#include "LayerIntf.h"
#include "DrawDevice.h"
#include "VirtualKey.h"
#include "Application.h"
#include "SystemImpl.h"
#include "TVPWindow.h"
#include "SysInitIntf.h"
#include "SysInitImpl.h"
#include "CharacterSet.h"
#include "WaveImpl.h"
#include "TimerThread.h"
#include "MsgIntf.h"
#include "DebugIntf.h"
#include "tjsArray.h"
#include "StorageIntf.h"
#include "XP3Archive.h"
#include "SevenZipArchive.h"
#include "SDLBitmapCompletion.h"
#include "SDLBitmapBridge.h"
#include "ScriptMgnIntf.h"
#include "SystemControl.h"
#include "PluginImpl.h"
#ifdef KRKRZ_ENABLE_CANVAS
#include "OpenGLScreenSDL2.h"
#endif
#ifdef _WIN32
#include <SDL_syswm.h>
#endif
#include <SDL.h>
#ifdef _WIN32
#include <shellapi.h>
#include <stdlib.h>
#endif

#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#ifdef __SWITCH__
#include <switch.h>
#include <sys/stat.h>
#include <dirent.h>
#include <algorithm>
#include <vector>
#endif

#include "KrkrNSLog.h"
#include "KrkrNSProf.h"
#include "ThreadIntf.h"
#include "GLCompositeBridge.h"

#ifdef __SWITCH__
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
void krkrsdl2_logf_impl(const char *fmt, ...)
{
	static int logfd = -1;
	if (logfd < 0)
		logfd = open("sdmc:/krkrsdl2_debug.log", O_WRONLY | O_CREAT | O_TRUNC);
	if (logfd < 0) return;
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0) return;
	if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
	buf[n++] = '\n';
	write(logfd, buf, n);
}

/* ---- Phase 0 per-frame profiler (see KrkrNSProf.h) ---- */
static uint64_t krkrsdl2_prof_freq()
{
	static uint64_t freq = 0;
	if (!freq) freq = SDL_GetPerformanceFrequency();
	return freq;
}

static struct {
	double seg[4];          // main-loop segments, ms
	double compose_ms;      // engine software raster (CompleteForWindow)
	double surf_copy_ms;    // compose -> SDL surface memcpy
	double upload_ms;       // SDL_UpdateTexture
	double present_ms;      // RenderClear + RenderCopy + RenderPresent
	unsigned long long upload_bytes;
	unsigned loop_frames;   // Application::Run iterations
	unsigned update_frames; // frames with a graphic update (emit cadence)
} g_krkrns_prof;

void krkrsdl2_prof_seg(int which, double ms)
{
	if (which >= 0 && which < 4) g_krkrns_prof.seg[which] += ms;
}

void krkrsdl2_prof_accum_frame()
{
	g_krkrns_prof.loop_frames++;
}

static uint64_t g_compose_start = 0;
void krkrsdl2_prof_begin_compose()
{
	g_compose_start = SDL_GetPerformanceCounter();
}

void krkrsdl2_prof_end_compose()
{
	if (!g_compose_start) return;
	const uint64_t now = SDL_GetPerformanceCounter();
	g_krkrns_prof.compose_ms +=
		(double)(now - g_compose_start) * 1000.0 / (double)krkrsdl2_prof_freq();
	g_compose_start = 0;
}

void krkrsdl2_prof_accum_surface_copy(double ms)
{
	g_krkrns_prof.surf_copy_ms += ms;
}

void krkrsdl2_prof_accum_upload(double ms, unsigned bytes)
{
	g_krkrns_prof.upload_ms += ms;
	g_krkrns_prof.upload_bytes += bytes;
	g_krkrns_prof.update_frames++;
}

void krkrsdl2_prof_accum_present(double ms)
{
	g_krkrns_prof.present_ms += ms;
}

void krkrsdl2_prof_emit_and_reset(double interval_ms)
{
	const unsigned n = g_krkrns_prof.update_frames ? g_krkrns_prof.update_frames : 1;
	const unsigned loop = g_krkrns_prof.loop_frames ? g_krkrns_prof.loop_frames : 1;
	static unsigned prevPoolB = 0, prevPoolBig = 0;
	const unsigned poolB = krkrsdl2_pool_begins();
	const unsigned poolBig = krkrsdl2_pool_bigbegins();
	KRKRNS_LOG("[prof] updates=%u loops=%u total=%.1fms/f fps=%.1f | seg ev=%.2f disp=%.2f tick=%.2f wait=%.2f | compose=%.2f surfcopy=%.2f upload=%.2f present=%.2f | upMB/f=%.1f poolB=%.0f bigB=%.0f", 
		g_krkrns_prof.update_frames, g_krkrns_prof.loop_frames, interval_ms,
		interval_ms > 0.0 ? 1000.0 / interval_ms : 0.0,
		g_krkrns_prof.seg[0] / loop, g_krkrns_prof.seg[1] / loop,
		g_krkrns_prof.seg[2] / loop, g_krkrns_prof.seg[3] / loop,
		g_krkrns_prof.compose_ms / n, g_krkrns_prof.surf_copy_ms / n,
		g_krkrns_prof.upload_ms / n, g_krkrns_prof.present_ms / n,
		(double)g_krkrns_prof.upload_bytes / (1024.0 * 1024.0 * (double)n),
		(double)(poolB - prevPoolB) / n, (double)(poolBig - prevPoolBig) / n);
	prevPoolB = poolB;
	prevPoolBig = poolBig;
	g_krkrns_prof = ((decltype(g_krkrns_prof)){});
}
#endif

#ifdef __EMSCRIPTEN__
EM_JS_DEPS(main, "$FS,$IDBFS");
#endif

#if defined(__IPHONEOS__) || defined(__ANDROID__) || defined(__EMSCRIPTEN__) || defined(__vita__) || defined(__SWITCH__)
#define KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
#endif

#if defined(__linux__)
// By specification of SDL_RenderPresent, the backbuffer should be
// considered invalidated after each call. This is required for
// some renderers to be enabled.
#define KRKRSDL2_RENDERER_FULL_UPDATES
#endif

extern void TVPLoadMessage();

class TVPWindowWindow;
static TVPWindowWindow *_lastWindowWindow, *_currentWindowWindow;
static SDL_GameController **sdl_controllers = nullptr;
static int sdl_controller_num = 0;

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
static void process_events();
#else
static bool process_events();
#endif

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
static int sdl_event_watch(void *userdata, SDL_Event *in_event);
#endif

static void refresh_controllers()
{
#if defined(__IPHONEOS__) || defined(__ANDROID__)
	// TODO: check why invalid pointers get set in SDL's controller subsystem which causes segfault
	{
		return;
	}
#endif
	if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
	{
		SDL_Init(SDL_INIT_GAMECONTROLLER);
	}
	if (sdl_controller_num && sdl_controllers)
	{
		for (int i = 0; i < sdl_controller_num; i += 1)
		{
			if (sdl_controllers[i])
			{
				SDL_GameControllerClose(sdl_controllers[i]);
				sdl_controllers[i] = nullptr;
			}
		}
		sdl_controller_num = 0;
		SDL_free(sdl_controllers);
		sdl_controllers = nullptr;
	}
	sdl_controller_num = SDL_NumJoysticks();
	if (sdl_controller_num)
	{
		sdl_controllers = (SDL_GameController**)SDL_malloc(sizeof(SDL_GameController*) * sdl_controller_num);
		if (!sdl_controllers)
		{
			sdl_controller_num = 0;
			TVPAddLog(ttstr("Could not allocate memory to store SDL controller information"));
			return;
		}
		for (int i = 0; i < sdl_controller_num; i += 1)
		{
			if (SDL_IsGameController(i))
			{
				sdl_controllers[i] = SDL_GameControllerOpen(i);
				if (!sdl_controllers[i])
				{
					TVPAddLog(ttstr("Could not open controller: ") + ttstr(SDL_GetError()));
				}
			}
		}
	}
}

#ifdef __SWITCH__
// ---- KRKR-ns patch: gamepad -> mouse/keyboard event synthesis ----
// KAG games are driven by mouse clicks / Enter / Space / Escape key events,
// and the Switch SDL2 driver only reports controller buttons/axes. Poll the
// opened SDL_GameController(s) every frame and inject synthetic SDL mouse /
// keyboard events via SDL_PushEvent (public API), letting the existing event
// pipeline deliver them
// to TJS as ordinary input (plus the engine's own VK_PAD* events).
#define NS_GP_DEADZONE 8000
#define NS_GP_STICK_PX_PER_FRAME 14.0f
#define NS_GP_DPAD_STEP 12
#define NS_GP_DPAD_REPEAT_MS 150

enum {
	NS_GP_BTN_A = 0, NS_GP_BTN_B, NS_GP_BTN_X, NS_GP_BTN_Y,
	NS_GP_BTN_BACK, NS_GP_BTN_START, NS_GP_BTN_LSTICK, NS_GP_BTN_RSTICK,
	NS_GP_BTN_LSHOULDER, NS_GP_BTN_RSHOULDER,
	NS_GP_BTN_DPAD_UP, NS_GP_BTN_DPAD_DOWN, NS_GP_BTN_DPAD_LEFT, NS_GP_BTN_DPAD_RIGHT,
	NS_GP_BTN_COUNT
};
struct ns_gamepad_state_t
{
	bool left_down = false;  // synthetic left mouse button held
	bool right_down = false; // synthetic right mouse button held
	int mouse_x = -1;        // cursor in window pixel coords
	int mouse_y = -1;
	Uint32 dpad_repeat_until[4] = { 0, 0, 0, 0 };
};
static ns_gamepad_state_t ns_gp;
static Uint16 ns_gp_prev_buttons = 0;

static Uint16 ns_gp_read_buttons(SDL_GameController *c)
{
	static const SDL_GameControllerButton map[NS_GP_BTN_COUNT] = {
		SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X,
		SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
		SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
		SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
		SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
		SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT
	};
	Uint16 bits = 0;
	for (int i = 0; i < NS_GP_BTN_COUNT; i += 1)
	{
		if (SDL_GameControllerGetButton(c, map[i]))
		{
			bits |= (Uint16)(1 << i);
		}
	}
	return bits;
}

static void ns_gp_push_mouse_motion(SDL_Window *window, int x, int y)
{
	SDL_Event ev;
	SDL_zero(ev);
	ev.type = SDL_MOUSEMOTION;
	ev.motion.windowID = SDL_GetWindowID(window);
	ev.motion.x = x;
	ev.motion.y = y;
	ev.motion.xrel = 0;
	ev.motion.yrel = 0;
	ev.motion.state = (Uint32)((ns_gp.left_down ? SDL_BUTTON(SDL_BUTTON_LEFT) : 0) |
	                           (ns_gp.right_down ? SDL_BUTTON(SDL_BUTTON_RIGHT) : 0));
	SDL_PushEvent(&ev);
}

static void ns_gp_push_mouse_button(SDL_Window *window, Uint8 state, Uint8 button)
{
	SDL_Event ev;
	SDL_zero(ev);
	ev.type = (state == SDL_PRESSED) ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	ev.button.windowID = SDL_GetWindowID(window);
	ev.button.state = state;
	ev.button.button = button;
	ev.button.x = ns_gp.mouse_x;
	ev.button.y = ns_gp.mouse_y;
	ev.button.clicks = 1;
	SDL_PushEvent(&ev);
}

static void ns_gp_push_key(SDL_Window *window, Uint8 state, SDL_Scancode scancode)
{
	SDL_Event ev;
	SDL_zero(ev);
	ev.type = (state == SDL_PRESSED) ? SDL_KEYDOWN : SDL_KEYUP;
	ev.key.windowID = SDL_GetWindowID(window);
	ev.key.state = state;
	ev.key.repeat = 0;
	ev.key.keysym.scancode = scancode;
	ev.key.keysym.sym = SDL_GetKeyFromScancode(scancode);
	ev.key.keysym.mod = KMOD_NONE;
	SDL_PushEvent(&ev);
}

static void ns_gp_push_mouse_wheel(SDL_Window *window, Sint32 y)
{
	SDL_Event ev;
	SDL_zero(ev);
	ev.type = SDL_MOUSEWHEEL;
	ev.wheel.windowID = SDL_GetWindowID(window);
	ev.wheel.x = 0;
	ev.wheel.y = y;
	ev.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
	SDL_PushEvent(&ev);
}
#endif

static Uint8 vk_key_to_sdl_gamecontrollerbutton(tjs_uint key)
{
	switch (key)
	{
#ifdef __SWITCH__
		// SDL uses Xbox face-button positions on Switch.  Keep Kirikiri's
		// PAD1..PAD4 semantic order as Nintendo A, B, X, Y.
		case VK_PAD1: return SDL_CONTROLLER_BUTTON_B;
		case VK_PAD2: return SDL_CONTROLLER_BUTTON_A;
		case VK_PAD3: return SDL_CONTROLLER_BUTTON_Y;
		case VK_PAD4: return SDL_CONTROLLER_BUTTON_X;
#else
		case VK_PAD1: return SDL_CONTROLLER_BUTTON_A;
		case VK_PAD2: return SDL_CONTROLLER_BUTTON_B;
		case VK_PAD3: return SDL_CONTROLLER_BUTTON_X;
		case VK_PAD4: return SDL_CONTROLLER_BUTTON_Y;
#endif
		case VK_PAD7: return SDL_CONTROLLER_BUTTON_BACK;
		case VK_PAD8: return SDL_CONTROLLER_BUTTON_START;
		case VK_PAD9: return SDL_CONTROLLER_BUTTON_LEFTSTICK;
		case VK_PAD10: return SDL_CONTROLLER_BUTTON_RIGHTSTICK;
		case VK_PAD5: return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
		case VK_PAD6: return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
		case VK_PADUP: return SDL_CONTROLLER_BUTTON_DPAD_UP;
		case VK_PADDOWN: return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
		case VK_PADLEFT: return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
		case VK_PADRIGHT: return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
		default: return 0;
	}
}

static tjs_uint sdl_gamecontrollerbutton_to_vk_key(Uint8 key)
{
	switch (key)
	{
#ifdef __SWITCH__
		case SDL_CONTROLLER_BUTTON_A: return VK_PAD2;
		case SDL_CONTROLLER_BUTTON_B: return VK_PAD1;
		case SDL_CONTROLLER_BUTTON_X: return VK_PAD4;
		case SDL_CONTROLLER_BUTTON_Y: return VK_PAD3;
#else
		case SDL_CONTROLLER_BUTTON_A: return VK_PAD1;
		case SDL_CONTROLLER_BUTTON_B: return VK_PAD2;
		case SDL_CONTROLLER_BUTTON_X: return VK_PAD3;
		case SDL_CONTROLLER_BUTTON_Y: return VK_PAD4;
#endif
		case SDL_CONTROLLER_BUTTON_BACK: return VK_PAD7;
		case SDL_CONTROLLER_BUTTON_START: return VK_PAD8;
		case SDL_CONTROLLER_BUTTON_LEFTSTICK: return VK_PAD9;
		case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return VK_PAD10;
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return VK_PAD5;
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return VK_PAD6;
		case SDL_CONTROLLER_BUTTON_DPAD_UP: return VK_PADUP;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return VK_PADDOWN;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return VK_PADLEFT;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return VK_PADRIGHT;
		default: return 0;
	}
}

#ifdef MK_SHIFT
#undef MK_SHIFT
#endif
#ifdef MK_CONTROL
#undef MK_CONTROL
#endif
#ifdef MK_ALT
#undef MK_ALT
#endif
#define MK_SHIFT 4
#define MK_CONTROL 8
#define MK_ALT (0x20)

static SDL_Cursor *sdl_system_cursors[SDL_NUM_SYSTEM_CURSORS] = {0};

static SDL_Keycode vk_key_to_sdl_key(tjs_uint key)
{
	// This is generated using regex find replace
	switch (key)
	{
		case VK_BACK: return SDLK_BACKSPACE;
		case VK_TAB: return SDLK_TAB;
		case VK_CLEAR: return SDLK_CLEAR;
		case VK_RETURN: return SDLK_RETURN;
		case VK_PAUSE: return SDLK_PAUSE;
		case VK_CAPITAL: return SDLK_CAPSLOCK;
		case VK_ESCAPE: return SDLK_ESCAPE;
		case VK_SPACE: return SDLK_SPACE;
		case VK_PRIOR: return SDLK_PAGEUP;
		case VK_NEXT: return SDLK_PAGEDOWN;
		case VK_END: return SDLK_END;
		case VK_HOME: return SDLK_HOME;
		case VK_LEFT: return SDLK_LEFT;
		case VK_UP: return SDLK_UP;
		case VK_RIGHT: return SDLK_RIGHT;
		case VK_DOWN: return SDLK_DOWN;
		case VK_SELECT: return SDLK_SELECT;
		case VK_EXECUTE: return SDLK_EXECUTE;
		case VK_SNAPSHOT: return SDLK_PRINTSCREEN;
		case VK_INSERT: return SDLK_INSERT;
		case VK_DELETE: return SDLK_DELETE;
		case VK_HELP: return SDLK_HELP;
		case VK_0: return SDLK_0;
		case VK_1: return SDLK_1;
		case VK_2: return SDLK_2;
		case VK_3: return SDLK_3;
		case VK_4: return SDLK_4;
		case VK_5: return SDLK_5;
		case VK_6: return SDLK_6;
		case VK_7: return SDLK_7;
		case VK_8: return SDLK_8;
		case VK_9: return SDLK_9;
		case VK_A: return SDLK_a;
		case VK_B: return SDLK_b;
		case VK_C: return SDLK_c;
		case VK_D: return SDLK_d;
		case VK_E: return SDLK_e;
		case VK_F: return SDLK_f;
		case VK_G: return SDLK_g;
		case VK_H: return SDLK_h;
		case VK_I: return SDLK_i;
		case VK_J: return SDLK_j;
		case VK_K: return SDLK_k;
		case VK_L: return SDLK_l;
		case VK_M: return SDLK_m;
		case VK_N: return SDLK_n;
		case VK_O: return SDLK_o;
		case VK_P: return SDLK_p;
		case VK_Q: return SDLK_q;
		case VK_R: return SDLK_r;
		case VK_S: return SDLK_s;
		case VK_T: return SDLK_t;
		case VK_U: return SDLK_u;
		case VK_V: return SDLK_v;
		case VK_W: return SDLK_w;
		case VK_X: return SDLK_x;
		case VK_Y: return SDLK_y;
		case VK_Z: return SDLK_z;
		case VK_LWIN: return SDLK_LGUI;
		case VK_RWIN: return SDLK_RGUI;
		case VK_SLEEP: return SDLK_SLEEP;
		case VK_NUMPAD0: return SDLK_KP_0;
		case VK_NUMPAD1: return SDLK_KP_1;
		case VK_NUMPAD2: return SDLK_KP_2;
		case VK_NUMPAD3: return SDLK_KP_3;
		case VK_NUMPAD4: return SDLK_KP_4;
		case VK_NUMPAD5: return SDLK_KP_5;
		case VK_NUMPAD6: return SDLK_KP_6;
		case VK_NUMPAD7: return SDLK_KP_7;
		case VK_NUMPAD8: return SDLK_KP_8;
		case VK_NUMPAD9: return SDLK_KP_9;
		case VK_MULTIPLY: return SDLK_KP_MULTIPLY;
		case VK_ADD: return SDLK_KP_PLUS;
		case VK_SUBTRACT: return SDLK_KP_MINUS;
		case VK_DECIMAL: return SDLK_KP_PERIOD;
		case VK_DIVIDE: return SDLK_KP_DIVIDE;
		case VK_F1: return SDLK_F1;
		case VK_F2: return SDLK_F2;
		case VK_F3: return SDLK_F3;
		case VK_F4: return SDLK_F4;
		case VK_F5: return SDLK_F5;
		case VK_F6: return SDLK_F6;
		case VK_F7: return SDLK_F7;
		case VK_F8: return SDLK_F8;
		case VK_F9: return SDLK_F9;
		case VK_F10: return SDLK_F10;
		case VK_F11: return SDLK_F11;
		case VK_F12: return SDLK_F12;
		case VK_F13: return SDLK_F13;
		case VK_F14: return SDLK_F14;
		case VK_F15: return SDLK_F15;
		case VK_F16: return SDLK_F16;
		case VK_F17: return SDLK_F17;
		case VK_F18: return SDLK_F18;
		case VK_F19: return SDLK_F19;
		case VK_F20: return SDLK_F20;
		case VK_F21: return SDLK_F21;
		case VK_F22: return SDLK_F22;
		case VK_F23: return SDLK_F23;
		case VK_F24: return SDLK_F24;
		case VK_NUMLOCK: return SDLK_NUMLOCKCLEAR;
		case VK_SCROLL: return SDLK_SCROLLLOCK;
		case VK_LSHIFT: return SDLK_LSHIFT;
		case VK_RSHIFT: return SDLK_RSHIFT;
		case VK_LCONTROL: return SDLK_LCTRL;
		case VK_RCONTROL: return SDLK_RCTRL;
		case VK_LMENU: return SDLK_LALT;
		case VK_RMENU: return SDLK_RALT;
		case VK_BROWSER_BACK: return SDLK_AC_BACK;
		case VK_BROWSER_FORWARD: return SDLK_AC_FORWARD;
		case VK_BROWSER_REFRESH: return SDLK_AC_REFRESH;
		case VK_BROWSER_STOP: return SDLK_AC_STOP;
		case VK_BROWSER_SEARCH: return SDLK_AC_SEARCH;
		case VK_BROWSER_FAVORITES: return SDLK_AC_BOOKMARKS;
		case VK_BROWSER_HOME: return SDLK_AC_HOME;
		case VK_VOLUME_MUTE: return SDLK_MUTE;
		case VK_VOLUME_DOWN: return SDLK_VOLUMEDOWN;
		case VK_VOLUME_UP: return SDLK_VOLUMEUP;
		case VK_MEDIA_NEXT_TRACK: return SDLK_AUDIONEXT;
		case VK_MEDIA_PREV_TRACK: return SDLK_AUDIOPREV;
		case VK_MEDIA_STOP: return SDLK_AUDIOSTOP;
		case VK_MEDIA_PLAY_PAUSE: return SDLK_AUDIOPLAY;
		case VK_LAUNCH_MAIL: return SDLK_MAIL;
		case VK_LAUNCH_MEDIA_SELECT: return SDLK_MEDIASELECT;
		default: return 0;
	}
}

static tjs_uint sdl_key_to_vk_key(SDL_Keycode key)
{
	// This is generated using regex find replace
	switch (key)
	{
		case SDLK_BACKSPACE: return VK_BACK;
		case SDLK_TAB: return VK_TAB;
		case SDLK_CLEAR: return VK_CLEAR;
		case SDLK_RETURN: return VK_RETURN;
		case SDLK_PAUSE: return VK_PAUSE;
		case SDLK_CAPSLOCK: return VK_CAPITAL;
		case SDLK_ESCAPE: return VK_ESCAPE;
		case SDLK_SPACE: return VK_SPACE;
		case SDLK_PAGEUP: return VK_PRIOR;
		case SDLK_PAGEDOWN: return VK_NEXT;
		case SDLK_END: return VK_END;
		case SDLK_HOME: return VK_HOME;
		case SDLK_LEFT: return VK_LEFT;
		case SDLK_UP: return VK_UP;
		case SDLK_RIGHT: return VK_RIGHT;
		case SDLK_DOWN: return VK_DOWN;
		case SDLK_SELECT: return VK_SELECT;
		case SDLK_EXECUTE: return VK_EXECUTE;
		case SDLK_PRINTSCREEN: return VK_SNAPSHOT;
		case SDLK_INSERT: return VK_INSERT;
		case SDLK_DELETE: return VK_DELETE;
		case SDLK_HELP: return VK_HELP;
		case SDLK_0: return VK_0;
		case SDLK_1: return VK_1;
		case SDLK_2: return VK_2;
		case SDLK_3: return VK_3;
		case SDLK_4: return VK_4;
		case SDLK_5: return VK_5;
		case SDLK_6: return VK_6;
		case SDLK_7: return VK_7;
		case SDLK_8: return VK_8;
		case SDLK_9: return VK_9;
		case SDLK_a: return VK_A;
		case SDLK_b: return VK_B;
		case SDLK_c: return VK_C;
		case SDLK_d: return VK_D;
		case SDLK_e: return VK_E;
		case SDLK_f: return VK_F;
		case SDLK_g: return VK_G;
		case SDLK_h: return VK_H;
		case SDLK_i: return VK_I;
		case SDLK_j: return VK_J;
		case SDLK_k: return VK_K;
		case SDLK_l: return VK_L;
		case SDLK_m: return VK_M;
		case SDLK_n: return VK_N;
		case SDLK_o: return VK_O;
		case SDLK_p: return VK_P;
		case SDLK_q: return VK_Q;
		case SDLK_r: return VK_R;
		case SDLK_s: return VK_S;
		case SDLK_t: return VK_T;
		case SDLK_u: return VK_U;
		case SDLK_v: return VK_V;
		case SDLK_w: return VK_W;
		case SDLK_x: return VK_X;
		case SDLK_y: return VK_Y;
		case SDLK_z: return VK_Z;
		case SDLK_LGUI: return VK_LWIN;
		case SDLK_RGUI: return VK_RWIN;
		case SDLK_SLEEP: return VK_SLEEP;
		case SDLK_KP_0: return VK_NUMPAD0;
		case SDLK_KP_1: return VK_NUMPAD1;
		case SDLK_KP_2: return VK_NUMPAD2;
		case SDLK_KP_3: return VK_NUMPAD3;
		case SDLK_KP_4: return VK_NUMPAD4;
		case SDLK_KP_5: return VK_NUMPAD5;
		case SDLK_KP_6: return VK_NUMPAD6;
		case SDLK_KP_7: return VK_NUMPAD7;
		case SDLK_KP_8: return VK_NUMPAD8;
		case SDLK_KP_9: return VK_NUMPAD9;
		case SDLK_KP_MULTIPLY: return VK_MULTIPLY;
		case SDLK_KP_PLUS: return VK_ADD;
		case SDLK_KP_MINUS: return VK_SUBTRACT;
		case SDLK_KP_PERIOD: return VK_DECIMAL;
		case SDLK_KP_DIVIDE: return VK_DIVIDE;
		case SDLK_F1: return VK_F1;
		case SDLK_F2: return VK_F2;
		case SDLK_F3: return VK_F3;
		case SDLK_F4: return VK_F4;
		case SDLK_F5: return VK_F5;
		case SDLK_F6: return VK_F6;
		case SDLK_F7: return VK_F7;
		case SDLK_F8: return VK_F8;
		case SDLK_F9: return VK_F9;
		case SDLK_F10: return VK_F10;
		case SDLK_F11: return VK_F11;
		case SDLK_F12: return VK_F12;
		case SDLK_F13: return VK_F13;
		case SDLK_F14: return VK_F14;
		case SDLK_F15: return VK_F15;
		case SDLK_F16: return VK_F16;
		case SDLK_F17: return VK_F17;
		case SDLK_F18: return VK_F18;
		case SDLK_F19: return VK_F19;
		case SDLK_F20: return VK_F20;
		case SDLK_F21: return VK_F21;
		case SDLK_F22: return VK_F22;
		case SDLK_F23: return VK_F23;
		case SDLK_F24: return VK_F24;
		case SDLK_NUMLOCKCLEAR: return VK_NUMLOCK;
		case SDLK_SCROLLLOCK: return VK_SCROLL;
		case SDLK_LSHIFT: return VK_LSHIFT;
		case SDLK_RSHIFT: return VK_RSHIFT;
		case SDLK_LCTRL: return VK_LCONTROL;
		case SDLK_RCTRL: return VK_RCONTROL;
		case SDLK_MENU: return VK_LMENU;
		case SDLK_LALT: return VK_LMENU;
		case SDLK_RALT: return VK_RMENU;
		case SDLK_AC_BACK: return VK_BROWSER_BACK;
		case SDLK_AC_FORWARD: return VK_BROWSER_FORWARD;
		case SDLK_AC_REFRESH: return VK_BROWSER_REFRESH;
		case SDLK_AC_STOP: return VK_BROWSER_STOP;
		case SDLK_AC_SEARCH: return VK_BROWSER_SEARCH;
		case SDLK_AC_BOOKMARKS: return VK_BROWSER_FAVORITES;
		case SDLK_AC_HOME: return VK_BROWSER_HOME;
		case SDLK_MUTE: return VK_VOLUME_MUTE;
		case SDLK_VOLUMEDOWN: return VK_VOLUME_DOWN;
		case SDLK_VOLUMEUP: return VK_VOLUME_UP;
		case SDLK_AUDIONEXT: return VK_MEDIA_NEXT_TRACK;
		case SDLK_AUDIOPREV: return VK_MEDIA_PREV_TRACK;
		case SDLK_AUDIOSTOP: return VK_MEDIA_STOP;
		case SDLK_AUDIOPLAY: return VK_MEDIA_PLAY_PAUSE;
		case SDLK_MAIL: return VK_LAUNCH_MAIL;
		case SDLK_MEDIASELECT: return VK_LAUNCH_MEDIA_SELECT;
		default: return 0;
	}
}

static int GetShiftState()
{
	int s = 0;
	s |= TVPGetAsyncKeyState(VK_MENU) ? MK_ALT : 0;
	s |= TVPGetAsyncKeyState(VK_LMENU) ? MK_ALT : 0;
	s |= TVPGetAsyncKeyState(VK_RMENU) ? MK_ALT : 0;
	s |= TVPGetAsyncKeyState(VK_SHIFT) ? MK_SHIFT : 0;
	s |= TVPGetAsyncKeyState(VK_LSHIFT) ? MK_SHIFT : 0;
	s |= TVPGetAsyncKeyState(VK_RCONTROL) ? MK_SHIFT : 0;
	s |= TVPGetAsyncKeyState(VK_CONTROL) ? MK_CONTROL : 0;
	s |= TVPGetAsyncKeyState(VK_LCONTROL) ? MK_CONTROL : 0;
	s |= TVPGetAsyncKeyState(VK_RCONTROL) ? MK_CONTROL : 0;
	return s;
}
static int GetMouseButtonState()
{
	int s = 0;
	s |= TVPGetAsyncKeyState(VK_LBUTTON) ? ssLeft : 0;
	s |= TVPGetAsyncKeyState(VK_RBUTTON) ? ssRight : 0;
	s |= TVPGetAsyncKeyState(VK_MBUTTON) ? ssMiddle : 0;
	s |= TVPGetAsyncKeyState(VK_XBUTTON1) ? ssX1 : 0;
	s |= TVPGetAsyncKeyState(VK_XBUTTON2) ? ssX2 : 0;
	return s;
}

#ifdef _WIN32
struct tTVPMessageReceiverRecord
{
	tTVPWindowMessageReceiver Proc;
	const void *UserData;
	bool Deliver(tTVPWindowMessage *Message)
	{
		return Proc(const_cast<void*>(UserData), Message);
	}
};
#endif

class TVPWindowWindow : public TTVPWindowForm
{
protected:
	SDL_Window *window;

	TVPWindowWindow *_prevWindow;
	TVPWindowWindow *_nextWindow;
	SDL_Texture *texture;
	SDL_Renderer *renderer;
	SDL_Surface *surface;
#ifdef KRKRZ_ENABLE_CANVAS
	SDL_GLContext context;
#endif
	tTJSNI_Window *TJSNativeInstance;
	bool hasDrawn = false;
	bool isVisible = true;
	bool visibilityHasInitialized = false;
	bool needsGraphicUpdate = false;
	bool isBeingDeleted = false;
	bool cursorTemporaryHidden = false;
	char *imeCompositionStr;
	size_t imeCompositionCursor;
	size_t imeCompositionLen;
	size_t imeCompositionSelection;
	SDL_Rect attentionPointRect;
	iTJSDispatch2 *fileDropArray;
	tjs_int fileDropArrayCount;
	TVPSDLBitmapCompletion *bitmapCompletion;
#ifdef __SWITCH__
	/* KRKR-ns patch: set by the trace marker, captured at the next present */
	bool traceCapturePending = false;
#endif
#ifdef KRKRZ_ENABLE_CANVAS
	tTVPOpenGLScreen *openGlScreen;
#endif
	int lastMouseX;
	int lastMouseY;

#ifdef KRKRSDL2_ENABLE_ZOOM
	tTVPRect FullScreenDestRect;
	tTVPRect LastSentDrawDeviceDestRect;

	//-- layer position / size
	tjs_int LayerLeft = 0;
	tjs_int LayerTop = 0;
	tjs_int LayerWidth = 32;
	tjs_int LayerHeight = 32;
	tjs_int ZoomDenom = 1; // Zooming factor denominator (setting)
	tjs_int ZoomNumer = 1; // Zooming factor numerator (setting)
	tjs_int ActualZoomDenom = 1; // Zooming factor denominator (actual)
	tjs_int ActualZoomNumer = 1; // Zooming factor numerator (actual)
	tjs_int InnerWidth = 32;
	tjs_int InnerHeight = 32;
#endif
#ifdef _WIN32
	tObjectList<tTVPMessageReceiverRecord> WindowMessageReceivers;
#endif

public:
	TVPWindowWindow(tTJSNI_Window *w);
	virtual ~TVPWindowWindow();
	SDL_Renderer *GetSDLRenderer() const { return renderer; }
	/* Called from tTJSNI_Window */
	virtual void SetPaintBoxSize(tjs_int w, tjs_int h) override;
	void TranslateWindowToDrawArea(int &x, int &y);
	void TranslateDrawAreaToWindow(int &x, int &y);
#ifdef __SWITCH__
	/* KRKR-ns patch: gamepad -> mouse/keyboard synthesis, polled each frame */
	void switch_process_gamepad_input();
	/* KRKR-ns patch: mark the whole compose surface as damaged so the next
	   frame re-uploads every pixel. Any (re)created SDL_TEXTUREACCESS_STREAMING
	   texture starts as undefined/black content, and the engine only reports
	   incremental damage, so a lost upload would otherwise never be retried. */
	void InvalidateFullSurface();
#endif
	/* Called from tTJSNI_Window */
	virtual bool GetFormEnabled() override;
	/* Called from tTJSNI_Window */
	virtual void SetDefaultMouseCursor() override;
	/* Called from tTJSNI_Window */
	virtual void SetMouseCursor(tjs_int handle) override;
	/* Called from tTJSNI_Window */
	virtual void SetMouseCursorState(tTVPMouseCursorState mcs) override;
	/* Called from tTJSNI_Window */
	virtual tTVPMouseCursorState GetMouseCursorState() const override;
	void RestoreMouseCursor();
	/* Called from tTJSNI_Window */
	virtual void HideMouseCursor() override;
	/* Called from tTJSNI_Window */
	virtual void GetCursorPos(tjs_int &x, tjs_int &y) override;
	/* Called from tTJSNI_Window */
	virtual void SetCursorPos(tjs_int x, tjs_int y) override;
	/* Called from tTJSNI_Window */
	virtual void SetAttentionPoint(tjs_int left, tjs_int top, const struct tTVPFont * font) override;
	/* Called from tTJSNI_Window */
	virtual void BringToFront() override;
	/* Called from tTJSNI_Window */
	virtual void ShowWindowAsModal() override;
	/* Called from tTJSNI_Window */
	virtual bool GetVisible() override;
	/* Called from member SetVisibleFromScript */
	virtual void SetVisible(bool visible) override;
	/* Called from tTJSNI_Window */
	virtual void SetFullScreenMode(bool fullscreen) override;
	/* Called from tTJSNI_Window */
	virtual bool GetFullScreenMode() override;
	/* Called from tTJSNI_Window */
	virtual void SetBorderStyle(tTVPBorderStyle bs) override;
	/* Called from tTJSNI_Window */
	virtual tTVPBorderStyle GetBorderStyle() const override;
	/* Called from tTJSNI_Window */
	virtual tjs_string GetCaption() override;
	virtual void GetCaption(tjs_string & v) const override;
	/* Called from tTJSNI_Window */
	virtual void SetCaption(const tjs_string & v) override;
	/* Called from tTJSNI_Window */
	virtual void SetWidth(tjs_int w) override;
	/* Called from tTJSNI_Window */
	virtual void SetHeight(tjs_int h) override;
	/* Called from tTJSNI_Window */
	virtual void SetSize(tjs_int w, tjs_int h) override;
	virtual void GetSize(tjs_int &w, tjs_int &h) override;
	/* Called from tTJSNI_Window */
	virtual tjs_int GetWidth() const override;
	/* Called from tTJSNI_Window */
	virtual tjs_int GetHeight() const override;
	/* Called from tTJSNI_Window */
	virtual void SetMinWidth(tjs_int w) override;
	/* Called from tTJSNI_Window */
	virtual void SetMaxWidth(tjs_int w) override;
	/* Called from tTJSNI_Window */
	virtual void SetMinHeight(tjs_int h) override;
	/* Called from tTJSNI_Window */
	virtual void SetMaxHeight(tjs_int h) override;
	/* Called from tTJSNI_Window */
	virtual void SetMinSize(tjs_int w, tjs_int h) override;
	/* Called from tTJSNI_Window */
	virtual void SetMaxSize(tjs_int w, tjs_int h) override;
	/* Called from tTJSNI_Window */
	virtual tjs_int GetMinWidth() override;
	/* Called from tTJSNI_Window */
	virtual tjs_int GetMaxWidth() override;
	/* Called from tTJSNI_Window */
	virtual tjs_int GetMinHeight() override;
	/* Called from tTJSNI_Window */
	virtual tjs_int GetMaxHeight() override;
	/* Called from tTJSNI_Window */
	virtual tjs_int GetLeft() override;
	/* Called from tTJSNI_Window */
	virtual void SetLeft(tjs_int l) override;
	/* Called from tTJSNI_Window */
	virtual tjs_int GetTop() override;
	/* Called from tTJSNI_Window */
	virtual void SetTop(tjs_int t) override;
	/* Called from tTJSNI_Window */
	virtual void SetPosition(tjs_int l, tjs_int t) override;
	virtual TVPSDLBitmapCompletion *GetTVPSDLBitmapCompletion() override;
#ifdef KRKRZ_ENABLE_CANVAS
	virtual void SetOpenGLScreen(tTVPOpenGLScreen *s) override;
	virtual void SetSwapInterval(int interval) override;
	virtual void GetDrawableSize(tjs_int &w, tjs_int &h) override;
	virtual void Swap() override;
#endif
	virtual void Show() override;
	/* Called from tTJSNI_Window */
	virtual void TickBeat() override;
	/* Called from tTJSNI_Window */
	virtual void InvalidateClose() override;
	/* Called from tTJSNI_Window */
	virtual bool GetWindowActive() override;
	bool Closing = false;
	bool ProgramClosing = false;
	bool CanCloseWork = false;
	bool in_mode_ = false; // is modal
	int modal_result_ = 0;
	enum CloseAction
	{
		caNone,
		caHide,
		caFree,
		caMinimize
	};
	void OnClose(CloseAction& action);
	bool OnCloseQuery();
	/* Called from tTJSNI_Window */
	virtual void Close() override;
	/* Called from tTJSNI_Window */
	virtual void OnCloseQueryCalled(bool b) override;
	/* Called from tTJSNI_Window */
	virtual void SetImeMode(tTVPImeMode mode) override;
	/* Called from tTJSNI_Window */
	virtual void ResetImeMode() override;
	/* Called from tTJSNI_Window */
	virtual void UpdateWindow(tTVPUpdateType type) override;
	/* Called from tTJSNI_Window */
	virtual void InternalKeyDown(tjs_uint16 key, tjs_uint32 shift) override;
	/* Called from tTJSNI_Window */
	virtual void OnKeyUp(tjs_uint16 vk, int shift) override;
	/* Called from tTJSNI_Window */
	virtual void OnKeyPress(tjs_uint16 vk, int repeat, bool prevkeystate, bool convertkey) override;
	void UpdateActualZoom(void);
	void SetDrawDeviceDestRect(void);
	/* Called from tTJSNI_Window */
	virtual void SetZoom(tjs_int numer, tjs_int denom, bool set_logical = true) override;
	/* Called from tTJSNI_Window */
	virtual void SetZoomNumer(tjs_int n) override;
	/* Called from tTJSNI_Window */
	virtual tjs_int GetZoomNumer() const override;
	/* Called from tTJSNI_Window */
	virtual void SetZoomDenom(tjs_int d) override;
	/* Called from tTJSNI_Window */
	virtual tjs_int GetZoomDenom() const override;
	/* Called from tTJSNI_Window */
	virtual void SetInnerWidth(tjs_int v) override;
	/* Called from tTJSNI_Window */
	virtual void SetInnerHeight(tjs_int v) override;
	/* Called from tTJSNI_Window */
	virtual void SetInnerSize(tjs_int w, tjs_int h) override;
	/* Called from tTJSNI_Window */
	virtual tjs_int GetInnerWidth() override;
	/* Called from tTJSNI_Window */
	virtual tjs_int GetInnerHeight() override;
#ifdef _WIN32
	virtual void RegisterWindowMessageReceiver(tTVPWMRRegMode mode, void *proc, const void *userdata) override;
	bool InternalDeliverMessageToReceiver(tTVPWindowMessage &msg);
	virtual HWND GetHandle() const override;
#endif
	bool should_try_parent_window(SDL_Event event);
	void window_receive_event(SDL_Event event);
	bool window_receive_event_input(SDL_Event event);
};

SDL_Renderer *TVPGetPrimarySDLRenderer(void)
{
	TVPWindowWindow *window = _currentWindowWindow ? _currentWindowWindow : _lastWindowWindow;
	return window ? window->GetSDLRenderer() : nullptr;
}

TVPWindowWindow::TVPWindowWindow(tTJSNI_Window *w)
{
	this->window = nullptr;
	this->ResetImeMode();
	this->fileDropArray = nullptr;
	this->fileDropArrayCount = 0;
	this->lastMouseX = 0;
	this->lastMouseY = 0;
	this->_nextWindow = nullptr;
	this->_prevWindow = _lastWindowWindow;
	_lastWindowWindow = this;
	if (this->_prevWindow)
	{
		this->_prevWindow->_nextWindow = this;
	}
	if (!_currentWindowWindow)
	{
		_currentWindowWindow = this;
	}
	this->TJSNativeInstance = w;

	KRKRNS_LOG("[ns] TVPWindowWindow: before SDL_Init(VIDEO)");
	if (!SDL_WasInit(SDL_INIT_VIDEO))
	{
		if (SDL_Init(SDL_INIT_VIDEO) < 0)
		{
			TVPThrowExceptionMessage(TJS_W("Cannot initialize SDL video subsystem: %1"), ttstr(SDL_GetError()));
		}
		KRKRNS_LOG("[ns] SDL_Init(VIDEO) ok, SDL_GetError=[%s]", SDL_GetError() ? SDL_GetError() : "");
		refresh_controllers();
	}

	int new_window_x = SDL_WINDOWPOS_UNDEFINED;
	int new_window_y = SDL_WINDOWPOS_UNDEFINED;
	int new_window_w = 640;
	int new_window_h = 480;
#ifdef __SWITCH__
	// KRKR-ns patch: the switch SDL driver reports 1080p even in handheld,
	// which breaks the display path. Use the real screen resolution.
	if (appletGetOperationMode() == AppletOperationMode_Console)
	{
		new_window_w = 1920;
		new_window_h = 1080;
	}
	else
	{
		new_window_w = 1280;
		new_window_h = 720;
	}
	KRKRNS_LOG("[win] operation mode -> %dx%d", new_window_w, new_window_h);
#endif
	Uint32 window_flags = 0;

#ifdef SDL_HINT_RENDER_SCALE_QUALITY
	SDL_SetHintWithPriority(SDL_HINT_RENDER_SCALE_QUALITY, "2", SDL_HINT_DEFAULT);
#endif

#ifdef KRKRZ_ENABLE_CANVAS
	if (!TVPIsEnableDrawDevice())
	{
#ifdef SDL_HINT_OPENGL_ES_DRIVER
		SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
#endif
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
		SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
		window_flags |= SDL_WINDOW_OPENGL;
	}
#endif

#ifdef SDL_HINT_TOUCH_MOUSE_EVENTS
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
#endif
#ifdef SDL_HINT_MOUSE_TOUCH_EVENTS
	SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
#endif

#ifdef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	window_flags |= SDL_WINDOW_RESIZABLE;
	window_flags |= SDL_WINDOW_ALLOW_HIGHDPI;
#ifndef __EMSCRIPTEN__
	window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
#ifdef __SWITCH__
	// KRKR-ns patch (onsyuri-proven combo): FULLSCREEN_DESKTOP plus the
	// EXPLICIT display-matched size (1280x720 handheld / 1920x1080 docked),
	// like ONScripter-jh-Switch. With size 0x0 the switch driver picks the
	// 1080p mode even in handheld and nothing is visible.
#else
	new_window_w = 0;
	new_window_h = 0;
#endif
#endif

#ifdef _WIN32
	window_flags |= SDL_WINDOW_HIDDEN;
#endif

	KRKRNS_LOG("[ns] before SDL_CreateWindow (w=%d h=%d)", new_window_w, new_window_h);
	this->window = SDL_CreateWindow("krkrsdl2", new_window_x, new_window_y, new_window_w, new_window_h, window_flags);
	if (!this->window)
	{
		KRKRNS_LOG("[ns] SDL_CreateWindow FAILED: [%s]", SDL_GetError() ? SDL_GetError() : "");
		TVPThrowExceptionMessage(TJS_W("Cannot create SDL window: %1"), ttstr(SDL_GetError()));
	}
	KRKRNS_LOG("[ns] SDL_CreateWindow ok, video driver=[%s]", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
#if defined(__EMSCRIPTEN__) && defined(KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE)
	EmscriptenFullscreenStrategy strategy;
	SDL_memset(&strategy, 0, sizeof(strategy));
	strategy.scaleMode = EMSCRIPTEN_FULLSCREEN_CANVAS_SCALE_STDDEF;
	strategy.filteringMode = EMSCRIPTEN_FULLSCREEN_FILTERING_DEFAULT;
	emscripten_enter_soft_fullscreen("#canvas", &strategy);
#endif
#ifdef KRKRZ_ENABLE_CANVAS
	this->context = nullptr;
	if (!TVPIsEnableDrawDevice())
	{
		this->context = SDL_GL_CreateContext(this->window);
		if (!this->context)
		{
			TVPThrowExceptionMessage(TJS_W("Cannot create SDL context: %1"), ttstr(SDL_GetError()));
		}
		SDL_GL_MakeCurrent(this->window, this->context);
	}
#endif
	this->renderer = nullptr;
	this->bitmapCompletion = nullptr;
#ifdef KRKRZ_ENABLE_CANVAS
	this->openGlScreen = nullptr;
#endif
	this->surface = nullptr;
#ifdef KRKRZ_ENABLE_CANVAS
	if (TVPIsEnableDrawDevice())
#endif
	{
#if !defined(__EMSCRIPTEN__) || (defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__))
		KRKRNS_LOG("[ns] before SDL_CreateRenderer(ACCELERATED|VSYNC)");
		this->renderer = SDL_CreateRenderer(this->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
		if (!this->renderer)
		{
			KRKRNS_LOG("[ns] SDL_CreateRenderer failed: [%s] -> fallback to window surface", SDL_GetError() ? SDL_GetError() : "");
			TVPAddLog(ttstr("Cannot create SDL renderer: ") + ttstr(SDL_GetError()));
		}
else
			{
				SDL_RendererInfo renderer_info;
				SDL_zero(renderer_info);
				if (SDL_GetRendererInfo(this->renderer, &renderer_info) == 0)
				{
					KRKRNS_LOG("[ns] SDL renderer=%s flags=0x%x max=%dx%d",
						renderer_info.name ? renderer_info.name : "?",
						static_cast<unsigned>(renderer_info.flags),
						renderer_info.max_texture_width, renderer_info.max_texture_height);
				}
#ifdef __SWITCH__
				// Phase 0 baseline: one-time environment facts so every log
				// run is self-describing (CPU count, vsync, refresh rate).
				SDL_DisplayMode dm;
				SDL_zero(dm);
				SDL_GetCurrentDisplayMode(0, &dm);
				KRKRNS_LOG("[ns] env: cpus=%d drawT=%d vsync=%d refresh=%d pos=%dx%d",
					SDL_GetCPUCount(), TVPGetThreadNum(),
					(renderer_info.flags & SDL_RENDERER_PRESENTVSYNC) != 0,
					dm.refresh_rate, new_window_w, new_window_h);
#endif
			}
#endif

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
		// move the event watch to after the SDL_RendererEventWatch to ensure transformed values are received
		SDL_DelEventWatch(sdl_event_watch, nullptr);
		SDL_AddEventWatch(sdl_event_watch, nullptr);
#endif

		this->bitmapCompletion = new TVPSDLBitmapCompletion();
		if (!this->renderer)
		{
			this->surface = SDL_GetWindowSurface(this->window);
			if (!this->surface)
			{
				TVPAddLog(ttstr("Cannot get surface from SDL window: ") + ttstr(SDL_GetError()));
			}
			this->bitmapCompletion->surface = this->surface;
		}
		if (!this->renderer && !this->surface)
		{
			TVPThrowExceptionMessage(TJS_W("Cannot get surface or renderer from SDL window"));
		}
		this->texture = nullptr;
#ifdef __SWITCH__
		// KRKR-ns patch: krkrz never calls SetPaintBoxSize in this
		// build/device combo, so the compose surface stays NULL and the
		// engine's redraws go nowhere. Pre-create BOTH the texture and the
		// surface at the window size (must come AFTER `texture = nullptr`).
		if (this->renderer)
		{
			// The Switch SDL driver can report the 1920x1080 display mode here
			// even when the application requested a 1280x720 handheld window.
			// Use the requested logical size for this fallback buffer; games
			// which call SetPaintBoxSize will replace it with their own size.
			int cw = new_window_w;
			int ch = new_window_h;
			if (cw <= 0 || ch <= 0) { cw = 1280; ch = 720; }
			this->texture = SDL_CreateTexture(this->renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, cw, ch);
			KRKRNS_LOG("[win] pre-created texture %p %dx%d", (void*)this->texture, cw, ch);
			this->surface = SDL_CreateRGBSurface(0, cw, ch, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0);
			KRKRNS_LOG("[win] pre-created surface %p %dx%d", (void*)this->surface, cw, ch);
			this->bitmapCompletion->surface = this->surface;
			// render the 720p content scaled to the (1080p fullscreen) window
			SDL_RenderSetLogicalSize(this->renderer, cw, ch);
			KRKRNS_LOG("[win] renderer logical size %dx%d", cw, ch);
			this->InvalidateFullSurface();
		}
#endif
		if (this->renderer)
		{
			SDL_SetRenderDrawColor(this->renderer, 0x00, 0x00, 0x00, 0xFF);
		}
	}
#ifdef _WIN32
	::SetWindowLongPtr(this->GetHandle(), GWLP_USERDATA, (LONG_PTR)this);
#endif
	Application->AddWindow(this);
}

TVPWindowWindow::~TVPWindowWindow()
{
	if (_lastWindowWindow == this)
	{
		_lastWindowWindow = this->_prevWindow;
	}
	if (this->_nextWindow)
	{
		this->_nextWindow->_prevWindow = this->_prevWindow;
	}
	if (this->_prevWindow)
	{
		this->_prevWindow->_nextWindow = this->_nextWindow;
	}
	if (_currentWindowWindow == this)
	{
		_currentWindowWindow = _lastWindowWindow;
	}
	if (this->bitmapCompletion)
	{
		delete this->bitmapCompletion;
		this->bitmapCompletion = nullptr;
	}
#ifdef KRKRZ_ENABLE_CANVAS
	if (this->context)
	{
		SDL_GL_DeleteContext(this->context);
		this->context = nullptr;
	}
#endif
	// These allocations can fail independently on a low-memory applet.  Do
	// not leak whichever half succeeded when construction later unwinds.
	if (this->texture)
	{
		SDL_DestroyTexture(this->texture);
		this->texture = nullptr;
	}
	if (this->surface)
	{
		SDL_FreeSurface(this->surface);
		this->surface = nullptr;
	}
	if (this->renderer)
	{
		SDL_DestroyRenderer(this->renderer);
		this->renderer = nullptr;
	}
	if (this->window)
	{
		SDL_DestroyWindow(this->window);
		this->window = nullptr;
	}

#ifdef _WIN32
	tjs_int count = this->WindowMessageReceivers.GetCount();
	for (tjs_int i = 0; i < count; i += 1)
	{
		tTVPMessageReceiverRecord *item = this->WindowMessageReceivers[i];
		if (!item)
		{
			continue;
		}
		delete item;
		this->WindowMessageReceivers.Remove(i);
	}
#endif

	Application->RemoveWindow(this);
}

void TVPWindowWindow::SetPaintBoxSize(tjs_int w, tjs_int h)
{
	KRKRNS_LOG("[win] SetPaintBoxSize w=%d h=%d renderer=%d", w, h, (this->renderer != nullptr));
#ifdef KRKRSDL2_ENABLE_ZOOM
	this->LayerWidth = w;
	this->LayerHeight = h;
#endif
	if (this->renderer)
	{
		if (this->texture)
		{
			SDL_DestroyTexture(this->texture);
			this->texture = nullptr;
		}
		this->texture = SDL_CreateTexture(this->renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, w, h);
		if (!this->texture)
		{
			TVPThrowExceptionMessage(TJS_W("Cannot create texture texture: %1"), ttstr(SDL_GetError()));
		}
		this->bitmapCompletion->surface = nullptr;
		if (this->surface)
		{
			SDL_FreeSurface(this->surface);
			this->surface = nullptr;
		}
		this->surface = SDL_CreateRGBSurface(0, w, h, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0);
		if (!this->surface)
		{
			TVPThrowExceptionMessage(TJS_W("Cannot create surface: %1"), ttstr(SDL_GetError()));
		}
		this->bitmapCompletion->surface = this->surface;
#ifdef __SWITCH__
		this->InvalidateFullSurface();
#endif
	}
#ifndef KRKRSDL2_ENABLE_ZOOM
	SDL_Rect cliprect;
	cliprect.x = 0;
	cliprect.y = 0;
	cliprect.w = w;
	cliprect.h = h;
#endif
	if (this->renderer)
	{
#ifdef KRKRSDL2_ENABLE_ZOOM
		this->UpdateActualZoom();
#else
		SDL_RenderSetLogicalSize(this->renderer, w, h);
#endif
	}
	if (this->TJSNativeInstance)
	{
		tTVPRect r;
		r.left = 0;
		r.top = 0;
		r.right = w;
		r.bottom = h;
		this->TJSNativeInstance->NotifyWindowExposureToLayer(r);
		this->TJSNativeInstance->GetDrawDevice()->SetClipRectangle(r);
		this->TJSNativeInstance->GetDrawDevice()->SetDestRectangle(r);
	}
}

#ifndef _WIN32
static int MulDiv(int nNumber, int nNumerator, int nDenominator)
{
	return (int)(((int64_t)nNumber * (int64_t)nNumerator) / nDenominator);
}
#endif

void TVPWindowWindow::TranslateWindowToDrawArea(int &x, int &y)
{
#ifdef KRKRSDL2_ENABLE_ZOOM
#ifdef KRKRZ_ENABLE_CANVAS
	if (this->context)
	{
		return;
	}
#endif
	x -= this->LastSentDrawDeviceDestRect.left;
	y -= this->LastSentDrawDeviceDestRect.top;
	x = MulDiv(x, this->GetInnerWidth(), this->LastSentDrawDeviceDestRect.get_width());
	y = MulDiv(y, this->GetInnerHeight(), this->LastSentDrawDeviceDestRect.get_height());
#endif
}

void TVPWindowWindow::TranslateDrawAreaToWindow(int &x, int &y)
{
#ifdef KRKRSDL2_ENABLE_ZOOM
#ifdef KRKRZ_ENABLE_CANVAS
	if (this->context)
	{
		return;
	}
#endif
	x = MulDiv(x, this->LastSentDrawDeviceDestRect.get_width(), this->GetInnerWidth());
	y = MulDiv(y, this->LastSentDrawDeviceDestRect.get_height(), this->GetInnerHeight());
	x += this->LastSentDrawDeviceDestRect.left;
	y += this->LastSentDrawDeviceDestRect.top;
#endif
}

bool TVPWindowWindow::GetFormEnabled()
{
	return (!!this->window && !!(SDL_GetWindowFlags(this->window) & SDL_WINDOW_SHOWN));
}
void TVPWindowWindow::SetDefaultMouseCursor()
{
	if (!sdl_system_cursors[0])
	{
		for (int i = 0; i < SDL_NUM_SYSTEM_CURSORS; i += 1)
		{
			sdl_system_cursors[i] = SDL_CreateSystemCursor((SDL_SystemCursor)i);
		}
	}
	SDL_SetCursor(sdl_system_cursors[SDL_SYSTEM_CURSOR_ARROW]);
}
void TVPWindowWindow::SetMouseCursor(tjs_int handle)
{
	if (!sdl_system_cursors[0])
	{
		for (int i = 0; i < SDL_NUM_SYSTEM_CURSORS; i += 1)
		{
			sdl_system_cursors[i] = SDL_CreateSystemCursor((SDL_SystemCursor)i);
		}
	}
	switch (handle)
	{
		case -2: // crArrow
			SDL_SetCursor(sdl_system_cursors[SDL_SYSTEM_CURSOR_ARROW]);
			break;
		case -3: // crCross
			SDL_SetCursor(sdl_system_cursors[SDL_SYSTEM_CURSOR_CROSSHAIR]);
			break;
		case -4: // crIBeam
			SDL_SetCursor(sdl_system_cursors[SDL_SYSTEM_CURSOR_IBEAM]);
			break;
		case -5: // crSize
			SDL_SetCursor(sdl_system_cursors[SDL_SYSTEM_CURSOR_SIZEALL]);
			break;
		case -6: // crSizeNESW
			SDL_SetCursor(sdl_system_cursors[SDL_SYSTEM_CURSOR_SIZENESW]);
			break;
		case -7: // crSizeNS
			SDL_SetCursor(sdl_system_cursors[SDL_SYSTEM_CURSOR_SIZENS]);
			break;
		case -8: // crSizeNWSE
			SDL_SetCursor(sdl_system_cursors[SDL_SYSTEM_CURSOR_SIZENWSE]);
			break;
		case -9: // crSizeWE
			SDL_SetCursor(sdl_system_cursors[SDL_SYSTEM_CURSOR_SIZEWE]);
			break;
		case -11: // crHourGlass
			SDL_SetCursor(sdl_system_cursors[SDL_SYSTEM_CURSOR_WAIT]);
			break;
		case -18: // crNo
			SDL_SetCursor(sdl_system_cursors[SDL_SYSTEM_CURSOR_NO]);
			break;
		case -19: // crAppStart
			SDL_SetCursor(sdl_system_cursors[SDL_SYSTEM_CURSOR_WAITARROW]);
			break;
		case -21: // crHandPoint
			SDL_SetCursor(sdl_system_cursors[SDL_SYSTEM_CURSOR_HAND]);
			break;
		case -22: // crSizeAll
			SDL_SetCursor(sdl_system_cursors[SDL_SYSTEM_CURSOR_SIZEALL]);
			break;
		default:
			SDL_SetCursor(sdl_system_cursors[SDL_SYSTEM_CURSOR_ARROW]);
			break;
	}
}
void TVPWindowWindow::SetMouseCursorState(tTVPMouseCursorState mcs)
{
	this->cursorTemporaryHidden = (mcs == mcsTempHidden);
	SDL_ShowCursor((mcs == mcsVisible) ? SDL_ENABLE : SDL_DISABLE);
}
tTVPMouseCursorState TVPWindowWindow::GetMouseCursorState() const
{
	return this->cursorTemporaryHidden ? mcsTempHidden : ((SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE) ? mcsVisible : mcsHidden);
}
void TVPWindowWindow::HideMouseCursor()
{
	this->SetMouseCursorState(mcsTempHidden);
}
void TVPWindowWindow::RestoreMouseCursor()
{
	if (this->cursorTemporaryHidden)
	{
		this->SetMouseCursorState(mcsVisible);
	}
}
void TVPWindowWindow::GetCursorPos(tjs_int &x, tjs_int &y)
{
	x = this->lastMouseX;
	y = this->lastMouseY;
	if (this->window != SDL_GetMouseFocus())
	{
		return;
	}
	tjs_int new_x = 0;
	tjs_int new_y = 0;
	SDL_GetMouseState(&new_x, &new_y);
	if (this->renderer)
	{
#ifdef KRKRSDL2_ENABLE_ZOOM
		this->TranslateWindowToDrawArea(new_x, new_y);
#else
		float scale_x, scale_y;
		SDL_Rect viewport;
		int window_w, window_h;
		int output_w, output_h;
		SDL_RenderGetScale(this->renderer, &scale_x, &scale_y);
		SDL_RenderGetViewport(this->renderer, &viewport);
		SDL_GetWindowSize(this->window, &window_w, &window_h);
		SDL_GetRendererOutputSize(this->renderer, &output_w, &output_h);
		float dpi_scale_x = (float)window_w / output_w;
		float dpi_scale_y = (float)window_h / output_h;
		new_x -= (int)(viewport.x * dpi_scale_x);
		new_y -= (int)(viewport.y * dpi_scale_y);
		new_x = (int)(new_x / (scale_x * dpi_scale_x));
		new_y = (int)(new_y / (scale_x * dpi_scale_y));
#endif
	}
	x = new_x;
	y = new_y;
}
void TVPWindowWindow::SetCursorPos(tjs_int x, tjs_int y)
{
	this->RestoreMouseCursor();
	if (!this->window)
	{
		return;
	}
	tjs_int new_x = x;
	tjs_int new_y = y;

	if (this->renderer)
	{
#ifdef KRKRSDL2_ENABLE_ZOOM
		this->TranslateDrawAreaToWindow(new_x, new_y);
#else
		float scale_x, scale_y;
		SDL_Rect viewport;
		int window_w, window_h;
		int output_w, output_h;
		SDL_RenderGetScale(this->renderer, &scale_x, &scale_y);
		SDL_RenderGetViewport(this->renderer, &viewport);
		SDL_GetWindowSize(this->window, &window_w, &window_h);
		SDL_GetRendererOutputSize(this->renderer, &output_w, &output_h);
		float dpi_scale_x = (float)window_w / output_w;
		float dpi_scale_y = (float)window_h / output_h;
		new_x = (int)(new_x * (scale_x * dpi_scale_x));
		new_y = (int)(new_y * (scale_x * dpi_scale_y));
		new_x += (int)(viewport.x * dpi_scale_x);
		new_y += (int)(viewport.y * dpi_scale_y);
#endif
	}
	SDL_WarpMouseInWindow(this->window, new_x, new_y);
}
void TVPWindowWindow::SetAttentionPoint(tjs_int left, tjs_int top, const struct tTVPFont *font)
{
	if (!font)
	{
		return;
	}
	this->attentionPointRect.x = left;
	this->attentionPointRect.y = top;
	this->attentionPointRect.w = 0;
	this->attentionPointRect.h = font->Height;
	this->TranslateDrawAreaToWindow(this->attentionPointRect.x, this->attentionPointRect.y);
	SDL_SetTextInputRect(&this->attentionPointRect);
}
void TVPWindowWindow::BringToFront()
{
	if (_currentWindowWindow != this)
	{
		if (_currentWindowWindow)
		{
			_currentWindowWindow->TJSNativeInstance->OnReleaseCapture();
		}
		_currentWindowWindow = this;
	}
	if (this->window)
	{
		SDL_RaiseWindow(this->window);
	}
}
void TVPWindowWindow::ShowWindowAsModal()
{
#if defined(KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE)
	TVPThrowExceptionMessage(TJS_W("Showing window as modal is not supported"));
#else
	this->in_mode_ = true;
	this->BringToFront();
	this->modal_result_ = 0;
	while (this == _currentWindowWindow && !this->modal_result_)
	{
		process_events();
		if (::Application->IsTarminate())
		{
			this->modal_result_ = mrCancel;
		}
		else if (this->modal_result_)
		{
			break;
		}
	}
	this->in_mode_ = false;
#endif
}
bool TVPWindowWindow::GetVisible()
{
	return (!this->visibilityHasInitialized) ? this->isVisible : (!!this->window && !!(SDL_GetWindowFlags(this->window) & SDL_WINDOW_SHOWN));
}
void TVPWindowWindow::SetVisible(bool visible)
{
	this->isVisible = visible;
	if (!this->visibilityHasInitialized)
	{
		return;
	}
	if (this->window)
	{
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
		if (visible)
		{
			SDL_ShowWindow(this->window);
		}
		else
		{
			SDL_HideWindow(this->window);
		}
#endif
	}
	if (visible)
	{
		this->BringToFront();
	}
	else if (_currentWindowWindow == this)
	{
		_currentWindowWindow = this->_prevWindow ? this->_prevWindow : this->_nextWindow;
		if (_currentWindowWindow)
		{
			_currentWindowWindow->BringToFront();
		}
	}
}
void TVPWindowWindow::SetFullScreenMode(bool fullscreen)
{
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		SDL_SetWindowFullscreen(this->window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	}
	this->UpdateWindow(utNormal);
#endif
}
bool TVPWindowWindow::GetFullScreenMode()
{
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	return !!this->window && !!(SDL_GetWindowFlags(this->window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP));
#else
	return false;
#endif
}
void TVPWindowWindow::SetBorderStyle(tTVPBorderStyle bs)
{
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		SDL_SetWindowBordered(this->window, (bs == bsNone) ? SDL_FALSE : SDL_TRUE);
		SDL_SetWindowResizable(this->window, (bs == bsSizeable || bs == bsSizeToolWin) ? SDL_TRUE : SDL_FALSE);
	}
#endif
}
tTVPBorderStyle TVPWindowWindow::GetBorderStyle() const
{
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		Uint32 flags = SDL_GetWindowFlags(this->window);
		if (flags & SDL_WINDOW_BORDERLESS)
		{
			return bsNone;
		}
		else if (flags & SDL_WINDOW_RESIZABLE)
		{
			return bsSizeable;
		}
	}
#endif
	return bsSingle;
}
tjs_string TVPWindowWindow::GetCaption()
{
	std::string v_utf8 = this->window ? SDL_GetWindowTitle(this->window) : "";
	tjs_string v_utf16;
	TVPUtf8ToUtf16(v_utf16, v_utf8);
	return v_utf16;
}
void TVPWindowWindow::GetCaption(tjs_string & v) const
{
	v.clear();
	std::string v_utf8 = this->window ? SDL_GetWindowTitle(this->window) : "";
	TVPUtf8ToUtf16(v, v_utf8);
}
void TVPWindowWindow::SetCaption(const tjs_string & v)
{
	if (!this->window)
	{
		return;
	}
	std::string v_utf8;
	if (!TVPUtf16ToUtf8(v_utf8, v))
	{
		return;
	}
	SDL_SetWindowTitle(this->window, v_utf8.c_str());
}
void TVPWindowWindow::SetWidth(tjs_int w)
{
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		int h;
		SDL_GetWindowSize(this->window, nullptr, &h);
		SDL_SetWindowSize(this->window, w, h);
		if (!this->renderer && this->surface)
		{
			this->bitmapCompletion->surface = nullptr;
			this->surface = SDL_GetWindowSurface(this->window);
			if (!this->surface)
			{
				TVPThrowExceptionMessage(TJS_W("Cannot get surface from SDL window: %1"), ttstr(SDL_GetError()));
			}
			this->bitmapCompletion->surface = this->surface;
		}
	}
#endif
#ifdef KRKRSDL2_ENABLE_ZOOM
	this->UpdateActualZoom();
#else
	this->UpdateWindow(utNormal);
#endif
}
void TVPWindowWindow::SetHeight(tjs_int h)
{
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		int w;
		SDL_GetWindowSize(this->window, &w, nullptr);
		SDL_SetWindowSize(this->window, w, h);
		if (!this->renderer && this->surface)
		{
			this->bitmapCompletion->surface = nullptr;
			this->surface = SDL_GetWindowSurface(this->window);
			if (!this->surface)
			{
				TVPThrowExceptionMessage(TJS_W("Cannot get surface from SDL window: %1"), ttstr(SDL_GetError()));
			}
			this->bitmapCompletion->surface = this->surface;
		}
	}
#endif
#ifdef KRKRSDL2_ENABLE_ZOOM
	this->UpdateActualZoom();
#else
	this->UpdateWindow(utNormal);
#endif
}
void TVPWindowWindow::SetSize(tjs_int w, tjs_int h)
{
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		SDL_SetWindowSize(this->window, w, h);
		if (!this->renderer && this->surface)
		{
			this->bitmapCompletion->surface = nullptr;
			this->surface = SDL_GetWindowSurface(this->window);
			if (!this->surface)
			{
				TVPThrowExceptionMessage(TJS_W("Cannot get surface from SDL window: %1"), ttstr(SDL_GetError()));
			}
			this->bitmapCompletion->surface = this->surface;
		}
	}
#endif
#ifdef KRKRSDL2_ENABLE_ZOOM
	this->UpdateActualZoom();
#else
	this->UpdateWindow(utNormal);
#endif
}
void TVPWindowWindow::GetSize(tjs_int &w, tjs_int &h)
{
	w = 0;
	h = 0;
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		SDL_GetWindowSize(this->window, &w, &h);
		return;
	}
#endif
	if (this->renderer)
	{
#ifdef KRKRSDL2_ENABLE_ZOOM
		SDL_GetRendererOutputSize(this->renderer, &w, &h);
#else
		SDL_RenderGetLogicalSize(this->renderer, &w, &h);
#endif
		return;
	}
}
tjs_int TVPWindowWindow::GetWidth() const
{
	int w = 0;
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		SDL_GetWindowSize(this->window, &w, nullptr);
		return w;
	}
#endif
	if (this->renderer)
	{
		int h;
#ifdef KRKRSDL2_ENABLE_ZOOM
		SDL_GetRendererOutputSize(this->renderer, &w, &h);
#else
		SDL_RenderGetLogicalSize(this->renderer, &w, &h);
#endif
	}
	return w;
}
tjs_int TVPWindowWindow::GetHeight() const
{
	int h = 0;
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		SDL_GetWindowSize(this->window, nullptr, &h);
		return h;
	}
#endif
	if (this->renderer)
	{
		int w;
#ifdef KRKRSDL2_ENABLE_ZOOM
		SDL_GetRendererOutputSize(this->renderer, &w, &h);
#else
		SDL_RenderGetLogicalSize(this->renderer, &w, &h);
#endif
	}
	return h;
}
void TVPWindowWindow::SetMinWidth(tjs_int w)
{
	this->SetMinSize(w, this->GetMinHeight());
}
void TVPWindowWindow::SetMaxWidth(tjs_int w)
{
	this->SetMaxSize(w, this->GetMaxHeight());
}
void TVPWindowWindow::SetMinHeight(tjs_int h)
{
	this->SetMinSize(this->GetMinWidth(), h);
}
void TVPWindowWindow::SetMaxHeight(tjs_int h)
{
	this->SetMaxSize(this->GetMaxWidth(), h);
}
void TVPWindowWindow::SetMinSize(tjs_int w, tjs_int h)
{
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		SDL_SetWindowMinimumSize(this->window, w, h);
	}
#endif
}
void TVPWindowWindow::SetMaxSize(tjs_int w, tjs_int h)
{
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		SDL_SetWindowMaximumSize(this->window, w, h);
	}
#endif
}
tjs_int TVPWindowWindow::GetMinWidth()
{
	int w = 0;
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		SDL_GetWindowMinimumSize(this->window, &w, nullptr);
		return w;
	}
#endif
	if (this->renderer)
	{
		int h;
#ifdef KRKRSDL2_ENABLE_ZOOM
		SDL_GetRendererOutputSize(this->renderer, &w, &h);
#else
		SDL_RenderGetLogicalSize(this->renderer, &w, &h);
#endif
	}
	return w;
}
tjs_int TVPWindowWindow::GetMaxWidth()
{
	int w = 0;
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		SDL_GetWindowMaximumSize(this->window, &w, nullptr);
		return w;
	}
#endif
	if (this->renderer)
	{
		int h;
#ifdef KRKRSDL2_ENABLE_ZOOM
		SDL_GetRendererOutputSize(this->renderer, &w, &h);
#else
		SDL_RenderGetLogicalSize(this->renderer, &w, &h);
#endif
	}
	return w;
}
tjs_int TVPWindowWindow::GetMinHeight()
{
	int h = 0;
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		SDL_GetWindowMinimumSize(this->window, &h, nullptr);
		return h;
	}
#endif
	if (this->renderer)
	{
		int w;
#ifdef KRKRSDL2_ENABLE_ZOOM
		SDL_GetRendererOutputSize(this->renderer, &w, &h);
#else
		SDL_RenderGetLogicalSize(this->renderer, &w, &h);
#endif
	}
	return h;
}
tjs_int TVPWindowWindow::GetMaxHeight()
{
	int h = 0;
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		SDL_GetWindowMaximumSize(this->window, &h, nullptr);
		return h;
	}
#endif
	if (this->renderer)
	{
		int w;
#ifdef KRKRSDL2_ENABLE_ZOOM
		SDL_GetRendererOutputSize(this->renderer, &w, &h);
#else
		SDL_RenderGetLogicalSize(this->renderer, &w, &h);
#endif
	}
	return h;
}
tjs_int TVPWindowWindow::GetLeft()
{
	int x = 0;
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		SDL_GetWindowPosition(this->window, &x, nullptr);
	}
#endif
	return x;
}
void TVPWindowWindow::SetLeft(tjs_int l)
{
	this->SetPosition(l, this->GetTop());
}
tjs_int TVPWindowWindow::GetTop()
{
	int y = 0;
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		SDL_GetWindowPosition(this->window, nullptr, &y);
	}
#endif
	return y;
}
void TVPWindowWindow::SetTop(tjs_int t)
{
	this->SetPosition(this->GetLeft(), t);
}
void TVPWindowWindow::SetPosition(tjs_int l, tjs_int t)
{
#ifndef KRKRSDL2_WINDOW_SIZE_IS_LAYER_SIZE
	if (this->window)
	{
		SDL_SetWindowPosition(this->window, l, t);
	}
#endif
}
TVPSDLBitmapCompletion *TVPWindowWindow::GetTVPSDLBitmapCompletion()
{
	this->needsGraphicUpdate = true;
	return this->bitmapCompletion;
}
#ifdef KRKRZ_ENABLE_CANVAS
void TVPWindowWindow::SetOpenGLScreen(tTVPOpenGLScreen *s)
{
	this->openGlScreen = s;
}
void TVPWindowWindow::SetSwapInterval(int interval)
{
	if (this->context)
	{
		SDL_GL_SetSwapInterval(interval);
	}
}
void TVPWindowWindow::GetDrawableSize(tjs_int &w, tjs_int &h)
{
	w = 0;
	h = 0;
	if (this->context)
	{
		SDL_GL_GetDrawableSize(this->window, &w, &h);
	}
}
void TVPWindowWindow::Swap()
{
	if (this->context)
	{
		SDL_GL_SwapWindow(this->window);
	}
}
#endif
void TVPWindowWindow::Show()
{
}
#ifdef __SWITCH__
void TVPWindowWindow::InvalidateFullSurface()
{
	if (this->bitmapCompletion && this->surface)
	{
		tTVPRect r;
		r.left = 0;
		r.top = 0;
		r.right = this->surface->w;
		r.bottom = this->surface->h;
		this->bitmapCompletion->update_rect.do_union(r);
		this->needsGraphicUpdate = true;
	}
}
#endif
#ifdef __SWITCH__
// Diagnostic helper: save each layer's own bitmap so a partial-scene defect
// can be attributed to the layer that produced it (bg content vs pool content
// vs manager composition vs upload).
static void KrkrDumpLayerBmps(tTJSNI_BaseLayer* layer, int depth, int& count, int path)
{
	if (!layer || depth > 5 || count >= 40) return;
	count++;
	try
	{
		ttstr name = ttstr("sdmc:/switch/krkrsdl2/layer_") + ttstr(path) + ttstr(".bmp");
		layer->SaveLayerImage(name, TJS_W("bmp"));
	}
	catch(...)
	{
		// dumping is best-effort; never take the game down for it
	}
	for (tjs_int i = 0; i < (tjs_int)layer->GetCount(); i++)
	{
		KrkrDumpLayerBmps(layer->GetChildren(i), depth + 1, count, path * 10 + (i + 1));
	}
}
#endif
void TVPWindowWindow::TickBeat()
{
	if (!this->visibilityHasInitialized)
	{
		this->visibilityHasInitialized = true;
		this->SetVisible(this->isVisible);
	}
#ifdef __SWITCH__
	// Opt-in diagnostics: create this marker on SD to capture the CPU
	// composite surface, the presented backbuffer and the real engine layer
	// tree, without changing scripts. Checked even when the scene is static,
	// then forces one full present so both captures are consistent.
	static Uint32 lastTraceCheck = 0;
	const Uint32 traceNow = SDL_GetTicks();
	if (traceNow - lastTraceCheck >= 1000)
	{
		lastTraceCheck = traceNow;
		const char* marker = "sdmc:/switch/krkrsdl2/trace-render.once";
		FILE* trace = fopen(marker, "rb");
		if (trace)
		{
			fclose(trace);
			remove(marker);
			if (this->surface)
				SDL_SaveBMP(this->surface, "sdmc:/switch/krkrsdl2/render-surface.bmp");
			KRKRNS_LOG("[render-trace] surface=%dx%d texture=%d update=%d,%d %dx%d",
				this->surface ? this->surface->w : 0,
				this->surface ? this->surface->h : 0,
				this->texture != nullptr,
				this->bitmapCompletion ? this->bitmapCompletion->update_rect.left : -1,
				this->bitmapCompletion ? this->bitmapCompletion->update_rect.top : -1,
				this->bitmapCompletion ? this->bitmapCompletion->update_rect.get_width() : 0,
				this->bitmapCompletion ? this->bitmapCompletion->update_rect.get_height() : 0);
			if (this->TJSNativeInstance)
				this->TJSNativeInstance->DumpPrimaryLayerStructure();
#ifdef __SWITCH__
			{
				iTVPDrawDevice* dev = this->TJSNativeInstance->GetDrawDevice();
				tTVPDrawDevice* zdev = dynamic_cast<tTVPDrawDevice*>(dev);
				if (zdev)
				{
					for (size_t m = 0; m < 16; m++)
					{
						iTVPLayerManager* mgr = zdev->GetLayerManagerAt(m);
						if (!mgr) break;
						tTJSNI_BaseLayer* pri = mgr->GetPrimaryLayer();
						if (!pri) continue;
						int count = 0;
						KrkrDumpLayerBmps(pri, 0, count, (int)(m * 1000));
					}
				}
			}
#endif
			this->InvalidateFullSurface();
			this->traceCapturePending = true;
		}
	}
#endif
	if (this->needsGraphicUpdate)
	{
		if (this->bitmapCompletion)
		{
			SDL_Rect rect;
			rect.x = this->bitmapCompletion->update_rect.left;
			rect.y = this->bitmapCompletion->update_rect.top;
			rect.w = this->bitmapCompletion->update_rect.get_width();
			rect.h = this->bitmapCompletion->update_rect.get_height();
			if (this->renderer)
			{
#if defined(KRKRSDL2_ENABLE_ZOOM) || defined(KRKRSDL2_RENDERER_FULL_UPDATES)
				SDL_RenderFillRect(this->renderer, nullptr);
#else
				SDL_Rect logical_rect;
				SDL_RenderGetLogicalSize(this->renderer, &(logical_rect.w), &(logical_rect.h));
				if (logical_rect.w == rect.w && logical_rect.h == rect.h)
				{
					// Clear extra artifacts
					SDL_RenderSetLogicalSize(this->renderer, 0, 0);
					SDL_RenderFillRect(this->renderer, nullptr);
					SDL_RenderSetLogicalSize(this->renderer, logical_rect.w, logical_rect.h);
				}
#endif
				// Phase 3 v2.8: mode5 (gpu-composite-gpuonly.txt) blits the
				// compose FBO straight onto the window backbuffer inside
				// krkrsdl2_glc_readback; when it returns true the surface
				// upload + RenderClear/Copy are skipped and the empty SDL
				// queue is presented (swap only). Declared outside the
				// __SWITCH__ guards so the upload gate below compiles on
				// every platform (always false off-Switch).
				bool gpuPresented = false;
#ifdef __SWITCH__
				if (!this->texture && this->renderer)
				{
					// The texture must match the compose surface, not the
					// window: a mismatch silently clips SDL_UpdateTexture and
					// loses everything outside the intersection.
					int cw = 0, ch = 0;
					if (this->surface)
					{
						cw = this->surface->w;
						ch = this->surface->h;
					}
					else
					{
						SDL_GetWindowSize(this->window, &cw, &ch);
						if (cw <= 0 || ch <= 0) { cw = 1280; ch = 720; }
					}
					this->texture = SDL_CreateTexture(this->renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, cw, ch);
					KRKRNS_LOG("[win] TickBeat late texture %p %dx%d", (void*)this->texture, cw, ch);
					this->InvalidateFullSurface();
				}
#endif
				if (this->texture)
				{
					if (this->surface)
					{
#ifdef __SWITCH__
						// One-shot diagnostic: a texture smaller than the
						// compose surface clips every upload (scene data loss).
						static bool sizeMismatchLogged = false;
						if (!sizeMismatchLogged)
						{
							int tw = 0, th = 0;
							if (SDL_QueryTexture(this->texture, nullptr, nullptr, &tw, &th) == 0 &&
								(tw != this->surface->w || th != this->surface->h))
							{
								sizeMismatchLogged = true;
								KRKRNS_LOG("[win] SIZE MISMATCH texture=%dx%d surface=%dx%d",
									tw, th, this->surface->w, this->surface->h);
							}
						}
#endif
#ifdef __SWITCH__
						// Full-surface upload every frame. At CPU-backend
						// frame rates the cost is negligible, and it removes
						// any chance of stale dirty-rect regions (previous
						// scene fragments lingering after a switch).
						// Phase 1: marker sdmc:/switch/krkrsdl2/dirtyrect-update.txt
						// switches to dirty-rect upload (the streaming texture
						// retains its content across UpdateTexture calls and
						// RenderCopy below still redraws the whole backbuffer,
						// so only the CPU->GPU transfer shrinks). A periodic
						// full upload stays as a safety net against any stale
						// dirty-rect bookkeeping.
						{
							static bool fDirtyRectSet = false;
							static bool fDirtyRectMode = false;
							static Uint32 fFullNetCounter = 0;
							if (!fDirtyRectSet)
							{
								fDirtyRectSet = true;
								FILE* m = fopen("sdmc:/switch/krkrsdl2/dirtyrect-update.txt", "rb");
								if (m) { fclose(m); fDirtyRectMode = true; }
								KRKRNS_LOG("[win] dirty-rect upload mode=%s",
									fDirtyRectMode ? "on" : "off");
							}
							if (!fDirtyRectMode || (++fFullNetCounter % 120) == 1)
							{
								tTVPRect full;
								full.left = 0;
								full.top = 0;
								full.right = this->surface->w;
								full.bottom = this->surface->h;
								rect = SDL_Rect{full.left, full.top,
												full.get_width(), full.get_height()};
							}
						}
#endif
#ifdef __SWITCH__
						// Phase 3: GPU-composed frame read back into the
						// surface; the validated upload/present chain follows.
						// Mode 5 returns true when the frame was blitted to
						// the window backbuffer directly (see gpuPresented).
						if (this->surface)
							gpuPresented = krkrsdl2_glc_readback(this->surface->pixels,
								this->surface->w, this->surface->h,
								this->surface->pitch);
#endif
						const Uint64 uploadStart = SDL_GetPerformanceCounter();
						if (!gpuPresented && TVPUploadDirtySurface(this->renderer, this->texture, this->surface, rect) != 0)
						{
							KRKRNS_LOG("[win] bitmap upload failed: %s", SDL_GetError());
							return; // Keep pending damage for the next frame.
						}
#ifdef __SWITCH__
						{
							const Uint64 now = SDL_GetPerformanceCounter();
							const double freq = (double)SDL_GetPerformanceFrequency();
							krkrsdl2_prof_accum_upload(
								(now - uploadStart) * 1000.0 / freq,
								(unsigned)(rect.w * rect.h * 4));
							static Uint32 uploadProf = 0;
							static Uint64 lastTick = 0;
							if (++uploadProf >= 60)
							{
								uploadProf = 0;
								const double interval =
									lastTick ? (now - lastTick) * 1000.0 / freq : 0.0;
								KRKRNS_LOG("[win] frame profile: upload=%.2fms interval=%.1fms",
									(now - uploadStart) * 1000.0 / freq, interval);
								krkrsdl2_prof_emit_and_reset(interval);
							}
							lastTick = now;
						}
#endif
					}
#if defined(KRKRSDL2_ENABLE_ZOOM)
					SDL_Rect destrect;
					destrect.x = this->LastSentDrawDeviceDestRect.left;
					destrect.y = this->LastSentDrawDeviceDestRect.top;
					destrect.w = this->LastSentDrawDeviceDestRect.get_width();
					destrect.h = this->LastSentDrawDeviceDestRect.get_height();
					SDL_Rect srcrect;
					srcrect.x = 0;
					srcrect.y = 0;
					srcrect.w = this->GetInnerWidth();
					srcrect.h = this->GetInnerHeight();
					SDL_RenderCopy(this->renderer, this->texture, &srcrect, &destrect);
#elif defined(__SWITCH__)
						// The Switch swapchain does not preserve untouched pixels after
						// Present, so redraw the complete resident texture each time.
						// (Skipped on GPU-presented frames: the blit already wrote the
						// backbuffer and the queue below is empty -> swap only.)
						if (!gpuPresented)
						{
							SDL_RenderClear(this->renderer);
							SDL_RenderCopy(this->renderer, this->texture, nullptr, nullptr);
						}
#elif defined(KRKRSDL2_RENDERER_FULL_UPDATES)
						SDL_RenderCopy(this->renderer, this->texture, nullptr, nullptr);
#else
						SDL_RenderCopy(this->renderer, this->texture, &rect, &rect);
#endif
					}
				#ifdef __SWITCH__
					if (this->traceCapturePending && this->renderer)
					{
					// Capture the presented backbuffer: comparing it with the
					// surface BMP decides whether a defect lives in layer
					// composition (surface already wrong) or in upload/present.
					this->traceCapturePending = false;
					int ow = 0, oh = 0;
					if (SDL_GetRendererOutputSize(this->renderer, &ow, &oh) == 0 && ow > 0 && oh > 0)
					{
						SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ABGR8888);
						if (shot)
						{
							if (SDL_RenderReadPixels(this->renderer, nullptr, shot->pitch, shot->pixels, shot->pitch) == 0)
								SDL_SaveBMP(shot, "sdmc:/switch/krkrsdl2/render-present.bmp");
							else
								KRKRNS_LOG("[render-trace] readpixels failed: %s", SDL_GetError());
							SDL_FreeSurface(shot);
						}
					}
				}
#endif
#ifdef __SWITCH__
				const Uint64 presentStart = SDL_GetPerformanceCounter();
#endif
				// GPU-presented frames were already swapped inside
				// krkrsdl2_glc_readback (SDL_GL_SwapWindow); presenting an
				// empty SDL queue again is a no-op at best and a skipped
				// swap at worst on this driver.
				if (!gpuPresented)
					SDL_RenderPresent(this->renderer);
#ifdef __SWITCH__
				krkrsdl2_prof_accum_present(
					(SDL_GetPerformanceCounter() - presentStart) * 1000.0 /
					(double)SDL_GetPerformanceFrequency());
#endif
#if !defined(KRKRSDL2_ENABLE_ZOOM) && !defined(KRKRSDL2_RENDERER_FULL_UPDATES) && !defined(__SWITCH__)
				if (logical_rect.w == rect.w && logical_rect.h == rect.h)
				{
					// Clear extra artifacts (for the back buffer)
					SDL_RenderSetLogicalSize(this->renderer, 0, 0);
					SDL_RenderFillRect(this->renderer, nullptr);
					SDL_RenderSetLogicalSize(this->renderer, logical_rect.w, logical_rect.h);
				}
				if (this->texture)
				{
					SDL_RenderCopy(this->renderer, this->texture, &rect, &rect);
				}
#endif
				this->hasDrawn = true;
			}
			else if (this->window && this->surface)
			{
				SDL_UpdateWindowSurfaceRects(this->window, &rect, 1);
				this->hasDrawn = true;
			}
			this->bitmapCompletion->update_rect.clear();
			this->needsGraphicUpdate = false;
		}
	}
#ifdef KRKRZ_ENABLE_CANVAS
	else if (this->context && this->TJSNativeInstance)
	{
		this->TJSNativeInstance->StartDrawing();
		this->hasDrawn = true;
	}
#endif
}
void TVPWindowWindow::InvalidateClose()
{
	this->TJSNativeInstance = nullptr;
	this->SetVisible(false);
	delete this;
}
bool TVPWindowWindow::GetWindowActive()
{
	return _currentWindowWindow == this && SDL_GetWindowFlags(this->window) & SDL_WINDOW_INPUT_FOCUS;
}
void TVPWindowWindow::OnClose(CloseAction& action)
{
	action = (!this->modal_result_) ? caNone : caHide;

	if (!this->ProgramClosing || !this->TJSNativeInstance)
	{
		return;
	}
	if (!this->TJSNativeInstance->IsMainWindow())
	{
		action = caFree;
	}
	iTJSDispatch2 *obj = this->TJSNativeInstance->GetOwnerNoAddRef();
	this->TJSNativeInstance->NotifyWindowClose();
	obj->Invalidate(0, nullptr, nullptr, obj);
	this->TJSNativeInstance = nullptr;
	this->SetVisible(false);
}
bool TVPWindowWindow::OnCloseQuery()
{
	// closing actions are 3 patterns;
	// 1. closing action by the user
	// 2. "close" method
	// 3. object invalidation

	if (TVPGetBreathing())
	{
		return false;
	}

	// the default event handler will invalidate this object when an onCloseQuery
	// event reaches the handler.
	if (!this->TJSNativeInstance)
	{
		return true;
	}
	if (this->modal_result_ && this->modal_result_ != mrCancel/* mrCancel=when close button is pushed in modal window */)
	{
		return true;
	}
	iTJSDispatch2 *obj = this->TJSNativeInstance->GetOwnerNoAddRef();
	if (!obj)
	{
		return true;
	}
	tTJSVariant arg[1] = { true };
	static ttstr eventname(TJS_W("onCloseQuery"));

	if (!this->ProgramClosing)
	{
		// close action does not happen immediately
		if (this->TJSNativeInstance)
		{
			TVPPostInputEvent(new tTVPOnCloseInputEvent(this->TJSNativeInstance));
		}

		this->Closing = true; // waiting closing...
	//	TVPSystemControl->NotifyCloseClicked();
		return false;
	}
	else
	{
		this->CanCloseWork = true;
		TVPPostEvent(obj, obj, eventname, 0, TVP_EPT_IMMEDIATE, sizeof(arg)/sizeof(arg[0]), arg);
		process_events(); // for post event
		// this event happens immediately
		// and does not return until done
		return this->CanCloseWork; // CanCloseWork is set by the event handler
	}
}
void TVPWindowWindow::Close()
{
	// closing action by "close" method
	if (this->Closing)
	{
		return; // already waiting closing...
	}

	this->ProgramClosing = true;
	try
	{
		//tTVPWindow::Close();
		if (this->in_mode_)
		{
			this->modal_result_ = mrCancel;
		}
		else if (this->OnCloseQuery())
		{
			CloseAction action = caFree;
			this->OnClose(action);
			switch (action)
			{
				case caNone:
					break;
				case caHide:
					this->SetVisible(false);
					break;
				case caMinimize:
					if (this->window)
					{
						SDL_MinimizeWindow(this->window);
					}
					break;
				case caFree:
				default:
					this->isBeingDeleted = true;
					//::PostMessage(this->GetHandle(), TVP_EV_WINDOW_RELEASE, 0, 0);
					break;
			}
		}
	}
	catch (...)
	{
		this->ProgramClosing = false;
		throw;
	}
	this->ProgramClosing = false;
}
void TVPWindowWindow::OnCloseQueryCalled(bool b)
{
	// closing is allowed by onCloseQuery event handler
	if (!this->ProgramClosing)
	{
		// closing action by the user
		if (b)
		{
			if (this->in_mode_)
			{
				this->modal_result_ = 1; // when modal
			}
			else
			{
				this->SetVisible(false);  // just hide
			}

			this->Closing = false;
			if (this->TJSNativeInstance)
			{
				if (this->TJSNativeInstance->IsMainWindow())
				{
					// this is the main window
					iTJSDispatch2 *obj = this->TJSNativeInstance->GetOwnerNoAddRef();
					obj->Invalidate(0, nullptr, nullptr, obj);
				}
			}
			else
			{
				delete this;
			}
		}
		else
		{
			this->Closing = false;
		}
	}
	else
	{
		// closing action by the program
		this->CanCloseWork = b;
	}
}
void TVPWindowWindow::SetImeMode(tTVPImeMode mode)
{
	if (!this->window || mode == ::imDisable || mode == ::imClose)
	{
		this->ResetImeMode();
	}
	else if (!SDL_IsTextInputActive())
	{
		SDL_SetTextInputRect(&this->attentionPointRect);
		SDL_StartTextInput();
	}
}
void TVPWindowWindow::ResetImeMode()
{
	this->imeCompositionStr = nullptr;
	this->imeCompositionLen = 0;
	this->imeCompositionCursor = 0;
	this->imeCompositionSelection = 0;
	this->attentionPointRect.x = 0;
	this->attentionPointRect.y = 0;
	this->attentionPointRect.w = 0;
	this->attentionPointRect.h = 0;
	if (this->window && SDL_IsTextInputActive())
	{
		SDL_SetTextInputRect(&this->attentionPointRect);
		SDL_StopTextInput();
	}
}
void TVPWindowWindow::UpdateWindow(tTVPUpdateType type)
{
	if (!this->TJSNativeInstance)
	{
		return;
	}
	tTVPRect r;
#ifdef KRKRSDL2_ENABLE_ZOOM
	r.left = 0;
	r.top = 0;
	r.right = this->LayerWidth;
	r.bottom = this->LayerHeight;
#else
	r.clear();
	if (this->renderer)
	{
		SDL_RenderGetLogicalSize(this->renderer, &(r.right), &(r.bottom));
		SDL_RenderSetLogicalSize(this->renderer, r.right, r.bottom);
	}
	else if (this->window)
	{
		SDL_GetWindowSize(this->window, &(r.right), &(r.bottom));
	}
#endif
	this->TJSNativeInstance->NotifyWindowExposureToLayer(r);
	TVPDeliverWindowUpdateEvents();
}
void TVPWindowWindow::InternalKeyDown(tjs_uint16 key, tjs_uint32 shift)
{
	TVPPostInputEvent(new tTVPOnKeyDownInputEvent(this->TJSNativeInstance, key, shift));
}
void TVPWindowWindow::OnKeyUp(tjs_uint16 vk, int shift)
{
	TVPPostInputEvent(new tTVPOnKeyUpInputEvent(this->TJSNativeInstance, vk, shift));
}
void TVPWindowWindow::OnKeyPress(tjs_uint16 vk, int repeat, bool prevkeystate, bool convertkey)
{
	TVPPostInputEvent(new tTVPOnKeyPressInputEvent(this->TJSNativeInstance, vk));
}

#ifdef KRKRSDL2_ENABLE_ZOOM
//---------------------------------------------------------------------------
//! @brief	do reduction for numer over denom
static void TVPDoReductionNumerAndDenom(tjs_int &n, tjs_int &d)
{
	tjs_int a = n;
	tjs_int b = d;
	while (b)
	{
		tjs_int t = b;
		b = a % b;
		a = t;
	}
	n = n / a;
	d = d / a;
}
#endif

void TVPWindowWindow::UpdateActualZoom(void)
{
#ifdef KRKRSDL2_ENABLE_ZOOM
	if (!this->renderer)
	{
		return;
	}
#ifdef KRKRZ_ENABLE_CANVAS
	if (this->context)
	{
		return;
	}
#endif
	// determine fullscreen zoom factor and client size
	int sb_w, sb_h, zoom_d, zoom_n, output_w, output_h;
	SDL_GetRendererOutputSize(this->renderer, &output_w, &output_h);

	float layer_aspect = (float)this->GetInnerWidth() / this->GetInnerHeight();
	float output_aspect = (float)output_w / output_h;

	// 0=letterbox, 1=crop
	int scale_policy = 0;
	SDL_Rect viewport;
	if (SDL_fabs(layer_aspect - output_aspect) < 0.0001)
	{
		zoom_n = 1;
		zoom_d = 1;
		viewport.x = 0;
		viewport.y = 0;
		viewport.w = this->GetInnerWidth();
		viewport.h = this->GetInnerHeight();
	}
	else if (layer_aspect > output_aspect)
	{
		if (scale_policy == 1)
		{
			// Crop left and right
			zoom_n = output_h;
			zoom_d = this->GetInnerHeight();
			TVPDoReductionNumerAndDenom(zoom_n, zoom_d);
			viewport.y = 0;
			viewport.h = output_h;
			viewport.w = MulDiv(this->GetInnerWidth(), zoom_n, zoom_d);
			viewport.x = (output_w - viewport.w) / 2;
		}
		else
		{
			// Top and bottom black bars (letterbox)
			zoom_n = output_w;
			zoom_d = this->GetInnerWidth();
			TVPDoReductionNumerAndDenom(zoom_n, zoom_d);
			viewport.x = 0;
			viewport.w = output_w;
			viewport.h = MulDiv(this->GetInnerHeight(), zoom_n, zoom_d);
			viewport.y = (output_h - viewport.h) / 2;
		}
	}
	else
	{
		if (scale_policy == 1)
		{
			// Crop top and bottom
			zoom_n = output_w;
			zoom_d = this->GetInnerWidth();
			TVPDoReductionNumerAndDenom(zoom_n, zoom_d);
			viewport.x = 0;
			viewport.w = output_w;
			viewport.h = MulDiv(this->GetInnerHeight(), zoom_n, zoom_d);
			viewport.y = (output_h - viewport.h) / 2;
		}
		else
		{
			// Left and right black bars (letterbox)
			zoom_n = output_h;
			zoom_d = this->GetInnerHeight();
			TVPDoReductionNumerAndDenom(zoom_n, zoom_d);
			viewport.y = 0;
			viewport.h = output_h;
			viewport.w = MulDiv(this->GetInnerWidth(), zoom_n, zoom_d);
			viewport.x = (output_w - viewport.w) / 2;
		}
	}
	this->FullScreenDestRect.set_size(viewport.w, viewport.h);
	this->FullScreenDestRect.set_offsets(viewport.x, viewport.y);

	this->ActualZoomNumer = zoom_n;
	this->ActualZoomDenom = zoom_d;
	this->SetDrawDeviceDestRect();
#endif
}

void TVPWindowWindow::SetDrawDeviceDestRect(void)
{
#ifdef KRKRSDL2_ENABLE_ZOOM
	tTVPRect destrect;
	tjs_int w = MulDiv(this->GetInnerWidth(),  this->ActualZoomNumer, this->ActualZoomDenom);
	tjs_int h = MulDiv(this->GetInnerHeight(), this->ActualZoomNumer, this->ActualZoomDenom);
	if (w < 1)
	{
		w = 1;
	}
	if (h < 1)
	{
		h = 1;
	}
	{
		destrect.left = this->FullScreenDestRect.left;
		destrect.top = this->FullScreenDestRect.top;
		destrect.right = destrect.left + w;
		destrect.bottom = destrect.top + h;
	}

	if (this->LastSentDrawDeviceDestRect != destrect)
	{
		this->LastSentDrawDeviceDestRect = destrect;
		this->UpdateWindow(utNormal);
	}
#endif
}

void TVPWindowWindow::SetZoom(tjs_int numer, tjs_int denom, bool set_logical)
{
#ifdef KRKRSDL2_ENABLE_ZOOM
	bool ischanged = false;
	// set layer zooming factor;
	// the zooming factor is passed in numerator/denoiminator style.
	// we must find GCM to optimize numer/denium via Euclidean algorithm.
	TVPDoReductionNumerAndDenom(numer, denom);
	if (set_logical)
	{
		if (this->ZoomNumer != numer || this->ZoomDenom != denom)
		{
			ischanged = true;
		}
		this->ZoomNumer = numer;
		this->ZoomDenom = denom;
	}
	this->UpdateActualZoom();
#endif
}

void TVPWindowWindow::SetZoomNumer(tjs_int n)
{
	this->SetZoom(n, this->GetZoomDenom());
}

tjs_int TVPWindowWindow::GetZoomNumer() const
{
#ifdef KRKRSDL2_ENABLE_ZOOM
	return this->ZoomNumer;
#else
	return 1;
#endif
}

void TVPWindowWindow::SetZoomDenom(tjs_int d)
{
	this->SetZoom(this->GetZoomNumer(), d);
}

tjs_int TVPWindowWindow::GetZoomDenom() const
{
#ifdef KRKRSDL2_ENABLE_ZOOM
	return this->ZoomDenom;
#else
	return 1;
#endif
}

void TVPWindowWindow::SetInnerWidth(tjs_int v)
{
#ifdef KRKRSDL2_ENABLE_ZOOM
	this->SetInnerSize(v, this->GetInnerHeight());
#else
	this->SetWidth(v);
#endif
}

void TVPWindowWindow::SetInnerHeight(tjs_int v)
{
#ifdef KRKRSDL2_ENABLE_ZOOM
	this->SetInnerSize(this->GetInnerWidth(), v);
#else
	this->SetHeight(v);
#endif
}

void TVPWindowWindow::SetInnerSize(tjs_int w, tjs_int h)
{
#ifdef KRKRSDL2_ENABLE_ZOOM
	this->InnerWidth = w;
	this->InnerHeight = h;
	this->UpdateActualZoom();
#endif
	this->SetSize(w, h);
}

tjs_int TVPWindowWindow::GetInnerWidth()
{
#ifdef KRKRSDL2_ENABLE_ZOOM
	return this->InnerWidth;
#else
	return this->GetWidth();
#endif
}

tjs_int TVPWindowWindow::GetInnerHeight()
{
#ifdef KRKRSDL2_ENABLE_ZOOM
	return this->InnerHeight;
#else
	return this->GetHeight();
#endif
}

#ifdef _WIN32
void TVPWindowWindow::RegisterWindowMessageReceiver(tTVPWMRRegMode mode, void *proc, const void *userdata)
{
	switch (mode)
	{
		case wrmRegister:
		case wrmUnregister:
		{
			tjs_int count = this->WindowMessageReceivers.GetCount();
			tjs_int i;
			for (i = 0; i < count; i += 1)
			{
				tTVPMessageReceiverRecord *item = this->WindowMessageReceivers[i];
				if (!item)
				{
					continue;
				}
				if ((void*)item->Proc == proc)
				{
					if (mode == wrmRegister)
					{
						break; // have already registered
					}
					if (mode == wrmUnregister)
					{
						// found
						this->WindowMessageReceivers.Remove(i);
						delete item;
					}
				}
			}
			if (mode == wrmRegister)
			{
				if (i == count)
				{
					// not have registered
					tTVPMessageReceiverRecord *item = new tTVPMessageReceiverRecord();
					item->Proc = (tTVPWindowMessageReceiver)proc;
					item->UserData = userdata;
					this->WindowMessageReceivers.Add(item);
				}
			}
			if (mode == wrmUnregister)
			{
				this->WindowMessageReceivers.Compact();
			}
			break;
		}
		default:
			break;
	}
}

bool TVPWindowWindow::InternalDeliverMessageToReceiver(tTVPWindowMessage &msg)
{
	if (!this->WindowMessageReceivers.GetCount() || !this->TJSNativeInstance)
	{
		return false;
	}
#ifdef KRKRSDL2_ENABLE_PLUGINS
	if (TVPPluginUnloadedAtSystemExit)
	{
		return false;
	}
#endif

	tObjectListSafeLockHolder<tTVPMessageReceiverRecord> holder(this->WindowMessageReceivers);
	tjs_int count = this->WindowMessageReceivers.GetSafeLockedObjectCount();

	bool block = false;
	for (tjs_int i = 0; i < count; i += 1)
	{
		tTVPMessageReceiverRecord *item = this->WindowMessageReceivers.GetSafeLockedObjectAt(i);
		if (!item)
		{
			continue;
		}
		bool b = item->Deliver(&msg);
		block = block || b;
	}
	return block;
}

HWND TVPWindowWindow::GetHandle() const
{
	SDL_SysWMinfo syswminfo;
	SDL_VERSION(&syswminfo.version);
	return SDL_GetWindowWMInfo(this->window, &syswminfo) ? syswminfo.info.win.window : nullptr;
}
#endif

bool TVPWindowWindow::should_try_parent_window(SDL_Event event)
{
	bool tryParentWindow = false;
	if (this->window && this->_prevWindow)
	{
		uint32_t windowID = SDL_GetWindowID(this->window);
		switch (event.type)
		{
			case SDL_DROPFILE:
			case SDL_DROPTEXT:
			case SDL_DROPBEGIN:
			case SDL_DROPCOMPLETE:
				tryParentWindow = event.drop.windowID != windowID;
				break;
			case SDL_KEYDOWN:
			case SDL_KEYUP:
				tryParentWindow = event.key.windowID != windowID;
				break;
			case SDL_MOUSEMOTION:
				tryParentWindow = event.motion.windowID != windowID;
				break;
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
				tryParentWindow = event.button.windowID != windowID;
				break;
			case SDL_MOUSEWHEEL:
				tryParentWindow = event.wheel.windowID != windowID;
				break;
			case SDL_TEXTEDITING:
				tryParentWindow = event.edit.windowID != windowID;
				break;
			case SDL_TEXTINPUT:
				tryParentWindow = event.text.windowID != windowID;
				break;
			case SDL_WINDOWEVENT:
				tryParentWindow = event.window.windowID != windowID;
				break;
			default:
				break;
		}
	}
	return tryParentWindow;
}

void TVPWindowWindow::window_receive_event(SDL_Event event)
{
	if (this->isBeingDeleted)
	{
		delete this;
		return;
	}
	if (this->should_try_parent_window(event))
	{
		if (!this->in_mode_)
		{
			this->_prevWindow->window_receive_event(event);
		}
		return;
	}
	if (this->window && this->hasDrawn)
	{
		tjs_uint32 s = TVP_TShiftState_To_uint32(GetShiftState());
		s |= GetMouseButtonState();
		if (this->TJSNativeInstance->CanDeliverEvents())
		{
			switch (event.type)
			{
				case SDL_TEXTINPUT:
				case SDL_TEXTEDITING:
				{
					if (!SDL_IsTextInputActive())
					{
						return;
					}
					// TODO: figure out vertical edit
					for (size_t i = 0; i < this->imeCompositionSelection; i += 1)
					{
						TVPPostInputEvent(new tTVPOnKeyDownInputEvent(this->TJSNativeInstance, VK_LEFT, TVP_SS_SHIFT));
						TVPPostInputEvent(new tTVPOnKeyUpInputEvent(this->TJSNativeInstance, VK_LEFT, TVP_SS_SHIFT));
#if 0
						TVPPostInputEvent(new tTVPOnKeyDownInputEvent(this->TJSNativeInstance, VK_DOWN, TVP_SS_SHIFT));
						TVPPostInputEvent(new tTVPOnKeyUpInputEvent(this->TJSNativeInstance, VK_DOWN, TVP_SS_SHIFT));
#endif
					}
					for (size_t i = 0; i < this->imeCompositionLen - this->imeCompositionCursor; i += 1)
					{
						TVPPostInputEvent(new tTVPOnKeyDownInputEvent(this->TJSNativeInstance, VK_RIGHT, 0));
						TVPPostInputEvent(new tTVPOnKeyUpInputEvent(this->TJSNativeInstance, VK_RIGHT, 0));
#if 0
						TVPPostInputEvent(new tTVPOnKeyDownInputEvent(this->TJSNativeInstance, VK_DOWN, 0));
						TVPPostInputEvent(new tTVPOnKeyUpInputEvent(this->TJSNativeInstance, VK_DOWN, 0));
#endif
					}
					for (size_t i = 0; i < this->imeCompositionLen; i += 1)
					{
						TVPPostInputEvent(new tTVPOnKeyDownInputEvent(this->TJSNativeInstance, VK_BACK, 0));
						TVPPostInputEvent(new tTVPOnKeyUpInputEvent(this->TJSNativeInstance, VK_BACK, 0));
					}
					switch (event.type)
					{
						case SDL_TEXTINPUT:
							this->imeCompositionStr = event.text.text;
							this->imeCompositionCursor = 0;
							this->imeCompositionSelection = 0;
							break;
						case SDL_TEXTEDITING:
							this->imeCompositionStr = event.edit.text;
							this->imeCompositionCursor = event.edit.start;
							this->imeCompositionSelection = event.edit.length;
							break;
					}
					size_t buffer_len = TVPUtf8ToWideCharString((const char*)(this->imeCompositionStr), nullptr);
					if (buffer_len == (size_t)-1)
					{
						return;
					}
					if (buffer_len)
					{
						this->imeCompositionLen = buffer_len;
						tjs_char *buffer = new tjs_char[buffer_len + 1];
						TVPUtf8ToWideCharString((const char*)(this->imeCompositionStr), buffer);
						for (size_t i = 0; i < buffer_len; i += 1)
						{
							TVPPostInputEvent(new tTVPOnKeyPressInputEvent(this->TJSNativeInstance, buffer[i]));
						}
						delete[] buffer;
					}
					else
					{
						this->imeCompositionStr = nullptr;
						this->imeCompositionLen = 0;
						this->imeCompositionCursor = 0;
						this->imeCompositionSelection = 0;
					}
					if (event.type == SDL_TEXTEDITING)
					{
						for (size_t i = 0; i < this->imeCompositionLen - this->imeCompositionCursor; i += 1)
						{
							TVPPostInputEvent(new tTVPOnKeyDownInputEvent(this->TJSNativeInstance, VK_LEFT, 0));
							TVPPostInputEvent(new tTVPOnKeyUpInputEvent(this->TJSNativeInstance, VK_LEFT, 0));
#if 0
							TVPPostInputEvent(new tTVPOnKeyDownInputEvent(this->TJSNativeInstance, VK_UP, 0));
							TVPPostInputEvent(new tTVPOnKeyUpInputEvent(this->TJSNativeInstance, VK_UP, 0));
#endif
						}
						for (size_t i = 0; i < this->imeCompositionSelection; i += 1)
						{
							TVPPostInputEvent(new tTVPOnKeyDownInputEvent(this->TJSNativeInstance, VK_RIGHT, TVP_SS_SHIFT));
							TVPPostInputEvent(new tTVPOnKeyUpInputEvent(this->TJSNativeInstance, VK_RIGHT, TVP_SS_SHIFT));
#if 0
							TVPPostInputEvent(new tTVPOnKeyDownInputEvent(this->TJSNativeInstance, VK_DOWN, TVP_SS_SHIFT));
							TVPPostInputEvent(new tTVPOnKeyUpInputEvent(this->TJSNativeInstance, VK_DOWN, TVP_SS_SHIFT));
#endif
						}
					}
					if (event.type == SDL_TEXTINPUT)
					{
						this->imeCompositionStr = nullptr;
						this->imeCompositionLen = 0;
					}
					return;
				}
				case SDL_DROPBEGIN:
				{
					if (!this->fileDropArray)
					{
						this->fileDropArray = TJSCreateArrayObject();
					}
					return;
				}
				case SDL_DROPCOMPLETE:
				{
					if (this->fileDropArray)
					{
						tTJSVariant arg(this->fileDropArray, this->fileDropArray);
						TVPPostInputEvent(new tTVPOnFileDropInputEvent(this->TJSNativeInstance, arg));
						this->fileDropArray->Release();
						this->fileDropArray = nullptr;
						this->fileDropArrayCount = 0;
					}
					return;
				}
				case SDL_DROPFILE:
				case SDL_DROPTEXT:
				{
					if (event.drop.file)
					{
						std::string f_utf8 = event.drop.file;
						tjs_string f_utf16;
						TVPUtf8ToUtf16(f_utf16, f_utf8);
						SDL_free(event.drop.file);
						if (TVPIsExistentStorageNoSearch(f_utf16))
						{
							tTJSVariant val = TVPNormalizeStorageName(ttstr(f_utf16));
							if (this->fileDropArray)
							{
								this->fileDropArray->PropSetByNum(TJS_MEMBERENSURE|TJS_IGNOREPROP, this->fileDropArrayCount, &val, this->fileDropArray);
								this->fileDropArrayCount += 1;
							}
							else
							{
								iTJSDispatch2 *file_drop_array_single = TJSCreateArrayObject();
								file_drop_array_single->PropSetByNum(TJS_MEMBERENSURE|TJS_IGNOREPROP, 0, &val, file_drop_array_single);
								{
									tTJSVariant arg(file_drop_array_single, file_drop_array_single);
									TVPPostInputEvent(new tTVPOnFileDropInputEvent(this->TJSNativeInstance, arg));
								}
								file_drop_array_single->Release();
							}
						}
					}
					return;
				}
				case SDL_CONTROLLERDEVICEADDED:
				case SDL_CONTROLLERDEVICEREMOVED:
				case SDL_CONTROLLERDEVICEREMAPPED:
				{
					refresh_controllers();
					return;
				}
				case SDL_WINDOWEVENT:
				{
					switch (event.window.event)
					{
						case SDL_WINDOWEVENT_EXPOSED:
						{
							this->UpdateWindow(utNormal);
							return;
						}
						case SDL_WINDOWEVENT_MINIMIZED:
						case SDL_WINDOWEVENT_MAXIMIZED:
						case SDL_WINDOWEVENT_RESTORED:
						case SDL_WINDOWEVENT_RESIZED:
						case SDL_WINDOWEVENT_SIZE_CHANGED:
						{
#ifdef KRKRSDL2_ENABLE_ZOOM
							this->UpdateActualZoom();
#else
							this->UpdateWindow(utNormal);
#endif
							TVPPostInputEvent(new tTVPOnResizeInputEvent(this->TJSNativeInstance), TVP_EPT_REMOVE_POST);
							return;
						}
						case SDL_WINDOWEVENT_ENTER:
						{
							TVPPostInputEvent(new tTVPOnMouseEnterInputEvent(this->TJSNativeInstance));
							return;
						}
						case SDL_WINDOWEVENT_LEAVE:
						{
							TVPPostInputEvent(new tTVPOnMouseOutOfWindowInputEvent(this->TJSNativeInstance));
							TVPPostInputEvent(new tTVPOnMouseLeaveInputEvent(this->TJSNativeInstance));
							return;
						}
						case SDL_WINDOWEVENT_FOCUS_GAINED:
						case SDL_WINDOWEVENT_FOCUS_LOST:
						{
							TVPPostInputEvent(new tTVPOnWindowActivateEvent(this->TJSNativeInstance, event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED), TVP_EPT_REMOVE_POST);
							return;
						}
						case SDL_WINDOWEVENT_CLOSE:
						{
							TVPPostInputEvent(new tTVPOnCloseInputEvent(this->TJSNativeInstance));
							return;
						}
						default:
						{
							return;
						}
					}
				}
				case SDL_QUIT:
				{
					TVPPostInputEvent(new tTVPOnCloseInputEvent(this->TJSNativeInstance));
					return;
				}
				default:
				{
#if !(defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__))
					this->window_receive_event_input(event);
#endif
					return;
				}
			}
		}
	}
}

bool TVPWindowWindow::window_receive_event_input(SDL_Event event)
{
	if (this->isBeingDeleted)
	{
		delete this;
		return false;
	}
	if (this->should_try_parent_window(event))
	{
		if (!this->in_mode_)
		{
			return this->_prevWindow->window_receive_event_input(event);
		}
		return false;
	}
	if (this->window && this->hasDrawn)
	{
		tjs_uint32 s = TVP_TShiftState_To_uint32(GetShiftState());
		s |= GetMouseButtonState();
		if (this->TJSNativeInstance->CanDeliverEvents())
		{
			switch (event.type)
			{
				case SDL_MOUSEMOTION:
				{
					this->RestoreMouseCursor();
					this->lastMouseX = event.motion.x;
					this->lastMouseY = event.motion.y;
					this->TranslateWindowToDrawArea(this->lastMouseX, this->lastMouseY);
					TVPPostInputEvent(new tTVPOnMouseMoveInputEvent(this->TJSNativeInstance, this->lastMouseX, this->lastMouseY, s));
					return true;
				}
				case SDL_MOUSEBUTTONDOWN:
				case SDL_MOUSEBUTTONUP:
				{
					if (SDL_IsTextInputActive() && this->imeCompositionStr)
					{
						return false;
					}
					tTVPMouseButton btn;
					bool hasbtn = true;
					switch (event.button.button)
					{
						case SDL_BUTTON_RIGHT:
							btn = tTVPMouseButton::mbRight;
							break;
						case SDL_BUTTON_MIDDLE:
							btn = tTVPMouseButton::mbMiddle;
							break;
						case SDL_BUTTON_LEFT:
							btn = tTVPMouseButton::mbLeft;
							break;
						case SDL_BUTTON_X1:
							btn = tTVPMouseButton::mbX1;
							break;
						case SDL_BUTTON_X2:
							btn = tTVPMouseButton::mbX2;
							break;
						default:
							hasbtn = false;
							break;
					}
					if (hasbtn)
					{
						this->lastMouseX = event.button.x;
						this->lastMouseY = event.button.y;
						this->TranslateWindowToDrawArea(this->lastMouseX, this->lastMouseY);
						TVPPostInputEvent(new tTVPOnMouseMoveInputEvent(this->TJSNativeInstance, this->lastMouseX, this->lastMouseY, s));
						switch (event.type)
						{
							case SDL_MOUSEBUTTONDOWN:
								TVPPostInputEvent(new tTVPOnMouseDownInputEvent(this->TJSNativeInstance, this->lastMouseX, this->lastMouseY, btn, s));
								break;
							case SDL_MOUSEBUTTONUP:
								if (event.button.clicks >= 2)
								{
									TVPPostInputEvent(new tTVPOnDoubleClickInputEvent(this->TJSNativeInstance, this->lastMouseX, this->lastMouseY));
								}
								else
								{
									TVPPostInputEvent(new tTVPOnClickInputEvent(this->TJSNativeInstance, this->lastMouseX, this->lastMouseY));
								}
								TVPPostInputEvent(new tTVPOnMouseUpInputEvent(this->TJSNativeInstance, this->lastMouseX, this->lastMouseY, btn, s));
								break;
						}
						return true;
					}
					return false;
				}
				case SDL_MOUSEWHEEL:
				{
					this->TranslateWindowToDrawArea(this->lastMouseX, this->lastMouseY);
					TVPPostInputEvent(new tTVPOnMouseWheelInputEvent(this->TJSNativeInstance, event.wheel.x, event.wheel.y, this->lastMouseX, this->lastMouseY));
					return true;
				}
				case SDL_FINGERMOTION:
				{
					TVPPostInputEvent(new tTVPOnTouchMoveInputEvent(this->TJSNativeInstance, event.tfinger.x, event.tfinger.y, 1, 1, event.tfinger.fingerId));
					return true;
				}
				case SDL_FINGERDOWN:
				case SDL_FINGERUP:
				{
					switch (event.tfinger.type)
					{
						case SDL_FINGERDOWN:
							TVPPostInputEvent(new tTVPOnTouchDownInputEvent(this->TJSNativeInstance, event.tfinger.x, event.tfinger.y, 1, 1, event.tfinger.fingerId));
							break;
						case SDL_FINGERUP:
							TVPPostInputEvent(new tTVPOnTouchUpInputEvent(this->TJSNativeInstance, event.tfinger.x, event.tfinger.y, 1, 1, event.tfinger.fingerId));
							break;
					}
					return true;
				}
				case SDL_MULTIGESTURE:
				{
					TVPPostInputEvent(new tTVPOnTouchScalingInputEvent(this->TJSNativeInstance, 0, event.mgesture.dDist, event.mgesture.x, event.mgesture.y, 0));
					TVPPostInputEvent(new tTVPOnTouchRotateInputEvent(this->TJSNativeInstance, 0, event.mgesture.dTheta, event.mgesture.dDist, event.mgesture.x, event.mgesture.y, 0));
					return true;
				}
				case SDL_CONTROLLERBUTTONDOWN:
				case SDL_CONTROLLERBUTTONUP:
				{
					switch (event.cbutton.state)
					{
						case SDL_PRESSED:
							TVPPostInputEvent(new tTVPOnKeyDownInputEvent(this->TJSNativeInstance, sdl_gamecontrollerbutton_to_vk_key(event.cbutton.button), s));
							break;
						case SDL_RELEASED:
							if (!SDL_IsTextInputActive())
							{
								TVPPostInputEvent(new tTVPOnKeyPressInputEvent(this->TJSNativeInstance, sdl_gamecontrollerbutton_to_vk_key(event.cbutton.button)));
							}
							TVPPostInputEvent(new tTVPOnKeyUpInputEvent(this->TJSNativeInstance, sdl_gamecontrollerbutton_to_vk_key(event.cbutton.button), s));
							break;
					}
					return true;
				}
				case SDL_KEYDOWN:
				{
					if (SDL_IsTextInputActive())
					{
						if (this->imeCompositionStr)
						{
							return false;
						}
					}
					if (event.key.repeat)
					{
						s |= TVP_SS_REPEAT;
					}
					tjs_uint unified_vk_key = 0;
					switch (event.key.keysym.sym)
					{
						case SDLK_LSHIFT:
						case SDLK_RSHIFT:
							unified_vk_key = VK_SHIFT;
							break;
						case SDLK_LCTRL:
						case SDLK_RCTRL:
							unified_vk_key = VK_CONTROL;
							break;
						case SDLK_LALT:
						case SDLK_RALT:
							unified_vk_key = VK_MENU;
							break;
					}
					TVPPostInputEvent(new tTVPOnKeyDownInputEvent(this->TJSNativeInstance, sdl_key_to_vk_key(event.key.keysym.sym), s));
					if (unified_vk_key)
					{
						TVPPostInputEvent(new tTVPOnKeyDownInputEvent(this->TJSNativeInstance, unified_vk_key, s));
					}
					SDL_SetTextInputRect(&this->attentionPointRect);
					return true;
				}
				case SDL_KEYUP:
				{
					if (SDL_IsTextInputActive())
					{
						if (this->imeCompositionStr)
						{
							return false;
						}
					}
					tjs_uint unified_vk_key = 0;
					switch (event.key.keysym.sym)
					{
						case SDLK_LSHIFT:
						case SDLK_RSHIFT:
							unified_vk_key = VK_SHIFT;
							break;
						case SDLK_LCTRL:
						case SDLK_RCTRL:
							unified_vk_key = VK_CONTROL;
							break;
						case SDLK_LALT:
						case SDLK_RALT:
							unified_vk_key = VK_MENU;
							break;
					}
					if (!SDL_IsTextInputActive())
					{
						TVPPostInputEvent(new tTVPOnKeyPressInputEvent(this->TJSNativeInstance, sdl_key_to_vk_key(event.key.keysym.sym)));
						if (unified_vk_key)
						{
							TVPPostInputEvent(new tTVPOnKeyPressInputEvent(this->TJSNativeInstance, unified_vk_key));
						}
					}
					TVPPostInputEvent(new tTVPOnKeyUpInputEvent(this->TJSNativeInstance, sdl_key_to_vk_key(event.key.keysym.sym), s));
					if (unified_vk_key)
					{
						TVPPostInputEvent(new tTVPOnKeyUpInputEvent(this->TJSNativeInstance, unified_vk_key, s));
					}
					SDL_SetTextInputRect(&this->attentionPointRect);
					return true;
				}
				default:
				{
					return false;
				}
			}
		}
	}
	return false;
}

#ifdef __SWITCH__
void TVPWindowWindow::switch_process_gamepad_input()
{
	if (isBeingDeleted || !window || !hasDrawn)
	{
		return;
	}
	if (!sdl_controllers)
	{
		refresh_controllers();
	}
	if (!sdl_controllers || !sdl_controller_num)
	{
		return;
	}
	if (ns_gp.mouse_x < 0)
	{
		int w = 0, h = 0;
		SDL_GetWindowSize(window, &w, &h);
		ns_gp.mouse_x = (w > 0) ? w / 2 : 640;
		ns_gp.mouse_y = (h > 0) ? h / 2 : 360;
		ns_gp_push_mouse_motion(window, ns_gp.mouse_x, ns_gp.mouse_y);
	}
	Uint16 buttons = 0;
	int ax = 0, ay = 0;
	for (int i = 0; i < sdl_controller_num; i += 1)
	{
		SDL_GameController *c = sdl_controllers[i];
		if (!c)
		{
			continue;
		}
		buttons |= ns_gp_read_buttons(c);
		int cax = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
		int cay = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
		if (abs(cax) > abs(ax)) ax = cax;
		if (abs(cay) > abs(ay)) ay = cay;
	}
	// left stick -> cursor motion (window pixel coords)
	if (abs(ax) >= NS_GP_DEADZONE || abs(ay) >= NS_GP_DEADZONE)
	{
		int w = 0, h = 0;
		SDL_GetWindowSize(window, &w, &h);
		if (abs(ax) >= NS_GP_DEADZONE) ns_gp.mouse_x += (int)((float)ax / 32768.0f * NS_GP_STICK_PX_PER_FRAME);
		if (abs(ay) >= NS_GP_DEADZONE) ns_gp.mouse_y += (int)((float)ay / 32768.0f * NS_GP_STICK_PX_PER_FRAME);
		if (w > 0)
		{
			if (ns_gp.mouse_x < 0) ns_gp.mouse_x = 0;
			else if (ns_gp.mouse_x >= w) ns_gp.mouse_x = w - 1;
		}
		if (h > 0)
		{
			if (ns_gp.mouse_y < 0) ns_gp.mouse_y = 0;
			else if (ns_gp.mouse_y >= h) ns_gp.mouse_y = h - 1;
		}
		ns_gp_push_mouse_motion(window, ns_gp.mouse_x, ns_gp.mouse_y);
	}
	// dpad -> cursor nudge with auto-repeat
	Uint32 now = SDL_GetTicks();
	{
		static const int step_x[4] = { 0, 0, -1, 1 };
		static const int step_y[4] = { -1, 1, 0, 0 };
		for (int d = 0; d < 4; d += 1)
		{
			Uint16 bit = (Uint16)(1 << (NS_GP_BTN_DPAD_UP + d));
			if (buttons & bit)
			{
				if (ns_gp.dpad_repeat_until[d] <= now)
				{
					ns_gp.mouse_x += step_x[d] * NS_GP_DPAD_STEP;
					ns_gp.mouse_y += step_y[d] * NS_GP_DPAD_STEP;
					ns_gp_push_mouse_motion(window, ns_gp.mouse_x, ns_gp.mouse_y);
					ns_gp.dpad_repeat_until[d] = now + NS_GP_DPAD_REPEAT_MS;
				}
			}
			else
			{
				ns_gp.dpad_repeat_until[d] = 0;
			}
		}
	}
	// button edges -> synthetic mouse / key events
	Uint16 changed = (Uint16)(buttons ^ ns_gp_prev_buttons);
	ns_gp_prev_buttons = buttons;
	for (int b = 0; b < NS_GP_BTN_COUNT; b += 1)
	{
		Uint16 bit = (Uint16)(1 << b);
		if (!(changed & bit))
		{
			continue;
		}
		bool pressed = !!(buttons & bit);
		switch (b)
		{
			// IMPORTANT (NS vs Xbox face buttons): SDL_CONTROLLER_BUTTON_*
			// follows XBOX POSITION semantics (A = bottom, B = right,
			// X = left, Y = top). The physical Switch layout is mirrored:
			// NS A = bottom-RIGHT, NS B = bottom, NS X = top-RIGHT, NS Y =
			// top-LEFT. The synthesized actions are therefore swapped
			// relative to the SDL labels to give NS-natural behavior:
			//   NS A (right) = left click, NS B (bottom) = right click,
			//   NS Y (left) = Space, NS X (top-right) = Enter.
			case NS_GP_BTN_A: // SDL A (Xbox bottom) = NS B = right click = menu / back
				ns_gp.right_down = pressed;
				ns_gp_push_mouse_button(window, pressed ? SDL_PRESSED : SDL_RELEASED, SDL_BUTTON_RIGHT);
				break;
			case NS_GP_BTN_B: // SDL B (Xbox right) = NS A = left click = confirm/advance
				ns_gp.left_down = pressed;
				ns_gp_push_mouse_button(window, pressed ? SDL_PRESSED : SDL_RELEASED, SDL_BUTTON_LEFT);
				break;
			case NS_GP_BTN_X: // SDL X (Xbox left) = NS Y (top-left) = Space
				ns_gp_push_key(window, pressed ? SDL_PRESSED : SDL_RELEASED, SDL_SCANCODE_SPACE);
				break;
			case NS_GP_BTN_Y: // SDL Y (Xbox top) = NS X (top-right) = Enter
				ns_gp_push_key(window, pressed ? SDL_PRESSED : SDL_RELEASED, SDL_SCANCODE_RETURN);
				break;
			case NS_GP_BTN_BACK: // Minus = F5 (quick save)
				ns_gp_push_key(window, pressed ? SDL_PRESSED : SDL_RELEASED, SDL_SCANCODE_F5);
				break;
			case NS_GP_BTN_START: // Plus = Escape (system menu / cancel)
				ns_gp_push_key(window, pressed ? SDL_PRESSED : SDL_RELEASED, SDL_SCANCODE_ESCAPE);
				break;
			case NS_GP_BTN_LSTICK: // L3 = Ctrl (hold to skip)
				ns_gp_push_key(window, pressed ? SDL_PRESSED : SDL_RELEASED, SDL_SCANCODE_LCTRL);
				break;
			case NS_GP_BTN_RSTICK: // R3 = F7 (quick load)
				ns_gp_push_key(window, pressed ? SDL_PRESSED : SDL_RELEASED, SDL_SCANCODE_F7);
				break;
			case NS_GP_BTN_LSHOULDER: // L = wheel up (back log)
				if (pressed) ns_gp_push_mouse_wheel(window, 1);
				break;
			case NS_GP_BTN_RSHOULDER: // R = wheel down (advance)
				if (pressed) ns_gp_push_mouse_wheel(window, -1);
				break;
			default:
				break;
		}
	}
}
#endif

void sdl_process_events()
{
	if (!SDL_WasInit(SDL_INIT_EVENTS))
	{
		return;
	}
	// KRKR-ns: events posted by timer/limiter threads are dispatched here on
	// the main thread (console SDL is single-threaded for events).
	krkrsdl2_drain_pending_native_events();
#ifdef __SWITCH__
	if (_currentWindowWindow)
	{
		_currentWindowWindow->switch_process_gamepad_input();
	}
#endif
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		if (event.type == SDL_CONTROLLERDEVICEADDED || event.type == SDL_CONTROLLERDEVICEREMOVED)
		{
			refresh_controllers();
		}
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE)
#else
		if (event.type == NativeEventQueueImplement::native_event_queue_custom_event_type)
		{
			// Wake-only markers carry no payload; real events are drained
			// from the pending list at the top of sdl_process_events().
			if (event.user.code == 0 && event.user.data2)
				((NativeEvent*)event.user.data2)->HandleEvent();
		}
		else
#endif
		if (_currentWindowWindow)
		{
			_currentWindowWindow->window_receive_event(event);
		}
		else if (event.type == SDL_QUIT)
		{
			Application->Terminate();
		}
	}
}

#ifdef _WIN32
static void sdl_windows_message_hook(void *userdata, void *hWnd, unsigned int message, Uint64 wParam, Sint64 lParam)
{
	TVPWindowWindow *win = reinterpret_cast<TVPWindowWindow*>(::GetWindowLongPtr((HWND)hWnd, GWLP_USERDATA));
	tTVPWindowMessage Message;
	Message.LParam = lParam;
	Message.WParam = wParam;
	Message.Msg = message;
	Message.Result = 0;
	if (win && win->InternalDeliverMessageToReceiver(Message))
	{
		// TODO: return Message.result and block
	}
}
#endif

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
static int sdl_event_watch(void *userdata, SDL_Event *in_event)
{
	SDL_Event event;
	SDL_memcpy(&event, in_event, sizeof(SDL_Event));
#if defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE)
#else
	if (event.type == NativeEventQueueImplement::native_event_queue_custom_event_type)
	{
		return 1;
	}
#endif
	if (_currentWindowWindow && _currentWindowWindow->window_receive_event_input(event) && TVPSystemControl)
	{
		// process events now
		// Some JS functions will only work in e.g. mouse down callback due to browser restrictions
		TVPSystemControl->ApplicationIdle();
	}
	return 1;
}
#endif

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
static void process_events()
#else
static bool process_events()
#endif
{
	try
	{
		try
		{
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
			tTJSNI_WaveSoundBuffer::Trigger();
			tTVPTimerThread::Trigger();
#endif
			::Application->Run();
			if (::Application->IsTarminate())
			{
				TVPSystemUninit();
				if (TVPSystemControl)
				{
					delete TVPSystemControl;
					TVPSystemControl = nullptr;
				}
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
				emscripten_cancel_main_loop();
#else
				return false;
#endif
			}
		}
		TJS_CONVERT_TO_TJS_EXCEPTION
	}
	TVP_CATCH_AND_SHOW_SCRIPT_EXCEPTION(TJS_W("SDL event processing"));

#if !defined(__EMSCRIPTEN__) || (defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__))
	return true;
#endif
}

void krkrsdl2_pre_init_platform(void)
{
	TVPTerminateCode = 0;
#ifdef _WIN32
	_set_error_mode(_OUT_TO_STDERR);
#endif
}

void krkrsdl2_set_args(int argc, tjs_char **argv)
{
	_argc = argc;
	_wargv = argv;
}

void krkrsdl2_convert_set_args(int argc, char **argv)
{
	_argc = argc;
	_wargv = new tjs_char*[argc];

	for (int i = 0; i < argc; i += 1)
	{
		const char *narg;
#if !defined(__EMSCRIPTEN__) && !defined(__vita__) && !defined(__SWITCH__) && !defined(_WIN32)
		if (!i)
		{
			narg = realpath(argv[i], nullptr);
		}
		else
#endif
		{
			narg = argv[i];
		}
		if (!narg)
		{
			tjs_char *warg_copy = new tjs_char[1];
			warg_copy[0] = '\0';
			_wargv[i] = warg_copy;
			continue;
		}
		std::string v_utf8 = narg;
		tjs_string v_utf16;
		TVPUtf8ToUtf16(v_utf16, v_utf8);
#if !defined(__EMSCRIPTEN__) && !defined(__vita__) && !defined(__SWITCH__) && !defined(_WIN32)
		if (!i)
		{
			free((void*)narg);
		}
#endif
		tjs_char *warg_copy = new tjs_char[v_utf16.length() + 1];
		SDL_memcpy(warg_copy, v_utf16.c_str(), sizeof(tjs_char) * (v_utf16.length()));
		warg_copy[v_utf16.length()] = '\0';
		_wargv[i] = warg_copy;
	}
}


#ifdef __SWITCH__
// game-mode globals (used by StorageImpl.cpp GetLocalName)
bool krkrsdl2_game_mode = false;
ttstr krkrsdl2_game_dir(TJS_W("sdmc:/krkr/")); // non-const: extern-linked
static const char *krkrsdl2_game_root = "sdmc:/krkr";

static void krkrsdl2_get_process_memory(u64 &total, u64 &used)
{
	total = 0;
	used = 0;
	if (R_FAILED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))) total = 0;
	if (R_FAILED(svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0))) used = 0;
}

// Album launches homebrew as a LibraryApplet with a much smaller memory
// budget.  Large KRKR games need application/title-override mode.  Some
// emulators expose an application-sized budget while reporting a generic
// applet type, so accept an independently verified >= 1 GiB process budget.
bool krkrsdl2_can_launch_game()
{
	const AppletType type = appletGetAppletType();
	u64 total = 0, used = 0;
	krkrsdl2_get_process_memory(total, used);
	const bool application = type == AppletType_Application || type == AppletType_SystemApplication;
	const bool enough_memory = total >= 1024ULL * 1024 * 1024;
	KRKRNS_LOG("[mem] launch check: applet=%d total=%llu used=%llu allowed=%d",
		static_cast<int>(type),
		static_cast<unsigned long long>(total),
		static_cast<unsigned long long>(used),
		application || enough_memory);
	return application || enough_memory;
}

static bool krkrsdl2_has_extension(const std::string &name, const char *extension)
{
	const size_t ext_len = strlen(extension);
	if (name.size() < ext_len) return false;
	return strcasecmp(name.c_str() + name.size() - ext_len, extension) == 0;
}

static bool krkrsdl2_safe_path_component(const std::string &name)
{
	return !name.empty() && name != "." && name != ".." && name[0] != '.' &&
		name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
		name.find(':') == std::string::npos;
}

static bool krkrsdl2_is_directory(const std::string &path)
{
	struct stat info;
	return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

static std::vector<ttstr> krkrsdl2_sorted_names(
	std::vector<std::pair<std::string, ttstr>> &entries)
{
	std::stable_sort(entries.begin(), entries.end(),
		[](const auto &left, const auto &right) {
			return strcasecmp(left.first.c_str(), right.first.c_str()) < 0;
		});
	std::vector<ttstr> result;
	result.reserve(entries.size());
	for (const auto &entry : entries) result.push_back(entry.second);
	return result;
}

// Preserve the spelling stored on the SD card.  The generic KRKR storage
// lister intentionally lower-cases names for auto-path lookup, which is not
// appropriate for a user-facing browser or for opening an exact archive.
std::vector<ttstr> krkrsdl2_list_game_directories()
{
	std::vector<std::pair<std::string, ttstr>> entries;
	DIR *dir = opendir(krkrsdl2_game_root);
	if (!dir) return {};

	struct dirent *ent;
	while ((ent = readdir(dir)) != nullptr)
	{
		const std::string name(ent->d_name);
		if (!krkrsdl2_safe_path_component(name)) continue;
		if (!krkrsdl2_is_directory(std::string(krkrsdl2_game_root) + "/" + name)) continue;

		tjs_string name16;
		if (!TVPUtf8ToUtf16(name16, name))
		{
			KRKRNS_LOG("[launcher] skipped non-UTF8 directory: %s", name.c_str());
			continue;
		}
		entries.emplace_back(name, ttstr(name16));
	}
	closedir(dir);
	return krkrsdl2_sorted_names(entries);
}

std::vector<ttstr> krkrsdl2_list_game_files(const ttstr &game_directory)
{
	std::string directory8;
	if (!TVPUtf16ToUtf8(directory8, game_directory.AsStdString()) ||
		!krkrsdl2_safe_path_component(directory8)) return {};

	const std::string native_directory = std::string(krkrsdl2_game_root) + "/" + directory8;
	if (!krkrsdl2_is_directory(native_directory)) return {};
	std::vector<std::pair<std::string, ttstr>> entries;
	DIR *dir = opendir(native_directory.c_str());
	if (!dir) return {};

	struct dirent *ent;
	while ((ent = readdir(dir)) != nullptr)
	{
		const std::string name(ent->d_name);
		if (!krkrsdl2_safe_path_component(name)) continue;
		// The KRKR picker exposes scripts and XP3 archives, matching the
		// launchable files shown by Kirikiroid-style front ends.
		if (!krkrsdl2_has_extension(name, ".xp3") &&
			!krkrsdl2_has_extension(name, ".tjs")) continue;
		if (krkrsdl2_is_directory(native_directory + "/" + name)) continue;

		tjs_string name16;
		if (!TVPUtf8ToUtf16(name16, name))
		{
			KRKRNS_LOG("[launcher] skipped non-UTF8 name: %s", name.c_str());
			continue;
		}
		entries.emplace_back(name, ttstr(name16));
	}
	closedir(dir);
	return krkrsdl2_sorted_names(entries);
}

static ttstr krkrsdl2_archive_path(const ttstr &file)
{
	return krkrsdl2_game_dir + file + TJS_W(">");
}

// Select and prepare one exact XP3.  Sibling archives become resource paths,
// while the selected archive is mounted last and its own startup.tjs is
// executed explicitly by Storages.launchXP3.  Game data remains read-only;
// saves are redirected to a separate per-game SD directory.
ttstr krkrsdl2_prepare_xp3_game(const ttstr &game_directory, const ttstr &selected)
{
	if (krkrsdl2_game_mode)
		throw eTJSError(TJS_W("A KRKR game is already running"));

	std::string directory8;
	std::string selected8;
	if (!TVPUtf16ToUtf8(directory8, game_directory.AsStdString()) ||
		!krkrsdl2_safe_path_component(directory8))
		throw eTJSError(TJS_W("Invalid KRKR game directory"));
	if (!TVPUtf16ToUtf8(selected8, selected.AsStdString()) ||
		!krkrsdl2_safe_path_component(selected8) || !krkrsdl2_has_extension(selected8, ".xp3"))
		throw eTJSError(TJS_W("Invalid XP3 entry name"));

	const std::string native_game_dir = std::string(krkrsdl2_game_root) + "/" + directory8;
	if (!krkrsdl2_is_directory(native_game_dir))
		throw eTJSError(TJS_W("The selected KRKR game directory does not exist"));
	tjs_string game_dir16;
	if (!TVPUtf8ToUtf16(game_dir16, native_game_dir + "/"))
		throw eTJSError(TJS_W("Cannot encode the KRKR game directory"));
	krkrsdl2_game_dir = ttstr(game_dir16);

	const auto files = krkrsdl2_list_game_files(game_directory);
	bool found = false;
	bool selected_is_supported = false;
	std::vector<ttstr> archives;
	for (const auto &file : files)
	{
		std::string file8;
		if (!TVPUtf16ToUtf8(file8, file.AsStdString())) continue;
		if (!krkrsdl2_has_extension(file8, ".xp3")) continue;
		const bool is_selected = file == selected;
		if (is_selected) found = true;

		// Archive format is content-driven.  Kirikiroid-compatible games can
		// deliberately give a standard 7-Zip resource package an .xp3 suffix.
		// Accept both formats implemented by this runtime and skip only truly
		// unsupported siblings; game files remain untouched.
		const ttstr archive_storage = TVPNormalizeStorageName(krkrsdl2_game_dir + file);
		if (!TVPIsXP3Archive(archive_storage) && !TVPIs7ZArchive(archive_storage))
		{
			KRKRNS_LOG("[launcher] skipped unsupported archive sibling: %s", file8.c_str());
			continue;
		}
		archives.push_back(file);
		if (is_selected) selected_is_supported = true;
	}
	if (!found) throw eTJSError(TJS_W("The selected XP3 file does not exist"));
	if (!selected_is_supported) throw eTJSError(TJS_W("The selected file is not a supported archive"));

	// Non-selected resource packs first; the chosen entry archive has the
	// highest game-resource priority.  The engine patch folder stays last.
	for (const auto &file : archives)
		if (file != selected) TVPAddAutoPath(krkrsdl2_archive_path(file));
	TVPAddAutoPath(krkrsdl2_archive_path(selected));
	// The portable NRO contains the compatibility layer used by the verified
	// emulator build, so a real console only needs this NRO and untouched game
	// archives.  Keep the SD patch directory last so users can override a
	// bundled shim without rebuilding the application.
	TVPAddAutoPath(ttstr(TJS_W("file://?/romfs:/compat/system/")));
	mkdir("sdmc:/switch/krkrsdl2/patch", 0777);
	mkdir("sdmc:/switch/krkrsdl2/patch/system", 0777);
	TVPAddAutoPath(ttstr(TJS_W("sdmc:/switch/krkrsdl2/patch/system/")));

	mkdir("sdmc:/switch/krkrsdl2/saves", 0777);
	const std::string native_save = "sdmc:/switch/krkrsdl2/saves/" + directory8;
	mkdir(native_save.c_str(), 0777);
	tjs_string save16;
	if (!TVPUtf8ToUtf16(save16, native_save + "/"))
		throw eTJSError(TJS_W("Cannot encode the KRKR save path"));

	krkrsdl2_game_mode = true;
	TVPProjectDir = TVPNormalizeStorageName(krkrsdl2_game_dir);
	TVPNativeProjectDir = krkrsdl2_game_dir.AsStdString();
	TVPDataPath = TVPNormalizeStorageName(ttstr(save16));
	TVPNativeDataPath = save16;
	TVPSetCurrentDirectory(krkrsdl2_game_dir);
	chdir(native_game_dir.c_str());

	KRKRNS_LOG("[launcher] launching directory/file: %s/%s", directory8.c_str(), selected8.c_str());
	KRKRNS_LOG("[launcher] save path: %s/", native_save.c_str());
	return krkrsdl2_archive_path(selected) + TJS_W("startup.tjs");
}

// Called once before the normal startup-script resolver.  Always start the
// built-in browser; a game is mounted only after the user chooses an XP3.
#ifdef __SWITCH__
// KRKR-ns diagnostic heartbeat: proves the log thread keeps writing while the
// main thread is stuck (heartbeats continue) or stops (crashed).
static char krkrsdl2_stage_buf[2][224];
static volatile int krkrsdl2_stage_idx = 0;
static volatile unsigned krkrsdl2_stage_version = 0; // bumped on every set
static volatile unsigned long long krkrsdl2_main_tick_val = 0;
void krkrsdl2_heartbeat_main_progress() { krkrsdl2_main_tick_val++; }
unsigned long long krkrsdl2_heartbeat_main_tick() { return krkrsdl2_main_tick_val; }
static volatile unsigned krkrsdl2_frozen_version = 0; // last version seen by watchdog
static volatile int krkrsdl2_frozen_ticks = 0;       // watchdog consecutive same-version beats
static volatile bool krkrsdl2_watchdog_kick = false;
void krkrsdl2_set_stage(const char* stage)
{
    const int next = krkrsdl2_stage_idx ^ 1;
    char* dst = krkrsdl2_stage_buf[next];
    int i = 0;
    for (; stage[i] && i < 200; ++i) dst[i] = stage[i];
    dst[i] = 0;
    krkrsdl2_stage_idx = next;
    krkrsdl2_stage_version++;
}
const char* krkrsdl2_get_stage() { return krkrsdl2_stage_buf[krkrsdl2_stage_idx]; }

static int krkrsdl2_heartbeat_thread(void* /*unused*/)
{
    int n = 0;
    unsigned long long lastMain = 0;
    int mainStill = 0;
    while (true)
    {
        SDL_Delay(3000);
        const unsigned long long nowMain = krkrsdl2_heartbeat_main_tick();
        if (nowMain == lastMain) ++mainStill; else mainStill = 0;
        lastMain = nowMain;
        KRKRNS_LOG("[heartbeat] alive %d stage=[%s] main=%s",
                   ++n, krkrsdl2_get_stage(),
                   mainStill >= 2 ? "STALLED" : "ok");
        if (mainStill >= 2)
        {
            KRKRNS_LOG("[heartbeat] MAIN THREAD STALLED for at least %d seconds "
                       "-- last stage=%s, tick=%llu",
                       mainStill * 3, krkrsdl2_get_stage(),
                       (unsigned long long)lastMain);
        }
        if (krkrsdl2_stage_version == krkrsdl2_frozen_version)
        {
            ++krkrsdl2_frozen_ticks;
        }
        else
        {
            krkrsdl2_frozen_version = krkrsdl2_stage_version;
            krkrsdl2_frozen_ticks = 0;
            krkrsdl2_watchdog_kick = false;
        }
    }
    return 0;
}



static void krkrsdl2_start_heartbeat()
{
    static bool started = false;
    if (started) return;
    started = true;
    SDL_Thread* thread = SDL_CreateThread(krkrsdl2_heartbeat_thread, "krkr-heartbeat", nullptr);
    (void)thread; // detached by design; lives for the process lifetime
}
#endif

void switch_game_mount(void)
{
	mkdir("sdmc:/switch/krkrsdl2", 0777);
	mkdir(krkrsdl2_game_root, 0777);
	krkrsdl2_game_mode = false;
	TVPStartupScriptName = ttstr(TJS_W("file://?/romfs:/startup.tjs"));
	const auto directories = krkrsdl2_list_game_directories();
	KRKRNS_LOG("[launcher] browser mode, %u game directorie(s)", (unsigned)directories.size());
}
#endif

bool krkrsdl2_init_platform(void)
{
	KRKRNS_LOG("[ns] init_platform: entry");
	KRKRNS_LOG("[ns] BUILD=REALMEM1(full-memory-guard)");
#ifdef __SWITCH__
	krkrsdl2_start_heartbeat();
#endif
#ifdef __SWITCH__
	{
		// libnx initializes the applet service before main().  Calling
		// appletInitialize() again adds a service reference which is never
		// balanced by the automatic shutdown path.
		u64 total = 0, used = 0;
		krkrsdl2_get_process_memory(total, used);
		KRKRNS_LOG("[ns] applet=%d process-memory total=%llu used=%llu",
			static_cast<int>(appletGetAppletType()),
			static_cast<unsigned long long>(total),
			static_cast<unsigned long long>(used));
	}
	{
		Result r = romfsInit();
		KRKRNS_LOG("[ns] romfsInit result: 0x%x", (unsigned int)r);
		DIR *d = opendir("romfs:/");
		if (d) {
			int n = 0;
			struct dirent *ent;
			while ((ent = readdir(d)) != nullptr) {
				KRKRNS_LOG("[ns] romfs entry: [%s]", ent->d_name);
				n++;
			}
			closedir(d);
			KRKRNS_LOG("[ns] romfs opendir ok, %d entries", n);
		} else {
			KRKRNS_LOG("[ns] romfs opendir FAILED");
		}
		// direct file open probe
		FILE *fp = fopen("romfs:/startup.tjs", "rb");
		if (fp) {
			char buf[128] = {0};
			size_t r = fread(buf, 1, sizeof(buf) - 1, fp);
			fclose(fp);
			KRKRNS_LOG("[ns] fopen romfs:/startup.tjs ok, read %d bytes: [%s]", (int)r, buf);
		} else {
			KRKRNS_LOG("[ns] fopen romfs:/startup.tjs FAILED, errno=%d", errno);
		}
	}
	// make the game data (RomFS) the process working directory,
	// so relative storage names ("file://./") resolve to the game data
	chdir("romfs:/");
	KRKRNS_LOG("[ns] chdir(romfs:/) ok");
	// ensure the save/config directory exists
	mkdir("sdmc:/switch", 0777);
	mkdir("sdmc:/switch/krkrsdl2", 0777);
	mkdir("sdmc:/krkr", 0777);
	KRKRNS_LOG("[ns] sdmc save dir ensured");
	socketInitializeDefault();
	KRKRNS_LOG("[ns] socketInitializeDefault ok");
	nxlinkStdio();
	KRKRNS_LOG("[ns] nxlinkStdio ok");
	// KRKR-ns patch: seed the engine's auto-path with the game data root
	// (RomFS) so storage resolution does not depend on the relative
	// current-directory machinery.
	extern void TVPAddAutoPath(const ttstr &name);
	TVPAddAutoPath(TJS_W("file://?/romfs:/"));
	KRKRNS_LOG("[ns] auto path seeded");
#endif

	SDL_setenv("VITA_DISABLE_TOUCH_BACK", "1", 1);
	SDL_setenv("DBUS_FATAL_WARNINGS", "0", 0);

#ifdef _WIN32
#ifdef SDL_HINT_AUDIODRIVER
	SDL_SetHintWithPriority(SDL_HINT_AUDIODRIVER, "directsound", SDL_HINT_DEFAULT);
#endif
#ifdef SDL_HINT_IME_SHOW_UI
	SDL_SetHintWithPriority(SDL_HINT_IME_SHOW_UI, "1", SDL_HINT_DEFAULT);
#endif
#endif

#ifdef TVP_LOG_TO_COMMANDLINE_CONSOLE
	SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_VERBOSE);
#endif

	KRKRNS_LOG("[ns] before TVPLoadMessage");
	TVPLoadMessage();
	KRKRNS_LOG("[ns] TVPLoadMessage ok");

#ifdef _WIN32
	SDL_SetWindowsMessageHook(sdl_windows_message_hook, nullptr);
#endif
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
	SDL_AddEventWatch(sdl_event_watch, nullptr);
#endif
	KRKRNS_LOG("[ns] before new tTVPApplication()");
	::Application = new tTVPApplication();
	KRKRNS_LOG("[ns] tTVPApplication created");
	bool ret = ::Application->StartApplication(_argc, _wargv);
	KRKRNS_LOG("[ns] StartApplication returned %d", (int)ret);
	return !!ret;
}

void krkrsdl2_run_main_loop(void)
{
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
	emscripten_set_main_loop(process_events, 0, 0);
#else
	krkrsdl2_heartbeat_main_progress();
	while (process_events());
#endif
}

void krkrsdl2_cleanup(void)
{
	// delete application and exit forcely
	// this prevents ugly exception message on exit
	delete ::Application;
	::Application = nullptr;
}

bool TVPGetKeyMouseAsyncState(tjs_uint keycode, bool getcurrent)
{
	if (keycode >= VK_LBUTTON && keycode <= VK_XBUTTON2 && keycode != VK_CANCEL)
	{
		Uint32 state = SDL_GetMouseState(nullptr, nullptr);
		switch (keycode)
		{
			case VK_LBUTTON:
				return !!(state & SDL_BUTTON(SDL_BUTTON_LEFT));
			case VK_RBUTTON:
				return !!(state & SDL_BUTTON(SDL_BUTTON_RIGHT));
			case VK_MBUTTON:
				return !!(state & SDL_BUTTON(SDL_BUTTON_MIDDLE));
			case VK_XBUTTON1:
				return !!(state & SDL_BUTTON(SDL_BUTTON_X1));
			case VK_XBUTTON2:
				return !!(state & SDL_BUTTON(SDL_BUTTON_X2));
			default:
				return false;
		}
	}
	if (keycode >= VK_SHIFT && keycode <= VK_MENU)
	{
		Uint32 state = SDL_GetModState();
		switch (keycode)
		{
			case VK_SHIFT:
				return !!(state & KMOD_SHIFT);
			case VK_MENU:
				return !!(state & KMOD_ALT);
			case VK_CONTROL:
				return !!(state & KMOD_CTRL);
			default:
				return false;
		}
	}
	const Uint8 *state = SDL_GetKeyboardState(nullptr);
	return !!(state[SDL_GetScancodeFromKey(vk_key_to_sdl_key(keycode))]);
}

bool TVPGetJoyPadAsyncState(tjs_uint keycode, bool getcurrent)
{
	bool is_pressed = false;
	if (sdl_controllers)
	{
		for (int i = 0; i < sdl_controller_num; i += 1)
		{
			if (sdl_controllers[i])
			{
				is_pressed = is_pressed || !!SDL_GameControllerGetButton(sdl_controllers[i], (SDL_GameControllerButton)vk_key_to_sdl_gamecontrollerbutton(keycode));
			}
		}
	}
	return is_pressed;
}

TTVPWindowForm *TVPCreateAndAddWindow(tTJSNI_Window *w)
{
	return new TVPWindowWindow(w);
}

tjs_uint32 TVPGetCurrentShiftKeyState()
{
	tjs_uint32 f = 0;
	if (TVPGetAsyncKeyState(VK_SHIFT)) f |= TVP_SS_SHIFT;
	if (TVPGetAsyncKeyState(VK_MENU)) f |= TVP_SS_ALT;
	if (TVPGetAsyncKeyState(VK_CONTROL)) f |= TVP_SS_CTRL;
	if (TVPGetAsyncKeyState(VK_LBUTTON)) f |= TVP_SS_LEFT;
	if (TVPGetAsyncKeyState(VK_RBUTTON)) f |= TVP_SS_RIGHT;
	if (TVPGetAsyncKeyState(VK_MBUTTON)) f |= TVP_SS_MIDDLE;
	return f;
}

ttstr TVPGetPlatformName()
{
	// Some game patches branch on the platform name and their Switch branch
	// is not playable (hangs). A marker file opts the whole engine into
	// reporting "Windows", the same environment the release explicitly
	// supports (emulators).
	static int platformResolved = 0;
	static ttstr platformName;
	if (!platformResolved)
	{
		platformResolved = 1;
		platformName = ttstr(SDL_GetPlatform());
		if (FILE* f = fopen("sdmc:/switch/krkrsdl2/platform-windows.txt", "rb"))
		{
			fclose(f);
			platformName = ttstr(TJS_W("Windows"));
			KRKRNS_LOG("[ns] platform override -> Windows (marker file)");
		}
	}
	return platformName;
}

ttstr TVPGetOSName()
{
	return TVPGetPlatformName();
}

tjs_uint32 TVP_TShiftState_To_uint32(TShiftState state)
{
	tjs_uint32 result = 0;
	if (state & MK_SHIFT) result |= ssShift;
	if (state & MK_CONTROL) result |= ssCtrl;
	if (state & MK_ALT) result |= ssAlt;
	return result;
}
TShiftState TVP_TShiftState_From_uint32(tjs_uint32 state)
{
	TShiftState result = 0;
	if (state & ssShift) result |= MK_SHIFT;
	if (state & ssCtrl) result |= MK_CONTROL;
	if (state & ssAlt) result |= MK_ALT;
	return result;
}

void TVPGetAllFontList(std::vector<tjs_string>& list) {}

const tjs_char *TVPGetDefaultFontName()
{
	if (!TVPGetCommandLine(TJS_W("-deffont"), nullptr))
	{
		// Keep the native fallback in sync with the font bundled in data/.
		// Games are still free to register and select their own fonts; this is
		// only used when a requested family is unavailable on the target.
		TVPSetCommandLine(TJS_W("-deffont"), TJS_W("Noto Sans SC"));
	}
	static tjs_int ArgumentGeneration = 0;
	if (ArgumentGeneration != TVPGetCommandLineArgumentGeneration())
	{
		ArgumentGeneration = TVPGetCommandLineArgumentGeneration();
		// Use that font, if specified on the command line
		tTJSVariant opt;
		if (TVPGetCommandLine(TJS_W("-deffont"), &opt))
		{
			ttstr str(opt);
			TVPDefaultFontName.AssignMessage(str.c_str());
		}
	}
	return TVPDefaultFontName;
}

void TVPSetDefaultFontName(const tjs_char *name)
{
	TVPSetCommandLine(TJS_W("-deffont"), name);
}

static ttstr TVPDefaultFaceNames;
const ttstr &TVPGetDefaultFaceNames()
{
	static tjs_int ArgumentGeneration = 0;
	if (ArgumentGeneration != TVPGetCommandLineArgumentGeneration())
	{
		ArgumentGeneration = TVPGetCommandLineArgumentGeneration();
		TVPDefaultFaceNames = TJS_W("");
	}
	if (TVPDefaultFaceNames.IsEmpty())
	{
		TVPDefaultFaceNames = ttstr(TVPGetDefaultFontName());
	}
	return TVPDefaultFaceNames;
}

#if defined(__vita__)
#define KRKRSDL2_OVERRIDE_NEW_ALLOCATOR_FUNCTIONS
#endif

// Override allocation functions by removing the std::bad_alloc throw and doing garbage collection.
#ifdef KRKRSDL2_OVERRIDE_NEW_ALLOCATOR_FUNCTIONS
void *operator new(std::size_t size) noexcept
{
	bool has_gced = false;

	if (!size)
		size = 1;
	void *p;
	while ((p = ::malloc(size)) == nullptr)
	{
		// If malloc fails, try to free up memory.
		if (has_gced)
			break;
		else
		{
			TVPDeliverCompactEvent(TVP_COMPACT_LEVEL_MAX);
			has_gced = true;
		}
	}
	return p;
}

void *operator new(size_t size, const std::nothrow_t&) noexcept
{
	void *p = 0;
	p = ::operator new(size);
	return p;
}

void *operator new[](size_t size) noexcept
{
	return ::operator new(size);
}

void *operator new[](size_t size, const std::nothrow_t&) noexcept
{
	void *p = 0;
	p = ::operator new[](size);
	return p;
}

void operator delete(void *ptr) noexcept
{
	::free(ptr);
}

void operator delete(void *ptr, const std::nothrow_t&) noexcept
{
	::operator delete(ptr);
}

void operator delete(void *ptr, size_t) noexcept
{
	::operator delete(ptr);
}

void operator delete[] (void *ptr) noexcept
{
	::operator delete(ptr);
}

void operator delete[] (void *ptr, const std::nothrow_t&) noexcept
{
	::operator delete[](ptr);
}

void operator delete[] (void *ptr, size_t) noexcept
{
	::operator delete[](ptr);
}

#ifdef __cpp_aligned_new
void *operator new(std::size_t size, std::align_val_t alignment) noexcept
{
	bool has_gced = false;

	if (!size)
		size = 1;
	if (static_cast<size_t>(alignment) < sizeof(void*))
		alignment = std::align_val_t(sizeof(void*));
	void *p;
	while (::posix_memalign(&p, static_cast<size_t>(alignment), size))
	{
		// If posix_memalign fails, try to free up memory.
		if (has_gced)
		{
			p = nullptr; // posix_memalign doesn't initialize 'p' on failure
			break;
		}
		else
		{
			TVPDeliverCompactEvent(TVP_COMPACT_LEVEL_MAX);
			has_gced = true;
		}
	}
	return p;
}

void *operator new(size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
	void *p = nullptr;
	p = ::operator new(size, alignment);
	return p;
}

void *operator new[](size_t size, std::align_val_t alignment) noexcept
{
	return ::operator new(size, alignment);
}

void *operator new[](size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
	void *p = nullptr;
	p = ::operator new[](size, alignment);
	return p;
}

void operator delete(void *ptr, std::align_val_t) noexcept
{
	::free(ptr);
}

void operator delete(void *ptr, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
	::operator delete(ptr, alignment);
}

void operator delete(void *ptr, size_t, std::align_val_t alignment) noexcept
{
	::operator delete(ptr, alignment);
}

void operator delete[] (void *ptr, std::align_val_t alignment) noexcept
{
	::operator delete(ptr, alignment);
}

void operator delete[] (void *ptr, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
	::operator delete[](ptr, alignment);
}

void operator delete[] (void *ptr, size_t, std::align_val_t alignment) noexcept
{
	::operator delete[](ptr, alignment);
}
#endif

#endif
