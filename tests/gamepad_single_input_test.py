"""Exercise production pad synthesis and dispatch with a real SDL virtual pad.

Extract the Switch mapping, raw controller-event handler and async pad query
unchanged. A press must have one mouse/key action and no extra VK_PAD shortcut;
the launcher's native face buttons and desktop pad input remain available.
"""
import argparse
from pathlib import Path
import shutil
import subprocess


HARNESS = r'''
#include <SDL.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#include "VirtualKey.h"
using tjs_uint = unsigned;
using tjs_uint32 = unsigned;
#include "tvpinputdefs.h"
// Define this after SDL's platform headers: use the host SDL runtime while
// exercising the production Switch branches.
#ifdef KRKRNS_TEST_SWITCH
#define __SWITCH__ 1
#endif
#define KRKRNS_LOG(...) ((void)0)
static struct {
    unsigned gp_push_motion = 0, gp_push_button = 0, gp_push_key = 0;
} g_krkrns_prof;
static SDL_GameController **sdl_controllers = nullptr;
static int sdl_controller_num = 0;
static void refresh_controllers() { throw std::runtime_error("unexpected pad refresh"); }
bool krkrsdl2_game_mode = true;
static bool MenuActive = false;
namespace { bool krkrns_menu_is_active() { return MenuActive; } }
enum Edge { Down, Press, Up };
struct InputEvent { Edge edge; unsigned key; };
struct tTVPOnKeyDownInputEvent : InputEvent {
    tTVPOnKeyDownInputEvent(void *, unsigned key, unsigned) : InputEvent{Down, key} {}
};
struct tTVPOnKeyPressInputEvent : InputEvent {
    tTVPOnKeyPressInputEvent(void *, unsigned key) : InputEvent{Press, key} {}
};
struct tTVPOnKeyUpInputEvent : InputEvent {
    tTVPOnKeyUpInputEvent(void *, unsigned key, unsigned) : InputEvent{Up, key} {}
};
static std::vector<InputEvent> NativeEvents;
static void TVPPostInputEvent(InputEvent *event) {
    NativeEvents.push_back(*event); delete event;
}
class TVPWindowWindow {
public:
    bool isBeingDeleted = false, hasDrawn = true, menuInputOwnedThisPoll = false;
    SDL_Window *window = nullptr;
    void *TJSNativeInstance = this;
    void switch_process_gamepad_input();
    void switch_process_gamepad_button(int button, bool pressed);
    bool dispatch_controller(const SDL_Event &event);
};
static TVPWindowWindow *_currentWindowWindow = nullptr;
'''


CASES = r'''
static void Require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
static std::vector<SDL_Event> MappedEvents;
static TVPWindowWindow Win;
static SDL_Joystick *Joystick = nullptr;
static void Drain() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
#ifdef KRKRNS_TEST_SWITCH
        ns_gp_process_controller_event(event);
#endif
        if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP) {
            // The menu normally consumes raw controller events before dispatch.
            if (!Win.menuInputOwnedThisPoll) Win.dispatch_controller(event);
        } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP ||
                   event.type == SDL_KEYDOWN || event.type == SDL_KEYUP ||
                   event.type == SDL_MOUSEWHEEL) {
            MappedEvents.push_back(event);
        }
    }
}
static void Frame() {
    SDL_PumpEvents();
    Win.menuInputOwnedThisPoll = false;
    Win.switch_process_gamepad_input();
    Drain();
}
static void SetButton(SDL_GameControllerButton button, bool down) {
    Require(SDL_JoystickSetVirtualButton(Joystick, button, down ? 1 : 0) == 0,
            "setting the SDL virtual pad failed");
    Frame();
}
static void Reset(bool game = true) {
    krkrsdl2_game_mode = game; MenuActive = false;
    ns_gp = ns_gamepad_state_t{};
    ns_gp_prev_buttons = ns_gp_menu_face_buttons = 0;
    ResetNativeFaceHolds();
    MappedEvents.clear(); NativeEvents.clear();
}
static void MousePair(SDL_GameControllerButton button, Uint8 mouseButton) {
    Reset();
    SetButton(button, true);
    Require(MappedEvents.size() == 1 && MappedEvents[0].type == SDL_MOUSEBUTTONDOWN &&
            MappedEvents[0].button.button == mouseButton, "pad mouse-down mapping changed");
    Require(NativeEvents.empty(), "mouse confirmation also delivered a native VK_PAD shortcut");
    Require(!TVPGetJoyPadAsyncState(sdl_gamecontrollerbutton_to_vk_key(button), true),
            "game polling still sees a second raw pad mapping");
    Frame(); Frame();
    Require(MappedEvents.size() == 1 && NativeEvents.empty(), "a held button repeated its action");
    SetButton(button, false);
    Require(MappedEvents.size() == 2 && MappedEvents[1].type == SDL_MOUSEBUTTONUP &&
            MappedEvents[1].button.button == mouseButton && NativeEvents.empty(),
            "pad mouse-up was missing or emitted an extra shortcut");
}
static void KeyPair(SDL_GameControllerButton button, SDL_Scancode key) {
    Reset(); SetButton(button, true);
    Require(MappedEvents.size() == 1 && MappedEvents[0].type == SDL_KEYDOWN &&
            MappedEvents[0].key.keysym.scancode == key && MappedEvents[0].key.padding2 == 0xA5,
            "pad keyboard-down mapping changed");
    Require(NativeEvents.empty(), "keyboard mapping also delivered a native VK_PAD shortcut");
    Require(!TVPGetJoyPadAsyncState(sdl_gamecontrollerbutton_to_vk_key(button), true),
            "keyboard mapping also exposed a raw pad polling state");
    Frame(); Require(MappedEvents.size() == 1, "held key unexpectedly repeated");
    SetButton(button, false);
    Require(MappedEvents.size() == 2 && MappedEvents[1].type == SDL_KEYUP &&
            MappedEvents[1].key.keysym.scancode == key && NativeEvents.empty(),
            "pad keyboard-up was missing or emitted an extra shortcut");
}
static void LauncherFace(SDL_GameControllerButton button, unsigned key) {
    Reset(false); SetButton(button, true);
    Require(MappedEvents.empty(), "launcher face button also generated a mouse/keyboard action");
    Require(NativeEvents.size() == 1 && NativeEvents[0].edge == Down && NativeEvents[0].key == key,
            "launcher native face-button press was lost");
    Require(TVPGetJoyPadAsyncState(key, true), "launcher pad polling state was lost");
    SetButton(button, false);
    Require(MappedEvents.empty() && NativeEvents.size() == 3 && NativeEvents[2].edge == Up &&
            NativeEvents[2].key == key && !TVPGetJoyPadAsyncState(key, true),
            "launcher native face-button release was lost");
}
int main() {
    _currentWindowWindow = &Win;
    SDL_SetMainReady();
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_GameController *controller = nullptr;
    SDL_GameController *secondController = nullptr;
    int device = -1, secondDevice = -1, result = 0;
    try {
        Require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) == 0, SDL_GetError());
        Win.window = SDL_CreateWindow("pad regression", 0, 0, 1280, 720, SDL_WINDOW_HIDDEN);
        Require(Win.window != nullptr, SDL_GetError());
        SDL_StopTextInput();
        device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                          SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
        Require(device >= 0 && SDL_IsGameController(device), "SDL virtual pad is not a controller");
        controller = SDL_GameControllerOpen(device);
        Require(controller != nullptr, SDL_GetError());
        Joystick = SDL_GameControllerGetJoystick(controller);
        sdl_controllers = &controller; sdl_controller_num = 1;
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
#ifdef KRKRNS_TEST_SWITCH
        // Nintendo A is SDL's positional B, and Nintendo B is SDL's A.
        for (int i = 0; i < 8; ++i) MousePair(SDL_CONTROLLER_BUTTON_B, SDL_BUTTON_LEFT);
        Reset();
        SDL_JoystickSetVirtualButton(Joystick, SDL_CONTROLLER_BUTTON_B, 1);
        SDL_PumpEvents();
        SDL_JoystickSetVirtualButton(Joystick, SDL_CONTROLLER_BUTTON_B, 0);
        SDL_PumpEvents();
        Frame();
        // SDL may defer events pushed during a poll to the next poll cycle.
        Frame();
        Require(NativeEvents.empty() && MappedEvents.size() == 2 &&
                MappedEvents[0].type == SDL_MOUSEBUTTONDOWN &&
                MappedEvents[1].type == SDL_MOUSEBUTTONUP,
                "a complete tap queued between frames was lost");
        MousePair(SDL_CONTROLLER_BUTTON_A, SDL_BUTTON_RIGHT);
        KeyPair(SDL_CONTROLLER_BUTTON_Y, SDL_SCANCODE_RETURN);
        KeyPair(SDL_CONTROLLER_BUTTON_X, SDL_SCANCODE_SPACE);
        KeyPair(SDL_CONTROLLER_BUTTON_BACK, SDL_SCANCODE_F5);
        KeyPair(SDL_CONTROLLER_BUTTON_START, SDL_SCANCODE_ESCAPE);
        KeyPair(SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_SCANCODE_LCTRL);
        KeyPair(SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_SCANCODE_F7);
        KeyPair(SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_SCANCODE_UP);
        KeyPair(SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_SCANCODE_DOWN);
        KeyPair(SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_SCANCODE_LEFT);
        KeyPair(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_SCANCODE_RIGHT);
        for (auto button : {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER}) {
            Reset(); SetButton(button, true); Frame(); SetButton(button, false);
            Require(NativeEvents.empty() && MappedEvents.size() == 1 &&
                    MappedEvents[0].type == SDL_MOUSEWHEEL && MappedEvents[0].wheel.y ==
                    (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ? 1 : -1),
                    "shoulder button must generate one wheel action");
        }
        secondDevice = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                                SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
        Require(secondDevice >= 0, "attaching the second pad failed");
        secondController = SDL_GameControllerOpen(secondDevice);
        Require(secondController != nullptr, SDL_GetError());
        SDL_GameController *pads[] = {controller, secondController};
        sdl_controllers = pads; sdl_controller_num = 2;
        Reset(); SetButton(SDL_CONTROLLER_BUTTON_B, true);
        SDL_Joystick *secondJoystick = SDL_GameControllerGetJoystick(secondController);
        SDL_JoystickSetVirtualButton(secondJoystick, SDL_CONTROLLER_BUTTON_B, 1);
        Frame(); SetButton(SDL_CONTROLLER_BUTTON_B, false); Frame();
        Require(MappedEvents.size() == 1 && NativeEvents.empty(),
                "releasing one pad released the confirmation still held on the other pad");
        SDL_JoystickSetVirtualButton(secondJoystick, SDL_CONTROLLER_BUTTON_B, 0);
        Frame(); Frame();
        Require(MappedEvents.size() == 2 && MappedEvents[1].type == SDL_MOUSEBUTTONUP &&
                NativeEvents.empty(), "the final pad release did not complete one mouse action");
        sdl_controllers = &controller; sdl_controller_num = 1;
        SDL_GameControllerClose(secondController); secondController = nullptr;
        SDL_JoystickDetachVirtual(secondDevice); secondDevice = -1;
        LauncherFace(SDL_CONTROLLER_BUTTON_B, VK_PAD1);
        LauncherFace(SDL_CONTROLLER_BUTTON_A, VK_PAD2);
        LauncherFace(SDL_CONTROLLER_BUTTON_Y, VK_PAD3);
        LauncherFace(SDL_CONTROLLER_BUTTON_X, VK_PAD4);
        Reset(false); SetButton(SDL_CONTROLLER_BUTTON_B, true);
        krkrsdl2_game_mode = true;
        NativeEvents.clear(); SetButton(SDL_CONTROLLER_BUTTON_B, false);
        Require(MappedEvents.empty() && NativeEvents.empty(),
                "launcher confirmation release generated an unmatched game click");
        Reset(); MenuActive = true; SetButton(SDL_CONTROLLER_BUTTON_B, true);
        Require(MappedEvents.size() == 1 && MappedEvents[0].type == SDL_KEYDOWN &&
                MappedEvents[0].key.keysym.scancode == SDL_SCANCODE_RETURN &&
                MappedEvents[0].key.padding3 == 0xA6 && NativeEvents.empty(),
                "menu confirmation must be one menu-owned Return press");
        MenuActive = false; SetButton(SDL_CONTROLLER_BUTTON_B, false);
        Require(MappedEvents.size() == 2 && MappedEvents[1].type == SDL_KEYUP &&
                MappedEvents[1].key.padding3 == 0xA6 && NativeEvents.empty(),
                "closing a menu must retain its confirmation release without a game click");
        std::cout << "PASS: single game actions, held/released and buffered tap edges, "
                     "multiple pads, polling isolation, launcher A/B/X/Y and game handoff, "
                     "and menu confirmation pairing\n";
#else
        // The added guards must leave the normal desktop controller API intact.
        SDL_JoystickSetVirtualButton(Joystick, SDL_CONTROLLER_BUTTON_A, 1);
        SDL_PumpEvents(); Drain();
        Require(NativeEvents.size() == 1 && NativeEvents[0].edge == Down &&
                NativeEvents[0].key == VK_PAD1 && TVPGetJoyPadAsyncState(VK_PAD1, true),
                "desktop native pad press/polling changed");
        SDL_JoystickSetVirtualButton(Joystick, SDL_CONTROLLER_BUTTON_A, 0);
        SDL_PumpEvents(); Drain();
        Require(NativeEvents.size() == 3 && NativeEvents[2].edge == Up &&
                NativeEvents[2].key == VK_PAD1 && !TVPGetJoyPadAsyncState(VK_PAD1, true),
                "desktop native pad release changed");
        std::cout << "PASS: desktop native controller events and polling preserved\n";
#endif
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n'; result = 1;
    }
    if (secondController) SDL_GameControllerClose(secondController);
    if (secondDevice >= 0) SDL_JoystickDetachVirtual(secondDevice);
    if (controller) SDL_GameControllerClose(controller);
    if (device >= 0) SDL_JoystickDetachVirtual(device);
    if (Win.window) SDL_DestroyWindow(Win.window);
    SDL_Quit(); return result;
}
'''


def function(text, signature):
    start = text.index(signature)
    end = text.index('\n}\n', start) + 3
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--git-head', action='store_true', help='negative control before the fix')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--sdl-dir', type=Path)
    parser.add_argument('--cmake', default=shutil.which('cmake'))
    args = parser.parse_args()
    if not args.cmake:
        parser.error('CMake is required; pass --cmake if it is not on PATH.')
    root = Path(__file__).resolve().parent.parent
    source = args.source or root / 'krkrsdl2/src/core/sdl2/SDLApplication.cpp'
    text = (subprocess.check_output(['git', '-C', str(root), 'show',
                                    'HEAD:krkrsdl2/src/core/sdl2/SDLApplication.cpp']).decode('utf-8')
            if args.git_head else source.read_text(encoding='utf-8'))
    text = text.replace('\r\n', '\n')
    helpers = text[text.index('#define NS_GP_DEADZONE '):
                   text.index('\n#endif', text.index('static void ns_gp_push_mouse_wheel('))]
    event_start = text.index('\t\t\t\tcase SDL_CONTROLLERBUTTONDOWN:',
                             text.index('bool TVPWindowWindow::window_receive_event_input('))
    event_end = text.index('\t\t\t\tcase SDL_KEYDOWN:', event_start)
    controller_handler = ('bool TVPWindowWindow::dispatch_controller(const SDL_Event &event) {\n'
                          ' const tjs_uint32 s = 0;\n switch (event.type) {\n' +
                          text[event_start:event_end] + '\n default: return false;\n }\n}\n')
    edge_mapper = ('\n'.join((function(text, 'void TVPWindowWindow::switch_process_gamepad_button('),
                               function(text, 'static void ns_gp_process_controller_event(')))
                   if 'static void ns_gp_process_controller_event(' in text else
                   'static void ns_gp_process_controller_event(const SDL_Event &) {}\n')
    reset_native = ('static void ResetNativeFaceHolds() { ns_gp_native_face_buttons = 0; }\n'
                    if 'static Uint16 ns_gp_native_face_buttons' in text else
                    'static void ResetNativeFaceHolds() {}\n')
    sections = [HARNESS, helpers, reset_native,
                function(text, 'static Uint8 vk_key_to_sdl_gamecontrollerbutton('),
                function(text, 'static tjs_uint sdl_gamecontrollerbutton_to_vk_key('),
                function(text, 'void TVPWindowWindow::switch_process_gamepad_input('),
                edge_mapper, function(text, 'bool TVPGetJoyPadAsyncState('),
                controller_handler, CASES]
    out = (args.output_dir or root / 'build-gamepad-single-input').resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'gamepad_single_input_test.cpp').write_text('\n'.join(sections), encoding='utf-8')
    cmake = ('cmake_minimum_required(VERSION 3.20)\n'
             'project(gamepad_single_input_test LANGUAGES CXX)\n'
             'set(CMAKE_CXX_STANDARD 17)\nfind_package(SDL2 CONFIG REQUIRED)\n')
    for target in ('gamepad_single_input_test', 'desktop_pad_input_test'):
        cmake += (f'add_executable({target} gamepad_single_input_test.cpp)\n'
                  f'target_compile_definitions({target} PRIVATE SDL_MAIN_HANDLED NOMINMAX)\n'
                  f'target_include_directories({target} PRIVATE\n'
                  f' "{root.as_posix()}/krkrsdl2/src/core/environ/sdl2"\n'
                  f' "{root.as_posix()}/krkrsdl2/external/krkrz/visual")\n'
                  f'target_link_libraries({target} PRIVATE SDL2::SDL2)\n'
                  f'if(MSVC)\n target_compile_options({target} PRIVATE /EHsc /utf-8)\nendif()\n'
                  f'if(WIN32)\nadd_custom_command(TARGET {target} POST_BUILD\n'
                  ' COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2>\n'
                  f' $<TARGET_FILE_DIR:{target}>)\nendif()\n')
    cmake += 'target_compile_definitions(gamepad_single_input_test PRIVATE KRKRNS_TEST_SWITCH)\n'
    (out / 'CMakeLists.txt').write_text(cmake, encoding='utf-8')
    configure = [args.cmake, '-S', str(out), '-B', str(out / 'build')]
    if args.sdl_dir:
        configure.append(f'-DSDL2_DIR={args.sdl_dir.resolve().as_posix()}')
    subprocess.run(configure, check=True, timeout=90)
    subprocess.run([args.cmake, '--build', str(out / 'build'), '--config', 'Release'],
                   check=True, timeout=180)
    for target in ('gamepad_single_input_test', 'desktop_pad_input_test'):
        binary = next(p for p in (out / f'build/Release/{target}.exe', out / f'build/{target}')
                      if p.is_file())
        result = subprocess.run([str(binary)], timeout=20)
        if result.returncode:
            return result.returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
