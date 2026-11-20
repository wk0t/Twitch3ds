// ui.h: drawing primitives (citro2d + system font)
// text functions parse UTF-8 into the app TextBuf (cleared every frame) — nothing persists between frames
#pragma once
#include "app.h"

void ui_init(App *a);
void ui_exit(void);

/* plain text, returns the width in px. scale 0.5f ≈ 15 px */
float ui_text(App *a, float x, float y, float scale, u32 color, const char *utf8);
/* same but truncates with "…" if it exceeds max_w */
float ui_text_ellipsis(App *a, float x, float y, float scale, u32 color,
                       const char *utf8, float max_w);
/* multi-line (word wrap), returns the height */
float ui_text_wrap(App *a, float x, float y, float scale, u32 color,
                   const char *utf8, float max_w, int max_lines);
float ui_text_width(App *a, float scale, const char *utf8);

void ui_panel(float x, float y, float w, float h, u32 color);
/* touch button, true if this frame's touch lands inside */
bool ui_button(App *a, float x, float y, float w, float h,
               const char *label, bool touched, const touchPosition *touch);

/* status band at the bottom (reads a->status) */
void ui_draw_status(App *a);
/* badge "LIVE 12 345" */
void ui_badge_live(App *a, float x, float y, int viewers);

/* "#RRGGBB" -> C2D color, Twitch-purple fallback */
u32 ui_color_from_hex(const char *hex);
