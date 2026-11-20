// ts_demux.c: MPEG-TS demux of Twitch HLS segments (H.264 video + AAC audio)
// PAT/PMT read dynamically; we only reassemble on PUSI (at Twitch one
// section fits in one packet).

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "ts_demux.h"

#define TS_PKT      188
#define TS_VBUF_CAP (512 * 1024)  // large: at real 160p one AU is ~50 KiB
#define TS_ABUF_CAP (64 * 1024)
#define PTS_UNSET   ((s64)-1)
#define PTS_QSIZE   64            // pending PTS queue (0-1 in practice)

// audio PES assembler: accumulates a payload and emits it whole
typedef struct {
    u8    *buf;
    size_t cap;
    size_t len;
    bool   active;
    s64    pts;
} PesAsm;

static struct {
    TsVideoCb vcb;
    TsAudioCb acb;
    void     *user;

    u8     partial[TS_PKT];
    size_t partial_len;

    int pid_pmt;
    int pid_video;
    int pid_audio;

    // video assembler (cut at AUDs)
    u8    *vbuf;
    size_t vcap;
    size_t vlen;
    bool   v_open;        // vbuf holds an AU started at an AUD
    s64    v_pts;
    int    v_zeros;       // consecutive zeros, to spot a start code
    bool   v_wait_hdr;    // start code seen, next byte = NAL header

    s64    pts_q[PTS_QSIZE];
    int    pts_head, pts_tail;

    // audio assembler (per PES)
    PesAsm aasm;

    u32 err_count;
} S = { .pid_pmt = -1, .pid_video = -1, .pid_audio = -1 };

/* --- PTS queue ---------------------------------------------------------- */

static void pts_push(s64 pts)
{
    int n = (S.pts_tail + 1) % PTS_QSIZE;
    if (n == S.pts_head)             // full: drop the oldest
        S.pts_head = (S.pts_head + 1) % PTS_QSIZE;
    S.pts_q[S.pts_tail] = pts;
    S.pts_tail = n;
}

static s64 pts_pop(void)
{
    if (S.pts_head == S.pts_tail)
        return PTS_UNSET;
    s64 p = S.pts_q[S.pts_head];
    S.pts_head = (S.pts_head + 1) % PTS_QSIZE;
    return p;
}

static void pts_clear(void)
{
    S.pts_head = S.pts_tail = 0;
}

/* --- video assembler ---------------------------------------------------- */

static void video_reset(void)
{
    S.vlen = 0;
    S.v_open = false;
    S.v_pts = PTS_UNSET;
    S.v_zeros = 0;
    S.v_wait_hdr = false;
    pts_clear();
}

static void video_emit(size_t len)
{
    if (S.v_open && len > 0 && S.vcb)
        S.vcb(S.vbuf, len, S.v_pts, S.user);
}

// Accumulates the H.264 stream in vbuf and cuts it at AUDs (NAL type 9): at each
// AUD we emit the previous AU and restart at the start code.
// We have to do it this way: Twitch PES are not always aligned, so "1 PES = 1 AU"
// would give buffers that do not start with a start code.
static void video_feed(const u8 *d, size_t n)
{
    if (!S.vbuf)
        return;

    for (size_t i = 0; i < n; i++) {
        u8 b = d[i];

        // overflow: drop the current AU but keep the sync
        if (S.vlen >= S.vcap) {
            S.err_count++;
            S.vlen = 0;
            S.v_open = false;
            S.v_zeros = 0;
            S.v_wait_hdr = false;
        }
        S.vbuf[S.vlen++] = b;

        if (S.v_wait_hdr) {
            S.v_wait_hdr = false;
            int nal_type = b & 0x1F;
            if (nal_type == 9) {                 // AUD = AU boundary
                // start code (00 00 01) at [vlen-4..vlen-2], header at vlen-1
                size_t sc_pos = S.vlen - 4;
                if (S.v_open && sc_pos > 0)
                    video_emit(sc_pos);
                // move start code + header back to the front for the new AU
                memmove(S.vbuf, S.vbuf + sc_pos, S.vlen - sc_pos);
                S.vlen -= sc_pos;
                S.v_open = true;
                S.v_pts = pts_pop();
            }
        }

        if (b == 0x00) {
            S.v_zeros++;
        } else if (b == 0x01 && S.v_zeros >= 2) {
            S.v_wait_hdr = true;
            S.v_zeros = 0;
        } else {
            S.v_zeros = 0;
        }
    }
}

// video packet: on PUSI skip the PES header and push the PTS
static void video_input(const u8 *p, size_t n, bool pusi)
{
    if (!pusi) {
        video_feed(p, n);
        return;
    }
    if (n < 9 || p[0] != 0x00 || p[1] != 0x00 || p[2] != 0x01) {
        S.err_count++;
        return;
    }
    u8 sid = p[3];
    if (sid == 0xBE || sid == 0xBF)
        return;

    u8     hdl = p[8];
    size_t off = 9 + (size_t)hdl;
    if (off > n) {
        S.err_count++;
        return;
    }
    if (p[7] & 0x80) {                            // PTS present
        s64 pts = ((s64)((p[9] >> 1) & 0x07) << 30)
                |  ((s64)p[10] << 22)
                |  ((s64)((p[11] >> 1) & 0x7FFF) << 15)
                |  ((s64)p[12] << 7)
                |  ((s64)(p[13] >> 1));
        pts_push(pts);
    }
    if (off < n)
        video_feed(p + off, n - off);
}

/* --- audio assembler (per PES) ------------------------------------------ */

static void aud_reset(PesAsm *a)
{
    a->active = false;
    a->len    = 0;
    a->pts    = PTS_UNSET;
}

static void aud_emit(PesAsm *a)
{
    if (a->active && a->len > 0 && S.acb)
        S.acb(a->buf, a->len, a->pts, S.user);
    aud_reset(a);
}

static void aud_append(PesAsm *a, const u8 *d, size_t n)
{
    if (!a->buf || a->len + n > a->cap) {
        S.err_count++;
        aud_reset(a);
        return;
    }
    memcpy(a->buf + a->len, d, n);
    a->len += n;
}

static void audio_input(PesAsm *a, const u8 *p, size_t n, bool pusi)
{
    if (!pusi) {
        if (a->active)
            aud_append(a, p, n);
        return;
    }
    aud_emit(a);

    if (n < 9 || p[0] != 0x00 || p[1] != 0x00 || p[2] != 0x01) {
        S.err_count++;
        return;
    }
    u8 sid = p[3];
    if (sid == 0xBE || sid == 0xBF)
        return;

    u8     hdl = p[8];
    size_t off = 9 + (size_t)hdl;
    if (off > n) {
        S.err_count++;
        return;
    }
    s64 pts = PTS_UNSET;
    if ((p[7] & 0x80) && hdl >= 5) {
        pts = ((s64)((p[9] >> 1) & 0x07) << 30)
            |  ((s64)p[10] << 22)
            |  ((s64)((p[11] >> 1) & 0x7FFF) << 15)
            |  ((s64)p[12] << 7)
            |  ((s64)(p[13] >> 1));
    }
    a->active = true;
    a->pts    = pts;
    if (off < n)
        aud_append(a, p + off, n - off);
}

/* --- PSI sections ------------------------------------------------------- */

static void parse_pat(const u8 *p, size_t n)
{
    if (n < 1)
        return;
    size_t ptr = p[0];
    if (1 + ptr >= n) {
        S.err_count++;
        return;
    }
    const u8 *s     = p + 1 + ptr;
    size_t    avail = n - 1 - ptr;
    if (avail < 3 || s[0] != 0x00)
        return;
    size_t sec_len = (size_t)(((s[1] & 0x0F) << 8) | s[2]);
    if (sec_len < 9 || 3 + sec_len > avail) {
        S.err_count++;
        return;
    }
    const u8 *e   = s + 8;
    const u8 *end = s + 3 + sec_len - 4;
    for (; e + 4 <= end; e += 4) {
        int prog = (e[0] << 8) | e[1];
        if (prog != 0) {
            S.pid_pmt = ((e[2] & 0x1F) << 8) | e[3];
            return;
        }
    }
}

static void parse_pmt(const u8 *p, size_t n)
{
    if (n < 1)
        return;
    size_t ptr = p[0];
    if (1 + ptr >= n) {
        S.err_count++;
        return;
    }
    const u8 *s     = p + 1 + ptr;
    size_t    avail = n - 1 - ptr;
    if (avail < 3 || s[0] != 0x02)
        return;
    size_t sec_len = (size_t)(((s[1] & 0x0F) << 8) | s[2]);
    if (sec_len < 13 || 3 + sec_len > avail) {
        S.err_count++;
        return;
    }
    size_t pil = (size_t)(((s[10] & 0x0F) << 8) | s[11]);
    const u8 *e   = s + 12 + pil;
    const u8 *end = s + 3 + sec_len - 4;
    if (e > end) {
        S.err_count++;
        return;
    }

    int vid = -1, aud = -1;
    while (e + 5 <= end) {
        u8     st  = e[0];
        int    pid = ((e[1] & 0x1F) << 8) | e[2];
        size_t il  = (size_t)(((e[3] & 0x0F) << 8) | e[4]);
        if      (st == 0x1B && vid < 0) vid = pid;   // H.264
        else if (st == 0x0F && aud < 0) aud = pid;   // AAC ADTS
        e += 5 + il;
    }
    if (vid >= 0 && vid != S.pid_video) { S.pid_video = vid; video_reset(); }
    if (aud >= 0 && aud != S.pid_audio) { S.pid_audio = aud; aud_reset(&S.aasm); }
}

/* --- TS packet ---------------------------------------------------------- */

static void process_packet(const u8 *b)
{
    if (b[1] & 0x80) {
        S.err_count++;
        return;
    }

    bool pusi = (b[1] & 0x40) != 0;
    int  pid  = ((b[1] & 0x1F) << 8) | b[2];
    u8   afc  = (b[3] >> 4) & 0x3;

    const u8 *p   = b + 4;
    const u8 *end = b + TS_PKT;
    if (afc & 0x2) {
        p = b + 5 + b[4];
        if (p > end) {
            S.err_count++;
            return;
        }
    }
    if (!(afc & 0x1) || p >= end)
        return;
    size_t n = (size_t)(end - p);

    if (pid == 0x1FFF)
        return;
    if (pid == 0x0000) {
        if (pusi) parse_pat(p, n);
    } else if (pid == S.pid_pmt) {
        if (pusi) parse_pmt(p, n);
    } else if (pid == S.pid_video) {
        video_input(p, n, pusi);
    } else if (pid == S.pid_audio) {
        audio_input(&S.aasm, p, n, pusi);
    }
}

/* --- public API --------------------------------------------------------- */

void ts_demux_init(TsVideoCb vcb, TsAudioCb acb, void *user)
{
    S.vcb  = vcb;
    S.acb  = acb;
    S.user = user;

    if (!S.vbuf) {
        S.vbuf = malloc(TS_VBUF_CAP);
        S.vcap = S.vbuf ? TS_VBUF_CAP : 0;
    }
    if (!S.aasm.buf) {
        S.aasm.buf = malloc(TS_ABUF_CAP);
        S.aasm.cap = S.aasm.buf ? TS_ABUF_CAP : 0;
    }

    ts_demux_reset();
}

void ts_demux_reset(void)
{
    S.partial_len = 0;
    S.pid_pmt     = -1;
    S.pid_video   = -1;
    S.pid_audio   = -1;
    S.err_count   = 0;
    video_reset();
    aud_reset(&S.aasm);
}

int ts_demux_feed(const u8 *data, size_t len)
{
    if (!S.vbuf || !S.aasm.buf)
        return -1;
    if (!data)
        return len ? -1 : 0;

    size_t i = 0;

    if (S.partial_len > 0) {
        size_t need = TS_PKT - S.partial_len;
        size_t take = (len < need) ? len : need;
        memcpy(S.partial + S.partial_len, data, take);
        S.partial_len += take;
        i = take;
        if (S.partial_len < TS_PKT)
            return 0;
        S.partial_len = 0;
        process_packet(S.partial);
    }

    while (i < len) {
        if (data[i] != 0x47) {
            size_t j = i + 1;
            while (j < len && !(data[j] == 0x47 &&
                                (j + TS_PKT >= len || data[j + TS_PKT] == 0x47)))
                j++;
            S.err_count++;
            i = j;
            if (i >= len)
                break;
        }
        if (len - i >= TS_PKT) {
            process_packet(data + i);
            i += TS_PKT;
        } else {
            memcpy(S.partial, data + i, len - i);
            S.partial_len = len - i;
            break;
        }
    }
    return 0;
}

void ts_demux_flush(void)
{
    // end of segment: emit the video AU and the audio PES still open
    if (S.v_open && S.vlen > 0)
        video_emit(S.vlen);
    video_reset();

    aud_emit(&S.aasm);
    S.partial_len = 0;
}

void ts_demux_exit(void)
{
    free(S.vbuf);
    free(S.aasm.buf);
    memset(&S, 0, sizeof(S));
    S.pid_pmt = S.pid_video = S.pid_audio = -1;
}
