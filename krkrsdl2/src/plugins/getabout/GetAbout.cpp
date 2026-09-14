// getabout.dll: System.getAboutString().
//
// Ported from Kirikiroid2 (src/plugins/getabout.cpp), which attaches the
// engine's about string to the System class.  Titles show it in their
// "about/version" screens and several probe System.getAboutString before
// enabling that screen at all.
#include "ncbind/ncbind.hpp"
#include "MsgIntf.h"

#define NCB_MODULE_NAME TJS_W("getabout.dll")
NCB_ATTACH_FUNCTION(getAboutString, System, TVPGetAboutString);

/* Anchor: a static archive drops translation units that nothing references, and
   the only reference to this one is the ncbind auto-register table.  PluginImpl
   calls this function before ncbAutoRegister::LoadModule. */
extern "C" void krkrsdl2_link_getabout_plugin() {}
