`LayerExDraw.cpp`, `LayerExDraw.hpp`, `DrawBackend.h`, `LayerExBase.{h,cpp,hpp}` port
krkrsdl3's `plugins/LayerExDraw` — the GDI+ emulation the KAG roll renderer
(`sysscn/exroll.tjs`) uses to draw outlined text.  See `LICENSE.krkrsdl3`.

## Why this needed a vendored library

The reference builds its rasteriser on **PlutoVG**, and neither the reference
tree nor this one carried the library.  It is now vendored under
`krkrsdl2/external/plutovg/` (upstream `f63f9b5`, MIT plus the FreeType licence
for the rasteriser it embeds) with one local patch: libnx has no `sys/mman.h`,
so font files are read into memory on Switch instead of being mapped.  See that
directory's `KRKRNS-PATCH.md`.

## Adaptations to this tree

| reference | here |
|---|---|
| `#include "PlatformFile.h"` | dropped: none of its symbols are used by the plugin |
| `#include "TVPStorage.h"` | `StorageIntf.h` + `BinaryStream.h` |
| `#include "TVPScript.h"` / `"TVPMsg.h"` / `"TVPSystem.h"` | `ScriptMgnIntf.h` / `MsgIntf.h` + `DebugIntf.h` / `SystemIntf.h` |
| `#include "TVPFont.h"` (font enumeration table) | `TVPSysFont.h`; this tree's `TVPGetAllFontList` is an empty stub, so `GdiPlus.getFontList` only reports the faces `addPrivateFont` loaded |
| `#include "tjsNativeLayer.h"` / `"RenderManager.h"` | `LayerIntf.h`; `TVPIsSoftwareRenderManager()` is replaced by `true` because this port reaches the layer's main image directly |
| `TVPCreateFontStream`, `GetResourceStream` | local helpers that resolve through the storage system; the fallback font is the romfs `notosanssc.ttf` this port ships |
| `plutovg` paths | `ToUtf8Path()` converts the UTF-16 TJS string, as plutovg takes UTF-8 |

## State of the reference

The text path the titles actually call is **implemented** upstream
(`measureString` uses `plutovg_font_face_text_extents`, `drawPathString` and
`drawString` iterate codepoints and build glyph paths, `measureStringInternal`
forwards to `measureString`).  These are stubs in the reference and stay stubs
here, recorded so nobody mistakes them for working code:

- `drawPathString2`, `measureString2`, `measureStringInternal2` (they return an
  empty `RectF()`; the non-`2` variants are the ones the register table exposes),
- `getGlyphOutline`, `getTextOutline` (`// TODO`; the working paths build their
  paths inline instead),
- `updateWhenDraw` is a C++ field of `LayerExDraw`, not a script member.

## Verification

`tests/fixtures/layer_ex_draw/startup.tjs` on the real ARM engine, 20 checks,
all passing: the file probe, the registration, the `GdiPlus` class, twelve
`Layer` members, the private font load, `measureString`, and then the pixels —
`drawPathString` writes 202 lit pixels for two glyphs at 24 px, the brush colour
reaches the layer (logged as a colour histogram, mostly the white pen with grey
antialiasing plus the green fill), and `drawLine` adds more.

Four things the fixture had to learn about this API, worth knowing before
writing another one:

- `RectF` exposes `x`/`y`/`width`/`height` (plus read-only `left`/`top`/…), not
  `w`/`h`; the rects `layerExSave` returns are dictionaries and do use `w`/`h`.
- ncbind registers the C++ arity: `Appearance.addBrush(color, ox, oy)` and
  `addPen(color, width, ox, oy)` need every argument even though the C++ side
  declares defaults, and `drawLine(app, x1, y1, x2, y2)` takes the appearance
  first.
- `Layer.record` is a property with a getter: reading it off the *class* object
  builds the layer helper and fails with "Not Layer", so probe the plain
  methods instead.
- The fixture is a plain TJS script, so no `for (key in object)` and no
  `Integer.toString(16)`; use `Dictionary.keys`/explicit key lists and
  `"%06x".sprintf(value)`.
