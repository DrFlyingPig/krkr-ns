#include "KrkrNSPaths.h"
//---------------------------------------------------------------------------
#include "KrkrNSPaths.h"
/*
#include "KrkrNSPaths.h"
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
#include "KrkrNSPaths.h"
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors
#include "KrkrNSPaths.h"

#include "KrkrNSPaths.h"
	See details of license at "license.txt"
#include "KrkrNSPaths.h"
*/
#include "KrkrNSPaths.h"
//---------------------------------------------------------------------------
#include "KrkrNSPaths.h"
// TJS2 Script Managing
#include "KrkrNSPaths.h"
//---------------------------------------------------------------------------
#include "KrkrNSPaths.h"

#include "KrkrNSPaths.h"
#include "tjsCommHead.h"
#include "KrkrNSLog.h"
#include <string>
#include <vector>

#include "tjs.h"
#include "tjsDebug.h"
#include "tjsArray.h"
#include "tjsDictionary.h"
#include "ScriptMgnIntf.h"
#include "StorageIntf.h"
#include "DebugIntf.h"
#include "WindowIntf.h"
#include "LayerIntf.h"
#include "WaveIntf.h"
#include "TimerIntf.h"
#include "EventIntf.h"
#include "SystemIntf.h"
#include "TickCount.h"
#include "PluginIntf.h"
#include "ClipboardIntf.h"
#include "MsgIntf.h"
#include "VideoOvlIntf.h"
#include "TextStream.h"
#include "Random.h"
#include "tjsRandomGenerator.h"
#include "SysInitIntf.h"
#include "PhaseVocoderFilter.h"
#ifdef WIN32
#endif
#if 1
#include "BasicDrawDevice.h"
#endif
#include "BinaryStream.h"
#include "SysInitImpl.h"
#include "SystemControl.h"
#include "Application.h"

#ifdef __SWITCH__
// UTF-8 view of a ttstr (which is UTF-16) for the SD log, truncated to maxlen
// characters.  Diagnostics only: the log is plain UTF-8 text.
static std::string krkrns_utf8_of(const ttstr &s, tjs_uint maxlen)
{
	std::string out;
	for (tjs_uint i = 0; i < s.GetLen() && i < maxlen; ++i)
	{
		tjs_uint32 ch = static_cast<tjs_uint32>(s[i]);
		if (ch < 0x80) out += static_cast<char>(ch);
		else if (ch < 0x800)
		{
			out += static_cast<char>(0xC0 | (ch >> 6));
			out += static_cast<char>(0x80 | (ch & 0x3F));
		}
		else
		{
			out += static_cast<char>(0xE0 | (ch >> 12));
			out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (ch & 0x3F));
		}
	}
	return out;
}
#endif

#include "RectItf.h"
#include "ImageFunction.h"
#include "BitmapIntf.h"
#include "tjsScriptBlock.h"
#if 0
#include "ApplicationSpecialPath.h"
#endif
#include "SystemImpl.h"
#include "BitmapLayerTreeOwner.h"
#include "Extension.h"

#ifdef KRKRSDL2_ENABLE_PSBFILE
#include "PsbFilePlugin.h"
#endif

#ifdef KRKRSDL2_ENABLE_KAGPARSER
#include "KAGParser.h"
#endif

#ifdef KRKRSDL2_ENABLE_CSVPARSER
#include "CSVParser.h"
#endif

#ifdef KRKRSDL2_ENABLE_TEXTRENDER
#include "TextRenderBase.h"
#endif

#ifdef KRKRZ_ENABLE_CANVAS
#include "CanvasIntf.h"
#include "OffscreenIntf.h"
#include "TextureIntf.h"
#include "Matrix44Intf.h"
#include "Matrix32Intf.h"
#include "ShaderProgramIntf.h"
#include "VertexBufferIntf.h"
#include "VertexBinderIntf.h"
#endif

//---------------------------------------------------------------------------
// global variables
//---------------------------------------------------------------------------
tTJS *TVPScriptEngine = NULL;
ttstr TVPStartupScriptName(TJS_W("startup.tjs"));
static ttstr TVPScriptTextEncoding(TJS_W("UTF-8"));
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// Garbage Collection stuff
//---------------------------------------------------------------------------
class tTVPTJSGCCallback : public tTVPCompactEventCallbackIntf
{
	void TJS_INTF_METHOD OnCompact(tjs_int level)
	{
		// OnCompact method from tTVPCompactEventCallbackIntf
		// called when the application is idle, deactivated, minimized, or etc...
		if(TVPScriptEngine)
		{
			if(level >= TVP_COMPACT_LEVEL_IDLE)
			{
				TVPScriptEngine->DoGarbageCollection();
			}
		}
	}
} static TVPTJSGCCallback;
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// TVPInitScriptEngine
//---------------------------------------------------------------------------
static bool TVPScriptEngineInit = false;
void TVPInitScriptEngine()
{
	KRKRNS_LOG("[script] TVPInitScriptEngine entry");
	if(TVPScriptEngineInit) return;
	TVPScriptEngineInit = true;
	KRKRNS_LOG("[script] init flag set");

	tTJSVariant val;

	// Set eval expression mode
	if(TVPGetCommandLine(TJS_W("-evalcontext"), &val) )
	{
		ttstr str(val);
		if(str == TJS_W("global"))
		{
			TJSEvalOperatorIsOnGlobal = true;
			TJSWarnOnNonGlobalEvalOperator = true;
		}
	}

	// Set igonre-prop compat mode
	if(TVPGetCommandLine(TJS_W("-unaryaster"), &val) )
	{
		ttstr str(val);
		if(str == TJS_W("compat"))
		{
			TJSUnaryAsteriskIgnoresPropAccess = true;
		}
	}

	// Set debug mode
	if(TVPGetCommandLine(TJS_W("-debug"), &val) )
	{
		ttstr str(val);
		if(str == TJS_W("yes"))
		{
			TJSEnableDebugMode = true;
			TVPAddImportantLog((const tjs_char *)TVPWarnDebugOptionEnabled);
			TJSWarnOnExecutionOnDeletingObject = true;
		}
	}
	// Set Read text encoding
	if(TVPGetCommandLine(TJS_W("-readencoding"), &val) )
	{
		ttstr str(val);
		TVPSetDefaultReadEncoding( str );
	}
	TVPScriptTextEncoding = ttstr(TVPGetDefaultReadEncoding());

#ifdef TVP_START_UP_SCRIPT_NAME
	TVPStartupScriptName = TVP_START_UP_SCRIPT_NAME;
#else
	// Set startup script name
	if(TVPGetCommandLine(TJS_W("-startup"), &val) )
	{
		ttstr str(val);
		TVPStartupScriptName = str;
	}
#endif

	KRKRNS_LOG("[script] before new tTJS()");
	// create script engine object
	KRKRNS_LOG("[script] before new tTJS (2)");
	TVPScriptEngine = new tTJS();
	KRKRNS_LOG("[script] new tTJS() ok");

	// add kirikiriz
	KRKRNS_LOG("[script] before SetPPValue kirikiriz");
	TVPScriptEngine->SetPPValue( TJS_W("kirikiriz"), 1 );
	KRKRNS_LOG("[script] SetPPValue ok");

	// system definition
#ifdef WIN32
	TVPScriptEngine->SetPPValue( TJS_W("windows"), 1 );
#endif

#ifdef ANDROID
	TVPScriptEngine->SetPPValue( TJS_W("android"), 1 );
#endif

	// set TJSGetRandomBits128
	KRKRNS_LOG("[script] before GetRandomBits128");
	TJSGetRandomBits128 = TVPGetRandomBits128;
	KRKRNS_LOG("[script] GetRandomBits128 ok");

	// script system initialization
	KRKRNS_LOG("[script] before ExecScript(system init)");
	TVPScriptEngine->ExecScript( TVPGetSystemInitializeScript() );
	KRKRNS_LOG("[script] ExecScript(system init) ok");

	// set console output gateway handler
	KRKRNS_LOG("[script] before SetConsoleOutput");
	TVPScriptEngine->SetConsoleOutput(TVPGetTJS2ConsoleOutputGateway());
	KRKRNS_LOG("[script] SetConsoleOutput ok");


	// set text stream functions
	TJSCreateTextStreamForRead = TVPCreateTextStreamForRead;
	TJSCreateTextStreamForWrite = TVPCreateTextStreamForWrite;
	
	// set binary stream functions
	TJSCreateBinaryStreamForRead = TVPCreateBinaryStreamInterfaceForRead;
	TJSCreateBinaryStreamForWrite = TVPCreateBinaryStreamInterfaceForWrite;

	// register some TVP classes/objects/functions/propeties
	iTJSDispatch2 *dsp;
	iTJSDispatch2 *global = TVPScriptEngine->GetGlobalNoAddRef();


#define REGISTER_OBJECT(classname, instance) \
	dsp = (instance); \
	val = tTJSVariant(dsp/*, dsp*/); \
	dsp->Release(); \
	global->PropSet(TJS_MEMBERENSURE|TJS_IGNOREPROP, TJS_W(#classname), NULL, \
		&val, global);

	/* classes */
	REGISTER_OBJECT(Debug, TVPCreateNativeClass_Debug());
	REGISTER_OBJECT(Font, TVPCreateNativeClass_Font());
	REGISTER_OBJECT(Layer, TVPCreateNativeClass_Layer());
	REGISTER_OBJECT(Timer, TVPCreateNativeClass_Timer());
	REGISTER_OBJECT(AsyncTrigger, TVPCreateNativeClass_AsyncTrigger());
	REGISTER_OBJECT(System, TVPCreateNativeClass_System());
	REGISTER_OBJECT(Storages, TVPCreateNativeClass_Storages());
	REGISTER_OBJECT(Plugins, TVPCreateNativeClass_Plugins());
	REGISTER_OBJECT(VideoOverlay, TVPCreateNativeClass_VideoOverlay());
	REGISTER_OBJECT(Clipboard, TVPCreateNativeClass_Clipboard());
	REGISTER_OBJECT(Scripts, TVPCreateNativeClass_Scripts()); // declared in this file
	REGISTER_OBJECT(Rect, TVPCreateNativeClass_Rect());
	REGISTER_OBJECT(Bitmap, TVPCreateNativeClass_Bitmap());
	REGISTER_OBJECT(ImageFunction, TVPCreateNativeClass_ImageFunction());
	REGISTER_OBJECT(BitmapLayerTreeOwner, TVPCreateNativeClass_BitmapLayerTreeOwner());
#ifdef KRKRSDL2_ENABLE_KAGPARSER
	// KAG 3's scenario parser is part of the classic Kirikiri core.  Keep it
	// built in on Switch because Win32 KAGParserEx plug-ins cannot be loaded.
	REGISTER_OBJECT(KAGParser, TVPCreateNativeClass_KAGParser());
#endif
#ifdef KRKRSDL2_ENABLE_CSVPARSER
	// Preserve csvParser.dll's public class for games that load configuration
	// tables through it.  The parser operates on KRKR storages, including XP3.
	REGISTER_OBJECT(CSVParser, TVPCreateNativeClass_CSVParser());
#endif
#ifdef KRKRSDL2_ENABLE_PSBFILE
	// Switch cannot load Win32 plug-ins at runtime.  Keep the public
	// psbfile.dll API, but provide it as a built-in native class.
	REGISTER_OBJECT(PSBFile, TVPCreateNativeClass_PSBFile());
#endif
#ifdef KRKRSDL2_ENABLE_TEXTRENDER
	// KAGEX's TextRender.tjs derives from the native TextRenderBase class.
	// Build that portable class into the NRO so games do not need patches.
	REGISTER_OBJECT(TextRenderBase, TVPCreateNativeClass_TextRenderBase());
#endif
#ifdef KRKRZ_ENABLE_CANVAS
	REGISTER_OBJECT(Canvas, TVPCreateNativeClass_Canvas());
	REGISTER_OBJECT(Texture, TVPCreateNativeClass_Texture());
	REGISTER_OBJECT(Offscreen, TVPCreateNativeClass_Offscreen());
	REGISTER_OBJECT(Matrix44, TVPCreateNativeClass_Matrix44());
	REGISTER_OBJECT(Matrix32, TVPCreateNativeClass_Matrix32());
	REGISTER_OBJECT(ShaderProgram, TVPCreateNativeClass_ShaderProgram());
	REGISTER_OBJECT(VertexBuffer, TVPCreateNativeClass_VertexBuffer());
	REGISTER_OBJECT(VertexBinder, TVPCreateNativeClass_VertexBinder());
#endif

	/* WaveSoundBuffer and its filters */
	iTJSDispatch2 * waveclass = NULL;
	REGISTER_OBJECT( WaveSoundBuffer, ( waveclass = TVPCreateNativeClass_SoundBuffer() ) );
	dsp = new tTJSNC_PhaseVocoder();
	val = tTJSVariant(dsp);
	dsp->Release();
	waveclass->PropSet(TJS_MEMBERENSURE|TJS_IGNOREPROP|TJS_STATICMEMBER,
		TJS_W("PhaseVocoder"), NULL, &val, waveclass);

	/* Window and its drawdevices */
	iTJSDispatch2 * windowclass = NULL;
	REGISTER_OBJECT(Window, (windowclass = TVPCreateNativeClass_Window()));
#ifdef WIN32
#endif
#if 1
	dsp = new tTJSNC_BasicDrawDevice();
	val = tTJSVariant(dsp);
	dsp->Release();
	windowclass->PropSet(TJS_MEMBERENSURE|TJS_IGNOREPROP|TJS_STATICMEMBER,
		TJS_W("BasicDrawDevice"), NULL, &val, windowclass);
#endif
	// Add Extension Classes
	TVPCauseAtInstallExtensionClass( global );

	// Garbage Collection Hook
	TVPAddCompactEventHook(&TVPTJSGCCallback);
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPUninitScriptEngine
//---------------------------------------------------------------------------
static bool TVPScriptEngineUninit = false;
void TVPUninitScriptEngine()
{
	if(TVPScriptEngineUninit) return;
	TVPScriptEngineUninit = true;

	TVPScriptEngine->Shutdown();
	TVPScriptEngine->Release();
	/*
		Objects, theirs lives are contolled by reference counter, may not be all
		freed here in some occations.
	*/
	TVPScriptEngine = NULL;
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPRestartScriptEngine
//---------------------------------------------------------------------------
void TVPRestartScriptEngine()
{
	TVPUninitScriptEngine();
	TVPScriptEngineInit = false;
	TVPInitScriptEngine();
}
//---------------------------------------------------------------------------
#ifdef __SWITCH__
// KRKR-ns: full script-engine restart for "end game -> back to launcher" on
// hosts that cannot chain-load the NRO (the emulator reports next-load=0).
//
// TVPUninitScriptEngine() is latched by TVPScriptEngineUninit and
// TVPInitScriptEngine() by TVPScriptEngineInit, so the pair can only ever run
// once per process: a plain restart would silently do nothing, and the next
// game would inherit the previous session's globals, classes and metadata.
// Clearing both latches is what makes a second, pristine engine possible.
void krkrsdl2_reset_script_engine_for_restart()
{
	KRKRNS_LOG("[reinit] script engine: uninit (init=%d uninit=%d engine=%p global=%p)",
		(int)TVPScriptEngineInit, (int)TVPScriptEngineUninit,
		(void *)TVPScriptEngine,
		TVPScriptEngine ? (void *)TVPScriptEngine->GetGlobalNoAddRef() : (void *)nullptr);
	TVPUninitScriptEngine();
	TVPScriptEngineUninit = false; // re-arm the one-shot shutdown latch
	TVPScriptEngineInit = false;   // and the one-shot init latch
	KRKRNS_LOG("[reinit] script engine: latches cleared, next init builds a fresh engine");
}
#endif
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPGetScriptEngine
//---------------------------------------------------------------------------
tTJS * TVPGetScriptEngine()
{
	return TVPScriptEngine;
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPGetScriptDispatch
//---------------------------------------------------------------------------
iTJSDispatch2 * TVPGetScriptDispatch()
{
	if(TVPScriptEngine) return TVPScriptEngine->GetGlobal(); else return NULL;
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// TVPExecuteScript
//---------------------------------------------------------------------------
void TVPExecuteScript(const ttstr& content, tTJSVariant *result)
{
	if(TVPScriptEngine)
		TVPScriptEngine->ExecScript(content, result);
	else
		TVPThrowInternalError;
}
//---------------------------------------------------------------------------
void TVPExecuteScript(const ttstr& content, const ttstr &name, tjs_int lineofs, tTJSVariant *result)
{
	if(TVPScriptEngine)
		TVPScriptEngine->ExecScript(content, result, NULL, &name, lineofs);
	else
		TVPThrowInternalError;
}
//---------------------------------------------------------------------------
void TVPExecuteScript(const ttstr& content, iTJSDispatch2 *context, tTJSVariant *result)
{
	if(TVPScriptEngine)
		TVPScriptEngine->ExecScript(content, result, context);
	else
		TVPThrowInternalError;
}
//---------------------------------------------------------------------------
void TVPExecuteScript(const ttstr& content, const ttstr &name, tjs_int lineofs, iTJSDispatch2 *context, tTJSVariant *result)
{
	if(TVPScriptEngine)
		TVPScriptEngine->ExecScript(content, result, context, &name, lineofs);
	else
		TVPThrowInternalError;
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPExecuteExpression
//---------------------------------------------------------------------------
void TVPExecuteExpression(const ttstr& content, tTJSVariant *result)
{
	TVPExecuteExpression(content, NULL, result);
}
//---------------------------------------------------------------------------
void TVPExecuteExpression(const ttstr& content, const ttstr &name, tjs_int lineofs, tTJSVariant *result)
{
	TVPExecuteExpression(content, name, lineofs, NULL, result);
}
//---------------------------------------------------------------------------
void TVPExecuteExpression(const ttstr& content, iTJSDispatch2 *context, tTJSVariant *result)
{
	if(TVPScriptEngine)
	{
		iTJSConsoleOutput *output = TVPScriptEngine->GetConsoleOutput();
		TVPScriptEngine->SetConsoleOutput(NULL); // once set TJS console to null
		try
		{
			TVPScriptEngine->EvalExpression(content, result, context);
		}
		catch(...)
		{
			TVPScriptEngine->SetConsoleOutput(output);
			throw;
		}
		TVPScriptEngine->SetConsoleOutput(output);
	}
	else
	{
		TVPThrowInternalError;
	}
}
//---------------------------------------------------------------------------
void TVPExecuteExpression(const ttstr& content, const ttstr &name, tjs_int lineofs, iTJSDispatch2 *context, tTJSVariant *result)
{
	if(TVPScriptEngine)
	{
		iTJSConsoleOutput *output = TVPScriptEngine->GetConsoleOutput();
		TVPScriptEngine->SetConsoleOutput(NULL); // once set TJS console to null
		try
		{
			TVPScriptEngine->EvalExpression(content, result, context, &name, lineofs);
		}
		catch(...)
		{
			TVPScriptEngine->SetConsoleOutput(output);
			throw;
		}
		TVPScriptEngine->SetConsoleOutput(output);
	}
	else
	{
		TVPThrowInternalError;
	}
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPExecuteBytecode
//---------------------------------------------------------------------------
void TVPExecuteBytecode( const tjs_uint8* content, size_t len, iTJSDispatch2 *context, tTJSVariant *result, const tjs_char *name )
{
	if(!TVPScriptEngine) TVPThrowInternalError;

	TVPScriptEngine->LoadByteCode( content, len, result, context, name);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TVPExecuteStorage(const ttstr &name, tTJSVariant *result, bool isexpression,
	const tjs_char * modestr)
{
	TVPExecuteStorage(name, NULL, result, isexpression, modestr);
}
//---------------------------------------------------------------------------
void TVPExecuteStorage(const ttstr &name, iTJSDispatch2 *context, tTJSVariant *result, bool isexpression,
	const tjs_char * modestr)
{
	// execute storage which contains script
	if(!TVPScriptEngine) TVPThrowInternalError;
	{ // for bytecode
		ttstr place(TVPSearchPlacedPath(name));
		ttstr shortname(TVPExtractStorageName(place));
		tTJSBinaryStream* stream = TVPCreateBinaryStreamForRead(place, modestr);
		if( stream ) {
			bool isbytecode = false;
			try {
				isbytecode = TVPScriptEngine->LoadByteCode( stream, result, context, shortname.c_str() );
			} catch(...) {
				delete stream;
				throw;
			}
			delete stream;
			if( isbytecode ) return;
		}
	}

	ttstr place(TVPSearchPlacedPath(name));
	ttstr shortname(TVPExtractStorageName(place));

	iTJSTextReadStream * stream = TVPCreateTextStreamForReadByEncoding(place, modestr,TVPScriptTextEncoding);
	ttstr buffer;
	try
	{
		stream->Read(buffer, 0);
	}
	catch(...)
	{
		stream->Destruct();
		throw;
	}
	stream->Destruct();

	if(TVPScriptEngine)
	{
		if(!isexpression)
			TVPScriptEngine->ExecScript(buffer, result, context,
				&shortname);
		else
			TVPScriptEngine->EvalExpression(buffer, result, context,
				&shortname);
	}
}
//---------------------------------------------------------------------------
void TVPCompileStorage( const ttstr& name, bool isrequestresult, bool outputdebug, bool isexpression, const ttstr& outputpath ) {
	// execute storage which contains script
	if(!TVPScriptEngine) TVPThrowInternalError;

	ttstr place(TVPSearchPlacedPath(name));
	ttstr shortname(TVPExtractStorageName(place));
	iTJSTextReadStream * stream = TVPCreateTextStreamForReadByEncoding(place, TJS_W(""),TVPScriptTextEncoding);

	ttstr buffer;
	try {
		stream->Read(buffer, 0);
	} catch(...) {
		stream->Destruct();
		throw;
	}
	stream->Destruct();

	tTJSBinaryStream* outputstream = TVPCreateStream(outputpath, TJS_BS_WRITE);
	if(TVPScriptEngine) {
		try {
			TVPScriptEngine->CompileScript( buffer.c_str(), outputstream, isrequestresult, outputdebug, isexpression, name.c_str(), 0 );
		} catch(...) {
			delete outputstream;
			throw;
		}
	}
	delete outputstream;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// TVPCreateMessageMapFile
//---------------------------------------------------------------------------
void TVPCreateMessageMapFile(const ttstr &filename)
{
#ifdef TJS_TEXT_OUT_CRLF
	ttstr script(TJS_W("{\r\n\tvar r = System.assignMessage;\r\n"));
#else
	ttstr script(TJS_W("{\n\tvar r = System.assignMessage;\n"));
#endif

	script += TJSCreateMessageMapString();

	script += TJS_W("}");

	iTJSTextWriteStream * stream = TVPCreateTextStreamForWrite(
		filename, TJS_W(""));
	try
	{
		stream->Write(script);
	}
	catch(...)
	{
		stream->Destruct();
		throw;
	}

	stream->Destruct();
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// TVPDumpScriptEngine
//---------------------------------------------------------------------------
void TVPDumpScriptEngine()
{
	TVPTJS2StartDump();
	TVPScriptEngine->SetConsoleOutput(TVPGetTJS2DumpOutputGateway());
	try
	{
		TVPScriptEngine->Dump();
	}
	catch(...)
	{
		TVPTJS2EndDump();
		TVPScriptEngine->SetConsoleOutput(TVPGetTJS2ConsoleOutputGateway());
		throw;
	}
	TVPScriptEngine->SetConsoleOutput(TVPGetTJS2ConsoleOutputGateway());
	TVPTJS2EndDump();
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPExecuteStartupScript
//---------------------------------------------------------------------------
void TVPExecuteStartupScript()
{
	// execute "startup.tjs"
	try
	{
		try
		{
			TVPAddLog( TVPInfoLoadingStartupScript + TVPStartupScriptName );
			TVPExecuteStorage(TVPStartupScriptName);
			TVPAddLog( (const tjs_char*)TVPInfoStartupScriptEnded );
		}
		TJS_CONVERT_TO_TJS_EXCEPTION
	}
	TVP_CATCH_AND_SHOW_SCRIPT_EXCEPTION(TJS_W("startup"))
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// unhandled exception handler related
//---------------------------------------------------------------------------
static bool  TJSGetSystem_exceptionHandler_Object(tTJSVariantClosure & dest)
{
	// get System.exceptionHandler
	iTJSDispatch2 * global = TVPGetScriptEngine()->GetGlobalNoAddRef();
	if(!global) return false;

	tTJSVariant val;
	tTJSVariant val2;
	tTJSVariantClosure clo;

	tjs_error er;
	er = global->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("System"), NULL, &val, global);
	if(TJS_FAILED(er)) return false;

	if(val.Type() != tvtObject) return false;

	clo = val.AsObjectClosureNoAddRef();

	if(clo.Object == NULL) return false;

	clo.PropGet(TJS_MEMBERMUSTEXIST, TJS_W("exceptionHandler"), NULL, &val2, NULL);

	if(val2.Type() != tvtObject) return false;

	dest = val2.AsObjectClosure();

	if(!dest.Object)
	{
		dest.Release();
		return false;
	}

	return true;
}
//---------------------------------------------------------------------------
bool TVPProcessUnhandledException(eTJSScriptException &e)
{
	bool result;
	tTJSVariantClosure clo;
	clo.Object = clo.ObjThis = NULL;

	try
	{
		// get the script engine
		tTJS *engine = TVPGetScriptEngine();
		if(!engine)
			return false; // the script engine had been shutdown

		// get System.exceptionHandler
		if(!TJSGetSystem_exceptionHandler_Object(clo))
			return false; // System.exceptionHandler cannot be retrieved

		// execute clo
		tTJSVariant obj(e.GetValue());

		tTJSVariant *pval[] =  { &obj };

		tTJSVariant res;

		clo.FuncCall(0, NULL, NULL, &res, 1, pval, NULL);

		result = res.operator bool();
	}
	catch(eTJSScriptError &e)
	{
		clo.Release();
		TVPShowScriptException(e);
	}
	catch(eTJS &e)
	{
		clo.Release();
		TVPShowScriptException(e);
	}
	catch(...)
	{
		clo.Release();
		throw;
	}
	clo.Release();

	return result;
}
//---------------------------------------------------------------------------
bool TVPProcessUnhandledException(eTJSScriptError &e)
{
	bool result;
	tTJSVariantClosure clo;
	clo.Object = clo.ObjThis = NULL;

	try
	{
		// get the script engine
		tTJS *engine = TVPGetScriptEngine();
		if(!engine)
			return false; // the script engine had been shutdown

		// get System.exceptionHandler
		if(!TJSGetSystem_exceptionHandler_Object(clo))
			return false; // System.exceptionHandler cannot be retrieved

		// execute clo
		tTJSVariant obj;
		tTJSVariant msg(e.GetMessage());
		tTJSVariant trace(e.GetTrace());
		TJSGetExceptionObject(engine, &obj, msg, &trace);

		tTJSVariant *pval[] =  { &obj };

		tTJSVariant res;

		clo.FuncCall(0, NULL, NULL, &res, 1, pval, NULL);

		result = res.operator bool();
	}
	catch(eTJSScriptError &e)
	{
		clo.Release();
		TVPShowScriptException(e);
	}
	catch(eTJS &e)
	{
		clo.Release();
		TVPShowScriptException(e);
	}
	catch(...)
	{
		clo.Release();
		throw;
	}
	clo.Release();

	return result;
}
//---------------------------------------------------------------------------
bool TVPProcessUnhandledException(eTJS &e)
{
	bool result;
	tTJSVariantClosure clo;
	clo.Object = clo.ObjThis = NULL;

	try
	{
		// get the script engine
		tTJS *engine = TVPGetScriptEngine();
		if(!engine)
			return false; // the script engine had been shutdown

		// get System.exceptionHandler
		if(!TJSGetSystem_exceptionHandler_Object(clo))
			return false; // System.exceptionHandler cannot be retrieved

		// execute clo
		tTJSVariant obj;
		tTJSVariant msg(e.GetMessage());
		TJSGetExceptionObject(engine, &obj, msg);

		tTJSVariant *pval[] =  { &obj };

		tTJSVariant res;

		clo.FuncCall(0, NULL, NULL, &res, 1, pval, NULL);

		result = res.operator bool();
	}
	catch(eTJSScriptError &e)
	{
		clo.Release();
		TVPShowScriptException(e);
	}
	catch(eTJS &e)
	{
		clo.Release();
		TVPShowScriptException(e);
	}
	catch(...)
	{
		clo.Release();
		throw;
	}
	clo.Release();

	return result;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
void TVPStartObjectHashMap()
{
	// addref ObjectHashMap if the program is being debugged.
	if(TJSEnableDebugMode)
		TJSAddRefObjectHashMap();
}

//---------------------------------------------------------------------------
// TVPBeforeProcessUnhandledException
//---------------------------------------------------------------------------
void TVPBeforeProcessUnhandledException()
{
#if 0
	TVPDumpHWException();
#endif
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPShowScriptException
//---------------------------------------------------------------------------
/*
	These functions display the error location, reason, etc.
	And disable the script event dispatching to avoid massive occurrence of
	errors.
*/
//---------------------------------------------------------------------------
void TVPShowScriptException(eTJS &e)
{
	TVPSetSystemEventDisabledState(true);
	TVPOnError();

	if(!TVPSystemUninitCalled)
	{
		ttstr errstr = (ttstr(TVPScriptExceptionRaised) + TJS_W("\n") + e.GetMessage());
		TVPAddLog(ttstr(TVPScriptExceptionRaised) + TJS_W("\n") + e.GetMessage());
#ifdef __SWITCH__
		// KRKR-ns: never let a script error stop the game.  Missing plugin
		// members, optional resources and stale references from a previous
		// session all surface here; reporting and continuing keeps every title
		// playable (the alternative is a fatal dialog, a stalled frame loop or
		// an exit).  Re-enable event delivery, which was disabled above.
		//
		// Log the MESSAGE and the script call stack, not just a marker: without
		// them a recovered error is undiagnosable (a bare "exception ignored"
		// line cannot be traced to a script or line).
		KRKRNS_LOG("[script] exception ignored (continuing): %s",
			krkrns_utf8_of(e.GetMessage(), 300).c_str());
		KRKRNS_LOG("[script]   script trace: %s",
			krkrns_utf8_of(TJSGetStackTraceString(16), 900).c_str());
		// A single recoverable error is fine, but a burst of them means the
		// environment is broken (duplicate definitions, missing plugin members).
		// Continuing in that state ends in a native crash, so end the session
		// and return to the picker instead.
		{
			extern bool krkrsdl2_game_mode;
			extern void krkrsdl2_request_return_to_launcher();
			static unsigned errorBurst = 0;
			static tjs_uint64 burstWindow = 0;
			const tjs_uint64 now = TVPGetTickCount();
			if (!burstWindow || now - burstWindow > 3000) { burstWindow = now; errorBurst = 0; }
			if (++errorBurst > 8 && krkrsdl2_game_mode)
			{
				errorBurst = 0;
				KRKRNS_LOG("[script] error burst: ending this game session");
				krkrsdl2_request_return_to_launcher();
			}
		}
		TVPSetSystemEventDisabledState(false);
#else
		Application->MessageDlg( errstr.AsStdString(), tjs_string(), mtError, mbOK );
		TVPTerminateSync(1);
#endif
	}
}
//---------------------------------------------------------------------------
void TVPShowScriptException(eTJSScriptError &e)
{
	TVPSetSystemEventDisabledState(true);
	TVPOnError();

	if(!TVPSystemUninitCalled)
	{
		ttstr errstr = (ttstr(TVPScriptExceptionRaised) + TJS_W("\n") + e.GetMessage());
		TVPAddLog(ttstr(TVPScriptExceptionRaised) + TJS_W("\n") + e.GetMessage());
		if(e.GetTrace().GetLen() != 0)
			TVPAddLog(ttstr(TJS_W("trace : ")) + e.GetTrace());
		Application->MessageDlg( errstr.AsStdString(), Application->GetTitle(), mtStop, mbOK );

#ifdef TVP_ENABLE_EXECUTE_AT_EXCEPTION
		const tjs_char* scriptName = e.GetBlockNoAddRef()->GetName();
		if( scriptName != NULL && scriptName[0] != 0 ) {
			ttstr path(scriptName);
			try {
				ttstr newpath = TVPGetPlacedPath(path);
				if( newpath.IsEmpty() ) {
					path = TVPNormalizeStorageName(path);
				} else {
					path = newpath;
				}
				TVPGetLocalName( path );
				tjs_string scriptPath( path.AsStdString() );
				tjs_int lineno = 1+e.GetBlockNoAddRef()->SrcPosToLine(e.GetPosition() )- e.GetBlockNoAddRef()->GetLineOffset();

#if defined(WIN32) && defined(_DEBUG) && !defined(ENABLE_DEBUGGER)
// デバッガ実行されている時、Visual Studio で行ジャンプする時の指定をデバッグ出力に出して、break で停止する
				if( ::IsDebuggerPresent() ) {
					tjs_string debuglile( tjs_string(TJS_W("2>"))+path.AsStdString()+TJS_W("(")+to_tjs_string(lineno)+TJS_W("): error :") + errstr.AsStdString() );
					::OutputDebugString( debuglile.c_str() );
					// ここで breakで停止した時、直前の出力行をダブルクリックすれば、例外箇所のスクリプトをVisual Studioで開ける
					::DebugBreak();
				}
#endif
				scriptPath = tjs_string(TJS_W("\"")) + scriptPath + tjs_string(TJS_W("\""));
				tTJSVariant val;
				if( TVPGetCommandLine(TJS_W("-exceptionexe"), &val) )
				{
					ttstr exepath(val);
					//exepath = ttstr(TJS_W("\"")) + exepath + ttstr(TJS_W("\""));
					if( TVPGetCommandLine(TJS_W("-exceptionarg"), &val) )
					{
						ttstr arg(val);
						if( !exepath.IsEmpty() && !arg.IsEmpty() ) {
							tjs_string str( arg.AsStdString() );
							str = ApplicationSpecialPath::ReplaceStringAll( str, tjs_string(TJS_W("%filepath%")), scriptPath );
							str = ApplicationSpecialPath::ReplaceStringAll( str, tjs_string(TJS_W("%line%")), to_tjs_string(lineno) );
							//exepath = exepath + ttstr(str);
							//_wsystem( exepath.c_str() );
							arg = ttstr(str);
							TVPAddLog( ttstr(TJS_W("(execute) "))+exepath+ttstr(TJS_W(" "))+arg);
#if defined(WIN32)
							TVPShellExecute( exepath, arg );
#endif	// Android では Intent で他のアプリに送れるようにする方がよい
						}
					}
				}
			} catch(...) {
			}
		}
#endif
#ifdef __SWITCH__
		// KRKR-ns: report and continue (see TVPShowScriptException(eTJS&)).
		// Include message and trace so a recovered error can still be located.
		KRKRNS_LOG("[script] script error ignored (continuing): %s",
			krkrns_utf8_of(e.GetMessage(), 300).c_str());
		KRKRNS_LOG("[script]   script trace: %s",
			krkrns_utf8_of(e.GetTrace(), 900).c_str());
		TVPSetSystemEventDisabledState(false);
#else
		TVPTerminateSync(1);
#endif
	}
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPInitializeStartupScript
//---------------------------------------------------------------------------
void TVPInitializeStartupScript()
{
	TVPStartObjectHashMap();

	TVPExecuteStartupScript();
	if(TVPTerminateOnNoWindowStartup && TVPGetWindowCount() == 0 ) {
		// no window is created and main window is invisible
		Application->Terminate();
	}
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// tTJSNC_Scripts
//---------------------------------------------------------------------------
static tTJSVariant TVPCloneScriptValue(const tTJSVariant &source);

class tTVPScriptDictionaryCloneCaller : public tTJSDispatch
{
	iTJSDispatch2 *Destination;

public:
	explicit tTVPScriptDictionaryCloneCaller(iTJSDispatch2 *destination)
		: Destination(destination) {}

	tjs_error TJS_INTF_METHOD FuncCall(tjs_uint32, const tjs_char *, tjs_uint32 *,
		tTJSVariant *result, tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *) override
	{
		if (numparams < 3) return TJS_E_BADPARAMCOUNT;
		tTJSVariant value = TVPCloneScriptValue(*param[2]);
		Destination->PropSet(TJS_MEMBERENSURE | static_cast<tjs_uint32>((tjs_int)*param[1]),
			param[0]->GetString(), nullptr, &value, Destination);
		if (result) *result = true;
		return TJS_S_OK;
	}
};

// EnumMembers adapter used by scriptsEx-compatible Scripts.getObjectKeys.
// Hidden members are implementation details and are excluded by the original
// plug-in as well.
class tTVPScriptObjectKeysCaller : public tTJSDispatch
{
	iTJSDispatch2 *Array;
	tjs_int Index = 0;

public:
	explicit tTVPScriptObjectKeysCaller(iTJSDispatch2 *array) : Array(array) {}

	tjs_error TJS_INTF_METHOD FuncCall(tjs_uint32, const tjs_char *, tjs_uint32 *,
		tTJSVariant *result, tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *) override
	{
		if (numparams >= 2 && (((tjs_int)*param[1]) & TJS_HIDDENMEMBER) == 0)
			Array->PropSetByNum(TJS_MEMBERENSURE, Index++, param[0], Array);
		if (result) *result = true;
		return TJS_S_OK;
	}
};

static tTJSVariant TVPGetScriptObjectKeys(tTJSVariantClosure &object)
{
	iTJSDispatch2 *array = TJSCreateArrayObject();
	auto *caller = new tTVPScriptObjectKeysCaller(array);
	tTJSVariantClosure closure(caller);
	object.EnumMembers(TJS_IGNOREPROP | TJS_ENUM_NO_VALUE, &closure, nullptr);
	caller->Release();
	static tjs_uint sort_hint = 0;
	array->FuncCall(0, TJS_W("sort"), &sort_hint, nullptr, 0, nullptr, array);
	tTJSVariant result(array, array);
	array->Release();
	return result;
}

// scriptsEx-compatible recursive structural comparison.  This deliberately
// follows the plug-in's semantics: only two Arrays or two Dictionaries are
// traversed; other objects use TJS discern comparison.  Numeric-loose mode
// additionally treats integer and real values with the same value as equal.
static bool TVPEqualScriptValues(const tTJSVariant &left,
	const tTJSVariant &right, bool numeric_loose)
{
	if(left.Type() == tvtObject && right.Type() == tvtObject)
	{
		if(left.AsObjectNoAddRef() == right.AsObjectNoAddRef()) return true;

		tTJSVariant left_copy(left), right_copy(right);
		tTJSVariantClosure &left_object = left_copy.AsObjectClosureNoAddRef();
		tTJSVariantClosure &right_object = right_copy.AsObjectClosureNoAddRef();
		if(!left_object.Object || !right_object.Object)
			return left.DiscernCompare(right);

		const bool left_array = left_object.IsInstanceOf(0, nullptr, nullptr,
			TJS_W("Array"), nullptr) == TJS_S_TRUE;
		const bool right_array = right_object.IsInstanceOf(0, nullptr, nullptr,
			TJS_W("Array"), nullptr) == TJS_S_TRUE;
		if(left_array && right_array)
		{
			tTJSVariant left_count_value, right_count_value;
			static tjs_uint count_hint = 0;
			left_object.PropGet(0, TJS_W("count"), &count_hint,
				&left_count_value, nullptr);
			right_object.PropGet(0, TJS_W("count"), &count_hint,
				&right_count_value, nullptr);
			if(!left_count_value.DiscernCompare(right_count_value)) return false;
			const tjs_int count = left_count_value;
			for(tjs_int i = 0; i < count; ++i)
			{
				tTJSVariant left_value, right_value;
				left_object.PropGetByNum(TJS_IGNOREPROP, i, &left_value, nullptr);
				right_object.PropGetByNum(TJS_IGNOREPROP, i, &right_value, nullptr);
				if(!TVPEqualScriptValues(left_value, right_value, numeric_loose))
					return false;
			}
			return true;
		}

		const bool left_dictionary = left_object.IsInstanceOf(0, nullptr, nullptr,
			TJS_W("Dictionary"), nullptr) == TJS_S_TRUE;
		const bool right_dictionary = right_object.IsInstanceOf(0, nullptr, nullptr,
			TJS_W("Dictionary"), nullptr) == TJS_S_TRUE;
		if(left_dictionary && right_dictionary)
		{
			tTJSVariant left_keys_value = TVPGetScriptObjectKeys(left_object);
			tTJSVariant right_keys_value = TVPGetScriptObjectKeys(right_object);
			tTJSVariantClosure &left_keys = left_keys_value.AsObjectClosureNoAddRef();
			tTJSVariantClosure &right_keys = right_keys_value.AsObjectClosureNoAddRef();
			tTJSVariant left_count_value, right_count_value;
			static tjs_uint count_hint = 0;
			left_keys.PropGet(0, TJS_W("count"), &count_hint,
				&left_count_value, nullptr);
			right_keys.PropGet(0, TJS_W("count"), &count_hint,
				&right_count_value, nullptr);
			if(!left_count_value.DiscernCompare(right_count_value)) return false;
			const tjs_int count = left_count_value;
			for(tjs_int i = 0; i < count; ++i)
			{
				tTJSVariant left_key, right_key;
				left_keys.PropGetByNum(TJS_IGNOREPROP, i, &left_key, nullptr);
				right_keys.PropGetByNum(TJS_IGNOREPROP, i, &right_key, nullptr);
				if(!left_key.DiscernCompare(right_key)) return false;

				tTJSVariant left_value, right_value;
				const tjs_char *key = left_key.GetString();
				if(TJS_FAILED(left_object.PropGet(TJS_MEMBERMUSTEXIST, key, nullptr,
					&left_value, nullptr)) ||
					TJS_FAILED(right_object.PropGet(TJS_MEMBERMUSTEXIST, key, nullptr,
					&right_value, nullptr)))
					return false;
				if(!TVPEqualScriptValues(left_value, right_value, numeric_loose))
					return false;
			}
			return true;
		}
	}

	if(numeric_loose &&
		(left.Type() == tvtInteger || left.Type() == tvtReal) &&
		(right.Type() == tvtInteger || right.Type() == tvtReal))
		return left.NormalCompare(right);

	return left.DiscernCompare(right);
}

// EnumMembers adapter used by scriptsEx-compatible Scripts.foreach.
// The callback receives (key, value, ...extraArgs) and returning any
// non-void value stops traversal, matching the original plug-in.
class tTVPScriptDictionaryForeachCaller : public tTJSDispatch
{
	iTJSDispatch2 *Function;
	iTJSDispatch2 *FunctionThis;
	tTJSVariant **Parameters;
	tjs_int ParameterCount;

public:
	tTJSVariant BreakResult;

	tTVPScriptDictionaryForeachCaller(iTJSDispatch2 *function,
		iTJSDispatch2 *function_this, tTJSVariant **parameters,
		tjs_int parameter_count)
		: Function(function), FunctionThis(function_this), Parameters(parameters),
		  ParameterCount(parameter_count) {}

	tjs_error TJS_INTF_METHOD FuncCall(tjs_uint32, const tjs_char *, tjs_uint32 *,
		tTJSVariant *result, tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *) override
	{
		BreakResult.Clear();
		if (numparams >= 3 && (((tjs_int)*param[1]) & TJS_HIDDENMEMBER) == 0)
		{
			Parameters[0] = param[0];
			Parameters[1] = param[2];
			Function->FuncCall(0, nullptr, nullptr, &BreakResult,
				ParameterCount, Parameters, FunctionThis);
		}
		if (result) *result = BreakResult.Type() == tvtVoid;
		return TJS_S_OK;
	}
};

// Compatible subset of scriptsEx.dll's Scripts.clone: arrays and
// dictionaries are copied recursively, other objects may provide clone(),
// while scalar and octet values retain their normal TJS value semantics.
static tTJSVariant TVPCloneScriptValue(const tTJSVariant &source)
{
	if (source.Type() != tvtObject) return source;
	tTJSVariant copy(source);
	tTJSVariantClosure &object = copy.AsObjectClosureNoAddRef();
	if (!object.Object) return copy;

	if (object.IsInstanceOf(0, nullptr, nullptr, TJS_W("Array"), nullptr) == TJS_S_TRUE)
	{
		iTJSDispatch2 *array = TJSCreateArrayObject();
		tTJSVariant countValue;
		static tjs_uint countHint = 0;
		object.PropGet(0, TJS_W("count"), &countHint, &countValue, nullptr);
		const tjs_int count = countValue;
		for (tjs_int i = 0; i < count; ++i)
		{
			tTJSVariant value;
			object.PropGetByNum(TJS_IGNOREPROP, i, &value, nullptr);
			value = TVPCloneScriptValue(value);
			array->PropSetByNum(TJS_MEMBERENSURE, i, &value, array);
		}
		tTJSVariant result(array, array);
		array->Release();
		return result;
	}

	if (object.IsInstanceOf(0, nullptr, nullptr, TJS_W("Dictionary"), nullptr) == TJS_S_TRUE)
	{
		iTJSDispatch2 *dictionary = TJSCreateDictionaryObject();
		auto *caller = new tTVPScriptDictionaryCloneCaller(dictionary);
		tTJSVariantClosure closure(caller);
		object.EnumMembers(TJS_IGNOREPROP, &closure, nullptr);
		caller->Release();
		tTJSVariant result(dictionary, dictionary);
		dictionary->Release();
		return result;
	}

	tTJSVariant result;
	static tjs_uint cloneHint = 0;
	if (object.FuncCall(0, TJS_W("clone"), &cloneHint, &result, 0, nullptr, nullptr) == TJS_S_TRUE)
		return result;
	return copy;
}

tjs_uint32 tTJSNC_Scripts::ClassID = -1;
tTJSNC_Scripts::tTJSNC_Scripts() : inherited(TJS_W("Scripts"))
{
	// registration of native members

	TJS_BEGIN_NATIVE_MEMBERS(Scripts)
	TJS_DECL_EMPTY_FINALIZE_METHOD
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL_NO_INSTANCE(/*TJS class name*/Scripts)
{
	return TJS_S_OK;
}
TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/Scripts)
//----------------------------------------------------------------------

//-- methods

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/execStorage)
{
	// execute script which stored in storage
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
#ifdef __SWITCH__
	{
		// Log both the requested name and the auto-path resolution result:
		// several KAG scripts exist both in the game archive and in the
		// engine's compat folder, and which copy wins decides whether boot
		// survives, so the resolved path is the important half of this line.
		ttstr nm = *param[0];
		std::string u8;
		for (tjs_uint i = 0; i < nm.GetLen() && i < 150; ++i)
		{
			tjs_uint32 ch = (tjs_uint32)nm[i];
			if (ch < 0x80) u8 += (char)ch;
			else if (ch < 0x800) { u8 += (char)(0xC0|(ch>>6)); u8 += (char)(0x80|(ch&0x3F)); }
			else { u8 += (char)(0xE0|(ch>>12)); u8 += (char)(0x80|((ch>>6)&0x3F)); u8 += (char)(0x80|(ch&0x3F)); }
		}
		std::string local8;
		try
		{
			ttstr ln = nm;
			TVPGetLocalName(ln);
			for (tjs_uint i = 0; i < ln.GetLen() && i < 220; ++i)
			{
				tjs_uint32 ch = (tjs_uint32)ln[i];
				if (ch < 0x80) local8 += (char)ch;
				else if (ch < 0x800) { local8 += (char)(0xC0|(ch>>6)); local8 += (char)(0x80|(ch&0x3F)); }
				else { local8 += (char)(0xE0|(ch>>12)); local8 += (char)(0x80|((ch>>6)&0x3F)); local8 += (char)(0x80|(ch&0x3F)); }
			}
		}
		catch (...) { local8 = "<unresolved>"; }
		KRKRNS_LOG("[execStorage] %s <- %s", u8.c_str(), local8.c_str());
	}
#endif

	ttstr name = *param[0];

	ttstr modestr;
	if(numparams >=2 && param[1]->Type() != tvtVoid)
		modestr = *param[1];

	iTJSDispatch2 *context = numparams >= 3 && param[2]->Type() != tvtVoid ? param[2]->AsObjectNoAddRef() : NULL;
	
	TVPExecuteStorage(name, context, result, false, modestr.c_str());

#ifdef __SWITCH__
	// KRKR-ns: a game ships its own system/k2compat.tjs (the desktop KAG copy)
	// and executes it during boot, which REPLACES global.Krkr2CompatUtils with a
	// stub whose members are meant to come from k2compat.dll -- a plugin that
	// does not exist here.  Everything looks fine until KAG's custom.tjs calls
	// one of those members and the boot dies with
	// `Member "loadPlugin" does not exist` (initialize.tjs -> KAGLoadScriptOnce).
	//
	// Repairing the namespace only at session end is not enough: the game
	// overwrites it again on every boot, so the very next
	// `-- back to the launcher -- pick another game --` cycle breaks while the
	// first game still worked.  Re-install our namespace immediately after the
	// game's own k2compat script runs, so the members KAG needs are always there
	// no matter which copy of that script the game carries.
	{
		const std::string local = krkrns_utf8_of(name, 220);
		std::string base = local;
		const size_t slash = base.find_last_of("/\\");
		if (slash != std::string::npos) base = base.substr(slash + 1);
		for (size_t i = 0; i < base.size(); ++i)
			base[i] = static_cast<char>(tolower(static_cast<unsigned char>(base[i])));
		if (base == "k2compat.tjs")
		{
			KRKRNS_LOG("[compat] game loaded %s -> reinstalling namespace", base.c_str());
			TVPExecuteStorage(ttstr(TJS_W("file://?/romfs:/compat/system/k2compat_reinstall.tjs")),
				nullptr, nullptr, false, nullptr);
		}
	}
#endif

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/execStorage)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/evalStorage)
{
	// execute expression which stored in storage
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr name = *param[0];

	ttstr modestr;
	if(numparams >=2 && param[1]->Type() != tvtVoid)
		modestr = *param[1];

	iTJSDispatch2 *context = numparams >= 3 && param[2]->Type() != tvtVoid ? param[2]->AsObjectNoAddRef() : NULL;

	TVPExecuteStorage(name, context, result, true, modestr.c_str());

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/evalStorage)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/clone)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
	if(result) *result = TVPCloneScriptValue(*param[0]);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/clone)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getObjectKeys)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
	tTJSVariantClosure &object = param[0]->AsObjectClosureNoAddRef();
	if(!object.Object) return TJS_E_INVALIDPARAM;

	iTJSDispatch2 *array = TJSCreateArrayObject();
	auto *caller = new tTVPScriptObjectKeysCaller(array);
	tTJSVariantClosure closure(caller);
	object.EnumMembers(TJS_IGNOREPROP | TJS_ENUM_NO_VALUE, &closure, nullptr);
	caller->Release();
	static tjs_uint sort_hint = 0;
	array->FuncCall(0, TJS_W("sort"), &sort_hint, nullptr, 0, nullptr, array);
	if(result) *result = tTJSVariant(array, array);
	array->Release();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getObjectKeys)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getObjectCount)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
	tjs_int count = 0;
	param[0]->AsObjectClosureNoAddRef().GetCount(&count, nullptr, nullptr, nullptr);
	if(result) *result = count;
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getObjectCount)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getObjectContext)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
	iTJSDispatch2 *context = param[0]->AsObjectClosureNoAddRef().ObjThis;
	if(result) *result = tTJSVariant(context, context);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getObjectContext)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/isNullContext)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
	if(result) *result = param[0]->AsObjectClosureNoAddRef().ObjThis == nullptr;
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/isNullContext)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/equalStruct)
{
	if(numparams < 2) return TJS_E_BADPARAMCOUNT;
	if(result) *result = TVPEqualScriptValues(*param[0], *param[1], false);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/equalStruct)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/equalStructNumericLoose)
{
	if(numparams < 2) return TJS_E_BADPARAMCOUNT;
	if(result) *result = TVPEqualScriptValues(*param[0], *param[1], true);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/equalStructNumericLoose)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/foreach)
{
	if(numparams < 2) return TJS_E_BADPARAMCOUNT;
	tTJSVariantClosure &object = param[0]->AsObjectClosureNoAddRef();
	tTJSVariantClosure &function_closure = param[1]->AsObjectClosureNoAddRef();
	if(!object.Object || !function_closure.Object) return TJS_E_INVALIDPARAM;

	iTJSDispatch2 *function_this = function_closure.ObjThis;
	if(!function_this) function_this = objthis;
	std::vector<tTJSVariant *> parameters(static_cast<size_t>(numparams));
	for(tjs_int i = 2; i < numparams; ++i)
		parameters[static_cast<size_t>(i)] = param[i];

	if(object.IsInstanceOf(0, nullptr, nullptr, TJS_W("Array"), nullptr) == TJS_S_TRUE)
	{
		tTJSVariant count_value;
		static tjs_uint count_hint = 0;
		object.PropGet(0, TJS_W("count"), &count_hint, &count_value, nullptr);
		const tjs_int count = count_value;
		tTJSVariant key, value, break_result;
		parameters[0] = &key;
		parameters[1] = &value;
		for(tjs_int i = 0; i < count; ++i)
		{
			key = i;
			break_result.Clear();
			object.PropGetByNum(TJS_IGNOREPROP, i, &value, nullptr);
			function_closure.Object->FuncCall(0, nullptr, nullptr, &break_result,
				numparams, parameters.data(), function_this);
			if(break_result.Type() != tvtVoid) break;
		}
		if(result) *result = break_result;
	}
	else
	{
		auto *caller = new tTVPScriptDictionaryForeachCaller(
			function_closure.Object, function_this, parameters.data(), numparams);
		tTJSVariantClosure closure(caller);
		object.EnumMembers(TJS_IGNOREPROP, &closure, nullptr);
		if(result) *result = caller->BreakResult;
		caller->Release();
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/foreach)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/compileStorage) // bytecode
{
	if(numparams < 2) return TJS_E_BADPARAMCOUNT;

	ttstr name = *param[0];
	ttstr output = *param[1];

	bool isresult = false;
	if( numparams >= 3 && (tjs_int)*param[2] ) {
		isresult = true;
	}

	bool outputdebug = false;
	if( numparams >= 4 && (tjs_int)*param[3] ) {
		outputdebug = true;
	}

	bool isexpression = false;
	if( numparams >= 5 && (tjs_int)*param[4] ) {
		isexpression = true;
	}
	TVPCompileStorage( name, isresult, outputdebug, isexpression, output );

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/compileStorage)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/exec)
{
	// execute given string as a script
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr content = *param[0];

	ttstr name;
	tjs_int lineofs = 0;
	if(numparams >= 2 && param[1]->Type() != tvtVoid) name = *param[1];
	if(numparams >= 3 && param[2]->Type() != tvtVoid) lineofs = *param[2];

	iTJSDispatch2 *context = numparams >= 4 && param[3]->Type() != tvtVoid ? param[3]->AsObjectNoAddRef() : NULL;

	if(TVPScriptEngine)
	{
#ifdef __SWITCH__
		{
			// KRKR-ns diagnostic: startup scripts call exec("@set(...)").
			// Log content and any failure; a boot-halting exec is visible.
			std::string utf8;
			for (tjs_uint i = 0; i < content.GetLen() && i < 300; ++i)
			{
				tjs_uint32 ch = static_cast<tjs_uint32>(content[i]);
				if (ch < 0x80) utf8 += static_cast<char>(ch);
				else if (ch < 0x800)
				{
					utf8 += static_cast<char>(0xC0 | (ch >> 6));
					utf8 += static_cast<char>(0x80 | (ch & 0x3F));
				}
				else
				{
					utf8 += static_cast<char>(0xE0 | (ch >> 12));
					utf8 += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
					utf8 += static_cast<char>(0x80 | (ch & 0x3F));
				}
			}
			KRKRNS_LOG("[exec] %s", utf8.c_str());
		}
		try
		{
			TVPScriptEngine->ExecScript(content, result, context, &name, lineofs);
			KRKRNS_LOG("[exec] done");
		}
		catch (eTJSScriptException &e)
		{
			std::string utf8m;
			ttstr msg(e.GetMessage());
			for (tjs_uint i = 0; i < msg.GetLen() && i < 300; ++i)
			{
				tjs_uint32 ch = static_cast<tjs_uint32>(msg[i]);
				if (ch < 0x80) utf8m += static_cast<char>(ch);
				else if (ch < 0x800)
				{
					utf8m += static_cast<char>(0xC0 | (ch >> 6));
					utf8m += static_cast<char>(0x80 | (ch & 0x3F));
				}
				else
				{
					utf8m += static_cast<char>(0xE0 | (ch >> 12));
					utf8m += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
					utf8m += static_cast<char>(0x80 | (ch & 0x3F));
				}
			}
			KRKRNS_LOG("[exec] FAILED: %s", utf8m.c_str());
			throw;
		}
		catch (...)
		{
			KRKRNS_LOG("[exec] FAILED: unknown exception");
			throw;
		}
#else
		TVPScriptEngine->ExecScript(content, result, context, &name, lineofs);
#endif
	}
	else
		TVPThrowInternalError;

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/exec)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/eval)
{
	// execute given string as a script
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr content = *param[0];

#ifdef __SWITCH__
	// KRKR-ns: [eval] / [eval] result logging cap, declared at function scope
	// because both log lines (command and result) gate on it.  KAG evaluates
	// constantly during play; an uncapped pair per call dominated the
	// per-session log volume.  (The runaway-script guard below is
	// unconditional and unaffected.)
	static int evalLogs = 0;
	const bool logThisEval = evalLogs < 200;
	if (logThisEval) ++evalLogs;
#endif

#ifdef __SWITCH__
	{
		// KRKR-ns diagnostic: games drive KAG boot through eval("exec ...");
		// log the first evals of the session so a silently failing boot
		// command is visible.
		if (logThisEval)
		{
			std::string utf8;
			for (tjs_uint i = 0; i < content.GetLen() && i < 150; ++i)
			{
				tjs_uint32 ch = static_cast<tjs_uint32>(content[i]);
				if (ch < 0x80) utf8 += static_cast<char>(ch);
				else if (ch < 0x800)
				{
					utf8 += static_cast<char>(0xC0 | (ch >> 6));
					utf8 += static_cast<char>(0x80 | (ch & 0x3F));
				}
				else
				{
					utf8 += static_cast<char>(0xE0 | (ch >> 12));
					utf8 += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
					utf8 += static_cast<char>(0x80 | (ch & 0x3F));
				}
			}
			KRKRNS_LOG("[eval] %s", utf8.c_str());
		}

		// KRKR-ns: runaway-script guard.  A menu page can spin thousands of
		// Scripts.eval calls with no I/O and no frame output (device logs:
		// thousands of "@'text/jp/…'"/"@'f_…'" evals, no [prof], heartbeats
		// alone) — the game looks frozen.  Interrupt the script so KAG's
		// error handler can drop back to its menu instead of freezing forever.
		// Marker sdmc:/switch/KRKR-ns/no-eval-guard.txt disables this.
		{
			static bool guardChecked = false;
			static bool guardEnabled = true;
			if (!guardChecked)
			{
				guardChecked = true;
				FILE* g = fopen(KRKRNS_BASE_A "/no-eval-guard.txt", "rb");
				if (g) { fclose(g); guardEnabled = false; }
			}
			if (guardEnabled)
			{
				static unsigned stormCount = 0;
				static tjs_uint64 stormStart = 0;
				const tjs_uint64 now = TVPGetTickCount();
				if (!stormStart || now - stormStart > 3000) { stormStart = now; stormCount = 0; }
				if (++stormCount > 2500)
				{
					stormCount = 0;
					// Dump the script call stack: this is what pinpoints the
					// runaway loop's script file and line (the eval frames sit
					// on top of the looping caller).
					{
						ttstr trace = TJSGetStackTraceString(16);
						std::string u8;
						for (tjs_uint i = 0; i < trace.GetLen() && i < 900; ++i)
						{
							tjs_uint32 ch = static_cast<tjs_uint32>(trace[i]);
							if (ch < 0x80) u8 += static_cast<char>(ch);
							else if (ch < 0x800)
							{
								u8 += static_cast<char>(0xC0 | (ch >> 6));
								u8 += static_cast<char>(0x80 | (ch & 0x3F));
							}
							else
							{
								u8 += static_cast<char>(0xE0 | (ch >> 12));
								u8 += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
								u8 += static_cast<char>(0x80 | (ch & 0x3F));
							}
						}
						KRKRNS_LOG("[eval] STORM trace: %s", u8.c_str());
					}
					KRKRNS_LOG("[eval] STORM: script aborting runaway loop");
					TVPThrowExceptionMessage(
						TJS_W("KRKRNS eval storm guard interrupted the script"));
				}
			}
		}
	}
#endif

	ttstr name;
	tjs_int lineofs = 0;
	if(numparams >= 2 && param[1]->Type() != tvtVoid) name = *param[1];
	if(numparams >= 3 && param[2]->Type() != tvtVoid) lineofs = *param[2];

	iTJSDispatch2 *context = numparams >= 4 && param[3]->Type() != tvtVoid ? param[3]->AsObjectNoAddRef() : NULL;
	
	if(TVPScriptEngine)
	{
#ifdef __SWITCH__
		try
		{
			TVPScriptEngine->EvalExpression(content, result, context,
				&name, lineofs);
			if (result && result->Type() != tvtVoid && logThisEval)
			{
				// KRKR-ns: startup control flow branches on eval results
				// (e.g. "@if(kirikiriz)1@endif"); log what actually came back.
				ttstr rs(*result);
				std::string utf8r;
				for (tjs_uint i = 0; i < rs.GetLen() && i < 150; ++i)
				{
					tjs_uint32 ch = static_cast<tjs_uint32>(rs[i]);
					if (ch < 0x80) utf8r += static_cast<char>(ch);
					else if (ch < 0x800)
					{
						utf8r += static_cast<char>(0xC0 | (ch >> 6));
						utf8r += static_cast<char>(0x80 | (ch & 0x3F));
					}
					else
					{
						utf8r += static_cast<char>(0xE0 | (ch >> 12));
						utf8r += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
						utf8r += static_cast<char>(0x80 | (ch & 0x3F));
					}
				}
				KRKRNS_LOG("[eval] result: %s", utf8r.c_str());
			}
			else if (logThisEval)
			{
				KRKRNS_LOG("[eval] result: <void>");
			}
		}
		catch (eTJSScriptException &e)
		{
			std::string utf8;
			ttstr msg(e.GetMessage());
			for (tjs_uint i = 0; i < msg.GetLen() && i < 300; ++i)
			{
				tjs_uint32 ch = static_cast<tjs_uint32>(msg[i]);
				if (ch < 0x80) utf8 += static_cast<char>(ch);
				else if (ch < 0x800)
				{
					utf8 += static_cast<char>(0xC0 | (ch >> 6));
					utf8 += static_cast<char>(0x80 | (ch & 0x3F));
				}
				else
				{
					utf8 += static_cast<char>(0xE0 | (ch >> 12));
					utf8 += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
					utf8 += static_cast<char>(0x80 | (ch & 0x3F));
				}
			}
			KRKRNS_LOG("[eval] FAILED: %s", utf8.c_str());
			throw;
		}
#else
		TVPScriptEngine->EvalExpression(content, result, context,
			&name, lineofs);
#endif
	}
	else
		TVPThrowInternalError;

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/eval)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/dump)
{
	// execute given string as a script
	TVPDumpScriptEngine();

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/dump)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getTraceString)
{
	// get current stack trace as string
	tjs_int limit = 0;

	if(numparams >= 1 && param[0]->Type() != tvtVoid)
		limit = *param[0];

	if(result)
	{
		*result = TJSGetStackTraceString(limit);
	}

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getTraceString)
//----------------------------------------------------------------------
#ifdef TJS_DEBUG_DUMP_STRING
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/dumpStringHeap)
{
	// dump all strings held by TJS2 framework
	TJSDumpStringHeap();

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/dumpStringHeap)
#endif
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setCallMissing) /* UNDOCUMENTED: subject to change */
{
	// set to call "missing" method
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	iTJSDispatch2 *dsp = param[0]->AsObjectNoAddRef();

	if(dsp)
	{
		tTJSVariant missing(TJS_W("missing"));
		dsp->ClassInstanceInfo(TJS_CII_SET_MISSING, 0, &missing);
	}

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/setCallMissing) /* UNDOCUMENTED: subject to change */
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getClassNames) /* UNDOCUMENTED: subject to change */
{
	// get class name as an array, last (most end) class first.
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	iTJSDispatch2 *dsp = param[0]->AsObjectNoAddRef();

	if(dsp)
	{
		iTJSDispatch2 * array =  TJSCreateArrayObject();
		try
		{
			tjs_uint num = 0;
			while(true)
			{
				tTJSVariant val;
				tjs_error err = dsp->ClassInstanceInfo(TJS_CII_GET, num, &val);
				if(TJS_FAILED(err)) break;
				array->PropSetByNum(TJS_MEMBERENSURE, num, &val, array);
				num ++;
			}
			if(result) *result = tTJSVariant(array, array);
		}
		catch(...)
		{
			array->Release();
			throw;
		}
		array->Release();
	}
	else
	{
		return TJS_E_FAIL;
	}

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getClassNames) /* UNDOCUMENTED: subject to change */
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(textEncoding)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = TVPScriptTextEncoding;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TVPScriptTextEncoding = *param;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(textEncoding)
//----------------------------------------------------------------------

	TJS_END_NATIVE_MEMBERS
}
//---------------------------------------------------------------------------
tTJSNativeInstance * tTJSNC_Scripts::CreateNativeInstance()
{
	// this class cannot create an instance
	TVPThrowExceptionMessage(TVPCannotCreateInstance);

	return NULL;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// TVPCreateNativeClass_Scripts
//---------------------------------------------------------------------------
tTJSNativeClass * TVPCreateNativeClass_Scripts()
{
	tTJSNC_Scripts *cls = new tTJSNC_Scripts();

	// setup some platform-specific members

//----------------------------------------------------------------------

// currently none

//----------------------------------------------------------------------
	return cls;
}
//---------------------------------------------------------------------------

