// scene_home.c : top streams + toute la logique de liste (browse_*) que scene_search reprend telle quelle
#include <stdio.h>
#include <string.h>
#include <3ds.h>
#include <citro2d.h>

#include "app.h"
#include "ui.h"
#include "twitch.h"
#include "thumb.h"
#include "config.h"
#include "update.h"

/* géométrie de la liste, écran bas 320x240 */
#define HEADER_H   24
#define LIST_Y     26
#define ROW_PITCH  42
#define ROW_H      40
#define ROW_X      2
#define ROW_W      313
#define VISIBLE    5
#define BAND_Y     216     /* le bandeau de hints commence ici */

// liste courante issue d'une recherche -> home rechargera le top au retour
static bool s_list_foreign = false;

/* etat DPAD/double-tap, une seule liste active a la fois */
static int s_repeat    = 0;
static u32 s_frame     = 0;
static u32 s_tap_frame = 0;
static int s_tap_index = -1;

/* --- helpers partagés avec scene_search.c --- */

HttpCtx *browse_http(App *a)
{
    // main.c ne remplit pas a->http, on le crée à la demande.
    // le header Client-ID est posé par twitch.c, pas la peine de le refaire ici.
    if (!a->http)
        a->http = http_ctx_new();
    return a->http;
}

void browse_release_thumbs(App *a)
{
    for (int i = 0; i < APP_MAX_STREAMS; i++)
        thumb_release(&a->thumbs[i]);
}

void browse_mark_foreign_list(void)
{
    s_list_foreign = true;
}

/* "12345" -> "12 345" */
static void fmt_viewers(char *dst, size_t cap, int v)
{
    char raw[16];
    int n = snprintf(raw, sizeof(raw), "%d", v < 0 ? 0 : v);
    size_t out = 0;
    for (int i = 0; i < n && out + 1 < cap; i++) {
        if (i > 0 && (n - i) % 3 == 0 && out + 2 < cap)
            dst[out++] = ' ';
        dst[out++] = raw[i];
    }
    dst[out] = '\0';
}

void browse_update(App *a, u32 kDown, u32 kHeld, const touchPosition *touch,
                   void (*reload)(App *a))
{
    s_frame++;

    /* DPAD haut/bas : delai 15 frames avant la repet, puis un cran tous les 4 */
    int dir = 0;
    if (kDown & KEY_DOWN)    { dir =  1; s_repeat = 0; }
    else if (kDown & KEY_UP) { dir = -1; s_repeat = 0; }
    else if (kHeld & (KEY_DOWN | KEY_UP)) {
        s_repeat++;
        if (s_repeat >= 15 && (s_repeat - 15) % 4 == 0)
            dir = (kHeld & KEY_DOWN) ? 1 : -1;
    } else {
        s_repeat = 0;
    }

    if (dir != 0 && a->stream_count > 0)
        a->sel += dir;

    /* tap = selection, double tap (< 30 frames) = lecture */
    if ((kDown & KEY_TOUCH) && touch &&
        touch->py >= LIST_Y && touch->py < BAND_Y) {
        int idx = a->scroll + (touch->py - LIST_Y) / ROW_PITCH;
        if (idx >= 0 && idx < a->stream_count) {
            if (idx == s_tap_index && s_frame - s_tap_frame < 30) {
                a->sel = idx;
                a->current = a->streams[idx];
                app_switch_scene(a, SCENE_PLAYER);
                s_tap_index = -1;
            } else {
                a->sel = idx;
                s_tap_index = idx;
                s_tap_frame = s_frame;
            }
        }
    }

    /* on reborne sel/scroll, le nombre de streams a pu bouger entre deux frames */
    if (a->sel < 0) a->sel = 0;
    if (a->sel >= a->stream_count) a->sel = a->stream_count - 1;
    if (a->sel < 0) a->sel = 0;
    if (a->scroll > a->sel) a->scroll = a->sel;
    if (a->sel >= a->scroll + VISIBLE) a->scroll = a->sel - VISIBLE + 1;
    int max_scroll = a->stream_count - VISIBLE;
    if (max_scroll < 0) max_scroll = 0;
    if (a->scroll > max_scroll) a->scroll = max_scroll;
    if (a->scroll < 0) a->scroll = 0;

    if ((kDown & KEY_A) && a->stream_count > 0) {
        a->current = a->streams[a->sel];
        app_switch_scene(a, SCENE_PLAYER);
    }
    if ((kDown & KEY_X) && reload)
        reload(a);
    if (kDown & KEY_Y)
        app_switch_scene(a, SCENE_SEARCH);
    if (kDown & KEY_SELECT)
        app_switch_scene(a, SCENE_LOGIN);

    if ((kDown & KEY_R) && update_available()) {
        // bloquant ~qq secondes, l'ecran se fige le temps du download
        int r = update_download();
        app_set_status(a, r == 0 ? "Maj installee ! Relance l'app."
                                 : "Echec du telechargement de la maj");
    }

    /* charge la miniature du selectionne (no-op si meme URL) */
    if (a->stream_count > 0 && a->streams[a->sel].preview_url[0])
        thumb_request(&a->thumbs[a->sel], a->streams[a->sel].preview_url);
}

void browse_draw_top(App *a)
{
    if (a->stream_count == 0) {
        /* ecran vide : logo + statut */
        const char *logo = "Twitch 3DS";
        float w = ui_text_width(a, 1.0f, logo);
        ui_text(a, (400.0f - w) / 2.0f, 90.0f, 1.0f, COL_PURPLE, logo);
        ui_text(a, (400.0f - w) / 2.0f + 0.5f, 90.0f, 1.0f, COL_PURPLE, logo);
        if (a->status[0]) {
            float sw = ui_text_width(a, 0.45f, a->status);
            ui_text(a, (400.0f - sw) / 2.0f, 130.0f, 0.45f, COL_TEXT_DIM,
                    a->status);
        }
        char acc[96];
        if (g_config.username[0] && g_config.oauth[0])
            snprintf(acc, sizeof acc, "Connecte : %s", g_config.username);
        else
            snprintf(acc, sizeof acc, "Non connecte  -  SELECT pour se connecter");
        float aw = ui_text_width(a, 0.4f, acc);
        u32 acol = (g_config.username[0] && g_config.oauth[0]) ? COL_GREEN : COL_TEXT_DIM;
        ui_text(a, (400.0f - aw) / 2.0f, 165.0f, 0.4f, acol, acc);

        if (update_available()) {
            char up[96];
            snprintf(up, sizeof up, "Mise a jour %s dispo  -  R pour installer",
                     update_latest());
            float uw = ui_text_width(a, 0.42f, up);
            ui_text(a, (400.0f - uw) / 2.0f, 192.0f, 0.42f, COL_GREEN, up);
        }
        return;
    }

    int sel = a->sel;
    if (sel < 0) sel = 0;
    if (sel >= a->stream_count) sel = a->stream_count - 1;
    const TwitchStream *s = &a->streams[sel];
    Thumb *t = &a->thumbs[sel];

    /* nom + badge LIVE. le nom est dessine 2x decale de 0.5px pour faire du gras */
    ui_badge_live(a, 274.0f, 5.0f, s->viewers);
    float name_max = 274.0f - 40.0f - 8.0f;
    ui_text_ellipsis(a, 40.0f, 3.0f, 0.55f, COL_TEXT, s->display, name_max);
    ui_text_ellipsis(a, 40.5f, 3.0f, 0.55f, COL_TEXT, s->display, name_max);

    /* miniature 320x180 */
    if (t->ready) {
        C2D_DrawImageAt(t->img, 40.0f, 24.0f, 0.5f, NULL, 1.0f, 1.0f);
    } else {
        ui_panel(40.0f, 24.0f, 320.0f, 180.0f, COL_PANEL);
        float w = ui_text_width(a, 0.7f, "...");
        ui_text(a, (400.0f - w) / 2.0f, 103.0f, 0.7f, COL_TEXT_DIM, "...");
    }

    /* jeu + titre sous la miniature (titre sur 2 lignes max) */
    ui_text_ellipsis(a, 40.0f, 205.0f, 0.38f, COL_PURPLE, s->game, 320.0f);
    ui_text_wrap(a, 40.0f, 217.0f, 0.35f, COL_TEXT_DIM, s->title, 320.0f, 2);
}

void browse_draw_bottom(App *a)
{
    /* en-tete */
    ui_panel(0.0f, 0.0f, 320.0f, HEADER_H, COL_PURPLE);
    float hint_w = ui_text_width(a, 0.4f, "Y: recherche");
    ui_text(a, 320.0f - 6.0f - hint_w, 6.0f, 0.4f, COL_TEXT, "Y: recherche");
    ui_text_ellipsis(a, 6.0f, 4.0f, 0.5f, COL_TEXT, a->list_title,
                     320.0f - 18.0f - hint_w);

    if (a->stream_count == 0) {
        const char *msg = "Aucun stream (X : actualiser)";
        float w = ui_text_width(a, 0.45f, msg);
        ui_text(a, (320.0f - w) / 2.0f, 110.0f, 0.45f, COL_TEXT_DIM, msg);
    }

    for (int i = a->scroll;
         i < a->stream_count && i < a->scroll + VISIBLE; i++) {
        const TwitchStream *s = &a->streams[i];
        float y = LIST_Y + (float)(i - a->scroll) * ROW_PITCH;
        ui_panel(ROW_X, y, ROW_W, ROW_H,
                 (i == a->sel) ? COL_PURPLE_DARK : COL_PANEL);

        /* viewers a droite : point rouge + nombre */
        char vbuf[24];
        fmt_viewers(vbuf, sizeof(vbuf), s->viewers);
        float dot_w = ui_text_width(a, 0.4f, "\u25CF ");
        float num_w = ui_text_width(a, 0.4f, vbuf);
        float vx = ROW_X + ROW_W - 6.0f - dot_w - num_w;
        ui_text(a, vx, y + 4.0f, 0.4f, COL_RED, "\u25CF ");
        ui_text(a, vx + dot_w, y + 4.0f, 0.4f, COL_TEXT, vbuf);

        /* nom en gras (double passe) puis titre et jeu dessous */
        float name_max = vx - (ROW_X + 6.0f) - 6.0f;
        ui_text_ellipsis(a, ROW_X + 6.0f, y + 2.0f, 0.5f, COL_TEXT,
                         s->display, name_max);
        ui_text_ellipsis(a, ROW_X + 6.5f, y + 2.0f, 0.5f, COL_TEXT,
                         s->display, name_max);

        ui_text_ellipsis(a, ROW_X + 6.0f, y + 18.0f, 0.38f, COL_TEXT_DIM,
                         s->title, ROW_W - 12.0f);
        ui_text_ellipsis(a, ROW_X + 6.0f, y + 29.0f, 0.35f, COL_PURPLE,
                         s->game, ROW_W - 12.0f);
    }

    /* scrollbar */
    if (a->stream_count > VISIBLE) {
        float track_h = (float)(BAND_Y - LIST_Y);
        float th = track_h * (float)VISIBLE / (float)a->stream_count;
        if (th < 8.0f) th = 8.0f;
        float ty = LIST_Y + (track_h - th) * (float)a->scroll
                          / (float)(a->stream_count - VISIBLE);
        ui_panel(317.0f, ty, 2.0f, th, COL_PURPLE);
    }

    /* bandeau bas : recouvre le debord de la 5e ligne et affiche les raccourcis */
    ui_panel(0.0f, BAND_Y, 320.0f, 240.0f - BAND_Y, COL_BG);
    if (update_available()) {
        char up[64];
        snprintf(up, sizeof up, "Maj %s dispo - R", update_latest());
        float uw = ui_text_width(a, 0.35f, up);
        ui_text(a, (320.0f - uw) / 2.0f, BAND_Y + 6.0f, 0.35f, COL_GREEN, up);
    } else {
        const char *hints = (a->scene == SCENE_SEARCH)
            ? "A lire  B retour  X actu.  SELECT compte  START quitter"
            : "A lire  X actu.  Y recherche  SELECT compte  START quitter";
        float hw = ui_text_width(a, 0.35f, hints);
        ui_text(a, (320.0f - hw) / 2.0f, BAND_Y + 6.0f, 0.35f, COL_TEXT_DIM,
                hints);
    }
}

/* --- scene home --- */

/* recharge le top. bloquant (~1s), le rendu est fige pendant l'appel */
static void home_reload(App *a)
{
    HttpCtx *http = browse_http(a);
    browse_release_thumbs(a);
    a->stream_count = 0;
    a->sel = 0;
    a->scroll = 0;
    snprintf(a->list_title, sizeof(a->list_title), "Top streams");
    s_list_foreign = false;
    if (!http) {
        app_set_status(a, "Erreur reseau (init HTTP)");
        return;
    }
    int n = twitch_top_streams(http, a->streams, APP_MAX_STREAMS);
    if (n < 0) {
        const char *g = twitch_last_error();          // erreur cote Twitch si dispo, sinon erreur HTTP brute
        const char *e = g[0] ? g : http_ctx_last_error(http);
        if (e[0])
            app_set_status(a, "Erreur (%d): %s", n, e);
        else
            app_set_status(a, "Erreur reseau (%d)", n);
        return;
    }
    a->stream_count = n;
    app_set_status(a, "%d streams", n);
}

static void home_enter(App *a)
{
    s_repeat = 0;
    s_tap_index = -1;
    /* premier lancement, ou on revient d'une recherche */
    if (a->stream_count == 0 || s_list_foreign)
        home_reload(a);
}

static void home_update(App *a, u32 kDown, u32 kHeld,
                        const touchPosition *touch)
{
    browse_update(a, kDown, kHeld, touch, home_reload);
}

Scene scene_home = {
    .enter       = home_enter,
    .leave       = NULL,
    .update      = home_update,
    .draw_top    = browse_draw_top,
    .draw_bottom = browse_draw_bottom,
};
