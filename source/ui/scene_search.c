// scene_search.c : clavier puis liste de résultats (réutilise la liste de scene_home)
#include <stdio.h>
#include <string.h>
#include <3ds.h>
#include <citro2d.h>

#include "app.h"
#include "ui.h"
#include "twitch.h"
#include "thumb.h"
#include "config.h"

// ces browse_* vivent dans scene_home.c
extern void browse_update(App *a, u32 kDown, u32 kHeld,
                          const touchPosition *touch,
                          void (*reload)(App *a));
extern void browse_draw_top(App *a);
extern void browse_draw_bottom(App *a);
extern void browse_release_thumbs(App *a);
extern void browse_mark_foreign_list(void);
extern HttpCtx *browse_http(App *a);

static char s_query[64]; // dernière requête tapée, re-servie par X

// bloquant (~1s), l'écran reste figé pendant l'appel
static void search_reload(App *a)
{
    HttpCtx *http = browse_http(a);
    browse_release_thumbs(a);
    a->stream_count = 0;
    a->sel = 0;
    a->scroll = 0;
    snprintf(a->list_title, sizeof(a->list_title), "Recherche : %.48s",
             s_query);
    browse_mark_foreign_list(); // scene_home rechargera son top au retour
    if (!http) {
        app_set_status(a, "Erreur reseau (init HTTP)");
        return;
    }
    int n = twitch_search_streams(http, s_query, a->streams,
                                  APP_MAX_STREAMS);
    if (n < 0) {
        const char *g = twitch_last_error();
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

static void search_prompt(App *a)
{
    char q[64];
    if (!app_keyboard("Rechercher une chaine ou un jeu", q, sizeof(q))) {
        app_switch_scene(a, SCENE_HOME);
        return;
    }
    snprintf(s_query, sizeof(s_query), "%s", q);
    search_reload(a);
}

// X : relance la même requête, sinon ouvre le clavier
static void search_refresh(App *a)
{
    if (s_query[0])
        search_reload(a);
    else
        search_prompt(a);
}

static void search_enter(App *a)
{
    search_prompt(a);
}

static void search_update(App *a, u32 kDown, u32 kHeld,
                          const touchPosition *touch)
{
    if (kDown & KEY_B) {
        app_switch_scene(a, SCENE_HOME);
        return;
    }
    if (kDown & KEY_Y) { // on chope Y ici, avant que browse_update le voie
        search_prompt(a);
        return;
    }
    browse_update(a, kDown, kHeld, touch, search_refresh);
}

Scene scene_search = {
    .enter       = search_enter,
    .leave       = NULL,
    .update      = search_update,
    .draw_top    = browse_draw_top,
    .draw_bottom = browse_draw_bottom,
};
