/*
 * orient_screen.c — Seletor de orientação auto-suficiente (v1.3).
 *
 * Não depende de touch calibrado nem de display correto: QUALQUER toque
 * curto cicla o modo de orientação (0..5) e QUALQUER pressão longa (>=1,2 s)
 * confirma. A seta desenhada permite julgar visualmente qual modo deixa a
 * imagem direita nesta unidade/montagem de vidro.
 *
 *   modo 0/1: direto (MADCTL paisagem / paisagem+180)
 *   modo 2..5: transposição por software (painéis que ignoram o bit MV)
 *
 * Ao confirmar, a calibração de touch é invalidada (o mapa afim é refeito
 * para a nova orientação) e o fluxo segue para a tela de calibração.
 */
#include <string.h>
#include <stdio.h>
#include "screens_common.h"
#include "app_types.h"
#include "app_config.h"
#include "nvs_config.h"
#include "lcd_spi_driver.h"
#include "ui_engine.h"

static void orient_draw(gfx_canvas_t *c)
{
    gfx_fill(c, 0, 0, LCD_H_RES, LCD_V_RES, COL_BG);
    ui_draw_header(c, "Orientacao");

    /* seta grande apontando para CIMA no modo corrente */
    int cx = LCD_H_RES / 2, top = 44;
    gfx_fill_tri(c, cx - 44, top + 50, cx + 44, top + 50, cx, top, COL_ACCENT);
    gfx_fill(c, cx - 15, top + 50, 30, 62, COL_ACCENT);

    char m[24];
    snprintf(m, sizeof(m), "modo %d/%d", appcfg_get()->lcd_xform, LCD_MODE_MAX);
    gfx_text(c, cx + 60, top + 60, m, COL_MUTED, 1);

    gfx_text(c, 44, 176, "Toque/arraste: girar modo", COL_TEXT, 1);
    gfx_text(c, 28, 196, "Segure 2 s: confirmar", COL_ACCENT, 1);
    gfx_text(c, 28, 216, "(seta p/ cima, texto legivel)", COL_MUTED, 1);
}

static bool s_drag_consumed;

static void orient_touch(const touch_evt_t *ev)
{
    app_config_t *cfg = appcfg_get();

    if (ev->type == TEV_PRESS) {
        s_drag_consumed = false;
        return;
    }
    if (ev->type == TEV_TAP || ev->type == TEV_DRAG) {
        int step = 0;
        if (ev->type == TEV_TAP) {
            step = +1;
        } else if (!s_drag_consumed && ev->dx >= 8) {
            step = +1;
            s_drag_consumed = true;
        } else if (!s_drag_consumed && ev->dx <= -8) {
            step = -1;
            s_drag_consumed = true;
        }
        if (step) {
            int next = (cfg->lcd_xform + step + LCD_MODE_MAX + 1) % (LCD_MODE_MAX + 1);
            cfg->lcd_xform = (uint8_t)next;
            lcd_set_xform(cfg->lcd_xform);
            appcfg_save();
            ui_invalidate_all();
        }
    } else if (ev->type == TEV_LONG) {
        cfg->orient_ok = 1;
        cfg->has_calib = 0;      /* recalibrar o touch na nova orientação */
        appcfg_save();
        ui_show(SCREEN_CALIB);
    }
}

const ui_screen_t orient_screen = {
    .name = "orient",
    .draw = orient_draw,
    .on_touch = orient_touch,
};
