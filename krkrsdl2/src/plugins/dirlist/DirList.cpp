// dirlist.dll: getDirList("dir/").
//
// Ported from Kirikiroid2 (src/plugins/dirlist.cpp).  The desktop plugin walks
// the native filesystem; here the folder is enumerated through the registered
// storage media, so an archive folder lists its members exactly like a native
// directory does.  Titles use it for save-slot folders and for scanning
// scenario/cg directories.
#include "tp_stub.h"
#include "ncbind/ncbind.hpp"
#include "StorageIntf.h"
#include <vector>

#define NCB_MODULE_NAME TJS_W("dirlist.dll")

class tGetDirListFunction : public tTJSDispatch
{
	tjs_error TJS_INTF_METHOD FuncCall(
		tjs_uint32 flag, const tjs_char * membername, tjs_uint32 *hint,
		tTJSVariant *result,
		tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis)
	{
		if(membername) return TJS_E_MEMBERNOTFOUND;
		if(numparams < 1) return TJS_E_BADPARAMCOUNT;

		ttstr dir(*param[0]);
		if(dir.GetLastChar() != TJS_W('/'))
			TVPThrowExceptionMessage(TJS_W("'/' must be specified at the end of given directory name."));

		class tLister : public iTVPStorageLister
		{
		public:
			std::vector<ttstr> list;
			void TJS_INTF_METHOD Add(const ttstr &file) { list.push_back(file); }
		} lister;

		try
		{
			TVPGetStorageListAt(dir, &lister);
		}
		catch(...)
		{
			// An unreadable/absent folder answers with an empty array, which is
			// what the desktop plugin's missing-directory case amounts to.
			lister.list.clear();
		}

		iTJSDispatch2 * array = TJSCreateArrayObject();
		if (!result) { array->Release(); return TJS_S_OK; }
		try
		{
			tTJSArrayNI* ni;
			array->NativeInstanceSupport(TJS_NIS_GETINSTANCE, TJSGetArrayClassID(), (iTJSNativeInstance**)&ni);
			for (size_t i = 0; i < lister.list.size(); ++i)
				ni->Items.emplace_back(lister.list[i]);
			*result = tTJSVariant(array, array);
			array->Release();
		}
		catch (...)
		{
			array->Release();
			throw;
		}
		return TJS_S_OK;
	}
} * GetDirListFunction;

static void PostRegistCallback()
{
	tTJSVariant val;
	iTJSDispatch2 * global = TVPGetScriptDispatch();

	GetDirListFunction = new tGetDirListFunction();
	val = tTJSVariant(GetDirListFunction);
	GetDirListFunction->Release();

	global->PropSet(TJS_MEMBERENSURE, TJS_W("getDirList"), NULL, &val, global);
	global->Release();
	val.Clear();
}
NCB_POST_REGIST_CALLBACK(PostRegistCallback);

/* Anchor: a static archive drops translation units that nothing references, and
   the only reference to this one is the ncbind auto-register table.  PluginImpl
   calls this function before ncbAutoRegister::LoadModule. */
extern "C" void krkrsdl2_link_dirlist_plugin() {}
