// scene_player.c: the live on top (video/audio), the IRC chat below
#include <stdio.h>
#include <string.h>
#include <3ds.h>
#include <citro2d.h>

#include "app.h"
#include "ui.h"
#include "config.h"
#include "player.h"
#include "irc.h"

/* Chat geometry (bottom screen 320x240) */
#define CHAT_TOP        20.0f   /* below the header */
#define CHAT_BOTTOM    214.0f   /* above the button band */
#define CHAT_X           4.0f
#define CHAT_W         312.0f
#define CHAT_SCALE       0.42f
#define CHAT_LINE_H     13.0f   /* ~0.42 * 30 px */
#define CHAT_GAP         3.0f
#define CHAT_MAX_LINES   4
#define CHAT_NICK_MAX_W 200.0f

#define CHAT_LOG_MAX 100

/* Button band */
#define BTN_Y        215.0f
#define BTN_H         23.0f
#define BTN_WRITE_X    4.0f
#define BTN_WRITE_W   90.0f
#define BTN_QUIT_X   100.0f
#define BTN_QUIT_W    72.0f

// scene state (one player at a time)
static ChatMsg s_log[CHAT_LOG_MAX];
static int s_head;              // next write slot
static int s_count;
static int s_scroll;            // nb of messages scrolled up from the bottom (0 = stuck at the bottom)

static bool s_overlay;
static u64  s_overlay_hide_at;  // 0 = pas de masquage auto

static bool s_dragging;
static int  s_drag_y;

static int s_rep_up, s_rep_down; // DPAD auto-repeat

// ---- circular log ----

// i = logical index, 0 = oldest message still kept
static const ChatMsg *log_at(int i)
{
    return &s_log[(s_head - s_count + i + 2 * CHAT_LOG_MAX) % CHAT_LOG_MAX];
}

static void log_push(const ChatMsg *m)
{
    s_log[s_head] = *m;
    s_head = (s_head + 1) % CHAT_LOG_MAX;
    if (s_count < CHAT_LOG_MAX) s_count++;
    // if scrolled, we keep the same message in view
    if (s_scroll > 0 && s_scroll < s_count - 1) s_scroll++;
}

static void scroll_by(int d)
{
    s_scroll += d;
    if (s_scroll > s_count - 1) s_scroll = s_count - 1;
    if (s_scroll < 0) s_scroll = 0;
}

// ---- chat-specific wrap ----
// ui_text_wrap always draws, but I need to measure first: I redo
// the word-by-word split with ui_text_width, measure or draw mode.

/* width of a space: lone whitespace measures 0 with C2D, so I go through
   a difference. cached. */
static float chat_space_w(App *a)
{
    static float w = -1.0f;
    if (w < 0.0f) {
        w = ui_text_width(a, CHAT_SCALE, ". .") - ui_text_width(a, CHAT_SCALE, "..");
        if (w <= 0.0f) w = 3.0f;
    }
    return w;
}

/* next word into out (cut exactly on UTF-8 boundaries); returns the pointer
   just after. word too long for the buffer = hard cut, the rest goes on as
   the next word. */
static const char *next_word(const char *s, char *out, size_t out_sz)
{
    while (*s == ' ') s++;
    size_t n = 0;
    while (*s && *s != ' ') {
        unsigned char c = (unsigned char)*s;
        size_t cl = 1;
        if      ((c & 0xE0) == 0xC0) cl = 2;
        else if ((c & 0xF0) == 0xE0) cl = 3;
        else if ((c & 0xF8) == 0xF0) cl = 4;
        if (n + cl >= out_sz) break;
        for (size_t i = 0; i < cl && *s; i++) out[n++] = *s++;
    }
    out[n] = 0;
    return s;
}

// user message: "<nick> " in color, then the wrapped text.
// draw=false => measure only. returns the line count.
static int chat_flow_user(App *a, const ChatMsg *m, float y, bool draw)
{
    const float right = CHAT_X + CHAT_W;
    float sp = chat_space_w(a);

    float nick_w = ui_text_width(a, CHAT_SCALE, m->nick);
    if (nick_w > CHAT_NICK_MAX_W) nick_w = CHAT_NICK_MAX_W;
    if (draw) {
        u32 nc = ui_color_from_hex(m->color);
        if (nick_w < CHAT_NICK_MAX_W)
            ui_text(a, CHAT_X, y, CHAT_SCALE, nc, m->nick);
        else
            ui_text_ellipsis(a, CHAT_X, y, CHAT_SCALE, nc, m->nick, CHAT_NICK_MAX_W);
    }

    int   lines = 1;
    float x = CHAT_X + nick_w + sp;
    const char *p = m->text;
    char w[96];

    while (*p) {
        p = next_word(p, w, sizeof w);
        if (!w[0]) break;
        float ww = ui_text_width(a, CHAT_SCALE, w);

        if (ww > CHAT_W) {
            // word wider than the zone: its own line, truncated
            if (x > CHAT_X) {
                if (lines >= CHAT_MAX_LINES) break;
                lines++;
            }
            if (draw)
                ui_text_ellipsis(a, CHAT_X, y + (lines - 1) * CHAT_LINE_H,
                                 CHAT_SCALE, COL_TEXT, w, CHAT_W);
            x = right; // ligne pleine
            continue;
        }
        if (x + ww > right && x > CHAT_X) {
            if (lines >= CHAT_MAX_LINES) break;
            lines++;
            x = CHAT_X;
        }
        if (draw)
            ui_text(a, x, y + (lines - 1) * CHAT_LINE_H, CHAT_SCALE, COL_TEXT, w);
        x += ww + sp;
    }
    return lines;
}

// system message: centered in grey (no italic available)
static int chat_flow_sys(App *a, const ChatMsg *m, float y, bool draw)
{
    float sp = chat_space_w(a);
    int lines = 0;
    char line[160];
    size_t ln = 0;
    float lw = 0.0f;
    const char *p = m->text;
    char w[96];

    while (*p) {
        p = next_word(p, w, sizeof w);
        if (!w[0]) break;
        float ww = ui_text_width(a, CHAT_SCALE, w);

        if (ln && lw + sp + ww > CHAT_W) {
            lines++;
            if (draw)
                ui_text(a, CHAT_X + (CHAT_W - lw) / 2.0f,
                        y + (lines - 1) * CHAT_LINE_H, CHAT_SCALE, COL_TEXT_DIM, line);
            if (lines >= CHAT_MAX_LINES) return lines;
            ln = 0;
            lw = 0.0f;
        }
        if (ln && ln < sizeof line - 1) { line[ln++] = ' '; lw += sp; }
        size_t wl = strlen(w);
        if (ln + wl > sizeof line - 1) wl = sizeof line - 1 - ln;
        memcpy(line + ln, w, wl);
        ln += wl;
        line[ln] = 0;
        lw += ww;
    }
    if (ln) {
        lines++;
        if (draw) {
            float cw = lw > CHAT_W ? CHAT_W : lw;
            ui_text(a, CHAT_X + (CHAT_W - cw) / 2.0f,
                    y + (lines - 1) * CHAT_LINE_H, CHAT_SCALE, COL_TEXT_DIM, line);
        }
    }
    return lines ? lines : 1;
}

// message height: measured before drawing, to stack from the bottom up
static float chat_msg_h(App *a, const ChatMsg *m)
{
    int lines = m->is_system ? chat_flow_sys(a, m, 0.0f, false)
                             : chat_flow_user(a, m, 0.0f, false);
    return lines * CHAT_LINE_H + CHAT_GAP;
}

// ---- actions ----

static bool pt_in(int px, int py, float x, float y, float w, float h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void chat_compose(App *a)
{
    static char buf[IRC_TEXT_MAX];
    buf[0] = 0;
    if (!app_keyboard("Chat message", buf, sizeof buf))
        return;
    if (irc_send(buf)) {
        // local echo: Twitch does not send our own PRIVMSG back
        ChatMsg m;
        memset(&m, 0, sizeof m);
        snprintf(m.nick, sizeof m.nick, "%.*s", (int)(sizeof m.nick - 1),
                 g_config.username[0] ? g_config.username : "me");
        snprintf(m.text, sizeof m.text, "%s", buf);
        log_push(&m);
        s_scroll = 0; // on recolle en bas
    } else {
        app_set_status(a, "Cannot send (chat disconnected?)");
    }
}

// ---- cycle de vie ----

static void enter(App *a)
{
    s_head = s_count = 0;
    s_scroll = 0;
    s_dragging = false;
    s_rep_up = s_rep_down = 0;
    s_overlay = true;
    s_overlay_hide_at = osGetTime() + 3000;

    Result rc = player_open(a->current.login, g_config.quality, a->is_new3ds);
    if (R_FAILED(rc))
        app_set_status(a, "Cannot play (%08lX)", (unsigned long)rc);

    rc = irc_start(a->current.login, g_config.username, g_config.oauth);
    if (R_FAILED(rc))
        app_set_status(a, "Chat unavailable (%08lX)", (unsigned long)rc);
}

static void leave(App *a)
{
    (void)a;
    player_close();
    irc_stop();
}

static void update(App *a, u32 kDown, u32 kHeld, const touchPosition *touch)
{
    player_update();

    // drain the IRC queue into the local log
    static ChatMsg batch[16];
    int n;
    while ((n = irc_poll(batch, 16)) > 0) {
        for (int i = 0; i < n; i++) log_push(&batch[i]);
        if (n < 16) break;
    }

    // overlay: auto-hides at first, SELECT toggles it
    if (s_overlay_hide_at && osGetTime() >= s_overlay_hide_at) {
        s_overlay = false;
        s_overlay_hide_at = 0;
    }
    if (kDown & KEY_SELECT) {
        s_overlay = !s_overlay;
        s_overlay_hide_at = 0;
    }

    if (kDown & KEY_B) {
        app_switch_scene(a, SCENE_HOME);
        return;
    }

    // DPAD scroll, with auto-repeat (up = older messages)
    if (kHeld & KEY_DUP) {
        if (s_rep_up == 0 || (s_rep_up > 18 && (s_rep_up & 3) == 0)) scroll_by(+1);
        s_rep_up++;
    } else {
        s_rep_up = 0;
    }
    if (kHeld & KEY_DDOWN) {
        if (s_rep_down == 0 || (s_rep_down > 18 && (s_rep_down & 3) == 0)) scroll_by(-1);
        s_rep_down++;
    } else {
        s_rep_down = 0;
    }

    // touch: buttons first, else drag inside the chat
    if (kDown & KEY_TOUCH) {
        int px = touch->px, py = touch->py;
        if (pt_in(px, py, BTN_WRITE_X, BTN_Y, BTN_WRITE_W, BTN_H)) {
            if (irc_can_send())
                chat_compose(a);
            else
                app_set_status(a, "Read-only: set username/oauth");
        } else if (pt_in(px, py, BTN_QUIT_X, BTN_Y, BTN_QUIT_W, BTN_H)) {
            app_switch_scene(a, SCENE_HOME);
        } else if (py >= (int)CHAT_TOP && py < (int)CHAT_BOTTOM) {
            s_dragging = true;
            s_drag_y = py;
        }
    } else if (kHeld & KEY_TOUCH) {
        if (s_dragging) {
            // drag down => scroll up in the history
            int dy = (int)touch->py - s_drag_y;
            int steps = dy / 15;
            if (steps) {
                scroll_by(steps);
                s_drag_y += steps * 15;
            }
        }
    } else {
        s_dragging = false;
    }
}

// ---- rendu ----

static void draw_top(App *a)
{
    const char *dn = a->current.display[0] ? a->current.display : a->current.login;

    if (player_has_video()) {
        // black letterbox, image scaled keeping the aspect ratio
        C2D_DrawRectSolid(0, 0, 0.0f, 400, 240, C2D_Color32(0, 0, 0, 0xFF));
        int vw = 0, vh = 0;
        player_video_dimensions(&vw, &vh);
        if (vw > 0 && vh > 0) {
            float s = 400.0f / (float)vw;
            float sy = 240.0f / (float)vh;
            if (sy < s) s = sy;
            float dw = vw * s, dh = vh * s;
            C2D_DrawImageAt(player_image(), (400.0f - dw) / 2.0f,
                            (240.0f - dh) / 2.0f, 0.1f, NULL, s, s);
        }
    } else {
        C2D_DrawRectSolid(0, 0, 0.0f, 400, 240, COL_PANEL);

        float nw = ui_text_width(a, 0.8f, dn);
        if (nw > 380.0f) nw = 380.0f;
        ui_text_ellipsis(a, (400.0f - nw) / 2.0f, 88, 0.8f, COL_TEXT, dn, nw);

        char st[96];
        st[0] = 0;
        u32 col = COL_TEXT_DIM;
        switch (player_state()) {
            case PLAYER_BUFFERING:
                snprintf(st, sizeof st, "Buffering... %d ms", player_buffered_ms());
                break;
            case PLAYER_PLAYING:
                if (player_is_audio_only()) {
                    snprintf(st, sizeof st, "\xE2\x99\xAA Live audio");
                    col = COL_TEXT;
                } else {
                    snprintf(st, sizeof st, "Playing...");
                }
                break;
            case PLAYER_ENDED:
                snprintf(st, sizeof st, "Stream ended / offline");
                break;
            case PLAYER_ERROR: {
                const char *e = player_error_msg();
                ui_text_wrap(a, 40, 126, 0.5f, COL_RED,
                             (e && e[0]) ? e : "Playback error", 320, 3);
                break;
            }
            default: /* IDLE / CONNECTING */
                snprintf(st, sizeof st, "Connecting...");
                break;
        }
        if (st[0]) {
            float sw = ui_text_width(a, 0.5f, st);
            ui_text(a, (400.0f - sw) / 2.0f, 128, 0.5f, col, st);
        }
    }

    if (s_overlay) {
        C2D_DrawRectSolid(0, 0, 0.5f, 400, 26, C2D_Color32(0, 0, 0, 180));
        ui_text_ellipsis(a, 6, 6, 0.5f, COL_TEXT, dn, 150);
        ui_badge_live(a, 164, 6, a->current.viewers);
        const char *q = player_quality_name();
        char info[64];
        snprintf(info, sizeof info, "%s  %d ms",
                 (q && q[0]) ? q : "-", player_buffered_ms());
        float iw = ui_text_width(a, 0.45f, info);
        ui_text(a, 394.0f - iw, 7, 0.45f, COL_TEXT, info);
    }

}

static void draw_bottom(App *a)
{
    /* from newest (bottom) to oldest; whatever overflows top/bottom is
       covered by the header and band drawn right after */
    if (s_count > 0) {
        int i = s_count - 1 - s_scroll;
        if (i < 0) i = 0;
        float yb = CHAT_BOTTOM;
        for (; i >= 0 && yb > CHAT_TOP; i--) {
            const ChatMsg *m = log_at(i);
            float h = chat_msg_h(a, m);
            float yt = yb - h;
            if (m->is_system)
                chat_flow_sys(a, m, yt + CHAT_GAP, true);
            else
                chat_flow_user(a, m, yt + CHAT_GAP, true);
            yb = yt;
        }
    }

    // purple header
    C2D_DrawRectSolid(0, 0, 0.3f, 320, CHAT_TOP, COL_PURPLE_DARK);
    char hdr[80];
    snprintf(hdr, sizeof hdr, "#%s", a->current.login);
    ui_text_ellipsis(a, 6, 3, 0.45f, COL_TEXT, hdr, 290);
    C2D_DrawCircleSolid(310, 10, 0.3f, 4,
                        irc_connected() ? COL_GREEN : COL_TEXT_DIM);

    // bottom band: drawing only here, presses are handled in update()
    C2D_DrawRectSolid(0, CHAT_BOTTOM, 0.3f, 320, 240.0f - CHAT_BOTTOM, COL_PANEL);
    static const touchPosition no_touch;
    if (irc_can_send()) {
        (void)ui_button(a, BTN_WRITE_X, BTN_Y, BTN_WRITE_W, BTN_H,
                        "Write", false, &no_touch);
    } else {
        ui_panel(BTN_WRITE_X, BTN_Y, BTN_WRITE_W, BTN_H, COL_PANEL2);
        float tw = ui_text_width(a, 0.4f, "Read-only");
        ui_text(a, BTN_WRITE_X + (BTN_WRITE_W - tw) / 2.0f, BTN_Y + 5.0f,
                0.4f, COL_TEXT_DIM, "Read-only");
    }
    (void)ui_button(a, BTN_QUIT_X, BTN_Y, BTN_QUIT_W, BTN_H,
                    "Quit", false, &no_touch);

    const char *hint = "SELECT: info";
    float hw = ui_text_width(a, 0.4f, hint);
    ui_text(a, 314.0f - hw, BTN_Y + 6.0f, 0.4f, COL_TEXT_DIM, hint);
}

Scene scene_player = {
    .enter = enter,
    .leave = leave,
    .update = update,
    .draw_top = draw_top,
    .draw_bottom = draw_bottom,
};
