# Switch SDL audio adapter

This directory replaces the audio bootstrap in the installed SDL 2.28.5 static
archive without modifying the SDK. The executable links the adapter object
directly; a common static library would be scanned before SDL requests the
bootstrap and could leave the original driver selected.

The devkitPro driver waited until a submitted hardware buffer was observed in
`Playing`. A renderer update can observe the same buffer already in `Done`, so
that wait never finishes. SDL then stops invoking the shared FAudio mixer. A
movie eventually fills its PCM ring and cannot advance its interleaved video
decoder. The original alternate wait could also use an invalid buffer index.

The adapter retains audren, native S16 output, sample rate, and two hardware
buffers. It copies PCM only into `Free` or `Done` buffers and waits for capacity
without requiring observation of the transient `Playing` state. Pending PCM is
preserved. Device shutdown and renderer errors leave the wait; renderer event
waits check shutdown every 50 ms. Five-second diagnostics report actual renderer
sample progress and completed buffers through the engine's existing log.

Upstream sources and licenses are identified in both source files:

- [devkitPro SDL backend, commit 0738d3c9](https://github.com/devkitPro/SDL/blob/0738d3c9f6993875e2f3dd0e8cc0bb4ae4440b4e/src/audio/switch/SDL_switchaudio.c).
- [SDL private audio ABI, version 2.28.5](https://github.com/libsdl-org/SDL/blob/15ead9a40d09a1eb9972215cceac2bf29c9b77f6/src/audio/SDL_sysaudio.h).

The private header retains the upstream declarations with public includes in
place of three internal dependencies. Its version gate and structure layout
assertions intentionally require the audited SDL 2.28.5 SDK. Re-audit the ABI
before upgrading this dependency. The repository's SDL 2.0.22 private headers
have a different driver layout and cannot replace this header.

Kirikiroid2's public movie implementation was compared at commit
`d1c2b1259423542c893e0b65eaeb46c848848f2b`: `AudioDevice.cpp` bounds audio writes
and supports abort; `AEFactory.cpp` bounds device-buffer waits and restarts an
inactive OpenAL source after submission. Those are useful lifecycle references,
but this fault belongs to the Switch SDL adapter, below either movie decoder.
This change therefore preserves the existing movie decoder, audio data, EOF
draining, and per-source BGM/movie controls.
