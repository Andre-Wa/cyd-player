/*
 * lcd_spi_driver.h — ILI9341 320x240 do CYD via SPI2.
 *
 * v1.2: escrita de inicialização/registros no modelo "clássico" comprovado
 * (TFT_eSPI/Adafruit): CS e DC controlados por GPIO, comando+parâmetros sob
 * UMA asserção de CS, transações polling. Em algumas unidades do CYD o
 * caminho esp_lcd panel_io não latchava comandos COM parâmetro (MADCTL),
 * deixando o painel em retrato. A verificação por readback (RDDMADCTL)
 * confirma no log se o registro gravou.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Inicializa barramento SPI2, painel (com verificação) e backlight. */
esp_err_t lcd_init(void);

/* Backlight 0..100 (LEDC PWM). */
void lcd_set_backlight(uint8_t pct);

/* Envia uma área (buffer RGB565 com bytes já trocados pelo gfx).
 * Síncrono nesta versão (polling); a assinatura "async" é mantida por
 * compatibilidade com o ui_engine (lcd_wait_* viram no-op). */
esp_err_t lcd_flush_async(int x, int y, int w, int h, const uint16_t *buf);

void lcd_wait_one(void);
void lcd_wait_all(void);

/* Modos de orientação (NVS lcd_xform):
 *   0/1 = direto, MADCTL paisagem / paisagem+180
 *   2..5 = transposição por software (painéis que ignoram o bit MV)
 * Aplica imediatamente (MADCTL quando aplicável). */
void lcd_set_xform(uint8_t xf);
