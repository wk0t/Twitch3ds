# Twitch 3DS

Twitch homebrew client for **New Nintendo 3DS / New 2DS**: browse top streams,
search, **live video playback** (hardware H.264 decoding via MVD) and **built-in
chat** (read + send messages).

![Twitch 3DS](icon.png)

## Features

- **Top streams** and **search** for channels/games (Twitch GQL API, JPEG thumbnails)
- **Live stream playback**:
  - New 3DS/2DS: 160p/360p/480p video decoded by the hardware H.264 chip (MVD) + AAC audio
  - Old 3DS/2DS: audio-only mode (no MVD on those consoles)
- **Twitch chat** on the bottom screen: instant anonymous read, sending messages
  with an OAuth token (see below)
- UI adapted to both screens: video on top, touch chat/navigation below

## Installation

1. Console with **Homebrew Launcher** (Luma3DS + boot9strap recommended).
2. Copy `twitch3ds.3dsx` into `sd:/3ds/`.
3. **For sound**: the file `sd:/3ds/dspfirm.cdc` must exist. If missing, run the
   [DSP1](https://github.com/zoogie/DSP1) homebrew once ("Dump DSP firmware") —
   without it, video plays silent.
4. Launch *Twitch 3DS* from the Homebrew Launcher (Wi-Fi on).

## Controls

| Screen | Input | Action |
|---|---|---|
| List | D-Pad up/down | navigate |
| List | A | play the stream |
| List | X | refresh |
| List | Y | search (on-screen keyboard) |
| List | SELECT | sign in to the Twitch account |
| List | R | install the update (if available) |
| Player | B | back to the list |
| Player | SELECT | show/hide info (quality, buffer) |
| Player | "Write" button | send a chat message |
| Everywhere | START | quit (from the lists) |

## Signing in (to write in chat)

Reading the chat works without an account. To **write**, sign in directly from
the app, safely (Twitch's official "device" method):

1. On the home screen, press **SELECT**.
2. The app shows a **code** (e.g. `GNSLNMLF`).
3. On your phone or PC, go to **twitch.tv/activate**, sign in
   (with your **2FA as usual** — handled by Twitch, **never** by the app) and
   enter the code.
4. The app detects the authorization and saves the session: the
   "Write" button becomes active in the chat.

> Your password and 2FA **never** go through the console: login happens entirely
> on Twitch's website. The app only receives a chat-scoped token
> (`chat:read` + `chat:edit`), stored in `sd:/3ds/twitch3ds/config.ini`.

### Manual configuration (optional)

`sd:/3ds/twitch3ds/config.ini` can also be edited by hand:

```ini
username=YourTwitchName
oauth=oauth:xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
quality=160p
volume=100
```

`quality` accepts `160p`, `360p`, `480p` or `audio_only`.
On weak Wi-Fi, `160p` (the default) is the most reliable; `480p` needs a very
good connection and pushes the decoder hard.

## Updates

At startup, the app checks whether a newer version exists in the
[GitHub releases](https://github.com/wk0t/Twitch3ds/releases). If so, a green
"Update available" banner shows on the home screen — press **R** to download
and replace the `.3dsx` automatically, then restart the app.

You can also grab the latest version manually from the releases page.

## Building from source

Prerequisites: [devkitPro](https://devkitpro.org/wiki/Getting_Started) with
devkitARM, libctru, citro2d/citro3d and the portlibs `3ds-curl`, `3ds-mbedtls`,
`3ds-zlib`, `3ds-libjpeg-turbo`:

```bash
pacman -S 3ds-dev 3ds-curl 3ds-mbedtls 3ds-zlib 3ds-libjpeg-turbo
make
```

The `twitch3ds.3dsx` is produced at the repo root.

## Technical architecture

| Module | Role |
|---|---|
| `source/net_http.c` | HTTPS via libcurl + mbedtls (Mozilla CA bundle embedded in romfs) |
| `source/twitch.c` | unofficial GQL API (web player's public client-id) + usher HLS |
| `source/hls.c` | live media playlist parser |
| `source/ts_demux.c` | MPEG-TS demuxer (dynamic PAT/PMT, PES -> H.264 AU + ADTS AAC) |
| `source/video_mvd.c` | hardware H.264 decoding MVDSTD -> citro2d texture (New 3DS) |
| `source/audio_aac.c` | AAC-LC decoding (Helix, fixed-point ARM) -> ndsp; master A/V clock |
| `source/player.c` | orchestration: download thread (core 2 of the New 3DS) + sync on the audio clock |
| `source/irc.c` | IRC chat (IRCv3 tags, PING/PONG, reconnect) |
| `source/ui/` | citro2d scenes (home, search, player) |

## Troubleshooting

| What you see | Why | What to do |
|---|---|---|
| "Network error (...)" on the home screen | Wi-Fi off or network issue | Check that Wi-Fi is on. The code in parentheses helps (see below) |
| Error mentioning `SSL` / `certificate` | **Console date/time is wrong** -> TLS certificate rejected | Fix the date in *System Settings -> Other Settings -> Date and Time* |
| Mute video (picture OK, no sound) | `sd:/3ds/dspfirm.cdc` missing | Run [DSP1](https://github.com/zoogie/DSP1) once to dump the DSP firmware |
| No picture, just the name + "Playing..." | Console without the MVD chip (Old 3DS / 2DS), or MVD unavailable -> audio-only mode | Normal on Old 3DS/2DS. On New 3DS/2DS, retry |
| Freezes or cuts after a few seconds | Wi-Fi too slow for the chosen quality | Switch to `160p` (`quality=160p` in `config.ini`) |
| Stuttering picture / video lagging behind audio | 480p too heavy for the connection / console | Pick `160p` or `360p` |
| Chat shows "Connecting to chat..." then "Disconnected" in a loop | The network blocks port 6667 (IRC) — rare, mostly on mobile hotspot | Try another Wi-Fi |
| "Read-only" button in the chat | Not signed in, or token expired | **SELECT** on the home screen to (re)sign in |
| "Stream ended / offline" | The channel is not live | Pick a stream that is actually live |
| Login: "Code expired" | Took too long to authorize on twitch.tv/activate | Press **SELECT** again and authorize faster |
| Login: "login not found" | Network issue while validating the token | Retry |
| Update: "Update download failed" | Network cut during the download | Retry, or download the `.3dsx` manually from the [Releases](https://github.com/wk0t/Twitch3ds/releases) page |
| Nothing displays / invisible text (emulator) | Missing system files (font) | Fine on real hardware; on an emulator you must provide the system files. Video does **not** work on emulators (MVD is not emulated) |

### Decoding the "Network error (...)" code

- **-6 / -7** -> DNS or connection impossible (Wi-Fi?)
- **-28** -> timeout (slow network)
- **-35 / -60** -> TLS / certificate problem (most often: **console date** to fix)
- **-403 / -429** -> Twitch refused the request (retry a bit later)

## Known limitations

- Chat emotes are shown as text (no emote images).
- VODs and clips are not supported (live only).
- On Old 3DS/2DS: audio only.
- The first connection to a channel takes a few seconds (token + playlist + buffer).

## Credits

- AAC decoder: [Helix AAC](https://en.wikipedia.org/wiki/Helix_Universal_Server) (RealNetworks, RPSL)
  via [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio)
- JSON: [cJSON](https://github.com/DaveGamble/cJSON) (MIT)
- CA: Mozilla bundle via [curl.se](https://curl.se/docs/caextract.html)
- Toolchain: [devkitPro](https://devkitpro.org) (libctru, citro2d, portlibs)

## New 3DS performance

To make the most of the New 3DS/2DS, the app:
- enables **804 MHz** mode (`osSetSpeedupEnable`);
- decodes H.264 video on the **hardware MVD chip** (no video CPU load);
- places the heavy thread (network + demux + AAC decoding) on the New 3DS's
  exclusive **core 2**, leaving the application core to the 60 fps UI.

Not affiliated with Twitch/Amazon or Nintendo.
