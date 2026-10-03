"""Exercise production pointer delivery when callbacks close their Window.

The harness keeps released native allocations readable to report the first
stale access deterministically. Production input methods are compiled unchanged;
callbacks model Window invalidation dropping the draw-device and layer refs.
"""
import argparse
from pathlib import Path
import shutil
import subprocess


HARNESS = r'''
#include <functional>
#include <iostream>
#include <stdexcept>
#include <utility>
using tjs_int = int;
using tjs_int64 = long long;
using tjs_uint32 = unsigned;
using tjs_real = double;
enum tTVPMouseButton { mbLeft };
struct ttstr {};
static void Require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
static bool TVPIsAnyMouseButtonPressedInShiftStateFlags(unsigned flags) {
    return flags != 0;
}
class tTVPLayerManager;
class tTJSNI_BaseLayer;
struct iTJSDispatch2 {
    int Refs = 1;
    bool Alive = true;
    void AddRef() { Require(Alive, "capturing a destroyed layer owner"); ++Refs; }
    void Release() {
        Require(Alive && Refs > 0, "layer owner reference underflow");
        if (--Refs == 0) Alive = false;
    }
};
class tTJSVariant {
    iTJSDispatch2 *Object = nullptr, *ObjThis = nullptr;
    void Retain() { if (Object) Object->AddRef(); if (ObjThis) ObjThis->AddRef(); }
    void Drop() { if (Object) Object->Release(); if (ObjThis) ObjThis->Release(); }
public:
    tTJSVariant() = default;
    tTJSVariant(iTJSDispatch2 *object, iTJSDispatch2 *objthis)
        : Object(object), ObjThis(objthis) { Retain(); }
    tTJSVariant(const tTJSVariant &other)
        : Object(other.Object), ObjThis(other.ObjThis) { Retain(); }
    tTJSVariant &operator=(const tTJSVariant &other) {
        if (this != &other) {
            // As in a TJS closure assignment, retain before dropping old refs.
            if (other.Object) other.Object->AddRef();
            if (other.ObjThis) other.ObjThis->AddRef();
            Drop(); Object = other.Object; ObjThis = other.ObjThis;
        }
        return *this;
    }
    ~tTJSVariant() { Drop(); }
};
class tTJSNI_BaseLayer {
public:
    iTJSDispatch2 *Owner;
    tTVPLayerManager *Manager;
    bool Shutdown = false;
    std::function<void()> Down, Up, Enter, Move, TouchDown, TouchUp;
    int DownEvents = 0, UpEvents = 0, MoveEvents = 0;
    void FromPrimaryCoordinates(int &, int &) { Require(Owner->Alive, "coordinates on a dead layer"); }
    void FromPrimaryCoordinates(double &, double &) { Require(Owner->Alive, "touch coordinates on a dead layer"); }
    void FireMouseDown(int, int, tTVPMouseButton, unsigned) { ++DownEvents; if (Down) Down(); }
    void FireMouseUp(int, int, tTVPMouseButton, unsigned) { ++UpEvents; if (Up) Up(); }
    void FireMouseMove(int, int, unsigned) { ++MoveEvents; if (Move) Move(); }
    void FireMouseEnter() { if (Enter) Enter(); }
    void FireMouseLeave() { Require(Owner->Alive && !Shutdown, "mouse leave on an invalid layer"); }
    void FireTouchDown(double, double, double, double, unsigned) { if (TouchDown) TouchDown(); }
    void FireTouchUp(double, double, double, double, unsigned) { if (TouchUp) TouchUp(); }
    void FireTouchMove(double, double, double, double, unsigned) { if (Move) Move(); }
    void SetCurrentCursorToWindow() { Require(Owner->Alive && !Shutdown, "cursor on an invalid layer"); }
    void SetCurrentHintToWindow() { Require(Owner->Alive && !Shutdown, "hint on an invalid layer"); }
};
class tTVPLayerManager {
public:
    int Refs = 2; // draw device + primary layer
    bool Alive = true;
    tTJSNI_BaseLayer *Primary = nullptr, *CaptureOwner = nullptr, *LastMouseMoveSent = nullptr;
    tTJSNI_BaseLayer *TouchCapture = nullptr;
    bool ReleaseCaptureCalled = false, InNotifyingHintOrCursorChange = false;
    long long ReleaseTouchCaptureIDMark = -1;
    int LastMouseMoveX = -1, LastMouseMoveY = -1;
    void Check() { Require(Alive, "input resumed on a destroyed layer manager"); }
    void AddRef() { Check(); ++Refs; }
    void Release() { Check(); Require(Refs > 0, "manager reference underflow"); if (--Refs == 0) Alive = false; }
    tTJSNI_BaseLayer *GetMostFrontChildAt(int, int) { Check(); return Primary; }
    void ReleaseCapture() {
        Check(); ReleaseCaptureCalled = true;
        auto *old = CaptureOwner; CaptureOwner = nullptr;
        if (old) old->Owner->Release();
    }
    void SetHint(void *, const ttstr &) { Check(); }
    void SetMouseCursor(int) { Check(); }
    void ReleaseTouchCapture(unsigned) {
        Check(); ReleaseTouchCaptureIDMark = -1;
        auto *old = TouchCapture; TouchCapture = nullptr;
        if (old) old->Owner->Release();
    }
    tTJSNI_BaseLayer *GetTouchCapture(unsigned) { Check(); return TouchCapture; }
    void SetTouchCapture(unsigned, tTJSNI_BaseLayer *layer) {
        Check(); Require(!layer->Shutdown && layer->Owner->Alive, "touch captured an invalid layer");
        TouchCapture = layer; layer->Owner->AddRef();
    }
    void PrimaryMouseDown(int, int, tTVPMouseButton, unsigned);
    void PrimaryMouseUp(int, int, tTVPMouseButton, unsigned);
    void PrimaryMouseMove(int, int, unsigned);
    void PrimaryTouchDown(double, double, double, double, unsigned);
    void PrimaryTouchUp(double, double, double, double, unsigned);
    void PrimaryTouchMove(double, double, double, double, unsigned);
};
struct Scene {
    iTJSDispatch2 Owner;
    tTVPLayerManager Manager;
    tTJSNI_BaseLayer Layer;
    Scene() {
        Layer.Owner = &Owner; Layer.Manager = &Manager; Manager.Primary = &Layer;
        // Most tests begin over a layer whose enter event was already sent.
        Manager.LastMouseMoveSent = &Layer; Owner.AddRef();
        Manager.LastMouseMoveX = Manager.LastMouseMoveY = 10;
    }
    void InvalidateLayer(bool closeWindow) {
        Layer.Shutdown = true; Layer.Manager = nullptr;
        Manager.ReleaseCapture(); Manager.ReleaseTouchCapture(1);
        if (Manager.LastMouseMoveSent) {
            Manager.LastMouseMoveSent = nullptr; Owner.Release();
        }
        Manager.Primary = nullptr;
        Owner.Release();
        if (closeWindow) { Manager.Release(); Manager.Release(); }
    }
};
'''


CASES = r'''
int main() {
    try {
        Scene normal;
        normal.Manager.PrimaryMouseDown(10, 10, mbLeft, 1);
        Require(normal.Layer.DownEvents == 1 && normal.Manager.CaptureOwner == &normal.Layer,
                "ordinary mouse down captures its live target once");
        normal.Manager.PrimaryMouseUp(10, 10, mbLeft, 1);
        Require(normal.Manager.CaptureOwner == &normal.Layer, "a held mouse button retains capture");
        normal.Manager.PrimaryMouseUp(10, 10, mbLeft, 0);
        Require(!normal.Manager.CaptureOwner && normal.Layer.UpEvents == 2,
                "last mouse up releases capture");
        Scene released;
        released.Layer.Down = [&] { released.Manager.ReleaseCapture(); };
        released.Manager.PrimaryMouseDown(10, 10, mbLeft, 1);
        Require(!released.Manager.CaptureOwner, "an explicit release in onMouseDown suppresses capture");
        Scene targetOnly;
        targetOnly.Layer.Down = [&] { targetOnly.InvalidateLayer(false); };
        targetOnly.Manager.PrimaryMouseDown(10, 10, mbLeft, 1);
        Require(targetOnly.Manager.Alive && !targetOnly.Manager.CaptureOwner && !targetOnly.Owner.Alive,
                "a deleted target is not captured after its callback");
        for (int session = 0; session != 64; ++session) {
            Scene down;
            down.Layer.Down = [&] { down.InvalidateLayer(true); };
            down.Manager.PrimaryMouseDown(10, 10, mbLeft, 1);
            Require(!down.Manager.Alive && !down.Owner.Alive,
                    "Window closes during onMouseDown; retained objects release after delivery");
            Scene up;
            up.Manager.PrimaryMouseDown(10, 10, mbLeft, 1);
            up.Layer.Up = [&] { up.InvalidateLayer(true); };
            up.Manager.PrimaryMouseUp(10, 10, mbLeft, 0);
            Require(!up.Manager.Alive && !up.Owner.Alive, "Window closes during onMouseUp");
            Scene entered;
            entered.Manager.LastMouseMoveSent = nullptr; entered.Owner.Release();
            entered.Layer.Enter = [&] { entered.InvalidateLayer(true); };
            entered.Manager.PrimaryMouseMove(11, 11, 0);
            Require(!entered.Manager.Alive && !entered.Owner.Alive,
                    "Window closes during onMouseEnter without sending cursor/hint to it");
            Scene moved;
            moved.Layer.Move = [&] { moved.InvalidateLayer(true); };
            moved.Manager.PrimaryMouseMove(11, 11, 0);
            Require(!moved.Manager.Alive && !moved.Owner.Alive, "Window closes during onMouseMove");
            Scene touched;
            touched.Layer.TouchDown = [&] { touched.InvalidateLayer(true); };
            touched.Manager.PrimaryTouchDown(10, 10, 0, 0, 1);
            Require(!touched.Manager.Alive && !touched.Owner.Alive,
                    "Window closes during onTouchDown without capturing its deleted target");
            Scene touchUp;
            touchUp.Manager.PrimaryTouchDown(10, 10, 0, 0, 1);
            touchUp.Layer.TouchUp = [&] { touchUp.InvalidateLayer(true); };
            touchUp.Manager.PrimaryTouchUp(10, 10, 0, 0, 1);
            Require(!touchUp.Manager.Alive && !touchUp.Owner.Alive, "Window closes during onTouchUp");
        }
        Scene throwing;
        throwing.Layer.Down = [] { throw std::runtime_error("script error"); };
        try { throwing.Manager.PrimaryMouseDown(10, 10, mbLeft, 1); }
        catch (const std::runtime_error &) {}
        Require(throwing.Manager.Refs == 2 && throwing.Owner.Refs == 2,
                "exception unwinding balances temporary input references");
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n'; return 1;
    }
    std::cout << "PASS: normal capture, explicit release, deleted target, 64 repeated Window lifetimes, and exception unwind\n";
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--cmake', default=shutil.which('cmake') or
                        r'D:\KRKR-ns-tools\cmake-3.31.6-windows-x86_64\bin\cmake.exe')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = args.source or root / 'krkrsdl2/external/krkrz/visual/LayerManager.cpp'
    text = source.read_text(encoding='utf-8')
    sections = [HARNESS]
    guard_start = text.find('class tTVPLayerInputGuard\n')
    if guard_start >= 0:
        guard_end = text.index('\n};', guard_start) + 3
        sections.append(text[guard_start:guard_end])
    for name in ('PrimaryMouseDown', 'PrimaryMouseUp', 'PrimaryMouseMove',
                 'PrimaryTouchDown', 'PrimaryTouchUp', 'PrimaryTouchMove'):
        start = text.index('void tTVPLayerManager::' + name + '(')
        end = text.index('\n}\n', start) + 3
        sections.append(text[start:end])
    sections.append(CASES)
    out = (args.output_dir or root / 'build-layer-input-lifetime').resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'layer_input_lifetime_test.cpp').write_text('\n'.join(sections), encoding='utf-8')
    (out / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.20)\n'
        'project(layer_input_lifetime_test LANGUAGES CXX)\n'
        'set(CMAKE_CXX_STANDARD 17)\n'
        'add_executable(layer_input_lifetime_test layer_input_lifetime_test.cpp)\n'
        'if(MSVC)\n target_compile_options(layer_input_lifetime_test PRIVATE /EHsc /utf-8)\nendif()\n',
        encoding='utf-8')
    subprocess.run([args.cmake, '-S', str(out), '-B', str(out / 'build')], check=True)
    subprocess.run([args.cmake, '--build', str(out / 'build'), '--config', 'Release'], check=True)
    binary = next(p for p in (out / 'build/Release/layer_input_lifetime_test.exe',
                             out / 'build/layer_input_lifetime_test') if p.is_file())
    return subprocess.run([str(binary)]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
