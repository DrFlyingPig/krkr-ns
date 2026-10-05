# PlutoVG in this tree

Upstream: https://github.com/sammycage/plutovg, commit `f63f9b5` (the tree was
cloned on 2026-10-05; upstream has no tagged releases, so the commit is the
pin).  `LICENSE` (MIT, for PlutoVG itself) and `source/FTL.TXT` (the FreeType
licence, for the font rasteriser it embeds) are the upstream files.

Why it is here: `layerExDraw.dll` — the GDI+ emulation the KAG roll renderer
uses for outlined text — is built on PlutoVG in its only portable reference
implementation (krkrsdl3/plugins/LayerExDraw).  That reference does not vendor
the library, so the port has to.

## Local changes

One patch, in `source/plutovg-font.c`:

- `sys/mman.h` is not available on libnx, so the include and the POSIX
  `plutovg_mmap` are guarded by `__SWITCH__`; the Switch branch reads the font
  file into a `malloc`ed buffer instead and `plutovg_unmap` frees it.  Callers
  only read the mapping, and `plutovg_font_face_load_from_file` passes the
  pointer to the same parsing code, so the behaviour is unchanged.  (The
  reference plugin itself loads faces with
  `plutovg_font_face_load_from_data`, which does not touch this path.)

The library is otherwise untouched.  Its embedded FreeType is prefixed `PVG_FT_`
and therefore does not collide with the engine's own FreeType.
