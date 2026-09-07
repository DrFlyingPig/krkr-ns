#ifndef KRKRSDL2_CSV_PARSER_H
#define KRKRSDL2_CSV_PARSER_H

#include "tjsNative.h"

// Built-in form of the classic csvParser.dll plug-in. Switch homebrew cannot
// load the game's Win32 DLL, so expose the same TJS class from the engine.
tTJSNativeClass *TVPCreateNativeClass_CSVParser();

#endif
