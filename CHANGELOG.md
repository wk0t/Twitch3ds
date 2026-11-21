# Changelog

## 1.0

First version that runs on New 3DS / New 2DS: browsing, search,
H.264 video playback (MVD), AAC audio, chat (read + send), account login.

### Dev notes / pitfalls encountered

- **GQL lists**: `streams(first:)` and `searchStreams(first:)` cap at 30
  on Twitch's side (beyond that: `argument 'first'...` error). Hence the cap at 30.
- **Video (MVD)**: `mvdstdRenderVideoFrame` returns INVALIDARG if the output dims
  are not 16-aligned and equal to the input. Fix: 16-align the output, let MVD
  write into a compact buffer, then copy to a pow2-strided buffer for the
  citro2d texture.
- **Audio**: the ndsp ring (~0.7 s) must be refilled continuously, not only
  while the network thread runs — else the sound cuts after ~1 s as soon as the
  download pauses. Hence `audio_update()` called every frame.
- **Login**: the username is fetched via `id.twitch.tv/oauth2/validate` (works
  with any token), not via Helix (which requires a registered client-id).
- **Chat (IRC)**: a non-blocking `connect` + `select()` does not detect connection
  completion on the 3DS SOC. Switched to a blocking `connect`.

### Known limitations

- Chat emotes are shown as text, not images.
- Live only (no VODs / clips).
- Old 3DS / 2DS: audio only (no MVD).
