// ts_demux.h : petit démux MPEG-TS pour les segments HLS Twitch.
// PAT/PMT lus dynamiquement, ne jamais supposer les PID fixes.
// Callbacks : vidéo = une AU complète (Annex-B) + PTS 90k, audio = payload
// PES entier (trames ADTS) + PTS 90k. Une seule instance, état statique.
#pragma once
#include <3ds.h>
#include <stddef.h>

typedef void (*TsVideoCb)(const u8 *au,   size_t len, s64 pts90k, void *user);
typedef void (*TsAudioCb)(const u8 *adts, size_t len, s64 pts90k, void *user);

void ts_demux_init(TsVideoCb vcb, TsAudioCb acb, void *user);
// remet à zéro l'état PES/PAT/PMT (changement de stream, discontinuité)
void ts_demux_reset(void);
// tolère n'importe quel découpage (paquets à cheval entre deux appels)
int  ts_demux_feed(const u8 *data, size_t len);
void ts_demux_flush(void);
void ts_demux_exit(void);
