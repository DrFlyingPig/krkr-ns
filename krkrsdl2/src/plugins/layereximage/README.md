`LayerExImage.cpp` and `LayerExImageAlgo.h` adapt krkrsdl3's
`plugins/LayerExImage.cpp` to krkrsdl2's CPU bitmap access.  The original source
is present in this workspace at `.zcode/upstream-krkrsdl3/plugins/LayerExImage.cpp`,
and the same plugin ships with several titles under
`sdmc:/switch/KRKR-ns/Game/*/plugin/layerExImage.dll` (the D3D/GL desktop builds);
the 1.3.9 Kirikiroid2 build registers the same six members.

Registered on the `Layer` class when a title links `layerExImage.dll`:

| member | contract |
|---|---|
| `light(brightness, contrast)` | 256 entry LUT over B/G/R; alpha untouched |
| `colorize(hue, saturation, blend)` | hue/saturation replacement with an 8.8 fixed point blend |
| `modulate(hue, saturation, luminance)` | double precision HSL shift in place |
| `noise(level)` | uniform +/- level/2 per channel, clamped |
| `generateWhiteNoise()` | one random grey value for B/G/R, alpha kept |
| `gaussianBlur(amount)` | separable Gaussian over all four channels; see the note below |

The arithmetic, clamping, truncation and channel order follow the reference
source exactly; `LayerExImageAlgo.h` carries the CxImage attribution the
operations derive from.  The LUT pass is the one place that is *not* a literal
transcription: the upstream `*p++ = pLut[*p]` reads `*p` unsequenced against the
`p++` side effect, so this port writes the three per channel assignments the
upstream expression means.  That matters here because the Switch toolchain is
clang, which is free to pick the other order.

Two deliberate deviations:

- **Clip rectangle.**  The reference works on the layer's clip rectangle
  (`clipLeft/clipTop/clipWidth/clipHeight`) and never validates it.  This port
  reads the same rectangle but clamps it to the image, so a degenerate or
  out-of-range clip cannot walk the buffer.  A layer without clip properties
  falls back to the whole image.  The fixture covers both halves of the
  behaviour (inside processed, outside untouched).
- **`gaussianBlur`** is implemented from a documented contract, not from the
  original's source.  KAG calls it through the image attribute `gblur` with one
  numeric argument (`system/KAGEnvImage.tjs`:
  `if (.gblur) list.add(["gaussianBlur", +.gblur]);`), and the 1.3.9 binary plus
  the 2013 `layerExImage.dll` shipped with 国王恋爱krkr register it, while the
  2018 build shipped with 双子洛丽塔 does not.  No local source shows the
  original's kernel, so this port fixes its own mapping and says so: the argument
  is a radius in pixels, sigma is radius / 2, the kernel spans +/- radius, the
  border replicates the edge pixel, radius is capped at 64, and alpha is blurred
  along with the colour.  The last point follows the engine's own blur, which is
  what KAG's sibling attribute blurx/blury uses: `doBoxBlur` defaults to
  `DoBoxBlurForAlpha`, a plain per channel filter that includes alpha.  Cost is
  O(width * height * radius * 4), and KAG marks `gaussianBlur` as needing a full
  redraw, so a title that sets it pays this on every redraw of that layer.
  Not byte-verified against the original DLL; the 2013 build in the game folders
  is available if that comparison is ever needed.

How KAG reaches these members: `world.tjs` looks them up with
`typeof Layer[name] == "Object"` followed by `func instanceof "Function"`.
A probe in the fixture records that the ncbind-attached members satisfy both
(`Layer.light typeof=Object instanceofFunction=1`), the same answer the
engine's own `Layer.fillRect` gives, so the image-filter pipeline really does
call into this plugin.

Nothing in this port caches TJS objects across a session, so no extra teardown
is needed beyond `ncbAutoRegister::AllUnregist()`.

The included `LICENSE.krkrsdl3` is copied unchanged from krkrsdl3's root license
and applies to this adaptation.

Verification: `tests/layer_ex_image_test.cpp` (host, 233 checks over the pixel
algorithms, stride handling and a negative pitch) and
`tests/fixtures/layer_ex_image/startup.tjs` (real ARM engine, 33 checks: file
probe, registration, `Plugins.getList`, repeated/case-insensitive link, the six
attachments, each operation's pixels and alpha, the clip rectangle, the KAG
`instanceof "Function"` probe, and the blur's fixed point, symmetry, finite
support and alpha spread).

In-game usage: scanning the XP3 script bytecode of the titles that link this
plugin (`tools/xp3_find_symbol.py`) confirms 魔女的夜宴, 千恋万花 v1.1 and 9-nine
feed `light` / `modulate` / `noise` entries through KAG's image-attribute list,
so those three members are on a live path.  Five recorded sessions with the
plugin loaded show no member-related script exception.
