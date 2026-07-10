// scene_login.c : login Twitch en device flow (code affiché, on poll jusqu'au jeton)
#include <stdio.h>
#include <string.h>
#include <3ds.h>

#include "app.h"
#include "ui.h"
#include "twitch.h"
#include "config.h"

typedef enum {
    L_STARTING,
    L_WAITING,    /* code affiché, on attend l'autorisation */
    L_SUCCESS,
    L_FAIL
} LoginPhase;

static LoginPhase       s_phase;
static TwitchDeviceAuth s_auth;
static u64  s_next_poll;
static u64  s_expire;
static char s_login[64];
static char s_msg[160];

static void login_enter(App *a)
{
    s_phase = L_STARTING;
    s_login[0] = '\0';
    s_msg[0] = '\0';

    if (!a->http || twitch_device_start(a->http, &s_auth) != 0) {
        s_phase = L_FAIL;
        snprintf(s_msg, sizeof s_msg, "Impossible de demarrer la connexion (reseau ?)");
        return;
    }
    u64 now = osGetTime();
    s_next_poll = now + (u64)s_auth.interval * 1000;
    s_expire    = now + (u64)(s_auth.expires_in > 0 ? s_auth.expires_in : 900) * 1000;
    s_phase = L_WAITING;
}

static void do_poll(App *a)
{
    char token[512], refresh[512];
    int r = twitch_device_poll(a->http, s_auth.device_code,
                               token, sizeof token, refresh, sizeof refresh);
    if (r == 1) {
        // ok, faut encore choper le login avant de sauver
        char login[64];
        if (twitch_get_login(a->http, token, login, sizeof login) != 0 || !login[0]) {
            s_phase = L_FAIL;
            snprintf(s_msg, sizeof s_msg, "Jeton obtenu mais login introuvable");
            return;
        }
        snprintf(g_config.username, sizeof g_config.username, "%s", login);
        snprintf(g_config.oauth, sizeof g_config.oauth, "oauth:%.*s",
                 (int)(sizeof g_config.oauth - 8), token);
        config_save();
        snprintf(s_login, sizeof s_login, "%s", login);
        s_phase = L_SUCCESS;
    } else if (r == 0) {
        u64 now = osGetTime();
        if (now >= s_expire) {
            s_phase = L_FAIL;
            snprintf(s_msg, sizeof s_msg, "Code expire. Reessaie.");
        } else {
            s_next_poll = now + (u64)s_auth.interval * 1000;
        }
    } else {
        s_phase = L_FAIL;
        snprintf(s_msg, sizeof s_msg, "Autorisation refusee ou expiree.");
    }
}

static void login_update(App *a, u32 kDown, u32 kHeld, const touchPosition *touch)
{
    (void)kHeld; (void)touch;

    if (kDown & KEY_B) {
        app_switch_scene(a, SCENE_HOME);
        return;
    }
    if (s_phase == L_SUCCESS && (kDown & KEY_A)) {
        app_switch_scene(a, SCENE_HOME);
        return;
    }
    if (s_phase == L_WAITING && osGetTime() >= s_next_poll)
        do_poll(a); // ça bloque ~1s mais l'écran bouge pas de toute façon
}

static void login_draw_top(App *a)
{
    C2D_DrawRectSolid(0, 0, 0.0f, 400, 240, COL_PANEL);
    ui_text(a, 16, 14, 0.6f, COL_PURPLE, "Connexion a Twitch");

    if (s_phase == L_SUCCESS) {
        char t[96];
        snprintf(t, sizeof t, "Connecte : %s", s_login);
        ui_text(a, 16, 100, 0.7f, COL_GREEN, t);
        ui_text(a, 16, 140, 0.45f, COL_TEXT_DIM, "Tu peux maintenant ecrire dans le chat.");
        return;
    }
    if (s_phase == L_FAIL) {
        ui_text_wrap(a, 16, 100, 0.5f, COL_RED, s_msg, 368, 3);
        return;
    }

    /* sinon on affiche les instructions + le code */
    ui_text(a, 16, 64, 0.5f, COL_TEXT, "1. Sur ton tel/PC, ouvre :");
    ui_text(a, 32, 92, 0.6f, COL_TEXT, "twitch.tv/activate");
    ui_text(a, 16, 130, 0.5f, COL_TEXT, "2. Entre ce code :");

    const char *code = s_auth.user_code[0] ? s_auth.user_code : "........";
    float cw = ui_text_width(a, 1.3f, code);
    ui_text(a, (400.0f - cw) / 2.0f, 158, 1.3f, COL_PURPLE, code);
}

static void login_draw_bottom(App *a)
{
    C2D_DrawRectSolid(0, 0, 0.0f, 320, 24, COL_PURPLE);
    ui_text(a, 8, 4, 0.45f, COL_TEXT, "Compte Twitch");

    if (s_phase == L_WAITING || s_phase == L_STARTING) {
        ui_text_wrap(a, 10, 40, 0.45f, COL_TEXT_DIM,
                     "En attente de ton autorisation dans le navigateur... "
                     "La connexion (et la 2FA) se fait sur Twitch, jamais ici.",
                     300, 4);
        // petits points qui clignotent, histoire que ça ait l'air vivant
        int dots = (int)((osGetTime() / 400) % 4);
        char sp[8] = "   ";
        for (int i = 0; i < dots && i < 3; i++) sp[i] = '.';
        ui_text(a, 150, 120, 0.6f, COL_PURPLE, sp);
    } else if (s_phase == L_SUCCESS) {
        ui_text(a, 10, 60, 0.5f, COL_GREEN, "Connecte avec succes !");
        ui_text(a, 10, 200, 0.4f, COL_TEXT_DIM, "A / B : retour");
    } else {
        ui_text(a, 10, 60, 0.5f, COL_RED, "Echec de connexion");
        ui_text(a, 10, 200, 0.4f, COL_TEXT_DIM, "B : retour");
    }

    ui_text(a, 10, 218, 0.4f, COL_TEXT_DIM, "B : annuler");
}

Scene scene_login = {
    .enter       = login_enter,
    .leave       = NULL,
    .update      = login_update,
    .draw_top    = login_draw_top,
    .draw_bottom = login_draw_bottom,
};
