// thumb.c : miniatures de streams (JPEG 320x180) chargées en tâche de fond.
// worker qui fetch+décode, les textures C3D se créent que sur le main thread.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include <3ds.h>
#include <jpeglib.h>

#include "thumb.h"
#include "net_http.h"

/* état d'un slot (t->state) */
enum {
    TH_IDLE = 0,
    TH_PENDING,    /* en file, attend le worker */
    TH_LOADING,
    TH_DECODED,    /* pixels prêts, reste la texture à créer (main thread) */
    TH_READY,
    TH_FAILED,
};

#define TH_TEXW      512
#define TH_TEXH      256
#define TH_QUEUE_MAX 8
#define TH_DONE_MAX  32
#define TH_STACK     (64 * 1024)
#define TH_URL_CAP   (sizeof(((Thumb *)0)->url))

// linéaire -> tiled RGB565, pas de scaling
#define TH_XFER_FLAGS                                        \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |   \
     GX_TRANSFER_RAW_COPY(0) |                               \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |         \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |        \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

/* les miniatures Twitch font 320x180, posées sur une texture 512x256 */
static const Tex3DS_SubTexture s_subtex = {
    .width  = 320,
    .height = 180,
    .left   = 0.0f,
    .top    = 1.0f,
    .right  = 320.0f / (float)TH_TEXW,
    .bottom = 1.0f - 180.0f / (float)TH_TEXH,
};

static LightLock  s_lock;
static LightEvent s_event;
static Thread     s_thread;
static volatile bool s_running;
static bool       s_inited;
static HttpCtx   *s_http;   /* gardé par init/exit pour pouvoir l'annuler */

/* file de requêtes (anneau) pour le worker */
static Thumb *s_queue[TH_QUEUE_MAX];
static int    s_qhead, s_qcount;

/* file des slots décodés, à transformer en texture */
static Thumb *s_done[TH_DONE_MAX];
static int    s_dhead, s_dcount;

/* files, toujours manipulées sous s_lock */

static bool queue_push(Thumb *t)
{
    if (s_qcount >= TH_QUEUE_MAX)
        return false;
    s_queue[(s_qhead + s_qcount) % TH_QUEUE_MAX] = t;
    s_qcount++;
    return true;
}

static bool done_push(Thumb *t)
{
    if (s_dcount >= TH_DONE_MAX)
        return false;
    s_done[(s_dhead + s_dcount) % TH_DONE_MAX] = t;
    s_dcount++;
    return true;
}

/* décodage JPEG (côté worker) */

typedef struct {
    struct jpeg_error_mgr mgr;
    jmp_buf jb;
} ThJpegErr;

/* le error_exit par défaut de libjpeg appelle exit(), donc on longjmp à la place */
static void th_jpeg_error_exit(j_common_ptr cinfo)
{
    ThJpegErr *e = (ThJpegErr *)cinfo->err;
    longjmp(e->jb, 1);
}

static void th_jpeg_silent(j_common_ptr cinfo)
{
    (void)cinfo; /* pas de log sur stderr */
}

static bool thumb_decode_jpeg(const u8 *data, size_t len,
                              u16 **out_px, int *out_w, int *out_h)
{
    struct jpeg_decompress_struct cinfo;
    ThJpegErr jerr;
    /* volatile : relus après le longjmp du handler d'erreur */
    u16 *volatile px  = NULL;
    u8  *volatile row = NULL;

    cinfo.err = jpeg_std_error(&jerr.mgr);
    jerr.mgr.error_exit     = th_jpeg_error_exit;
    jerr.mgr.output_message = th_jpeg_silent;
    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&cinfo);
        free(px);
        free(row);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, (unsigned long)len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int w = (int)cinfo.output_width;
    int h = (int)cinfo.output_height;
    if (w < 1 || h < 1 || w > TH_TEXW || h > TH_TEXH ||
        cinfo.output_components != 3) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    px  = malloc((size_t)w * (size_t)h * 2);
    row = malloc((size_t)w * 3);
    if (!px || !row) {
        jpeg_destroy_decompress(&cinfo);
        free(px);
        free(row);
        return false;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        int y = (int)cinfo.output_scanline;
        JSAMPROW rp = row;
        if (jpeg_read_scanlines(&cinfo, &rp, 1) != 1)
            longjmp(jerr.jb, 1); /* flux tronqué */
        const u8 *s = row;
        u16 *d = px + (size_t)y * (size_t)w;
        for (int i = 0; i < w; i++, s += 3)
            d[i] = (u16)(((s[0] & 0xF8) << 8) | ((s[1] & 0xFC) << 3) | (s[2] >> 3));
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(row);

    *out_px = px;
    *out_w  = w;
    *out_h  = h;
    return true;
}

/* worker : réseau + décodage. Jamais de C3D/GX ici (main thread only). */

static void thumb_worker(void *arg)
{
    (void)arg;
    HttpCtx *http = s_http;
    HttpBuf  buf  = {0};

    while (s_running) {
        LightEvent_Wait(&s_event);

        for (;;) {
            if (!s_running)
                break;

            /* prend la prochaine requête encore valide */
            Thumb *t = NULL;
            char url[TH_URL_CAP];
            LightLock_Lock(&s_lock);
            while (s_qcount > 0) {
                Thumb *c = s_queue[s_qhead];
                s_qhead = (s_qhead + 1) % TH_QUEUE_MAX;
                s_qcount--;
                if (c->state == TH_PENDING) { /* sinon l'entrée est périmée */
                    t = c;
                    break;
                }
            }
            if (t) {
                memcpy(url, t->url, TH_URL_CAP);
                url[TH_URL_CAP - 1] = '\0';
                t->state = TH_LOADING;
            }
            LightLock_Unlock(&s_lock);
            if (!t)
                break;

            u16 *pixels = NULL;
            int  pw = 0, ph = 0;
            bool ok = false;
            if (http && url[0]) {
                int code = http_get(http, url, &buf);
                if (code >= 200 && code < 300 && buf.len > 0)
                    ok = thumb_decode_jpeg(buf.data, buf.len, &pixels, &pw, &ph);
            }

            LightLock_Lock(&s_lock);
            if (t->state == TH_LOADING) {
                if (strcmp(t->url, url) == 0) {
                    if (ok && done_push(t)) {
                        t->pixels = pixels;
                        t->pw = pw;
                        t->ph = ph;
                        t->state = TH_DECODED;
                        pixels = NULL; /* le slot en est proprio maintenant */
                    } else {
                        t->failed = true;
                        t->state  = TH_FAILED;
                    }
                } else {
                    /* l'URL a changé en route : on jette, le slot redevient demandable */
                    t->state = TH_IDLE;
                }
            }
            /* sinon le slot a été reset/relâché entre-temps, on jette tout */
            LightLock_Unlock(&s_lock);
            free(pixels);
        }
    }

    http_buf_free(&buf);
    // s_http appartient à thumb_system_exit, on n'y touche pas ici
}

/* API publique, appelée depuis le thread principal */

void thumb_system_init(void)
{
    if (s_inited)
        return;
    LightLock_Init(&s_lock);
    LightEvent_Init(&s_event, RESET_ONESHOT);
    s_qhead = s_qcount = 0;
    s_dhead = s_dcount = 0;
    s_running = true;
    s_http = http_ctx_new();

    /* on vise le cœur 2 (New 3DS), sinon syscore 1, sinon le cœur appli.
       Le worker ne tourne qu'en navigation (lecteur fermé), donc le cœur 1
       est libre à ce moment-là. */
    s_thread = threadCreate(thumb_worker, NULL, TH_STACK, 0x2D, 2, false);
    if (!s_thread)
        s_thread = threadCreate(thumb_worker, NULL, TH_STACK, 0x2D, 1, false);
    if (!s_thread)
        s_thread = threadCreate(thumb_worker, NULL, TH_STACK, 0x31, 0, false);
    if (!s_thread)
        s_running = false;
    s_inited = true;
}

void thumb_system_exit(void)
{
    if (!s_inited)
        return;
    if (s_thread) {
        s_running = false;
        /* abandonne un http_get en cours (sinon jusqu'à 15 s de gel à la sortie) */
        if (s_http)
            http_ctx_cancel(s_http);
        LightEvent_Signal(&s_event);
        threadJoin(s_thread, U64_MAX);
        threadFree(s_thread);
        s_thread = NULL;
    }
    if (s_http) {
        http_ctx_free(s_http);
        s_http = NULL;
    }
    s_qcount = 0;
    s_dcount = 0;
    s_inited = false;
}

void thumb_request(Thumb *t, const char *url)
{
    if (!t || !url || !url[0] || !s_inited)
        return;
    if (!s_thread) { /* pas de worker : on échoue direct */
        t->failed = true;
        t->state  = TH_FAILED;
        return;
    }

    char urlbuf[TH_URL_CAP];
    strncpy(urlbuf, url, TH_URL_CAP - 1);
    urlbuf[TH_URL_CAP - 1] = '\0';

    LightLock_Lock(&s_lock);

    bool same = (strcmp(t->url, urlbuf) == 0);
    if (same && (t->ready || t->state == TH_PENDING ||
                 t->state == TH_LOADING || t->state == TH_DECODED)) {
        LightLock_Unlock(&s_lock);
        return; /* déjà prêt ou déjà en cours pour cette URL */
    }

    if (t->state == TH_PENDING) {
        /* déjà en file, le worker prendra la nouvelle URL au dépilage */
        memcpy(t->url, urlbuf, TH_URL_CAP);
        t->failed = false;
        LightLock_Unlock(&s_lock);
        return;
    }

    if (t->state == TH_LOADING) {
        /* un vieux téléchargement tourne encore, il sera jeté à l'arrivée */
        memcpy(t->url, urlbuf, TH_URL_CAP);
        t->failed = false;
        if (queue_push(t))
            t->state = TH_PENDING;
        /* si la file est pleine on reste LOADING, le worker le repassera
           IDLE (URL différente) et un prochain appel retentera */
        LightLock_Unlock(&s_lock);
        return;
    }

    /* slot au repos : on remet tout à zéro */
    if (t->pixels) {
        free(t->pixels);
        t->pixels = NULL;
    }
    if (t->img.subtex && t->img.subtex != &s_subtex)
        free((void *)t->img.subtex);
    if (t->tex.data)
        C3D_TexDelete(&t->tex); /* main thread only */
    memset(&t->tex, 0, sizeof(t->tex));
    t->img.tex    = NULL;
    t->img.subtex = NULL;
    t->ready  = false;
    t->failed = false;
    t->pw = t->ph = 0;
    memcpy(t->url, urlbuf, TH_URL_CAP);

    if (queue_push(t)) {
        t->state = TH_PENDING;
        LightLock_Unlock(&s_lock);
        LightEvent_Signal(&s_event);
    } else {
        t->state = TH_IDLE; /* file pleine, on retentera au prochain appel */
        LightLock_Unlock(&s_lock);
    }
}

/* pixels -> texture. t n'est plus dans les files, il appartient au main. */
static void thumb_upload(Thumb *t)
{
    int pw = t->pw, ph = t->ph;

    if (!t->pixels || pw < 1 || ph < 1 || pw > TH_TEXW || ph > TH_TEXH)
        goto fail;

    if (!C3D_TexInit(&t->tex, TH_TEXW, TH_TEXH, GPU_RGB565))
        goto fail;
    C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&t->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    u16 *lin = linearAlloc(TH_TEXW * TH_TEXH * 2);
    if (!lin) {
        C3D_TexDelete(&t->tex);
        memset(&t->tex, 0, sizeof(t->tex));
        goto fail;
    }
    memset(lin, 0, TH_TEXW * TH_TEXH * 2);
    for (int y = 0; y < ph; y++)
        memcpy(lin + (size_t)y * TH_TEXW, t->pixels + (size_t)y * pw, (size_t)pw * 2);

    GSPGPU_FlushDataCache(lin, TH_TEXW * TH_TEXH * 2);
    GX_DisplayTransfer((u32 *)lin, GX_BUFFER_DIM(TH_TEXW, TH_TEXH),
                       (u32 *)t->tex.data, GX_BUFFER_DIM(TH_TEXW, TH_TEXH),
                       TH_XFER_FLAGS);
    gspWaitForPPF();
    linearFree(lin);

    free(t->pixels);
    t->pixels = NULL;

    /* subtex partagée pour le 320x180 habituel, sinon on en alloue une */
    const Tex3DS_SubTexture *st = &s_subtex;
    if (pw != 320 || ph != 180) {
        Tex3DS_SubTexture *dyn = malloc(sizeof(*dyn));
        if (dyn) {
            dyn->width  = (u16)pw;
            dyn->height = (u16)ph;
            dyn->left   = 0.0f;
            dyn->top    = 1.0f;
            dyn->right  = (float)pw / (float)TH_TEXW;
            dyn->bottom = 1.0f - (float)ph / (float)TH_TEXH;
            st = dyn;
        }
    }
    t->img.tex    = &t->tex;
    t->img.subtex = st;

    LightLock_Lock(&s_lock);
    t->ready = true;
    t->state = TH_READY;
    LightLock_Unlock(&s_lock);
    return;

fail:
    free(t->pixels);
    t->pixels = NULL;
    LightLock_Lock(&s_lock);
    t->failed = true;
    t->state  = TH_FAILED;
    LightLock_Unlock(&s_lock);
}

void thumb_update(void)
{
    if (!s_inited)
        return;

    for (;;) {
        Thumb *t = NULL;
        LightLock_Lock(&s_lock);
        while (s_dcount > 0) {
            Thumb *c = s_done[s_dhead];
            s_dhead = (s_dhead + 1) % TH_DONE_MAX;
            s_dcount--;
            if (c->state == TH_DECODED) { /* sinon le slot a été reset entre-temps */
                t = c;
                break;
            }
        }
        LightLock_Unlock(&s_lock);
        if (!t)
            break;
        thumb_upload(t);
    }
}

void thumb_release(Thumb *t)
{
    if (!t)
        return;
    if (s_inited)
        LightLock_Lock(&s_lock);
    /* les entrées de file en cours deviennent périmées (state != PENDING),
       et un download en cours sera jeté par le worker (state != LOADING) */
    if (t->pixels)
        free(t->pixels);
    if (t->img.subtex && t->img.subtex != &s_subtex)
        free((void *)t->img.subtex);
    if (t->tex.data)
        C3D_TexDelete(&t->tex);
    memset(t, 0, sizeof(*t));
    if (s_inited)
        LightLock_Unlock(&s_lock);
}
