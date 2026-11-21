// player.c: live playback (fetch + TS demux + audio/video sync)
// two threads: fetch downloads/demuxes, the main one decodes video locked on the audio clock.
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

// sync constants (in 90 kHz unless noted)
#define PTS_LEAD          2700      /* 30 ms: presentation margin         */
#define PTS_LATE          45000     /* 500 ms: drop threshold             */
#define PTS_DISCONT       900000    /* 10 s: PTS going backwards = discontinuity */
#define PTS_PREFETCH_CAP  540000    /* 6 s: max fetch lead                 */
#define AUDIO_PREFETCH_MS 6000
#define BUFFER_START_MS   1200
#define BUFFER_START_AUS  60
#define BUFFER_MAX_WAIT_MS 3000     /* beyond that we start even without an audio clock
                                     * (stream without audio track, no DSP...) */
#define STARVE_MS         2000
#define MAX_DECODE_PER_UPDATE 2
#define MAX_POPS_PER_UPDATE   32    /* bounds the drop work per frame */
#define FETCH_MAX_FAILS   5
#define ENDED_DRAIN_MS    60000     /* guard: bounded drain after ENDLIST */

// circular queue of video AUs
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
static u32       s_drop_full;   /* AUs dropped for lack of room */
static u32       s_drop_late;   /* AUs dropped because late     */

// global player state
static struct {
    LightLock lock;                 /* guards settings / error message */
    volatile PlayerState state;
    volatile bool quit;
    volatile bool audio_only;       /* effective mode (may switch if MVD fails) */

    bool  opened;                   /* fetch thread running (to join)   */
    bool  locks_inited;
    Thread fetch;
    HttpCtx *fetch_ctx;             /* created/freed by open/close (main thread),
                                     * cancellable during a transfer */

    char login[64];
    char pref[24];
    bool allow_video;

    char quality_name[24];
    int  q_width, q_height;
    char err[128];

    /* set by the fetch thread, read by close() after join */
    bool demux_inited;
    bool audio_started;

    /* main thread only */
    bool video_inited;
    u32  frames_decoded;
    bool drop_until_key;
    u64  starve_since;              /* osGetTime(), 0 = not starving */
    u64  buffering_since;           /* osGetTime() when entering BUFFERING, 0 otherwise */

    /* fallback clock if audio_clock_pts() stays at -1 (stream without audio) */
    bool fb_valid;
    s64  fb_base_pts;
    u64  fb_base_ms;
} P;

/* big buffers off the stack (only one fetch thread at a time) */
static TwitchQuality s_quals[TW_MAX_QUALITIES];
static HlsPlaylist   s_pl;
static char          s_media_url[2048];

// ---- utilities ----

static void ensure_locks(void)
{
    if (!P.locks_inited) {
        LightLock_Init(&P.lock);
        LightLock_Init(&s_vq_lock);
        P.locks_inited = true;
    }
}

// sets the error message before switching to PLAYER_ERROR
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

// sleeps in 100 ms slices while watching quit; returns true if we must exit
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

// min/max PTS of the queue, false if empty
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

// ---- demux callbacks (fetch thread) ----

static void on_video_au(const u8 *au, size_t len, s64 pts90k, void *user)
{
    (void)user;
    if (P.audio_only || len == 0)
        return;

    /* scans Annex-B start codes to spot IDR (5) and SPS (7) */
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
        return; /* out of memory: skip the AU */
    e->len = len;
    e->pts = pts90k;
    e->keyframe = kf;
    e->has_sps = sps;
    memcpy(e->data, au, len);

    LightLock_Lock(&s_vq_lock);
    if (s_vq_count == VQ_MAX) {
        /* full: drop the oldest */
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
    /* AAC decoding happens here, in the fetch thread: on purpose */
    audio_feed(adts, len, pts90k);
}

// ---- fetch thread ----

// picks a quality in s_quals[0..n-1], -1 if none
static int pick_quality(int n)
{
    bool want_audio = (strcmp(P.pref, "audio_only") == 0) || !P.allow_video;

    if (want_audio) {
        for (int i = 0; i < n; i++)
            if (s_quals[i].audio_only)
                return i;
        /* no audio_only rendition: smallest bandwidth,
         * the video will simply be ignored */
    } else {
        size_t plen = strlen(P.pref);
        if (plen > 0) {
            for (int i = 0; i < n; i++)
                if (!s_quals[i].audio_only &&
                    strncmp(s_quals[i].name, P.pref, plen) == 0)
                    return i;
        }
    }

    /* default: smallest video bandwidth */
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (s_quals[i].audio_only)
            continue;
        if (best < 0 || s_quals[i].bandwidth < s_quals[best].bandwidth)
            best = i;
    }
    if (best < 0 && n > 0)
        best = 0; /* only audio_only available */
    return best;
}

/* memory cap: no more than ~6 s of lead (video PTS or audio buffer) */
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

    /* the HTTP ctx belongs to open/close (main thread), which can
     * cancel it mid-transfer */
    HttpCtx *ctx = P.fetch_ctx;
    if (!ctx) {
        set_error("Network error (HTTP ctx)");
        return;
    }

    HttpBuf plbuf = {0}, segbuf = {0};

    int n = twitch_get_qualities(ctx, P.login, s_quals, TW_MAX_QUALITIES);
    if (n == 0) {
        /* channel offline */
        P.state = PLAYER_ENDED;
        goto cleanup;
    }
    if (n < 0) {
        set_error("Cannot get the stream (%d)", n);
        goto cleanup;
    }

    int qi = pick_quality(n);
    if (qi < 0) {
        set_error("No playable quality");
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
                    P.state = PLAYER_ENDED; /* stream cut by the CDN */
                else
                    set_error("Playlist unreachable (%d)", code);
                goto cleanup;
            }
            if (sleep_check_quit(1000))
                break;
            continue;
        }

        /* lock onto the live edge: last 3 segments */
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
                    set_error("Segment unreachable (%d)", code);
                    goto cleanup;
                }
                seg_fail = true;
                break; /* retried on the next refresh */
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
            /* ENDLIST: switch to ENDED once the queues are drained (bounded drain) */
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
    /* ctx belongs to close: we do not free it here */
}

// ---- public API (main thread) ----

Result player_open(const char *login, const char *quality_pref, bool allow_video)
{
    ensure_locks();
    player_close(); /* in case a playback is still running */

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

    /* HTTP ctx held by the main thread, cancellable by close */
    P.fetch_ctx = http_ctx_new();
    if (!P.fetch_ctx) {
        set_error("Network error (HTTP ctx)");
        return MAKERESULT(RL_PERMANENT, RS_OUTOFRESOURCE, RM_APPLICATION,
                          RD_OUT_OF_MEMORY);
    }

    /* 128 KiB: curl + TLS + TS demux + soft AAC, this is the heaviest thread.
     * Target core 2 of the New 3DS (full power), else core 1 (syscore),
     * else the application core 0. */
    P.fetch = threadCreate(fetch_thread_main, NULL, 128 * 1024, 0x2A, 2, false);
    if (!P.fetch)
        P.fetch = threadCreate(fetch_thread_main, NULL, 128 * 1024, 0x2A, 1, false);
    if (!P.fetch)
        P.fetch = threadCreate(fetch_thread_main, NULL, 128 * 1024, 0x2A, 0, false);
    if (!P.fetch) {
        http_ctx_free(P.fetch_ctx);
        P.fetch_ctx = NULL;
        set_error("Cannot create the network thread");
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
    /* we cancel the ongoing curl transfer. Else the thread stays blocked until the
     * timeout (10-15 s) and threadFree frees its stack while it is still running
     * on it -> use-after-free. The progress callback aborts in ~1 s, so we can
     * join without a bound. */
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

/* playback clock: audio is the master, fallback to osGetTime if no audio */
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
    /* we refill the audio ring every frame, independently of fetch: else the sound
     * cuts as soon as fetch pauses at the prefetch cap, and the clock
     * (hence the video) freezes with it */
    audio_update();

    PlayerState st = P.state;

    if (st == PLAYER_BUFFERING) {
        P.starve_since = 0;
        if (P.buffering_since == 0)
            P.buffering_since = osGetTime();

        bool video_ready = vq_count() > BUFFER_START_AUS;
        /* audio drives the clock: we start as soon as it has buffered enough.
         * Else we start when the video is ready and audio will not drive
         * playback: immediately if there is no DSP, or after a grace delay
         * (stream without audio track). Without this fallback we would stay
         * stuck in buffering. */
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
        /* we only pop if the head AU is due */
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

        if (P.audio_only) { /* no MVD: purge */
            free(au);
            continue;
        }

        /* PTS going back by >10 s: it is a discontinuity, let it through */
        bool discont = au->pts < clock - PTS_DISCONT;
        bool late = !discont && (au->pts < clock - PTS_LATE);

        if (P.drop_until_key && !au->keyframe) {
            s_drop_late++;
            free(au);
            continue;
        }
        if (late && !au->keyframe) {
            /* late: drop until the next keyframe */
            P.drop_until_key = true;
            s_drop_late++;
            free(au);
            continue;
        }
        /* drop_until_key is only cleared AFTER a successful keyframe decode
         * (below): else a keyframe whose MVD decode fails would let P frames
         * through without a valid reference */

        if (!P.video_inited) {
            /* init MVD on the first AU carrying an SPS */
            if (!au->has_sps) {
                free(au);
                continue;
            }
            int w = P.q_width  > 0 ? P.q_width  : 640;
            int h = P.q_height > 0 ? P.q_height : 360;
            if (R_FAILED(video_init(w, h))) {
                /* MVD failed: continue audio-only */
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
            /* keyframe decoded: the IDR reference is in place, we can
             * accept P frames again */
            if (au->keyframe)
                P.drop_until_key = false;
        }
        free(au);
        decoded++;
    }

    /* prolonged starvation -> re-buffering */
    if (vq_count() == 0 && audio_buffered_ms() <= 0) {
        u64 now = osGetTime();
        if (P.starve_since == 0) {
            P.starve_since = now;
        } else if (now - P.starve_since > STARVE_MS) {
            P.starve_since = 0;
            P.buffering_since = 0;   /* rearm the audio grace delay */
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
