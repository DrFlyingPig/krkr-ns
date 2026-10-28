/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace krkrns {
// Startup display only. This never pumps events or waits for asynchronous work.
class StartupConsoleState {
public:
    enum class Phase { Idle, Loading, AwaitFirstPump };
    struct Line { std::u16string text; bool important = false; };
    static constexpr std::size_t MaxLines = 18, MaxCharacters = 200;
private:
    Phase phase_ = Phase::Idle;
    std::deque<Line> lines_;
    std::uint64_t runSerial_ = 0, returnedInRun_ = 0, lastDraw_ = 0;
    unsigned runDepth_ = 0;
    bool drawing_ = false, drawn_ = false;
public:
    bool Begin() {
        if (Active() || drawing_) return false;
        lines_.clear(); phase_ = Phase::Loading; drawn_ = false; lastDraw_ = 0;
        return true;
    }
    bool Active() const noexcept { return phase_ != Phase::Idle; }
    Phase CurrentPhase() const noexcept { return phase_; }
    bool Drawing() const noexcept { return drawing_; }
    const std::deque<Line>& Lines() const noexcept { return lines_; }
    void Abort() noexcept { phase_ = Phase::Idle; lines_.clear(); drawn_ = false; }
    void Append(const char16_t* text, std::size_t length, bool important) {
        if (!Active() || !text || !length) return;
        std::size_t start = 0;
        while (start < length) {
            std::size_t end = start;
            while (end < length && text[end] != u'\r' && text[end] != u'\n') ++end;
            if (end > start) {
                auto count = (end - start < MaxCharacters) ? end - start : MaxCharacters;
                if (count && text[start + count - 1] >= 0xd800 && text[start + count - 1] <= 0xdbff) --count;
                Line line{std::u16string(text + start, count), important};
                if (lines_.size() == MaxLines) lines_.pop_front();
                lines_.push_back(std::move(line));
            }
            start = end;
            while (start < length && (text[start] == u'\r' || text[start] == u'\n')) ++start;
        }
    }
    void EntryReturned() noexcept {
        if (phase_ == Phase::Loading) { phase_ = Phase::AwaitFirstPump; returnedInRun_ = runSerial_; }
    }
    void EnterRun() noexcept { if (!runDepth_++) ++runSerial_; }
    bool LeaveRun(bool complete) noexcept {
        if (!runDepth_) return false;
        const bool outer = runDepth_ == 1;
        --runDepth_;
        if (outer && !complete) { Abort(); return false; }
        if (outer && phase_ == Phase::AwaitFirstPump && runSerial_ > returnedInRun_) {
            Abort(); return true;
        }
        return false;
    }
    bool BeginDraw(std::uint64_t now) noexcept {
        if (!Active() || drawing_ || (drawn_ && now - lastDraw_ < 100)) return false;
        drawing_ = true; drawn_ = true; lastDraw_ = now; return true;
    }
    void EndDraw() noexcept { drawing_ = false; }
};
}

#ifdef __SWITCH__
bool krkrsdl2_startup_console_begin();
void krkrsdl2_startup_console_returned() noexcept;
void krkrsdl2_startup_console_abort() noexcept;
void krkrsdl2_startup_console_log(const char16_t*, std::size_t, bool);
void krkrsdl2_startup_console_enter_run() noexcept;
void krkrsdl2_startup_console_leave_run(bool complete);
namespace krkrns {
class StartupConsoleLaunchGuard {
    bool owned_, returned_ = false;
public:
    StartupConsoleLaunchGuard() : owned_(krkrsdl2_startup_console_begin()) {}
    ~StartupConsoleLaunchGuard() { if (owned_ && !returned_) krkrsdl2_startup_console_abort(); }
    void Returned() noexcept { if (owned_) { krkrsdl2_startup_console_returned(); returned_ = true; } }
};
class StartupConsoleRunGuard {
    bool finished_ = false;
public:
    StartupConsoleRunGuard() { krkrsdl2_startup_console_enter_run(); }
    ~StartupConsoleRunGuard() { if (!finished_) krkrsdl2_startup_console_leave_run(false); }
    void Complete() { finished_ = true; krkrsdl2_startup_console_leave_run(true); }
};
}
#endif
