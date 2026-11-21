// scene_login.c: Twitch login via device flow (code shown, we poll until the token)
#include <stdio.h>
#include <string.h>
#include <3ds.h>

#include "app.h"
#include "ui.h"
#include "twitch.h"
#include "config.h"

typedef enum {
    L_STARTING,
    L_WAITING,    /* code shown, waiting for authorization */
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
        snprintf(s_msg, sizeof s_msg, "Cannot start login (network?)");
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
        // ok, still need the login before saving
        char login[64];
        if (twitch_get_login(a->http, token, login, sizeof login) != 0 || !login[0]) {
            s_phase = L_FAIL;
            snprintf(s_msg, sizeof s_msg, "Token obtained but login not found");
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
            snprintf(s_msg, sizeof s_msg, "Code expired. Try again.");
        } else {
            s_next_poll = now + (u64)s_auth.interval * 1000;
        }
    } else {
        s_phase = L_FAIL;
        snprintf(s_msg, sizeof s_msg, "Authorization refused or expired.");
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
        do_poll(a); // blocks ~1s but the screen does not move anyway
}

static void login_draw_top(App *a)
{
    C2D_DrawRectSolid(0, 0, 0.0f, 400, 240, COL_PANEL);
    ui_text(a, 16, 14, 0.6f, COL_PURPLE, "Sign in to Twitch");

    if (s_phase == L_SUCCESS) {
        char t[96];
        snprintf(t, sizeof t, "Connected: %s", s_login);
        ui_text(a, 16, 100, 0.7f, COL_GREEN, t);
        ui_text(a, 16, 140, 0.45f, COL_TEXT_DIM, "You can now write in the chat.");
        return;
    }
    if (s_phase == L_FAIL) {
        ui_text_wrap(a, 16, 100, 0.5f, COL_RED, s_msg, 368, 3);
        return;
    }

    /* otherwise show the instructions + the code */
    ui_text(a, 16, 64, 0.5f, COL_TEXT, "1. On your phone/PC, open:");
    ui_text(a, 32, 92, 0.6f, COL_TEXT, "twitch.tv/activate");
    ui_text(a, 16, 130, 0.5f, COL_TEXT, "2. Enter this code:");

    const char *code = s_auth.user_code[0] ? s_auth.user_code : "........";
    float cw = ui_text_width(a, 1.3f, code);
    ui_text(a, (400.0f - cw) / 2.0f, 158, 1.3f, COL_PURPLE, code);
}

static void login_draw_bottom(App *a)
{
    C2D_DrawRectSolid(0, 0, 0.0f, 320, 24, COL_PURPLE);
    ui_text(a, 8, 4, 0.45f, COL_TEXT, "Twitch account");

    if (s_phase == L_WAITING || s_phase == L_STARTING) {
        ui_text_wrap(a, 10, 40, 0.45f, COL_TEXT_DIM,
                     "Waiting for your authorization in the browser... "
                     "Login (and 2FA) happens on Twitch, never here.",
                     300, 4);
        // blinking dots, just to make it look alive
        int dots = (int)((osGetTime() / 400) % 4);
        char sp[8] = "   ";
        for (int i = 0; i < dots && i < 3; i++) sp[i] = '.';
        ui_text(a, 150, 120, 0.6f, COL_PURPLE, sp);
    } else if (s_phase == L_SUCCESS) {
        ui_text(a, 10, 60, 0.5f, COL_GREEN, "Signed in successfully!");
        ui_text(a, 10, 200, 0.4f, COL_TEXT_DIM, "A / B: back");
    } else {
        ui_text(a, 10, 60, 0.5f, COL_RED, "Sign-in failed");
        ui_text(a, 10, 200, 0.4f, COL_TEXT_DIM, "B: back");
    }

    ui_text(a, 10, 218, 0.4f, COL_TEXT_DIM, "B: cancel");
}

Scene scene_login = {
    .enter       = login_enter,
    .leave       = NULL,
    .update      = login_update,
    .draw_top    = login_draw_top,
    .draw_bottom = login_draw_bottom,
};
