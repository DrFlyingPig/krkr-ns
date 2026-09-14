// win32dialog.dll: WIN32Dialog.messageBox().
//
// Ported from Kirikiroid2 (src/plugins/win32dialog.cpp).  The desktop plugin
// shows a native message box; here the engine's own System.inform is used, and
// its return value is inverted to match the Win32 IDYES/IDNO answer the titles
// compare against.
//
// The compat layer also installs a script-level WIN32Dialog
// (compat-patches/system/win32dialog.tjs) for titles that assume the class is
// already there before linking anything; this module keeps the probe honest.
#include "ncbind/ncbind.hpp"

#define NCB_MODULE_NAME TJS_W("win32dialog.dll")

static void InitPlugin_WIN32Dialog()
{
	TVPExecuteScript(
		TJS_W("class WIN32Dialog {")
		TJS_W("	function messageBox(message, caption, type) {return !System.inform(message, caption, 2);}")
		TJS_W("}")
		);
}

NCB_PRE_REGIST_CALLBACK(InitPlugin_WIN32Dialog);

/* Anchor: a static archive drops translation units that nothing references, and
   the only reference to this one is the ncbind auto-register table.  PluginImpl
   calls this function before ncbAutoRegister::LoadModule. */
extern "C" void krkrsdl2_link_win32dialog_plugin() {}
