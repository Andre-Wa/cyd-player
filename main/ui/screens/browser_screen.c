/*
 * browser_screen.c — Navegador de arquivos do cartão SD: pastas + *.wav,
 * com rolagem por arrasto.
 */
#include <string.h>
#include <stdio.h>
#include "screens_common.h"
#include "app_types.h"
#include "app_config.h"
#include "playlist.h"
#include "nvs_config.h"
#include "sd_card.h"

#define LIST_Y    28
#define ROW_H     29
#define ROWS      6                    /* 28 + 6*29 = 202 */
#define LIST_BOTTOM (LIST_Y + ROWS * ROW_H)

static int s_off;                      /* primeira entrada visível */
static bool s_loaded;

static void browser_load_root(void)
{
    playlist_load_dir(appcfg_get()->root_dir);
    s_off = 0;
    s_loaded = true;
}

static void browser_enter(void)
{
    if (!s_loaded) {
        browser_load_root();
    }
}

static void clamp_offset(void)
{
    int n = playlist_count() + 1;      /* + entrada ".." */
    int max_off = n - ROWS;
    if (max_off < 0) {
        max_off = 0;
    }
    if (s_off > max_off) {
        s_off = max_off;
    }
    if (s_off < 0) {
        s_off = 0;
    }
}

/* índice lógico -> entrada: 0 = "..", 1.. = playlist */
static int logical_count(void)
{
    return playlist_count() + 1;
}

static void browser_draw(gfx_canvas_t *c)
{
    gfx_fill(c, 0, 0, LCD_H_RES, LCD_V_RES, COL_BG);
    ui_draw_header(c, "Arquivos");

    /* diretório atual */
    gfx_text_fit(c, 8, LIST_Y - 22, 304, playlist_dir()[0] ? playlist_dir() : "-",
                 COL_ACCENT, 1);

    if (!sd_card_is_mounted()) {
        gfx_text(c, 40, 100, "Cartao SD ausente", COL_ERR, 1);
        ui_draw_toast(c);
        return;
    }

    clamp_offset();
    int total = logical_count();

    for (int r = 0; r < ROWS; r++) {
        int idx = s_off + r;
        int y = LIST_Y + r * ROW_H;
        if (idx >= total) {
            break;
        }
        gfx_fill(c, 4, y, LCD_H_RES - 8, ROW_H - 3, COL_PANEL);
        if (idx == 0) {
            gfx_icon(c, ICON_BACK, 18, y + 12, 12, COL_MUTED);
            gfx_text(c, 32, y + 5, "..", COL_MUTED, 1);
            continue;
        }
        const pl_entry_t *e = playlist_get(idx - 1);
        if (!e) {
            continue;
        }
        if (e->is_dir) {
            gfx_icon(c, ICON_FOLDER, 18, y + 12, 14, COL_ACCENT);
            gfx_text_fit(c, 32, y + 5, 250, e->name, COL_TEXT, 1);
            gfx_text(c, LCD_H_RES - 24, y + 5, ">", COL_MUTED, 1);
        } else {
            gfx_icon(c, ICON_NOTE, 18, y + 12, 13, COL_BT);
            gfx_text_fit(c, 32, y + 5, 268, e->name, COL_TEXT, 1);
        }
    }

    /* indicador de rolagem */
    if (total > ROWS) {
        int sh = (LIST_BOTTOM - LIST_Y) * ROWS / total;
        int sy = LIST_Y + (LIST_BOTTOM - LIST_Y - sh) * s_off /
                 (total - ROWS > 0 ? total - ROWS : 1);
        gfx_fill(c, LCD_H_RES - 5, sy, 3, sh, COL_BORDER);
    }

    /* rodapé */
    ui_draw_back_button(c);
    gfx_button(c, 100, 206, 100, 28, "Raiz", false);
    char cnt[24];
    snprintf(cnt, sizeof(cnt), "%d itens", total - 1);
    gfx_text(c, 216, 212, cnt, COL_MUTED, 1);

    ui_draw_toast(c);
}

static void activate_row(int row)
{
    int idx = s_off + row;
    if (idx >= logical_count()) {
        return;
    }
    if (idx == 0) {                                   /* ".." */
        char up[PL_PATH_LEN];
        if (playlist_parent(up, sizeof(up))) {
            playlist_load_dir(up);
            s_off = 0;
            ui_invalidate_all();
        }
        return;
    }
    const pl_entry_t *e = playlist_get(idx - 1);
    if (!e) {
        return;
    }
    if (e->is_dir) {
        char path[PL_PATH_LEN];
        playlist_path(idx - 1, path, sizeof(path));
        playlist_load_dir(path);
        s_off = 0;
        ui_invalidate_all();
    } else {
        app_cmd_t cmd = {0};
        playlist_path(idx - 1, cmd.str, sizeof(cmd.str));
        cmd.id = CMD_PLAY_FILE;
        cmd.val = idx - 1;
        app_send_cmd(&cmd);
        ui_show(SCREEN_PLAYER);
    }
}

static void browser_touch(const touch_evt_t *ev)
{
    if (ev->type == TEV_TAP) {
        if (ui_back_button_hit(ev->x, ev->y)) {
            ui_show(SCREEN_PLAYER);
        } else if (gfx_in_rect(ev->x, ev->y, 100, 206, 100, 28)) {
            browser_load_root();
            ui_invalidate_all();
        } else if (ev->y >= LIST_Y && ev->y < LIST_BOTTOM) {
            activate_row((ev->y - LIST_Y) / ROW_H);
        }
    } else if (ev->type == TEV_DRAG) {
        int rows = -ev->dy / ROW_H;
        if (rows != 0) {
            s_off += rows;
            clamp_offset();
            ui_invalidate_all();
        }
    }
}

const ui_screen_t browser_screen = {
    .name = "browser",
    .on_enter = browser_enter,
    .draw = browser_draw,
    .on_touch = browser_touch,
};
