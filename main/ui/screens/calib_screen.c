/*
 * calib_screen.c — Calibração do touch resistivo em 4 pontos.
 * Ajusta uma transformação afim (mínimos quadrados) dos valores brutos do
 * XPT2046 para coordenadas de tela e a grava na NVS.
 */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "screens_common.h"
#include "app_types.h"
#include "app_config.h"
#include "nvs_config.h"
#include "touch_xpt2046.h"
#include "esp_log.h"

static const char *TAG = "calib";

/* alvos (com margem para o dedo não sair da tela) */
static const int16_t TX[4] = { 30, LCD_H_RES - 31, 30, LCD_H_RES - 31 };
static const int16_t TY[4] = { 60, 60, LCD_V_RES - 45, LCD_V_RES - 45 };

static int s_step;
static float s_rx[4], s_ry[4];
static bool s_done_ok;

static void calib_enter(void)
{
    s_step = 0;
    s_done_ok = false;
    ESP_LOGI(TAG, "=== calibracao iniciada (xform=%u) ===",
             appcfg_get()->lcd_xform);
}

/* resolve sistema 3x3 (Cramer) */
static bool solve3(float m[3][3], float r[3], float out[3])
{
    float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
              - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
              + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (fabsf(det) < 1e-6f) {
        return false;
    }
    for (int col = 0; col < 3; col++) {
        float a[3][3];
        memcpy(a, m, sizeof(a));
        for (int row = 0; row < 3; row++) {
            a[row][col] = r[row];
        }
        float d = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
                - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
        out[col] = d / det;
    }
    return true;
}

/* mínimos quadrados: [rx ry 1] * [a b c]^T = alvo */
static bool fit_axis(float coef[3], const float rx[4], const float ry[4],
                     const int16_t t[4])
{
    float m[3][3] = {{0}}, r[3] = {0};
    for (int i = 0; i < 4; i++) {
        float row[3] = { rx[i], ry[i], 1.0f };
        for (int a = 0; a < 3; a++) {
            for (int b = 0; b < 3; b++) {
                m[a][b] += row[a] * row[b];
            }
            r[a] += row[a] * t[i];
        }
    }
    return solve3(m, r, coef);
}

static bool finish_calibration(void)
{
    float cx[3], cy[3];
    if (!fit_axis(cx, s_rx, s_ry, TX) || !fit_axis(cy, s_rx, s_ry, TY)) {
        return false;
    }
    /* sanidade: escala razoável (~4096 -> ~320/240) */
    ESP_LOGI(TAG, "afim X: %.6f %.6f %.6f", cx[0], cx[1], cx[2]);
    ESP_LOGI(TAG, "afim Y: %.6f %.6f %.6f", cy[0], cy[1], cy[2]);
    float sx = fabsf(cx[0]) + fabsf(cx[1]);
    float sy = fabsf(cy[0]) + fabsf(cy[1]);
    ESP_LOGI(TAG, "sanidade: |sx|=%.4f |sy|=%.4f (faixa ok 0.02..0.5)", sx, sy);
    if (sx < 0.02f || sx > 0.5f || sy < 0.02f || sy > 0.5f) {
        ESP_LOGE(TAG, "posicao final: REJEITADA (pontos inconsistentes)");
        return false;
    }
    app_config_t *cfg = appcfg_get();
    cfg->calib[0] = cx[0]; cfg->calib[1] = cx[1]; cfg->calib[2] = cx[2];
    cfg->calib[3] = cy[0]; cfg->calib[4] = cy[1]; cfg->calib[5] = cy[2];
    cfg->has_calib = true;
    appcfg_save();
    xpt2046_set_calibration(cfg->calib);
    ESP_LOGI(TAG, "posicao final: APLICADA e salva na NVS");
    return true;
}

static void calib_draw(gfx_canvas_t *c)
{
    gfx_fill(c, 0, 0, LCD_H_RES, LCD_V_RES, COL_BG);
    ui_draw_header(c, "Calibrar touch");

    if (s_step >= 4) {
        if (s_done_ok) {
            gfx_icon(c, ICON_CHECK, LCD_H_RES / 2, 110, 40, COL_OK);
            gfx_text(c, 88, 145, "Calibracao salva!", COL_OK, 1);
        } else {
            gfx_text(c, 56, 100, "Falha na calibracao.", COL_ERR, 1);
            gfx_text(c, 40, 122, "Pontos inconsistentes.", COL_ERR, 1);
        }
        gfx_button(c, 110, 170, 100, 30, "OK", false);
        ui_draw_toast(c);
        return;
    }

    gfx_text(c, 24, 32, "Toque no centro da mira", COL_TEXT, 1);
    char p[24];
    snprintf(p, sizeof(p), "Ponto %d de 4", s_step + 1);
    gfx_text(c, 120, 214, p, COL_MUTED, 1);

    /* miras concluídas + atual */
    for (int i = 0; i <= s_step; i++) {
        uint16_t col = (i < s_step) ? COL_OK : COL_ACCENT;
        int x = TX[i], y = TY[i];
        gfx_line(c, x - 10, y, x + 10, y, col);
        gfx_line(c, x, y - 10, x, y + 10, col);
        if (i < s_step) {
            gfx_rect(c, x - 5, y - 5, 10, 10, col);
        }
    }
    /* botão pular */
    gfx_button(c, 232, 30, 80, 24, "Pular", false);

    ui_draw_toast(c);
}

static void calib_touch(const touch_evt_t *ev)
{
    if (ev->type != TEV_TAP) {
        return;
    }
    if (s_step >= 4) {
        if (gfx_in_rect(ev->x, ev->y, 110, 170, 100, 30)) {
            ui_show(SCREEN_SETTINGS);
        }
        return;
    }
    if (gfx_in_rect(ev->x, ev->y, 232, 30, 80, 24)) {   /* pular */
        ui_show(SCREEN_SETTINGS);
        return;
    }
    /* valida proximidade do alvo (eviva toques acidentais) */
    int dx = ev->x - TX[s_step], dy = ev->y - TY[s_step];
    if (dx * dx + dy * dy > 60 * 60) {
        return;
    }
    s_rx[s_step] = (float)ev->rx;
    s_ry[s_step] = (float)ev->ry;
    ESP_LOGI(TAG, "tentativa ponto %d: raw=(%u,%u) scr=(%d,%d) alvo=(%d,%d)",
             s_step + 1, ev->rx, ev->ry, ev->x, ev->y, TX[s_step], TY[s_step]);
    s_step++;
    if (s_step == 4) {
        s_done_ok = finish_calibration();
    }
    ui_invalidate_all();
}

const ui_screen_t calib_screen = {
    .name = "calib",
    .on_enter = calib_enter,
    .draw = calib_draw,
    .on_touch = calib_touch,
};
