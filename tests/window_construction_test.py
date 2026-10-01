"""Exercise production Window construction/invalidation after native attachment.

The dispatch fixture models the actual TJS order: attach the native instance
before calling the named constructor, then invalidate attached instances during
finalization even when the script constructor omitted super or threw. Production
constructors and invalidators are compiled unchanged against controlled services.

The optional causal controls distinguish a latent uninitialized field from its
newly introduced dereference: HEAD ctor + HEAD Base Invalidate completes safely;
HEAD ctor + current menu Invalidate fails at the menu variant's AddRef attempt.
Both use the current SDL invalidator, which clears Owner at the end of cleanup.
"""
import argparse
from pathlib import Path
import shutil
import subprocess


HARNESS = r'''
#include <algorithm>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
using tjs_int = int;
using tjs_error = int;
using tjs_char = wchar_t;
#define TJS_INTF_METHOD
#define TJS_W(x) L##x
#define TJS_S_OK 0
#define TJS_FAILED(x) ((x) < 0)
#define TJS_SUCCEEDED(x) ((x) >= 0)
#define TJS_MEMBERMUSTEXIST 1
#define TJS_MEMBERENSURE 2
#define TJS_IGNOREPROP 4
#define __SWITCH__
enum { tvtVoid, tvtObject };
static void Require(bool ok, const char *message) {
    if(!ok) throw std::runtime_error(message);
}
struct eTJSError : std::runtime_error {
    using std::runtime_error::runtime_error;
    const char *GetMessage() const { return what(); }
};
static void TVPAddLog(const char *) {}
class iTJSDispatch2;
class tTJSVariant;
static std::vector<const void *> LiveDispatches;
static void RequireLiveDispatch(const void *object) {
    Require(!object || std::find(LiveDispatches.begin(), LiveDispatches.end(), object) != LiveDispatches.end(),
            "menu cleanup attempts to AddRef an unconstructed Window's poisoned Owner");
}
struct tTJSVariantClosure {
    iTJSDispatch2 *Object, *ObjThis;
    int FuncCall(int, const wchar_t *, void *, tTJSVariant *, int,
                 tTJSVariant **, iTJSDispatch2 *);
    void Invalidate(int, void *, void *, iTJSDispatch2 *);
    void Release();
};
class iTJSDispatch2 {
public:
    enum Kind { Owner, Global, MenuFactory, MenuClose, DrawClass, DrawObject, Menu };
    Kind Type;
    int RefCount = 1, AddRefs = 0, Invalidates = 0;
    iTJSDispatch2 *MenuMember = nullptr;
    explicit iTJSDispatch2(Kind kind) : Type(kind) { LiveDispatches.push_back(this); }
    ~iTJSDispatch2() {
        LiveDispatches.erase(std::remove(LiveDispatches.begin(), LiveDispatches.end(), this), LiveDispatches.end());
    }
    void AddRef() { ++RefCount; ++AddRefs; }
    void Release() { Require(RefCount > 0, "dispatch reference underflow"); --RefCount; }
    int PropGet(int, const wchar_t *, void *, tTJSVariant *, iTJSDispatch2 *);
    int PropSet(int, const wchar_t *, void *, const tTJSVariant *, iTJSDispatch2 *);
    int DeleteMember(int, const wchar_t *, void *, iTJSDispatch2 *);
    int CreateNew(int, void *, void *, iTJSDispatch2 **, int, void *, iTJSDispatch2 *);
    int FuncCall(tTJSVariant *, int, tTJSVariant **);
};
class tTJSVariant {
    iTJSDispatch2 *Object = nullptr, *ObjThis = nullptr;
    void Retain() {
        // Detect the real invalid AddRef attempt without invoking undefined
        // behavior or crashing the host process on its controlled poison bytes.
        RequireLiveDispatch(Object); RequireLiveDispatch(ObjThis);
        if(Object) Object->AddRef(); if(ObjThis) ObjThis->AddRef();
    }
    void Drop() { if(Object) Object->Release(); if(ObjThis) ObjThis->Release(); }
public:
    tTJSVariant() = default;
    tTJSVariant(iTJSDispatch2 *object, iTJSDispatch2 *objthis)
        : Object(object), ObjThis(objthis) { Retain(); }
    tTJSVariant(const tTJSVariant &other)
        : Object(other.Object), ObjThis(other.ObjThis) { Retain(); }
    tTJSVariant &operator=(const tTJSVariant &other) {
        if(this != &other) { Drop(); Object = other.Object; ObjThis = other.ObjThis; Retain(); }
        return *this;
    }
    ~tTJSVariant() { Drop(); }
    int Type() const { return Object ? tvtObject : tvtVoid; }
    iTJSDispatch2 *AsObjectNoAddRef() const { return Object; }
    tTJSVariantClosure AsObjectClosureNoAddRef() const { return {Object, ObjThis}; }
};
static iTJSDispatch2 Global(iTJSDispatch2::Global), Factory(iTJSDispatch2::MenuFactory),
    Close(iTJSDispatch2::MenuClose), DrawClass(iTJSDispatch2::DrawClass),
    DrawObject(iTJSDispatch2::DrawObject), Menu(iTJSDispatch2::Menu);
static int MenuCreates = 0, MenuCloses = 0;
static iTJSDispatch2 *LastClosedOwner = nullptr;
static bool ThrowDrawCreation = false;
int iTJSDispatch2::PropGet(int, const wchar_t *name, void *, tTJSVariant *out,
                          iTJSDispatch2 *) {
    iTJSDispatch2 *value = nullptr;
    if(Type == Global && std::wstring(name) == L"__krkrnsCreateMenuRoot") value = &Factory;
    if(Type == Global && std::wstring(name) == L"__krkrnsMenuClose") value = &Close;
    if(Type == Owner && std::wstring(name) == L"menu") value = MenuMember;
    if(!value) return -1;
    *out = tTJSVariant(value, value);
    return 0;
}
int iTJSDispatch2::PropSet(int, const wchar_t *, void *, const tTJSVariant *value,
                          iTJSDispatch2 *) {
    Require(Type == Owner && !MenuMember, "unexpected menu assignment");
    MenuMember = value->AsObjectNoAddRef();
    if(MenuMember) MenuMember->AddRef();
    return 0;
}
int iTJSDispatch2::DeleteMember(int, const wchar_t *, void *, iTJSDispatch2 *) {
    if(MenuMember) MenuMember->Release();
    MenuMember = nullptr;
    return 0;
}
int iTJSDispatch2::CreateNew(int, void *, void *, iTJSDispatch2 **out, int, void *,
                            iTJSDispatch2 *) {
    *out = &::DrawObject; ::DrawObject.AddRef(); return 0;
}
int iTJSDispatch2::FuncCall(tTJSVariant *out, int count, tTJSVariant **args) {
    Require(count == 1 && args[0]->AsObjectNoAddRef(), "menu owner argument missing");
    if(Type == MenuFactory) { ++MenuCreates; *out = tTJSVariant(&::Menu, &::Menu); }
    else if(Type == MenuClose) { ++MenuCloses; LastClosedOwner = args[0]->AsObjectNoAddRef(); }
    return 0;
}
int tTJSVariantClosure::FuncCall(int, const wchar_t *, void *, tTJSVariant *out,
        int count, tTJSVariant **args, iTJSDispatch2 *) { return Object->FuncCall(out, count, args); }
void tTJSVariantClosure::Invalidate(int, void *, void *, iTJSDispatch2 *) { ++Object->Invalidates; }
void tTJSVariantClosure::Release() { if(Object) Object->Release(); if(ObjThis) ObjThis->Release(); }
class tTJS { public: iTJSDispatch2 *GetGlobalNoAddRef() { return &Global; } };
static tTJS Engine;
static tTJS *TVPGetScriptEngine() { return &Engine; }
static iTJSDispatch2 *TVPCreateDefaultDrawDevice() {
    if(ThrowDrawCreation) throw eTJSError("draw constructor failure");
    DrawClass.AddRef(); return &DrawClass;
}
static const int TVPInternalError = 0;
static void TVPThrowExceptionMessage(int, const wchar_t *) { throw eTJSError("construction failed"); }
class tTJSNI_Window;
static std::vector<tTJSNI_Window *> Registry;
static void TVPRegisterWindowToList(tTJSNI_Window *w) { Registry.push_back(w); }
static void TVPUnregisterWindowToList(tTJSNI_Window *w) {
    Registry.erase(std::remove(Registry.begin(), Registry.end(), w), Registry.end());
}
static void TVPCancelSourceEvents(iTJSDispatch2 *) {}
static void TVPCancelInputEvents(void *) {}
static void TVPRemoveWindowUpdate(tTJSNI_Window *) {}
class tTJSNI_BaseVideoOverlay { public: void Disconnect() {} };
struct OverlayList : std::vector<tTJSNI_BaseVideoOverlay *> {
    int GetSafeLockedObjectCount() { return 0; }
    tTJSNI_BaseVideoOverlay *GetSafeLockedObjectAt(int) { return nullptr; }
};
template<class T> class tObjectListSafeLockHolder {
public:
    explicit tObjectListSafeLockHolder(std::vector<T *> &) {}
    int GetSafeLockedObjectCount() { return 0; }
    T *GetSafeLockedObjectAt(int) { return nullptr; }
};
class NativeBase { public: int NativeInvalidates = 0; void Invalidate() { ++NativeInvalidates; } };
class tTJSNI_BaseWindow : public NativeBase {
    using inherited = NativeBase;
public:
    iTJSDispatch2 *Owner; // deliberately mirrors the declaration, not the repair
    bool WaitVSync, ObjectVectorLocked, WindowUpdating;
    int *DrawBuffer;
    std::vector<int> WindowExposedRegion;
    void *DrawDevice;
    OverlayList VideoOverlay;
    std::vector<tTJSVariantClosure> ObjectVector;
    tTJSVariant DrawDeviceObject;
    tTJSNI_BaseWindow();
    int Construct(int, tTJSVariant **, iTJSDispatch2 *);
    void Invalidate();
    iTJSDispatch2 *GetOwnerNoAddRef() { return Owner; }
    void SetDrawDeviceObject(const tTJSVariant &value) { DrawDeviceObject = value; }
};
class TTVPWindowForm { public: void InvalidateClose() {} };
class tTJSNI_Window : public tTJSNI_BaseWindow {
public:
    TTVPWindowForm *Form;
    tTJSNI_Window();
    void Invalidate();
};
'''


CASES = r'''
// TJS attaches a native instance before invoking the named constructor. This
// fixture deliberately keeps those two operations separate, including unwind.
struct AttachedNative {
    void *Storage = ::operator new(sizeof(tTJSNI_Window));
    tTJSNI_Window *Window;
    bool Finalized = false;
    AttachedNative() {
        const char16_t path[] = u"sdmc:/switch/KRKR-ns/game/data.xp3";
        for(size_t i = 0; i < sizeof(tTJSNI_Window); ++i)
            static_cast<unsigned char *>(Storage)[i] = reinterpret_cast<const unsigned char *>(path)[i % (sizeof(path) - 2)];
        Window = new(Storage) tTJSNI_Window;
    }
    void Finalize() { Window->Invalidate(); Finalized = true; }
    ~AttachedNative() {
        TVPUnregisterWindowToList(Window);
        Window->~tTJSNI_Window();
        ::operator delete(Storage);
    }
};
static void ResetCounters() { MenuCreates = MenuCloses = Menu.Invalidates = 0; LastClosedOwner = nullptr; }
static void CheckUnconstructed(bool throws) {
    ResetCounters();
    AttachedNative native;
#ifndef TEST_OLD_CONSTRUCTOR
    Require(native.Window->GetOwnerNoAddRef() == nullptr,
            "native Window retains path bytes as Owner before Construct");
#endif
    if(throws) {
        try { throw eTJSError("script constructor failed before super"); }
        catch(const eTJSError &) { native.Finalize(); }
    } else native.Finalize();
    Require(native.Window->GetOwnerNoAddRef() == nullptr && native.Window->NativeInvalidates == 1,
            "unconstructed native finalization did not complete");
    Require(MenuCreates == 0 && MenuCloses == 0 && Menu.Invalidates == 0 && Registry.empty(),
            "unconstructed Window invoked menus or entered the window registry");
}
static void CheckConstructed(bool throws_after, bool throws_in_base) {
    ResetCounters();
    iTJSDispatch2 owner(iTJSDispatch2::Owner);
    AttachedNative native;
    ThrowDrawCreation = throws_in_base;
    try {
        native.Window->Construct(0, nullptr, &owner);
        if(throws_after) throw eTJSError("script constructor failed after super");
    } catch(const eTJSError &) {}
    ThrowDrawCreation = false;
    Require(native.Window->GetOwnerNoAddRef() == &owner && Registry.size() == 1,
            "Construct failed to bind the legitimate owner");
    Require(MenuCreates == (throws_in_base ? 0 : 1), "normal menu factory behavior changed");
    native.Finalize();
    Require(native.Window->GetOwnerNoAddRef() == nullptr && Registry.empty(),
            "constructed or partially constructed Window did not finish invalidation");
    Require(MenuCloses == 1 && LastClosedOwner == &owner,
            "constructed Window did not close its own menu");
    Require(Menu.Invalidates == (throws_in_base ? 0 : 1) && !owner.MenuMember && owner.RefCount == 1,
            "menu invalidation or temporary owner references leaked");
}
int main() {
    try {
        CheckUnconstructed(false);
        CheckUnconstructed(true);
#ifndef TEST_OLD_LIFECYCLE
        CheckConstructed(false, false);
        CheckConstructed(true, false);
        CheckConstructed(false, true);
#endif
    } catch(const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
#ifdef TEST_OLD_LIFECYCLE
    std::cout << "PASS: original ctor + original Base Invalidate safely finalize omitted super and pre-super throw; no menu Owner dereference\n";
#else
    std::cout << "PASS: omitted super, pre-super throw, normal menus, post-super throw, partial native construction\n";
#endif
}
'''


def function(text, signature):
    start = text.index(signature)
    body = text.index('{', start)
    depth = 0
    for end in range(body, len(text)):
        if text[end] == '{':
            depth += 1
        elif text[end] == '}':
            depth -= 1
            if depth == 0:
                return text[start:end + 1] + '\n'
    raise ValueError(f'unterminated function: {signature}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path)
    baseline = parser.add_mutually_exclusive_group()
    baseline.add_argument('--git-head-constructor', action='store_true',
                          help='negative control: Git HEAD ctor with current menu invalidation')
    baseline.add_argument('--git-head-lifecycle', action='store_true',
                          help='causal control: Git HEAD ctor and Base Invalidate, before menu cleanup')
    parser.add_argument('--cmake', default=shutil.which('cmake'))
    args = parser.parse_args()
    if not args.cmake:
        parser.error('CMake is required; install it on PATH or pass --cmake.')
    root = Path(__file__).resolve().parent.parent
    port = root / 'krkrsdl2'
    base = (port / 'external/krkrz/visual/WindowIntf.cpp').read_text(encoding='utf-8')
    platform = (port / 'src/core/visual/sdl2/WindowImpl.cpp').read_text(encoding='utf-8')
    ctor_source = invalidation_source = base
    defines = ''
    if args.git_head_constructor or args.git_head_lifecycle:
        repo_root = Path(subprocess.check_output(
            ['git', '-C', str(port), 'rev-parse', '--show-toplevel'],
            text=True, encoding='utf-8').strip()).resolve()
        tracked_path = (port / 'external/krkrz/visual/WindowIntf.cpp').relative_to(repo_root).as_posix()
        ctor_source = subprocess.check_output(
            ['git', '-C', str(port), 'show', f'HEAD:{tracked_path}'],
            text=True, encoding='utf-8')
        defines = '#define TEST_OLD_CONSTRUCTOR\n'
        if args.git_head_lifecycle:
            invalidation_source = ctor_source
            defines += '#define TEST_OLD_LIFECYCLE\n'
    implementations = (
        function(ctor_source, 'tTJSNI_BaseWindow::tTJSNI_BaseWindow()') +
        'tjs_error TJS_INTF_METHOD\n' + function(base, 'tTJSNI_BaseWindow::Construct(') +
        'void TJS_INTF_METHOD\n' + function(invalidation_source, 'tTJSNI_BaseWindow::Invalidate()') +
        function(platform, 'tTJSNI_Window::tTJSNI_Window()') +
        'void TJS_INTF_METHOD\n' + function(platform, 'tTJSNI_Window::Invalidate()'))
    out = (args.output_dir or root / 'build-window-construction-test').resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'window_construction_test.cpp').write_text(defines + HARNESS + implementations + CASES, encoding='utf-8')
    (out / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.20)\n'
        'project(window_construction_test LANGUAGES CXX)\n'
        'set(CMAKE_CXX_STANDARD 17)\n'
        'add_executable(window_construction_test window_construction_test.cpp)\n'
        'if(MSVC)\n target_compile_options(window_construction_test PRIVATE /EHsc /utf-8)\nendif()\n',
        encoding='utf-8')
    subprocess.run([args.cmake, '-S', str(out), '-B', str(out / 'build')], check=True)
    subprocess.run([args.cmake, '--build', str(out / 'build'), '--config', 'Release'], check=True)
    binary = next(path for path in (out / 'build/Release/window_construction_test.exe',
                                   out / 'build/window_construction_test') if path.is_file())
    return subprocess.run([str(binary)]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
