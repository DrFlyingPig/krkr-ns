// fftgraph.dll: drawFFTGraph().
//
// Ported from Kirikiroid2 (src/plugins/fftgraph.cpp).  The original renders a
// spectrum graph in a debug window; on this host it is a no-op stub, which is
// what the plugin is for titles too: they only need the symbol to exist so
// their debug/visualiser scripts load.
#include "ncbind/ncbind.hpp"

#define NCB_MODULE_NAME TJS_W("fftgraph.dll")

static void InitPlugin()
{
	TVPExecuteScript(TJS_W("function drawFFTGraph(){}"));
}

NCB_PRE_REGIST_CALLBACK(InitPlugin);

/* Anchor: a static archive drops translation units that nothing references, and
   the only reference to this one is the ncbind auto-register table.  PluginImpl
   calls this function before ncbAutoRegister::LoadModule. */
extern "C" void krkrsdl2_link_fftgraph_plugin() {}
