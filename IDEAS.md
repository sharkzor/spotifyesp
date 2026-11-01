# Suggested features

Ideas that are not implemented yet.

## Fixed line-level volume

Add `CONFIG_SPOTIFY_FIXED_VOLUME` in `main/Kconfig.projbuild`. When set, ignore the
`VOLUME` event in `main/SpotifyPlayer.cpp` so the output stays at 65535 (100%).

Why: with the board feeding an amp, volume should be set on the amp. The software
volume in `main/I2SAudioSink.cpp` is linear and loses bit depth at lower settings,
and the Spotify slider would otherwise change the line level.

Note: the PCM5102A outputs about 2.1 Vrms at full scale, so check the amp's input
sensitivity.
