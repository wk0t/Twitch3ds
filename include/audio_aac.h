// audio_aac.h : décodage AAC + sortie ndsp.
// L'audio sert d'horloge maîtresse, la vidéo se cale sur audio_clock_pts().
// Sans dspfirm.cdc, audio_available() reste false mais on compte quand même
// les PTS pour garder une horloge (osGetTime).
#pragma once
#include <3ds.h>
#include <stddef.h>

Result audio_init(void);
void   audio_exit(void);
bool   audio_available(void);     /* false = pas de dspfirm, son muet */

void audio_start(void);           /* démarre : purge files + horloge */
void audio_stop(void);

/* Payload PES : une ou plusieurs trames ADTS collées. pts90k = PTS de la 1re. */
void audio_feed(const u8 *data, size_t len, s64 pts90k);

// Recharge l'anneau ndsp depuis le reliquat déjà reçu. À appeler régulièrement
// depuis player_update, sinon l'anneau (~0,7 s) se vide quand le download se met
// en pause (plafond de préchargement) et le son coupe.
void audio_update(void);

/* horloge de lecture en 90 kHz, -1 tant que rien n'a joué */
s64 audio_clock_pts(void);

/* volume 0.0 - 1.0 */
void audio_set_volume(float v);

/* ms d'audio en file, pour la jauge de buffer */
int audio_buffered_ms(void);
