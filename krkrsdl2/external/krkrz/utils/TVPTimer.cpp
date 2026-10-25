
#include "tjsCommHead.h"
#include "TVPTimer.h"
#ifdef __SWITCH__
#include <SDL.h>
#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

struct TVPTimer::State {
	std::shared_ptr<TVPTimerEventIntarface> handler;
	tjs_uint64 interval = 1000, due = 0, generation = 0;
	bool alive = true, enabled = true, firing = false;
};

struct TVPTimer::Scheduler {
	std::vector<std::weak_ptr<State>> timers;
	static tjs_uint64 NextTick(tjs_uint64 now, tjs_uint64 interval) {
		const auto maximum = (std::numeric_limits<tjs_uint64>::max)();
		return interval > maximum - now ? maximum : now + interval;
	}
};

TVPTimer::Scheduler& TVPTimer::GetScheduler() {
	// Native objects can be finalized after engine restart. Keep the small
	// registry alive; weak entries never retain an engine timer or callback.
	static auto* const scheduler = new Scheduler;
	return *scheduler;
}

TVPTimer::TVPTimer() : state_(std::make_shared<State>()) {
	auto& timers = GetScheduler().timers;
	timers.erase(std::remove_if(timers.begin(), timers.end(),
		[](const std::weak_ptr<State>& timer) { return timer.expired(); }), timers.end());
	timers.push_back(state_);
}

TVPTimer::~TVPTimer() noexcept(false) {
	// A dispatch snapshot may retain this state, but it must never touch the
	// destroyed timer or invoke its old callback after cancellation.
	state_->alive = false;
	state_->enabled = false;
	++state_->generation;
	state_->handler.reset();
}

void TVPTimer::SetHandler(std::shared_ptr<TVPTimerEventIntarface> handler) {
	state_->handler = std::move(handler);
	++state_->generation;
	if (state_->enabled)
		state_->due = Scheduler::NextTick(SDL_GetTicks64(), state_->interval);
}

void TVPTimer::SetInterval(tjs_uint64 milliseconds) {
	if (state_->interval == milliseconds) return;
	state_->interval = milliseconds;
	++state_->generation;
	if (state_->enabled)
		state_->due = Scheduler::NextTick(SDL_GetTicks64(), milliseconds);
}

void TVPTimer::SetEnabled(bool enabled) {
	if (state_->enabled == enabled) return;
	state_->enabled = enabled;
	++state_->generation;
	if (enabled) state_->due = Scheduler::NextTick(SDL_GetTicks64(), state_->interval);
}

bool TVPTimer::GetEnabled() const { return state_->enabled; }

void TVPTimer::ProgressAllTimer() {
	// Like the reference's internal timer wheel, collect due work once per
	// frame and reschedule from this frame's clock. A long stall produces one
	// native callback, not a burst. No script-Timer mutex is held here.
	// TVPGetTickCount uses SDL's 32-bit compatibility clock in this port.
	// The same monotonic SDL epoch in 64 bits keeps deadlines valid at rollover.
	const tjs_uint64 now = SDL_GetTicks64();
	struct Pending { std::shared_ptr<State> state; tjs_uint64 generation; };
	std::vector<Pending> pending;
	auto& timers = GetScheduler().timers;
	for (auto iterator = timers.begin(); iterator != timers.end();) {
		auto state = iterator->lock();
		if (!state || !state->alive) { iterator = timers.erase(iterator); continue; }
		if (state->enabled && state->interval && state->handler && !state->firing && state->due < now)
			pending.push_back({state, state->generation});
		++iterator;
	}
	for (auto& item : pending) {
		auto& state = *item.state;
		if (!state.alive || !state.enabled || state.firing || state.generation != item.generation)
			continue;
		// Keep the handler alive through self-destruction/replacement. Nested
		// frame pumps may run other timers, but cannot reenter this callback or
		// execute a stale outer snapshot of a timer already handled inside.
		auto handler = state.handler;
		++state.generation;
		state.due = Scheduler::NextTick(now, state.interval);
		state.firing = true;
		struct FiringScope { State& state; ~FiringScope() { state.firing = false; } } scope{state};
		handler->Handle();
	}
}
#else
#include "TimerIntf.h"

TVPTimer::TVPTimer() : event_(nullptr) {
	tTVPTimerThread::Add(this);
}

TVPTimer::~TVPTimer() {
	tTVPTimerThread::Remove(this);
	ZeroPendingCount();
	if( event_ ) delete event_;
}

void TVPTimer::SetInterval(tjs_uint64 n) {
	double interval = (double)n * (1<<TVP_SUBMILLI_FRAC_BITS);
	tTVPTimerBase::SetInterval((tjs_int64)(interval + 0.5));
}
#endif
