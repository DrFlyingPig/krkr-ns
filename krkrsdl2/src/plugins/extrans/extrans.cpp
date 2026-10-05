// extrans.dll: the transition providers Kirikiroid2 compiles in from the Win32
// extrans plug-in.  KAG names them from scenario tags (`@trans method=wave`),
// from a title's own transition table, or from the load/auto-save screens, so a
// missing name is a visible difference: the engine falls back to crossfade.
//
// Ported from krkrsdl3/plugins/extrans; see LICENSE.krkrsdl3.  Stage 1 ships the
// provider framework plus wave and mosaic; turn, the rotate family and ripple
// follow in their own commits, so the available names grow with the port.  The
// titles that reference these names only link the plug-in and request the
// method, so an incomplete set degrades to the crossfade fallback in
// TransIntf.cpp rather than failing.
#include "ncbind/ncbind.hpp"

#include "tjsCommHead.h"
#include "transhandler.h"

#include "wave.h"
#include "mosaic.h"
#include "turn.h"
#include "rotatetrans.h"
#include "ripple.h"

#define NCB_MODULE_NAME TJS_W("extrans.dll")

void InitPlugin_extrans()
{
    RegisterWaveTransHandlerProvider();
    RegisterMosaicTransHandlerProvider();
    RegisterTurnTransHandlerProvider();
    RegisterRotateTransHandlerProvider();
    RegisterRippleTransHandlerProvider();
}

void DonePlugin_extrans()
{
    UnregisterWaveTransHandlerProvider();
    UnregisterMosaicTransHandlerProvider();
    UnregisterTurnTransHandlerProvider();
    UnregisterRotateTransHandlerProvider();
    UnregisterRippleTransHandlerProvider();
}

NCB_PRE_REGIST_CALLBACK(InitPlugin_extrans);
NCB_POST_UNREGIST_CALLBACK(DonePlugin_extrans);

// krkrsdl2common is a static archive: the registrations above only exist as
// static initializers, so the linker would drop this unit without an anchor.
// PluginImpl calls this before ncbAutoRegister::LoadModule.
extern "C" void krkrsdl2_link_extrans_plugin()
{
}
