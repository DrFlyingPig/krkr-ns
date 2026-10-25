
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

static std::atomic<tjs_uint64> TVPAsyncTouchGeneration(0);

tTVPTmpBitmapImage::tTVPTmpBitmapImage()
	: w(0), h(0), pitch(0), buf(NULL), MetaInfo(NULL), PrivateBuffer(false),
	  MaxBytes((std::numeric_limits<tjs_uint64>::max)())
{}
tTVPTmpBitmapImage::~tTVPTmpBitmapImage() {
	if(buf) {
		if (PrivateBuffer) std::free(buf);
		else tTVPBitmapBitsAlloc::Free(buf);
		buf = NULL;
	}
	if( MetaInfo ) {
		delete MetaInfo;
		MetaInfo = NULL;
	}
}
tTVPImageLoadCommand::tTVPImageLoadCommand()
	: owner_(NULL), bmp_(NULL), dest_(NULL), touch_(false), touch_generation_(0) {}
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
		size > (std::numeric_limits<tjs_uint>::max)() || size > img->MaxBytes)
		throw std::bad_alloc();
	if (img->buf) {
		if (img->PrivateBuffer) std::free(img->buf);
		else tTVPBitmapBitsAlloc::Free(img->buf);
		img->buf = NULL;
	}
	img->h = h;
	img->w = w;
	BitmapInfomation info( w, h, 32 );
	if (img->PrivateBuffer) {
		// BitmapBitsAlloc retries through compact/GC and initializes a compact
		// hook. Cache-only decoding must never run either operation here.
		img->buf = static_cast<tjs_uint32*>(std::malloc(info.GetImageSize()));
		if (!img->buf) throw std::bad_alloc();
	} else {
		img->buf = (tjs_uint32*)tTVPBitmapBitsAlloc::Alloc(info.GetImageSize(), w, h);
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
: EventQueue(this,&tTVPAsyncImageLoader::Proc), Stopping(false), TouchLoading(false)
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
		delete cmd;
	}
	while( LoadedQueue.size() > 0 ) {
		tTVPImageLoadCommand* cmd = LoadedQueue.front();
		LoadedQueue.pop();
		delete cmd;
	}
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
	HandleLoadedImage();
}
void tTVPAsyncImageLoader::HandleLoadedImage() {
	bool loading;
	do {
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
				TouchLoading = false;
				// A preload is best effort. Even an allocation/cache failure must
				// release its buffer and allow the next request to progress.
				try { HandleTouchedImage(cmd); } catch (...) {}
				command.reset();
				QueueNextTouch();
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
	if (Stopping.load(std::memory_order_acquire)) return;
	//tTVPBaseBitmap* dest = new tTVPBaseBitmap( 32, 32, 32 );
	tTVPBaseBitmap dest( TVPGetInitialBitmap() );
	iTJSDispatch2* metainfo = NULL;
	ttstr nname = TVPNormalizeStorageName(name);
	if( TVPCheckImageCache(nname,&dest,glmNormal,0,0,TVP_clNone,&metainfo) ) {
		// キャッシュ内に発見、即座に読込みを完了する
		bmp->CopyFrom( &dest );
		bmp->SetLoading( false );
		if (!owner) {
			if (metainfo) metainfo->Release();
			return;
		}

		tTJSVariant param[4];
		param[0] = tTJSVariant(metainfo,metainfo);
		if( metainfo ) metainfo->Release();
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
	if (!handler) TVPThrowExceptionMessage(TVPUnknownGraphicFormat, nname);
	PushLoadQueue(owner, bmp, nname, nname, handler);
}

// tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);
//	tTJSBinaryStream* stream = TVPCreateStream(nname, TJS_BS_READ);
// TVPCreateStream はロックされているので、非同期で実行可能
bool tTVPAsyncImageLoader::PushLoadQueue(iTJSDispatch2 *owner, tTJSNI_Bitmap *bmp,
	const ttstr &nname, const ttstr &loadname, tTVPGraphicHandlerType *handler,
	bool touch, tjs_uint64 generation, tjs_uint64 maxbytes) {
	std::unique_ptr<tTVPImageLoadCommand> cmd(new tTVPImageLoadCommand());
	cmd->owner_ = owner;
	if (owner) owner->AddRef();
	cmd->bmp_ = bmp;
	cmd->path_ = nname;
	cmd->load_path_ = loadname;
	cmd->dest_ = new tTVPTmpBitmapImage();
	cmd->handler_.reset(new tTVPGraphicHandlerType(*handler));
	cmd->touch_ = touch;
	cmd->touch_generation_ = generation;
	if (touch) {
		cmd->dest_->PrivateBuffer = true;
		cmd->dest_->MaxBytes = maxbytes;
	}
	cmd->result_.Clear();
	{
		// キューをロックしてプッシュ
		tTJSCriticalSectionHolder cs(CommandQueueCS);
		if (Stopping.load(std::memory_order_acquire)) return false;
		CommandQueue.push(cmd.get());
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
				KRKRNS_LOG("[imgload] begin %s", cmd->path_.AsNarrowStdString().c_str());
				LoadImageFromCommand(cmd);
				KRKRNS_LOG("[imgload] done %s%s", cmd->path_.AsNarrowStdString().c_str(),
					cmd->result_.length() ? " (error)" : "");
				{	// Lock
					tTJSCriticalSectionHolder cs(ImageQueueCS);
					LoadedQueue.push(cmd);
				}
				// Send to message
				SendToLoadFinish();
			}
		} while (loading && !Stopping.load(std::memory_order_acquire));
	}
}
void tTVPAsyncImageLoader::LoadImageFromCommand( tTVPImageLoadCommand* cmd ) {
	try {
		if (cmd->touch_ && cmd->touch_generation_ !=
			TVPAsyncTouchGeneration.load(std::memory_order_acquire)) return;
		tTVPStreamHolder holder(cmd->load_path_);
		(cmd->handler_->Load)(cmd->handler_->FormatData, (void*)cmd->dest_, TVPLoadGraphicAsync_SizeCallback,
			TVPLoadGraphicAsync_ScanLineCallback, TVPLoadGraphicAsync_MetaInfoPushCallback,
			holder.Get(), -1, glmNormal );
	} catch(...) {
		// 例外は全てキャッチ
		cmd->result_ = TVPImageLoadError;
	}
}

void tTVPAsyncImageLoader::InvalidateTouchRequests() {
	TVPAsyncTouchGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void tTVPAsyncImageLoader::TouchImages(const std::vector<ttstr> &names,
	tjs_uint64 limit, tjs_uint64 timeout) {
	if (names.empty() || !limit || Stopping.load(std::memory_order_acquire)) return;
	tTouchBatch batch;
	batch.Names = names;
	batch.Limit = limit;
	batch.Started = TVPGetTickCount();
	batch.Timeout = timeout;
	batch.Generation = TVPAsyncTouchGeneration.load(std::memory_order_acquire);
	TouchBatches.push_back(std::move(batch));
	QueueNextTouch();
}

void tTVPAsyncImageLoader::QueueNextTouch() {
	if (TouchLoading || Stopping.load(std::memory_order_acquire)) return;
	while (!TouchBatches.empty()) {
		auto &batch = TouchBatches.front();
		if (batch.Generation != TVPAsyncTouchGeneration.load(std::memory_order_acquire) ||
			batch.Bytes >= batch.Limit || batch.Next >= batch.Names.size() ||
			(batch.Timeout && TVPGetTickCount() - batch.Started >= batch.Timeout)) {
			TouchBatches.pop_front();
			continue;
		}
		const ttstr name = batch.Names[batch.Next++];
		try {
			const ttstr nname = TVPNormalizeStorageName(name);
			if (TVPHasImageCache(nname, glmNormal, 0, 0, TVP_clNone)) continue;
			ttstr loadname;
			tTVPGraphicHandlerType *handler = NULL;
			if (!TVPResolveGraphicForAsyncTouch(nname, loadname, handler)) continue;
			TouchLoading = PushLoadQueue(NULL, NULL, nname, loadname, handler,
				true, batch.Generation, batch.Limit - batch.Bytes);
			if (TouchLoading) return;
		} catch (...) {
			// Match touchImages' best-effort handling of an invalid storage.
		}
	}
}

void tTVPAsyncImageLoader::HandleTouchedImage(tTVPImageLoadCommand *cmd) {
	if (Stopping.load(std::memory_order_acquire) || cmd->result_.length() ||
		cmd->touch_generation_ != TVPAsyncTouchGeneration.load(std::memory_order_acquire) ||
		TouchBatches.empty() || !cmd->dest_->buf) return;
	auto &batch = TouchBatches.front();
	if (batch.Generation != cmd->touch_generation_) return;
	auto *image = cmd->dest_;
	const tjs_uint size = static_cast<tjs_uint>(image->pitch) * image->h;
	batch.Bytes += size;
	if (TVPHasImageCache(cmd->path_, glmNormal, 0, 0, TVP_clNone)) return;

	// Main-thread allocation owns the engine bitmap. Its OOM compact callback
	// may cancel this very batch; check the generation again before caching.
	tTVPBaseBitmap bitmap(TVPGetInitialBitmap());
	std::unique_ptr<void, void (*)(void*)> bits(
		tTVPBitmapBitsAlloc::Alloc(size, image->w, image->h), &tTVPBitmapBitsAlloc::Free);
	std::memcpy(bits.get(), image->buf, size);
	bitmap.SetSizeAndImageBuffer(image->w, image->h, bits.get());
	bits.release();
	if (cmd->touch_generation_ != TVPAsyncTouchGeneration.load(std::memory_order_acquire)) return;
	auto *pairs = image->MetaInfo;
	image->MetaInfo = NULL; // PushGraphicCache consumes this even on failure.
	TVPPushGraphicCache(cmd->path_, &bitmap, pairs);
}

