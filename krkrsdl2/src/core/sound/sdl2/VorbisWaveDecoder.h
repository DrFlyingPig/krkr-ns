#pragma once

// Register the built-in Ogg Vorbis decoder with Kirikiri's wave decoder
// manager.  The function is idempotent because every WaveSoundBuffer instance
// goes through the same construction path.
void TVPRegisterVorbisWaveDecoderCreator();

