// player.h: live playback API (see player.c for the sync details)
#pragma once
#include <citro2d.h>
#include "twitch.h"

typedef enum {
    PLAYER_IDLE,
    PLAYER_CONNECTING,   /* token/usher/playlist resolution */
    PLAYER_BUFFERING,
    PLAYER_PLAYING,
    PLAYER_ENDED,        /* stream ended (ENDLIST / offline) */
    PLAYER_ERROR,
} PlayerState;

/* starts playback. quality_pref e.g. "160p" (else the closest below),
 * "audio_only" forces audio. Returns immediately (CONNECTING state);
 * URL resolution happens in the fetch thread. */
Result player_open(const char *login, const char *quality_pref, bool allow_video);
void   player_close(void);

PlayerState player_state(void);
const char *player_error_msg(void);   /* valid when PLAYER_ERROR */

// call every frame (main thread)
void player_update(void);

/* true + image when a video frame is available */
bool player_has_video(void);
C2D_Image player_image(void);
void player_video_dimensions(int *w, int *h);

bool player_is_audio_only(void);
int  player_buffered_ms(void);
/* quality actually played, e.g. "160p" */
const char *player_quality_name(void);
