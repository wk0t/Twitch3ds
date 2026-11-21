// thumb.c: stream thumbnails (JPEG 320x180) loaded in the background.
// worker that fetches+decodes; C3D textures are created only on the main thread.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include <3ds.h>
#include <jpeglib.h>

#include "thumb.h"
#include "net_http.h"

/* state of a slot (t->state) */
enum {
    TH_IDLE = 0,
    TH_PENDING,    /* queued, waiting for the worker */
    TH_LOADING,
    TH_DECODED,    /* pixels ready, texture left to create (main thread) */
    TH_READY,
    TH_FAILED,
};

#define TH_TEXW      512
#define TH_TEXH      256
#define TH_QUEUE_MAX 8
#define TH_DONE_MAX  32
#define TH_STACK     (64 * 1024)
#define TH_URL_CAP   (sizeof(((Thumb *)0)->url))

// linear -> tiled RGB565, no scaling
#define TH_XFER_FLAGS                                        \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |   \
     GX_TRANSFER_RAW_COPY(0) |                               \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |         \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |        \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

/* Twitch thumbnails are 320x180, placed on a 512x256 texture */
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
static HttpCtx   *s_http;   /* kept by init/exit so we can cancel it */

/* request queue (ring) for the worker */
static Thumb *s_queue[TH_QUEUE_MAX];
static int    s_qhead, s_qcount;

/* queue of decoded slots, to turn into textures */
static Thumb *s_done[TH_DONE_MAX];
static int    s_dhead, s_dcount;

/* queues, always handled under s_lock */

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

/* JPEG decoding (worker side) */

typedef struct {
    struct jpeg_error_mgr mgr;
    jmp_buf jb;
} ThJpegErr;

/* libjpeg's default error_exit calls exit(), so we longjmp instead */
static void th_jpeg_error_exit(j_common_ptr cinfo)
{
    ThJpegErr *e = (ThJpegErr *)cinfo->err;
    longjmp(e->jb, 1);
}

static void th_jpeg_silent(j_common_ptr cinfo)
{
    (void)cinfo; /* no logging to stderr */
}

static bool thumb_decode_jpeg(const u8 *data, size_t len,
                              u16 **out_px, int *out_w, int *out_h)
{
    struct jpeg_decompress_struct cinfo;
    ThJpegErr jerr;
    /* volatile: re-read after the longjmp from the error handler */
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
            longjmp(jerr.jb, 1); /* truncated stream */
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

/* worker: network + decoding. Never C3D/GX here (main thread only). */

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

            /* takes the next still-valid request */
            Thumb *t = NULL;
            char url[TH_URL_CAP];
            LightLock_Lock(&s_lock);
            while (s_qcount > 0) {
                Thumb *c = s_queue[s_qhead];
                s_qhead = (s_qhead + 1) % TH_QUEUE_MAX;
                s_qcount--;
                if (c->state == TH_PENDING) { /* else the entry is stale */
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
                        pixels = NULL; /* the slot owns it now */
                    } else {
                        t->failed = true;
                        t->state  = TH_FAILED;
                    }
                } else {
                    /* the URL changed along the way: drop it, the slot becomes requestable again */
                    t->state = TH_IDLE;
                }
            }
            /* else the slot was reset/released meanwhile, drop everything */
            LightLock_Unlock(&s_lock);
            free(pixels);
        }
    }

    http_buf_free(&buf);
    // s_http belongs to thumb_system_exit, do not touch it here
}

/* public API, called from the main thread */

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

    /* target core 2 (New 3DS), else syscore 1, else the app core.
       The worker only runs during browsing (player closed), so core 1
       is free at that time. */
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
        /* aborts an ongoing http_get (else up to 15 s freeze on exit) */
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
    if (!s_thread) { /* no worker: fail directly */
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
        return; /* already ready or already loading for this URL */
    }

    if (t->state == TH_PENDING) {
        /* already queued, the worker will pick the new URL at dequeue */
        memcpy(t->url, urlbuf, TH_URL_CAP);
        t->failed = false;
        LightLock_Unlock(&s_lock);
        return;
    }

    if (t->state == TH_LOADING) {
        /* an old download is still running, it will be dropped on arrival */
        memcpy(t->url, urlbuf, TH_URL_CAP);
        t->failed = false;
        if (queue_push(t))
            t->state = TH_PENDING;
        /* if the queue is full we stay LOADING, the worker will set it back to
           IDLE (different URL) and a later call will retry */
        LightLock_Unlock(&s_lock);
        return;
    }

    /* idle slot: reset everything */
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
        t->state = TH_IDLE; /* queue full, retry on the next call */
        LightLock_Unlock(&s_lock);
    }
}

/* pixels -> texture. t is no longer in the queues, the main thread owns it. */
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

    /* shared subtex for the usual 320x180, else we allocate one */
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
            if (c->state == TH_DECODED) { /* else the slot was reset meanwhile */
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
    /* queued entries become stale (state != PENDING),
       and an ongoing download will be dropped by the worker (state != LOADING) */
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
