// SPDX-License-Identifier: MIT
// KRKR-ns: reset hooks for E-mote state that outlives the script engine.
//
// The E-mote plugin caches objects built from script data in file-scope statics
// (see emoteplayerclass.cpp).  On a real console the process is chain-loaded, so
// those statics die with it; on hosts that restart the engine in-process they
// survive and point at objects belonging to the engine that was destroyed.
//
// Include this header and call the hooks from the engine teardown, before the
// script engine is shut down -- afterwards the referenced objects are gone.
#ifndef KRKRNS_EMOTEPLAYER_RESET_H
#define KRKRNS_EMOTEPLAYER_RESET_H

namespace emoteplayer
{
// Drops the cached SeparateLayerAdaptor built from the KAG window's poolLayer.
// It is created once behind a `== nullptr` guard and never rebuilt, so after an
// engine restart it still references a layer that died with the old engine --
// reached again by the next ResourceManager and faulting inside
// tTJSObjectProxy::PropGet -> tTJSCustomObject::Find.
void krkrsdl2_emote_reset_motion_work_layer();
}

namespace krkrsdl3
{
// Drops the render backend's cached SDL renderer handles.  The SDL renderer is
// destroyed with the old engine, and the allocator often reuses the same
// address, so the backend's own "renderer changed" check cannot see it.
void TVPResetRenderBackendForEngineRestart();
}

#endif // KRKRNS_EMOTEPLAYER_RESET_H
