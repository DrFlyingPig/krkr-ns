`LayerExSave.cpp` implements the `layerExSave.dll` surface that the titles in
the test corpus reach.  Unlike the other ports in this directory there is **no
reference source**: krkrsdl3 ships a zero byte placeholder, and no other tree in
the workspace contains these members.  The contract therefore comes from two
places, and both are recorded here:

- the 1.3.9 register table, extracted from the IPA binary: the class
  `WindowSaveImage` on `Window` plus the `Layer` members `getCropRectZero`,
  `getDiffRect`, `getDiffPixel`, `oozeColor`, `copyBlueToAlpha`, `isBlank`,
  `clearAlpha`, `saveLayerImageTlg5`, `saveLayerImagePng`,
  `saveLayerImagePngOctet` and the error strings ("Different layer size.",
  "invalid layer range", "deflate initialize", …);
- the call sites in the titles' own system scripts, which is what decided the
  scope: ten titles link the plugin and then call these members, several of them
  **unguarded** right after linking (`system/BMPBaseAffineSourceLayer.tjs` calls
  `clearAlpha` and `oozeColor`, `sysscn/exroll.tjs` calls `getCropRect` and
  `oozeColor`), so a partial implementation would push those titles onto paths
  that throw.  That is why this lands as one piece.

| member | used by | contract |
|---|---|---|
| `saveLayerImagePng(name)` | MainWindow.tjs, psdlayer.tjs, standview.tjs, debugutil.tjs, exchview.tjs | save the layer as PNG |
| `saveLayerImageTlg5(name)` | psdlayer.tjs | save the layer as TLG5 |
| `getCropRect()` / `getCropRectZero()` | world.tjs, exroll.tjs, psdlayer.tjs, BMPBaseAffineSourceLayer.tjs | `%[x, y, w, h]` bounding box of the non transparent area, void when the layer is empty |
| `getDiffRect(other)` | StandAffineSourceLayer.tjs | bounding box of the pixels that differ from another layer of the same size, void when identical, throws "Different layer size." on a size mismatch |
| `isBlank(l, t, w, h)` | psdlayer.tjs | true when every alpha inside the (clipped) rect is zero |
| `copyBlueToAlpha(src)` | psdlayer.tjs, world.tjs | `dst.alpha = src.blue` per pixel, same size required |
| `clearAlpha()` | BMPBaseAffineSourceLayer.tjs | alpha 0 for every pixel, colour untouched |
| `oozeColor(level)` | exroll.tjs, StandAffineSourceLayer.tjs | spread colour outwards into fully transparent pixels, `level` passes, alpha untouched |

Not implemented, because no title in the corpus calls them (zero hits in the
bytecode scan): `WindowSaveImage`'s `startSaveLayerImage` /
`cancelSaveLayerImage` / `stopSaveLayerImage`, `getDiffPixel`,
`saveLayerImagePngOctet`, `primaryLayer`, and the `onSaveLayerImageProgress` /
`onSaveLayerImageDone` callbacks.  A title that needs them would show up as a
missing member in a log first.

**Two members are inferred rather than documented**, and are the ones to look at
if a title ever renders differently:

- `oozeColor` has no description anywhere in this workspace.  This port defines
  it as `level` dilation passes, each copying the average of the opaque
  orthogonal neighbours into a transparent pixel that has one, leaving alpha
  alone.  That matches the intent ("do the extension processing" before saving,
  so scaling does not pull fringe colours out of the empty area) and the
  observed `oozeColor(4)` call, but the original's exact rule is unknown.
- `getCropRect` is registered here as an alias of `getCropRectZero`: the 1.3.9
  table places a `getCropRect` name near the `Window` group, yet every title
  calls it on a **Layer** (`(global.Layer.getCropRect incontextof layer)()`),
  and both call sites use it as a "has content" test, which is what the zero
  variant returns.

The savers reuse the core method (`layer.saveLayerImage(name, "png" | "tlg5")`)
so the engine's own meta dictionary handling (`offs_x` / `offs_y`) stays in one
place.  The original `layerExSave.dll` ships in the game folders
(`<game>/plugin/layerExSave.dll`) and is available if a member ever needs a
pixel level comparison.

Verification: `tests/fixtures/layer_ex_save/startup.tjs` on the real ARM engine,
34 checks: registration of all nine members, the crop and diff rectangles, the
blank test including rect clipping, both alpha helpers, the colour spread, and
both savers actually writing a file with content.

In-game: 国王恋爱krkr links the plugin (its `plugin/` folder ships the desktop
DLL and its scripts call these members unguarded) and its CG mode renders
normally in the emulator, with no member error in the session log
(2026-10-05, user confirmed).
