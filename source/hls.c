// hls.c: parse an HLS media playlist (live Twitch), keep only the useful tags
#include <stdlib.h>
#include <string.h>

#include "hls.h"

/* strncmp stops at the 1st differing byte -> no out-of-bounds read
 * if the line is shorter than the prefix */
#define HAS_PREFIX(l, p) (strncmp((l), (p), sizeof(p) - 1) == 0)

// keep only the last HLS_MAX_SEGS
static void hls_push(HlsPlaylist *out, const char *url, size_t url_len,
                     double duration, u64 seq)
{
    if (out->count == HLS_MAX_SEGS) {
        memmove(&out->segs[0], &out->segs[1],
                (HLS_MAX_SEGS - 1) * sizeof(HlsSegment));
        out->count--;
    }
    HlsSegment *s = &out->segs[out->count++];
    memcpy(s->url, url, url_len);
    s->url[url_len] = '\0';
    s->duration     = duration;
    s->seq          = seq;
}

int hls_parse_media_playlist(const char *text, HlsPlaylist *out)
{
    if (!text || !out)
        return -1;

    memset(out, 0, sizeof(*out));

    const char *p = text;
    if ((u8)p[0] == 0xEF && (u8)p[1] == 0xBB && (u8)p[2] == 0xBF)
        p += 3;                              // optional UTF-8 BOM

    bool   header_ok = false;
    bool   have_inf  = false;                /* EXTINF seen, waiting for the URL */
    double inf_dur   = 0.0;
    u64    index     = 0;

    while (*p) {
        const char *line = p;
        const char *nl   = strchr(p, '\n');
        size_t      len  = nl ? (size_t)(nl - p) : strlen(p);
        p = nl ? nl + 1 : p + len;

        /* strip the trailing \r (CRLF) and stray spaces */
        while (len && (line[len-1] == '\r' || line[len-1] == ' ' || line[len-1] == '\t'))
            len--;
        while (len && (line[0] == ' ' || line[0] == '\t')) {
            line++;
            len--;
        }
        if (!len)
            continue;

        if (!header_ok) {
            if (HAS_PREFIX(line, "#EXTM3U")) {
                header_ok = true;
                continue;
            }
            return -2;   // first non-empty line is not #EXTM3U
        }

        if (line[0] == '#') {
            /* strtod/strtoull stop by themselves on ',', '\r' or '\n' */
            if (HAS_PREFIX(line, "#EXTINF:")) {
                inf_dur  = strtod(line + sizeof("#EXTINF:") - 1, NULL);
                have_inf = true;
            } else if (HAS_PREFIX(line, "#EXT-X-TARGETDURATION:")) {
                out->target_duration = strtod(line + sizeof("#EXT-X-TARGETDURATION:") - 1, NULL);
            } else if (HAS_PREFIX(line, "#EXT-X-MEDIA-SEQUENCE:")) {
                out->media_seq = strtoull(line + sizeof("#EXT-X-MEDIA-SEQUENCE:") - 1, NULL, 10);
            } else if (len == sizeof("#EXT-X-ENDLIST") - 1 && HAS_PREFIX(line, "#EXT-X-ENDLIST")) {
                out->ended = true;
            }
            // the rest (incl. TWITCH-PREFETCH, unfinalized segments): ignored
            continue;
        }

        // a URL only counts if an EXTINF precedes it
        if (!have_inf)
            continue;
        have_inf = false;

        u64 seq = out->media_seq + index;
        index++;                             // advance even on an invalid segment

        if (len >= HLS_URL_MAX)
            continue;                        // URL too long, drop it

        hls_push(out, line, len, inf_dur, seq);
    }

    return header_ok ? 0 : -2;
}
