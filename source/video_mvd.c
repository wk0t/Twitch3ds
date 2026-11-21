// video_mvd.c: H.264 hardware decoding via MVDSTD, New 3DS only.
// (called only from the main thread)
#include <string.h>
#include <3ds.h>
#include <citro2d.h>

#include "video_mvd.h"

#define NAL_BUF_SIZE   (512 * 1024)
#define MAX_TEX_DIM    1024
/* guard against an infinite loop on INCOMPLETEPROCESSING */
#define MAX_INCOMPLETE_RETRY 16

#define TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static struct {
    bool               init;
    bool               tex_init;
    int                w, h;          /* real display dims */
    int                aw, ah;        /* 16-aligned = MVD output */
    int                tex_w, tex_h;  /* pow2 >= aw,ah (texture) */
    u8                *nal_buf;
    u8                *mvd_buf;       /* compact MVD output aw x ah */
    u8                *out_buf;       /* strided buffer tex_w x tex_h */
    u32                out_size;
    C3D_Tex            tex;
    Tex3DS_SubTexture  subtex;
    MVDSTD_Config      config;
} s;

static int next_pow2(int v)
{
    int p = 64; /* minimum comfortable for the GPU / DisplayTransfer */
    while (p < v)
        p <<= 1;
    return p;
}

/* linear buffer -> tiled texture, with PPF wait */
static Result transfer_to_tex(void)
{
    GSPGPU_FlushDataCache(s.out_buf, s.out_size);
    Result r = GX_DisplayTransfer((u32 *)s.out_buf,
                                  GX_BUFFER_DIM((u32)s.tex_w, (u32)s.tex_h),
                                  (u32 *)s.tex.data,
                                  GX_BUFFER_DIM((u32)s.tex_w, (u32)s.tex_h),
                                  TRANSFER_FLAGS);
    if (R_FAILED(r))
        return r;
    gspWaitForPPF();
    return 0;
}

Result video_init(int width, int height)
{
    if (s.init)
        video_exit();

    if (width <= 0 || height <= 0)
        return MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_APPLICATION, RD_INVALID_SIZE);

    /* MVD wants 16-aligned output dims (H.264 macroblocks) equal to the input,
     * else mvdstdRenderVideoFrame returns INVALIDARG. */
    int aw = (width  + 15) & ~15;
    int ah = (height + 15) & ~15;
    int tw = next_pow2(aw);
    int th = next_pow2(ah);
    if (tw > MAX_TEX_DIM || th > MAX_TEX_DIM)
        return MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_APPLICATION, RD_INVALID_SIZE);

    memset(&s, 0, sizeof(s));
    s.w = width;
    s.h = height;
    s.aw = aw;
    s.ah = ah;
    s.tex_w = tw;
    s.tex_h = th;
    s.out_size = (u32)tw * (u32)th * 2;

    s.nal_buf = linearMemAlign(NAL_BUF_SIZE, 0x40);
    s.mvd_buf = linearMemAlign((u32)aw * (u32)ah * 2, 0x40);
    s.out_buf = linearMemAlign(s.out_size, 0x40);
    if (!s.nal_buf || !s.mvd_buf || !s.out_buf) {
        video_exit();
        return MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_APPLICATION, RD_OUT_OF_MEMORY);
    }
    memset(s.mvd_buf, 0, (u32)aw * (u32)ah * 2);
    memset(s.out_buf, 0, s.out_size); /* first frame: black */

    /* default workbuf (~9.4 MiB, the N3DS browser one): ok up to 480p */
    Result r = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_RGB565,
                          MVD_DEFAULT_WORKBUF_SIZE, NULL);
    if (R_FAILED(r)) {
        video_exit();
        return r;
    }
    s.init = true; /* from here on, mvdstdExit needed on failure */

    /* in == out == aligned dims; MVD writes compact aw x ah (stride aw)
     * into mvd_buf, we then repack into out_buf (stride tex_w). */
    mvdstdGenerateDefaultConfig(&s.config, (u32)aw, (u32)ah, (u32)aw, (u32)ah, NULL,
                                (u32 *)s.mvd_buf, (u32 *)s.mvd_buf);

    /* texture in VRAM preferably (DisplayTransfer writes there without CPU) */
    if (!C3D_TexInitVRAM(&s.tex, (u16)tw, (u16)th, GPU_RGB565) &&
        !C3D_TexInit(&s.tex, (u16)tw, (u16)th, GPU_RGB565)) {
        video_exit();
        return MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_APPLICATION, RD_OUT_OF_MEMORY);
    }
    s.tex_init = true;
    C3D_TexSetFilter(&s.tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&s.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    /* citro2d: t axis flipped, t=1.0 = row 0 */
    s.subtex.width  = (u16)width;
    s.subtex.height = (u16)height;
    s.subtex.left   = 0.0f;
    s.subtex.top    = 1.0f;
    s.subtex.right  = (float)width  / (float)tw;
    s.subtex.bottom = 1.0f - (float)height / (float)th;

    /* black texture: something displayable before the 1st frame */
    transfer_to_tex();

    return 0;
}

void video_exit(void)
{
    if (s.init)
        mvdstdExit();
    if (s.tex_init)
        C3D_TexDelete(&s.tex);
    if (s.nal_buf)
        linearFree(s.nal_buf);
    if (s.mvd_buf)
        linearFree(s.mvd_buf);
    if (s.out_buf)
        linearFree(s.out_buf);
    memset(&s, 0, sizeof(s));
}

bool video_ready(void)
{
    return s.init;
}

/* next start code 00 00 01 in [from, len), else len */
static size_t find_startcode(const u8 *p, size_t len, size_t from)
{
    for (size_t i = from; i + 2 < len; i++) {
        if (p[i] == 0x00 && p[i + 1] == 0x00 && p[i + 2] == 0x01)
            return i;
    }
    return len;
}

/* sends one "00 00 01 <nal>" unit to MVD (the 3-byte prefix is part of the
   input). Handles INCOMPLETEPROCESSING by resubmitting the unconsumed tail. */
static Result process_nal(const u8 *unit, size_t size)
{
    memcpy(s.nal_buf, unit, size);
    GSPGPU_FlushDataCache(s.nal_buf, size);

    MVDSTD_ProcessNALUnitOut po;
    memset(&po, 0, sizeof(po));
    Result r = mvdstdProcessVideoFrame(s.nal_buf, size, 0, &po);

    int guard = 0;
    size_t cur = size;
    while (r == MVD_STATUS_INCOMPLETEPROCESSING && guard++ < MAX_INCOMPLETE_RETRY) {
        size_t rem = po.remaining_size;
        if (rem == 0 || rem >= cur)
            break; /* no progress, drop this unit */
        /* the unconsumed rest is at the buffer end, move it back to the front */
        memmove(s.nal_buf, s.nal_buf + (cur - rem), rem);
        GSPGPU_FlushDataCache(s.nal_buf, rem);
        cur = rem;
        memset(&po, 0, sizeof(po));
        r = mvdstdProcessVideoFrame(s.nal_buf, cur, 0, &po);
    }
    return r;
}

int video_decode_au(const u8 *au, size_t len)
{
    if (!s.init)
        return -1;
    if (!au || len < 4)
        return 0;

    size_t start = find_startcode(au, len, 0);
    if (start >= len)
        return 0; /* no Annex-B NAL here */

    bool frame_ready = false;

    while (start < len) {
        /* end of unit = start of the next prefix (00 00 01, or 00 00 00 01
           whose leading zero we drop) */
        size_t next = find_startcode(au, len, start + 3);
        size_t end = next;
        if (next < len && au[next - 1] == 0x00 && next - 1 > start + 2)
            end = next - 1;

        size_t unit_size = end - start; /* includes the 3 bytes 00 00 01 */
        bool last = (next >= len);

        if (unit_size > 3) { /* skip empty units (prefix alone) */
            if (unit_size > NAL_BUF_SIZE)
                return -2; /* NAL too big for nal_buf */

            Result r = process_nal(au + start, unit_size);
            if (!MVD_CHECKNALUPROC_SUCCESS(r))
                return -3;

            /* complete frame: FRAMEREADY, or OK on the last NAL (slice) */
            if (r == MVD_STATUS_FRAMEREADY || (last && r == MVD_STATUS_OK))
                frame_ready = true;
        }

        start = next;
    }

    if (!frame_ready)
        return 0; /* only SPS/PPS/SEI, nothing to display */

    Result r = mvdstdRenderVideoFrame(&s.config, true);
    if (r != MVD_STATUS_OK)
        return -4;

    /* MVD wrote compact aw x ah (stride aw) into mvd_buf; we copy it
     * line by line into out_buf at stride tex_w (the rest keeps its black).
     * Flush to read on the CPU what MVD just wrote in hardware. */
    GSPGPU_FlushDataCache(s.mvd_buf, (u32)s.aw * (u32)s.ah * 2);
    for (int row = 0; row < s.ah; row++)
        memcpy(s.out_buf + (size_t)row * s.tex_w * 2,
               s.mvd_buf + (size_t)row * s.aw * 2,
               (size_t)s.aw * 2);

    if (R_FAILED(transfer_to_tex()))
        return -5;

    return 1;
}

C2D_Image video_image(void)
{
    C2D_Image img = { &s.tex, &s.subtex };
    return img;
}

void video_dimensions(int *w, int *h)
{
    if (w) *w = s.w;
    if (h) *h = s.h;
}
