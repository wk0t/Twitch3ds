// audio_aac.c : décode l'AAC (Helix) et pousse le PCM dans ndsp.
// Le décodage tourne dans le thread qui appelle audio_feed(), le reste se lit
// depuis le thread principal, d'où le LightLock.
#include <stdlib.h>
#include <string.h>
#include <3ds.h>

#include "audio_aac.h"
#include "aacdec.h"

#define AUDIO_CH     0
#define RING_N       32
#define FRAME_SAMPS  1024                              /* samples/canal, une trame AAC-LC */
#define PCM_BYTES    (FRAME_SAMPS * 2 * sizeof(s16))   /* stéréo entrelacé PCM16 */
#define PENDING_MAX  (256 * 1024)

typedef struct {
    bool init;
    bool available;      /* ndsp ok (dspfirm.cdc présent) */
    bool started;

    HAACDecoder hdec;
    LightLock   lock;

    /* anneau de wavebufs ndsp, rempli dans l'ordre */
    s16        *pcm_pool;            /* linearAlloc, RING_N tranches de PCM_BYTES */
    ndspWaveBuf ring[RING_N];
    s64         ring_pts[RING_N];
    u32         ring_dur[RING_N];
    int         ring_next;
    int         ring_tail;           /* plus vieux slot encore en vol */
    int         in_flight;

    bool  chn_cfg;
    int   sample_rate;
    float volume;

    /* reliquat ADTS pas encore décodé (ring plein ou trame coupée) */
    u8    *pending;
    size_t pending_len;
    s64    pending_pts;              /* PTS de la 1re trame, -1 si inconnu */

    /* horloge : dernier wavebuf DONE + temps écoulé depuis */
    s64 last_done_pts;               /* -1 tant que rien n'a joué */
    u64 last_done_time;
    s64 queued_end_pts;

    /* horloge bidon quand y'a pas de DSP */
    s64 sim_base_pts;
    u64 sim_base_time;
    s64 sim_end_pts;
} AudioState;

static AudioState s;

/* buffer de sortie Helix, touché seulement sous lock */
static s16 s_decbuf[AAC_MAX_NSAMPS * AAC_MAX_NCHANS];

static const int s_adts_rates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000, 7350, 0, 0, 0
};

/* lit un en-tête ADTS, renvoie la taille de trame ou -1 */
static int adts_frame_len(const u8 *p, size_t len, int *samplerate)
{
    if (len < 7 || p[0] != 0xFF || (p[1] & 0xF6) != 0xF0)
        return -1;
    int sr = s_adts_rates[(p[2] >> 2) & 0x0F];
    int fl = ((p[3] & 0x03) << 11) | (p[4] << 3) | (p[5] >> 5);
    if (sr == 0 || fl < 7)
        return -1;
    if (samplerate)
        *samplerate = sr;
    return fl;
}

/* durée d'une trame (1024 samples) en unités 90 kHz */
static u32 frame_dur90k(int samplerate)
{
    if (samplerate <= 0)
        samplerate = 48000;
    return (u32)((1024ULL * 90000 + samplerate / 2) / samplerate);
}

/* compte les trames ADTS entières du buffer (pour estimer la durée en file) */
static void adts_scan(const u8 *p, size_t len, int *frames, int *samplerate)
{
    int n = 0;
    size_t off = 0;
    while (off + 7 <= len) {
        int sr = 0;
        int fl = adts_frame_len(p + off, len - off, &sr);
        if (fl < 0) {
            int idx = AACFindSyncWord((unsigned char *)(p + off + 1), (int)(len - off - 1));
            if (idx < 0)
                break;
            off += (size_t)(1 + idx);
            continue;
        }
        if (off + (size_t)fl > len)
            break;
        off += (size_t)fl;
        n++;
        if (samplerate)
            *samplerate = sr;
    }
    *frames = n;
}

/* avance last_done_pts sur les wavebufs finis (lock tenu) */
static void clock_scan(void)
{
    while (s.in_flight > 0 && s.ring[s.ring_tail].status == NDSP_WBUF_DONE) {
        s.last_done_pts  = s.ring_pts[s.ring_tail] + s.ring_dur[s.ring_tail];
        s.last_done_time = osGetTime();
        s.ring_tail = (s.ring_tail + 1) % RING_N;
        s.in_flight--;
    }
}

/* horloge courante en 90 kHz (lock tenu), -1 si rien joué */
static s64 clock_now(void)
{
    if (!s.available) {
        if (s.sim_base_pts < 0)
            return -1;
        s64 c = s.sim_base_pts + (s64)(osGetTime() - s.sim_base_time) * 90;
        if (s.sim_end_pts >= 0 && c > s.sim_end_pts)
            c = s.sim_end_pts;
        return c;
    }
    clock_scan();
    if (s.last_done_pts < 0)
        return -1;
    s64 c = s.last_done_pts + (s64)(osGetTime() - s.last_done_time) * 90;
    if (s.queued_end_pts >= 0 && c > s.queued_end_pts)
        c = s.queued_end_pts;
    return c;
}

static void apply_mix(void)
{
    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = mix[1] = s.volume;
    ndspChnSetMix(AUDIO_CH, mix);
}

/* config du canal 0, à la 1re trame décodée (lock tenu) */
static void chn_configure(int rate)
{
    ndspChnReset(AUDIO_CH);
    ndspChnSetInterp(AUDIO_CH, NDSP_INTERP_LINEAR);
    ndspChnSetRate(AUDIO_CH, (float)rate);
    ndspChnSetFormat(AUDIO_CH, NDSP_FORMAT_STEREO_PCM16);
    apply_mix();
    s.sample_rate = rate;
    s.chn_cfg = true;
}

/* décode le reliquat vers l'anneau tant qu'il reste de la place (lock tenu) */
static void pump_ring(void)
{
    for (;;) {
        if (s.pending_len < 7)
            break;

        clock_scan();
        ndspWaveBuf *wb = &s.ring[s.ring_next];
        if (wb->status == NDSP_WBUF_QUEUED || wb->status == NDSP_WBUF_PLAYING)
            break; /* anneau plein, le reste patiente dans pending */

        unsigned char *in = s.pending;
        int left = (int)s.pending_len;
        int err = AACDecode(s.hdec, &in, &left, s_decbuf);

        if (err == ERR_AAC_INDATA_UNDERFLOW)
            break; /* trame pas complète, on attend la suite */

        if (err < 0) {
            /* trame pourrie : on resync sur le prochain sync ADTS.
               Helix n'a pas bougé les pointeurs quand ça foire. */
            int idx = AACFindSyncWord(s.pending + 1, (int)s.pending_len - 1);
            if (idx < 0) {
                s.pending_len = 0;
                break;
            }
            s.pending_len -= (size_t)(1 + idx);
            memmove(s.pending, s.pending + 1 + idx, s.pending_len);
            continue;
        }

        /* ok, Helix a mangé une trame ; on recompacte le reste en tête */
        s.pending_len = (size_t)left;
        if (left > 0)
            memmove(s.pending, in, (size_t)left);

        AACFrameInfo fi;
        AACGetLastFrameInfo(s.hdec, &fi);
        if (fi.outputSamps <= 0 || fi.nChans <= 0 || fi.sampRateOut <= 0)
            continue;

        if (!s.chn_cfg) {
            chn_configure(fi.sampRateOut);
        } else if (fi.sampRateOut != s.sample_rate) {
            ndspChnSetRate(AUDIO_CH, (float)fi.sampRateOut);
            s.sample_rate = fi.sampRateOut;
        }

        int spc = fi.outputSamps / fi.nChans;
        if (spc > FRAME_SAMPS)
            spc = FRAME_SAMPS;

        s16 *dst = s.pcm_pool + (size_t)s.ring_next * (FRAME_SAMPS * 2);
        if (fi.nChans == 1) {
            /* mono : on double en stéréo entrelacé */
            for (int i = 0; i < spc; i++) {
                dst[2 * i]     = s_decbuf[i];
                dst[2 * i + 1] = s_decbuf[i];
            }
        } else {
            memcpy(dst, s_decbuf, (size_t)spc * 2 * sizeof(s16));
        }
        DSP_FlushDataCache(dst, (u32)(spc * 2 * sizeof(s16)));

        s64 pts = (s.pending_pts >= 0) ? s.pending_pts : 0;
        u32 dur = frame_dur90k(fi.sampRateOut);

        memset(wb, 0, sizeof(*wb));
        wb->data_vaddr = dst;
        wb->nsamples   = (u32)spc;
        s.ring_pts[s.ring_next] = pts;
        s.ring_dur[s.ring_next] = dur;
        ndspChnWaveBufAdd(AUDIO_CH, wb);

        if (s.last_done_pts < 0) {
            /* 1re trame : la lecture démarre là */
            s.last_done_pts  = pts;
            s.last_done_time = osGetTime();
        }
        s.queued_end_pts = pts + dur;
        s.pending_pts    = pts + dur;

        s.ring_next = (s.ring_next + 1) % RING_N;
        s.in_flight++;
    }
}

/* remet tout à zéro : files ndsp, anneau, reliquat, horloges (lock tenu) */
static void purge_locked(void)
{
    if (s.available) {
        ndspChnWaveBufClear(AUDIO_CH);
        ndspChnReset(AUDIO_CH);
    }
    memset(s.ring, 0, sizeof(s.ring));
    memset(s.ring_pts, 0, sizeof(s.ring_pts));
    memset(s.ring_dur, 0, sizeof(s.ring_dur));
    s.ring_next = s.ring_tail = 0;
    s.in_flight = 0;
    s.pending_len = 0;
    s.pending_pts = -1;
    s.chn_cfg = false;
    s.sample_rate = 0;
    s.last_done_pts = -1;
    s.last_done_time = 0;
    s.queued_end_pts = -1;
    s.sim_base_pts = -1;
    s.sim_base_time = 0;
    s.sim_end_pts = -1;
    if (s.hdec)
        AACFlushCodec(s.hdec);
}

Result audio_init(void)
{
    if (s.init)
        return 0;

    memset(&s, 0, sizeof(s));
    LightLock_Init(&s.lock);
    s.volume = 1.0f;
    s.pending_pts = -1;
    s.last_done_pts = -1;
    s.queued_end_pts = -1;
    s.sim_base_pts = -1;
    s.sim_end_pts = -1;

    s.pending = malloc(PENDING_MAX);
    if (!s.pending)
        return MAKERESULT(RL_FATAL, RS_OUTOFRESOURCE, RM_APPLICATION, RD_OUT_OF_MEMORY);

    s.hdec = AACInitDecoder();
    if (!s.hdec) {
        free(s.pending);
        s.pending = NULL;
        return MAKERESULT(RL_FATAL, RS_OUTOFRESOURCE, RM_APPLICATION, RD_OUT_OF_MEMORY);
    }

    /* si ndspInit rate (pas de dspfirm.cdc) on joue muet, c'est pas fatal */
    if (R_SUCCEEDED(ndspInit())) {
        s.pcm_pool = linearAlloc(RING_N * PCM_BYTES);
        if (s.pcm_pool) {
            ndspSetOutputMode(NDSP_OUTPUT_STEREO);
            s.available = true;
        } else {
            ndspExit();
        }
    }

    s.init = true;
    return 0;
}

void audio_exit(void)
{
    if (!s.init)
        return;

    LightLock_Lock(&s.lock);
    s.started = false;
    if (s.available) {
        ndspChnWaveBufClear(AUDIO_CH);
        ndspChnReset(AUDIO_CH);
    }
    LightLock_Unlock(&s.lock);

    if (s.available)
        ndspExit();
    if (s.pcm_pool) {
        linearFree(s.pcm_pool);
        s.pcm_pool = NULL;
    }
    if (s.hdec) {
        AACFreeDecoder(s.hdec);
        s.hdec = NULL;
    }
    free(s.pending);
    s.pending = NULL;
    s.available = false;
    s.init = false;
}

bool audio_available(void)
{
    return s.init && s.available;
}

void audio_start(void)
{
    if (!s.init)
        return;
    LightLock_Lock(&s.lock);
    purge_locked();
    s.started = true;
    LightLock_Unlock(&s.lock);
}

void audio_stop(void)
{
    if (!s.init)
        return;
    LightLock_Lock(&s.lock);
    purge_locked();
    s.started = false;
    LightLock_Unlock(&s.lock);
}

void audio_feed(const u8 *data, size_t len, s64 pts90k)
{
    if (!s.init || !data || len == 0)
        return;

    LightLock_Lock(&s.lock);
    if (!s.started) {
        LightLock_Unlock(&s.lock);
        return;
    }

    if (!s.available) {
        /* pas de DSP : on fait tourner une horloge calée sur le temps réel */
        int frames = 0, sr = 0;
        adts_scan(data, len, &frames, &sr);
        if (pts90k >= 0) {
            if (s.sim_base_pts < 0) {
                s.sim_base_pts  = pts90k;
                s.sim_base_time = osGetTime();
            }
            s64 end = pts90k + (s64)frames * frame_dur90k(sr);
            if (end > s.sim_end_pts)
                s.sim_end_pts = end;
        }
        LightLock_Unlock(&s.lock);
        return;
    }

    /* payload délirant, plus gros que tout le buffer : on garde que la fin */
    if (len >= PENDING_MAX) {
        data += len - PENDING_MAX / 2;
        len = PENDING_MAX / 2;
        s.pending_len = 0;
        s.pending_pts = -1;
    }

    /* ça déborde : on jette les vieilles trames (le player a pris du retard) */
    if (s.pending_len + len > PENDING_MAX) {
        size_t need = s.pending_len + len - PENDING_MAX;
        size_t drop = 0;
        while (drop < need && drop < s.pending_len) {
            int sr = 0;
            int fl = adts_frame_len(s.pending + drop, s.pending_len - drop, &sr);
            if (fl > 0 && drop + (size_t)fl <= s.pending_len) {
                drop += (size_t)fl;
                if (s.pending_pts >= 0)
                    s.pending_pts += frame_dur90k(sr);
            } else if (s.pending_len - drop >= 2) {
                int idx = AACFindSyncWord(s.pending + drop + 1,
                                          (int)(s.pending_len - drop - 1));
                if (idx < 0) {
                    drop = s.pending_len;
                    break;
                }
                drop += (size_t)(1 + idx);
            } else {
                drop = s.pending_len;
                break;
            }
        }
        if (drop >= s.pending_len) {
            s.pending_len = 0;
            s.pending_pts = -1;
        } else {
            s.pending_len -= drop;
            memmove(s.pending, s.pending + drop, s.pending_len);
        }
    }

    /* PTS de la 1re trame : celui du PES si le reliquat est vide, sinon on
       continue en ajoutant la durée des trames une par une */
    if (s.pending_len == 0 && pts90k >= 0)
        s.pending_pts = pts90k;
    else if (s.pending_pts < 0)
        s.pending_pts = (pts90k >= 0) ? pts90k : 0;

    memcpy(s.pending + s.pending_len, data, len);
    s.pending_len += len;

    pump_ring();
    LightLock_Unlock(&s.lock);
}

void audio_update(void)
{
    if (!s.init || !s.available)
        return;
    LightLock_Lock(&s.lock);
    if (s.started)
        pump_ring();   /* recharge l'anneau au fur et à mesure */
    LightLock_Unlock(&s.lock);
}

s64 audio_clock_pts(void)
{
    if (!s.init)
        return -1;
    LightLock_Lock(&s.lock);
    s64 c = clock_now();
    LightLock_Unlock(&s.lock);
    return c;
}

void audio_set_volume(float v)
{
    if (!s.init)
        return;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    LightLock_Lock(&s.lock);
    s.volume = v;
    if (s.available && s.chn_cfg)
        apply_mix();
    LightLock_Unlock(&s.lock);
}

int audio_buffered_ms(void)
{
    if (!s.init)
        return 0;

    LightLock_Lock(&s.lock);
    s64 ms = 0;
    if (s.available) {
        s64 c = clock_now();
        if (c >= 0 && s.queued_end_pts > c)
            ms += (s.queued_end_pts - c) / 90;
        if (s.pending_len >= 7) {
            int frames = 0, sr = 0;
            adts_scan(s.pending, s.pending_len, &frames, &sr);
            if (sr == 0)
                sr = s.sample_rate;
            if (frames > 0)
                ms += ((s64)frames * frame_dur90k(sr)) / 90;
        }
    } else {
        s64 c = clock_now();
        if (c >= 0 && s.sim_end_pts > c)
            ms = (s.sim_end_pts - c) / 90;
    }
    LightLock_Unlock(&s.lock);

    if (ms < 0) ms = 0;
    if (ms > 0x7FFFFFFF) ms = 0x7FFFFFFF;
    return (int)ms;
}
