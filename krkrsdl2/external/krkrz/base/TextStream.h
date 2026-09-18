//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Text read/write stream
//---------------------------------------------------------------------------
#ifndef TextStreamH
#define TextStreamH


#include "StorageIntf.h"

//---------------------------------------------------------------------------
// TextStream Functions
//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF(iTJSTextReadStream *, TVPCreateTextStreamForRead, (const ttstr &name, const ttstr &modestr));
TJS_EXP_FUNC_DEF(iTJSTextReadStream *, TVPCreateTextStreamForReadByEncoding, (const ttstr & name, const ttstr & modestr, const ttstr & encoding));
TJS_EXP_FUNC_DEF(iTJSTextWriteStream *, TVPCreateTextStreamForWrite, (const ttstr &name, const ttstr &modestr));
TJS_EXP_FUNC_DEF(void, TVPSetDefaultReadEncoding, (const ttstr& encoding));
TJS_EXP_FUNC_DEF(const tjs_char*, TVPGetDefaultReadEncoding, ());
// The encoding scripts are read with (Scripts.textEncoding).  Kirikiroid2's
// kirikiroid2.dll publishes it on Storages as setTextEncoding/getTextEncoding,
// and the Storages members live in StorageIntf.cpp, hence these accessors.
TJS_EXP_FUNC_DEF(void, TVPSetScriptTextEncoding, (const ttstr& encoding));
TJS_EXP_FUNC_DEF(const ttstr&, TVPGetScriptTextEncoding, ());
//---------------------------------------------------------------------------

#endif
