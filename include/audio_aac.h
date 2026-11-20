// audio_aac.h: AAC decoding + ndsp output.
// Audio is the master clock; video locks onto audio_clock_pts().
// Without dspfirm.cdc, audio_available() stays false but we still count
// PTS to keep a clock (osGetTime).
#pragma once
#include <3ds.h>
#include <stddef.h>

Result audio_init(void);
void   audio_exit(void);
bool   audio_available(void);     /* false = no dspfirm, silent */

void audio_start(void);           /* start: flush queues + clock */
void audio_stop(void);

/* PES payload: one or more back-to-back ADTS frames. pts90k = PTS of the 1st. */
void audio_feed(const u8 *data, size_t len, s64 pts90k);

// Refills the ndsp ring from the leftover already received. Call regularly
// from player_update, else the ring (~0.7 s) drains when the download pauses
// (prefetch cap) and the sound cuts out.
void audio_update(void);

/* playback clock in 90 kHz, -1 until something played */
s64 audio_clock_pts(void);

/* volume 0.0 - 1.0 */
void audio_set_volume(float v);

/* queued audio in ms, for the buffer gauge */
int audio_buffered_ms(void);
