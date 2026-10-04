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
- **`gaussianBlur`** is *not* implemented.  The 1.3.9 binary and the 2013
  `layerExImage.dll` shipped with 国王恋爱krkr register it, but the 2018 build
  shipped with 双子洛丽塔 does not, and no local reference source shows its
  parameter list.  Its contract is recorded in `docs/K2_PLUGIN_CONTRACTS.md`
  instead of guessing a signature.

Nothing in this port caches TJS objects across a session, so no extra teardown
is needed beyond `ncbAutoRegister::AllUnregist()`.

The included `LICENSE.krkrsdl3` is copied unchanged from krkrsdl3's root license
and applies to this adaptation.

Verification: `tests/layer_ex_image_test.cpp` (host, 182 checks over the pixel
algorithms and stride handling) and `tests/fixtures/layer_ex_image/startup.tjs`
(real ARM engine, 25 checks: file probe, registration, `Plugins.getList`,
repeated/case-insensitive link, the five attachments, each operation's pixels
and alpha, and the clip rectangle).
