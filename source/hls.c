// hls.c : parse une playlist HLS média (live Twitch), on garde que les tags utiles
#include <stdlib.h>
#include <string.h>

#include "hls.h"

/* strncmp s'arrete au 1er octet different -> pas de lecture hors-borne
 * si la ligne est plus courte que le prefixe */
#define HAS_PREFIX(l, p) (strncmp((l), (p), sizeof(p) - 1) == 0)

// garde seulement les HLS_MAX_SEGS derniers
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
        p += 3;                              // BOM UTF-8 eventuel

    bool   header_ok = false;
    bool   have_inf  = false;                /* EXTINF vu, on attend l'URL */
    double inf_dur   = 0.0;
    u64    index     = 0;

    while (*p) {
        const char *line = p;
        const char *nl   = strchr(p, '\n');
        size_t      len  = nl ? (size_t)(nl - p) : strlen(p);
        p = nl ? nl + 1 : p + len;

        /* vire le \r de fin (CRLF) et les espaces parasites */
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
            return -2;   // 1ere ligne non vide pas un #EXTM3U
        }

        if (line[0] == '#') {
            /* strtod/strtoull s'arretent tout seuls sur ',', '\r' ou '\n' */
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
            // le reste (dont TWITCH-PREFETCH, segments pas finalisés) : ignoré
            continue;
        }

        // une URL ne compte que si un EXTINF la précède
        if (!have_inf)
            continue;
        have_inf = false;

        u64 seq = out->media_seq + index;
        index++;                             // on avance même sur segment invalide

        if (len >= HLS_URL_MAX)
            continue;                        // URL trop longue, on jette

        hls_push(out, line, len, inf_dur, seq);
    }

    return header_ok ? 0 : -2;
}
