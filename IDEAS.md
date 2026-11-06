# Suggested features

Remaining work and ideas that are not implemented yet.

## Validation before more playback changes

- [ ] Verify reconnect during active playback using a controlled interruption
  affecting only this board. Two idle reconnects succeeded without a panic, but
  audio resumption after a reconnect has not been verified.
- [ ] Run longer normal-use playback sessions. The PSRAM buffer change and startup
  task cleanup each completed a 30-minute capture with audio through the end.
  After both changes, the final capture showed about 64 KB free internal RAM,
  with a sampled minimum of about 58 KB.

Keep WiFi buffer counts, PCM queue capacity and playback-task stacks unchanged.
Implement and validate future changes individually.

## Remember Spotify login

- [x] Implemented on the `improvements` branch: reusable credentials are saved in
  NVS (unencrypted), a rejected login is erased, and the portal has a "Forget the
  saved Spotify login" checkbox. Hardware-verified: first login saves, and a reboot signs in
  without the phone. Still to check: playback after that reboot and the forget
  checkbox. NVS access runs on an internal-stack helper task, because flash access
  from the PSRAM-stack receiver task asserts.
- [ ] Optional: enable NVS/flash encryption to protect the stored credentials.

## Fixed line-level volume

Add `CONFIG_SPOTIFY_FIXED_VOLUME` in `main/Kconfig.projbuild`. When set, ignore the
`VOLUME` event in `main/SpotifyPlayer.cpp` so the output stays at 65535 (100%).

Why: with the board feeding an amp, volume should be set on the amp. The software
volume in `main/I2SAudioSink.cpp` is linear and loses bit depth at lower settings,
and the Spotify slider would otherwise change the line level.

Note: the PCM5102A outputs about 2.1 Vrms at full scale, so check the amp's input
sensitivity.

## Later playback improvements

- [ ] Optional track/album loudness normalization with clipping protection.
- [ ] PSRAM-backed read-ahead and bounded retry/resume for recoverable stream
  failures, rather than immediately skipping the track.
- [ ] Improve gapless transitions by preparing the next audio stream earlier.
- [ ] Review newer Spotify metadata endpoints and receiver-side queue handling,
  using librespot as a reference. Consider shuffle/repeat and autoplay separately.

These are candidates, not promises of compatibility or small changes. A full
librespot port and Spotify lossless support are not planned.

## Done on `improvements`

- Track restrictions now ignore rules aimed at other account types (catalogues), as
  librespot does. Fixed a Premium track that was wrongly reported as unavailable.
