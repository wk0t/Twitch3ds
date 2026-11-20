// twitch.h: unofficial Twitch API (GQL + usher). impl in twitch.c
#pragma once
#include "net_http.h"

#define TWITCH_CLIENT_ID "kimne78kx3ncx6brgo4mv6wki5h1ko"
#define TWITCH_GQL_URL   "https://gql.twitch.tv/gql"

#define TW_MAX_QUALITIES 12

typedef struct {
    char login[64];        /* lowercase */
    char display[64];
    char title[192];       /* UTF-8, cleanly truncated */
    char game[96];
    char preview_url[256]; /* 320x180 thumbnail */
    int  viewers;
} TwitchStream;

typedef struct {
    char name[24];   /* e.g. "480p", "audio_only", "1080p60 (source)" */
    char url[2048];
    int  width, height;
    int  bandwidth;
    bool audio_only;
} TwitchQuality;

/* Returns: number of items written, or negative on error. */
int twitch_top_streams(HttpCtx *c, TwitchStream *out, int max);
int twitch_search_streams(HttpCtx *c, const char *query, TwitchStream *out, int max);

/* PlaybackAccessToken -> usher -> master playlist parse.
   Returns: nb of qualities, 0 if offline, negative on error. */
int twitch_get_qualities(HttpCtx *c, const char *login, TwitchQuality *out, int max);

/* last Twitch error message (the "errors" field), "" if none.
   Read it right after a failed twitch_* call. */
const char *twitch_last_error(void);

/* ===== OAuth Device Flow =====
   login without typing the password in the app: the user authorizes
   from a browser (twitch.tv/activate). */

typedef struct {
    char user_code[16];     /* code to enter, e.g. "GNSLNMLF" */
    char verify_uri[128];   /* URL to open (twitch.tv/activate) */
    char device_code[64];   /* opaque, for polling */
    int  interval;          /* min delay between polls (s) */
    int  expires_in;        /* code validity (s) */
} TwitchDeviceAuth;

/* Starts the flow: requests a device code. Returns 0 ok, negative = error. */
int twitch_device_start(HttpCtx *c, TwitchDeviceAuth *out);

/* polls the token server.
   1 = authorized (token/refresh filled), 0 = pending, -1 = expired/refused/error. */
int twitch_device_poll(HttpCtx *c, const char *device_code,
                       char *token, size_t token_size,
                       char *refresh, size_t refresh_size);

/* login (lowercase) of the account tied to the token. */
int twitch_get_login(HttpCtx *c, const char *token, char *login, size_t size);
