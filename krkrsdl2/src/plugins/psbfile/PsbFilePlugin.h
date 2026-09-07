#ifndef KRKRSDL2_PSB_FILE_PLUGIN_H
#define KRKRSDL2_PSB_FILE_PLUGIN_H

#include "tjsNative.h"

// Built-in, portable subset of psbfile.dll used by KAG games for PIMG UI
// assets.  The implementation lives in the engine so games can remain
// unmodified on platforms that cannot load Win32 DLL plug-ins.
class tTJSNI_PSBFile : public tTJSNativeInstance
{
	typedef tTJSNativeInstance inherited;

	iTJSDispatch2 *Root;

public:
	tTJSNI_PSBFile();
	~tTJSNI_PSBFile() override;

	tjs_error TJS_INTF_METHOD Construct(tjs_int numparams,
		tTJSVariant **param, iTJSDispatch2 *tjs_obj) override;
	void TJS_INTF_METHOD Invalidate() override;

	bool Load(const ttstr &storage);
	iTJSDispatch2 *GetRoot() const { return Root; }
};

class tTJSNC_PSBFile : public tTJSNativeClass
{
	typedef tTJSNativeClass inherited;

public:
	tTJSNC_PSBFile();
	static tjs_uint32 ClassID;

protected:
	tTJSNativeInstance *CreateNativeInstance() override;
};

tTJSNativeClass *TVPCreateNativeClass_PSBFile();

#endif
