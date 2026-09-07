# KRKR-ns runtime compatibility patches

These TJS files replace same-named scripts from the mounted game archives on
Nintendo Switch.  They provide small, no-op shims for Windows-only plugin APIs
that cannot run on the console.

`build_nro.sh` deploys `system/` into the emulator SD card at:

`sdmc:/switch/krkrsdl2/patch/system/`

Keep the shims minimal.  A successful parse and startup path is preferable to
emulating Windows UI behavior that the Switch build cannot expose.

Historical shims that must no longer shadow game scripts live in `retired/`.
In particular, the old `motion.tjs` stub predates the built-in E-mote runtime;
deploying it disables the real motion setup and makes character sprites vanish.
