`LayerExRaster.cpp` and `RasterCopy.h` adapt the complete public
`Layer.copyRaster(layer, maxh, lines, cycle, time)` API from
krkrsdl3's `plugins/LayerExRaster.cpp` to krkrsdl2's CPU bitmap access.
The original source is present in this workspace at
`.zcode/upstream-krkrsdl3/plugins/LayerExRaster.cpp`.

The sine phase, integer `height / 2`, displacement truncation, full 32-bit
pixel copy, size-mismatch no-op, and unchanged uncovered edges follow that
source. Zero divisors and shifts beyond a row are guarded; overlapping
in-place copies use `memmove`. The caller still requests `Layer.update()`.

The included `LICENSE.krkrsdl3` is copied unchanged from krkrsdl3's root
license and applies to this adaptation. No proprietary game DLL source is
used.
