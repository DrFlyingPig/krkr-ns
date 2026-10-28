
#include "tjsCommHead.h"
#include "KrkrNSLog.h"

#include "BitmapIntf.h"
#include "GraphicsLoadThread.h"
#include "ThreadIntf.h"
#include "NativeEventQueue.h"
#include "UserEvent.h"
#include "EventIntf.h"
#include "StorageIntf.h"
#include "LayerBitmapIntf.h"
#include "MsgIntf.h"
#include "UtilStreams.h"
#include "BitmapBitsAlloc.h"
#include "LayerIntf.h"
#include "TickCount.h"
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

tTVPTmpBitmapImage::tTVPTmpBitmapImage()
	: w(0), h(0), pitch(0), buf(NULL), MetaInfo(NULL)
{}
void tTVPTmpBitmapImage::ReleaseTouchBytes() noexcept {
	if (ReservedBytes) {
		TouchMemory->Pending.fetch_sub(ReservedBytes, std::memory_order_acq_rel);
		ReservedBytes = 0;
	}
}
tTVPTmpBitmapImage::~tTVPTmpBitmapImage() {
	if(buf) {
		tTVPBitmapBitsAlloc::Free(buf);
		buf = NULL;
	}
	ReleaseTouchBytes();
	if( MetaInfo ) {
		delete MetaInfo;
		MetaInfo = NULL;
	}
}
tTVPImageLoadCommand::tTVPImageLoadCommand()
	: owner_(NULL), bmp_(NULL), dest_(NULL), touch_(false) {}
tTVPImageLoadCommand::~tTVPImageLoadCommand() {
	if( owner_ ) {
		owner_->Release();
		owner_ = NULL;
	}
	if( dest_ ) {
		delete dest_;
		dest_ = NULL;
	}
	bmp_ = NULL;
}

static void TVPLoadGraphicAsync_SizeCallback(void *callbackdata, tjs_uint w, tjs_uint h)
{
	tTVPTmpBitmapImage* img = (tTVPTmpBitmapImage*)callbackdata;
	const tjs_uint64 pitch = ((static_cast<tjs_uint64>(w) + 3) & ~tjs_uint64(3)) * 4;
	const tjs_uint64 size = pitch * h;
	if (!w || !h || pitch > (std::numeric_limits<tjs_int>::max)() ||
		size > (std::numeric_limits<tjs_uint>::max)())
		throw std::bad_alloc();
	if (img->buf) {
		tTVPBitmapBitsAlloc::Free(img->buf);
		img->buf = NULL;
	}
	img->ReleaseTouchBytes();
	img->h = h;
	img->w = w;
	BitmapInfomation info( w, h, 32 );
	// Like the reference bitmap, all decoded images use engine-owned pixels.
	// The worker must not run the ordinary allocator's VM/cache/GPU compaction.
	img->buf = (tjs_uint32*)tTVPBitmapBitsAlloc::AllocNoCompact(info.GetImageSize(), w, h);
	if (!img->buf) throw std::bad_alloc();
	if (img->TouchMemory) {
		const auto pending = img->TouchMemory->Pending.fetch_add(size, std::memory_order_acq_rel) + size;
		img->ReservedBytes = size;
		auto peak = img->TouchMemory->Peak.load(std::memory_order_relaxed);
		while (peak < pending && !img->TouchMemory->Peak.compare_exchange_weak(
			peak, pending, std::memory_order_relaxed)) {}
	}
	img->pitch = info.GetPitchBytes();
}
//---------------------------------------------------------------------------
static void* TVPLoadGraphicAsync_ScanLineCallback(void *callbackdata, tjs_int y)
{
	tTVPTmpBitmapImage* img = (tTVPTmpBitmapImage*)callbackdata;
	if( y >= 0 ) {
		if( y < (tjs_int)img->h ) {
			return (img->h - y -1 ) * img->pitch + (tjs_uint8*)img->buf;
		} else {
			return NULL;
		}
	}
	return NULL; // -1 の時のフラッシュ処理は何もしない
}
//---------------------------------------------------------------------------
static void TVPLoadGraphicAsync_MetaInfoPushCallback(void *callbackdata, const ttstr & name, const ttstr & value)
{
	tTVPTmpBitmapImage * img = (tTVPTmpBitmapImage *)callbackdata;

	if(!img->MetaInfo) img->MetaInfo = new std::vector<tTVPGraphicMetaInfoPair>();
	img->MetaInfo->push_back(tTVPGraphicMetaInfoPair(name, value));
}
//---------------------------------------------------------------------------

tTVPAsyncImageLoader::tTVPAsyncImageLoader()
: EventQueue(this,&tTVPAsyncImageLoader::Proc), Stopping(false),
  TouchMemory(std::make_shared<tTVPAsyncTouchMemory>())
{
	EventQueue.Allocate();
}
tTVPAsyncImageLoader::~tTVPAsyncImageLoader() {
	ExitRequest();
	WaitFor();
	EventQueue.Deallocate();
	while( CommandQueue.size() > 0 ) {
		tTVPImageLoadCommand* cmd = CommandQueue.front();
		CommandQueue.pop();
		if (cmd->touch_batch_) {
			if (!cmd->touch_cancelled_) ++cmd->touch_batch_->Cancelled;
			--cmd->touch_batch_->Outstanding;
		}
		delete cmd;
	}
	while( LoadedQueue.size() > 0 ) {
		tTVPImageLoadCommand* cmd = LoadedQueue.front();
		LoadedQueue.pop();
		if (cmd->touch_batch_) {
			if (!cmd->touch_cancelled_) ++cmd->touch_batch_->Cancelled;
			--cmd->touch_batch_->Outstanding;
		}
		delete cmd;
	}
	ReportTouchBatches();
	KRKRNS_LOG("[touch] summary batches=%llu requested=%llu prepared=%llu decoded=%llu cached=%llu pending=%llu peak=%llu",
		static_cast<unsigned long long>(TouchSequence), static_cast<unsigned long long>(TouchRequested),
		static_cast<unsigned long long>(TouchPrepared), static_cast<unsigned long long>(TouchDecoded),
		static_cast<unsigned long long>(TouchCached), static_cast<unsigned long long>(TouchMemory->Pending.load()),
		static_cast<unsigned long long>(TouchMemory->Peak.load()));
}
void tTVPAsyncImageLoader::ExitRequest() {
	Stopping.store(true, std::memory_order_release);
	Terminate();
	PushCommandQueueEvent.Set();
}
void tTVPAsyncImageLoader::Execute() {
	// プライオリティは最低にする
	SetPriority(ttpIdle);
	LoadingThread();
}
void tTVPAsyncImageLoader::SendToLoadFinish() {
	NativeEvent ev(TVP_EV_IMAGE_LOAD_THREAD);
	EventQueue.PostEvent(ev);
}
void tTVPAsyncImageLoader::Proc( NativeEvent& ev )
{
	if(ev.Message != TVP_EV_IMAGE_LOAD_THREAD) {
		EventQueue.HandlerDefault(ev);
		return;
	}
	try {
		HandleLoadedImage();
	} catch (...) {
		bool pending;
		{
			tTJSCriticalSectionHolder cs(ImageQueueCS);
			pending = !LoadedQueue.empty();
		}
		// Touch notifications are coalesced. A throwing ordinary callback must
		// not strand results already behind it; retry through the next main pump.
		if (pending && !Stopping.load(std::memory_order_acquire)) {
			try { SendToLoadFinish(); }
			catch (...) { /* Keep the original dispatch exception if posting also fails. */ }
		}
		throw;
	}
}
void tTVPAsyncImageLoader::HandleLoadedImage() {
	bool loading;
	do {
		// Termination may be requested by the current onLoaded callback. Keep
		// remaining commands owned until the outer pump stops/joins this loader.
		if (Stopping.load(std::memory_order_acquire)) break;
		loading = false;
		tTVPImageLoadCommand* cmd = NULL;
		{
			tTJSCriticalSectionHolder cs(ImageQueueCS);
			if( LoadedQueue.size() > 0 ) {
				cmd = LoadedQueue.front();
				LoadedQueue.pop();
				loading = true;
			}
		}
		if( cmd != NULL ) {
			std::unique_ptr<tTVPImageLoadCommand> command(cmd);
			if (cmd->touch_) {
				// A preload is best effort. Even an allocation/cache failure must
				// release its buffer and allow the next request to progress.
				try { HandleTouchedImage(cmd); } catch (...) { ++cmd->touch_batch_->Errors; }
				--cmd->touch_batch_->Outstanding;
				command.reset();
				ReportTouchBatches();
				continue;
			}
			if (cmd->bmp_) cmd->bmp_->SetLoading(false);
			KRKRNS_LOG("[imgload] dispatch %s%s", cmd->path_.AsNarrowStdString().c_str(),
				cmd->result_.length() ? " (error)" : "");
			if( cmd->result_.length() > 0 ) {
				// error
				tTJSVariant param[4];
				param[0] = tTJSVariant((iTJSDispatch2*)NULL,(iTJSDispatch2*)NULL);
				param[1] = 1; // true async
				param[2] = 1; // true error
				param[3] = TVPFormatMessage(cmd->result_.c_str(), cmd->path_); // error_mes
				static ttstr eventname(TJS_W("onLoaded"));
				if( cmd->owner_ && cmd->owner_->IsValid(0,NULL,NULL,cmd->owner_) == TJS_S_TRUE ) {
					TVPPostEvent(cmd->owner_, cmd->owner_, eventname, 0, TVP_EPT_IMMEDIATE, 4, param);
				}

				if( cmd->dest_->MetaInfo ) {
					delete cmd->dest_->MetaInfo;
					cmd->dest_->MetaInfo = NULL;
				}
			} else {
				iTJSDispatch2* metainfo = TVPMetaInfoPairsToDictionary(cmd->dest_->MetaInfo);
				tTJSVariant param[4];
				param[0] = tTJSVariant(metainfo,metainfo);
				if (metainfo) metainfo->Release();

				cmd->bmp_->SetSizeAndImageBuffer( cmd->dest_->w, cmd->dest_->h, cmd->dest_->buf );
				cmd->dest_->buf = NULL;
				// 読込み完了時にもキャッシュチェック(非同期なので完了前に読み込まれている可能性あり)
				if( TVPHasImageCache( cmd->path_, glmNormal, 0, 0, TVP_clNone ) == false ) {
					auto *pairs = cmd->dest_->MetaInfo;
					cmd->dest_->MetaInfo = NULL;
					TVPPushGraphicCache(cmd->path_, cmd->bmp_->GetBitmap(), pairs);
				} else {
					delete cmd->dest_->MetaInfo;
					cmd->dest_->MetaInfo = NULL;
				}

				param[1] = 1; // true async
				param[2] = 0; // false error
				param[3] = TJS_W(""); // error_mes
				static ttstr eventname(TJS_W("onLoaded"));
				if( cmd->owner_ && cmd->owner_->IsValid(0,NULL,NULL,cmd->owner_) == TJS_S_TRUE ) {
					TVPPostEvent(cmd->owner_, cmd->owner_, eventname, 0, TVP_EPT_IMMEDIATE, 4, param);
				}
			}
		}
	} while(loading);
}
//---------------------------------------------------------------------------

// onLoaded( dic, is_async, is_error, error_mes ); エラーは
// sync ( main thead )
void tTVPAsyncImageLoader::LoadRequest( iTJSDispatch2 *owner, tTJSNI_Bitmap* bmp, const ttstr &name ) {
	if (Stopping.load(std::memory_order_acquire)) {
		if (bmp) bmp->SetLoading(false);
		return;
	}
	//tTVPBaseBitmap* dest = new tTVPBaseBitmap( 32, 32, 32 );
	tTVPBaseBitmap dest( TVPGetInitialBitmap() );
	iTJSDispatch2* metainfo = NULL;
	ttstr nname = TVPNormalizeStorageName(name);
	if( TVPCheckImageCache(nname,&dest,glmNormal,0,0,TVP_clNone,&metainfo) ) {
		// CopyFrom and the immediate callback can throw. Retain the returned
		// dictionary before either operation, including the ownerless path.
		tTJSVariant metadata(metainfo, metainfo);
		if (metainfo) metainfo->Release();
		// キャッシュ内に発見、即座に読込みを完了する
		bmp->CopyFrom( &dest );
		bmp->SetLoading( false );
		if (!owner) {
			return;
		}

		tTJSVariant param[4];
		param[0] = metadata;
		param[1] = 0; // false
		param[2] = 0; // false
		param[3] = TJS_W(""); // error_mes
		static ttstr eventname(TJS_W("onLoaded"));
		TVPPostEvent(owner, owner, eventname, 0, TVP_EPT_IMMEDIATE, 4, param);
		return;
	}
	if( TVPIsExistentStorage(name) == false ) {
		TVPThrowExceptionMessage(TVPCannotFindStorage, name);
	}
	ttstr ext = TVPExtractStorageExt(name);
	if(ext == TJS_W("")) {
		TVPThrowExceptionMessage(TJS_W("Filename extension not found/%1"), name);
	}

	tTVPGraphicHandlerType *handler = TVPGetGraphicLoadHandler(ext);
	// Like the reference, an existing resource with an unknown extension is an
	// asynchronous onLoaded error; it is not a synchronous validation failure.
	if (!PushLoadQueue(owner, bmp, nname, nname, handler) && bmp) bmp->SetLoading(false);
}

// tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);
//	tTJSBinaryStream* stream = TVPCreateStream(nname, TJS_BS_READ);
// TVPCreateStream はロックされているので、非同期で実行可能
bool tTVPAsyncImageLoader::PushLoadQueue(iTJSDispatch2 *owner, tTJSNI_Bitmap *bmp,
	const ttstr &nname, const ttstr &loadname, tTVPGraphicHandlerType *handler,
	bool touch,
	const std::shared_ptr<tTVPAsyncTouchBatch>& batch) {
	std::unique_ptr<tTVPImageLoadCommand> cmd(new tTVPImageLoadCommand());
	cmd->owner_ = owner;
	if (owner) owner->AddRef();
	cmd->bmp_ = bmp;
	cmd->path_ = nname;
	cmd->load_path_ = loadname;
	cmd->dest_ = new tTVPTmpBitmapImage();
	if (handler) cmd->handler_.reset(new tTVPGraphicHandlerType(*handler));
	cmd->touch_ = touch;
	cmd->touch_batch_ = batch;
	if (touch) {
		cmd->dest_->TouchMemory = TouchMemory;
	}
	cmd->result_.Clear();
	{
		// キューをロックしてプッシュ
		tTJSCriticalSectionHolder cs(CommandQueueCS);
		if (Stopping.load(std::memory_order_acquire)) return false;
		CommandQueue.push(cmd.get());
		if (touch) ++batch->Outstanding;
		cmd.release();
	}
	// 追加したことをイベントで通知
	PushCommandQueueEvent.Set();
	return true;
}
void tTVPAsyncImageLoader::LoadingThread() {
	while (!Stopping.load(std::memory_order_acquire)) {
		// キュー追加イベント待ち
		PushCommandQueueEvent.WaitFor(0);
		if (Stopping.load(std::memory_order_acquire)) break;
		bool loading;
		do {
			loading = false;
			tTVPImageLoadCommand* cmd = NULL;

			{ // Lock
				tTJSCriticalSectionHolder cs(CommandQueueCS);
				if( CommandQueue.size() ) {
					cmd = CommandQueue.front();
					CommandQueue.pop();
				}
			}
			if( cmd ) {
				loading = true;
				if (!cmd->touch_) KRKRNS_LOG("[imgload] begin %s", cmd->path_.AsNarrowStdString().c_str());
				LoadImageFromCommand(cmd);
				if (!cmd->touch_) KRKRNS_LOG("[imgload] done %s%s", cmd->path_.AsNarrowStdString().c_str(),
					cmd->result_.length() ? " (error)" : "");
				bool notify;
				{	// Lock
					tTJSCriticalSectionHolder cs(ImageQueueCS);
					notify = LoadedQueue.empty() || !cmd->touch_;
					LoadedQueue.push(cmd);
				}
				// Send to message
				if (notify) SendToLoadFinish();
			}
		} while (loading && !Stopping.load(std::memory_order_acquire));
	}
}
void tTVPAsyncImageLoader::LoadImageFromCommand( tTVPImageLoadCommand* cmd ) {
	try {
		if (!cmd->handler_) {
			cmd->result_ = TVPUnknownGraphicFormat;
			if (cmd->touch_batch_) ++cmd->touch_batch_->Errors;
			return;
		}
		if (cmd->touch_) {
			if (Stopping.load(std::memory_order_acquire)) {
				++cmd->touch_batch_->Cancelled; cmd->touch_cancelled_ = true;
				ResetTouchImage(cmd); return;
			}
		}
		tTVPStreamHolder holder(cmd->load_path_);
		(cmd->handler_->Load)(cmd->handler_->FormatData, (void*)cmd->dest_, TVPLoadGraphicAsync_SizeCallback,
			TVPLoadGraphicAsync_ScanLineCallback, TVPLoadGraphicAsync_MetaInfoPushCallback,
			holder.Get(), -1, glmNormal );
		if (cmd->touch_ && cmd->dest_->buf) {
			cmd->touch_batch_->Bytes.fetch_add(tjs_uint64(cmd->dest_->pitch) * cmd->dest_->h,
				std::memory_order_relaxed);
			++cmd->touch_batch_->Decoded;
		}
	} catch(...) {
		// 例外は全てキャッチ
		cmd->result_ = TVPImageLoadError;
		if (cmd->touch_) { ++cmd->touch_batch_->Errors; ResetTouchImage(cmd); }
	}
}

void tTVPAsyncImageLoader::ResetTouchImage(tTVPImageLoadCommand *cmd) noexcept {
	// Only the worker's unpublished private decoder state is discarded here.
	// Path/handler preparation and cache/VM ownership stay on the main thread.
	auto* image = cmd->dest_;
	if (image->buf) { tTVPBitmapBitsAlloc::Free(image->buf); image->buf = NULL; }
	image->ReleaseTouchBytes();
	delete image->MetaInfo; image->MetaInfo = NULL;
	image->w = image->h = 0; image->pitch = 0;
}

void tTVPAsyncImageLoader::TouchImages(const std::vector<ttstr> &names,
	tjs_uint64 limit, tjs_uint64 timeout) {
	if (names.empty() || Stopping.load(std::memory_order_acquire)) return;
	// NoCompact never initializes the allocator or registers compact hooks on
	// a worker. Initialize the shared engine bitmap on its owning main thread.
	(void)TVPGetInitialBitmap();
	auto batch = std::make_shared<tTVPAsyncTouchBatch>();
	batch->Id = ++TouchSequence; batch->Requested = names.size();
	TouchRequested += names.size();
	// The reference computes these arguments before its asynchronous branch,
	// then queues every miss without applying either to decoding.
	batch->Limit = limit; batch->Timeout = timeout;
	TouchBatches.push_back(batch);
	for (size_t index = 0; index < names.size(); ++index) {
		if (Stopping.load(std::memory_order_acquire)) {
			batch->Cancelled.fetch_add(names.size() - index); break;
		}
		try {
			const ttstr nname = TVPNormalizeStorageName(names[index]);
			if (TVPHasImageCache(nname, glmNormal, 0, 0, TVP_clNone)) { ++batch->CacheHits; continue; }
			ttstr loadname;
			tTVPGraphicHandlerType *handler = NULL;
			TVPResolveGraphicForAsyncTouch(nname, loadname, handler);
			// Missing handlers complete through the same worker error path.
			// Repeated names remain separate FIFO commands until cached.
			if (PushLoadQueue(NULL, NULL, nname, loadname, handler, true, batch))
				++batch->Prepared;
		} catch (...) {
			// Reference enqueue/preparation errors propagate synchronously.
			// Already queued commands still own this batch and finish normally.
			++batch->Rejected;
			batch->PreparationComplete = true;
			try { ReportTouchBatches(); }
			catch (...) { /* Preserve the original preparation exception. */ }
			throw;
		}
	}
	batch->PreparationComplete = true;
	KRKRNS_LOG("[touch] queued batch=%llu requested=%llu prepared=%llu hits=%llu duplicates=0 rejected=%llu limit=%llu timeout=%llu policy=reference-fifo async-params-ignored=1",
		static_cast<unsigned long long>(batch->Id), static_cast<unsigned long long>(batch->Requested),
		static_cast<unsigned long long>(batch->Prepared), static_cast<unsigned long long>(batch->CacheHits),
		static_cast<unsigned long long>(batch->Rejected),
		static_cast<unsigned long long>(batch->Limit), static_cast<unsigned long long>(batch->Timeout));
	ReportTouchBatches();
}

void tTVPAsyncImageLoader::ReportTouchBatches() {
	while (!TouchBatches.empty()) {
		const auto& batch = TouchBatches.front();
		if (!batch->PreparationComplete || batch->Outstanding.load(std::memory_order_acquire)) return;
		TouchPrepared += batch->Prepared; TouchDecoded += batch->Decoded.load(); TouchCached += batch->Cached.load();
		KRKRNS_LOG("[touch] done batch=%llu decoded=%llu cached=%llu bytes=%llu cancelled=%llu errors=%llu pending=%llu peak=%llu",
			static_cast<unsigned long long>(batch->Id), static_cast<unsigned long long>(batch->Decoded.load()),
			static_cast<unsigned long long>(batch->Cached.load()), static_cast<unsigned long long>(batch->Bytes.load()),
			static_cast<unsigned long long>(batch->Cancelled.load()), static_cast<unsigned long long>(batch->Errors.load()),
			static_cast<unsigned long long>(TouchMemory->Pending.load()),
			static_cast<unsigned long long>(TouchMemory->Peak.load()));
		TouchBatches.pop_front();
	}
}

void tTVPAsyncImageLoader::HandleTouchedImage(tTVPImageLoadCommand *cmd) {
	if (!cmd->touch_batch_) return;
	if (Stopping.load(std::memory_order_acquire)) {
		if (!cmd->touch_cancelled_) ++cmd->touch_batch_->Cancelled;
		cmd->touch_cancelled_ = true;
		return;
	}
	if (cmd->result_.length() || !cmd->dest_->buf) return;
	auto *image = cmd->dest_;
	if (TVPHasImageCache(cmd->path_, glmNormal, 0, 0, TVP_clNone)) return;

	// Main-thread allocation owns the engine bitmap. Cache clear/limit changes
	// do not cancel queued reference preloads; publication honors current limits.
	tTVPBaseBitmap bitmap(TVPGetInitialBitmap());
	bitmap.SetSizeAndImageBuffer(image->w, image->h, image->buf);
	image->buf = NULL; // The bitmap took ownership only after construction succeeded.
	image->ReleaseTouchBytes();
	auto *pairs = image->MetaInfo;
	image->MetaInfo = NULL; // PushGraphicCache consumes this even on failure.
	TVPPushGraphicCache(cmd->path_, &bitmap, pairs);
	++cmd->touch_batch_->Cached;
}

