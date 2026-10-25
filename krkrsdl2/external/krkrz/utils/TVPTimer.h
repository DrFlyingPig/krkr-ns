
#ifndef __TVP_TIMER_H__
#define __TVP_TIMER_H__

#ifdef __SWITCH__
#include <memory>
#else
#include "TimerThread.h"
#endif

class TVPTimerEventIntarface {
public:
	virtual void Handle() = 0;
};

template<typename T>
class TVPTimerEvent : public TVPTimerEventIntarface {
	void (T::*handler_)();
	T* owner_;

public:
	TVPTimerEvent( T* owner, void (T::*Handler)() ) : owner_(owner), handler_(Handler) {}
	void Handle() override { (owner_->*handler_)(); }
};

#ifdef __SWITCH__
// Internal engine timers follow the reference's main-frame progression. Script
// Timer objects retain tTVPTimerBase and its independent pending-event thread.
// Construction, mutation, destruction and ProgressAllTimer run on the main thread.
class TVPTimer {
	struct State;
	struct Scheduler;
	std::shared_ptr<State> state_;
	static Scheduler& GetScheduler();
	void SetHandler(std::shared_ptr<TVPTimerEventIntarface> handler);
public:
	TVPTimer();
	virtual ~TVPTimer() noexcept(false);
	TVPTimer(const TVPTimer&) = delete;
	TVPTimer& operator=(const TVPTimer&) = delete;
	template<typename T>
	void SetOnTimerHandler(T* owner, void (T::*handler)()) {
		SetHandler(std::make_shared<TVPTimerEvent<T>>(owner, handler));
	}
	virtual void SetInterval(tjs_uint64 milliseconds);
	void SetEnabled(bool enabled);
	bool GetEnabled() const;
	bool GetEnable() const { return GetEnabled(); }
	static void ProgressAllTimer();
};
#else
class TVPTimer : public tTVPTimerBase {
	TVPTimerEventIntarface* event_;

protected:
	// タイマー処理実態。メインスレッドで呼ばれる。
	virtual void Fire(tjs_uint n) override {
		if( event_ ) {
			event_->Handle();
		}
	}

	// イベントをキャンセルする。SetEnabled/SetIntervalをコールした時内部で呼ばれる。
	virtual void CancelEvents() override {}

private:
	// 隠ぺいする
	// Interval値は16ビットシフトされたものになっている
	tjs_uint64 GetInterval() const override {
		return tTVPTimerBase::GetInterval();
	}
public:
	TVPTimer();
	virtual ~TVPTimer();

	template<typename T>
	void SetOnTimerHandler( T* owner, void (T::*Handler)() ) {
		if( event_ ) delete event_;
		event_ = new TVPTimerEvent<T>( owner, Handler );
	}

	void SetInterval(tjs_uint64 n) override;
};
#endif


#endif // __TVP_TIMER_H__
