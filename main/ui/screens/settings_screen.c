/*
 * settings_screen.c — Volume, brilho, alto-falante local, auto-avanço,
 * calibração do touch e informações do sistema.
 */
#include <string.h>
#include <stdio.h>
#include "screens_common.h"
#include "app_types.h"
#include "app_config.h"
#include "nvs_config.h"
#include "lcd_spi_driver.h"
#include "sd_card.h"
#include "esp_heap_caps.h"

#define SL_X      110
#define SL_W      190
#define ROW_VOL   36
#define ROW_BR    66
#define ROW_LOC   98
#define ROW_AUTO  128
#define ROW_CAL   158
#define INFO_Y    190

static void draw_slider_row(gfx_canvas_t *c, int y, const char *label,
                            int pct, uint16_t col)
{
    char v[8];
    snprintf(v, sizeof(v), "%d%%", pct);
    gfx_text(c, 8, y - 2, label, COL_TEXT, 1);
    gfx_slider(c, SL_X, y, SL_W, pct, col);
    gfx_text(c, SL_X - 42, y - 4, v, COL_MUTED, 1);
}

static void settings_draw(gfx_canvas_t *c)
{
    gfx_fill(c, 0, 0, LCD_H_RES, LCD_V_RES, COL_BG);
    ui_draw_header(c, "Ajustes");

    app_config_t *cfg = appcfg_get();

    draw_slider_row(c, ROW_VOL, "Volume", cfg->volume, COL_ACCENT);
    draw_slider_row(c, ROW_BR, "Brilho", cfg->brightness, COL_BT);

    /* toggles */
    char s[48];
    snprintf(s, sizeof(s), "Alto-falante: %s", cfg->local_out ? "LIGADO" : "DESL");
    gfx_button(c, 8, ROW_LOC, 196, 26, s, cfg->local_out);

    snprintf(s, sizeof(s), "Auto-avancar: %s", cfg->auto_next ? "SIM" : "NAO");
    gfx_button(c, 8, ROW_AUTO, 196, 26, s, cfg->auto_next);

    snprintf(s, sizeof(s), "Tela: modo %d", cfg->lcd_xform);
    gfx_button(c, 212, 158, 100, 26, s, cfg->lcd_xform >= 2);

    gfx_button(c, 212, ROW_LOC, 100, 56, NULL, false);
    gfx_icon(c, ICON_CHECK, 232, ROW_LOC + 28, 13, COL_ACCENT);
    gfx_text(c, 246, ROW_LOC + 20, "Calibrar", COL_TEXT, 1);

    /* informações */
    char info[96];
    snprintf(info, sizeof(info), "Heap livre: %u KB",
             (unsigned)(esp_get_free_heap_size() / 1024));
    gfx_text(c, 8, INFO_Y - 14, info, COL_MUTED, 1);
    uint64_t tot = 0, fr = 0;
    sd_card_get_usage(&tot, &fr);
    if (tot) {
        snprintf(info, sizeof(info), "SD: %llu MB livres de %llu MB",
                 (unsigned long long)(fr >> 20), (unsigned long long)(tot >> 20));
    } else {
        snprintf(info, sizeof(info), "SD: ausente");
    }
    gfx_text_fit(c, 8, INFO_Y + 2, 304, info, COL_MUTED, 1);

    ui_draw_back_button(c);
    gfx_text(c, 100, 212, "v" APP_VERSION, COL_MUTED, 1);
    ui_draw_toast(c);
}

static int slider_pct_from_x(int x)
{
    int p = (x - SL_X) * 100 / SL_W;
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return p;
}

static void settings_touch(const touch_evt_t *ev)
{
    int x = ev->x, y = ev->y;

    if (ev->type == TEV_TAP || ev->type == TEV_DRAG) {
        /* sliders (faixa de captura generosa) */
        if (x >= SL_X - 8 && x <= SL_X + SL_W + 8) {
            app_cmd_t cmd = {0};
            if (y >= ROW_VOL - 12 && y <= ROW_VOL + 20) {
                cmd.id = CMD_SET_VOLUME;
                cmd.val = slider_pct_from_x(x);
                app_send_cmd(&cmd);
                return;
            }
            if (y >= ROW_BR - 12 && y <= ROW_BR + 20) {
                cmd.id = CMD_SET_BRIGHTNESS;
                cmd.val = slider_pct_from_x(x);
                app_send_cmd(&cmd);
                return;
            }
        }
    }

    if (ev->type != TEV_TAP) {
        return;
    }
    app_cmd_t cmd = {0};
    app_config_t *cfg = appcfg_get();

    if (gfx_in_rect(x, y, 8, ROW_LOC, 196, 26)) {
        cmd.id = CMD_SET_LOCAL_OUT;
        cmd.val = !cfg->local_out;
        app_send_cmd(&cmd);
    } else if (gfx_in_rect(x, y, 8, ROW_AUTO, 196, 26)) {
        cfg->auto_next = !cfg->auto_next;
        appcfg_save();
        ui_invalidate_all();
    } else if (gfx_in_rect(x, y, 212, 158, 100, 26)) {
        cfg->lcd_xform = (cfg->lcd_xform + 1) % (LCD_MODE_MAX + 1);
        lcd_set_xform(cfg->lcd_xform);
        cfg->has_calib = 0;      /* touch precisa recalibrar p/ nova orientação */
        appcfg_save();
        ui_show(SCREEN_CALIB);
    } else if (gfx_in_rect(x, y, 212, ROW_LOC, 100, 56)) {
        ui_show(SCREEN_CALIB);
    } else if (ui_back_button_hit(x, y)) {
        ui_show(SCREEN_PLAYER);
    }
}

const ui_screen_t settings_screen = {
    .name = "settings",
    .draw = settings_draw,
    .on_touch = settings_touch,
};
