// app.h: global state + scenes (top = video/previews, bottom = nav/chat/touch)
#pragma once
#include <citro2d.h>
#include "twitch.h"
#include "thumb.h"

/* Palette Twitch */
#define COL_BG      C2D_Color32(0x0E, 0x0E, 0x10, 0xFF)
#define COL_PANEL   C2D_Color32(0x18, 0x18, 0x1B, 0xFF)
#define COL_PANEL2  C2D_Color32(0x26, 0x26, 0x2C, 0xFF)
#define COL_PURPLE  C2D_Color32(0x91, 0x46, 0xFF, 0xFF)
#define COL_PURPLE_DARK C2D_Color32(0x5C, 0x16, 0xC5, 0xFF)
#define COL_TEXT    C2D_Color32(0xEF, 0xEF, 0xF1, 0xFF)
#define COL_TEXT_DIM C2D_Color32(0xAD, 0xAD, 0xB8, 0xFF)
#define COL_RED     C2D_Color32(0xEB, 0x04, 0x00, 0xFF)
#define COL_GREEN   C2D_Color32(0x00, 0xC8, 0x5A, 0xFF)

#define APP_MAX_STREAMS 40

typedef enum {
    SCENE_HOME,
    SCENE_SEARCH,
    SCENE_PLAYER,   /* playback + chat */
    SCENE_LOGIN,    /* device flow oauth */
    SCENE_COUNT
} SceneId;

typedef struct App App;

typedef struct {
    void (*enter)(App *a);
    void (*leave)(App *a);
    void (*update)(App *a, u32 kDown, u32 kHeld, const touchPosition *touch);
    void (*draw_top)(App *a);     // the target is already bound
    void (*draw_bottom)(App *a);
} Scene;

struct App {
    C3D_RenderTarget *top, *bottom;
    C2D_TextBuf textbuf;          // cleared every frame

    SceneId scene;
    SceneId next_scene;           // applied at end of frame, not mid-draw

    bool is_new3ds;               /* MVD => video possible, else audio only */
    bool audio_ok;
    bool net_ok;

    HttpCtx *http;                // main-thread ctx (threads have their own)

    // list shared home <-> search
    TwitchStream streams[APP_MAX_STREAMS];
    Thumb        thumbs[APP_MAX_STREAMS];
    int  stream_count;
    int  sel;
    int  scroll;                  // first visible index
    char list_title[64];

    TwitchStream current;

    char status[160];             // status band, UTF-8
    u64  status_until;            // osGetTime() display limit
};

extern Scene scene_home, scene_search, scene_player, scene_login;

void app_set_status(App *a, const char *fmt, ...);
void app_switch_scene(App *a, SceneId s);

// soft keyboard (applet), false if cancelled
bool app_keyboard(const char *hint, char *out, size_t out_size);
