
#ifndef __GRAPHICS_LOAD_THREAD_H__
#define __GRAPHICS_LOAD_THREAD_H__

#include <queue>
#include <vector>
#include <deque>
#include <memory>
#include <atomic>
#include <set>
#include "ThreadIntf.h"
#include "NativeEventQueue.h"
#include "GraphicsLoaderIntf.h"

// Private decoded buffers are bounded across every pending touch batch. They
// never own VM objects or enter the engine bitmap allocator on the worker.
struct tTVPAsyncTouchMemory {
	std::atomic<tjs_uint64> Pending{0}, Peak{0};
};
struct tTVPAsyncTouchBatch {
	tjs_uint64 Id = 0, Requested = 0, Prepared = 0, CacheHits = 0, Duplicates = 0, Rejected = 0;
	tjs_uint64 Limit = 0, Started = 0, Timeout = 0, Generation = 0;
	bool PreparationComplete = false;
	std::atomic<tjs_uint64> Outstanding{0}, Bytes{0}, Decoded{0}, Cached{0};
	std::atomic<tjs_uint64> BudgetSkipped{0}, TimedOut{0}, Cancelled{0}, Errors{0}, CapacityWaits{0};
};

// BaseBitmap を使うとリエントラントではないので、別の構造体に独自にロードする必要がある
struct tTVPTmpBitmapImage {
	tjs_uint32 w;
	tjs_uint32 h;
	tjs_int pitch;
	tjs_uint32* buf;
	std::vector<tTVPGraphicMetaInfoPair> * MetaInfo;
	bool PrivateBuffer;
	tjs_uint64 MaxBytes;
	std::shared_ptr<tTVPAsyncTouchMemory> TouchMemory;
	tjs_uint64 PendingLimit = 0, ReservedBytes = 0;
	void ReleaseTouchBytes() noexcept;
	tTVPTmpBitmapImage();
	~tTVPTmpBitmapImage();
// パレット関連は現状読まない、ファイルに従うのではなく、事前指定方式なので
};

struct tTVPImageLoadCommand {
	iTJSDispatch2*			owner_;	// send to event
	class tTJSNI_Bitmap*	bmp_;	// set bitmap image
	ttstr					path_;
	ttstr                    load_path_;
	tTVPTmpBitmapImage*		dest_;
	ttstr					result_;
	std::unique_ptr<tTVPGraphicHandlerType> handler_;
	bool touch_;
	bool touch_cancelled_ = false;
	tjs_uint64 touch_generation_;
	std::shared_ptr<tTVPAsyncTouchBatch> touch_batch_;
	tTVPImageLoadCommand();
	~tTVPImageLoadCommand();
};

class tTVPAsyncImageLoader : public tTVPThread {
	/** 読込み要求コマンドのキュー用CS */
	tTJSCriticalSection CommandQueueCS;
	/** 読込み済み画像キュー用CS */
	tTJSCriticalSection ImageQueueCS;

	/** ロード完了後メインスレッドで処理するためのメッセージキュー */
	NativeEventQueue<tTVPAsyncImageLoader> EventQueue;
	/**  読込みスレッドへ読込み要求があったことを伝えるイベント */
	tTVPThreadEvent PushCommandQueueEvent;

	/** 読込み要求コマンドキュー */
	std::queue<tTVPImageLoadCommand*> CommandQueue;
	std::deque<tTVPImageLoadCommand*> TouchQueue;
	tTVPImageLoadCommand* DeferredTouch = nullptr;
	/** 読込み完了画像キュー */
	std::queue<tTVPImageLoadCommand*> LoadedQueue;
	std::atomic<bool> Stopping;
	// Main-thread preparation/cache publication; the worker only uses immutable
	// prepared handlers and atomic counters. Ordinary Bitmap requests have priority.
	std::deque<std::shared_ptr<tTVPAsyncTouchBatch>> TouchBatches;
	std::set<std::pair<tjs_uint64, ttstr>> TouchNames;
	std::shared_ptr<tTVPAsyncTouchMemory> TouchMemory;
	tjs_uint64 TouchSequence = 0, TouchRequested = 0, TouchPrepared = 0;
	tjs_uint64 TouchDecoded = 0, TouchCached = 0;

private:
	/**
	 * 読込みスレッドからメインスレッドへ読込みが完了したことを通知する
	 */
	void SendToLoadFinish();
	/**
	 * 読込み完了した画像をメインスレッドでBitmapへ格納して、イベント通知する
	 */
	void HandleLoadedImage();

	/**
	 * 読込みを読込みスレッドに要求する(キューへ入れる)
	 */
	bool PushLoadQueue(iTJSDispatch2 *owner, tTJSNI_Bitmap *bmp,
		const ttstr &nname, const ttstr &loadname, tTVPGraphicHandlerType *handler,
		bool touch = false, tjs_uint64 generation = 0, tjs_uint64 maxbytes = 0,
		const std::shared_ptr<tTVPAsyncTouchBatch>& batch = {});
	void ReportTouchBatches();
	void ResetTouchImage(tTVPImageLoadCommand *cmd) noexcept;
	void HandleTouchedImage(tTVPImageLoadCommand *cmd);
	
	/**
	 * 読込みスレッド実体
	 * キューにコマンドが入るのを待ち、イベントが来たらキューからコマンドを取り出して読込み処理を実行
	 * 読込みが完了したら読込み済み画像キューに入れてメインスレッドへ完了を通知する
	 */
	void LoadingThread();
	
	/**
	 * 画像読込み処理
	 */
	// false means retry after private-buffer capacity becomes available.
	bool LoadImageFromCommand( tTVPImageLoadCommand* cmd );
	
	/**
	 * 読込みスレッドメイン
	 */
	void Execute();

	/**
	 * メインスレッドハンドラ
	 * メインスレッドへのイベント(メッセージ)通知を受ける
	 */
	void Proc( NativeEvent& ev );

public:
	tTVPAsyncImageLoader();
	~tTVPAsyncImageLoader();

	/**
	 読込みスレッドの終了を要求する(終了は待たない)
	 */
	void ExitRequest();
	
	/**
	 * 読込み要求
	 * メインスレッドから読込みスレッドへ読込みを要求する。
	 * 読込み前にエラーが発生した場合やキャッシュ上に画像があった場合は要求は行われず
	 * 即座に終了し、onLoaded イベントを発生させる。
	 */
	void LoadRequest( iTJSDispatch2 *owner, tTJSNI_Bitmap* bmp, const ttstr &name );
	void TouchImages(const std::vector<ttstr> &names, tjs_uint64 limit,
		tjs_uint64 timeout);
	// Cache clear/limit changes invalidate results without touching VM objects
	// or depending on an Application object still being alive at process exit.
	static void InvalidateTouchRequests();
};

// Resolves handlers and companion images on the main thread, never in the
// decoder. Cache-only loads must not cache an image lacking its mask/province.
bool TVPResolveGraphicForAsyncTouch(const ttstr &name, ttstr &loadname,
	tTVPGraphicHandlerType *&handler);

#endif // __GRAPHICS_LOAD_THREAD_H__
