// Native API shape adapted from krkrsdl3 AlphaMovie.cpp; LICENSE.krkrsdl3.
// Playback semantics follow the original plugin manual and KAG's frame loop.
#include "tjsCommHead.h"
#include "ncbind/ncbind.hpp"
#include "StorageIntf.h"
#include "LayerIntf.h"
#include "DebugIntf.h"
#include "AmvMovie.h"
#include <algorithm>
#include <exception>
#include <set>
#include <utility>
#include <cstring>

#define NCB_MODULE_NAME TJS_W("AlphaMovie.dll")
extern "C" void krkrsdl2_link_alphamovie_plugin() {}

namespace {
template<class F> auto Guard(F&& function) -> decltype(function()) {
    try { return function(); }
    catch (const std::exception& error) {
        TVPThrowExceptionMessage((ttstr(TJS_W("AlphaMovie: ")) + ttstr(error.what())).c_str());
        throw;
    }
}
class StorageInput final : public krkr::amv::Input {
public:
    explicit StorageInput(const ttstr& name) : stream_(TVPCreateStream(name, TJS_BS_READ)) {
        if (!stream_) TVPThrowExceptionMessage(TJS_W("Cannot open AlphaMovie storage"));
        size_ = stream_->GetSize();
    }
    uint64_t Size() const override { return size_; }
    void Read(uint64_t offset, void* dest, size_t bytes) override {
        if (offset > size_ || bytes > size_ - offset || bytes > 0xffffffffu)
            TVPThrowExceptionMessage(TJS_W("Truncated AlphaMovie storage"));
        stream_->SetPosition(offset); stream_->ReadBuffer(dest, static_cast<tjs_uint>(bytes));
    }
private:
    std::unique_ptr<tTJSBinaryStream> stream_;
    uint64_t size_;
};
class tTJSNI_AlphaMovie;
std::set<tTJSNI_AlphaMovie*> instances;
iTJSDispatch2* pluginClass = nullptr;

class tTJSNI_AlphaMovie final : public tTJSNativeInstance {
public:
    tTJSNI_AlphaMovie() { instances.insert(this); }
    ~tTJSNI_AlphaMovie() override { player_.Close(); instances.erase(this); }
    void TJS_INTF_METHOD Invalidate() override { Finalize(); }
    void Finalize() { player_.Close(); }
    void open(const ttstr& name) {
        Guard([&] { player_.Open(std::make_unique<StorageInput>(name)); });
        const auto info = player_.GetInfo();
        TVPAddLog(ttstr(TJS_W("(info) AlphaMovie opened: ")) + name + TJS_W(" frames=") +
                  ttstr(static_cast<tjs_int>(info.count)) + TJS_W(" alpha=") +
                  ttstr(static_cast<tjs_int>(info.alphaMode)));
    }
    void clear() { player_.ClearDecoded(); }
    void play() { Guard([&] { player_.Play(); }); }
    void stop() { player_.Stop(); }
    bool isPlaying() { return player_.IsPlaying(); }
    void setPosition(tjs_int x, tjs_int y) { left_=x; top_=y; }
    void setNextMovieFile(const ttstr& name) {
        Guard([&] { player_.SetNext(name.IsEmpty() ? nullptr : std::make_unique<StorageInput>(name)); });
    }
    tjs_int showNextImage(const tTJSVariant& value) {
        auto closure = value.AsObjectClosureNoAddRef();
        tTJSNI_BaseLayer* layer = nullptr;
        if (!closure.Object || TJS_FAILED(closure.Object->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
                tTJSNC_Layer::ClassID, reinterpret_cast<iTJSNativeInstance**>(&layer))) || !layer)
            TVPThrowExceptionMessage(TJS_W("AlphaMovie.showNextImage requires a Layer"));
        return Guard([&]() -> tjs_int {
            auto frame = player_.NextImage();
            if (!frame) return -1;
            // Empty FRAMs advance playback without changing the destination.
            // Verified against the original DLL, including seek after a frame.
            if (!frame->width) return static_cast<tjs_int>(frame->index);
            const auto w = frame->width, h = frame->height;
            // The original plugin exposes each changing rectangle through Layer
            // position/size; GenericFlip uses this with the movie canvas size.
            layer->SetImageSize(w, h);
            layer->SetSize(w, h);
            layer->SetImageLeft(0); layer->SetImageTop(0);
            layer->SetLeft(left_ + static_cast<tjs_int>(frame->left));
            layer->SetTop(top_ + static_cast<tjs_int>(frame->top));
            auto* pixels = static_cast<uint8_t*>(layer->GetMainImagePixelBufferForWrite());
            const auto pitch = layer->GetMainImagePixelBufferPitch();
            // The original DLL writes straight BGRA even for ltAddAlpha /
            // dfAddAlpha. GenericFlip owns the following composition step.
            krkr::amv::CopyBGRA(*frame,pixels,pitch,w,h,false);
            layer->Update();
            return static_cast<tjs_int>(frame->index);
        });
    }
    tjs_int GetNumOfFrame() { return player_.GetInfo().count; }
    tjs_int GetFrame() { return static_cast<tjs_int>(player_.FrameNumber()); }
    void SetFrame(tjs_int value) { Guard([&] { player_.Seek(value); }); }
    bool GetLoop() { return player_.Loop(); }
    void SetLoop(bool value) { player_.SetLoop(value); }
    bool GetNextLoop() { return player_.NextLoop(); }
    void SetNextLoop(bool value) { player_.SetNextLoop(value); }
    tjs_int GetPreloadSamples() { return player_.Preload(); }
    void SetPreloadSamples(tjs_int value) { Guard([&] { player_.SetPreload(value); }); }
    tjs_int GetLeft() { return left_; }
    void SetLeft(tjs_int value) { left_=value; }
    tjs_int GetTop() { return top_; }
    void SetTop(tjs_int value) { top_=value; }
    tjs_int GetScreenWidth() { return player_.GetInfo().width; }
    tjs_int GetScreenHeight() { return player_.GetInfo().height; }
    tjs_real GetFPSScale() { return 1.0; }
    tjs_real GetFPSRate() { return player_.GetInfo().rate; }
private:
    krkr::amv::Player player_;
    tjs_int left_=0, top_=0;
};
} // anonymous namespace
class tTJSNC_AlphaMovie : public tTJSNativeClass
{
    typedef tTJSNativeClass inherited;

public:
    tTJSNC_AlphaMovie();

    static tjs_uint32 ClassID;

protected:
    tTJSNativeInstance* CreateNativeInstance()
    {
        return new tTJSNI_AlphaMovie();
    }
};
tjs_uint32 tTJSNC_AlphaMovie::ClassID = (tjs_uint32)-1;
tTJSNC_AlphaMovie::tTJSNC_AlphaMovie()
  : tTJSNativeClass(TJS_N("AlphaMovie")){
        // register native methods/properties

        TJS_BEGIN_NATIVE_MEMBERS(AlphaMovie)
TJS_BEGIN_NATIVE_METHOD_DECL(finalize) {
    TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_AlphaMovie);
    _this->Finalize();
    return TJS_S_OK;
} TJS_END_NATIVE_METHOD_DECL(finalize)

            //----------------------------------------------------------------------
            // constructor/methods
            //----------------------------------------------------------------------
            TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/ _this,
                                              /*var.type*/ tTJSNI_AlphaMovie,
                                              /*TJS class name*/ AlphaMovie){return TJS_S_OK;
}
TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/ AlphaMovie)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ open)
{
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AlphaMovie);
    if (numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    _this->open(param[0]->AsStringNoAddRef());
    return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/ open)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ clear)
{
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AlphaMovie);
    _this->clear();
    return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/ clear)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ showNextImage)
{
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AlphaMovie);
    if (numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    tjs_int num = _this->showNextImage(*param[0]);
    if (result)
        *result = num;
    return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/ showNextImage)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ isPlaying)
{
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AlphaMovie);
    if (result)
        *result = _this->isPlaying();
    return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/ isPlaying)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ play)
{
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AlphaMovie);
    _this->play();
    return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/ play)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ stop)
{
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AlphaMovie);
    _this->stop();
    return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/ stop)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ setPosition)
{
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AlphaMovie);

    if (numparams < 2)
        return TJS_E_BADPARAMCOUNT;
    _this->setPosition(*param[0], *param[1]);
    return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/ setPosition)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ setNextMovieFile)
{
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AlphaMovie);
    if (numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    _this->setNextMovieFile(param[0]->AsStringNoAddRef());
    return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/ setNextMovieFile)
//----------------------------------------------------------------------

//---------------------------------------------------------------------------

//----------------------------------------------------------------------
// properties
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(numOfFrame){TJS_BEGIN_NATIVE_PROP_GETTER{TJS_GET_NATIVE_INSTANCE(
    /*var. name*/ _this, /*var. type*/ tTJSNI_AlphaMovie);
*result = _this->GetNumOfFrame();
return TJS_S_OK;
}
TJS_END_NATIVE_PROP_GETTER

TJS_DENY_NATIVE_PROP_SETTER

}
TJS_END_NATIVE_PROP_DECL(numOfFrame)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(frame){
    TJS_BEGIN_NATIVE_PROP_GETTER{TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                                                         /*var. type*/ tTJSNI_AlphaMovie);
*result = _this->GetFrame();
return TJS_S_OK;
}
TJS_END_NATIVE_PROP_GETTER

TJS_BEGIN_NATIVE_PROP_SETTER
{
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AlphaMovie);
    _this->SetFrame(*param);
    return TJS_S_OK;
}
TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(frame)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(loop){
    TJS_BEGIN_NATIVE_PROP_GETTER{TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                                                         /*var. type*/ tTJSNI_AlphaMovie);
*result = _this->GetLoop();
return TJS_S_OK;
}
TJS_END_NATIVE_PROP_GETTER

TJS_BEGIN_NATIVE_PROP_SETTER
{
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AlphaMovie);
    _this->SetLoop(*param);
    return TJS_S_OK;
}
TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(loop)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(nextLoop){
    TJS_BEGIN_NATIVE_PROP_GETTER{TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                                                         /*var. type*/ tTJSNI_AlphaMovie);
*result = _this->GetNextLoop();
return TJS_S_OK;
}
TJS_END_NATIVE_PROP_GETTER

TJS_BEGIN_NATIVE_PROP_SETTER
{
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AlphaMovie);
    _this->SetNextLoop(*param);
    return TJS_S_OK;
}
TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(nextLoop)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(preloadSamples){
    TJS_BEGIN_NATIVE_PROP_GETTER{TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                                                         /*var. type*/ tTJSNI_AlphaMovie);
*result = _this->GetPreloadSamples();
return TJS_S_OK;
}
TJS_END_NATIVE_PROP_GETTER

TJS_BEGIN_NATIVE_PROP_SETTER
{
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AlphaMovie);
    _this->SetPreloadSamples(*param);
    return TJS_S_OK;
}
TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(preloadSamples)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(left){
    TJS_BEGIN_NATIVE_PROP_GETTER{TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                                                         /*var. type*/ tTJSNI_AlphaMovie);
*result = _this->GetLeft();
return TJS_S_OK;
}
TJS_END_NATIVE_PROP_GETTER

TJS_BEGIN_NATIVE_PROP_SETTER
{
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AlphaMovie);
    _this->SetLeft(*param);
    return TJS_S_OK;
}
TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(left)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(top){
    TJS_BEGIN_NATIVE_PROP_GETTER{TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                                                         /*var. type*/ tTJSNI_AlphaMovie);
*result = _this->GetTop();
return TJS_S_OK;
}
TJS_END_NATIVE_PROP_GETTER

TJS_BEGIN_NATIVE_PROP_SETTER
{
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AlphaMovie);
    _this->SetTop(*param);
    return TJS_S_OK;
}
TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(top)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(screenWidth){
    TJS_BEGIN_NATIVE_PROP_GETTER{TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                                                         /*var. type*/ tTJSNI_AlphaMovie);
*result = _this->GetScreenWidth();
return TJS_S_OK;
}
TJS_END_NATIVE_PROP_GETTER

TJS_DENY_NATIVE_PROP_SETTER

}
TJS_END_NATIVE_PROP_DECL(screenWidth)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(screenHeight){
    TJS_BEGIN_NATIVE_PROP_GETTER{TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                                                         /*var. type*/ tTJSNI_AlphaMovie);
*result = _this->GetScreenHeight();
return TJS_S_OK;
}
TJS_END_NATIVE_PROP_GETTER

TJS_DENY_NATIVE_PROP_SETTER

}
TJS_END_NATIVE_PROP_DECL(screenHeight)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(FPSScale){
    TJS_BEGIN_NATIVE_PROP_GETTER{TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                                                         /*var. type*/ tTJSNI_AlphaMovie);
*result = _this->GetFPSScale();
return TJS_S_OK;
}
TJS_END_NATIVE_PROP_GETTER

TJS_DENY_NATIVE_PROP_SETTER

}
TJS_END_NATIVE_PROP_DECL(FPSScale)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(FPSRate){
    TJS_BEGIN_NATIVE_PROP_GETTER{TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                                                         /*var. type*/ tTJSNI_AlphaMovie);
*result = _this->GetFPSRate();
return TJS_S_OK;
}
TJS_END_NATIVE_PROP_GETTER

TJS_DENY_NATIVE_PROP_SETTER

}
TJS_END_NATIVE_PROP_DECL(FPSRate)
//----------------------------------------------------------------------
TJS_END_NATIVE_MEMBERS
}
tTJSNativeClass* TVPCreateNativeClass_AlphaMovie()
{
    return new tTJSNC_AlphaMovie();
}


namespace {
void InitPlugin_AlphaMovie() {
    if (pluginClass) return;
    auto* global=TVPGetScriptDispatch();
    if (!global) return;
    try {
        pluginClass=TVPCreateNativeClass_AlphaMovie();
        tTJSVariant value(pluginClass);
        global->PropSet(TJS_MEMBERENSURE,TJS_W("AlphaMovie"),nullptr,&value,global);
    } catch (...) {
        if (pluginClass) { pluginClass->Release(); pluginClass=nullptr; }
        global->Release(); throw;
    }
    global->Release();
}
void UninitPlugin_AlphaMovie() {
    for (auto* instance:instances) instance->Finalize();
    auto* global=TVPGetScriptDispatch();
    if (global) {
        tTJSVariant current;
        if (TJS_SUCCEEDED(global->PropGet(TJS_MEMBERMUSTEXIST,TJS_W("AlphaMovie"),nullptr,&current,global)) &&
            current.Type()==tvtObject && current.AsObjectNoAddRef()==pluginClass)
            global->DeleteMember(0,TJS_W("AlphaMovie"),nullptr,global);
        global->Release();
    }
    if (pluginClass) { pluginClass->Release(); pluginClass=nullptr; }
}
}
NCB_PRE_REGIST_CALLBACK(InitPlugin_AlphaMovie);
NCB_POST_UNREGIST_CALLBACK(UninitPlugin_AlphaMovie);
