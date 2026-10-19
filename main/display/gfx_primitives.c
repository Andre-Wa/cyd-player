#include <string.h>
#include <stdio.h>
#include "gfx_primitives.h"
#include "font8x16.h"

/* ---------- troca de ordem de bytes p/ ILI9341 (big-endian no RAMWR) ---------- */
static inline uint16_t swz(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

void gfx_canvas_init(gfx_canvas_t *c, uint16_t *buf, int w, int h, int x0, int y0)
{
    c->buf = buf;
    c->w = w;
    c->h = h;
    c->x0 = x0;
    c->y0 = y0;
    gfx_reset_clip(c);
}

void gfx_reset_clip(gfx_canvas_t *c)
{
    c->cx0 = c->x0;
    c->cy0 = c->y0;
    c->cx1 = c->x0 + c->w - 1;
    c->cy1 = c->y0 + c->h - 1;
}

void gfx_set_clip(gfx_canvas_t *c, int x0, int y0, int x1, int y1)
{
    if (x0 < c->x0) x0 = c->x0;
    if (y0 < c->y0) y0 = c->y0;
    if (x1 > c->x0 + c->w - 1) x1 = c->x0 + c->w - 1;
    if (y1 > c->y0 + c->h - 1) y1 = c->y0 + c->h - 1;
    c->cx0 = x0;
    c->cy0 = y0;
    c->cx1 = x1;
    c->cy1 = y1;
}

void gfx_pixel(gfx_canvas_t *c, int x, int y, uint16_t col)
{
    if (x < c->cx0 || x > c->cx1 || y < c->cy0 || y > c->cy1) {
        return;
    }
    c->buf[(y - c->y0) * c->w + (x - c->x0)] = swz(col);
}

void gfx_fill(gfx_canvas_t *c, int x, int y, int w, int h, uint16_t col)
{
    /* recorta aos limites do clip */
    int x1 = x + w - 1, y1 = y + h - 1;
    if (x < c->cx0) x = c->cx0;
    if (y < c->cy0) y = c->cy0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (y1 > c->cy1) y1 = c->cy1;
    if (x > x1 || y > y1) {
        return;
    }
    uint16_t v = swz(col);
    int line_w = x1 - x + 1;
    for (int yy = y; yy <= y1; yy++) {
        uint16_t *row = c->buf + (yy - c->y0) * c->w + (x - c->x0);
        for (int i = 0; i < line_w; i++) {
            row[i] = v;
        }
    }
}

void gfx_rect(gfx_canvas_t *c, int x, int y, int w, int h, uint16_t col)
{
    gfx_hline(c, x, y, w, col);
    gfx_hline(c, x, y + h - 1, w, col);
    gfx_vline(c, x, y, h, col);
    gfx_vline(c, x + w - 1, y, h, col);
}

void gfx_hline(gfx_canvas_t *c, int x, int y, int w, uint16_t col)
{
    gfx_fill(c, x, y, w, 1, col);
}

void gfx_vline(gfx_canvas_t *c, int x, int y, int h, uint16_t col)
{
    gfx_fill(c, x, y, 1, h, col);
}

void gfx_line(gfx_canvas_t *c, int x0, int y0, int x1, int y1, uint16_t col)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        gfx_pixel(c, x0, y0, col);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

void gfx_fill_tri(gfx_canvas_t *c, int ax, int ay, int bx, int by,
                  int cx, int cy, uint16_t col)
{
    /* scanline simples */
    int miny = ay < by ? ay : by; if (cy < miny) miny = cy;
    int maxy = ay > by ? ay : by; if (cy > maxy) maxy = cy;
    for (int y = miny; y <= maxy; y++) {
        int minx = 1 << 30, maxx = -(1 << 30);
        int px[3] = {ax, bx, cx}, py[3] = {ay, by, cy};
        for (int i = 0; i < 3; i++) {
            int j = (i + 1) % 3;
            if ((py[i] <= y && py[j] > y) || (py[j] <= y && py[i] > y)) {
                int x = px[i] + (y - py[i]) * (px[j] - px[i]) / (py[j] - py[i]);
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
            }
        }
        if (minx <= maxx) {
            gfx_hline(c, minx, y, maxx - minx + 1, col);
        }
    }
}

/* ============================ texto ============================ */

static void draw_glyph(gfx_canvas_t *c, int x, int y, char ch,
                       uint16_t fg, uint16_t bg, bool use_bg, int scale)
{
    int idx = (unsigned char)ch - FONT8X16_FIRST;
    const uint8_t *g;
    static const uint8_t blank[FONT8X16_H] = {0};
    if (idx < 0 || idx >= FONT8X16_NUM_CHARS) {
        g = blank;
    } else {
        g = font8x16_glyphs[idx];
    }
    uint16_t fgv = swz(fg), bgv = swz(bg);
    for (int row = 0; row < FONT8X16_H; row++) {
        uint8_t bits = g[row];
        if (!bits && !use_bg) {
            continue;   /* linha vazia transparente: pula */
        }
        int yy = y + row * scale;
        if (yy + scale - 1 < c->cy0 || yy > c->cy1) {
            continue;
        }
        for (int col = 0; col < FONT8X16_W; col++) {
            bool on = (bits >> (7 - col)) & 1;
            if (!on && !use_bg) {
                continue;
            }
            uint16_t v = on ? fgv : bgv;
            int xx = x + col * scale;
            if (xx + scale - 1 < c->cx0 || xx > c->cx1) {
                continue;
            }
            if (scale == 1) {
                c->buf[(yy - c->y0) * c->w + (xx - c->x0)] = v;
            } else {
                for (int sy = 0; sy < scale; sy++) {
                    if (yy + sy > c->cy1) break;
                    for (int sx = 0; sx < scale; sx++) {
                        if (xx + sx > c->cx1) break;
                        c->buf[(yy + sy - c->y0) * c->w + (xx + sx - c->x0)] = v;
                    }
                }
            }
        }
    }
}

int gfx_text_w(const char *s, int scale)
{
    return (int)strlen(s) * FONT8X16_W * scale;
}

void gfx_text(gfx_canvas_t *c, int x, int y, const char *s, uint16_t fg, int scale)
{
    for (const char *p = s; *p; p++, x += FONT8X16_W * scale) {
        if (x > c->cx1) {
            break;
        }
        draw_glyph(c, x, y, *p, fg, 0, false, scale);
    }
}

void gfx_text_bg(gfx_canvas_t *c, int x, int y, const char *s, uint16_t fg,
                 uint16_t bg, int scale)
{
    int w = gfx_text_w(s, scale);
    if (x + w - 1 >= c->cx0 && x <= c->cx1) {
        gfx_fill(c, x, y, w, FONT8X16_H * scale, bg);
    }
    gfx_text(c, x, y, s, fg, scale);
}

void gfx_text_fit(gfx_canvas_t *c, int x, int y, int max_w, const char *s,
                  uint16_t fg, int scale)
{
    int cw = FONT8X16_W * scale;
    int max_chars = max_w / cw;
    if (max_chars <= 0) {
        return;
    }
    int n = (int)strlen(s);
    if (n <= max_chars) {
        gfx_text(c, x, y, s, fg, scale);
        return;
    }
    char tmp[48];
    int keep = max_chars - 1 < (int)sizeof(tmp) - 1 ? max_chars - 1 : (int)sizeof(tmp) - 1;
    memcpy(tmp, s, keep);
    tmp[keep] = '~';
    tmp[keep + 1] = '\0';
    gfx_text(c, x, y, tmp, fg, scale);
}

void gfx_text_wrap(gfx_canvas_t *c, int x, int y, int max_w, const char *s,
                   uint16_t fg, int scale, int max_lines)
{
    int cw = FONT8X16_W * scale;
    int max_chars = max_w / cw;
    if (max_chars <= 0 || max_lines <= 0) {
        return;
    }
    char line[64];
    if (max_chars > (int)sizeof(line) - 1) {
        max_chars = sizeof(line) - 1;
    }
    int line_no = 0;
    const char *p = s;

    while (*p && line_no < max_lines) {
        /* cabe tudo o que resta? */
        if ((int)strlen(p) <= max_chars) {
            gfx_text(c, x, y + line_no * FONT8X16_H * scale, p, fg, scale);
            break;
        }
        /* copia max_chars+1 caracteres e procura último espaço */
        int n = max_chars + 1;
        memcpy(line, p, n);
        line[n] = '\0';
        int cut = -1;
        for (int i = n; i >= 0; i--) {
            if (line[i] == ' ') {
                cut = i;
                break;
            }
        }
        if (cut <= 0) {
            cut = max_chars;      /* palavra maior que a linha: corta no meio */
        }
        line[cut] = '\0';
        gfx_text(c, x, y + line_no * FONT8X16_H * scale, line, fg, scale);
        line_no++;
        p += cut;
        while (*p == ' ') {
            p++;
        }
    }
}

/* ============================ widgets ============================ */

void gfx_bar(gfx_canvas_t *c, int x, int y, int w, int h, int pct,
             uint16_t fg, uint16_t bg)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    gfx_fill(c, x, y, w, h, bg);
    int fw = (w * pct) / 100;
    if (fw > 0) {
        gfx_fill(c, x, y, fw, h, fg);
    }
    gfx_rect(c, x, y, w, h, COL_BORDER);
}

void gfx_button(gfx_canvas_t *c, int x, int y, int w, int h, const char *label,
                bool active)
{
    gfx_fill(c, x, y, w, h, active ? COL_BTN_HI : COL_BTN);
    gfx_rect(c, x, y, w, h, active ? COL_ACCENT : COL_BORDER);
    if (label && label[0]) {
        int tw = gfx_text_w(label, 1);
        int tx = x + (w - tw) / 2;
        int ty = y + (h - FONT8X16_H) / 2;
        gfx_text(c, tx, ty, label, active ? COL_ACCENT : COL_TEXT, 1);
    }
}

void gfx_slider(gfx_canvas_t *c, int x, int y, int w, int pct, uint16_t col)
{
    const int h = 8;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    gfx_fill(c, x, y, w, h, COL_PANEL2);
    int fw = (w * pct) / 100;
    if (fw > 0) {
        gfx_fill(c, x, y, fw, h, col);
    }
    gfx_rect(c, x, y, w, h, COL_BORDER);
    int kx = x + fw - 4;
    if (kx < x) kx = x;
    if (kx + 8 > x + w) kx = x + w - 8;
    gfx_fill(c, kx, y - 4, 8, h + 8, COL_TEXT);
    gfx_rect(c, kx, y - 4, 8, h + 8, COL_BORDER);
}

/* ============================ ícones ============================ */

void gfx_icon(gfx_canvas_t *c, gfx_icon_t icon, int cx, int cy, int s,
              uint16_t col)
{
    int r = s / 2;                 /* meia largura */
    int x0 = cx - r, y0 = cy - r;
    switch (icon) {
    case ICON_PLAY:
        gfx_fill_tri(c, x0 + s / 5, y0, x0 + s / 5, y0 + s - 1,
                     x0 + s - 1, cy, col);
        break;
    case ICON_PAUSE:
        gfx_fill(c, x0 + s / 5, y0 + 1, s / 4, s - 2, col);
        gfx_fill(c, x0 + s - s / 5 - s / 4, y0 + 1, s / 4, s - 2, col);
        break;
    case ICON_NEXT:
        gfx_fill_tri(c, x0, y0, x0, y0 + s - 1, cx + r / 2, cy, col);
        gfx_fill(c, cx + r / 2, y0 + 1, s / 6, s - 2, col);
        break;
    case ICON_PREV:
        gfx_fill_tri(c, x0 + s - 1, y0, x0 + s - 1, y0 + s - 1, cx - r / 2, cy, col);
        gfx_fill(c, cx - r / 2 - s / 6, y0 + 1, s / 6, s - 2, col);
        break;
    case ICON_STOP:
        gfx_fill(c, x0 + 2, y0 + 2, s - 4, s - 4, col);
        break;
    case ICON_FOLDER: {
        int tab = s / 3;
        gfx_fill(c, x0, y0 + 2, tab, s / 4, col);
        gfx_fill(c, x0, y0 + s / 4 + 1, s, s - s / 4 - 2, col);
        break;
    }
    case ICON_NOTE: {
        int nx = x0 + s / 3;
        gfx_vline(c, nx + s / 3, y0, s - s / 4, col);
        gfx_vline(c, nx + s / 3 + 1, y0, s - s / 4, col);
        gfx_hline(c, nx + s / 6, y0, s / 2 + 1, col);
        gfx_fill(c, nx, y0 + s - s / 3, s / 3 + 1, s / 4, col);
        break;
    }
    case ICON_BT: {
        /* runa bluetooth */
        int t = x0 + s / 2;
        gfx_vline(c, t, y0, s, col);
        gfx_line(c, x0 + 1, y0 + s / 4, x0 + s - 2, y0 + s * 3 / 4 - 1, col);
        gfx_line(c, x0 + 1, y0 + s * 3 / 4 - 1, x0 + s - 2, y0 + s / 4, col);
        gfx_line(c, t, y0, x0 + s - 2, y0 + s / 4, col);
        gfx_line(c, t, y0 + s - 1, x0 + s - 2, y0 + s * 3 / 4 - 1, col);
        break;
    }
    case ICON_BT_OFF:
        gfx_icon(c, ICON_BT, cx, cy, s, COL_MUTED);
        gfx_line(c, x0, y0, x0 + s - 1, y0 + s - 1, COL_ERR);
        gfx_line(c, x0, y0 + 1, x0 + s - 1, y0 + s, COL_ERR);
        break;
    case ICON_BACK:
        gfx_line(c, x0 + 1, cy, x0 + s - 2, cy, col);
        gfx_line(c, x0 + 1, cy, cx, y0 + 2, col);
        gfx_line(c, x0 + 1, cy, cx, y0 + s - 3, col);
        break;
    case ICON_SPEAKER:
        gfx_fill(c, x0, cy - s / 5, s / 3, s * 2 / 5, col);
        gfx_fill_tri(c, x0 + s / 3, cy - s / 5, x0 + s / 3, cy + s * 2 / 5 - 1,
                     cx + s / 4, cy, col);
        break;
    case ICON_EQ:
        for (int i = 0; i < 3; i++) {
            int bx = x0 + 2 + i * (s / 3);
            gfx_vline(c, bx, y0 + 2, s - 4, COL_MUTED);
            int ky = y0 + 3 + (i * (s / 3)) % (s - 8);
            gfx_fill(c, bx - 2, ky, 5, 4, col);
        }
        break;
    case ICON_CHECK:
        gfx_line(c, x0 + 1, cy, cx - 1, y0 + s - 3, col);
        gfx_line(c, cx - 1, y0 + s - 3, x0 + s - 1, y0 + 1, col);
        break;
    case ICON_SEARCH: {
        int cr = s / 3;
        int ccx = cx - s / 10, ccy = cy - s / 10;
        for (int dy = -cr; dy <= cr; dy++) {
            for (int dx = -cr; dx <= cr; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 >= (cr - 1) * (cr - 1) && d2 <= cr * cr + 1) {
                    gfx_pixel(c, ccx + dx, ccy + dy, col);
                }
            }
        }
        gfx_line(c, ccx + cr * 7 / 10, ccy + cr * 7 / 10,
                 x0 + s - 1, y0 + s - 1, col);
        break;
    }
    case ICON_SD:
        gfx_fill(c, x0 + 1, y0, s - 2, s - 1, col);
        gfx_fill(c, x0 + s - s / 3, y0, s / 3 - 1, s / 4, COL_BG);
        break;
    }
}

void gfx_fmt_time(char *out, int out_len, uint32_t seconds)
{
    snprintf(out, out_len, "%lu:%02lu",
             (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
}
