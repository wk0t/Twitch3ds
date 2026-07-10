// hls.h : parse une playlist HLS média (live Twitch)
#pragma once
#include <3ds.h>
#include <stdbool.h>

#define HLS_MAX_SEGS 16
#define HLS_URL_MAX  2048   // les URLs de segment Twitch montent à ~1600 (?dna=...)

typedef struct {
    char   url[HLS_URL_MAX];
    double duration;   /* secondes (EXTINF) */
    u64    seq;        // numéro de séquence média absolu
} HlsSegment;

typedef struct {
    HlsSegment segs[HLS_MAX_SEGS];
    int        count;
    u64        media_seq;          // seq du 1er segment du m3u8
    double     target_duration;
    bool       ended;              // EXT-X-ENDLIST vu (stream fini)
} HlsPlaylist;

// 0 si ok, négatif si le texte n'est pas une playlist valide
int hls_parse_media_playlist(const char *text, HlsPlaylist *out);
