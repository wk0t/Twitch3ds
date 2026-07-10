// player.c : lecture d'un live (fetch + demux TS + sync audio/video)
// deux threads : le fetch télécharge/démuxe, le principal décode la vidéo calée sur l'horloge audio.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <3ds.h>

#include "player.h"
#include "twitch.h"
#include "net_http.h"
#include "hls.h"
#include "ts_demux.h"
#include "audio_aac.h"
#include "video_mvd.h"

// constantes de synchro (en 90 kHz sauf mention)
#define PTS_LEAD          2700      /* 30 ms : marge de présentation      */
#define PTS_LATE          45000     /* 500 ms : seuil de drop             */
#define PTS_DISCONT       900000    /* 10 s : recul de PTS = discontinuité */
#define PTS_PREFETCH_CAP  540000    /* 6 s : avance max du fetch          */
#define AUDIO_PREFETCH_MS 6000
#define BUFFER_START_MS   1200
#define BUFFER_START_AUS  60
#define BUFFER_MAX_WAIT_MS 3000     /* au-delà on démarre même sans horloge audio
                                     * (flux sans piste audio, DSP absent...) */
#define STARVE_MS         2000
#define MAX_DECODE_PER_UPDATE 2
#define MAX_POPS_PER_UPDATE   32    /* borne le travail de drop par frame */
#define FETCH_MAX_FAILS   5
#define ENDED_DRAIN_MS    60000     /* garde-fou : drain borné après ENDLIST */

// file circulaire des AU vidéo
typedef struct {
    size_t len;
    s64    pts;
    bool   keyframe;   /* NAL IDR (type 5)  */
    bool   has_sps;    /* NAL SPS (type 7)  */
    u8     data[];
} VideoAU;

#define VQ_MAX 256
static VideoAU  *s_vq[VQ_MAX];
static int       s_vq_head;
static int       s_vq_count;
static LightLock s_vq_lock;
static u32       s_drop_full;   /* AUs jetées faute de place */
static u32       s_drop_late;   /* AUs jetées car en retard  */

// état global du player
static struct {
    LightLock lock;                 /* protège paramètres / message d'erreur */
    volatile PlayerState state;
    volatile bool quit;
    volatile bool audio_only;       /* mode effectif (peut basculer si MVD KO) */

    bool  opened;                   /* thread fetch en cours (à joindre)  */
    bool  locks_inited;
    Thread fetch;
    HttpCtx *fetch_ctx;             /* créé/libéré par open/close (thread principal),
                                     * annulable pendant un transfert */

    char login[64];
    char pref[24];
    bool allow_video;

    char quality_name[24];
    int  q_width, q_height;
    char err[128];

    /* posés par le thread fetch, lus par close() après join */
    bool demux_inited;
    bool audio_started;

    /* côté thread principal uniquement */
    bool video_inited;
    u32  frames_decoded;
    bool drop_until_key;
    u64  starve_since;              /* osGetTime(), 0 = pas en famine */
    u64  buffering_since;           /* osGetTime() d'entrée en BUFFERING, 0 sinon */

    /* horloge de secours si audio_clock_pts() reste à -1 (stream sans audio) */
    bool fb_valid;
    s64  fb_base_pts;
    u64  fb_base_ms;
} P;

/* buffers volumineux hors pile (un seul thread fetch à la fois) */
static TwitchQuality s_quals[TW_MAX_QUALITIES];
static HlsPlaylist   s_pl;
static char          s_media_url[2048];

// ---- utilitaires ----

static void ensure_locks(void)
{
    if (!P.locks_inited) {
        LightLock_Init(&P.lock);
        LightLock_Init(&s_vq_lock);
        P.locks_inited = true;
    }
}

// pose le message d'erreur avant de basculer en PLAYER_ERROR
static void set_error(const char *fmt, ...)
{
    va_list ap;
    LightLock_Lock(&P.lock);
    va_start(ap, fmt);
    vsnprintf(P.err, sizeof(P.err), fmt, ap);
    va_end(ap);
    LightLock_Unlock(&P.lock);
    P.state = PLAYER_ERROR;
}

// sommeil par tranches de 100 ms en surveillant quit ; renvoie true si on doit quitter
static bool sleep_check_quit(int total_ms)
{
    while (total_ms > 0 && !P.quit) {
        int step = total_ms < 100 ? total_ms : 100;
        svcSleepThread((s64)step * 1000000LL);
        total_ms -= step;
    }
    return P.quit;
}

static int vq_count(void)
{
    LightLock_Lock(&s_vq_lock);
    int n = s_vq_count;
    LightLock_Unlock(&s_vq_lock);
    return n;
}

static void vq_clear(void)
{
    LightLock_Lock(&s_vq_lock);
    while (s_vq_count > 0) {
        free(s_vq[s_vq_head]);
        s_vq_head = (s_vq_head + 1) % VQ_MAX;
        s_vq_count--;
    }
    s_vq_head = 0;
    LightLock_Unlock(&s_vq_lock);
}

// PTS min/max de la file, false si vide
static bool vq_pts_range(s64 *oldest, s64 *newest)
{
    bool ok = false;
    LightLock_Lock(&s_vq_lock);
    if (s_vq_count > 0) {
        *oldest = s_vq[s_vq_head]->pts;
        *newest = s_vq[(s_vq_head + s_vq_count - 1) % VQ_MAX]->pts;
        ok = true;
    }
    LightLock_Unlock(&s_vq_lock);
    return ok;
}

// ---- callbacks démux (thread fetch) ----

static void on_video_au(const u8 *au, size_t len, s64 pts90k, void *user)
{
    (void)user;
    if (P.audio_only || len == 0)
        return;

    /* scan des start codes Annex-B pour repérer IDR (5) et SPS (7) */
    bool kf = false, sps = false;
    for (size_t i = 0; i + 3 < len; i++) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            u8 t = au[i + 3] & 0x1F;
            if (t == 5) kf = true;
            else if (t == 7) sps = true;
            i += 2;
        }
    }

    VideoAU *e = malloc(sizeof(VideoAU) + len);
    if (!e)
        return; /* plus de mémoire : on saute l'AU */
    e->len = len;
    e->pts = pts90k;
    e->keyframe = kf;
    e->has_sps = sps;
    memcpy(e->data, au, len);

    LightLock_Lock(&s_vq_lock);
    if (s_vq_count == VQ_MAX) {
        /* pleine : on jette la plus ancienne */
        free(s_vq[s_vq_head]);
        s_vq_head = (s_vq_head + 1) % VQ_MAX;
        s_vq_count--;
        s_drop_full++;
    }
    s_vq[(s_vq_head + s_vq_count) % VQ_MAX] = e;
    s_vq_count++;
    LightLock_Unlock(&s_vq_lock);
}

static void on_audio_pes(const u8 *adts, size_t len, s64 pts90k, void *user)
{
    (void)user;
    /* le décodage AAC se fait ici, dans le thread fetch : c'est voulu */
    audio_feed(adts, len, pts90k);
}

// ---- thread fetch ----

// choisit une qualité dans s_quals[0..n-1], -1 si rien
static int pick_quality(int n)
{
    bool want_audio = (strcmp(P.pref, "audio_only") == 0) || !P.allow_video;

    if (want_audio) {
        for (int i = 0; i < n; i++)
            if (s_quals[i].audio_only)
                return i;
        /* pas de rendition audio_only : plus petite bande passante,
         * la vidéo sera simplement ignorée */
    } else {
        size_t plen = strlen(P.pref);
        if (plen > 0) {
            for (int i = 0; i < n; i++)
                if (!s_quals[i].audio_only &&
                    strncmp(s_quals[i].name, P.pref, plen) == 0)
                    return i;
        }
    }

    /* à défaut : plus petite bande passante vidéo */
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (s_quals[i].audio_only)
            continue;
        if (best < 0 || s_quals[i].bandwidth < s_quals[best].bandwidth)
            best = i;
    }
    if (best < 0 && n > 0)
        best = 0; /* uniquement de l'audio_only disponible */
    return best;
}

/* cap mémoire : pas plus de ~6 s d'avance (PTS vidéo ou buffer audio) */
static void wait_prefetch_cap(void)
{
    while (!P.quit) {
        PlayerState st = P.state;
        if (st == PLAYER_ERROR || st == PLAYER_ENDED)
            return;

        s64 oldest, newest;
        s64 lead = -1;
        if (vq_pts_range(&oldest, &newest)) {
            s64 clock = audio_clock_pts();
            lead = (clock >= 0 && clock <= newest) ? newest - clock
                                                   : newest - oldest;
        }
        if (lead < PTS_PREFETCH_CAP && audio_buffered_ms() < AUDIO_PREFETCH_MS)
            return;
        svcSleepThread(100000000LL); /* 100 ms */
    }
}

static void fetch_thread_main(void *arg)
{
    (void)arg;

    /* le contexte HTTP appartient à open/close (thread principal), qui peut
     * l'annuler en plein transfert */
    HttpCtx *ctx = P.fetch_ctx;
    if (!ctx) {
        set_error("Erreur reseau (contexte HTTP)");
        return;
    }

    HttpBuf plbuf = {0}, segbuf = {0};

    int n = twitch_get_qualities(ctx, P.login, s_quals, TW_MAX_QUALITIES);
    if (n == 0) {
        /* chaine hors-ligne */
        P.state = PLAYER_ENDED;
        goto cleanup;
    }
    if (n < 0) {
        set_error("Impossible de recuperer le stream (%d)", n);
        goto cleanup;
    }

    int qi = pick_quality(n);
    if (qi < 0) {
        set_error("Aucune qualite lisible");
        goto cleanup;
    }

    LightLock_Lock(&P.lock);
    snprintf(P.quality_name, sizeof(P.quality_name), "%.*s",
             (int)sizeof(s_quals[qi].name) - 1, s_quals[qi].name);
    LightLock_Unlock(&P.lock);
    P.q_width  = s_quals[qi].width;
    P.q_height = s_quals[qi].height;
    if (s_quals[qi].audio_only)
        P.audio_only = true;
    snprintf(s_media_url, sizeof(s_media_url), "%s", s_quals[qi].url);

    if (P.quit)
        goto cleanup;

    audio_start();
    P.audio_started = true;
    ts_demux_init(on_video_au, on_audio_pes, NULL);
    P.demux_inited = true;
    P.state = PLAYER_BUFFERING;

    u64  last_seq = 0;
    bool have_seq = false;
    bool ended = false;
    int  fails = 0;
    int  last_code = 0;

    while (!P.quit && !ended) {
        int code = http_get(ctx, s_media_url, &plbuf);
        last_code = code;
        if (code < 200 || code >= 300 || !plbuf.data ||
            hls_parse_media_playlist((const char *)plbuf.data, &s_pl) < 0) {
            if (++fails >= FETCH_MAX_FAILS) {
                if (code == 404)
                    P.state = PLAYER_ENDED; /* stream coupé côté CDN */
                else
                    set_error("Playlist inaccessible (%d)", code);
                goto cleanup;
            }
            if (sleep_check_quit(1000))
                break;
            continue;
        }

        /* on se cale au bord du live : 3 derniers segments */
        int start = 0;
        if (!have_seq) {
            start = s_pl.count > 3 ? s_pl.count - 3 : 0;
        } else {
            while (start < s_pl.count && s_pl.segs[start].seq <= last_seq)
                start++;
        }

        bool seg_fail = false;
        for (int i = start; i < s_pl.count && !P.quit; i++) {
            wait_prefetch_cap();
            if (P.quit)
                break;

            code = http_get(ctx, s_pl.segs[i].url, &segbuf);
            if (code < 200 || code >= 300) {
                if (++fails >= FETCH_MAX_FAILS) {
                    set_error("Segment inaccessible (%d)", code);
                    goto cleanup;
                }
                seg_fail = true;
                break; /* re-essayé au prochain rafraîchissement */
            }
            fails = 0;
            if (segbuf.len > 0) {
                ts_demux_feed(segbuf.data, segbuf.len);
                ts_demux_flush();
            }
            last_seq = s_pl.segs[i].seq;
            have_seq = true;
        }
        if (!seg_fail)
            fails = 0;

        if (s_pl.ended && !seg_fail) {
            /* ENDLIST : on passe ENDED une fois les files vidées (drain borné) */
            int waited = 0;
            while (!P.quit && waited < ENDED_DRAIN_MS &&
                   (vq_count() > 0 || audio_buffered_ms() > 0)) {
                svcSleepThread(100000000LL);
                waited += 100;
            }
            if (!P.quit)
                P.state = PLAYER_ENDED;
            ended = true;
            break;
        }

        double td = s_pl.target_duration;
        int sleep_ms = (int)(td * 500.0);
        if (sleep_ms < 500)  sleep_ms = 500;
        if (sleep_ms > 5000) sleep_ms = 5000;
        if (sleep_check_quit(sleep_ms))
            break;
    }
    (void)last_code;

cleanup:
    http_buf_free(&plbuf);
    http_buf_free(&segbuf);
    /* ctx appartient à close : on ne le libère pas ici */
}

// ---- API publique (thread principal) ----

Result player_open(const char *login, const char *quality_pref, bool allow_video)
{
    ensure_locks();
    player_close(); /* au cas où une lecture tourne encore */

    P.quit = false;
    P.demux_inited = false;
    P.audio_started = false;
    P.video_inited = false;
    P.frames_decoded = 0;
    P.drop_until_key = false;
    P.starve_since = 0;
    P.buffering_since = 0;
    P.fb_valid = false;
    P.q_width = P.q_height = 0;
    s_drop_full = s_drop_late = 0;

    LightLock_Lock(&P.lock);
    snprintf(P.login, sizeof(P.login), "%s", login ? login : "");
    snprintf(P.pref, sizeof(P.pref), "%s", quality_pref ? quality_pref : "");
    P.allow_video = allow_video;
    P.quality_name[0] = '\0';
    P.err[0] = '\0';
    LightLock_Unlock(&P.lock);
    P.audio_only = (strcmp(P.pref, "audio_only") == 0) || !allow_video;

    P.state = PLAYER_CONNECTING;

    /* contexte HTTP tenu par le thread principal, annulable par close */
    P.fetch_ctx = http_ctx_new();
    if (!P.fetch_ctx) {
        set_error("Erreur reseau (contexte HTTP)");
        return MAKERESULT(RL_PERMANENT, RS_OUTOFRESOURCE, RM_APPLICATION,
                          RD_OUT_OF_MEMORY);
    }

    /* 128 Ko : curl + TLS + démux TS + AAC soft, c'est le thread le plus lourd.
     * On vise le cœur 2 du New 3DS (pleine puissance), sinon le cœur 1 (syscore),
     * et à défaut le cœur applicatif 0. */
    P.fetch = threadCreate(fetch_thread_main, NULL, 128 * 1024, 0x2A, 2, false);
    if (!P.fetch)
        P.fetch = threadCreate(fetch_thread_main, NULL, 128 * 1024, 0x2A, 1, false);
    if (!P.fetch)
        P.fetch = threadCreate(fetch_thread_main, NULL, 128 * 1024, 0x2A, 0, false);
    if (!P.fetch) {
        http_ctx_free(P.fetch_ctx);
        P.fetch_ctx = NULL;
        set_error("Creation du thread reseau impossible");
        return MAKERESULT(RL_PERMANENT, RS_OUTOFRESOURCE, RM_APPLICATION,
                          RD_OUT_OF_MEMORY);
    }
    P.opened = true;
    return 0;
}

void player_close(void)
{
    ensure_locks();
    if (!P.opened) {
        P.state = PLAYER_IDLE;
        return;
    }

    P.quit = true;
    /* on annule le transfert curl en cours. Sinon le thread reste bloqué jusqu'au
     * timeout (10-15 s) et threadFree libère sa pile pendant qu'il tourne encore
     * dessus -> use-after-free. Le callback de progression coupe en ~1 s, donc on
     * peut joindre sans borne. */
    if (P.fetch_ctx)
        http_ctx_cancel(P.fetch_ctx);
    if (P.fetch) {
        threadJoin(P.fetch, U64_MAX);
        threadFree(P.fetch);
        P.fetch = NULL;
    }
    if (P.fetch_ctx) {
        http_ctx_free(P.fetch_ctx);
        P.fetch_ctx = NULL;
    }

    vq_clear();
    if (P.audio_started)
        audio_stop();
    if (P.video_inited)
        video_exit();
    if (P.demux_inited)
        ts_demux_exit();

    P.audio_started = false;
    P.demux_inited = false;
    P.video_inited = false;
    P.frames_decoded = 0;
    P.opened = false;
    P.state = PLAYER_IDLE;
}

PlayerState player_state(void)
{
    return P.state;
}

const char *player_error_msg(void)
{
    return P.err;
}

/* horloge de lecture : l'audio est maître, repli sur osGetTime si pas d'audio */
static s64 playback_clock(s64 front_pts)
{
    s64 clock = audio_clock_pts();
    if (clock >= 0) {
        P.fb_valid = false;
        return clock;
    }
    if (!P.fb_valid) {
        P.fb_base_pts = front_pts;
        P.fb_base_ms = osGetTime();
        P.fb_valid = true;
    }
    return P.fb_base_pts + (s64)(osGetTime() - P.fb_base_ms) * 90;
}

void player_update(void)
{
    ensure_locks();
    /* on recharge l'anneau audio à chaque frame, indépendamment du fetch : sinon
     * le son coupe dès que le fetch se met en pause au plafond de préchargement,
     * et l'horloge (donc la vidéo) se fige avec lui */
    audio_update();

    PlayerState st = P.state;

    if (st == PLAYER_BUFFERING) {
        P.starve_since = 0;
        if (P.buffering_since == 0)
            P.buffering_since = osGetTime();

        bool video_ready = vq_count() > BUFFER_START_AUS;
        /* l'audio pilote l'horloge : on démarre dès qu'il a assez tamponné.
         * Sinon on démarre quand la vidéo est prête et que l'audio ne viendra pas
         * driver la lecture : tout de suite s'il n'y a pas de DSP, ou après un
         * délai de grâce (flux sans piste audio). Sans ce repli on resterait
         * coincé en buffering. */
        bool audio_wont_drive =
            !audio_available() ||
            (osGetTime() - P.buffering_since) > BUFFER_MAX_WAIT_MS;

        if (audio_buffered_ms() > BUFFER_START_MS ||
            (video_ready && audio_wont_drive)) {
            P.fb_valid = false;
            P.buffering_since = 0;
            P.state = PLAYER_PLAYING;
        }
        return;
    }
    if (st != PLAYER_PLAYING)
        return;

    int decoded = 0, pops = 0;
    while (decoded < MAX_DECODE_PER_UPDATE && pops < MAX_POPS_PER_UPDATE) {
        /* on ne pop que si l'AU de tête est due */
        VideoAU *au = NULL;
        s64 clock = 0;
        LightLock_Lock(&s_vq_lock);
        if (s_vq_count > 0) {
            VideoAU *front = s_vq[s_vq_head];
            clock = playback_clock(front->pts);
            if (front->pts <= clock + PTS_LEAD) {
                au = front;
                s_vq_head = (s_vq_head + 1) % VQ_MAX;
                s_vq_count--;
            }
        }
        LightLock_Unlock(&s_vq_lock);
        if (!au)
            break;
        pops++;

        if (P.audio_only) { /* MVD indisponible : on purge */
            free(au);
            continue;
        }

        /* PTS qui recule de >10 s : c'est une discontinuité, on laisse passer */
        bool discont = au->pts < clock - PTS_DISCONT;
        bool late = !discont && (au->pts < clock - PTS_LATE);

        if (P.drop_until_key && !au->keyframe) {
            s_drop_late++;
            free(au);
            continue;
        }
        if (late && !au->keyframe) {
            /* en retard : on jette jusqu'à la prochaine keyframe */
            P.drop_until_key = true;
            s_drop_late++;
            free(au);
            continue;
        }
        /* drop_until_key n'est levé qu'APRÈS un décodage réussi de la keyframe
         * (plus bas) : sinon une keyframe dont le décodage MVD échoue laisserait
         * passer des images P sans référence valide */

        if (!P.video_inited) {
            /* on init MVD au premier AU qui porte un SPS */
            if (!au->has_sps) {
                free(au);
                continue;
            }
            int w = P.q_width  > 0 ? P.q_width  : 640;
            int h = P.q_height > 0 ? P.q_height : 360;
            if (R_FAILED(video_init(w, h))) {
                /* MVD KO : on continue en audio seul */
                P.audio_only = true;
                free(au);
                vq_clear();
                break;
            }
            P.video_inited = true;
        }

        int r = video_decode_au(au->data, au->len);
        if (r == 1) {
            P.frames_decoded++;
            /* keyframe décodée : la référence IDR est en place, on peut
             * réaccepter les images P */
            if (au->keyframe)
                P.drop_until_key = false;
        }
        free(au);
        decoded++;
    }

    /* famine prolongée -> re-buffering */
    if (vq_count() == 0 && audio_buffered_ms() <= 0) {
        u64 now = osGetTime();
        if (P.starve_since == 0) {
            P.starve_since = now;
        } else if (now - P.starve_since > STARVE_MS) {
            P.starve_since = 0;
            P.buffering_since = 0;   /* réarme le délai de grâce audio */
            P.fb_valid = false;
            P.state = PLAYER_BUFFERING;
        }
    } else {
        P.starve_since = 0;
    }
}

bool player_has_video(void)
{
    return P.video_inited && P.frames_decoded > 0;
}

C2D_Image player_image(void)
{
    if (!player_has_video()) {
        C2D_Image none = {0};
        return none;
    }
    return video_image();
}

void player_video_dimensions(int *w, int *h)
{
    if (!P.video_inited) {
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    video_dimensions(w, h);
}

bool player_is_audio_only(void)
{
    return P.audio_only;
}

int player_buffered_ms(void)
{
    ensure_locks();
    int a = audio_buffered_ms();
    int v = 0;
    s64 oldest, newest;
    if (vq_pts_range(&oldest, &newest)) {
        s64 clock = audio_clock_pts();
        s64 ref = (clock >= 0 && clock <= newest) ? clock : oldest;
        s64 ms = (newest - ref) / 90;
        if (ms > 0)
            v = ms > 0x7FFFFFFF ? 0x7FFFFFFF : (int)ms;
    }
    return a > v ? a : v;
}

const char *player_quality_name(void)
{
    return P.quality_name;
}
