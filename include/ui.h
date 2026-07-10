// ui.h : primitives de dessin (citro2d + police système)
// les fonctions texte parsent en UTF-8 dans le TextBuf de l'app (vidé chaque frame) — rien à garder entre frames
#pragma once
#include "app.h"

void ui_init(App *a);
void ui_exit(void);

/* texte simple, renvoie la largeur en px. scale 0.5f ≈ 15 px */
float ui_text(App *a, float x, float y, float scale, u32 color, const char *utf8);
/* pareil mais tronque avec "…" si ça dépasse max_w */
float ui_text_ellipsis(App *a, float x, float y, float scale, u32 color,
                       const char *utf8, float max_w);
/* multi-lignes (retour à la ligne par mots), renvoie la hauteur */
float ui_text_wrap(App *a, float x, float y, float scale, u32 color,
                   const char *utf8, float max_w, int max_lines);
float ui_text_width(App *a, float scale, const char *utf8);

void ui_panel(float x, float y, float w, float h, u32 color);
/* bouton tactile, true si le touch de la frame tombe dedans */
bool ui_button(App *a, float x, float y, float w, float h,
               const char *label, bool touched, const touchPosition *touch);

/* bandeau d'état en bas (lit a->status) */
void ui_draw_status(App *a);
/* badge "LIVE 12 345" */
void ui_badge_live(App *a, float x, float y, int viewers);

/* "#RRGGBB" -> couleur C2D, fallback violet Twitch */
u32 ui_color_from_hex(const char *hex);
