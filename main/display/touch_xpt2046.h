/*
 * touch_xpt2046.h — Leitura do touch resistivo XPT2046 via SPI software
 * (bit-bang), já que o ESP32 só tem 2 hosts SPI de hardware (LCD + SD).
 * Amostragem por polling a cada 20 ms (spec §2.2) com filtro de ruído.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t xpt2046_init(void);

/* Lê uma amostra; retorna true se houver toque. x/y em coordenadas de tela
 * (0..319, 0..239) já calibradas. */
bool xpt2046_read(uint16_t *x, uint16_t *y);

/* Como xpt2046_read, mas também devolve os valores brutos do ADC (rx, ry) —
 * usado pela tela de calibração. */
bool xpt2046_read_full(uint16_t *x, uint16_t *y, uint16_t *rx, uint16_t *ry);

/* Lê valores brutos do ADC (para a tela de calibração). */
bool xpt2046_read_raw(uint16_t *rx, uint16_t *ry);

/* Aplica calibração afim: xs = c0*rx + c1*ry + c2 ; ys = c3*rx + c4*ry + c5.
 * Com NULL usa o mapeamento padrão de app_config.h. */
void xpt2046_set_calibration(const float c[6]);
