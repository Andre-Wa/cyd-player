/*
 * player_screen.c — Tela principal: título da faixa, progresso, transporte
 * (anterior/tocar-pausar/próxima), rota de áudio e navegação.
 */
#include <string.h>
#include <stdio.h>
#include "screens_common.h"
#include "app_types.h"
#include "app_config.h"
#include "bt_a2dp_source.h"
#include "nvs_config.h"

/* layout */
#define TR_TITLE_Y   32
#define TR_STATUS_Y  70
#define TR_TIME_Y    92
#define TR_BAR_Y     112
#define TR_BTN_Y     132
#define TR_BTN_H     56
#define NAV_Y        200
#define NAV_H        32

static void draw_status_line(gfx_canvas_t *c)
{
    char s[96];
    const char *route_txt;
    uint16_t col = COL_MUTED;
    switch (g_ui.route) {
    case ROUTE_BT:
        snprintf(s, sizeof(s), "> BT: %s", g_ui.bt_peer[0] ? g_ui.bt_peer : "-");
        col = COL_BT;
        break;
    case ROUTE_LOCAL:
        snprintf(s, sizeof(s), "> Alto-falante local");
        col = COL_OK;
        break;
    default:
        if (g_ui.pstate == PST_PAUSED) {
            snprintf(s, sizeof(s), "> Pausado");
            col = COL_ACCENT;
        } else {
            snprintf(s, sizeof(s), "> Sem saida (conecte o Bluetooth)");
        }
        break;
    }
    (void)route_txt;
    if (g_ui.pstate == PST_PAUSED && g_ui.route != ROUTE_NONE) {
        snprintf(s, sizeof(s), "> Pausado (%s)",
                 g_ui.route == ROUTE_BT ? "BT" : "local");
        col = COL_ACCENT;
    }
    gfx_text_fit(c, 12, TR_STATUS_Y, 296, s, col, 1);
}

static void player_draw(gfx_canvas_t *c)
{
    gfx_fill(c, 0, 0, LCD_H_RES, LCD_V_RES, COL_BG);
    ui_draw_header(c, "Player");

    /* título da faixa (2 linhas) */
    if (g_ui.title[0]) {
        gfx_text_wrap(c, 12, TR_TITLE_Y, 296, g_ui.title, COL_TEXT, 1, 2);
    } else {
        gfx_text(c, 12, TR_TITLE_Y, "Nenhuma faixa", COL_MUTED, 1);
        gfx_text(c, 12, TR_TITLE_Y + 18, "Toque em 'Lista' para escolher",
                 COL_MUTED, 1);
    }

    draw_status_line(c);

    /* tempos + barra de progresso */
    char t1[12], t2[12];
    gfx_fmt_time(t1, sizeof(t1), g_ui.elapsed_s);
    gfx_fmt_time(t2, sizeof(t2), g_ui.total_s);
    gfx_text(c, 12, TR_TIME_Y, t1, COL_MUTED, 1);
    int tw = gfx_text_w(t2, 1);
    gfx_text(c, LCD_H_RES - 12 - tw, TR_TIME_Y, t2, COL_MUTED, 1);
    int pct = g_ui.total_s ? (int)(g_ui.elapsed_s * 100 / g_ui.total_s) : 0;
    gfx_bar(c, 12, TR_BAR_Y, LCD_H_RES - 24, 10, pct, COL_ACCENT, COL_PANEL2);

    /* botões de transporte */
    bool playing = (g_ui.pstate == PST_PLAYING);
    /* anterior */
    gfx_button(c, 24, TR_BTN_Y, 64, TR_BTN_H, NULL, false);
    gfx_icon(c, ICON_PREV, 24 + 32, TR_BTN_Y + TR_BTN_H / 2, 22, COL_TEXT);
    /* play/pause (destaque) */
    gfx_button(c, 112, TR_BTN_Y, 96, TR_BTN_H, NULL, true);
    gfx_icon(c, playing ? ICON_PAUSE : ICON_PLAY, 160,
             TR_BTN_Y + TR_BTN_H / 2, 30, COL_ACCENT);
    /* próxima */
    gfx_button(c, 232, TR_BTN_Y, 64, TR_BTN_H, NULL, false);
    gfx_icon(c, ICON_NEXT, 232 + 32, TR_BTN_Y + TR_BTN_H / 2, 22, COL_TEXT);

    /* navegação inferior */
    gfx_button(c, 8, NAV_Y, 92, NAV_H, "Lista", false);
    gfx_button(c, 112, NAV_Y, 96, NAV_H, "Bluetooth", false);
    gfx_button(c, 220, NAV_Y, 92, NAV_H, "Ajustes", false);

    ui_draw_toast(c);
}

static void player_touch(const touch_evt_t *ev)
{
    if (ev->type != TEV_TAP) {
        return;
    }
    int x = ev->x, y = ev->y;
    app_cmd_t cmd = {0};

    if (gfx_in_rect(x, y, 24, TR_BTN_Y, 64, TR_BTN_H)) {
        cmd.id = CMD_PREV;
    } else if (gfx_in_rect(x, y, 112, TR_BTN_Y, 96, TR_BTN_H)) {
        cmd.id = CMD_TOGGLE;
    } else if (gfx_in_rect(x, y, 232, TR_BTN_Y, 64, TR_BTN_H)) {
        cmd.id = CMD_NEXT;
    } else if (gfx_in_rect(x, y, 8, NAV_Y, 92, NAV_H)) {
        ui_show(SCREEN_BROWSER);
        return;
    } else if (gfx_in_rect(x, y, 112, NAV_Y, 96, NAV_H)) {
        ui_show(SCREEN_BT);
        return;
    } else if (gfx_in_rect(x, y, 220, NAV_Y, 92, NAV_H)) {
        ui_show(SCREEN_SETTINGS);
        return;
    } else {
        return;
    }
    app_send_cmd(&cmd);
}

const ui_screen_t player_screen = {
    .name = "player",
    .draw = player_draw,
    .on_touch = player_touch,
};
