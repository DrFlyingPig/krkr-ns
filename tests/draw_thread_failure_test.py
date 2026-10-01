"""Compile the production SDL draw pool and inject SDL_CreateThread failures.

Use real SDL threads, mutexes and conditions. Extract the production base-thread
declaration/implementation and the complete draw pool unchanged; only allocation
and thread-creation calls are wrapped to inject failure and count owned resources.
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess


HARNESS = r'''
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <process.h>
#endif
using tjs_int = int;
using TVP_THREAD_TASK_FUNC = void (*)(void*);
using TVP_THREAD_PARAM = void*;
constexpr int TVPMaxThreadNum = 8;
#define TJS_USERENTRY
#define KRKRZ_USE_SDL_THREADS
#define TVPThrowInternalError throw std::runtime_error("injected SDL thread creation failure")
#define KRKRNS_LOG(...) ((void)0)
static int ConfiguredThreads = 4;
tjs_int TVPGetThreadNum() { return ConfiguredThreads; }
static int LiveThreads = 0, LiveMutexes = 0, LiveConditions = 0;
static int CreateAttempts = 0, FailAtAttempt = 0;
static bool FailEveryAttempt = false;
static SDL_cond *LastCondition = nullptr, *PoolCompletionCondition = nullptr;
static SDL_sem *CompletionGate = nullptr;
static bool GateEnabled = false;
static std::atomic<bool> CompletionWaitObserved{false};
static SDL_Thread* CreateThread(SDL_ThreadFunction fn, const char* name, void* arg) {
    ++CreateAttempts;
    if (FailEveryAttempt || CreateAttempts == FailAtAttempt) return nullptr;
    auto* thread = SDL_CreateThread(fn, name, arg);
    if (thread) ++LiveThreads;
    return thread;
}
static void WaitThread(SDL_Thread* thread, int* status) {
    SDL_WaitThread(thread, status);
    if (thread) --LiveThreads;
}
static SDL_mutex* CreateMutex() {
    auto* mutex = SDL_CreateMutex();
    if (mutex) ++LiveMutexes;
    return mutex;
}
static void DestroyMutex(SDL_mutex* mutex) {
    SDL_DestroyMutex(mutex);
    if (mutex) --LiveMutexes;
}
static SDL_cond* CreateCondition() {
    auto* condition = SDL_CreateCond();
    if (condition) ++LiveConditions;
    LastCondition = condition;
    return condition;
}
static void DestroyCondition(SDL_cond* condition) {
    SDL_DestroyCond(condition);
    if (condition) --LiveConditions;
}
static int WaitCondition(SDL_cond* condition, SDL_mutex* mutex) {
    // Hold one real worker until EndTask reaches the actual completion wait.
    // This verifies that returning from EndTask includes all worker writes.
    if (GateEnabled && condition == PoolCompletionCondition &&
        !CompletionWaitObserved.exchange(true)) SDL_SemPost(CompletionGate);
    return SDL_CondWait(condition, mutex);
}
#undef SDL_CreateThread
#define SDL_CreateThread CreateThread
#define SDL_WaitThread WaitThread
#define SDL_CreateMutex CreateMutex
#define SDL_DestroyMutex DestroyMutex
#define SDL_CreateCond CreateCondition
#define SDL_DestroyCond DestroyCondition
#define SDL_CondWait WaitCondition
// SDL was already included for the real host platform. Disable only the pool's
// unrelated Windows affinity enumeration; the production SDL execution stays on.
#undef _WIN32
'''


CASES = r'''
static void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static unsigned Batches = 0, Tasks = 0;
static unsigned long long ComparedBytes = 0;
static SDL_threadID Caller;
struct Work {
    const uint32_t* source;
    uint32_t* destination;
    int begin, end, id;
    int calls = 0;
    bool onCaller = false;
};
static uint32_t Pixel(uint32_t source, unsigned x, unsigned y) {
    return ((source << 7) | (source >> 25)) ^ (x * 0x10203u) ^ (y * 0x40506u);
}
static void RunWork(void* raw) {
    auto& work = *static_cast<Work*>(raw);
    ++work.calls;
    work.onCaller = SDL_ThreadID() == Caller;
    if (work.id == 0 && GateEnabled) SDL_SemWait(CompletionGate);
    for (int y = work.begin; y < work.end; ++y)
        for (int x = 0; x < 65; ++x)
            work.destination[y * 65 + x] = Pixel(work.source[y * 65 + x], x, y);
}
static void CheckResources(int workers) {
    Require(LiveThreads == workers, "unexpected live worker count");
    Require(LiveMutexes == 1 + 2 * workers && LiveConditions == 1 + 2 * workers,
            "failed worker leaked synchronization resources");
}
static void CheckBatch(DrawThreadPool& pool, int requested, int asyncTasks) {
    constexpr int width = 65, height = 97, guard = 16;
    std::vector<uint32_t> source(width * height + 2 * guard);
    for (size_t i = 0; i < source.size(); ++i) source[i] = uint32_t(i * 0x9e3779b9u);
    const auto savedSource = source;
    std::vector<uint32_t> destination(source.size(), 0xdeadbeefu), expected = destination;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            expected[guard + y * width + x] = Pixel(source[guard + y * width + x], x, y);
    std::array<Work, TVPMaxThreadNum> work;
    GateEnabled = asyncTasks != 0;
    CompletionWaitObserved.store(false);
    pool.BeginTask(requested);
    for (int i = 0; i < requested; ++i) {
        work[i] = {source.data() + guard, destination.data() + guard,
                   height * i / requested, height * (i + 1) / requested, i};
        pool.ExecTask(RunWork, &work[i]);
    }
    pool.WaitForTask();
    Require(destination == expected && source == savedSource,
            "task fallback lost pixels, changed a source, or touched guards");
    for (int i = 0; i < requested; ++i) {
        Require(work[i].calls == 1, "a submitted task was dropped or ran twice");
        Require(work[i].onCaller == (i >= asyncTasks), "task used an unavailable worker");
    }
    Require(CompletionWaitObserved.load() == (asyncTasks != 0),
            "EndTask did not wait for its blocked worker");
    Require(SDL_SemValue(CompletionGate) == 0, "completion gate was not consumed");
    GateEnabled = false;
    ++Batches;
    Tasks += requested;
    ComparedBytes += 2 * destination.size() * sizeof(uint32_t);
}
static void ResetFailure(int threads = 4) {
    ConfiguredThreads = threads;
    CreateAttempts = FailAtAttempt = 0;
    FailEveryAttempt = false;
    Require(LiveThreads == 0 && LiveMutexes == 0 && LiveConditions == 0,
            "previous pool did not release all resources");
}
static void TestFirstFailure() {
    ResetFailure();
    FailEveryAttempt = true;
    {
        DrawThreadPool pool;
        PoolCompletionCondition = LastCondition;
        for (int i = 0; i < 20; ++i) {
            CheckBatch(pool, 4, 0);
            CheckResources(0);
        }
        Require(CreateAttempts == 20, "failed creation did not retry on later batches");
        FailEveryAttempt = false;
        CheckBatch(pool, 4, 3);
        CheckResources(3);
    }
    ResetFailure();
}
static void TestPartialFailure() {
    ResetFailure();
    FailAtAttempt = 2;
    {
        DrawThreadPool pool;
        PoolCompletionCondition = LastCondition;
        CheckBatch(pool, 4, 1);
        CheckResources(1);
        Require(CreateAttempts == 2, "pool continued creating after failure");
        FailAtAttempt = 0;
        FailEveryAttempt = true;
        for (int i = 0; i < 20; ++i) {
            CheckBatch(pool, 4, 1);
            CheckResources(1);
        }
        FailEveryAttempt = false;
        CheckBatch(pool, 4, 3);
        CheckResources(3);
        ConfiguredThreads = 6;
        FailEveryAttempt = true;
        CheckBatch(pool, 6, 3);
        CheckResources(3);
        FailEveryAttempt = false;
        CheckBatch(pool, 6, 5);
        CheckResources(5);
        ConfiguredThreads = 2;
        CheckBatch(pool, 2, 1);
        CheckResources(5); // session workers remain alive, as in production
    }
}
static void TestNormalAndSmallBatches() {
    ResetFailure();
    {
        DrawThreadPool pool;
        PoolCompletionCondition = LastCondition;
        FailEveryAttempt = true;
        CheckBatch(pool, 1, 0);
        Require(CreateAttempts == 0, "a one-item batch created workers");
        FailEveryAttempt = false;
        CheckBatch(pool, 2, 1);
        CheckResources(3);
        CheckBatch(pool, 1, 0);
        CheckBatch(pool, 4, 3);
    }
    ResetFailure(2);
    {
        DrawThreadPool pool;
        PoolCompletionCondition = LastCondition;
        CheckBatch(pool, 8, 1); // task partitions can exceed available execution capacity
        CheckResources(1);
    }
}
int main() {
    try {
        Require(SDL_Init(0) == 0, "SDL_Init failed");
        Caller = SDL_ThreadID();
        CompletionGate = SDL_CreateSemaphore(0);
        Require(CompletionGate != nullptr, "SDL semaphore creation failed");
        TestFirstFailure();
        TestPartialFailure();
        TestNormalAndSmallBatches();
        ResetFailure();
        SDL_DestroySemaphore(CompletionGate);
        SDL_Quit();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS: first/partial/repeated creation failures, recovery, capacity changes, "
                 "small batches and actual completion waits; no leaked SDL threads/mutexes/conditions; "
              << Batches << " complete buffers, " << Tasks << " exact-once tasks, "
              << ComparedBytes << " compared bytes\n";
}
'''


def definition(text, signature):
    start = text.index(signature)
    body = text.index('{', start)
    depth = 0
    for end in range(body, len(text)):
        if text[end] == '{':
            depth += 1
        elif text[end] == '}':
            depth -= 1
            if depth == 0:
                return text[start:end + 1] + ';\n'
    raise ValueError(f'unterminated definition: {signature}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--git-head', action='store_true', help='negative control using the old pool')
    parser.add_argument('--sdl-dir', type=Path)
    parser.add_argument('--sdl-include-dir', type=Path)
    parser.add_argument('--sdl-library', type=Path)
    parser.add_argument('--sdl-runtime', type=Path)
    parser.add_argument('--cmake', default=shutil.which('cmake'))
    args = parser.parse_args()
    if not args.cmake:
        parser.error('CMake is required; install it on PATH or pass --cmake.')
    root = Path(__file__).resolve().parent.parent
    source_path = args.source or root / 'krkrsdl2/external/krkrz/utils/ThreadIntf.cpp'
    raw_source = source_path.read_bytes()
    if args.git_head:
        raw_source = subprocess.check_output(
            ['git', '-C', str(root), 'show',
             'HEAD:krkrsdl2/external/krkrz/utils/ThreadIntf.cpp'])
    source = raw_source.decode('utf-8').replace('\r\n', '\n')
    header = (root / 'krkrsdl2/external/krkrz/utils/ThreadIntf.h').read_text(encoding='utf-8')
    declaration = definition(header, 'enum tTVPThreadPriority') + definition(header, 'class tTVPThread\n')
    methods = source[source.index('tTVPThread::tTVPThread()'):
                     source.index('tTVPThreadPriority tTVPThread::GetPriority()')]
    pool = source[source.index('static void TJS_USERENTRY DummyThreadTask'):
                  source.index('static DrawThreadPool TVPTheadPool;')]
    out = (args.output_dir or root / 'build-draw-thread-failure-test').resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'draw_thread_failure_test.cpp').write_text(
        HARNESS + declaration + methods + pool + CASES, encoding='utf-8')
    cmake = ('cmake_minimum_required(VERSION 3.20)\n'
             'project(draw_thread_failure_test LANGUAGES CXX)\n'
             'set(CMAKE_CXX_STANDARD 17)\n')
    if args.sdl_library:
        if not args.sdl_include_dir:
            parser.error('--sdl-library requires --sdl-include-dir')
        cmake += ('add_library(SDL2::SDL2 UNKNOWN IMPORTED)\n'
                  'set_target_properties(SDL2::SDL2 PROPERTIES\n'
                  f' IMPORTED_LOCATION "{args.sdl_library.resolve().as_posix()}"\n'
                  f' INTERFACE_INCLUDE_DIRECTORIES "{args.sdl_include_dir.resolve().as_posix()}")\n')
    else:
        cmake += 'find_package(SDL2 CONFIG REQUIRED)\n'
    cmake += ('add_executable(draw_thread_failure_test draw_thread_failure_test.cpp)\n'
              'target_compile_definitions(draw_thread_failure_test PRIVATE SDL_MAIN_HANDLED)\n'
              'target_link_libraries(draw_thread_failure_test PRIVATE SDL2::SDL2)\n'
              'if(MSVC)\n target_compile_options(draw_thread_failure_test PRIVATE /EHsc /utf-8)\nendif()\n')
    runtime = f'"{args.sdl_runtime.resolve().as_posix()}"' if args.sdl_runtime else '$<TARGET_FILE:SDL2::SDL2>'
    cmake += ('if(WIN32)\nadd_custom_command(TARGET draw_thread_failure_test POST_BUILD\n'
              f' COMMAND ${{CMAKE_COMMAND}} -E copy_if_different {runtime}\n'
              ' $<TARGET_FILE_DIR:draw_thread_failure_test>)\nendif()\n')
    (out / 'CMakeLists.txt').write_text(cmake, encoding='utf-8')
    print(f'Production source SHA256 {hashlib.sha256(raw_source).hexdigest()}', flush=True)
    configure = [args.cmake, '-S', str(out), '-B', str(out / 'build')]
    if args.sdl_dir:
        configure.append(f'-DSDL2_DIR={args.sdl_dir.resolve().as_posix()}')
    subprocess.run(configure, check=True, timeout=90)
    subprocess.run([args.cmake, '--build', str(out / 'build'), '--config', 'Release'],
                   check=True, timeout=180)
    binary = next(path for path in (out / 'build/Release/draw_thread_failure_test.exe',
                                   out / 'build/draw_thread_failure_test') if path.is_file())
    return subprocess.run([str(binary)], timeout=20).returncode


if __name__ == '__main__':
    raise SystemExit(main())
