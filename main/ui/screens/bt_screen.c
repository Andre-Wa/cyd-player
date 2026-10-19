/*
 * bt_screen.c — Varredura, pareamento e conexão do alto-falante Bluetooth
 * (A2DP Sink). Dispositivos descobertos aparecem em lista; toque conecta.
 */
#include <string.h>
#include <stdio.h>
#include "screens_common.h"
#include "app_types.h"
#include "app_config.h"
#include "bt_a2dp_source.h"
#include "nvs_config.h"

#define ST_Y      30
#define BTN_Y     50
#define BTN_H     30
#define SAVE_Y    86
#define SAVE_H    28
#define LIST_Y    120
#define LIST_H    24
#define LIST_ROWS 4                    /* 120 + 4*24 = 216 */

static void bt_draw(gfx_canvas_t *c)
{
    gfx_fill(c, 0, 0, LCD_H_RES, LCD_V_RES, COL_BG);
    ui_draw_header(c, "Bluetooth");

    /* linha de estado */
    char s[80];
    uint16_t col;
    switch (g_ui.bt_state) {
    case BT_CONNECTED:
        snprintf(s, sizeof(s), "Conectado: %s", g_ui.bt_peer);
        col = COL_BT;
        break;
    case BT_SCANNING:
        snprintf(s, sizeof(s), "Procurando dispositivos...");
        col = COL_ACCENT;
        break;
    case BT_CONNECTING:
        snprintf(s, sizeof(s), "Conectando...");
        col = COL_ACCENT;
        break;
    case BT_READY:
        snprintf(s, sizeof(s), "Desconectado");
        col = COL_MUTED;
        break;
    default:
        snprintf(s, sizeof(s), "Bluetooth indisponivel");
        col = COL_ERR;
        break;
    }
    gfx_text_fit(c, 8, ST_Y, 304, s, col, 1);

    /* botões de ação */
    bool scanning = (g_ui.bt_state == BT_SCANNING);
    gfx_button(c, 8, BTN_Y, 148, BTN_H, scanning ? "Parar busca" : "Escanear",
               scanning);
    gfx_button(c, 164, BTN_Y, 148, BTN_H, "Desconectar",
               g_ui.bt_state == BT_CONNECTED);

    /* dispositivo salvo (reconexão rápida) */
    app_config_t *cfg = appcfg_get();
    if (cfg->has_bt && cfg->bt_name[0]) {
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "Reconectar: %s", cfg->bt_name);
        gfx_button(c, 8, SAVE_Y, 304, SAVE_H, NULL, false);
        gfx_icon(c, ICON_BT, 22, SAVE_Y + SAVE_H / 2, 13, COL_BT);
        gfx_text_fit(c, 36, SAVE_Y + 6, 270, lbl, COL_TEXT, 1);
    }

    /* lista de dispositivos descobertos */
    gfx_text(c, 8, LIST_Y - 18, "Dispositivos encontrados:", COL_MUTED, 1);
    if (g_ui.n_devices == 0) {
        gfx_text(c, 16, LIST_Y + 8,
                 scanning ? "aguarde..." : "toque em 'Escanear'", COL_MUTED, 1);
    }
    for (int i = 0; i < g_ui.n_devices && i < LIST_ROWS; i++) {
        int y = LIST_Y + i * LIST_H;
        gfx_fill(c, 4, y, LCD_H_RES - 8, LIST_H - 3, COL_PANEL);
        gfx_icon(c, g_ui.devices[i].rendering ? ICON_SPEAKER : ICON_BT,
                 16, y + 10, 12,
                 g_ui.devices[i].rendering ? COL_BT : COL_MUTED);
        gfx_text_fit(c, 30, y + 3, 220, g_ui.devices[i].name, COL_TEXT, 1);
        char r[8];
        snprintf(r, sizeof(r), "%ddB", g_ui.devices[i].rssi);
        gfx_text(c, 268, y + 3, r, COL_MUTED, 1);
    }

    /* rodapé */
    gfx_button(c, 8, 206, 80, 28, NULL, false);
    gfx_icon(c, ICON_BACK, 28, 220, 14, COL_TEXT);
    gfx_text(c, 40, 212, "Voltar", COL_TEXT, 1);
    gfx_text_fit(c, 100, 212, 212, APP_BT_DEVICE_NAME, COL_MUTED, 1);

    ui_draw_toast(c);
}

static void send_connect(const uint8_t mac[6])
{
    app_cmd_t cmd = {0};
    cmd.id = CMD_BT_CONNECT;
    memcpy(cmd.mac, mac, 6);
    app_send_cmd(&cmd);
}

static void bt_touch(const touch_evt_t *ev)
{
    if (ev->type != TEV_TAP) {
        return;
    }
    int x = ev->x, y = ev->y;

    if (gfx_in_rect(x, y, 8, BTN_Y, 148, BTN_H)) {
        app_cmd_t cmd = {0};
        cmd.id = CMD_BT_SCAN;
        cmd.val = (g_ui.bt_state == BT_SCANNING) ? -1 : 0;  /* -1 = parar */
        app_send_cmd(&cmd);
        return;
    }
    if (gfx_in_rect(x, y, 164, BTN_Y, 148, BTN_H)) {
        if (g_ui.bt_state == BT_CONNECTED) {
            app_cmd_t cmd = { .id = CMD_BT_DISCONNECT };
            app_send_cmd(&cmd);
        }
        return;
    }
    app_config_t *cfg = appcfg_get();
    if (cfg->has_bt && gfx_in_rect(x, y, 8, SAVE_Y, 304, SAVE_H)) {
        send_connect(cfg->bt_mac);
        return;
    }
    if (y >= LIST_Y && y < LIST_Y + LIST_ROWS * LIST_H) {
        int i = (y - LIST_Y) / LIST_H;
        if (i < g_ui.n_devices) {
            send_connect(g_ui.devices[i].mac);
        }
        return;
    }
    if (gfx_in_rect(x, y, 8, 206, 80, 28)) {
        ui_show(SCREEN_PLAYER);
    }
}

const ui_screen_t bt_screen = {
    .name = "bt",
    .draw = bt_draw,
    .on_touch = bt_touch,
};
