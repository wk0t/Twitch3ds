// ui.c : petites primitives de dessin (citro2d + police système)
// tout le texte passe par a->textbuf, que main.c vide à chaque frame — donc rien à garder entre 2 frames
#include <stdio.h>
#include <string.h>

#include "ui.h"

/* le "…" en UTF-8, NUL compris */
static const char UI_ELLIPSIS[] = "\xE2\x80\xA6";

#define UI_WRAP_MAX_LINES 16
#define UI_LINE_CAP       512

static App *s_app;

void ui_init(App *a)
{
    s_app = a;
}

void ui_exit(void)
{
    s_app = NULL;
}

/* parse dans le textbuf de la frame, false si vide/imparsable */
static bool ui_parse(App *a, C2D_Text *t, const char *utf8)
{
    if (!a || !a->textbuf || !utf8 || !utf8[0])
        return false;
    return C2D_TextParse(t, a->textbuf, utf8) != NULL;
}

float ui_text(App *a, float x, float y, float scale, u32 color, const char *utf8)
{
    C2D_Text t;
    if (!ui_parse(a, &t, utf8))
        return 0.0f;
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
    float w = 0.0f;
    C2D_TextGetDimensions(&t, scale, scale, &w, NULL);
    return w;
}

float ui_text_width(App *a, float scale, const char *utf8)
{
    C2D_Text t;
    if (!ui_parse(a, &t, utf8))
        return 0.0f;
    float w = 0.0f;
    C2D_TextGetDimensions(&t, scale, scale, &w, NULL);
    return w;
}

float ui_text_ellipsis(App *a, float x, float y, float scale, u32 color,
                       const char *utf8, float max_w)
{
    if (!utf8 || !utf8[0])
        return 0.0f;

    float w = ui_text_width(a, scale, utf8);
    if (w <= max_w)
        return ui_text(a, x, y, scale, color, utf8);

    char tmp[UI_LINE_CAP];
    int len = (int)strlen(utf8);
    if (len > (int)sizeof(tmp) - (int)sizeof(UI_ELLIPSIS))
        len = (int)sizeof(tmp) - (int)sizeof(UI_ELLIPSIS);
    /* recule jusqu'à une frontière UTF-8 propre */
    while (len > 0 && ((u8)utf8[len] & 0xC0) == 0x80)
        len--;

    /* dichotomie sur la longueur du préfixe */
    int lo = 0, hi = len, best = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int m = mid;
        while (m > 0 && ((u8)utf8[m] & 0xC0) == 0x80)
            m--;
        memcpy(tmp, utf8, (size_t)m);
        memcpy(tmp + m, UI_ELLIPSIS, sizeof(UI_ELLIPSIS));
        if (ui_text_width(a, scale, tmp) <= max_w) {
            best = m;
            lo = mid + 1;
        } else {
            hi = m - 1;
        }
    }

    memcpy(tmp, utf8, (size_t)best);
    memcpy(tmp + best, UI_ELLIPSIS, sizeof(UI_ELLIPSIS));
    return ui_text(a, x, y, scale, color, tmp);
}

/* dessine une ligne, renvoie sa hauteur */
static float ui_draw_line(App *a, float x, float y, float scale, u32 color, const char *s)
{
    C2D_Text t;
    if (!ui_parse(a, &t, s))
        return 0.0f;
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
    float h = 0.0f;
    C2D_TextGetDimensions(&t, scale, scale, NULL, &h);
    return h;
}

/* plus long préfixe de s qui tient dans max_w (jamais moins d'un glyphe) */
static size_t ui_fit_prefix(App *a, float scale, const char *s, size_t n, float max_w)
{
    char tmp[UI_LINE_CAP];
    size_t best = 0, i = 0;
    while (i < n && i < sizeof(tmp) - 1) {
        i++;
        while (i < n && ((u8)s[i] & 0xC0) == 0x80)
            i++;
        memcpy(tmp, s, i);
        tmp[i] = '\0';
        if (ui_text_width(a, scale, tmp) <= max_w)
            best = i;
        else
            break;
    }
    return best ? best : i; /* force la progression même si un seul glyphe déborde */
}

float ui_text_wrap(App *a, float x, float y, float scale, u32 color,
                   const char *utf8, float max_w, int max_lines)
{
    /* l'UI est mono-thread, donc buffer statique ok */
    static char L[UI_WRAP_MAX_LINES][UI_LINE_CAP];

    if (!a || !utf8 || !utf8[0] || max_lines < 1 || max_w <= 0.0f)
        return 0.0f;

    int cap = max_lines < UI_WRAP_MAX_LINES ? max_lines : UI_WRAP_MAX_LINES;
    int n = 0;
    bool truncated = false;
    char cur[UI_LINE_CAP];
    cur[0] = '\0';
    size_t cl = 0;
    const char *p = utf8;

    /* découpe gloutonne mot à mot */
    while (*p && !truncated) {
        if (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
            continue;
        }
        if (*p == '\n') { /* saut de ligne explicite */
            p++;
            if (n >= cap) {
                truncated = true;
                break;
            }
            strcpy(L[n++], cur);
            cur[0] = '\0';
            cl = 0;
            continue;
        }

        const char *ws = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
            p++;
        size_t wl = (size_t)(p - ws);
        if (wl > UI_LINE_CAP - 8) { /* mot à rallonge : on le coupe (frontière UTF-8) */
            size_t k = UI_LINE_CAP - 8;
            while (k > 0 && ((u8)ws[k] & 0xC0) == 0x80)
                k--;
            wl = k;
            p = ws + wl;
        }
        char word[UI_LINE_CAP];
        memcpy(word, ws, wl);
        word[wl] = '\0';

        if (cl > 0) {
            /* on tente d'ajouter " mot" à la ligne courante */
            char cand[UI_LINE_CAP];
            if (cl + 1 + wl < sizeof(cand)) {
                memcpy(cand, cur, cl);
                cand[cl] = ' ';
                memcpy(cand + cl + 1, word, wl + 1);
                if (ui_text_width(a, scale, cand) <= max_w) {
                    memcpy(cur, cand, cl + 1 + wl + 1);
                    cl += 1 + wl;
                    continue;
                }
            }
            /* ça déborde : on ferme la ligne et on repart avec le mot */
            if (n >= cap) {
                truncated = true;
                break;
            }
            strcpy(L[n++], cur);
            cur[0] = '\0';
            cl = 0;
        }

        /* mot seul trop large : on le tranche caractère par caractère */
        while (ui_text_width(a, scale, word) > max_w) {
            size_t rem = strlen(word);
            size_t fit = ui_fit_prefix(a, scale, word, rem, max_w);
            if (fit == 0 || fit >= rem)
                break;
            if (n >= cap) {
                truncated = true;
                break;
            }
            memcpy(L[n], word, fit);
            L[n][fit] = '\0';
            n++;
            memmove(word, word + fit, rem - fit + 1);
        }
        if (truncated)
            break;
        strcpy(cur, word);
        cl = strlen(cur);
    }

    if (!truncated && cl > 0) {
        if (n < cap)
            strcpy(L[n++], cur);
        else
            truncated = true;
    }
    if (*p)
        truncated = true;
    if (n == 0)
        return 0.0f;

    float line_h = 30.0f * scale; /* réajusté avec la vraie hauteur de la 1re ligne */
    for (int i = 0; i < n; i++) {
        float ly = y + (float)i * line_h;
        if (i == n - 1 && truncated) {
            size_t ll = strlen(L[i]);
            if (ll + sizeof(UI_ELLIPSIS) <= sizeof(L[i]))
                memcpy(L[i] + ll, UI_ELLIPSIS, sizeof(UI_ELLIPSIS));
            ui_text_ellipsis(a, x, ly, scale, color, L[i], max_w);
        } else if (L[i][0]) {
            float h = ui_draw_line(a, x, ly, scale, color, L[i]);
            if (i == 0 && h > 0.0f)
                line_h = h;
        }
    }
    return (float)n * line_h;
}

void ui_panel(float x, float y, float w, float h, u32 color)
{
    C2D_DrawRectSolid(x, y, 0.0f, w, h, color);
}

bool ui_button(App *a, float x, float y, float w, float h,
               const char *label, bool touched, const touchPosition *touch)
{
    bool hit = touched && touch &&
               (float)touch->px >= x && (float)touch->px < x + w &&
               (float)touch->py >= y && (float)touch->py < y + h;

    ui_panel(x, y, w, h, hit ? COL_PURPLE : COL_PANEL2);

    /* liseré 1 px autour */
    C2D_DrawRectSolid(x,            y,            0.0f, w,    1.0f, COL_PURPLE_DARK);
    C2D_DrawRectSolid(x,            y + h - 1.0f, 0.0f, w,    1.0f, COL_PURPLE_DARK);
    C2D_DrawRectSolid(x,            y,            0.0f, 1.0f, h,    COL_PURPLE_DARK);
    C2D_DrawRectSolid(x + w - 1.0f, y,            0.0f, 1.0f, h,    COL_PURPLE_DARK);

    C2D_Text t;
    if (ui_parse(a, &t, label)) {
        C2D_TextOptimize(&t);
        float tw = 0.0f, th = 0.0f;
        C2D_TextGetDimensions(&t, 0.5f, 0.5f, &tw, &th);
        C2D_DrawText(&t, C2D_WithColor,
                     x + (w - tw) * 0.5f, y + (h - th) * 0.5f,
                     0.5f, 0.5f, 0.5f, COL_TEXT);
    }
    return hit;
}

/* "1234567" -> "1 234 567" */
static void ui_fmt_thousands(char *out, size_t outsz, int v)
{
    char raw[16];
    unsigned uv = (v < 0) ? 0u : (unsigned)v;
    int rn = snprintf(raw, sizeof(raw), "%u", uv);
    size_t o = 0;
    for (int i = 0; i < rn && o + 2 < outsz; i++) {
        if (i > 0 && (rn - i) % 3 == 0)
            out[o++] = ' ';
        out[o++] = raw[i];
    }
    out[o] = '\0';
}

void ui_badge_live(App *a, float x, float y, int viewers)
{
    const float scale = 0.4f;

    C2D_Text t;
    if (!ui_parse(a, &t, "LIVE"))
        return;
    C2D_TextOptimize(&t);
    float tw = 0.0f, th = 0.0f;
    C2D_TextGetDimensions(&t, scale, scale, &tw, &th);

    float bw = tw + 8.0f, bh = th + 2.0f;
    ui_panel(x, y, bw, bh, COL_RED);
    C2D_DrawText(&t, C2D_WithColor, x + 4.0f, y + 1.0f, 0.5f, scale, scale, COL_TEXT);

    char num[24];
    ui_fmt_thousands(num, sizeof(num), viewers);
    ui_text(a, x + bw + 6.0f, y + 1.0f, scale, COL_TEXT, num);
}

void ui_draw_status(App *a)
{
    if (!a || !a->status[0])
        return;
    if (osGetTime() >= a->status_until)
        return;

    /* bandeau tout en bas de l'écran du bas (320 px) */
    ui_panel(0.0f, 225.0f, 320.0f, 15.0f, COL_PANEL);

    C2D_Text t;
    if (!ui_parse(a, &t, a->status))
        return;
    C2D_TextOptimize(&t);
    float tw = 0.0f, th = 0.0f;
    C2D_TextGetDimensions(&t, 0.45f, 0.45f, &tw, &th);
    float tx = (320.0f - tw) * 0.5f;
    if (tx < 2.0f)
        tx = 2.0f;
    C2D_DrawText(&t, C2D_WithColor, tx, 225.0f + (15.0f - th) * 0.5f,
                 0.5f, 0.45f, 0.45f, COL_TEXT);
}

static int ui_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

u32 ui_color_from_hex(const char *hex)
{
    if (!hex || hex[0] != '#')
        return COL_PURPLE;

    int v[6];
    for (int i = 0; i < 6; i++) {
        v[i] = ui_hexval(hex[1 + i]); /* si on tombe sur le NUL ça rend -1, pas de débordement */
        if (v[i] < 0)
            return COL_PURPLE;
    }
    if (hex[7] != '\0')
        return COL_PURPLE;

    int r = v[0] * 16 + v[1];
    int g = v[2] * 16 + v[3];
    int b = v[4] * 16 + v[5];

    /* trop sombre sur fond noir : on éclaircit en mélangeant du blanc */
    if (r + g + b < 120) {
        r = (r + 255) / 2;
        g = (g + 255) / 2;
        b = (b + 255) / 2;
    }
    return C2D_Color32((u8)r, (u8)g, (u8)b, 0xFF);
}
