#include <string.h>
#include <stdio.h>
#include "screens_common.h"
#include "nvs_config.h"
#include "bt_a2dp_source.h"
#include "app_config.h"

void ui_draw_header(gfx_canvas_t *c, const char *title)
{
    gfx_fill(c, 0, 0, LCD_H_RES, HEADER_H, COL_PANEL);
    gfx_hline(c, 0, HEADER_H - 1, LCD_H_RES, COL_BORDER);
    gfx_text_fit(c, 8, 5, 150, title, COL_TEXT, 1);

    /* volume */
    char v[8];
    snprintf(v, sizeof(v), "%u%%", appcfg_get()->volume);
    gfx_icon(c, ICON_SPEAKER, 196, HEADER_H / 2, 12, COL_MUTED);
    gfx_text(c, 206, 5, v, COL_MUTED, 1);

    /* cartão SD */
    gfx_icon(c, ICON_SD, 258, HEADER_H / 2, 12,
             g_ui.sd_ok ? COL_MUTED : COL_ERR);

    /* bluetooth */
    uint16_t btc = COL_MUTED;
    gfx_icon_t bti = ICON_BT;
    switch (g_ui.bt_state) {
    case BT_CONNECTED:  btc = COL_BT; break;
    case BT_SCANNING:
    case BT_CONNECTING: btc = COL_ACCENT; break;
    case BT_READY:      btc = COL_MUTED; break;
    default:            bti = ICON_BT_OFF; btc = COL_ERR; break;
    }
    gfx_icon(c, bti, 296, HEADER_H / 2, 16, btc);
}

void ui_draw_toast(gfx_canvas_t *c)
{
    if (!g_ui.error_until_ms || ui_now_ms() >= g_ui.error_until_ms) {
        return;
    }
    int y = LCD_V_RES - 26;
    gfx_fill(c, 0, y, LCD_H_RES, 26, GFX_RGB(90, 20, 20));
    gfx_hline(c, 0, y, LCD_H_RES, COL_ERR);
    gfx_text_fit(c, 8, y + 5, LCD_H_RES - 16, g_ui.error, COL_TEXT, 1);
}

void ui_draw_back_button(gfx_canvas_t *c)
{
    gfx_button(c, 8, 206, 80, 28, NULL, false);
    gfx_icon(c, ICON_BACK, 28, 220, 14, COL_TEXT);
    gfx_text(c, 40, 212, "Voltar", COL_TEXT, 1);
}

bool ui_back_button_hit(int x, int y)
{
    return gfx_in_rect(x, y, 8, 206, 80, 28);
}
