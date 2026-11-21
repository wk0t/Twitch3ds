// ts_demux.h: small MPEG-TS demux for Twitch HLS segments.
// PAT/PMT read dynamically, never assume fixed PIDs.
// Callbacks: video = one complete AU (Annex-B) + 90k PTS, audio = whole
// PES payload (ADTS frames) + 90k PTS. Single instance, static state.
#pragma once
#include <3ds.h>
#include <stddef.h>

typedef void (*TsVideoCb)(const u8 *au,   size_t len, s64 pts90k, void *user);
typedef void (*TsAudioCb)(const u8 *adts, size_t len, s64 pts90k, void *user);

void ts_demux_init(TsVideoCb vcb, TsAudioCb acb, void *user);
// resets PES/PAT/PMT state (stream change, discontinuity)
void ts_demux_reset(void);
// tolerates any chunking (packets straddling two calls)
int  ts_demux_feed(const u8 *data, size_t len);
void ts_demux_flush(void);
void ts_demux_exit(void);
