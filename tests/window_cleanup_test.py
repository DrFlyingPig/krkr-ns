"""Compile the production cleanup function against controlled window lifetimes."""
import argparse
from pathlib import Path
import shutil
import subprocess

HARNESS = r'''
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>
using tjs_int = int;
static int FormsFreed = 0, Messages = 0;
#define KRKRNS_LOG(...) (++Messages)
class tTJSNI_Window;
class TVPWindowWindow;
static TVPWindowWindow *_currentWindowWindow = nullptr;
static TVPWindowWindow *_lastWindowWindow = nullptr;
static std::vector<tTJSNI_Window*> Registry;
class TTVPWindowForm {
public:
    virtual void InvalidateClose() = 0;
    virtual ~TTVPWindowForm() = default;
};
class TVPWindowWindow final : public TTVPWindowForm {
public:
    tTJSNI_Window *TJSNativeInstance = nullptr;
    ~TVPWindowWindow() {
        ++FormsFreed;
        if (_currentWindowWindow == this) _currentWindowWindow = nullptr;
        if (_lastWindowWindow == this) _lastWindowWindow = nullptr;
    }
    void SetVisible(bool) {}
    void InvalidateClose() override;
};
class iTJSDispatch2 {
public:
    tTJSNI_Window *Window = nullptr;
    bool ThrowFinalize = false;
    bool DeleteOnZero = false;
    int RefCount = 1;
    int Finalizes = 0;
    void AddRef() { ++RefCount; }
    void Release();
    void Invalidate();
};
struct WindowLifetime {
    bool Alive = true, DestroyedDuringCleanup = false, FallbackComplete = false;
    int NativeCalls = 0, RefCountAfterRootRelease = 0, Destructions = 0;
};
class tTJSVariant {
    iTJSDispatch2 *Object, *ObjThis;
public:
    tTJSVariant(iTJSDispatch2 *object, iTJSDispatch2 *objthis)
        : Object(object), ObjThis(objthis) {
        if(Object) Object->AddRef();
        if(ObjThis) ObjThis->AddRef();
    }
    ~tTJSVariant() {
        if(Object) Object->Release();
        if(ObjThis) ObjThis->Release();
    }
    tTJSVariant(const tTJSVariant&) = delete;
    tTJSVariant& operator=(const tTJSVariant&) = delete;
};
class tTJSNI_Window {
public:
    iTJSDispatch2 *Owner = nullptr;
    TTVPWindowForm *Form = new TVPWindowWindow;
    bool ThrowNativeChild = false, ThrowBeforeUnregister = false;
    bool DropRootReference = false;
    WindowLifetime *Lifetime = nullptr;
    int Invalidates = 0;
    tTJSNI_Window() {
        static_cast<TVPWindowWindow*>(Form)->TJSNativeInstance = this;
        Registry.push_back(this);
    }
    ~tTJSNI_Window() {
        Registry.erase(std::remove(Registry.begin(), Registry.end(), this), Registry.end());
        if(Lifetime) {
            Lifetime->Alive = false;
            Lifetime->DestroyedDuringCleanup = !Lifetime->FallbackComplete;
            ++Lifetime->Destructions;
        }
    }
    iTJSDispatch2 *GetOwnerNoAddRef() { return Owner; }
    TTVPWindowForm *GetForm() { return Form; }
    void NotifyWindowClose() {
        Form = nullptr;
        if(Lifetime) Lifetime->FallbackComplete = true;
    }
    void Invalidate() {
        ++Invalidates;
        if(Lifetime) ++Lifetime->NativeCalls;
        if (ThrowBeforeUnregister) throw std::runtime_error("native failure before unregister");
        if(DropRootReference) {
            DropRootReference = false;
            // The root was the last owner of this Window before the production
            // guard took references. Its invalidation releases that reference.
            Owner->Release();
            if(Lifetime) Lifetime->RefCountAfterRootRelease = Owner->RefCount;
        }
        Registry.erase(std::remove(Registry.begin(), Registry.end(), this), Registry.end());
        if (ThrowNativeChild) throw std::runtime_error("child invalidation failed");
        if (Form) Form->InvalidateClose();
        Form = nullptr;
    }
};
void iTJSDispatch2::Release() {
    if(--RefCount == 0 && DeleteOnZero) {
        delete Window;
        delete this;
    }
}
void iTJSDispatch2::Invalidate() {
    ++Finalizes;
    if (ThrowFinalize) throw std::runtime_error("extractTrigger is already invalidated");
    Window->Invalidate();
}
struct tTJSVariantClosure {
    iTJSDispatch2 *Object, *ObjThis = nullptr;
    explicit tTJSVariantClosure(iTJSDispatch2 *object) : Object(object) {}
    void Invalidate(int, void*, void*, iTJSDispatch2*) { Object->Invalidate(); }
};
int TVPGetWindowCount() { return static_cast<int>(Registry.size()); }
tTJSNI_Window *TVPGetWindowListAt(int i) { return Registry.at(i); }
static void Require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
static void Reset() {
    Registry.clear();
    _currentWindowWindow = _lastWindowWindow = nullptr;
    FormsFreed = Messages = 0;
}
'''

CASES = r'''
int main() {
    try {
        Reset();
        tTJSNI_Window normal;
        iTJSDispatch2 normalOwner;
        normal.Owner = &normalOwner; normalOwner.Window = &normal;
        krkrsdl2_release_leftover_windows();
        Require(Registry.empty() && normal.Invalidates == 1 && FormsFreed == 1,
                "normal script invalidation releases its native window once");

        Reset();
        tTJSNI_Window broken, following;
        iTJSDispatch2 brokenOwner, followingOwner;
        broken.Owner = &brokenOwner; brokenOwner.Window = &broken;
        brokenOwner.ThrowFinalize = true;
        following.Owner = &followingOwner; followingOwner.Window = &following;
        krkrsdl2_release_leftover_windows();
        krkrsdl2_release_leftover_windows(); // engine restart step 1 calls it again
        Require(Registry.empty() && brokenOwner.Finalizes == 1 && broken.Invalidates == 1,
                "a failed finalize is not retried by the event loop or restart");
        Require(followingOwner.Finalizes == 1 && FormsFreed == 2,
                "one failed finalize does not prevent cleanup of the next window");

        Reset();
        tTJSNI_Window nativeFailure;
        iTJSDispatch2 nativeOwner;
        nativeFailure.Owner = &nativeOwner; nativeOwner.Window = &nativeFailure;
        nativeOwner.ThrowFinalize = true; nativeFailure.ThrowNativeChild = true;
        _currentWindowWindow = _lastWindowWindow =
            static_cast<TVPWindowWindow*>(nativeFailure.Form);
        krkrsdl2_release_leftover_windows();
        Require(Registry.empty() && nativeFailure.Form == nullptr && FormsFreed == 1,
                "native child failure severs the form before releasing it once");
        Require(!_currentWindowWindow && !_lastWindowWindow,
                "aliased form pointers cannot cause a second deletion");

        Reset();
        WindowLifetime menuRootLifetime;
        auto *menuRootWindow = new tTJSNI_Window;
        auto *menuRootOwner = new iTJSDispatch2;
        menuRootWindow->Owner = menuRootOwner; menuRootOwner->Window = menuRootWindow;
        menuRootOwner->DeleteOnZero = true;
        menuRootOwner->ThrowFinalize = true;
        menuRootWindow->DropRootReference = true;
        menuRootWindow->ThrowNativeChild = true;
        menuRootWindow->Lifetime = &menuRootLifetime;
        // RefCount == 1 models a game that dropped its Window reference: the
        // menu root alone retains the dispatch. A failed script finalizer sends
        // production cleanup through native fallback, which breaks that cycle.
        krkrsdl2_release_leftover_windows();
        Require(menuRootLifetime.RefCountAfterRootRelease == 2,
                "the real dispatch guard retains both closure references after root release");
        Require(menuRootLifetime.FallbackComplete && !menuRootLifetime.DestroyedDuringCleanup,
                "Window stays alive through fallback GetForm and NotifyWindowClose");
        Require(!menuRootLifetime.Alive && menuRootLifetime.Destructions == 1 &&
                menuRootLifetime.NativeCalls == 1 && Registry.empty() && FormsFreed == 1,
                "the guard releases and destroys Window exactly once after fallback completes");

        Reset();
        tTJSNI_Window registeredChildFailure;
        iTJSDispatch2 registeredChildOwner;
        registeredChildFailure.Owner = &registeredChildOwner;
        registeredChildOwner.Window = &registeredChildFailure;
        registeredChildFailure.ThrowNativeChild = true;
        _currentWindowWindow = _lastWindowWindow =
            static_cast<TVPWindowWindow*>(registeredChildFailure.Form);
        krkrsdl2_release_leftover_windows();
        Require(Registry.empty() && registeredChildFailure.Invalidates == 1,
                "native invalidation that already unregistered is not retried");
        Require(registeredChildFailure.Form == nullptr && FormsFreed == 1,
                "a directly closed leftover form notifies its native instance");

        Reset();
        tTJSNI_Window ownerless;
        krkrsdl2_release_leftover_windows();
        Require(Registry.empty() && ownerless.Invalidates == 1 && FormsFreed == 1,
                "an ownerless window still releases native resources");

        Reset();
        tTJSNI_Window noProgress;
        iTJSDispatch2 noProgressOwner;
        noProgress.Owner = &noProgressOwner; noProgressOwner.Window = &noProgress;
        noProgressOwner.ThrowFinalize = true; noProgress.ThrowBeforeUnregister = true;
        krkrsdl2_release_leftover_windows();
        Require(noProgressOwner.Finalizes == 1 && noProgress.Invalidates == 1 && FormsFreed == 1,
                "failure before unregister returns without retrying a stuck registry");
        Require(noProgress.Form == nullptr, "emergency native close leaves no stale form");
        Registry.clear();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS: normal cleanup, failed finalize, menu-root lifetime, failed native child, ownerless window, and no-progress bound\n";
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
    source = args.source or root / 'krkrsdl2/src/core/sdl2/SDLApplication.cpp'
    text = source.read_text(encoding='utf-8')
    start = text.index('void krkrsdl2_release_leftover_windows()\n{')
    end = text.index('\n}\n', start) + 3
    close_start = text.index('void TVPWindowWindow::InvalidateClose()\n{')
    close_end = text.index('\n}\n', close_start) + 3
    out = (args.output_dir or root / 'build-scene-freeze/window-cleanup-test').resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'window_cleanup_test.cpp').write_text(
        HARNESS + text[close_start:close_end] + text[start:end] + CASES,
                                                encoding='utf-8')
    (out / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.20)\n'
        'project(window_cleanup_test LANGUAGES CXX)\n'
        'set(CMAKE_CXX_STANDARD 17)\n'
        'add_executable(window_cleanup_test window_cleanup_test.cpp)\n'
        'if(MSVC)\n target_compile_options(window_cleanup_test PRIVATE /EHsc /utf-8)\nendif()\n',
        encoding='utf-8')
    subprocess.run([args.cmake, '-S', str(out), '-B', str(out / 'build')], check=True)
    subprocess.run([args.cmake, '--build', str(out / 'build'), '--config', 'Release'], check=True)
    binaries = [out / 'build/Release/window_cleanup_test.exe',
                out / 'build/window_cleanup_test']
    binary = next(path for path in binaries if path.is_file())
    return subprocess.run([str(binary)]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
