// main.c : init des services + grosse boucle de scenes a 60fps
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <3ds.h>
#include <citro2d.h>

#include "app.h"
#include "ui.h"
#include "config.h"
#include "net_http.h"
#include "audio_aac.h"
#include "player.h"
#include "irc.h"
#include "update.h"

/* grosse pile : curl/mbedtls tournent sur le thread principal */
u32 __stacksize__ = 0x40000;

static App g_app;

static Scene *scene_table(SceneId id)
{
    switch (id) {
        case SCENE_HOME:   return &scene_home;
        case SCENE_SEARCH: return &scene_search;
        case SCENE_PLAYER: return &scene_player;
        case SCENE_LOGIN:  return &scene_login;
        default:           return &scene_home;
    }
}

void app_set_status(App *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->status, sizeof(a->status), fmt, ap);
    va_end(ap);
    a->status_until = osGetTime() + 4000;
}

void app_switch_scene(App *a, SceneId s)
{
    a->next_scene = s;
}

bool app_keyboard(const char *hint, char *out, size_t out_size)
{
    SwkbdState kbd;
    swkbdInit(&kbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&kbd, hint);
    swkbdSetValidation(&kbd, SWKBD_NOTBLANK_NOTEMPTY, 0, 0);
    swkbdSetFeatures(&kbd, SWKBD_PREDICTIVE_INPUT);
    SwkbdButton btn = swkbdInputText(&kbd, out, out_size);
    return btn == SWKBD_BUTTON_CONFIRM && out[0] != '\0';
}

int main(void)
{
    App *a = &g_app;
    memset(a, 0, sizeof(*a));

    osSetSpeedupEnable(true);

    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    a->top    = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    a->bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    a->textbuf = C2D_TextBufNew(8192);

    romfsInit();
    config_load();

    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    a->is_new3ds = n3ds;

    // on prend 30% du CPU syscore pour les threads reseau/demux
    APT_SetAppCpuTimeLimit(30);

    a->net_ok = R_SUCCEEDED(http_global_init());
    a->audio_ok = R_SUCCEEDED(audio_init());

    if (a->net_ok)
        update_start_check();   // regarde s'il y a une nouvelle version

    ui_init(a);
    thumb_system_init();

    a->scene = SCENE_HOME;
    a->next_scene = SCENE_HOME;
    Scene *sc = scene_table(a->scene);
    if (sc->enter) sc->enter(a);

    if (!a->net_ok)
        app_set_status(a, "Erreur reseau : Wi-Fi actif ?");
    else if (!a->is_new3ds)
        app_set_status(a, "Old 3DS : mode audio seul (MVD absent)");

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();
        touchPosition touch;
        hidTouchRead(&touch);

        if ((kDown & KEY_START) && a->scene != SCENE_PLAYER)
            break;

        sc = scene_table(a->scene);
        if (sc->update) sc->update(a, kDown, kHeld, &touch);
        thumb_update();

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TextBufClear(a->textbuf);

        C2D_TargetClear(a->top, COL_BG);
        C2D_SceneBegin(a->top);
        if (sc->draw_top) sc->draw_top(a);

        C2D_TargetClear(a->bottom, COL_BG);
        C2D_SceneBegin(a->bottom);
        if (sc->draw_bottom) sc->draw_bottom(a);
        ui_draw_status(a);

        C3D_FrameEnd(0);

        if (a->next_scene != a->scene) {
            if (sc->leave) sc->leave(a);
            a->scene = a->next_scene;
            sc = scene_table(a->scene);
            if (sc->enter) sc->enter(a);
        }
    }

    player_close();
    irc_stop();
    thumb_system_exit();
    ui_exit();
    audio_exit();
    if (a->http) http_ctx_free(a->http);
    http_global_exit();
    config_save();

    C2D_TextBufDelete(a->textbuf);
    C2D_Fini();
    C3D_Fini();
    romfsExit();
    gfxExit();
    return 0;
}
