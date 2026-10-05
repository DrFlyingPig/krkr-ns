`ShrinkCopy.cpp` ports krkrsdl3's `plugins/shrinkCopy.cpp` to this engine.  The
original source is present in this workspace at
`.zcode/upstream-krkrsdl3/plugins/shrinkCopy.cpp`, and ten titles ship a guarded
call to each member.

| member | contract |
|---|---|
| `shrinkCopy(dx, dy, dw, dh, src, sx, sy, sw, sh)` | area-average resize of a source rectangle into a destination rectangle, with precomputed fixed point weight tables and edge weighting; refuses an enlargement |
| `shrinkCopyFast(src, stepx, stepy)` | in-place block shrink: the destination gets `ceil(width/stepx) x ceil(height/stepy)` and every destination pixel is the average of its `stepx x stepy` block, remainder blocks included.  Alpha is always written as 255 |

Both are used on live paths that the recorded sessions had not reached yet:
`sysscn/cgmode.tjs` calls `shrinkCopyFast` while building CG thumbnails (guarded
by `typeof .shrinkCopyFast == "Object"`, otherwise it falls back to a
`stretchCopy`), and `sysscn/PreRenderFontEx.tjs` calls the nine argument
`shrinkCopy` while resizing a pre-rendered font layer (guarded by
`typeof resizeLayer.shrinkCopy == "Object"`).  Because the guards are how those
titles degrade, implementing the members changes their path: the CG thumbnails
switch from `stretchCopy` to the intended block average, so this port keeps the
reference's arithmetic rather than approximating it.

The port keeps the reference implementation as it stands, with `TJS_W` in place
of krkrsdl3's narrow-string macro and the include block adjusted to this tree
(the file needs nothing beyond ncbind).  Nothing is cached across a session, so
`ncbAutoRegister::AllUnregist()` is the whole teardown.

Verification: `tests/fixtures/shrink_copy/startup.tjs` on the real ARM engine
(21 checks).  The block average is exact arithmetic, so the fixture computes the
expected pixels itself instead of reimplementing them on the host: 2x2 block
averages, remainder columns and rows, a vertical-only step, an oversized step,
the forced alpha 255, the destination size, and for the nine argument member a
uniform field, a 1:1 copy, alpha handling and the rejected enlargement.

The included `LICENSE.krkrsdl3` is copied unchanged from krkrsdl3's root license
and applies to this adaptation.
