#ifndef KRKRSDL2_TEXT_RENDER_BASE_H
#define KRKRSDL2_TEXT_RENDER_BASE_H

#include "tjsNative.h"

// Portable built-in replacement for textrender.dll's TextRenderBase class.
// The game's TextRender.tjs remains untouched and derives from this class in
// exactly the same way it does on Windows.
tTJSNativeClass *TVPCreateNativeClass_TextRenderBase();

#endif
