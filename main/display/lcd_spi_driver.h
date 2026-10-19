/*
 * lcd_spi_driver.h — ILI9341 320x240 via esp_lcd (SPI2 + DMA), com
 * transmissão assíncrona de áreas (ping-pong gerenciado pelo ui_engine).
 * Spec §3: sem framebuffer completo, apenas buffers parciais DMA.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Inicializa barramento SPI2, panel_io, sequência ILI9341 e backlight. */
esp_err_t lcd_init(void);

/* Backlight 0..100 (LEDC PWM). */
void lcd_set_backlight(uint8_t pct);

/* Envia uma área (buffer RGB565 nativo, byte order já trocado pelo gfx).
 * Assíncrono: retorna assim que a transação é enfileirada; o buffer DEVE
 * permanecer válido até a conclusão (lcd_wait_one/lcd_wait_all).
 * Até 2 transações podem ficar em voo (ping-pong). */
esp_err_t lcd_flush_async(int x, int y, int w, int h, const uint16_t *buf);

/* Aguarda a conclusão de UMA transmissão pendente (se houver). */
void lcd_wait_one(void);

/* Aguarda todas as transmissões pendentes. */
void lcd_wait_all(void);
