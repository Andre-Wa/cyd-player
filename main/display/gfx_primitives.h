/*
 * gfx_primitives.h — Primitivas de desenho sobre buffers parciais RGB565.
 * Todas as operações usam coordenadas de TELA e são recortadas (clip) à
 * região do buffer, o que permite renderização por tiras sem framebuffer
 * completo (spec §3).
 *
 * Os pixels são armazenados com bytes trocados (big-endian), como o ILI9341
 * espera no RAMWR — a conversão acontece aqui, uma única vez.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define GFX_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

/* ---- paleta do tema ---- */
#define COL_BG        GFX_RGB(10, 13, 18)
#define COL_PANEL     GFX_RGB(23, 28, 38)
#define COL_PANEL2    GFX_RGB(34, 41, 55)
#define COL_BORDER    GFX_RGB(70, 82, 105)
#define COL_TEXT      GFX_RGB(238, 240, 244)
#define COL_MUTED     GFX_RGB(140, 150, 168)
#define COL_ACCENT    GFX_RGB(255, 208, 0)     /* amarelo CYD */
#define COL_OK        GFX_RGB(60, 200, 110)
#define COL_ERR       GFX_RGB(255, 82, 82)
#define COL_BT        GFX_RGB(96, 165, 250)
#define COL_BTN       GFX_RGB(38, 45, 60)
#define COL_BTN_HI    GFX_RGB(58, 68, 90)

typedef struct {
    uint16_t *buf;      /* buffer da tira */
    int w, h;           /* dimensões do buffer */
    int x0, y0;         /* origem do buffer em coordenadas de tela */
    int cx0, cy0, cx1, cy1;  /* recorte (coords de tela, inclusivo) */
} gfx_canvas_t;

void gfx_canvas_init(gfx_canvas_t *c, uint16_t *buf, int w, int h, int x0, int y0);
/* Define/limpa o recorte em coordenadas de tela */
void gfx_set_clip(gfx_canvas_t *c, int x0, int y0, int x1, int y1);
void gfx_reset_clip(gfx_canvas_t *c);

void gfx_pixel(gfx_canvas_t *c, int x, int y, uint16_t col);
void gfx_fill(gfx_canvas_t *c, int x, int y, int w, int h, uint16_t col);
void gfx_rect(gfx_canvas_t *c, int x, int y, int w, int h, uint16_t col);
void gfx_hline(gfx_canvas_t *c, int x, int y, int w, uint16_t col);
void gfx_vline(gfx_canvas_t *c, int x, int y, int h, uint16_t col);
void gfx_line(gfx_canvas_t *c, int x0, int y0, int x1, int y1, uint16_t col);
void gfx_fill_tri(gfx_canvas_t *c, int ax, int ay, int bx, int by,
                  int cx, int cy, uint16_t col);

/* ---- texto (fonte 8x16, escala 1 ou 2) ---- */
int  gfx_text_w(const char *s, int scale);
void gfx_text(gfx_canvas_t *c, int x, int y, const char *s, uint16_t fg, int scale);
void gfx_text_bg(gfx_canvas_t *c, int x, int y, const char *s, uint16_t fg,
                 uint16_t bg, int scale);
/* Trunca com '~' para caber em max_w px */
void gfx_text_fit(gfx_canvas_t *c, int x, int y, int max_w, const char *s,
                  uint16_t fg, int scale);
/* Quebra em palavras, até max_lines linhas */
void gfx_text_wrap(gfx_canvas_t *c, int x, int y, int max_w, const char *s,
                   uint16_t fg, int scale, int max_lines);

/* ---- widgets compostos ---- */
void gfx_bar(gfx_canvas_t *c, int x, int y, int w, int h, int pct,
             uint16_t fg, uint16_t bg);
void gfx_button(gfx_canvas_t *c, int x, int y, int w, int h, const char *label,
                bool active);
void gfx_slider(gfx_canvas_t *c, int x, int y, int w, int pct, uint16_t col);

/* ---- ícones (desenhados em uma caixa size×size centrada em cx,cy) ---- */
typedef enum {
    ICON_PLAY = 0, ICON_PAUSE, ICON_NEXT, ICON_PREV, ICON_STOP,
    ICON_FOLDER, ICON_NOTE, ICON_BT, ICON_BT_OFF, ICON_BACK,
    ICON_SPEAKER, ICON_EQ, ICON_CHECK, ICON_SEARCH, ICON_SD,
} gfx_icon_t;
void gfx_icon(gfx_canvas_t *c, gfx_icon_t icon, int cx, int cy, int size,
              uint16_t col);

/* utilitário de hit-test */
static inline bool gfx_in_rect(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* formata mm:ss */
void gfx_fmt_time(char *out, int out_len, uint32_t seconds);
