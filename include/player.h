// player.h : API de lecture d'un live (voir player.c pour les détails de synchro)
#pragma once
#include <citro2d.h>
#include "twitch.h"

typedef enum {
    PLAYER_IDLE,
    PLAYER_CONNECTING,   /* résolution token/usher/playlist */
    PLAYER_BUFFERING,
    PLAYER_PLAYING,
    PLAYER_ENDED,        /* stream terminé (ENDLIST / hors-ligne) */
    PLAYER_ERROR,
} PlayerState;

/* démarre la lecture. quality_pref ex "160p" (sinon la plus proche en dessous),
 * "audio_only" force l'audio. Rend la main tout de suite (état CONNECTING),
 * la résolution des URLs se fait dans le thread fetch. */
Result player_open(const char *login, const char *quality_pref, bool allow_video);
void   player_close(void);

PlayerState player_state(void);
const char *player_error_msg(void);   /* valide si PLAYER_ERROR */

// à appeler chaque frame (thread principal)
void player_update(void);

/* true + image si une frame vidéo est dispo */
bool player_has_video(void);
C2D_Image player_image(void);
void player_video_dimensions(int *w, int *h);

bool player_is_audio_only(void);
int  player_buffered_ms(void);
/* qualité réellement lue, ex "160p" */
const char *player_quality_name(void);
