// twitch.h : API Twitch non officielle (GQL + usher). impl dans twitch.c
#pragma once
#include "net_http.h"

#define TWITCH_CLIENT_ID "kimne78kx3ncx6brgo4mv6wki5h1ko"
#define TWITCH_GQL_URL   "https://gql.twitch.tv/gql"

#define TW_MAX_QUALITIES 12

typedef struct {
    char login[64];        /* minuscules */
    char display[64];
    char title[192];       /* UTF-8, tronqué proprement */
    char game[96];
    char preview_url[256]; /* miniature 320x180 */
    int  viewers;
} TwitchStream;

typedef struct {
    char name[24];   /* ex "480p", "audio_only", "1080p60 (source)" */
    char url[2048];
    int  width, height;
    int  bandwidth;
    bool audio_only;
} TwitchQuality;

/* Retour : nombre d'éléments écrits, ou négatif en cas d'erreur. */
int twitch_top_streams(HttpCtx *c, TwitchStream *out, int max);
int twitch_search_streams(HttpCtx *c, const char *query, TwitchStream *out, int max);

/* PlaybackAccessToken -> usher -> parse de la master playlist.
   retour : nb de qualités, 0 si hors-ligne, négatif si erreur. */
int twitch_get_qualities(HttpCtx *c, const char *login, TwitchQuality *out, int max);

/* dernier message d'erreur Twitch (champ "errors"), "" si aucun.
   à lire juste après un twitch_* qui a échoué. */
const char *twitch_last_error(void);

/* ===== OAuth Device Flow =====
   connexion sans taper le mot de passe dans l'app : l'utilisateur autorise
   depuis un navigateur (twitch.tv/activate). */

typedef struct {
    char user_code[16];     /* code à saisir, ex "GNSLNMLF" */
    char verify_uri[128];   /* URL à ouvrir (twitch.tv/activate) */
    char device_code[64];   /* opaque, pour le polling */
    int  interval;          /* délai min entre polls (s) */
    int  expires_in;        /* validité du code (s) */
} TwitchDeviceAuth;

/* Démarre le flux : demande un code d'appareil. Retour 0 ok, négatif = erreur. */
int twitch_device_start(HttpCtx *c, TwitchDeviceAuth *out);

/* interroge le serveur de jetons.
   1 = autorisé (token/refresh remplis), 0 = en attente, -1 = expiré/refusé/erreur. */
int twitch_device_poll(HttpCtx *c, const char *device_code,
                       char *token, size_t token_size,
                       char *refresh, size_t refresh_size);

/* login (minuscules) du compte associé au token. */
int twitch_get_login(HttpCtx *c, const char *token, char *login, size_t size);
