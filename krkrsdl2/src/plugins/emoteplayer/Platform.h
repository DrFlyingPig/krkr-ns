#pragma once

// Compatibility bridge for the krkrsdl3 E-mote runtime.  The upstream
// parser reports diagnostics through TVPConsoleLog; KRKR-ns keeps those
// messages in the same SD-card debug log as the rest of the engine.
#include "KrkrNSLog.h"

#define TVPConsoleLog(...) KRKRNS_LOG("[emote] " __VA_ARGS__)



