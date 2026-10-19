/*
 * local_dac.h — Saída local pelo amplificador SC8002D do CYD (GPIO26 =
 * canal 1 do DAC de 8 bits do ESP32, modo contínuo via I2S + DMA).
 * O GPIO25 (canal 0) pertence ao touch e NÃO é tocado aqui.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t local_dac_init(void);

/* Escreve amostras mono 8 bits (44,1 kHz). Bloqueia até carregar no DMA
 * (timeout_ms). Quando o player está ocioso, chame local_dac_silence()
 * periodicamente para manter o nível médio e evitar estalos. */
esp_err_t local_dac_write(const uint8_t *mono8, size_t len, int timeout_ms);

/* Liga/desliga a saída (disable zera o DAC; enable retoma nível médio). */
void local_dac_enable(bool on);
