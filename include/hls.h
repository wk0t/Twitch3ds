// hls.h: parse an HLS media playlist (live Twitch)
#pragma once
#include <3ds.h>
#include <stdbool.h>

#define HLS_MAX_SEGS 16
#define HLS_URL_MAX  2048   // Twitch segment URLs go up to ~1600 chars (?dna=...)

typedef struct {
    char   url[HLS_URL_MAX];
    double duration;   /* seconds (EXTINF) */
    u64    seq;        // absolute media sequence number
} HlsSegment;

typedef struct {
    HlsSegment segs[HLS_MAX_SEGS];
    int        count;
    u64        media_seq;          // seq of the first segment of the m3u8
    double     target_duration;
    bool       ended;              // EXT-X-ENDLIST seen (stream over)
} HlsPlaylist;

// 0 if ok, negative if the text is not a valid playlist
int hls_parse_media_playlist(const char *text, HlsPlaylist *out);
